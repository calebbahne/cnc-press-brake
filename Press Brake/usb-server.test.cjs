const { test } = require('node:test');
const assert = require('node:assert/strict');
const { EventEmitter, once } = require('node:events');
const { WebSocket } = require('ws');
const { createUsbHost } = require('./usb-server.cjs');

class FakeSerial extends EventEmitter {
  static instance;
  constructor(options) { super(); this.options=options; this.isOpen=false; this.writes=[]; FakeSerial.instance=this; }
  open(done) { this.isOpen=true; queueMicrotask(() => { this.emit('open'); done(); }); }
  set(_signals,done) { done(); }
  write(data) { this.writes.push(data); }
  close() { this.isOpen=false; this.emit('close'); }
  receive(message) { this.emit('data',Buffer.from(message)); }
}
async function until(check) {
  for (let i=0;i<100 && !check();++i) await new Promise(resolve=>setTimeout(resolve,10));
  assert.ok(check(),'Expected serial write did not arrive');
}

test('USB relay keeps the browser protocol and settings response', async () => {
  const host=createUsbHost({comPort:'COM7',Serial:FakeSerial});
  await new Promise(resolve=>host.listen(0,'127.0.0.1',resolve));
  const base=`http://127.0.0.1:${host.address().port}`;
  const serial=FakeSerial.instance;
  await until(()=>serial.isOpen);
  // Telemetry starts at ESP boot, before anyone opens the browser.
  serial.receive('@{"type":"status","enabled":false}\n');
  const ws=new WebSocket(base.replace('http','ws')+'/ws',{headers:{Origin:base}});
  try {
    await once(ws,'open');
    assert.equal(serial.options.baudRate,115200);
    assert.ok(serial.writes.includes('K\n'));
    serial.receive('debug line\n@{"type":"hello","id":1,"protocol":2,"stepsPerMm":200,"settings":{}}\n');
    assert.equal(JSON.parse((await once(ws,'message'))[0]).type,'hello');
    ws.send('arm');
    await until(()=>serial.writes.includes('C:arm\n'));
    assert.ok(serial.writes.includes('C:arm\n'));
    const request=fetch(base+'/settings',{method:'POST',body:'current=600&stopDiag=0',headers:{'Content-Type':'application/x-www-form-urlencoded'}});
    await until(()=>serial.writes.some(x=>x.startsWith('S:')));
    const setting=serial.writes.find(x=>x.startsWith('S:'));
    assert.match(setting,/^S:\d+:current=600&stopDiag=0\n$/);
    const id=Number(setting.split(':')[1]);
    serial.receive(`@{"type":"settingsResult","id":${id},"code":409,"message":"Disable outputs first."}\n`);
    const response=await request;
    assert.equal(response.status,409);
    assert.equal(await response.text(),'Disable outputs first.');
    assert.equal((await fetch(base+'/firmware/press_brake/wifi_secrets.h')).status,404);
    ws.close(); await once(ws,'close');
    await until(()=>serial.writes.includes('X\n'));
  } finally { ws.terminate(); host.shutdown(); }
});

test('USB loss closes browser control and reports offline', async () => {
  const host=createUsbHost({comPort:'COM8',Serial:FakeSerial,retryMs:60000});
  await new Promise(resolve=>host.listen(0,'127.0.0.1',resolve));
  const base=`http://127.0.0.1:${host.address().port}`;
  const serial=FakeSerial.instance;
  const ws=new WebSocket(base.replace('http','ws')+'/ws',{headers:{Origin:base}});
  try {
    await once(ws,'open');
    const closed=once(ws,'close');
    serial.close();
    await closed;
    assert.match(await (await fetch(base+'/health')).text(),/disconnected/);
    const response=await fetch(base+'/settings',{method:'POST',body:'current=600',headers:{'Content-Type':'application/x-www-form-urlencoded'}});
    assert.equal(response.status,502);
  } finally { ws.terminate(); host.shutdown(); }
});
