#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>

// --- Cấu hình WiFi ---
const char* ssid = "Wokwi-GUEST";
const char* password = "";

// --- Cấu hình MQTT ---
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;

WiFiClient espClient;
PubSubClient client(espClient);

// --- Cấu hình DHT22 ---
#define DHTPIN 4          // Chân kết nối DHT22 (Kiểm tra kỹ trên Wokwi)
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// --- Cấu hình Chân Cơ cấu chấp hành ---
const int MIST_PIN = 18;   // Máy phun sương
const int FAN_PIN = 19;    // Quạt (điều khiển thủ công từ Web/App)

bool manualMode = false;

// --- Cấu trúc dữ liệu cho FreeRTOS Queue ---
struct SensorData {
  float temp;
  float hum;
};

// Khai báo FreeRTOS Queues
QueueHandle_t dataQueue;
QueueHandle_t dataAlertQueue;

// Biến quản lý thời gian non-blocking cho MQTT reconnect
unsigned long lastReconnectAttempt = 0;

// --- Hàm nhận lệnh điều khiển từ MQTT (Web/App) ---
void callback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  message.trim();
  message.toUpperCase();

  if (String(topic) == "garden/mist/set") {
    manualMode = true;
    if (message == "ON") {
      digitalWrite(MIST_PIN, HIGH);
    } else if (message == "OFF") {
      digitalWrite(MIST_PIN, LOW);
    }
  }
  else if (String(topic) == "garden/fan/set") {
    manualMode = true;
    if (message == "ON") {
      digitalWrite(FAN_PIN, HIGH);
    } else if (message == "OFF") {
      digitalWrite(FAN_PIN, LOW);
    }
  }
  else if (String(topic) == "garden/mode/set") {
    if (message == "AUTO") {
      manualMode = false;
    } else if (message == "MANUAL") {
      manualMode = true;
    }
  }
}

// --- Kết nối WiFi ---
void setup_wifi() {
  Serial.print("Dang ket noi WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");
}

// --- Hàm kết nối lại MQTT ---
boolean reconnect() {
  if (client.connect("ESP32_Garden_Client_231432239")) {
    Serial.println("MQTT Connected!");
    client.subscribe("garden/mist/set");
    client.subscribe("garden/fan/set");
    client.subscribe("garden/mode/set");
  } else {
    Serial.print("MQTT failed, rc=");
    Serial.println(client.state());
  }
  return client.connected();
}

// ==========================================
// TASK 1: TRUYỀN DATA (Chạy trên Core 0)
// ==========================================
void taskTransmitData(void * pvParameters) {
  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  while (true) {
    if (!client.connected()) {
      unsigned long now = millis();
      if (now - lastReconnectAttempt >= 5000) {
        lastReconnectAttempt = now;
        if (reconnect()) {
          lastReconnectAttempt = 0;
        }
      }
    } else {
      client.loop();
    }

    SensorData rxData;
    if (xQueueReceive(dataQueue, &rxData, pdMS_TO_TICKS(1000)) == pdPASS) {
      if (client.connected()) {
        char msg[50];
        snprintf(msg, sizeof(msg), "{\"temp\":%.1f,\"hum\":%.1f}", rxData.temp, rxData.hum);
        if (client.publish("ptit/nongnghiep/sensor", msg)) {
          Serial.println("-> Da gui du lieu len MQTT Broker thanh cong!");
        } else {
          Serial.println("-> Gui MQTT that bai!");
        }
      }
    }
  }
}

// ==========================================
// TASK 2: ĐỌC DATA (Chạy trên Core 1)
// ==========================================
void taskReadData(void * pvParameters) {
  dht.begin();
  vTaskDelay(pdMS_TO_TICKS(2000));

  while (true) {
    float h = dht.readHumidity();
    float t = dht.readTemperature();

    if (!isnan(h) && !isnan(t)) {
      Serial.println("----------------------------------------");
      Serial.printf("Nhiet do: %.1f C | Do am: %.1f %%\n", t, h);

      SensorData data = {t, h};
      xQueueSend(dataQueue, &data, pdMS_TO_TICKS(100));

      float alertHum = h;
      xQueueSend(dataAlertQueue, &alertHum, pdMS_TO_TICKS(100));
    } else {
      Serial.println("Loi doc cam bien DHT22! Kiem tra lai chan ket noi.");
    }

    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// ==========================================
// TASK 3: CẢNH BÁO & ĐIỀU KHIỂN (Chạy trên Core 1)
// ==========================================
void taskAlertControl(void * pvParameters) {
  float humVal;
  while (true) {
    if (xQueueReceive(dataAlertQueue, &humVal, portMAX_DELAY) == pdPASS) {
      if (!manualMode) {
        if (humVal < 30.0) {
          Serial.println("Do am < 30%: Bat may phun suong 100% cong suat trong 30s");
          digitalWrite(MIST_PIN, HIGH);
          vTaskDelay(pdMS_TO_TICKS(30000));
          digitalWrite(MIST_PIN, LOW);
        }
        else if (humVal >= 30.0 && humVal < 35.0) {
          Serial.println("Do am 30-35%: Bat may phun suong 50% cong suat trong 20s");
          digitalWrite(MIST_PIN, HIGH);
          vTaskDelay(pdMS_TO_TICKS(20000));
          digitalWrite(MIST_PIN, LOW);
        }
        else if (humVal >= 35.0 && humVal <= 40.0) {
          Serial.println("Do am 35-40%: Bat may phun suong 25% cong suat trong 10s");
          digitalWrite(MIST_PIN, HIGH);
          vTaskDelay(pdMS_TO_TICKS(10000));
          digitalWrite(MIST_PIN, LOW);
        }
        else {
          digitalWrite(MIST_PIN, LOW);
        }
      }
    }
  }
}

// --- Khởi tạo hệ thống RTOS ---
void setup() {
  Serial.begin(115200);

  pinMode(MIST_PIN, OUTPUT);
  pinMode(FAN_PIN, OUTPUT);
  digitalWrite(MIST_PIN, LOW);
  digitalWrite(FAN_PIN, LOW);

  dataQueue = xQueueCreate(10, sizeof(SensorData));
  dataAlertQueue = xQueueCreate(10, sizeof(float));

  xTaskCreatePinnedToCore(
    taskTransmitData, "Task Transmit", 4096, NULL, 1, NULL, 0
  );

  xTaskCreatePinnedToCore(
    taskReadData, "Task Read Data", 4096, NULL, 2, NULL, 1
  );

  xTaskCreatePinnedToCore(
    taskAlertControl, "Task Alert", 4096, NULL, 2, NULL, 1
  );
}

void loop() {
  
}
