#include <Arduino.h>
#include <HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include <WiFiServer.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <math.h>

//Host WiFi
const char* WIFI_SSID = "f16";
const char* WIFI_PASSWORD = "tawan123";
const char* SERVER_URL ="http://192.168.26.6:4000/api/sensor";//ถ้าใช้คอมต่อ wifi เลาเตอร์ใช้ ip คอม แต่ถ้าแชร์เน็ตใช้ ip เครื่องที่แชร์เน็ต
// ดู ip จาก cmd --> ipconfig

// Must be identical to the DEVICE_API_KEY environment variable on the Render server.
const char* DEVICE_API_KEY = "PUT-YOUR-DEVICE-KEY-HERE";

//ESP-32 hotspot WiFi
const char* ssid = "Motor-ESP32";
const char* password = "motor1234";

WebServer server(80);

//SET PIN
#define TEMP_PIN        4
#define TURBIDITY1_PIN  5
#define TURBIDITY2_PIN  6
#define PRESSURE1_PIN   7
#define PH_PIN          8
#define RELAY1_PIN      9
#define RELAY2_PIN      10
#define RELAY3_PIN      11
#define PRESSURE2_PIN   15
#define FLOW_PIN        16
#define TDS1_PIN        17
#define TDS2_PIN        18

#define RELAY_VALVE1 RELAY1_PIN
#define RELAY_VALVE2 RELAY2_PIN
#define RELAY_VALVE3 RELAY3_PIN

// ================= MOTOR DRIVER =================
const int STBY = 1;
const int PWMA = 2;
const int AIN1 = 41;
const int AIN2 = 40;

const uint32_t MOTOR_PWM_FREQUENCY = 1000;
const uint8_t MOTOR_PWM_RESOLUTION = 8;

// Adjustable WQI thresholds

const float WQI_SLOW_THRESHOLD = 70.0f;
const float WQI_MEDIUM_THRESHOLD = 50.0f;

// Adjustable motor speeds (0-255)

int SLOW_PWM = 64;
int MEDIUM_PWM = 140;
int FAST_PWM = 217;

// Safety timeout

const unsigned long WQI_TIMEOUT_MS = 15000;
const unsigned long WIFI_RETRY_INTERVAL_MS = 10000;

// Motor status

int motorSpeed = 0;

bool motorRunning = false;
bool emergencyStop = false;

String pumpLevel = "WAITING FOR WQI";

float currentWQI = -1.0f;

unsigned long lastWQIUpdate = 0;
unsigned long lastWiFiAttempt = 0;

bool lastPHReadingSafe = false;
bool lastWQISensorsValid = false;
bool hostWiFiWasConnected = false;

// ========================================
// MOTOR CONTROL
// ========================================

void updateMotor()
{
    digitalWrite(AIN1, HIGH);
    digitalWrite(AIN2, LOW);
    ledcWrite(PWMA, motorSpeed);
    motorRunning = motorSpeed > 0;
}

void stopMotor()
{
    motorSpeed = 0;

    ledcWrite(PWMA, 0);

    motorRunning = false;
}

void applyAutomaticMotorControl()
{
    if (emergencyStop)
    {
        stopMotor();
        pumpLevel = "EMERGENCY STOP";
        return;
    }

    if (lastWQIUpdate == 0 ||
        millis() - lastWQIUpdate > WQI_TIMEOUT_MS)
    {
        stopMotor();
        pumpLevel = "WAITING FOR WQI";
        return;
    }

    if (!lastPHReadingSafe)
    {
        stopMotor();
        pumpLevel = "UNSAFE PH";
        return;
    }

    if (!lastWQISensorsValid ||
        !isfinite(currentWQI) ||
        currentWQI < 0 ||
        currentWQI > 100)
    {
        stopMotor();
        pumpLevel = "SENSOR ERROR";
        return;
    }

    // CONDITION 1: WQI 70-100
    if (currentWQI >= WQI_SLOW_THRESHOLD)
    {
        motorSpeed = constrain(SLOW_PWM, 0, 255);
        pumpLevel = "SLOW";
    }

    // CONDITION 2: WQI 50-69.99
    else if (currentWQI >= WQI_MEDIUM_THRESHOLD)
    {
        motorSpeed = constrain(MEDIUM_PWM, 0, 255);
        pumpLevel = "MEDIUM";
    }

    // CONDITION 3: WQI below 50
    else
    {
        motorSpeed = constrain(FAST_PWM, 0, 255);
        pumpLevel = "FAST";
    }

    updateMotor();
}

