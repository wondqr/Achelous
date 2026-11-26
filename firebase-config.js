/* =============================================================
   firebase-config.js — ค่าเชื่อมต่อ Firebase ที่ทุกหน้าใช้ร่วมกัน
   ต้องโหลดหลัง firebase-app-compat.js (และ SDK อื่นที่หน้านั้นใช้)

   หมายเหตุ: Web config ของ Firebase เปิดเผยในหน้าเว็บได้ตามปกติ
   ความปลอดภัยของข้อมูลอยู่ที่ Security Rules (ดู docs/firebase-storage-design.md)
   ============================================================= */
window.ACHELOUS_FIREBASE_CONFIG = {
  apiKey: 'AIzaSyAavZZpl_3jFlQmmlEw6ZDtqEFRP65N0GQ',
  authDomain: 'archelous.firebaseapp.com',
  databaseURL: 'https://archelous-default-rtdb.asia-southeast1.firebasedatabase.app',
  projectId: 'archelous',
  storageBucket: 'archelous.firebasestorage.app',
  messagingSenderId: '977068768365',
  appId: '1:977068768365:web:bd92ad666f65107552d364',
  measurementId: 'G-DHX1XCHYHC'
};

// เริ่ม Firebase ครั้งเดียวต่อหน้า; คืน null ถ้าโหลด SDK ไม่ได้
window.ACHELOUS_initFirebase = function () {
  if (!window.firebase || !firebase.initializeApp) return null;
  if (!firebase.apps.length) firebase.initializeApp(window.ACHELOUS_FIREBASE_CONFIG);
  return firebase.app();
};
