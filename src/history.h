#pragma once
// ============================================================
//  history.h
//  สรุปค่าที่วัดได้เป็น "รายชั่วโมง" และ "รายวัน" แล้วเก็บลง Firebase ที่
//    /history/hourly/{YYYY-MM}/{DD}/{HH}
//    /history/daily/{YYYY-MM}/{DD}
//  (โครงสร้างตาม docs/firebase-storage-design.md ข้อ 4)
//
//  ใช้งาน:  historyRecord(sample);   // เรียกจาก loop() ทุกครั้งที่วัดค่า (ไม่รอเน็ต)
//  upload task ใน firebase_upload.h จะดึงงานไปทำเอง:
//    - GET   กู้ค่าที่เก็บไว้ของชั่วโมง/วันปัจจุบันหลังรีบูต (ข้อมูลไม่หาย)
//    - PUT   ส่งค่าสะสมทุก 5 นาที และตอนขึ้นชั่วโมง/วันใหม่
//    - DELETE ลบข้อมูลที่เก่ากว่า 12 เดือน
//  ต้องได้เวลาจาก NTP ก่อน (configTime ใน setup) ถึงจะเริ่มเก็บ
// ============================================================
#include <Arduino.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>

enum { H_WQI, H_WQI1, H_WQI2, H_PH, H_TEMP, H_TDS1, H_TDS2, H_TURB1, H_TURB2, H_METRICS };
static const char* const HISTORY_METRIC_NAMES[H_METRICS] = {
  "wqi", "wqi1", "wqi2", "ph", "temperature", "tds1", "tds2", "turbidity1", "turbidity2"
};

static const float         HISTORY_DIRTY_WQI       = 50.0f;               // ตรงกับ DIRTY_WQI ในหน้า archiveddata.html
static const unsigned long HISTORY_PUT_INTERVAL_MS = 5UL * 60UL * 1000UL; // ส่งค่าสะสมทุก 5 นาที
static const unsigned long HISTORY_RETRY_MS        = 10000;               // ส่งไม่สำเร็จ -> รอ 10 วิ แล้วลองใหม่
static const int           HISTORY_OUTBOX_SIZE     = 12;

struct HistorySample
{
  float v[H_METRICS];       // ค่าตามลำดับ HISTORY_METRIC_NAMES (NaN = อ่านไม่ได้ ไม่นับ)
  bool throughFilter;       // น้ำกำลังผ่านไส้กรอง (valve2 เปิด)
  float totalVolumeLiters;  // ปริมาตรสะสมจาก flow sensor
};

struct HistoryStat { double sum; uint32_t n; float min; float max; };

struct HistoryAcc
{
  String key;               // "YYYY-MM/DD/HH" (ชั่วโมง) หรือ "YYYY-MM/DD" (วัน)
  bool resumed;             // รวมค่าที่เก็บไว้ใน Firebase เข้ามาแล้ว (หรือไม่จำเป็น)
  bool resumeInFlight;
  bool changed;             // มีค่าใหม่ที่ยังไม่ได้ส่ง
  double sec, dirtySec, filterSec, flowLiters;
  HistoryStat m[H_METRICS];
};

struct HistoryOut { String path; String body; };   // body ว่าง = DELETE

enum { JOB_OUTBOX, JOB_RESUME_HOUR, JOB_RESUME_DAY };
struct HistoryJob { const char* method; String path; String body; int kind; String key; };

static SemaphoreHandle_t historyMutex = nullptr;
static HistoryAcc hourAcc, dayAcc;
static HistoryOut historyOutbox[HISTORY_OUTBOX_SIZE];
static int historyOutCount = 0;
static unsigned long historyLastPut = 0;
static unsigned long historyRetryAt = 0;
static unsigned long historyLastSampleMs = 0;
static float historyLastVolume = NAN;
static String historyRetentionDay;

static void historyBegin()
{
  historyMutex = xSemaphoreCreateMutex();
}

static void historyResetAcc(HistoryAcc& a, const String& key, bool resumed)
{
  a.key = key;
  a.resumed = resumed;
  a.resumeInFlight = false;
  a.changed = false;
  a.sec = a.dirtySec = a.filterSec = a.flowLiters = 0;
  for (int i = 0; i < H_METRICS; i++) a.m[i] = { 0, 0, INFINITY, -INFINITY };
}

static String historyPath(const char* resolution, const String& key)
{
  return String("/history/") + resolution + "/" + key;
}

