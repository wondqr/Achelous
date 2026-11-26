#include <Arduino.h>
#include <HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include <WiFiServer.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include "firebase_upload.h"
#include "secrets.h"  //Host WiFi  (ค่าจริงอยู่ใน secrets.h ซึ่งห้ามอัปโหลดขึ้น GitHub)


const char* WIFI_SSID = SECRET_WIFI_SSID;
const char* WIFI_PASSWORD = SECRET_WIFI_PASSWORD;


//ESP-32 hotspot WiFi
const char* ssid = "Motor-ESP32";
const char* password = "motor1234";

WebServer server(80);

//SET PIN
#define FLOW_PIN        0
#define TEMP_PIN        4
#define TURBIDITY1_PIN  5
#define TURBIDITY2_PIN  6
#define PRESSURE1_PIN   7
#define PH_PIN          8
#define RELAY1_PIN      9
#define RELAY2_PIN      10
#define RELAY3_PIN      11
#define PRESSURE2_PIN   15
#define TDS1_PIN        17
#define TDS2_PIN        18

#define RELAY_VALVE1 RELAY1_PIN
#define RELAY_VALVE2 RELAY2_PIN
#define RELAY_VALVE3 RELAY2_PIN
#define RELAY_VALVE4 RELAY3_PIN

// ================= MOTOR DRIVER =================
const int STBY = 1;
const int PWMA = 2;
const int AIN1 = 41;
const int AIN2 = 40;


int motorSpeed = 0;
int motorDirection = 1;

void updateMotor() {
  digitalWrite(AIN1, motorDirection == 1 ? HIGH : LOW);
  digitalWrite(AIN2, motorDirection == 1 ? LOW : HIGH);
  ledcWrite(PWMA, motorSpeed);
}

// ================= AUTOMATIC PUMP CONTROL =================

bool automaticMode = true;
void controlPumpByWQI(float overallWQI)
{
  if (!automaticMode) return;

  if (!isfinite(overallWQI))
  {
    motorSpeed = 0;
    ledcWrite(PWMA, 0);
    Serial.println("Invalid WQI - Pump STOPPED");
    return;
  }

  if (overallWQI >= 70)
  {
    motorSpeed = 175;
  }
  else if (overallWQI >= 50)
  {
    motorSpeed = 100;
  }
  else if (overallWQI >= 25)
  {
    motorSpeed = 50;
  }
  else
  {
    motorSpeed = 0;
  }

  updateMotor();

  Serial.printf(
    "AUTO PUMP | Overall WQI: %.1f | Motor PWM: %d/255\n",
    overallWQI,
    motorSpeed
  );
}

