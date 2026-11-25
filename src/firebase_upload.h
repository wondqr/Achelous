#pragma once
// ============================================================
//  firebase_upload.h
//  ส่งข้อมูลเข้า Firebase Realtime Database (แผนฟรี Spark) ผ่าน HTTPS
//
//  ทำงานใน "task แยก" (core 0) เพื่อไม่ให้การรอเน็ตไปหน่วงการวัดค่า
//  และการควบคุมปั๊ม/วาล์วใน loop() หลัก
//
//  ใช้งาน:  startUploadTask();   // เรียกครั้งเดียวใน setup()
//           queueUpload(json);   // เรียกทุกครั้งที่มีข้อมูลใหม่ (ไม่รอ)
//  JSON ต้องมี "timestamp":{".sv":"timestamp"} เพื่อให้ Firebase ใส่เวลาให้
// ============================================================
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "secrets.h"

static SemaphoreHandle_t uploadMutex = nullptr;
static String pendingJson;
static bool hasPending = false;

static String idToken;
static unsigned long tokenObtainedMs = 0;
// โทเคนหมดอายุ 60 นาที -> ล็อกอินใหม่ทุก 50 นาที
static const unsigned long TOKEN_LIFETIME_MS = 50UL * 60UL * 1000UL;

// ดึงค่า string ของ key จาก JSON แบบง่าย ๆ (ไม่ต้องใช้ไลบรารีเพิ่ม)
static String extractJsonString(const String& body, const char* key)
{
  String pat = String("\"") + key + "\"";
  int k = body.indexOf(pat);
  if (k < 0) return "";
  int colon = body.indexOf(':', k + pat.length());
  if (colon < 0) return "";
  int q1 = body.indexOf('"', colon + 1);
  if (q1 < 0) return "";
  int q2 = body.indexOf('"', q1 + 1);
  if (q2 < 0) return "";
  return body.substring(q1 + 1, q2);
}

// ล็อกอินบัญชี ESP32 เพื่อขอ idToken
static bool firebaseSignIn(WiFiClientSecure& client)
{
  client.stop();   // ปิด connection เดิม (คนละโฮสต์)
  HTTPClient http;
  http.setConnectTimeout(6000);
  http.setTimeout(8000);

  String url = String("https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=")
               + FIREBASE_API_KEY;
  if (!http.begin(client, url)) return false;

  http.addHeader("Content-Type", "application/json");
  String body = String("{\"email\":\"") + FIREBASE_USER_EMAIL +
                "\",\"password\":\"" + FIREBASE_USER_PASSWORD +
                "\",\"returnSecureToken\":true}";

  int code = http.POST(body);
  String resp = (code > 0) ? http.getString() : String("");
  http.end();
  client.stop();

  if (code != 200)
  {
    Serial.printf("[firebase] sign-in failed, HTTP %d\n", code);
    if (resp.length()) Serial.println(resp);
    return false;
  }

  String t = extractJsonString(resp, "idToken");
  if (t.length() < 100)
  {
    Serial.println("[firebase] sign-in reply has no idToken");
    return false;
  }
  idToken = t;
  tokenObtainedMs = millis();
  Serial.println("[firebase] signed in");
  return true;
}

static void uploadTask(void*)
{
  WiFiClientSecure client;
  client.setInsecure();   // ไม่ตรวจใบรับรอง (ง่าย ใช้ได้กับงานนี้ แต่ปลอดภัยน้อยกว่าตรวจใบรับรอง)
  HTTPClient http;
  http.setReuse(true);    // ใช้ connection เดิมซ้ำ ไม่ต้องจับมือ TLS ใหม่ทุกครั้ง
  unsigned long lastErrLog = 0;

  for (;;)
  {
    vTaskDelay(pdMS_TO_TICKS(200));
    if (WiFi.status() != WL_CONNECTED) continue;

    String json;
    bool have = false;
    xSemaphoreTake(uploadMutex, portMAX_DELAY);
    if (hasPending) { json = pendingJson; hasPending = false; have = true; }
    xSemaphoreGive(uploadMutex);
    if (!have) continue;

    if (idToken.length() == 0 || millis() - tokenObtainedMs > TOKEN_LIFETIME_MS)
    {
      if (!firebaseSignIn(client))
      {
        vTaskDelay(pdMS_TO_TICKS(5000));   // รอแล้วลองใหม่รอบถัดไป (ข้อมูลรอบนี้ทิ้ง)
        continue;
      }
    }

    String url = String(FIREBASE_DB_URL) + "/latest.json?auth=" + idToken;
    http.setConnectTimeout(6000);
    http.setTimeout(8000);

    int code = -1;
    if (http.begin(client, url))
    {
      http.addHeader("Content-Type", "application/json");
      code = http.PUT(json);        // PUT = เขียนทับค่าเดิมที่ /latest
      http.end();
    }

    if (code != 200)
    {
      if (code == 401 || code == 403) idToken = "";   // โทเคนใช้ไม่ได้ -> ล็อกอินใหม่รอบหน้า
      if (code < 0) client.stop();                    // connection เสีย -> เริ่มใหม่
      if (millis() - lastErrLog > 10000)
      {
        lastErrLog = millis();
        Serial.printf("[firebase] upload failed, HTTP %d\n", code);
      }
    }
  }
}

static void startUploadTask()
{
  uploadMutex = xSemaphoreCreateMutex();
  // stack 16 KB เพราะ TLS ใช้เยอะ ; ปักไว้ที่ core 0 ให้ loop() อยู่ core 1 ตามเดิม
  xTaskCreatePinnedToCore(uploadTask, "fbUpload", 16384, nullptr, 1, nullptr, 0);
}

// เรียกจาก loop(): ใส่ข้อมูลล่าสุดไว้ให้ task ส่ง (ถ้ายังส่งของเก่าไม่เสร็จ จะถูกแทนที่ด้วยของใหม่)
static void queueUpload(const String& json)
{
  if (!uploadMutex) return;
  xSemaphoreTake(uploadMutex, portMAX_DELAY);
  pendingJson = json;
  hasPending = true;
  xSemaphoreGive(uploadMutex);
}