void acceptNewWQI(
    float wqi,
    bool phReadingSafe,
    bool sensorReadingsValid)
{
    currentWQI = isfinite(wqi) ? wqi : -1.0f;
    lastPHReadingSafe = phReadingSafe;
    lastWQISensorsValid = sensorReadingsValid;
    lastWQIUpdate = millis();

    applyAutomaticMotorControl();

    Serial.printf(
        "Motor | WQI: %.1f | Level: %s | PWM: %d/255\n",
        currentWQI,
        pumpLevel.c_str(),
        motorSpeed
    );
}

void checkMotorWQITimeout()
{
    if (!emergencyStop &&
        lastWQIUpdate != 0 &&
        millis() - lastWQIUpdate > WQI_TIMEOUT_MS)
    {
        stopMotor();
        pumpLevel = "WQI DATA TIMEOUT";
    }
}

// ========================================
// MOTOR DASHBOARD
// ========================================

const char page[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html>
<head>

<meta name="viewport" content="width=device-width, initial-scale=1">

<title>Automatic Motor Controller</title>

<style>

body {
    font-family: Arial;
    text-align: center;
    background: #eef2ff;
    padding: 20px;
}
main {
    max-width: 430px;
    margin: auto;
    background: white;
    padding: 25px;
    border-radius: 18px;
    box-shadow: 0 4px 15px #0002;
}
h2 {
    color: #123b55;
}
.value {
    font-size: 35px;
    font-weight: bold;
    margin: 12px;
}
.info {
    background: #f1f5f9;
    padding: 15px;
    margin: 12px 0;
    border-radius: 12px;
}
button {
    width: 100%;
    padding: 14px;
    margin-top: 12px;
    border: none;
    border-radius: 9px;
    color: white;
    font-size: 16px;
    cursor: pointer;
}
.start {
    background: #16a34a;
}

.stop {
    background: #dc2626;
}

.small {
    font-size: 13px;
    color: #64748b;
}
</style>
</head>
<body>
<main>

<h2>ESP32 Automatic Motor</h2>

<p class="small">WQI-Based Water Pump Control</p>

<div class="info">
    <p>Current WQI</p>
    <div class="value" id="wqi">--</div>
</div>
<div class="info">
    <p>Pump Level</p>
    <div class="value" id="level">WAITING</div>
</div>

<div class="info">
    <p>PWM Duty</p>
    <div class="value">
        <span id="pwm">0</span>/255
    </div>
    <p id="percent">0%</p>
</div>
<div class="info">
    <p>PWM Frequency</p>
    <div class="value" id="pwmFrequency">1000 Hz</div>
    <p class="small">
        PWM frequency is fixed; duty cycle changes pump power.
    </p>
</div>
<div class="info">
    <p>Water Flow Sensor Frequency</p>
    <div class="value">
        <span id="flowHz">0</span> Hz
    </div>
    <p>
        <span id="flowRate">0</span> L/min
    </p>
</div>
<div class="info">
    <p>Motor Status</p>
    <div class="value" id="motor">STOPPED</div>
</div>

<button class="start" onclick="send('/auto')">
    RESUME AUTOMATIC MODE
</button>

<button class="stop" onclick="send('/stop')">
    EMERGENCY STOP
</button>

<p class="small" id="connection">
    Connecting...
</p>

</main>
<script>

async function updateStatus()
{
    try
    {
        const response = await fetch('/status');

        const data = await response.json();

        document.getElementById('wqi').textContent =
            data.wqi < 0 ? '--' : Number(data.wqi).toFixed(2);

        document.getElementById('level').textContent =
            data.level;

        document.getElementById('pwm').textContent =
            data.pwm;

        document.getElementById('percent').textContent =
            data.percent + '%';

        document.getElementById('motor').textContent =
            data.motor;

        document.getElementById('pwmFrequency').textContent =
            data.pwmFrequency + ' Hz';

        document.getElementById('flowHz').textContent =
            Number(data.flowFrequency).toFixed(2);

        document.getElementById('flowRate').textContent =
            Number(data.flowRate).toFixed(2);

        document.getElementById('connection').textContent =
            'Connected - Live monitoring';
    }

    catch(error)
    {
        document.getElementById('connection').textContent =
            'Connection lost';
    }
}

async function send(url)
{
    try
    {
        await fetch(url);
        updateStatus();
    }

    catch(error)
    {
        alert('Connection error');
    }
}

setInterval(updateStatus, 1000);

updateStatus();

</script>
</body>
</html>
)HTML";

