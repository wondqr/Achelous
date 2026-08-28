const express = require('express');
const http = require('http');
const { Server } = require('socket.io');

const app = express();
const server = http.createServer(app);
const io = new Server(server, { cors: { origin: '*' } });

app.use(express.json());        // อ่านข้อมูล JSON ที่ ESP32 ส่งมา
app.use(express.static('public')); // เสิร์ฟไฟล์ index.html

let latestData = { temperature: 0, ph: 0, tds: 0, turbidity: 0 };

// ESP32 ส่งข้อมูลมาที่นี่
app.post('/api/sensor-data', (req, res) => {
  latestData = req.body;
  console.log('ได้รับข้อมูล:', latestData);

  io.emit('sensorData', latestData); // ส่งต่อให้ทุกคนที่เปิดเว็บอยู่ ทันที
  res.json({ status: 'ok' });
});

// เผื่อคนเพิ่งเปิดเว็บ ให้ดึงค่าล่าสุดได้
app.get('/api/sensor-data', (req, res) => {
  res.json(latestData);
});

const PORT = process.env.PORT || 3000;
server.listen(PORT, () => console.log('Server running on port ' + PORT));