static String historyToJson(const HistoryAcc& a)
{
  JsonDocument doc;
  doc["sec"] = (uint32_t)lround(a.sec);
  doc["dirtySec"] = (uint32_t)lround(a.dirtySec);
  doc["filterSec"] = (uint32_t)lround(a.filterSec);
  doc["flowLiters"] = lround(a.flowLiters * 1000.0) / 1000.0;
  JsonObject m = doc["m"].to<JsonObject>();
  for (int i = 0; i < H_METRICS; i++)
  {
    if (!a.m[i].n) continue;
    JsonObject s = m[HISTORY_METRIC_NAMES[i]].to<JsonObject>();
    s["sum"] = a.m[i].sum;
    s["n"] = a.m[i].n;
    s["min"] = a.m[i].min;
    s["max"] = a.m[i].max;
  }
  doc["updatedAt"][".sv"] = "timestamp";
  String out;
  serializeJson(doc, out);
  return out;
}

// รวมบันทึกที่อ่านจาก Firebase (หลังรีบูต) เข้ากับค่าที่สะสมอยู่
static void historyMergeStored(HistoryAcc& a, const String& body)
{
  if (body.length() == 0 || body == "null") return;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return;
  a.sec += doc["sec"] | 0.0;
  a.dirtySec += doc["dirtySec"] | 0.0;
  a.filterSec += doc["filterSec"] | 0.0;
  a.flowLiters += doc["flowLiters"] | 0.0;
  JsonObject m = doc["m"];
  for (int i = 0; i < H_METRICS; i++)
  {
    JsonObject s = m[HISTORY_METRIC_NAMES[i]];
    uint32_t n = s["n"] | 0;
    if (s.isNull() || n == 0) continue;
    HistoryStat& st = a.m[i];
    st.sum += s["sum"] | 0.0;
    st.n += n;
    st.min = fminf(st.min, s["min"] | (float)INFINITY);
    st.max = fmaxf(st.max, s["max"] | (float)-INFINITY);
  }
}

// ใส่งานลงคิว (ต้องถือ historyMutex อยู่) ; path เดิมที่ยังไม่ได้ส่งจะถูกแทนด้วยค่าใหม่
static void historyQueue(const String& path, const String& body)
{
  for (int i = 0; i < historyOutCount; i++)
  {
    if (historyOutbox[i].path == path) { historyOutbox[i].body = body; return; }
  }
  if (historyOutCount == HISTORY_OUTBOX_SIZE)
  {
    for (int i = 1; i < historyOutCount; i++) historyOutbox[i - 1] = historyOutbox[i];
    historyOutCount--;
    Serial.println("[history] outbox full, dropped oldest");
  }
  historyOutbox[historyOutCount].path = path;
  historyOutbox[historyOutCount].body = body;
  historyOutCount++;
}

// เก็บย้อนหลัง 12 เดือน: ข้อมูลเดือน M ถูกลบในเดือน M ของปีถัดไป
static void historyQueueRetention(const struct tm& t)
{
  char month[8];
  snprintf(month, sizeof month, "%04d-%02d", t.tm_year + 1900 - 1, t.tm_mon + 1);
  historyQueue(historyPath("hourly", month), "");
  historyQueue(historyPath("daily", month), "");
}

static void historyAdd(HistoryAcc& a, const HistorySample& s, double dt, float dVolume)
{
  a.sec += dt;
  if (isfinite(s.v[H_WQI2]) && s.v[H_WQI2] < HISTORY_DIRTY_WQI) a.dirtySec += dt;
  if (s.throughFilter) a.filterSec += dt;
  a.flowLiters += dVolume;
  for (int i = 0; i < H_METRICS; i++)
  {
    float v = s.v[i];
    if (!isfinite(v)) continue;
    HistoryStat& st = a.m[i];
    st.sum += v;
    st.n++;
    st.min = fminf(st.min, v);
    st.max = fmaxf(st.max, v);
  }
  a.changed = true;
}

