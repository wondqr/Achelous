#include <Arduino.h>
#include <HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <math.h>

// =================================================
// WIFI AND BACKEND
// =================================================
const char* WIFI_SSID = "SPACE AC";
const char* WIFI_PASSWORD = "0632317613";
const char* SERVER_URL = "https://achelous-1.onrender.com/api/sensor";

// =================================================
// ESP32-S3 PIN CONFIGURATION
// =================================================
#define TEMP_PIN        4
#define PH_PIN          8
#define TDS1_PIN        17
#define TDS2_PIN        18
#define TURBIDITY1_PIN  5
#define TURBIDITY2_PIN  6

// IMPORTANT:
// ESP32 ADC pins must not receive more than 3.3 V.
// Use an appropriate voltage divider when a sensor output can exceed 3.3 V.
// For the turbidity formula below, Code 2 assumes a 10k/20k divider.

OneWire oneWire(TEMP_PIN);
DallasTemperature tempSensor(&oneWire);

const unsigned long SEND_INTERVAL_MS = 2000;
unsigned long lastSend = 0;

// =================================================
// SENSOR CALIBRATION SETTINGS FROM CODE 2
// =================================================
const int NUM_SAMPLES = 30;
const int TURB_NUM_SAMPLES = 50;

const float TDS_FACTOR_1 = 1.00f;
const float TDS_FACTOR_2 = 1.00f;

const float TURB_DIVIDER_RATIO = 1.5f;
const float TURB_CLEAR_VOLTAGE = 1.590f;
const float TURB_MILK_VOLTAGE = 0.0555f;
const float TURB_MILK_REFERENCE_NTU = 1000.0f;

// =================================================
// READ AVERAGE ANALOG VOLTAGE
// =================================================
float readVoltage(int pin, int samples, int delayMs)
{
  uint64_t totalMilliVolts = 0;

  for (int i = 0; i < samples; i++)
  {
    totalMilliVolts += analogReadMilliVolts(pin);
    delay(delayMs);
  }

  return (totalMilliVolts / (float)samples) / 1000.0f;
}

// =================================================
// CALCULATE TDS (ppm), WITH TEMPERATURE COMPENSATION
// =================================================
float calculateTDS(float voltage, float temperature, float calibrationFactor)
{
  float compensationCoefficient =
      1.0f + 0.02f * (temperature - 25.0f);

  // Avoid division by zero if an invalid temperature is received.
  if (compensationCoefficient <= 0.0f)
    compensationCoefficient = 1.0f;

  float compensatedVoltage = voltage / compensationCoefficient;

  float tds =
      (133.42f * compensatedVoltage * compensatedVoltage * compensatedVoltage
       - 255.86f * compensatedVoltage * compensatedVoltage
       + 857.39f * compensatedVoltage) * 0.5f;

  tds *= calibrationFactor;

  if (tds < 0.0f)
    tds = 0.0f;

  return tds;
}

// =================================================
// CALCULATE TURBIDITY (experimental NTU estimate)
// =================================================
float readTurbidityNTU(int pin)
{
  float adcVoltage = readVoltage(pin, TURB_NUM_SAMPLES, 10);
  float sensorVoltage = adcVoltage * TURB_DIVIDER_RATIO;

  float ntu =
      (TURB_CLEAR_VOLTAGE - sensorVoltage) /
      (TURB_CLEAR_VOLTAGE - TURB_MILK_VOLTAGE) *
      TURB_MILK_REFERENCE_NTU;

  if (ntu < 0.0f)
    ntu = 0.0f;

  return ntu;
}

// =================================================
// CALCULATE pH (must be calibrated for your probe)
// =================================================
float readPH()
{
  float adcVoltage = readVoltage(PH_PIN, 10, 5);
  return 8.0f + ((2.5f - adcVoltage) / 0.18f);
}

// =================================================
// WQI SCORING
// These bands and formulas are copied from Code 2.
// Verify the ranges and calibration for your project.
// =================================================
const float W_PH   = 0.34f;
const float W_TDS  = 0.33f;
const float W_TURB = 0.33f;

struct Band
{
  float vmin;
  float vmax;
  float qmin;
  float qmax;
};

