#pragma once
// ============================================================
//  secrets.h  -  ค่าลับทั้งหมดอยู่ที่นี่ ไฟล์เดียว
//  *** ห้ามอัปโหลดไฟล์นี้ขึ้น GitHub *** (ใส่ชื่อไฟล์นี้ใน .gitignore)
// ============================================================

// WiFi ที่ ESP32 ใช้ต่ออินเทอร์เน็ต (เอาจากค่าเดิมใน main.cpp)
#define SECRET_WIFI_SSID      "SPACE AC"
#define SECRET_WIFI_PASSWORD  "0632317613"

// Firebase: Project settings > General > Web API Key
#define FIREBASE_API_KEY      "ใส่ Web API Key"

// Realtime Database URL "ไม่มี / ท้ายสุด" เช่น
// https://achelous-xxxxx-default-rtdb.asia-southeast1.firebasedatabase.app
#define FIREBASE_DB_URL       "ใส่ Database URL"

// บัญชีที่สร้างใน Firebase > Authentication > Users (ใช้สำหรับ ESP32 เท่านั้น)
#define FIREBASE_USER_EMAIL    "esp32@achelous.local"
#define FIREBASE_USER_PASSWORD "ใส่รหัสผ่านของบัญชีนี้"