// ========================================
// TEMPERATURE SENSOR
// ========================================

OneWire oneWire(TEMP_PIN);
DallasTemperature tempSensor(&oneWire);

// ========================================
// TIMING
// ========================================

const unsigned long SEND_INTERVAL_MS = 3000;
unsigned long lastSend = 0;

// ========================================
// FLOW SENSOR: YF-S201
// ========================================

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

    if (now - lastFlowMeasure < 1000)
        return;

    unsigned long elapsed = now - lastFlowMeasure;

    lastFlowMeasure = now;
    noInterrupts();
    uint32_t pulses = flowPulseCount;
    flowPulseCount = 0;
    interrupts();
    totalFlowPulses += pulses;
    flowFrequencyHz =pulses * 1000.0f / elapsed;
    flowRateLMin =flowFrequencyHz / FLOW_CALIBRATION;
    totalVolumeLiters =totalFlowPulses / FLOW_PULSES_PER_LITER;
}

// ========================================
// SENSOR CALIBRATION
// ========================================

const int NUM_SAMPLES = 30;
const int RELAY_ON = LOW;
const int RELAY_OFF = HIGH;

const float TDS_FACTOR_1 = 1.00f;
const float TDS_FACTOR_2 = 1.00f;

const float TURB_DIVIDER_RATIO = 1.5f;
const float TURB_CLEAR_VOLTAGE = 1.590f;
const float TURB_MILK_VOLTAGE = 0.0555f;
const float TURB_MILK_REFERENCE_NTU = 1000.0f;

const float WQI_THRESHOLD = 69.0f;

const float PH_MIN = 5.5f;
const float PH_MAX = 9.5f;

const float PRESSURE_DIVIDER_RATIO = 1.5f;
const float PRESSURE_VOLTAGE_MIN = 0.5f;
const float PRESSURE_VOLTAGE_MAX = 4.5f;

const float PRESSURE_RANGE_KPA = 800.0f;

// ========================================
// SENSOR READ FUNCTIONS
// ========================================

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

    float v = constrain(
        r.sensorV,
        PRESSURE_VOLTAGE_MIN,
        PRESSURE_VOLTAGE_MAX
    );

    r.kPa =
        (v - PRESSURE_VOLTAGE_MIN) /
        (PRESSURE_VOLTAGE_MAX - PRESSURE_VOLTAGE_MIN) *
        PRESSURE_RANGE_KPA;

    r.MPa = r.kPa / 1000.0f;
    r.bar = r.kPa / 100.0f;
    r.psi = r.kPa * 0.1450377f;

    return r;
}