const char page[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Motor Control</title>
<style>
body { font-family:Arial;text-align:center;padding:25px;background:#eef2ff; }
main { max-width:400px;margin:auto;background:white;padding:22px;border-radius:16px; }
input { width:100%; }
button { padding:14px;margin:5px;border:0;border-radius:8px;color:white;font-size:16px; }
</style>
</head>
<body>
<main>
<h2>ESP32 Motor Control</h2>
<h3>Speed: <span id="v">0</span>/255</h3>
<input id="s" type="range" min="0" max="255" value="0"
 oninput="document.getElementById('v').textContent=this.value"
 onchange="send('/speed?value='+this.value)">
<p id="status">Stopped</p>
<button style="background:green" onclick="send('/dir?d=f')">Forward</button>
<button style="background:blue" onclick="send('/dir?d=r')">Reverse</button>
<button style="background:red" onclick="stopMotor()">STOP</button>
<button style="background:#555" onclick="send('/mode?m=auto')">Auto</button>
<button style="background:#555" onclick="send('/mode?m=manual')">Manual</button>
</main>
<script>
function send(url) {
 fetch(url)
 .then(r => r.text())
 .then(t => document.getElementById('status').textContent=t)
 .catch(() => document.getElementById('status').textContent='Connection error');
}
function stopMotor() {
 document.getElementById('s').value=0;
 document.getElementById('v').textContent=0;
 send('/stop');
}
 let dragging = false;
const sl = document.getElementById('s');
sl.addEventListener('pointerdown', () => dragging = true);
sl.addEventListener('pointerup',   () => dragging = false);

function refresh() {
 fetch('/status').then(r => r.json()).then(d => {
  if (!dragging) {
   sl.value = d.speed;
   document.getElementById('v').textContent = d.speed;
  }
  document.getElementById('mode').textContent =
   (d.auto ? 'AUTO' : 'MANUAL') + ' | ' + (d.dir == 1 ? 'Forward' : 'Reverse');
 }).catch(() => {});
}
setInterval(refresh, 1000);
refresh();
</script>
</body>
</html>
)HTML";


OneWire oneWire(TEMP_PIN);
DallasTemperature tempSensor(&oneWire);

// ================= TIMING =================
const unsigned long MEASURE_INTERVAL_MS     = 1000;   // measure every 1 s
const unsigned long SEND_INTERVAL_NORMAL_MS = 5000;   // send every 5 s (clean)
const unsigned long SEND_INTERVAL_DIRTY_MS  = 1000;   // send every 1 s (dirty)
const float         DIRTY_WQI_THRESHOLD     = 50.0f;
const unsigned long DIRTY_HOLD_MS           = 10000;  // stay fast 10 s after last dirty
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 10000;
const unsigned long HTTP_TIMEOUT_MS         = 3000;
unsigned long lastMeasure = 0;
unsigned long lastSend = 0;
unsigned long lastDirtyTime = 0;

// ================= FLOW SENSOR: YF-S201 =================
const float FLOW_CALIBRATION = 7.5f;
const float FLOW_PULSES_PER_LITER = 450.0f;

volatile uint32_t flowPulseCount = 0;
uint32_t totalFlowPulses = 0;
unsigned long lastFlowMeasure = 0;

float flowRateLMin = 0.0f;
float totalVolumeLiters = 0.0f;
float flowFrequencyHz = 0.0f;

void ARDUINO_ISR_ATTR flowPulseISR()
{
  flowPulseCount++;
}

void updateFlowSensor()
{
  unsigned long now = millis();

  if (now - lastFlowMeasure < 1000) return;

  unsigned long elapsed = now - lastFlowMeasure;
  lastFlowMeasure = now;

  noInterrupts();
  uint32_t pulses = flowPulseCount;
  flowPulseCount = 0;
  interrupts();

  totalFlowPulses += pulses;
  flowFrequencyHz = pulses * 1000.0f / elapsed;
  flowRateLMin = flowFrequencyHz / FLOW_CALIBRATION;
  totalVolumeLiters = totalFlowPulses / FLOW_PULSES_PER_LITER;
}

// ================= SENSOR CALIBRATION =================
const int NUM_SAMPLES = 30;

const int RELAY_ON = LOW;
const int RELAY_OFF = HIGH;

const float TDS_FACTOR_1 = 1.00f;
const float TDS_FACTOR_2 = 1.00f;

const float TURB_DIVIDER_RATIO = 1.5f;
const float TURB_CLEAR_VOLTAGE = 1.585f;
const float TURB_MILK_VOLTAGE = 0.035f;
const float TURB_MILK_REFERENCE_NTU = 1015.5f;

const float WQI_THRESHOLD = 70.0f;
const float PH_MIN = 5.5f;
const float PH_MAX = 9.5f;

const float PRESSURE_DIVIDER_RATIO = 1.5f;
const float PRESSURE_VOLTAGE_MIN = 0.5f;
const float PRESSURE_VOLTAGE_MAX = 4.5f;
const float PRESSURE_RANGE_KPA = 800.0f;

// ================= SENSOR READ FUNCTIONS =================
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

struct PressureReading
{
  float adcV;
  float sensorV;
  float kPa;
  float MPa;
  float bar;
  float psi;
};

PressureReading readPressure(int pin)
{
  PressureReading r;

  r.adcV = readVoltage(pin, 10, 2);
  r.sensorV = r.adcV * PRESSURE_DIVIDER_RATIO;

  if (!isfinite(r.sensorV) ||
      r.sensorV < PRESSURE_VOLTAGE_MIN - 0.2f ||
      r.sensorV > PRESSURE_VOLTAGE_MAX + 0.2f)
  {
    r.kPa = r.MPa = r.bar = r.psi = NAN;
    return r;
  }

  float v = constrain(r.sensorV, PRESSURE_VOLTAGE_MIN, PRESSURE_VOLTAGE_MAX);

  r.kPa = (v - PRESSURE_VOLTAGE_MIN) /
          (PRESSURE_VOLTAGE_MAX - PRESSURE_VOLTAGE_MIN) *
          PRESSURE_RANGE_KPA;

  r.MPa = r.kPa / 1000.0f;
  r.bar = r.kPa / 100.0f;
  r.psi = r.kPa * 0.1450377f;

  return r;
}

String jsonNum(float v, int decimals)
{
  return isfinite(v) ? String(v, decimals) : String("null");
}

float calculateTDS(float voltage, float temperature, float calibrationFactor)
{
  float coefficient = 1.0f + 0.02f * (temperature - 25.0f);
  if (coefficient <= 0.0f) coefficient = 1.0f;

  float v = voltage / coefficient;
  float tds = (133.42f * v * v * v - 255.86f * v * v + 857.39f * v) * 0.5f;
  tds *= calibrationFactor;
  if (tds < 0.0f) tds = 0.0f;
  return tds;
}

float readTurbidity(int pin)
{
    const int samples = 30;
    uint64_t totalRaw = 0;
    uint64_t totalMilliVolts = 0;

    // Read ADC values and voltage
    for (int i = 0; i < samples; i++)
    {
        totalRaw += analogRead(pin);
        totalMilliVolts += analogReadMilliVolts(pin);
        delay(5);
    }

    float raw = totalRaw / (float)samples;
    float adcVoltage =(totalMilliVolts / (float)samples) / 1000.0f;
    float sensorVoltage = adcVoltage * TURB_DIVIDER_RATIO;
    float denominator =TURB_CLEAR_VOLTAGE - TURB_MILK_VOLTAGE;
    float ntu = NAN;

    if (denominator > 0)
    {
        ntu = ((TURB_CLEAR_VOLTAGE - sensorVoltage)
               / denominator) * TURB_MILK_REFERENCE_NTU;

        if (ntu < 0)
            ntu = 0;
    }
    return ntu;
}

float readPH()
{
  float adcVoltage = readVoltage(PH_PIN, 10, 2);
  float ph = 8.0f + ((2.5f - adcVoltage) / 0.18f);
  return ph;
}

// ================= WQI SCORING =================
const float W_PH = 0.34f;
const float W_TDS = 0.33f;
const float W_TURB = 0.33f;

struct Band
{
  float vmin;
  float vmax;
  float qmin;
  float qmax;
};

const Band TDS_BANDS[] = {
  {0, 100, 85, 100},
  {101, 150, 70, 84},
  {151, 300, 50, 69},
  {301, 500, 25, 49},
  {500, 750, 0, 24}
};

const Band TURB_BANDS[] = {
  {0, 0.5, 85, 100},
  {0.5, 0.99, 70, 84},
  {1, 10, 50, 69},
  {11, 20, 25, 49},
  {20, 100, 0, 24}
};

const Band PH_HIGH_BANDS[] = {
  {7, 8.5, 70, 100},
  {8.5, 9.5, 50, 69},
  {9.5, 10.5, 25, 49},
  {10.5, 14, 0, 24}
};

const Band PH_LOW_BANDS[] = {
  {6.5, 7, 70, 100},
  {5.5, 6.5, 50, 69},
  {4.0, 5.5, 25, 49},
  {0, 4.0, 0, 24}
};

float qDecreasing(float v, const Band &b)
{
  if (b.vmax == b.vmin) return b.qmin;
  float q = (b.vmin - v) / (b.vmax - b.vmin) * (b.qmax - b.qmin) + b.qmax;
  return constrain(q, b.qmin, b.qmax);
}

float qIncreasing(float v, const Band &b)
{
  if (b.vmax == b.vmin) return b.qmin;
  float q = (v - b.vmin) / (b.vmax - b.vmin) * (b.qmax - b.qmin) + b.qmin;
  return constrain(q, b.qmin, b.qmax);
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
  if (!isfinite(tds) || tds < 0) return 0.0f;
  return qFromBands(tds, TDS_BANDS, 5);
}

float qTurbidity(float ntu)
{
  if (!isfinite(ntu) || ntu < 0) return 0.0f;
  return qFromBands(ntu, TURB_BANDS, 5);
}

float qPH(float ph)
{
  if (!isfinite(ph) || ph < 0 || ph > 14) return 7.0f; 
  if (ph >= 7) return qFromBands(ph, PH_HIGH_BANDS, 4);
  for (int i = 0; i < 4; i++)
  {
    if (ph >= PH_LOW_BANDS[i].vmin)
      return qIncreasing(ph, PH_LOW_BANDS[i]);
  }
  return 0.0f;
}

float calculateWQI(float ph, float tds, float ntu)
{
  return W_PH * qPH(ph) + W_TDS * qTDS(tds) + W_TURB * qTurbidity(ntu);
}

const char* wqiLevel(float wqi)
{
  if (wqi >= 85) return "Very Clean";
  if (wqi >= 70) return "Clean";
  if (wqi >= 50) return "Normal";
  if (wqi >= 25) return "Dirty";
  return "Very Dirty";
}

// ================= RELAY CONTROL =================
void setValve(int pin, bool open)
{
  digitalWrite(pin, open ? RELAY_ON : RELAY_OFF);
}

void setValves(bool v1, bool v2, bool v3, bool v4)
{
  setValve(RELAY_VALVE1, v1);
  setValve(RELAY_VALVE2, v2);
  setValve(RELAY_VALVE3, v3);
  setValve(RELAY_VALVE4, v4);
}

void closeAllValves()
{
  setValves(false, false, false, false);
}

void controlValves(float pHwater, float wqiBefore, float wqiAfter)
{
  bool phUnsafe = !isfinite(pHwater) || pHwater < PH_MIN || pHwater > PH_MAX;

  if (phUnsafe)
  {
    setValves(true, true, true, false);

    Serial.println("WARNING: UNSAFE pH!");
    Serial.printf("pH = %.2f\n", pHwater);
    return;
  }
  if (!isfinite(wqiBefore))
  {
    closeAllValves();
  }
  else if (wqiBefore >= WQI_THRESHOLD)
  {
    setValve(RELAY_VALVE1, true);
    setValve(RELAY_VALVE2, false);
    setValve(RELAY_VALVE3, false);
  }
  else
  {
    setValve(RELAY_VALVE1, false);
    setValve(RELAY_VALVE2, true);
    setValve(RELAY_VALVE3, true);
  }

  if (!isfinite(wqiAfter) || wqiAfter < WQI_THRESHOLD)
  {
    setValve(RELAY_VALVE4, false);
  }
  else
  {
    setValve(RELAY_VALVE4, true);
  }

}

// ================= WIFI =================
void connectWiFi()
{
  if (WiFi.status() == WL_CONNECTED) return;

  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to WiFi");
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < WIFI_CONNECT_TIMEOUT_MS)
  {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("Host WiFi Connected!");
    Serial.print("STA IP: ");
    Serial.println(WiFi.localIP());
  }
  else
  {
    Serial.println("Host WiFi connection failed.");
    Serial.println("ESP32 hotspot is still available.");
  }
}