const Band TDS_BANDS[] = {
  {0,   100, 85, 100},
  {101, 150, 70, 84},
  {151, 300, 50, 69},
  {301, 500, 25, 49},
  {500, 750, 0,  24}
};

const Band TURB_BANDS[] = {
  {0,   0.5,  85, 100},
  {0.5, 0.99, 70, 84},
  {1,   10,   50, 69},
  {11,  20,   25, 49},
  {20,  100,  0,  24}
};

const Band PH_HIGH_BANDS[] = {
  {7,    8.5,  70, 100},
  {8.5,  9.5,  50, 69},
  {9.5, 10.5,  25, 49},
  {10.5, 14,    0,  24}
};

const Band PH_LOW_BANDS[] = {
  {6.5, 7,   70, 100},
  {5.5, 6.5, 50, 69},
  {4.0, 5.5, 25, 49},
  {0,   4.0, 0,  24}
};

float qDecreasing(float v, const Band &b)
{
  if (b.vmax == b.vmin)
    return b.qmin;

  float q = (b.vmin - v) / (b.vmax - b.vmin)
            * (b.qmax - b.qmin) + b.qmax;

  if (q > b.qmax) q = b.qmax;
  if (q < b.qmin) q = b.qmin;
  return q;
}

float qIncreasing(float v, const Band &b)
{
  if (b.vmax == b.vmin)
    return b.qmin;

  float q = (v - b.vmin) / (b.vmax - b.vmin)
            * (b.qmax - b.qmin) + b.qmin;

  if (q > b.qmax) q = b.qmax;
  if (q < b.qmin) q = b.qmin;
  return q;
}

float qFromBands(float v, const Band* bands, int n)
{
  for (int i = 0; i < n; i++)
  {
    if (v <= bands[i].vmax)
      return qDecreasing(v, bands[i]);
  }
  return 0.0f;
}

float qTDS(float tds)
{
  if (tds < 0) return 0;
  return qFromBands(tds, TDS_BANDS, 5);
}

float qTurbidity(float ntu)
{
  if (ntu < 0) return 0;
  return qFromBands(ntu, TURB_BANDS, 5);
}

float qPH(float ph)
{
  if (ph < 0 || ph > 14) return 0;

  if (ph >= 7)
    return qFromBands(ph, PH_HIGH_BANDS, 4);

  for (int i = 0; i < 4; i++)
  {
    if (ph >= PH_LOW_BANDS[i].vmin)
      return qIncreasing(ph, PH_LOW_BANDS[i]);
  }

  return 0;
}

float calculateWQI(float ph, float tds, float ntu)
{
  return W_PH * qPH(ph)
       + W_TDS * qTDS(tds)
       + W_TURB * qTurbidity(ntu);
}

const char* wqiLevel(float wqi)
{
  if (wqi >= 85) return "Very Clean";
  if (wqi >= 70) return "Clean";
  if (wqi >= 50) return "Normal";
  if (wqi >= 25) return "Dirty";
  return "Very Dirty";
}

// =================================================
// WIFI
// =================================================
void connectWiFi()
{
  if (WiFi.status() == WL_CONNECTED)
    return;

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("Connected. ESP32 IP: ");
  Serial.println(WiFi.localIP());
}

// =================================================
// SETUP
// =================================================
void setup()
{
  Serial.begin(115200);
  delay(1000);

  analogReadResolution(12);

  analogSetPinAttenuation(PH_PIN, ADC_11db);
  analogSetPinAttenuation(TDS1_PIN, ADC_11db);
  analogSetPinAttenuation(TDS2_PIN, ADC_11db);
  analogSetPinAttenuation(TURBIDITY1_PIN, ADC_11db);
  analogSetPinAttenuation(TURBIDITY2_PIN, ADC_11db);

  tempSensor.begin();
  connectWiFi();

  Serial.println();
  Serial.println("====================================");
  Serial.println(" ESP32-S3 WATER QUALITY MONITOR");
  Serial.println(" 2 TDS + 2 TURBIDITY + pH + TEMP");
  Serial.println(" Sending data to Render backend");
  Serial.println("====================================");
}

