#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// =================================================
// PIN
// =================================================

#define TDS_PIN 1
#define TEMP_PIN 4
#define PH_PIN 5

// NOTE: TDS already uses GPIO1, so turbidity must be on a different ADC pin.
// Change this to match your wiring.
#define TURBIDITY_PIN 2

// =================================================
// DS18B20
// =================================================

OneWire oneWire(TEMP_PIN);
DallasTemperature tempSensor(&oneWire);

// =================================================
// TDS
// =================================================

const int NUM_SAMPLES = 30;

// TDS calibration factor
float TDS_FACTOR = 1.00;

float pHwater = 7.0;
// =================================================
// READ TDS VOLTAGE
// =================================================

float readTDSVoltage()
{
    float sum = 0;
    for (int i = 0; i < NUM_SAMPLES; i++)
    {
        // Read ADC in millivolts
        uint32_t mv = analogReadMilliVolts(TDS_PIN);
        sum += mv;
        delay(5);
    }
    float average_mV = sum / NUM_SAMPLES;
    return average_mV / 1000.0;
}

// =================================================
// CALCULATE TDS
// =================================================

float calculateTDS(float voltage, float temperature)
{
    // Temperature compensation
    float compensationCoefficient =
        1.0 + 0.02 * (temperature - 25.0);

    float compensatedVoltage =
        voltage / compensationCoefficient;

    // Polynomial approximation
    float tds =
        (133.42 * compensatedVoltage * compensatedVoltage * compensatedVoltage
        - 255.86 * compensatedVoltage * compensatedVoltage
        + 857.39 * compensatedVoltage)
        * 0.5;

    // Calibration factor
    tds = tds * TDS_FACTOR;

    return tds;
}

// =================================================
// TURBIDITY (experimental NTU estimate)
// =================================================

// Voltage divider: 10k + 20k, ratio = 1.5
const float TURB_DIVIDER_RATIO = 1.5f;

// Number of ADC samples
const int TURB_NUM_SAMPLES = 50;

// Calibration values from experiment
const float TURB_CLEAR_VOLTAGE = 1.590f;
const float TURB_MILK_VOLTAGE = 0.0555f;
const float TURB_MILK_REFERENCE_NTU = 1000.0f;

float readTurbidityNTU()
{
    uint32_t totalMilliVolts = 0;

    for (int i = 0; i < TURB_NUM_SAMPLES; i++)
    {
        totalMilliVolts += analogReadMilliVolts(TURBIDITY_PIN);
        delay(10);
    }

    // Average voltage measured at the ESP32-S3 ADC
    float adcVoltage =
        (totalMilliVolts / (float)TURB_NUM_SAMPLES) / 1000.0f;

    // Estimate the original sensor output voltage
    float sensorVoltage = adcVoltage * TURB_DIVIDER_RATIO;

    // Experimental linear interpolation
    float ntu =
        (TURB_CLEAR_VOLTAGE - sensorVoltage) /
        (TURB_CLEAR_VOLTAGE - TURB_MILK_VOLTAGE) *
        TURB_MILK_REFERENCE_NTU;

    // Clamp negative estimates to zero
    if (ntu < 0.0f)
    {
        ntu = 0.0f;
    }

    return ntu;
}

// =================================================
// WQI (Water Quality Index)
// =================================================

// WQI weights (temperature is display only, not part of WQI)
const float W_PH   = 0.34;
const float W_TDS  = 0.33;
const float W_TURB = 0.33;

struct Band
{
    float vmin;   // sensor value: lower bound of the band
    float vmax;   // sensor value: upper bound of the band
    float qmin;   // WQI: lower bound of the band
    float qmax;   // WQI: upper bound of the band
};

// Bands ordered from cleanest to dirtiest
const Band TDS_BANDS[] = {
    {0,   100, 85, 100},   // Very Clean
    {101, 150, 70, 84},    // Clean
    {151, 300, 50, 69},    // Normal
    {301, 500, 25, 49},    // Dirty
    {500, 750, 0,  24}     // Very Dirty
};

const Band TURB_BANDS[] = {
    {0,    0.5,  85, 100},  // Very Clean
    {0.5,  0.99, 70, 84},   // Clean
    {1,    10,   50, 69},   // Normal
    {11,   20,   25, 49},   // Dirty
    {20,   100,  0,  24}    // Very Dirty
};

// pH > 7 side (cleanest first)
const Band PH_HIGH_BANDS[] = {
    {7,    8.5,  70, 100},
    {8.5,  9.5,  50, 69},
    {9.5,  10.5, 25, 49},
    {10.5, 14,   0,  24}
};

