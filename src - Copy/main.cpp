#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ----- WiFi -----
const char* WIFI_SSID     = "SPACE AC";
const char* WIFI_PASSWORD = "0632317613";

// ----- Backend (Render) -----
const char* SERVER_URL = "https://achelous-1.onrender.com/api/sensor";

// ----- Pin assignments สำหรับ ESP32-S3-WROOM-1 (ปลอดภัยทุกรุ่น ไม่ชนกับ flash/PSRAM) -----
#define PIN_DS18B20   4   // OneWire data — ต้องมีตัวต้านทาน pull-up 4.7k ไป 3.3V
#define PIN_PH        1   // ADC1_CH0
#define PIN_TDS       2   // ADC1_CH1
#define PIN_TURBIDITY 5   // ADC1_CH4

// ⚠️ โมดูล pH/TDS/Turbidity ส่วนใหญ่จ่ายสัญญาณ analog สูงสุด 5V
// แต่ขา ADC ของ ESP32 รับได้ไม่เกิน 3.3V — ต้องมีวงจรแบ่งแรงดัน (voltage divider)
// คั่นก่อนเข้าแต่ละขานี้ ไม่งั้นขา ADC อาจเสียหายได้

OneWire oneWire(PIN_DS18B20);
DallasTemperature tempSensor(&oneWire);

const unsigned long SEND_INTERVAL_MS = 2000;
unsigned long lastSend = 0;

void connectWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected, IP: ");
  Serial.println(WiFi.localIP());
}

void setup() {
  Serial.begin(115200);
  tempSensor.begin();
  connectWiFi();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  unsigned long now = millis();
  if (now - lastSend >= SEND_INTERVAL_MS) {
    lastSend = now;

    tempSensor.requestTemperatures();
    float tempC = tempSensor.getTempCByIndex(0);

    // ค่า ADC ดิบ (0–4095) — ยังไม่มีสูตร calibration
    int phRaw   = analogRead(PIN_PH);
    int tdsRaw  = analogRead(PIN_TDS);
    int turbRaw = analogRead(PIN_TURBIDITY);

    String json = "{\"temp\":" + String(tempC, 1) +
                  ",\"ph\":" + String(phRaw) +
                  ",\"tds\":" + String(tdsRaw) +
                  ",\"turb\":" + String(turbRaw) + "}";

    WiFiClientSecure client;
    client.setInsecure(); // ข้ามการตรวจ certificate — โอเคสำหรับทดสอบ

    HTTPClient http;
    http.begin(client, SERVER_URL);
    http.addHeader("Content-Type", "application/json");
    int httpCode = http.POST(json);

    Serial.print("POST -> ");
    Serial.print(httpCode);
    Serial.print(" | ");
    Serial.println(json);

    http.end();
  }
}