void printIPs() {
  Serial.println("---------- ESP32 IP ----------");
  Serial.printf("Hotspot  : %s -> http://%s\n", ssid, WiFi.softAPIP().toString().c_str());
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("Host WiFi: http://%s\n", WiFi.localIP().toString().c_str());
  else
    Serial.println("Host WiFi: not connected");
}

// ================= SETUP =================
void setup()
{
  Serial.begin(115200);
  delay(1000);

  pinMode(STBY, OUTPUT);
  pinMode(AIN1, OUTPUT);
  pinMode(AIN2, OUTPUT);

  digitalWrite(STBY, LOW);

  if (!ledcAttach(PWMA, 1000, 8)) {
    Serial.println("PWM initialization failed");
    while (true) delay(1000);
}

  ledcWrite(PWMA, 0);
  digitalWrite(STBY, HIGH);

  WiFi.mode(WIFI_AP_STA);

  bool apStarted = WiFi.softAP(ssid, password);

  if (apStarted)
  {
    Serial.println("ESP32 Hotspot Started!");
    Serial.print("SSID: ");
    Serial.println(ssid);

    Serial.print("Password: ");
    Serial.println(password);

    Serial.print("Motor Control Page: http://");
    Serial.println(WiFi.softAPIP());
  }
  else
  {
    Serial.println("ESP32 Hotspot Failed!");
  }

  server.on("/", []() {
    server.send(200, "text/html", page);
  });

  server.on("/speed", []() {
    if (server.hasArg("value")) {
      motorSpeed = constrain(server.arg("value").toInt(), 0, 255);
      updateMotor();
      Serial.printf("Speed: %d\n", motorSpeed);
    }
    server.send(200, "text/plain", "Speed updated");
  });

  server.on("/dir", []() {
    motorSpeed = 0;
    ledcWrite(PWMA, 0);
    delay(100);

    motorDirection = server.arg("d") == "r" ? -1 : 1;
    updateMotor();

    Serial.println(motorDirection == 1 ? "Forward" : "Reverse");
    server.send(200, "text/plain", "Direction updated");
  });

    server.on("/stop", []() {
    motorSpeed = 0;
    ledcWrite(PWMA, 0);
    Serial.println("Motor stopped");
    server.send(200, "text/plain", "Stopped");
  });

    server.on("/status", []() {
    String j = "{\"speed\":" + String(motorSpeed) +
               ",\"dir\":" + String(motorDirection) +
               ",\"auto\":" + String(automaticMode ? "true" : "false") + "}";
    server.send(200, "application/json", j);
  });

  server.on("/mode", []() {
    automaticMode = (server.arg("m") == "auto");
    server.send(200, "text/plain", automaticMode ? "Mode: AUTO" : "Mode: MANUAL");
  });

  server.begin();
  Serial.println("Server ready");

  pinMode(RELAY_VALVE1, OUTPUT);
  pinMode(RELAY_VALVE2, OUTPUT);
  pinMode(RELAY_VALVE3, OUTPUT);
  pinMode(RELAY_VALVE4, OUTPUT);
  closeAllValves();

  pinMode(FLOW_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), flowPulseISR, FALLING);
  lastFlowMeasure = millis();

  analogReadResolution(12);
  analogSetPinAttenuation(PH_PIN, ADC_11db);
  analogSetPinAttenuation(TDS1_PIN, ADC_11db);
  analogSetPinAttenuation(TDS2_PIN, ADC_11db);
  analogSetPinAttenuation(TURBIDITY1_PIN, ADC_11db);
  analogSetPinAttenuation(TURBIDITY2_PIN, ADC_11db);
  analogSetPinAttenuation(PRESSURE1_PIN, ADC_11db);
  analogSetPinAttenuation(PRESSURE2_PIN, ADC_11db);

  pinMode(TURBIDITY1_PIN, INPUT);
  pinMode(TURBIDITY2_PIN, INPUT);

  pinMode(PRESSURE1_PIN, INPUT);
  pinMode(PRESSURE2_PIN, INPUT);

  tempSensor.begin();
  connectWiFi();
  configTime(7 * 3600, 0, "pool.ntp.org", "time.google.com");   // เวลาไทย (UTC+7) สำหรับ key ของข้อมูลย้อนหลัง
  startUploadTask();

  Serial.println("ESP32-S3 Water Quality System Initialized.");
}

