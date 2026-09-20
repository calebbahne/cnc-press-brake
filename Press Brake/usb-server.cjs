// Local browser UI and USB serial relay. The ESP owns all motion decisions.
const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const { WebSocket, WebSocketServer } = require('ws');
const { SerialPort } = require('serialport');

function createUsbHost({ comPort, Serial = SerialPort, retryMs = 1500 } = {}) {
  if (!/^COM\d+$/i.test(comPort || '')) throw new Error('Provide the ESP COM port, for example COM5.');
  const files = { '/': ['index.html','text/html'], '/app.js': ['app.js','text/javascript'],
    '/model.js': ['model.js','text/javascript'], '/style.css': ['style.css','text/css'] };
  let port, browser, retry, helloRetry, line = '', nextId = 0, shuttingDown = false;
  let badFrames = 0, lastBadReport = 0;
  const pending = new Map();
  const wss = new WebSocketServer({ noServer: true });
  const server = http.createServer((req, res) => {
    const host = `127.0.0.1:${server.address().port}`;
    if (req.headers.host !== host || (req.headers.origin && req.headers.origin !== `http://${host}`))
      return reply(res, 403, 'Open this UI at its local 127.0.0.1 address.');
    if (req.method === 'GET' && files[req.url]) {
      const [name, mime] = files[req.url];
      res.writeHead(200, { 'Content-Type': mime+'; charset=utf-8', 'Cache-Control': 'no-store' });
      fs.createReadStream(path.join(__dirname,'ui',name)).pipe(res); return;
    }
    if (req.method === 'GET' && req.url === '/health') return reply(res,200,`USB relay on ${comPort}: ${port?.isOpen?'connected':'disconnected'}`);
    if (req.method !== 'POST' || req.url !== '/settings') return reply(res,404,'Not found');
    if (!String(req.headers['content-type']||'').startsWith('application/x-www-form-urlencoded')) return reply(res,415,'Expected form data');
    let body='', size=0;
    req.on('data', chunk => { size+=chunk.length; if (size<=4096) body+=chunk.toString(); });
    req.on('end', () => {
      if (size>4096) return reply(res,413,'Settings too large');
      if (!port?.isOpen || browser?.readyState!==WebSocket.OPEN) return reply(res,502,'USB controller unavailable.');
      const id=++nextId;
      const timeout=setTimeout(() => { pending.delete(id); reply(res,504,'ESP settings response timed out.'); },10000);
      pending.set(id,{res,timeout});
      write(`S:${id}:${body}`);
    });
  });
  function reply(res, code, message) {
    if (res.writableEnded) return;
    res.writeHead(code, { 'Content-Type':'text/plain; charset=utf-8', 'Cache-Control':'no-store' }); res.end(message);
  }
  function write(message) { if (port?.isOpen) port.write(message+'\n'); }
  function disconnect() {
    clearInterval(helloRetry);
    if (browser) { const old=browser; browser=null; write('X'); old.close(); }
    for (const [id,p] of pending) { clearTimeout(p.timeout); reply(p.res,502,'USB controller disconnected.'); pending.delete(id); }
  }
  function receive(data) {
    line+=data.toString('utf8');
    if (line.length>16384) { line=''; reportBadFrame(); }
    for (let end; (end=line.indexOf('\n'))>=0;) {
      const row=line.slice(0,end).trim(); line=line.slice(end+1);
      if (!row.startsWith('@')) {
        if (row && /^[\x20-\x7e]+$/.test(row) && !row.includes('"type":"status"')) console.log(`ESP: ${row}`);
        else if (row) reportBadFrame();
        continue;
      }
      let msg;
      try { msg=JSON.parse(row.slice(1)); } catch { reportBadFrame(); continue; }
      if (msg.type==='settingsResult') {
        const p=pending.get(msg.id); if (!p) continue;
        pending.delete(msg.id); clearTimeout(p.timeout); reply(p.res,msg.code,msg.message); continue;
      }
      if (msg.type==='hello') clearInterval(helloRetry);
      if (browser?.readyState===WebSocket.OPEN) browser.send(JSON.stringify(msg));
    }
  }
  function reportBadFrame() {
    ++badFrames;
    if (Date.now()-lastBadReport>5000) {
      console.warn(`USB serial data corrupted (${badFrames} bad frames). Check cable, COM port, and board power.`);
      lastBadReport=Date.now(); badFrames=0;
    }
  }
  function open() {
    if (shuttingDown) return;
    const active=new Serial({path:comPort.toUpperCase(),baudRate:115200,autoOpen:false}); port=active;
    active.on('data', receive);
    active.on('open', () => { line=''; active.set?.({dtr:false,rts:false},()=>{}); console.log(`ESP USB connected on ${comPort}`); disconnect(); });
    active.on('error', error => console.error(`USB ${comPort}: ${error.message}`));
    active.on('close', () => {
      if (port!==active) return;
      console.warn(`USB ${comPort} disconnected; retrying.`);
      port=null; disconnect();
      if (!shuttingDown) retry=setTimeout(open,retryMs);
    });
    active.open(error => {
      if (error) { console.error(`USB ${comPort}: ${error.message}`); if (port===active) port=null; if (!shuttingDown) retry=setTimeout(open,retryMs); }
    });
  }
  server.on('upgrade', (req, socket, head) => {
    const host=`127.0.0.1:${server.address().port}`;
    if (req.url!=='/ws' || req.headers.host!==host || req.headers.origin!==`http://${host}` || !port?.isOpen || browser) {
      socket.destroy(); return;
    }
    wss.handleUpgrade(req,socket,head,ws => {
      browser=ws; write('K');
      helloRetry=setInterval(() => { if (browser===ws) write('K'); else clearInterval(helloRetry); },1000);
      ws.on('message', (data,isBinary) => { if (!isBinary && data.length<=96) write('C:'+data.toString()); });
      ws.on('close', () => { if (browser===ws) { clearInterval(helloRetry); browser=null; write('X'); } });
      ws.on('error', () => {});
    });
  });
  server.shutdown = () => { shuttingDown=true; clearTimeout(retry); disconnect(); port?.close(); wss.close(); server.close(); };
  open();
  return server;
}

if (require.main===module) {
  const comPort=process.argv[2];
  const server=createUsbHost({comPort});
  server.listen(8080,'127.0.0.1',()=>console.log(`UI: http://127.0.0.1:8080\nESP: ${comPort} over USB. Keep this window open.`));
  for (const signal of ['SIGINT','SIGTERM']) process.on(signal,()=>server.shutdown());
}
module.exports={createUsbHost};
