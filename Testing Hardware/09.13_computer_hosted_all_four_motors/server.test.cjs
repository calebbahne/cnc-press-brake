const { test } = require('node:test');
const assert = require('node:assert/strict');
const http = require('node:http');
const net = require('node:net');
const { once } = require('node:events');
const { createHost } = require('./server.cjs');
const listen = server => new Promise(resolve => server.listen(0, '127.0.0.1', resolve));

test('serves UI, proxies settings/results, rejects foreign origin and handles offline ESP', async () => {
  let received;
  const esp = http.createServer((req, res) => {
    let body = ''; req.on('data', c => body += c);
    req.on('end', () => { received = [req.method, req.url, body]; res.writeHead(409); res.end('Disable first'); });
  });
  await listen(esp);
  const host = createHost({ espHost: '127.0.0.1', espHttpPort: esp.address().port });
  await listen(host);
  const url = `http://127.0.0.1:${host.address().port}`;
  try {
    const html = await (await fetch(url)).text();
    assert.match(html, /Computer-hosted four-motor test/);
    assert.match(html, /location.host\+'\/ws'/);
    const body = 'current=600&stopDiag=0';
    const response = await fetch(url + '/settings', { method: 'POST', body, headers: { 'Content-Type': 'application/x-www-form-urlencoded' } });
    assert.equal(response.status, 409); assert.equal(await response.text(), 'Disable first');
    assert.deepEqual(received, ['POST', '/settings', body]);
    assert.equal((await fetch(url, { headers: { Origin: 'http://untrusted.example' } })).status, 403);
    assert.equal((await fetch(url + '/settings', { method: 'POST', body: 'x'.repeat(4097), headers: { 'Content-Type': 'application/x-www-form-urlencoded' } })).status, 413);
    await new Promise(resolve => esp.close(resolve));
    assert.equal((await fetch(url + '/settings', { method: 'POST', body, headers: { 'Content-Type': 'application/x-www-form-urlencoded' } })).status, 502);
  } finally { host.shutdown(); esp.close(); }
});

test('WebSocket tunnel preserves bytes both ways and closes upstream with browser', { timeout: 5000 }, async () => {
  let upstreamSocket;
  let resolveClosed;
  const closed = new Promise(resolve => resolveClosed = resolve);
  const esp = net.createServer(socket => {
    upstreamSocket = socket;
    let handshake = true;
    socket.on('data', data => {
      if (handshake) {
        assert.match(data.toString(), /GET \/ HTTP\/1.1/);
        handshake = false;
        socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n');
      } else socket.write(data);
    });
    socket.on('close', resolveClosed);
  });
  await listen(esp);
  const host = createHost({ espHost: '127.0.0.1', espWsPort: esp.address().port });
  await listen(host);
  const client = net.connect(host.address().port, '127.0.0.1');
  try {
    await once(client, 'connect');
    client.write(`GET /ws HTTP/1.1\r\nHost: 127.0.0.1:${host.address().port}\r\nOrigin: http://127.0.0.1:${host.address().port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n`);
    assert.match((await once(client, 'data'))[0].toString(), /101 Switching/);
    const frame = Buffer.from([0x81, 0x84, 1, 2, 3, 4, 99, 103, 98, 112]);
    client.write(frame);
    assert.deepEqual((await once(client, 'data'))[0], frame);
    client.destroy();
    await closed;
  } finally { client.destroy(); upstreamSocket?.destroy(); host.shutdown(); esp.close(); }
});