// ================= MAIN LOOP =================
void loop()
{

static unsigned long lastIpPrint = 0;
  if (millis() - lastIpPrint > 10000) { lastIpPrint = millis(); printIPs(); }

  server.handleClient();

  updateFlowSensor();

  if (WiFi.status() != WL_CONNECTED)
  {
    static unsigned long lastReconnect = 0;
    if (millis() - lastReconnect > 15000)
    {
      lastReconnect = millis();
      WiFi.reconnect();
    }
  }

  unsigned long now = millis();
  if (now - lastMeasure < MEASURE_INTERVAL_MS) return;
  lastMeasure = now;

  // ----- TEMPERATURE -----
  tempSensor.requestTemperatures();
  float temperature = tempSensor.getTempCByIndex(0);
  if (temperature == DEVICE_DISCONNECTED_C || !isfinite(temperature))
  {
    temperature = 25.0f;
  }

  // ----- pH -----
  float pHwater = readPH();
  if (!isfinite(pHwater) || pHwater < 0 || pHwater > 14)
  {
    pHwater = 7.0f; 
  }

  // ----- TDS -----
  float voltageTDS1 = readVoltage(TDS1_PIN, NUM_SAMPLES, 2);
  float tds1 = calculateTDS(voltageTDS1, temperature, TDS_FACTOR_1);

  float voltageTDS2 = readVoltage(TDS2_PIN, NUM_SAMPLES, 2);
  float tds2 = calculateTDS(voltageTDS2, temperature, TDS_FACTOR_2);

  // ----- TURBIDITY -----
  float ntu1 = readTurbidity(TURBIDITY1_PIN);
  float ntu2 = readTurbidity(TURBIDITY2_PIN);

  // ----- WQI -----
  float wqi1 = calculateWQI(pHwater, tds1, ntu1);
  float wqi2 = calculateWQI(pHwater, tds2, ntu2);
  float averageTDS = (tds1 + tds2) / 2.0f;
  float averageNTU = (ntu1 + ntu2) / 2.0f;
  float overallWQI = calculateWQI(pHwater, averageTDS, averageNTU);
  controlPumpByWQI(overallWQI);

  // ----- VALVES -----
  controlValves(pHwater, wqi1, wqi2);

  // ----- PRESSURE -----
  PressureReading p1 = readPressure(PRESSURE1_PIN);
  PressureReading p2 = readPressure(PRESSURE2_PIN);

  // ----- HISTORY (สรุปรายชั่วโมง/รายวันลง Firebase) -----
  HistorySample hs = {
    { overallWQI, wqi1, wqi2, pHwater, temperature, tds1, tds2, ntu1, ntu2 },
    digitalRead(RELAY_VALVE2) == RELAY_ON,
    totalVolumeLiters
  };
  historyRecord(hs);

  // Upload mornitor data's speed in site pagex
  if (isfinite(overallWQI) && overallWQI < DIRTY_WQI_THRESHOLD)
{
  lastDirtyTime = now;
}
bool dirtyMode = (lastDirtyTime != 0) && (now - lastDirtyTime < DIRTY_HOLD_MS);
unsigned long sendInterval = dirtyMode ? SEND_INTERVAL_DIRTY_MS : SEND_INTERVAL_NORMAL_MS;

if (now - lastSend < sendInterval) return;   // measured, but not time to send yet
lastSend = now;


  // ----- JSON UPLOAD -----
  String json = "{";
  json += "\"temperature\":" + String(temperature, 2) + ",";
  json += "\"ph\":" + String(pHwater, 2) + ",";
  json += "\"tds1\":" + String(tds1, 2) + ",";
  json += "\"tds2\":" + String(tds2, 2) + ",";
  json += "\"turbidity1\":" + String(ntu1, 2) + ",";
  json += "\"turbidity2\":" + String(ntu2, 2) + ",";
  json += "\"wqi\":" + String(overallWQI, 1) + ",";
  json += "\"wqi1\":" + String(wqi1, 1) + ",";
  json += "\"wqi2\":" + String(wqi2, 1) + ",";
  json += "\"flowRate\":" + String(flowRateLMin, 2) + ",";
  json += "\"totalVolume\":" + String(totalVolumeLiters, 3) + ",";
  json += "\"flowFrequency\":" + String(flowFrequencyHz, 2) + ",";
  json += "\"pressureKPa\":" + jsonNum(p1.kPa, 2) + ",";
  json += "\"pressureMPa\":" + jsonNum(p1.MPa, 4) + ",";
  json += "\"pressureBar\":" + jsonNum(p1.bar, 3) + ",";
  json += "\"pressurePsi\":" + jsonNum(p1.psi, 2) + ",";
  json += "\"pressure2KPa\":" + jsonNum(p2.kPa, 2) + ",";
  json += "\"pressure2MPa\":" + jsonNum(p2.MPa, 4) + ",";
  json += "\"pressure2Bar\":" + jsonNum(p2.bar, 3) + ",";
  json += "\"pressure2Psi\":" + jsonNum(p2.psi, 2) + ",";
  json += "\"valve1Open\":" + String(digitalRead(RELAY_VALVE1) == RELAY_ON ? "true" : "false") + ",";
  json += "\"valve2Open\":" + String(digitalRead(RELAY_VALVE2) == RELAY_ON ? "true" : "false") + ",";
  json += "\"valve3Open\":" + String(digitalRead(RELAY_VALVE3) == RELAY_ON ? "true" : "false") + ",";
  json += "\"valve4Open\":" + String(digitalRead(RELAY_VALVE4) == RELAY_ON ? "true" : "false"); 
  json += ",\"timestamp\":{\".sv\":\"timestamp\"}"; 
  json += "}";

  // ----- FULL SERIAL DEBUG PRINTING -----
  Serial.println("\n========== WATER QUALITY ==========");
  Serial.printf("Temperature : %.2f °C\n", temperature);
  Serial.printf("pH          : %.2f\n", pHwater);
  Serial.printf("TDS Before  : %.2f ppm\n", tds1);
  Serial.printf("TDS After   : %.2f ppm\n", tds2);
  Serial.printf("Turbidity 1 : %.2f NTU\n", ntu1);
  Serial.printf("Turbidity 2 : %.2f NTU\n", ntu2);
  Serial.printf("WQI Before  : %.1f %s\n", wqi1, wqiLevel(wqi1));
  Serial.printf("WQI After   : %.1f %s\n", wqi2, wqiLevel(wqi2));
  Serial.printf("Overall WQI : %.1f %s\n", overallWQI, wqiLevel(overallWQI));


  queueUpload(json);
}
