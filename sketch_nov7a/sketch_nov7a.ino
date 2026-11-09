#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>

// --- Cấu hình WiFi ---
const char* ssid = "Wokwi-GUEST";
const char* password = "";

// --- Cấu hình MQTT Broker ---
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;

WiFiClient espClient;
PubSubClient client(espClient);

// --- Cấu hình DHT22 ---
#define DHTPIN 4          // Chân kết nối DHT22 (phù hợp với Wokwi)
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// --- Cấu hình Chân Cơ cấu chấp hành ---
const int MIST_PIN = 18;   // Máy phun sương (chân 18)
const int FAN_PIN = 19;    // Quạt (chân 19)

// Biến trạng thái chế độ (false: AUTO, true: MANUAL)
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
    manualMode = true; // Chuyển sang Manual khi có tác động thủ công
    if (message == "ON") {
      digitalWrite(MIST_PIN, HIGH);
      Serial.println("[MQTT] MIST ON (Manual Mode) -> Trang thai: BAT MIST");
    } else if (message == "OFF") {
      digitalWrite(MIST_PIN, LOW);
      Serial.println("[MQTT] MIST OFF (Manual Mode) -> Trang thai: TAT MIST");
    }
  }
  else if (String(topic) == "garden/fan/set") {
    manualMode = true; // Chuyển sang Manual khi có tác động thủ công
    if (message == "ON") {
      digitalWrite(FAN_PIN, HIGH);
      Serial.println("[MQTT] FAN ON (Manual Mode)");
    } else if (message == "OFF") {
      digitalWrite(FAN_PIN, LOW);
      Serial.println("[MQTT] FAN OFF (Manual Mode)");
    }
  }
  else if (String(topic) == "garden/mode/set") {
    if (message == "AUTO") {
      manualMode = false;
      Serial.println("[MQTT] Mode switched to AUTO");
    } else if (message == "MANUAL") {
      manualMode = true;
      Serial.println("[MQTT] Mode switched to MANUAL");
    }
  }
}

// --- Kết nối WiFi ---
void setup_wifi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected! IP address: ");
  Serial.println(WiFi.localIP());
}

// --- Hàm kết nối lại MQTT ---
boolean reconnect() {
  if (client.connect("ESP32_Garden_Client_231432239")) {
    Serial.println("MQTT Connected!");
    client.subscribe("garden/mist/set");
    client.subscribe("garden/fan/set");
    client.subscribe("garden/mode/set");
  } else {
    Serial.print("MQTT connection failed, rc=");
    Serial.println(client.state());
  }
  return client.connected();
}

// ==========================================
// TASK 1: TRUYỀN DATA (Chạy trên Core 0)
// ==========================================
void taskTransmitData(void* pvParameters) {
  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  while (true) {
    if (!client.connected()) {
      unsigned long now = millis();
      if (now - lastReconnectAttempt >= 10000) {
        lastReconnectAttempt = now;
        if (reconnect()) {
          lastReconnectAttempt = 0;
        }
      }
    } else {
      client.loop();
    }

    SensorData rxData;
    // Nhận dữ liệu từ hàng đợi "dataQueue" để gửi lên MQTT Broker
    if (xQueueReceive(dataQueue, &rxData, pdMS_TO_TICKS(500)) == pdPASS) {
      if (client.connected()) {
        char msg[50];
        snprintf(msg, sizeof(msg), "{\"temp\":%.1f,\"hum\":%.1f}", rxData.temp, rxData.hum);
        client.publish("ptit/nongnghiep/sensor", msg);
        Serial.printf("[Core 0 - Transmit] Published MQTT -> Temp: %.1f°C, Hum: %.1f%%\n", rxData.temp, rxData.hum);
      }
    }
    
    vTaskDelay(pdMS_TO_TICKS(200)); 
  }
}

// ==========================================
// TASK 2: ĐỌC DATA (Chạy trên Core 1)
// ==========================================
void taskReadData(void* pvParameters) {
  dht.begin();
  
  // Biến lưu trữ giá trị cũ để so sánh thay đổi
  float lastLoggedTemp = -999.0;
  float lastLoggedHum = -999.0;

  while (true) {
    float h = dht.readHumidity();
    float t = dht.readTemperature();

    if (!isnan(h) && !isnan(t)) {
      // Kiểm tra xem giá trị có thay đổi so với lần trước không (làm tròn đến 0.1)
      bool tempChanged = (abs(t - lastLoggedTemp) >= 0.1);
      bool humChanged = (abs(h - lastLoggedHum) >= 0.1);

      // Chỉ in Serial và đẩy vào Queue khi có sự thay đổi giá trị
      if (tempChanged || humChanged) {
        Serial.println("----------------------------------------");
        Serial.printf("[Core 1 - Read (Gia tri thay doi)] Nhiet do: %.1f C | Do am: %.1f %%\n", t, h);

        // Cập nhật lại giá trị cũ
        lastLoggedTemp = t;
        lastLoggedHum = h;

        // 1. Đóng gói dữ liệu đầy đủ gửi vào "dataQueue"
        SensorData data = {t, h};
        xQueueSend(dataQueue, &data, pdMS_TO_TICKS(100));

        // 2. Gửi dữ liệu độ ẩm vào hàng đợi cảnh báo "dataAlertQueue"
        float alertHum = h;
        xQueueSend(dataAlertQueue, &alertHum, pdMS_TO_TICKS(100));
      } else {
        // Nếu giá trị không đổi, có thể bật comment dòng dưới nếu muốn debug kiểm tra vòng lặp
        // Serial.println("[Core 1 - Read] Gia tri khong doi, bo qua gui/in.");
      }
    } else {
      Serial.println("[Core 1 - Read] Loi doc cam bien DHT22!");
    }

    vTaskDelay(pdMS_TO_TICKS(4000)); // Chu kỳ đọc kiểm tra mỗi 4 giây
  }
}

