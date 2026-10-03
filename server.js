const express = require('express');
const http = require('http');
const { Server } = require('socket.io');
const cors = require('cors');

const app = express();
app.use(cors());
app.use(express.json());

const server = http.createServer(app);
const io = new Server(server, {
  cors: { origin: '*' } // ทดสอบก่อน ค่อยจำกัดเป็น URL เว็บจริงทีหลัง
});

let latest = { temp: null, ph: null, tds: null, turb: null };

app.post('/api/sensor', (req, res) => {
  latest = req.body;
  io.emit('sensor-data', latest);
  res.sendStatus(200);
});

app.get('/api/sensor', (req, res) => {
  res.json(latest);
});

const PORT = process.env.PORT || 3000;
server.listen(PORT, () => console.log('listening on ' + PORT));