// pH < 7 side (cleanest first)
const Band PH_LOW_BANDS[] = {
    {6.5, 7,   70, 100},
    {5.5, 6.5, 50, 69},
    {4.0, 5.5, 25, 49},
    {0,   4.0, 0,  24}
};

// Formula for Temp, pH(>7), TDS, Turbidity
float qDecreasing(float v, const Band &b)
{
    float q = (b.vmin - v) / (b.vmax - b.vmin) * (b.qmax - b.qmin) + b.qmax;
    if (q > b.qmax) q = b.qmax;
    if (q < b.qmin) q = b.qmin;
    return q;
}

// Formula for pH(<7)
float qIncreasing(float v, const Band &b)
{
    float q = (v - b.vmin) / (b.vmax - b.vmin) * (b.qmax - b.qmin) + b.qmin;
    if (q > b.qmax) q = b.qmax;
    if (q < b.qmin) q = b.qmin;
    return q;
}

// Pick the first band (cleanest first) whose vmax is not exceeded
float qFromBands(float v, const Band *bands, int n)
{
    for (int i = 0; i < n; i++)
    {
        if (v <= bands[i].vmax) return qDecreasing(v, bands[i]);
    }
    return 0;   // above the highest band
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
    if (ph >= 7) return qFromBands(ph, PH_HIGH_BANDS, 4);
    for (int i = 0; i < 4; i++)
    {
        if (ph >= PH_LOW_BANDS[i].vmin) return qIncreasing(ph, PH_LOW_BANDS[i]);
    }
    return 0;
}

float calculateWQI(float ph, float tds, float ntu)
{
    return W_PH   * qPH(ph)
         + W_TDS  * qTDS(tds)
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
// SETUP
// =================================================

void setup()
{
    Serial.begin(115200);

    delay(1000);

    Serial.println();
    Serial.println("==============================");
    Serial.println(" ESP32-S3 + AB202 + DS18B20 ");
    Serial.println("==============================");

    // =================================================
    // ADC
    // =================================================

    analogReadResolution(12);

    // ESP32-S3 ADC attenuation
    analogSetPinAttenuation(TDS_PIN, ADC_11db);
    analogSetPinAttenuation(PH_PIN, ADC_11db);
    analogSetPinAttenuation(TURBIDITY_PIN, ADC_11db);

    // =================================================
    // DS18B20
    // =================================================

    tempSensor.begin();

    Serial.println("System Ready");
    Serial.println();
}

// =================================================
// LOOP
// =================================================

void loop()
{
    // =================================================
    // Read pH
    // =================================================
   long sum = 0;
   for (int i = 0; i < 10; i++) {
        sum += analogRead(PH_PIN);
        delay(5);
    }
    float adcValue = sum / 10.0;
    float pHwater = 8 + ((2.5 - (adcValue * (3.3 / 4095))) / 0.18);  
    // =================================================
    // READ TEMPERATURE
    // =================================================

    tempSensor.requestTemperatures();

    float temperature =
        tempSensor.getTempCByIndex(0);

    // Check DS18B20
    if (temperature == DEVICE_DISCONNECTED_C)
    {
        Serial.println("ERROR: DS18B20 disconnected!");
        delay(1000);
        return;
    }

    // =================================================
    // READ TDS VOLTAGE
    // =================================================

    float voltage = readTDSVoltage();

    // =================================================
    // CALCULATE TDS
    // =================================================

    float tds = calculateTDS(
        voltage,
        temperature
    );

    // =================================================
    // CALCULATE WQI
    // =================================================

    float ntu = readTurbidityNTU();

    float wqi = calculateWQI(pHwater, tds, ntu);
    wqi = roundf(wqi * 10.0) / 10.0;   // 1 decimal, so level matches displayed value

    // =================================================
    // DISPLAY
    // =================================================

    Serial.println("------------------------------");

    Serial.print("Temperature : ");
    Serial.print(temperature, 2);
    Serial.println(" °C");

    Serial.print("pH water: ");
    Serial.print(pHwater, 2);
    Serial.println();

    Serial.print("Voltage     : ");
    Serial.print(voltage, 4);
    Serial.println(" V");

    Serial.print("TDS         : ");
    Serial.print(tds, 1);
    Serial.println(" ppm");

    Serial.print("Turbidity   : ");
    Serial.print(ntu, 1);
    Serial.println(" NTU");

    Serial.print("WQI         : ");
    Serial.println(wqi, 1);

    Serial.print("Quality     : ");
    Serial.println(wqiLevel(wqi));

    Serial.println("------------------------------");

    delay(1000);
}