static void historyRecord(const HistorySample& s)
{
  if (!historyMutex) return;
  struct tm t;
  if (!getLocalTime(&t, 0) || t.tm_year + 1900 < 2024) return;   // ยังไม่ได้เวลา NTP

  char day[12], hour[16];
  snprintf(day, sizeof day, "%04d-%02d/%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  snprintf(hour, sizeof hour, "%s/%02d", day, t.tm_hour);

  unsigned long now = millis();
  double dt = historyLastSampleMs ? (now - historyLastSampleMs) / 1000.0 : 1.0;
  if (dt > 5.0) dt = 5.0;   // ไม่นับช่วงที่หยุดวัดนาน ๆ
  historyLastSampleMs = now;

  float dVolume = (isfinite(historyLastVolume) && s.totalVolumeLiters >= historyLastVolume)
                  ? s.totalVolumeLiters - historyLastVolume : 0.0f;
  historyLastVolume = s.totalVolumeLiters;

  xSemaphoreTake(historyMutex, portMAX_DELAY);

  // ขึ้นชั่วโมง/วันใหม่: ส่งค่าสุดท้ายของช่วงเก่า แล้วเริ่มนับใหม่
  // (ช่วงแรกหลังบูตต้องกู้ค่าที่เก็บไว้ก่อน -> resumed = false)
  if (hourAcc.key != hour)
  {
    bool rollover = hourAcc.key.length() > 0;
    if (rollover) historyQueue(historyPath("hourly", hourAcc.key), historyToJson(hourAcc));
    historyResetAcc(hourAcc, hour, rollover);
  }
  if (dayAcc.key != day)
  {
    bool rollover = dayAcc.key.length() > 0;
    if (rollover) historyQueue(historyPath("daily", dayAcc.key), historyToJson(dayAcc));
    historyResetAcc(dayAcc, day, rollover);
  }
  if (historyRetentionDay != day)
  {
    historyRetentionDay = day;
    historyQueueRetention(t);
  }

  historyAdd(hourAcc, s, dt, dVolume);
  historyAdd(dayAcc, s, dt, dVolume);

  if (now - historyLastPut >= HISTORY_PUT_INTERVAL_MS)
  {
    historyLastPut = now;
    if (hourAcc.resumed && hourAcc.changed)
    {
      historyQueue(historyPath("hourly", hourAcc.key), historyToJson(hourAcc));
      hourAcc.changed = false;
    }
    if (dayAcc.resumed && dayAcc.changed)
    {
      historyQueue(historyPath("daily", dayAcc.key), historyToJson(dayAcc));
      dayAcc.changed = false;
    }
  }

  xSemaphoreGive(historyMutex);
}

// ---------- เรียกจาก upload task ----------
static bool historyNextJob(HistoryJob& job)
{
  if (!historyMutex) return false;
  if (historyRetryAt && (long)(millis() - historyRetryAt) < 0) return false;

  bool found = false;
  xSemaphoreTake(historyMutex, portMAX_DELAY);

  HistoryAcc* accs[2] = { &hourAcc, &dayAcc };
  const char* resolution[2] = { "hourly", "daily" };
  for (int k = 0; k < 2 && !found; k++)
  {
    HistoryAcc* a = accs[k];
    if (a->key.length() && !a->resumed && !a->resumeInFlight)
    {
      a->resumeInFlight = true;
      job.method = "GET";
      job.path = historyPath(resolution[k], a->key);
      job.body = "";
      job.kind = (k == 0) ? JOB_RESUME_HOUR : JOB_RESUME_DAY;
      job.key = a->key;
      found = true;
    }
  }
  if (!found && historyOutCount > 0)
  {
    job.method = historyOutbox[0].body.length() ? "PUT" : "DELETE";
    job.path = historyOutbox[0].path;
    job.body = historyOutbox[0].body;
    job.kind = JOB_OUTBOX;
    job.key = "";
    found = true;
  }

  xSemaphoreGive(historyMutex);
  return found;
}

static void historyJobDone(const HistoryJob& job, int code, const String& response)
{
  xSemaphoreTake(historyMutex, portMAX_DELAY);

  if (job.kind == JOB_OUTBOX)
  {
    // เอาออกจากคิวเฉพาะเมื่อยังเป็นค่าเดิม (ถ้าระหว่างส่งมีค่าใหม่มาแทน ให้ส่งค่าใหม่รอบหน้า)
    if (code == 200 && historyOutCount > 0 &&
        historyOutbox[0].path == job.path && historyOutbox[0].body == job.body)
    {
      for (int i = 1; i < historyOutCount; i++) historyOutbox[i - 1] = historyOutbox[i];
      historyOutCount--;
    }
  }
  else
  {
    HistoryAcc& a = (job.kind == JOB_RESUME_HOUR) ? hourAcc : dayAcc;
    if (a.key == job.key)
    {
      a.resumeInFlight = false;
      if (code == 200)
      {
        historyMergeStored(a, response);
        a.resumed = true;
        a.changed = true;
      }
    }
  }
  historyRetryAt = (code == 200) ? 0 : millis() + HISTORY_RETRY_MS;

  xSemaphoreGive(historyMutex);
}
