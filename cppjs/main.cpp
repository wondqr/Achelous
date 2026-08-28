#include <OneWire.h>
#include <DallasTemperature.h>

#define PIN_DS18B20     4
#define PIN_PH_SENSOR   34
#define PIN_TDS_SENSOR  35
#define PIN_TURBIDITY   32

OneWire oneWire(PIN_DS18B20);
DallasTemperature tempSensor(&oneWire);

void setup() {
  Serial.begin(115200);
  tempSensor.begin();
  analogReadResolution(12); // ESP32 ADC 12-bit (0-4095)
}

void loop() {
  // อุณหภูมิ
  tempSensor.requestTemperatures();
  float temperature = tempSensor.getTempCByIndex(0);

  // pH, TDS, Turbidity: อ่านค่า ADC แล้วแปลงเป็นแรงดันไฟฟ้า
  float phVoltage = analogRead(PIN_PH_SENSOR) * (3.3 / 4095.0);
  float tdsVoltage = analogRead(PIN_TDS_SENSOR) * (3.3 / 4095.0);
  float turbidityVoltage = analogRead(PIN_TURBIDITY) * (3.3 / 4095.0);

  Serial.print("Temp: "); Serial.print(temperature); Serial.println(" C");
  Serial.print("pH Voltage: "); Serial.print(phVoltage); Serial.println(" V");
  Serial.print("TDS Voltage: "); Serial.print(tdsVoltage); Serial.println(" V");
  Serial.print("Turbidity Voltage: "); Serial.print(turbidityVoltage); Serial.println(" V");
  Serial.println("---");

  delay(2000);
}















#include <WiFi.h>
#include <HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#define PIN_DS18B20     4
#define PIN_PH_SENSOR   34
#define PIN_TDS_SENSOR  35
#define PIN_TURBIDITY   32

const char* WIFI_SSID = "wifiname";
const char* WIFI_PASSWORD = "wifipassword";
const char* SERVER_URL = "https://your-app-name.onrender.com/api/sensor-data";

OneWire oneWire(PIN_DS18B20);
DallasTemperature tempSensor(&oneWire);

void setup() {
  Serial.begin(115200);
  tempSensor.begin();
  analogReadResolution(12);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("กำลังเชื่อมต่อ WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nเชื่อมต่อสำเร็จ! IP: " + WiFi.localIP().toString());
}

void loop() {
  // 1. อ่านค่าเซนเซอร์
  tempSensor.requestTemperatures();
  float temperature = tempSensor.getTempCByIndex(0);
  float phVoltage = analogRead(PIN_PH_SENSOR) * (3.3 / 4095.0);
  float tdsVoltage = analogRead(PIN_TDS_SENSOR) * (3.3 / 4095.0);
  float turbidityVoltage = analogRead(PIN_TURBIDITY) * (3.3 / 4095.0);

  // 2. สร้างข้อความ JSON ส่งไปให้ server
  String jsonData = "{\"temperature\":" + String(temperature) +
                     ",\"ph\":" + String(phVoltage) +
                     ",\"tds\":" + String(tdsVoltage) +
                     ",\"turbidity\":" + String(turbidityVoltage) + "}";

  // 3. ส่งไปที่ server
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(SERVER_URL);
    http.addHeader("Content-Type", "application/json");
    int responseCode = http.POST(jsonData);
    Serial.println("ส่งข้อมูล -> รหัสตอบกลับ: " + String(responseCode));
    http.end();
  }

  delay(2000); // ส่งทุก 2 วินาที
}