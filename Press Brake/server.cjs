// Dependency-free local UI host. Heartbeats originate ONLY in the browser.
const http = require('node:http');
const net = require('node:net');
const fs = require('node:fs');
const path = require('node:path');

function createHost({ espHost = 'cnc-press-brake.local', espHttpPort = 80, espWsPort = 81 } = {}) {
  if (!/^[a-zA-Z0-9.-]+$/.test(espHost)) throw new Error('Use an ESP IP address or hostname, without http:// or a port.');
  const sockets = new Set();
  const page = path.join(__dirname, 'ui', 'index.html');
  function allowed(req) {
    const expected = `127.0.0.1:${server.address().port}`;
    return req.headers.host === expected && (!req.headers.origin || req.headers.origin === `http://${expected}`);
  }
  function reply(res, code, body) {
    res.writeHead(code, { 'Content-Type': 'text/plain; charset=utf-8', 'Cache-Control': 'no-store' });
    res.end(body);
  }
  const server = http.createServer((req, res) => {
    if (!allowed(req)) return reply(res, 403, 'Open this UI using http://127.0.0.1 and its local port.');
    const assets = { '/': ['index.html','text/html'], '/app.js': ['app.js','text/javascript'], '/model.js': ['model.js','text/javascript'], '/style.css': ['style.css','text/css'] };
    if (req.method === 'GET' && assets[req.url]) {
      const [file, mime] = assets[req.url];
      res.writeHead(200, { 'Content-Type': mime + '; charset=utf-8', 'Cache-Control': 'no-store' });
      const stream = fs.createReadStream(path.join(__dirname, 'ui', file));
      stream.on('error', () => res.destroy());
      stream.pipe(res);
      return;
    }
    if (req.method === 'GET' && req.url === '/health') return reply(res, 200, `PC host running; ESP target ${espHost}. This does not verify ESP connectivity.`);
    if (req.method !== 'POST' || req.url !== '/settings') return reply(res, 404, 'Not found');
    if (!String(req.headers['content-type'] || '').startsWith('application/x-www-form-urlencoded')) return reply(res, 415, 'Expected form data');
    let body = '', size = 0;
    req.on('data', chunk => {
      size += chunk.length;
      if (size <= 4096) body += chunk.toString();
    });
    req.on('end', () => {
      if (size > 4096) return reply(res, 413, 'Settings too large');
      const upstream = http.request({ hostname: espHost, port: espHttpPort, path: '/settings', method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded', 'Content-Length': Buffer.byteLength(body) } }, incoming => {
        res.writeHead(incoming.statusCode, { 'Content-Type': 'text/plain; charset=utf-8', 'Cache-Control': 'no-store' });
        incoming.on('error', () => res.destroy());
        incoming.pipe(res);
      });
      upstream.setTimeout(10000, () => upstream.destroy(new Error('ESP timeout')));
      upstream.on('error', () => {
        if (!res.headersSent) reply(res, 502, 'ESP unavailable; settings were not confirmed.');
        else res.destroy();
      });
      res.on('close', () => upstream.destroy());
      upstream.end(body);
    });
  });
  server.on('connection', socket => {
    sockets.add(socket);
    socket.on('close', () => sockets.delete(socket));
  });
  server.on('upgrade', (req, client, head) => {
    if (!allowed(req) || req.url !== '/ws' || req.headers.upgrade?.toLowerCase() !== 'websocket' ||
        !req.headers['sec-websocket-key'] || req.headers['sec-websocket-version'] !== '13') {
      client.end('HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n');
      return;
    }
    // One upstream per browser preserves ESP client ownership and disconnect stops.
    const upstream = net.connect({ host: espHost, port: espWsPort });
    sockets.add(upstream);
    const timeout = setTimeout(() => upstream.destroy(), 5000);
    upstream.on('connect', () => {
      clearTimeout(timeout);
      upstream.setNoDelay(true); client.setNoDelay(true);
      upstream.write(`GET / HTTP/1.1\r\nHost: ${espHost}:${espWsPort}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${req.headers['sec-websocket-key']}\r\nSec-WebSocket-Version: 13\r\n\r\n`);
      if (head.length) upstream.write(head);
      client.pipe(upstream); upstream.pipe(client);
    });
    upstream.on('error', () => client.destroy());
    client.on('error', () => upstream.destroy());
    client.on('close', () => upstream.destroy());
    upstream.on('close', () => { clearTimeout(timeout); sockets.delete(upstream); client.destroy(); });
  });
  server.shutdown = () => { for (const socket of sockets) socket.destroy(); server.close(); };
  return server;
}

if (require.main === module) {
  const simulate = process.argv.includes('--simulate');
  const espHost = simulate ? '127.0.0.1' : process.argv[2] || 'cnc-press-brake.local';
  const port = Number(process.argv[3] || 8080);
  if (!Number.isInteger(port) || port < 1 || port > 65535) throw new Error('Invalid local port');
  const simulator = simulate ? require('./simulator.cjs').createSimulator() : null;
  function start() {
  const server = createHost({ espHost, ...(simulator ? {espHttpPort:simulator.address().port,espWsPort:simulator.address().port} : {}) });
  server.on('error', error => { console.error(error.message); process.exitCode = 1; });
  server.listen(port, '127.0.0.1', () => {
    console.log(`UI: http://127.0.0.1:${port}\nESP: ${espHost} (HTTP 80 / WebSocket 81)\nKeep this server running. Ctrl+C stops it and disconnects the controller.`);
  });
  if (simulate) console.log('SIMULATION ONLY — no hardware connection.');
  for (const signal of ['SIGINT', 'SIGTERM']) process.on(signal, () => {server.shutdown();simulator?.shutdown();});
  }
  if (simulator) simulator.listen(0,'127.0.0.1',start); else start();
}
module.exports = { createHost };