String jsonNum(float v, int decimals)
{
    return isfinite(v)
        ? String(v, decimals)
        : String("null");
}

float calculateTDS(
    float voltage,
    float temperature,
    float calibrationFactor)
{
    float coefficient =
        1.0f + 0.02f * (temperature - 25.0f);

    if (coefficient <= 0.0f)
        coefficient = 1.0f;

    float v = voltage / coefficient;

    float tds =
        (133.42f * v * v * v
        - 255.86f * v * v
        + 857.39f * v) * 0.5f;

    tds *= calibrationFactor;

    if (tds < 0.0f)
        tds = 0.0f;

    return tds;
}

float readTurbidityNTU(int pin)
{
    float adcVoltage = readVoltage(pin, 20, 5);
    float sensorVoltage =adcVoltage * TURB_DIVIDER_RATIO;
    float denominator =TURB_CLEAR_VOLTAGE - TURB_MILK_VOLTAGE;

    if (denominator <= 0.0f)
        return NAN;

    float ntu =
        (TURB_CLEAR_VOLTAGE - sensorVoltage) /denominator *TURB_MILK_REFERENCE_NTU;

    if (ntu < 0.0f)
        ntu = 0.0f;
    return ntu;
}

float readPH()
{
    float adcVoltage = readVoltage(PH_PIN, 10, 2);
    float ph =8.0f + ((2.5f - adcVoltage) / 0.18f);
    return ph;
}

// ========================================
// WQI SCORING
// ========================================

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
    if (b.vmax == b.vmin)
        return b.qmin;

    float q =
        (b.vmin - v) /
        (b.vmax - b.vmin) *
        (b.qmax - b.qmin) +
        b.qmax;

    return constrain(q, b.qmin, b.qmax);
}

float qIncreasing(float v, const Band &b)
{
    if (b.vmax == b.vmin)
        return b.qmin;

    float q =
        (v - b.vmin) /
        (b.vmax - b.vmin) *
        (b.qmax - b.qmin) +
        b.qmin;

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
    if (!isfinite(tds) || tds < 0)
        return 0.0f;
    return qFromBands(tds, TDS_BANDS, 5);
}

float qTurbidity(float ntu)
{
    if (!isfinite(ntu) || ntu < 0)
        return 0.0f;
    return qFromBands(ntu, TURB_BANDS, 5);
}

