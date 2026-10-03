#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ----- Pin assignments (ปรับตามการต่อสายจริง) -----
#define PIN_DS18B20   4   // OneWire data — ต้องมีตัวต้านทาน pull-up 4.7k โอห์ม ไป 3.3V//Temp
#define PIN_PH        37  // ADC1_CH6, analog input only
#define PIN_TDS       35  // ADC1_CH7, analog input only
#define PIN_TURBIDITY 32  // ADC1_CH4

// ⚠️ โมดูล pH/TDS/Turbidity ส่วนใหญ่จ่ายสัญญาณ analog สูงสุด 5V
// แต่ขา ADC ของ ESP32 รับได้ไม่เกิน 3.3V — ต้องมีวงจรแบ่งแรงดัน (voltage divider)
// คั่นก่อนเข้าแต่ละขานี้ ไม่งั้นขา ADC อาจเสียหายได้

OneWire oneWire(PIN_DS18B20);
DallasTemperature tempSensor(&oneWire);

const unsigned long SEND_INTERVAL_MS = 1000;
unsigned long lastSend = 0;

void setup() {
  Serial.begin(115200);
  tempSensor.begin();
}

void loop() {
  unsigned long now = millis();
  if (now - lastSend >= SEND_INTERVAL_MS) {
    lastSend = now;

    tempSensor.requestTemperatures();
    float tempC = tempSensor.getTempCByIndex(0);

    // ค่า ADC ดิบ (0–4095) — ยังไม่มีสูตร calibration
    int phRaw   = analogRead(PIN_PH);
    int tdsRaw  = analogRead(PIN_TDS);
    int turbRaw = analogRead(PIN_TURBIDITY);

    // ส่งเป็น JSON บรรทัดเดียว ให้เว็บอ่านผ่าน Web Serial API
    Serial.print("{\"temp\":");
    Serial.print(tempC, 1);
    Serial.print(",\"ph\":");
    Serial.print(phRaw);
    Serial.print(",\"tds\":");
    Serial.print(tdsRaw);
    Serial.print(",\"turb\":");
    Serial.print(turbRaw);
    Serial.println("}");
  }
}