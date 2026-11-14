/*
 * Giám sát môi trường vườn cây công nghiệp (cà phê, hồ tiêu)
 * ESP32 + DHT22 + MQTT + FreeRTOS
 *
 *  Core 0: taskComm  - kết nối WiFi/MQTT, lấy dữ liệu từ dataQueue, publish; nhận lệnh bơm/quạt
 *  Core 1: taskRead  - đọc DHT22, in Serial, đẩy dữ liệu vào dataQueue + alertQueue
 *  Core 1: taskAlert - đọc alertQueue, điều khiển máy phun sương theo ngưỡng độ ẩm
 */
#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>

// ---------- Phần cứng ----------
#define DHT_PIN    4
#define DHT_TYPE   DHT22
#define PUMP_PIN   26       // Máy bơm   (relay / LED xanh lá)
#define FAN_PIN    27       // Quạt      (relay / LED xanh dương)
#define MIST_PIN   25       // Máy phun sương (PWM / LED cam, độ sáng = công suất)

#define MIST_PWM_FREQ 1000
#define MIST_PWM_RES  8   // 8 bit: 0..255
#define MIST_PWM_CH   0   // chỉ dùng cho core Arduino 2.x

// ---------- WiFi / MQTT ----------
const char* WIFI_SSID = "Wokwi-GUEST";   // đổi thành WiFi thật khi chạy phần cứng
const char* WIFI_PASS = "";
const char* MQTT_HOST = "broker.hivemq.com";
const uint16_t MQTT_PORT = 1883;

// Nên đổi BASE thành tên riêng của bạn (broker công cộng, ai cũng thấy topic)
#define BASE         "vuoncay/esp32-demo"
#define TOPIC_DATA   BASE "/data"
#define TOPIC_STATUS BASE "/status"
#define TOPIC_PUMP   BASE "/cmd/pump"
#define TOPIC_FAN    BASE "/cmd/fan"

// ---------- Thuật toán độ ẩm ----------
#define HUM_MIN         40.0f   // ngưỡng dưới của vùng ổn định (40–60 %)
#define MIST_REST_MS    5000    // nghỉ giữa 2 chu kỳ phun để cảm biến kịp phản hồi

// ---------- Kiểu dữ liệu & hàng đợi ----------
typedef struct { float temperature; float humidity; } SensorData;   // "data Queue"
typedef struct { float humidity; }                   AlertData;    // "data Alert"
typedef struct { uint8_t percent; uint32_t durationMs; } MistLevel; // mức phun sương

QueueHandle_t dataQueue;
QueueHandle_t alertQueue;

// Trạng thái thiết bị (ghi bởi nhiều task nên để volatile)
volatile bool    pumpOn = false;
volatile bool    fanOn  = false;
volatile uint8_t mistPercent = 0;
volatile bool    forcePublish = false;

DHT dht(DHT_PIN, DHT_TYPE);
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

// ---------- Tiện ích ----------
void mistWrite(uint8_t percent) {
  uint32_t duty = (255UL * percent) / 100;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(MIST_PIN, duty);
#else
  ledcWrite(MIST_PWM_CH, duty);
#endif
  mistPercent = percent;
  forcePublish = true;
}

// Gửi vào queue; nếu đầy thì bỏ phần tử cũ nhất để luôn giữ dữ liệu mới
void queuePushLatest(QueueHandle_t q, const void *item) {
  if (xQueueSend(q, item, 0) != pdTRUE) {
    uint8_t dump[16];                // đủ lớn cho SensorData (8 byte) và AlertData (4 byte)
    xQueueReceive(q, dump, 0);
    xQueueSend(q, item, 0);
  }
}

// ---------- MQTT ----------
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  msg.trim();
  msg.toUpperCase();
  bool on = (msg == "ON" || msg == "1");

  if (strcmp(topic, TOPIC_PUMP) == 0) {
    pumpOn = on;
    digitalWrite(PUMP_PIN, on);
    Serial.printf("[MQTT] Máy bơm: %s\n", on ? "BẬT" : "TẮT");
  } else if (strcmp(topic, TOPIC_FAN) == 0) {
    fanOn = on;
    digitalWrite(FAN_PIN, on);
    Serial.printf("[MQTT] Quạt: %s\n", on ? "BẬT" : "TẮT");
  }
  forcePublish = true;   // báo trạng thái mới lên web ngay
}

bool mqttReconnect() {
  String clientId = "esp32-garden-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.print("[MQTT] Đang kết nối broker... ");
  if (mqtt.connect(clientId.c_str(), NULL, NULL, TOPIC_STATUS, 0, true, "offline")) {
    Serial.println("OK");
    mqtt.publish(TOPIC_STATUS, "online", true);
    mqtt.subscribe(TOPIC_PUMP);
    mqtt.subscribe(TOPIC_FAN);
    return true;
  }
  Serial.printf("lỗi rc=%d\n", mqtt.state());
  return false;
}

void publishData(const SensorData& d) {
  char buf[128];
  snprintf(buf, sizeof(buf),
           "{\"t\":%.1f,\"h\":%.1f,\"pump\":%d,\"fan\":%d,\"mist\":%d}",
           d.temperature, d.humidity, pumpOn ? 1 : 0, fanOn ? 1 : 0, (int)mistPercent);
  mqtt.publish(TOPIC_DATA, buf);
}