// ==========================================
// TASK 3: CẢNH BÁO & ĐIỀU KHIỂN (Chạy trên Core 1)
// ==========================================
void taskAlertControl(void* pvParameters) {
  float humVal;
  float lastProcessedHum = -999.0; // Lưu lại mức độ ẩm đã xử lý điều khiển trước đó

  while (true) {
    // Nhận giá trị độ ẩm từ hàng đợi "dataAlertQueue"
    if (xQueueReceive(dataAlertQueue, &humVal, portMAX_DELAY) == pdPASS) {
      
      // Nếu độ ẩm không thay đổi so với lần xử lý trước thì bỏ qua không điều khiển lại liên tục
      if (abs(humVal - lastProcessedHum) < 0.1) {
        continue; 
      }
      lastProcessedHum = humVal; // Cập nhật lại mốc độ ẩm mới

      // Nếu đang ở chế độ AUTO, thực hiện thuật toán điều khiển tự động
      if (!manualMode) {
        if (humVal < 30.0) {
          Serial.println("[Core 1 - Alert AUTO] Do am < 30%: Bat 100% cong suat trong 30s");
          Serial.println("-> [TRANG THAI MIST]: BAT 100% (CHAY MIST)");
          digitalWrite(MIST_PIN, HIGH);
          vTaskDelay(pdMS_TO_TICKS(30000));
          digitalWrite(MIST_PIN, LOW);
          Serial.println("-> [TRANG THAI MIST]: TAT MIST SAU THOI GIAN CHAY");
        }
        else if (humVal >= 30.0 && humVal < 35.0) {
          Serial.println("[Core 1 - Alert AUTO] Do am 30-35%: Bat 50% cong suat trong 20s");
          Serial.println("-> [TRANG THAI MIST]: BAT 50% (CHAY MIST)");
          digitalWrite(MIST_PIN, HIGH);
          vTaskDelay(pdMS_TO_TICKS(20000));
          digitalWrite(MIST_PIN, LOW);
          Serial.println("-> [TRANG THAI MIST]: TAT MIST SAU THOI GIAN CHAY");
        }
        else if (humVal >= 35.0 && humVal <= 40.0) {
          Serial.println("[Core 1 - Alert AUTO] Do am 35-40%: Bat 25% cong suat trong 10s");
          Serial.println("-> [TRANG THAI MIST]: BAT 25% (CHAY MIST)");
          digitalWrite(MIST_PIN, HIGH);
          vTaskDelay(pdMS_TO_TICKS(10000));
          digitalWrite(MIST_PIN, LOW);
          Serial.println("-> [TRANG THAI MIST]: TAT MIST SAU THOI GIAN CHAY");
        }
        else {
          digitalWrite(MIST_PIN, LOW);
          Serial.println("-> [TRANG THAI MIST]: TAT MIST (Do am dat chuan >= 40%)");
        }
      } else {
        Serial.println("[Core 1 - Alert] Dang o che do MANUAL, bo qua dieu khien tu dong.");
      }
    }
  }
}

// --- Khởi tạo hệ thống RTOS ---
void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(MIST_PIN, OUTPUT);
  pinMode(FAN_PIN, OUTPUT);
  digitalWrite(MIST_PIN, LOW);
  digitalWrite(FAN_PIN, LOW);

  // Khởi tạo các hàng đợi (Queue)
  dataQueue = xQueueCreate(10, sizeof(SensorData));
  dataAlertQueue = xQueueCreate(10, sizeof(float));

  if (dataQueue == NULL || dataAlertQueue == NULL) {
    Serial.println("Khoi tao Queue that bai!");
    return;
  }

  // --- Tạo các Task FreeRTOS ---
  // Task 1: Truyền data (Core 0)
  xTaskCreatePinnedToCore(
    taskTransmitData,   
    "Task Transmit",    
    4096,               
    NULL,               
    1,                  
    NULL,               
    0                   // Chạy trên Core 0
  );

  // Task 2: Đọc data cảm biến (Core 1)
  xTaskCreatePinnedToCore(
    taskReadData,
    "Task Read Data",
    4096,
    NULL,
    2,
    NULL,
    1                   // Chạy trên Core 1
  );

  // Task 3: Cảnh báo & Điều khiển tự động (Core 1)
  xTaskCreatePinnedToCore(
    taskAlertControl,
    "Task Alert",
    4096,
    NULL,
    2,
    NULL,
    1                   // Chạy trên Core 1
  );
  
  Serial.println("He thong RTOS da khoi dong thanh cong!");
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}