float qPH(float ph)
{
    if (!isfinite(ph) || ph < 0 || ph > 14)return 7.0f;
    if (ph >= 7)return qFromBands(ph, PH_HIGH_BANDS, 4);
    for (int i = 0; i < 4; i++)
    {
        if (ph >= PH_LOW_BANDS[i].vmin)return qIncreasing(ph, PH_LOW_BANDS[i]);
    }

    return 0.0f;
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

// ========================================
// RELAY CONTROL
// ========================================

void setValve(int pin, bool open)
{
    digitalWrite(pin, open ? RELAY_ON : RELAY_OFF);
}

void closeAllValves()
{
    setValve(RELAY_VALVE1, false);
    setValve(RELAY_VALVE2, false);
    setValve(RELAY_VALVE3, false);
}

void controlValves(
    float pHwater,
    float wqiBefore,
    float wqiAfter)
{
    bool phUnsafe =
        !isfinite(pHwater) ||
        pHwater < PH_MIN ||
        pHwater > PH_MAX;

    if (phUnsafe)
    {
        closeAllValves();

        stopMotor();

        pumpLevel = "UNSAFE PH";
        Serial.println("WARNING: UNSAFE pH!");
        Serial.printf("pH = %.2f\n", pHwater);
        Serial.println(
            "All valves CLOSED. Pump STOPPED."
        );

        return;
    }

    if (!isfinite(wqiBefore))
    {
        setValve(RELAY_VALVE1, false);
        setValve(RELAY_VALVE2, false);
    }
    else if (wqiBefore > WQI_THRESHOLD)
    {
        setValve(RELAY_VALVE1, false);
        setValve(RELAY_VALVE2, true);
    }
    else
    {
        setValve(RELAY_VALVE1, true);
        setValve(RELAY_VALVE2, false);
    }

    if (!isfinite(wqiAfter) ||
        wqiAfter <= WQI_THRESHOLD)
    {
        setValve(RELAY_VALVE3, false);
    }
    else
    {
        setValve(RELAY_VALVE3, true);

        Serial.printf(
            "pH: %.2f | Unsafe: %s\n",
            pHwater,
            phUnsafe ? "YES" : "NO"
        );
    }
}

// ========================================
// WIFI CONNECTION
// ========================================

void connectWiFi()
{
    if (WiFi.status() == WL_CONNECTED)
        return;

    unsigned long now = millis();

    if (lastWiFiAttempt != 0 &&
        now - lastWiFiAttempt < WIFI_RETRY_INTERVAL_MS)
        return;

    lastWiFiAttempt = now;

    // Keep the ESP32 dashboard hotspot running
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    Serial.print("Connecting to host WiFi: ");
    Serial.println(WIFI_SSID);
}

// ========================================
// WEB SERVER ROUTES
// ========================================

void setupWebServer()
{
    server.on("/", HTTP_GET, []()
    {
        server.send(200, "text/html", page);
    });

    // Live dashboard information

    server.on("/status", HTTP_GET, []()
    {
        String json = "{";
        json += "\"wqi\":" + String(currentWQI, 2) + ",";
        json += "\"level\":\"" + pumpLevel + "\",";
        json += "\"pwm\":" + String(motorSpeed) + ",";
        json += "\"percent\":" +String((motorSpeed * 100) / 255) + ",";
        json += "\"pwmFrequency\":" +String(MOTOR_PWM_FREQUENCY) + ",";
        json += "\"flowFrequency\":" +String(flowFrequencyHz, 2) + ",";
        json += "\"flowRate\":" +String(flowRateLMin, 2) + ",";
        json += "\"motor\":\"" +String(motorRunning ? "RUNNING" : "STOPPED") + "\",";
        json += "\"mode\":\"" +String(emergencyStop ? "EMERGENCY STOP" : "AUTOMATIC") + "\"";
        json += "}";
        server.send(200, "application/json", json);
    });

    // Emergency stop

    server.on("/stop", HTTP_GET, []()
    {
        emergencyStop = true;

        stopMotor();

        pumpLevel = "EMERGENCY STOP";

        server.send(
            200,
            "text/plain",
            "Motor stopped. Press resume to restart."
        );
    });

    // Resume automatic mode

    server.on("/auto", HTTP_GET, []()
    {
        emergencyStop = false;
        applyAutomaticMotorControl();

        server.send(
            200,
            "text/plain",
            "Automatic mode requested. Check WQI and sensor status."
        );
    });
    server.begin();
    Serial.println("Motor dashboard web server started.");
}

// ========================================
// SETUP
// ========================================

void setup()
{
    Serial.begin(115200);

    delay(1000);

    // ----- MOTOR DRIVER -----

    pinMode(STBY, OUTPUT);
    pinMode(AIN1, OUTPUT);
    pinMode(AIN2, OUTPUT);

    digitalWrite(STBY, LOW);

    if (!ledcAttach(
        PWMA,
        MOTOR_PWM_FREQUENCY,
        MOTOR_PWM_RESOLUTION))
    {
        Serial.println("PWM initialization failed!");

        while (true)
            delay(1000);
    }
    ledcWrite(PWMA, 0);

    digitalWrite(AIN1, HIGH);
    digitalWrite(AIN2, LOW);
    digitalWrite(STBY, HIGH);

    // ----- RELAYS -----

    pinMode(RELAY_VALVE1, OUTPUT);
    pinMode(RELAY_VALVE2, OUTPUT);
    pinMode(RELAY_VALVE3, OUTPUT);

    closeAllValves();

    // ----- FLOW SENSOR -----

    pinMode(FLOW_PIN, INPUT);
    attachInterrupt(digitalPinToInterrupt(FLOW_PIN),flowPulseISR,FALLING);

    lastFlowMeasure = millis();

    // ----- ADC AND SENSORS -----

    analogReadResolution(12);
    analogSetPinAttenuation(PH_PIN, ADC_11db);
    analogSetPinAttenuation(TDS1_PIN, ADC_11db);
    analogSetPinAttenuation(TDS2_PIN, ADC_11db);
    analogSetPinAttenuation(TURBIDITY1_PIN, ADC_11db);
    analogSetPinAttenuation(TURBIDITY2_PIN, ADC_11db);
    analogSetPinAttenuation(PRESSURE1_PIN, ADC_11db);
    analogSetPinAttenuation(PRESSURE2_PIN, ADC_11db);
    pinMode(PRESSURE1_PIN, INPUT);
    pinMode(PRESSURE2_PIN, INPUT);

    tempSensor.begin();

    // ----- WIFI: AP + STA -----

    WiFi.mode(WIFI_AP_STA);
    WiFi.setAutoReconnect(true);

    if (WiFi.softAP(ssid, password))
    {
        Serial.println("Motor dashboard hotspot started.");
        Serial.print("Dashboard URL: http://");
        Serial.println(WiFi.softAPIP());
    }
    else
    {
        Serial.println("ERROR: Could not start Motor-ESP32 hotspot.");
    }
    connectWiFi();
    setupWebServer();

    Serial.println(
        "ESP32-S3 Water Quality + Automatic Motor Initialized."
    );

    Serial.println(
        "Pump waits for a valid WQI reading before starting."
    );
}
//Main Loop
void loop()
{
    server.handleClient();

    checkMotorWQITimeout();

    updateFlowSensor();

    // Check host WiFi connection

    if (WiFi.status() == WL_CONNECTED)
    {
        if (!hostWiFiWasConnected)
        {
            Serial.print("Host WiFi connected. STA IP: ");
            Serial.println(WiFi.localIP());
            hostWiFiWasConnected = true;
        }
    }
    else
    {
        hostWiFiWasConnected = false;

        connectWiFi();
    }

    unsigned long now = millis();
    if (now - lastSend < SEND_INTERVAL_MS)
        return;
    lastSend = now;

    // ----- TEMPERATURE -----

    tempSensor.requestTemperatures();

    tempSensor.requestTemperatures();
  float temperature = tempSensor.getTempCByIndex(0);
  if (temperature == DEVICE_DISCONNECTED_C || !isfinite(temperature))
  {
    temperature = 25.0f;
  }

    // ----- pH -----
    float pHwater = readPH();

    bool pHReadingSafe =
        isfinite(pHwater) &&
        pHwater >= PH_MIN &&
        pHwater <= PH_MAX;

    if (!isfinite(pHwater) ||
        pHwater < 0.0f ||
        pHwater > 14.0f)
    {
        Serial.println(
            "WARNING: pH sensor reading is invalid."
        );
    }

    // ----- TDS -----

    float voltageTDS1 = readVoltage(TDS1_PIN, NUM_SAMPLES, 2);
    float tds1 = calculateTDS(voltageTDS1, temperature, TDS_FACTOR_1);

    float voltageTDS2 = readVoltage(TDS2_PIN, NUM_SAMPLES, 2);
    float tds2 = calculateTDS(voltageTDS2, temperature, TDS_FACTOR_2);

    // ----- TURBIDITY -----

    float ntu1 = readTurbidityNTU(TURBIDITY1_PIN);
    float ntu2 = readTurbidityNTU(TURBIDITY2_PIN);

    // ----- WQI -----

   float wqi1 = calculateWQI(pHwater, tds1, ntu1);
   float wqi2 = calculateWQI(pHwater, tds2, ntu2);
   float averageTDS = (tds1 + tds2) / 2.0f;
   float averageNTU = (ntu1 + ntu2) / 2.0f;
   float overallWQI = calculateWQI(pHwater, averageTDS, averageNTU);


    // ----- AUTOMATIC PUMP CONTROL -----

    bool wqiSensorReadingsValid =
        isfinite(tds1) && tds1 >= 0.0f &&
        isfinite(tds2) && tds2 >= 0.0f &&
        isfinite(ntu1) && ntu1 >= 0.0f &&
        isfinite(ntu2) && ntu2 >= 0.0f;

    acceptNewWQI(
        overallWQI,
        pHReadingSafe,
        wqiSensorReadingsValid
    );

    // ----- VALVES -----

    controlValves(
        pHwater,
        wqi1,
        wqi2
    );

    // ----- PRESSURE -----

    PressureReading p1 = readPressure(PRESSURE1_PIN);
    PressureReading p2 = readPressure(PRESSURE2_PIN);

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
  json += "\"valve3Open\":" + String(digitalRead(RELAY_VALVE3) == RELAY_ON ? "true" : "false");
  json += "}";

    // ----- SERIAL DEBUG -----

    Serial.println("\n========== WATER QUALITY ==========");

    Serial.printf(
        "Temperature : %.2f °C\n",
        temperature
    );

    Serial.printf(
        "pH          : %.2f\n",
        pHwater
    );

    Serial.printf(
        "TDS Before  : %.2f ppm\n",
        tds1
    );

    Serial.printf(
        "TDS After   : %.2f ppm\n",
        tds2
    );

    Serial.printf(
        "Turbidity 1 : %.2f NTU\n",
        ntu1
    );

    Serial.printf(
        "Turbidity 2 : %.2f NTU\n",
        ntu2
    );

    Serial.printf(
        "WQI Before  : %.1f %s\n",
        wqi1,
        wqiLevel(wqi1)
    );

    Serial.printf(
        "WQI After   : %.1f %s\n",
        wqi2,
        wqiLevel(wqi2)
    );

    Serial.printf(
        "Overall WQI : %.1f %s\n",
        overallWQI,
        wqiLevel(overallWQI)
    );

    Serial.printf(
        "Pump control: %s | PWM: %d/255 | Running: %s\n",
        pumpLevel.c_str(),
        motorSpeed,
        motorRunning ? "YES" : "NO"
    );

    // ----- HTTP POST -----

    WiFiClient client;

    HTTPClient http;

    http.setTimeout(15000);

    Serial.println("Uploading sensor data...");

    Serial.println("URL: " + String(SERVER_URL));

    Serial.println("JSON: " + json);

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println(
            "Host WiFi disconnected. Upload skipped; dashboard remains available."
        );
    }
    else if (http.begin(client, SERVER_URL))
    {
        http.addHeader(
            "Content-Type",
            "application/json"
        );

        http.addHeader(
            "x-device-api-key",
            DEVICE_API_KEY
        );

        int httpCode = http.POST(json);

        Serial.printf(
            "POST status: %d\n",
            httpCode
        );

        if (httpCode > 0)
        {
            Serial.println("Server reply:");

            Serial.println(http.getString());

            if (httpCode >= 200 && httpCode < 300)
            {
                Serial.println("Upload successful!");
            }
            else
            {
                Serial.println(
                    "Server rejected the request."
                );

                Serial.println(
                    "Check API route, authentication and JSON."
                );
            }
        }
        else
        {
            Serial.printf(
                "HTTP error: %s\n",
                http.errorToString(httpCode).c_str()
            );
        }

        http.end();
    }
    else
    {
        Serial.println(
            "Could not initialize HTTP connection."
        );
    }
}
