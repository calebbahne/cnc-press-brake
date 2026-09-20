const {test}=require('node:test'),assert=require('node:assert/strict'),net=require('node:net');
const {createHost}=require('./server.cjs'),{createSimulator}=require('./simulator.cjs'),B=require('./ui/model.js');
const listen=s=>new Promise(r=>s.listen(0,'127.0.0.1',r));
async function fixture(t){const sim=createSimulator();await listen(sim);const host=createHost({espHost:'127.0.0.1',espHttpPort:sim.address().port,espWsPort:sim.address().port});await listen(host);const socket=net.connect(host.address().port,'127.0.0.1');let buffer=Buffer.alloc(0),handshake=false,messages=[],waiters=[];
  socket.on('data',chunk=>{buffer=Buffer.concat([buffer,chunk]);if(!handshake){const end=buffer.indexOf('\r\n\r\n');if(end<0)return;buffer=buffer.subarray(end+4);handshake=true;}while(buffer.length>=2){let len=buffer[1]&127,off=2;if(len===126){if(buffer.length<4)return;len=buffer.readUInt16BE(2);off=4;}if(buffer.length<off+len)return;const m=JSON.parse(buffer.subarray(off,off+len));buffer=buffer.subarray(off+len);messages.push(m);for(const w of [...waiters])if(w.fn(m)){clearTimeout(w.timer);waiters=waiters.filter(x=>x!==w);w.resolve(m);}}});
  function send(text){const b=Buffer.from(text),mask=Buffer.from([1,2,3,4]),h=Buffer.from([0x81,0x80|b.length]);for(let i=0;i<b.length;i++)b[i]^=mask[i%4];socket.write(Buffer.concat([h,mask,b]));}
  function wait(fn){const m=messages.find(fn);if(m)return Promise.resolve(m);return new Promise((resolve,reject)=>{const w={fn,resolve,timer:setTimeout(()=>reject(Error('Timed out waiting for protocol event')),2500)};waiters.push(w);});}
  socket.write(`GET /ws HTTP/1.1\r\nHost: 127.0.0.1:${host.address().port}\r\nOrigin: http://127.0.0.1:${host.address().port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n`);
  await wait(m=>m.type==='hello');
  t.after(()=>{for(const w of waiters)clearTimeout(w.timer);socket.destroy();host.shutdown();sim.shutdown();});
  return {send,wait,clear:()=>messages=[],url:`http://127.0.0.1:${host.address().port}`};
}
test('simulated relay: manual home, complete stroke, reject cap, stop preserves home, disable clears it',{timeout:10000},async t=>{
  const w=await fixture(t),data=new URLSearchParams(Object.entries({...B.settings,toolMax:2000}).map(([k,v])=>[k,typeof v==='boolean'?String(+v):String(v)]));
  assert.equal((await fetch(w.url+'/settings',{method:'POST',body:data})).status,200);
  w.send('arm');await w.wait(m=>m.type==='status'&&m.enabled);w.send('zero:v');w.send('zero:h');await w.wait(m=>m.zeroed?.every(Boolean));
  for(const [id,a,target]of [[1,'h',20],[2,'v',20],[3,'v',30],[4,'v',40],[5,'v',20]]){w.clear();w.send(`${id}|goto:${a}:${target}`);const hold=setInterval(()=>{w.send('beat');w.send('hold:'+id);},60);try{assert.equal((await w.wait(m=>m.type==='result'&&m.id===id)).ok,true);const s=await w.wait(m=>m.completedId===id);assert.equal(s.position[a==='v'?0:1],target);}finally{clearInterval(hold);}}
  w.clear();w.send('6|goto:v:2001');assert.equal((await w.wait(m=>m.type==='result'&&m.id===6)).ok,false);
  w.send('stop');assert.equal((await w.wait(m=>m.type==='status'&&m.axis===-1)).zeroed[0],true);
  w.clear();w.send('disable');const disabled=await w.wait(m=>m.type==='status'&&!m.enabled);assert.deepEqual(disabled.zeroed,[false,false]);
});
test('simulated controller expires missing motion hold even when connection heartbeat continues',{timeout:5000},async t=>{const w=await fixture(t);w.send('arm');await w.wait(m=>m.enabled);w.clear();w.send('1|step:v:200');const heartbeat=setInterval(()=>w.send('beat'),60);try{assert.equal((await w.wait(m=>m.type==='result')).ok,true);const stopped=await w.wait(m=>m.fault==='motion hold expired');assert.equal(stopped.axis,-1);assert.notEqual(stopped.completedId,1);assert.equal(stopped.zeroed[0],false);}finally{clearInterval(heartbeat);}});