// ---------- Task truyền data (Core 0) ----------
void taskComm(void *pv) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);   // kênh 6 giúp Wokwi-GUEST kết nối nhanh hơn
  Serial.print("[WiFi] Đang kết nối");
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(pdMS_TO_TICKS(500));
    Serial.print(".");
  }
  Serial.printf("\n[WiFi] Đã kết nối, IP: %s\n", WiFi.localIP().toString().c_str());

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  SensorData d;
  SensorData last = {NAN, NAN};

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {      // mất WiFi: thử lại
      WiFi.reconnect();
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    if (!mqtt.connected()) {
      if (!mqttReconnect()) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
    }
    mqtt.loop();

    if (xQueueReceive(dataQueue, &d, pdMS_TO_TICKS(50)) == pdTRUE) {
      last = d;
      forcePublish = false;
      publishData(d);
    } else if (forcePublish && !isnan(last.temperature)) {
      forcePublish = false;
      publishData(last);                     // chỉ cập nhật trạng thái thiết bị
    }
  }
}

// ---------- Task đọc data (Core 1) ----------
void taskRead(void *pv) {
  dht.begin();
  TickType_t lastWake = xTaskGetTickCount();

  for (;;) {
    float h = dht.readHumidity();
    float t = dht.readTemperature();

    if (isnan(h) || isnan(t)) {
      Serial.println("[DHT22] Lỗi đọc cảm biến!");
    } else {
      Serial.printf("[DHT22] Nhiệt độ: %.1f °C | Độ ẩm: %.1f %% | Bơm: %s | Quạt: %s | Phun sương: %d%%\n",
                    t, h, pumpOn ? "BẬT" : "TẮT", fanOn ? "BẬT" : "TẮT", (int)mistPercent);

      SensorData d = {t, h};
      AlertData  a = {h};
      queuePushLatest(dataQueue, &d);    // dữ liệu đầy đủ -> task truyền
      queuePushLatest(alertQueue, &a);   // độ ẩm cảnh báo -> task cảnh báo
    }
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(2000));   // DHT22 tối thiểu 2 s/lần
  }
}

// ---------- Task cảnh báo (Core 1) ----------
// Trả về true nếu cần phun, kèm mức công suất và thời gian
bool getMistLevel(float h, MistLevel& lv) {
  if (h >= HUM_MIN) return false;                // >= 40 %: không phun
  if (h >= 35.0f)      lv = {25, 10000};          // 35–40 %: 25 % trong 10 s
  else if (h >= 30.0f) lv = {50, 20000};          // 30–35 %: 50 % trong 20 s
  else                 lv = {100, 30000};         // < 30 %:  100 % trong 30 s
  return true;
}

void taskAlert(void *pv) {
  AlertData a;
  bool running = false;
  uint32_t endMs = 0, restUntil = 0;

  for (;;) {
    if (xQueueReceive(alertQueue, &a, pdMS_TO_TICKS(100)) == pdTRUE) {
      MistLevel lv;
      bool need = getMistLevel(a.humidity, lv);
      uint32_t now = millis();

      if (!need) {
        if (running) {                            // độ ẩm đã về vùng an toàn -> dừng sớm
          mistWrite(0);
          running = false;
          Serial.println("[Phun sương] Độ ẩm đã ≥ 40% → TẮT");
        }
      } else if (!running && (int32_t)(now - restUntil) >= 0) {
        mistWrite(lv.percent);                        // bắt đầu chu kỳ phun mới
        endMs = now + lv.durationMs;
        running = true;
        Serial.printf("[Phun sương] Độ ẩm %.1f%% → BẬT %d%% trong %lu s\n",
                      a.humidity, lv.percent, (unsigned long)(lv.durationMs / 1000));
      } else if (running && lv.percent > mistPercent) {
        mistWrite(lv.percent);                        // độ ẩm giảm sâu hơn -> nâng mức
        endMs = now + lv.durationMs;
        Serial.printf("[Phun sương] Độ ẩm %.1f%% → nâng lên %d%% trong %lu s\n",
                      a.humidity, lv.percent, (unsigned long)(lv.durationMs / 1000));
      }
    }

    if (running && (int32_t)(millis() - endMs) >= 0) {   // hết thời gian phun
      mistWrite(0);
      running = false;
      restUntil = millis() + MIST_REST_MS;
      Serial.println("[Phun sương] Hết thời gian → TẮT");
    }
  }
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Giám sát vườn cây công nghiệp: cà phê, hồ tiêu ===");

  pinMode(PUMP_PIN, OUTPUT);
  pinMode(FAN_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, LOW);
  digitalWrite(FAN_PIN, LOW);

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(MIST_PIN, MIST_PWM_FREQ, MIST_PWM_RES);
#else
  ledcSetup(MIST_PWM_CH, MIST_PWM_FREQ, MIST_PWM_RES);
  ledcAttachPin(MIST_PIN, MIST_PWM_CH);
#endif
  mistWrite(0);

  dataQueue  = xQueueCreate(10, sizeof(SensorData));
  alertQueue = xQueueCreate(5,  sizeof(AlertData));

  //                    hàm        tên    stack  tham số prio  handle core
  xTaskCreatePinnedToCore(taskComm,  "Comm",  8192,  NULL,   1,   NULL,  0);   // Core 0
  xTaskCreatePinnedToCore(taskRead,  "Read",  4096,  NULL,   2,   NULL,  1);   // Core 1
  xTaskCreatePinnedToCore(taskAlert, "Alert", 4096,  NULL,   2,   NULL,  1);   // Core 1
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));   // mọi việc đã nằm trong các task
}