// =================================================
// MAIN LOOP
// =================================================
void loop()
{
  if (WiFi.status() != WL_CONNECTED)
    connectWiFi();

  unsigned long now = millis();
  if (now - lastSend < SEND_INTERVAL_MS)
    return;

  lastSend = now;

  // 1. Temperature
  tempSensor.requestTemperatures();
  float temperature = tempSensor.getTempCByIndex(0);

  if (temperature == DEVICE_DISCONNECTED_C || isnan(temperature))
  {
    Serial.println("ERROR: DS18B20 disconnected. Check sensor and 4.7k pull-up.");
    return;
  }

  // 2. pH
  float pHwater = readPH();

  // 3. TDS sensor 1
  float voltageTDS1 = readVoltage(TDS1_PIN, NUM_SAMPLES, 5);
  float tds1 = calculateTDS(voltageTDS1, temperature, TDS_FACTOR_1);

  // 4. TDS sensor 2
  float voltageTDS2 = readVoltage(TDS2_PIN, NUM_SAMPLES, 5);
  float tds2 = calculateTDS(voltageTDS2, temperature, TDS_FACTOR_2);

  // 5. Turbidity sensor 1 and 2
  float ntu1 = readTurbidityNTU(TURBIDITY1_PIN);
  float ntu2 = readTurbidityNTU(TURBIDITY2_PIN);

  // 6. WQI values
  float wqi1 = calculateWQI(pHwater, tds1, ntu1);
  float wqi2 = calculateWQI(pHwater, tds2, ntu2);

  float averageTDS = (tds1 + tds2) / 2.0f;
  float averageNTU = (ntu1 + ntu2) / 2.0f;
  float overallWQI = calculateWQI(pHwater, averageTDS, averageNTU);

  // 7. Send JSON to Render.
  // Property names are aligned with Code 2:
  // temperature, ph, tds1, tds2, turbidity1, turbidity2, wqi
  String json = "{";
  json += "\"temperature\":" + String(temperature, 2) + ",";
  json += "\"ph\":" + String(pHwater, 2) + ",";
  json += "\"tds1\":" + String(tds1, 2) + ",";
  json += "\"tds2\":" + String(tds2, 2) + ",";
  json += "\"turbidity1\":" + String(ntu1, 2) + ",";
  json += "\"turbidity2\":" + String(ntu2, 2) + ",";
  json += "\"wqi\":" + String(overallWQI, 1);
  json += "}";

  WiFiClientSecure client;
  client.setInsecure(); // For testing only; use certificate validation for production.

  HTTPClient http;
  http.setTimeout(10000);

  if (http.begin(client, SERVER_URL))
  {
    http.addHeader("Content-Type", "application/json");
    int httpCode = http.POST(json);

    Serial.println();
    Serial.println("========== WATER QUALITY ==========");
    Serial.printf("Temperature : %.2f C\n", temperature);
    Serial.printf("pH          : %.2f\n", pHwater);
    Serial.printf("TDS 1       : %.2f ppm\n", tds1);
    Serial.printf("TDS 2       : %.2f ppm\n", tds2);
    Serial.printf("Turbidity 1 : %.2f NTU (estimated)\n", ntu1);
    Serial.printf("Turbidity 2 : %.2f NTU (estimated)\n", ntu2);
    Serial.printf("WQI Pair 1  : %.1f - %s\n", wqi1, wqiLevel(wqi1));
    Serial.printf("WQI Pair 2  : %.1f - %s\n", wqi2, wqiLevel(wqi2));
    Serial.printf("Overall WQI : %.1f - %s\n", overallWQI, wqiLevel(overallWQI));
    Serial.printf("POST status : %d\n", httpCode);
    Serial.print("JSON        : ");
    Serial.println(json);

    if (httpCode > 0)
    {
      String response = http.getString();
      Serial.print("Server reply: ");
      Serial.println(response);
    }
    else
    {
      Serial.print("HTTP error: ");
      Serial.println(http.errorToString(httpCode));
    }

    http.end();
  }
  else
  {
    Serial.println("Could not connect to SERVER_URL");
  }
}
