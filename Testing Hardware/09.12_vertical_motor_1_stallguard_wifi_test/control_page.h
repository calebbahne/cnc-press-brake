#pragma once
const char CONTROL_PAGE[] PROGMEM = R"HTML(
<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Motor 1 · StallGuard</title><style>
body{font:16px system-ui;background:#101923;color:#e8eff7;max-width:960px;margin:25px auto;padding:0 20px}h1{font-size:28px}section{background:#1c2938;border:1px solid #354355;border-radius:12px;padding:18px;margin:16px 0}button,input{font:inherit;padding:10px;border-radius:7px;border:1px solid #6c8197}button{cursor:pointer;background:#bce8df;color:#102a27;margin:5px}button:disabled{opacity:.4}button.jog{touch-action:none;user-select:none;padding:20px 35px}.danger{background:#ffb5aa}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:16px}label{display:grid;gap:5px}small,p{color:#b9c7d6;line-height:1.5}meter{width:100%;height:35px}#load{font-size:42px;font-weight:700}pre{white-space:pre-wrap;font-size:14px}.notice{color:#ffd88a}input[type=checkbox]{width:22px;height:22px}#message{min-height:24px}
</style><h1>Motor 1 · StallGuard live test</h1>
<p>Full steps · interpolation off · CoolStep off · StealthChop · UART address 0</p>
<section><div id="connection">Connecting…</div><div id="load">—</div><meter id="meter" min="0" max="100" value="0"></meter>
<p>Relative electrical load index, not measured torque or force. The default scale maps SG 510 → 0% and SG 0 → 100%. Tune endpoints at the same speed, current and direction. Idle, precharge and ramp readings are not shown as load.</p>
<pre id="telemetry">Waiting for telemetry</pre></section>
<section><button id="arm" onclick="send('arm')">Arm / hold torque</button><button class="danger" onclick="release();send('disable')">Disable outputs</button>
<p>Arm, then hold a jog button. Release to stop immediately and retain hold current. Every jog starts with stationary precharge. Stop before reversing.</p>
<button class="jog" id="up">Hold to jog UP</button><button class="jog" id="down">Hold to jog DOWN</button><button class="danger" onclick="release()">STOP</button>
<p class="notice">Only use with motor 1 mechanically independent and the vertical load supported. Global EN energizes all attached drivers. A disconnected browser or Wi-Fi disables outputs after at most 750 ms. Travel budget is per jog, not an absolute travel limit.</p></section>
<section><h2>Tuning controls</h2><p>Disable outputs before applying. Values below are drafts until Apply succeeds. Start with a short unloaded jog. Higher acceleration demands more torque; reduce it if starting stalls.</p>
<form id="settings"><div class="grid" id="fields"></div>
<p><label><span><input type="checkbox" name="stopDiag"> Stop on DIAG in the settled sensing window</span></label></p>
<p><label><span><input type="checkbox" name="limits" checked> Require closed NC limits (GPIO34 and GPIO35)</span></label></p>
<p class="notice">Uncheck limits only for a detached bench motor. Pins 34/35 need external pull-ups. GPIO39 DIAG has no internal pull resistor; wire the driver output and use an external 10 kΩ pull-down to define the disconnected state.</p>
<button type="submit">Apply settings / clear fault</button><button type="button" onclick="send('export')">Print applied settings to Serial</button></form>
<p>Current is RMS winding current, adjustable 300–1000 mA; it does not set a power-supply current limit. Measure the supply current and stay below 2 A. Do not exceed the motor rating. Defaults: 600 mA, 50% hold, 400 ms precharge, 250 full steps/s, 100 full steps/s².</p>
<p>SGTHRS is initially 0 and DIAG stopping is initially off for baseline logging. DIAG state and rising-edge count are always displayed. After calibration, set SGTHRS (hardware trip at SG_RESULT ≤ 2 × SGTHRS) and enable stopping. Sensing minimum is an experimental gate, not proof that StallGuard is reliable.</p>
<div id="message" role="status"></div></section>
<script>
const specs=[['current','Run current (mA RMS)',300,1000,600],['hold','Hold current (%)',30,100,50],['precharge','Precharge at run current (ms)',100,2000,400],['speed','Target speed (full steps/s)',20,600,250],['accel','Acceleration (full steps/s²)',10,1000,100],['start','Starting speed (full steps/s)',5,100,20],['sgthrs','SGTHRS (0–255)',0,255,0],['senseMin','Sensing minimum (full steps/s)',50,600,200],['settle','Settle time at target speed (ms)',100,2000,300],['sgZero','SG_RESULT at 0% load',1,510,510],['sgFull','SG_RESULT at 100% load',0,509,0],['maxTravel','Max full steps per jog',20,2000,600]];
const form=document.getElementById('settings'),message=document.getElementById('message');
for(const [key,label,min,max,value] of specs){const l=document.createElement('label');l.textContent=label;const i=document.createElement('input');Object.assign(i,{name:key,type:'number',min,max,value,step:1,required:true});l.append(i);document.getElementById('fields').append(l)}
let ws,id=-1,owner=-1,lastRx=0,active=false;
function send(s){if(ws?.readyState===1)ws.send(s)}
function release(){active=false;send('stop')}
function loseFocus(){release();send('disable')}
function connect(){ws=new WebSocket(`ws://${location.hostname}:81/`);ws.onopen=()=>{document.getElementById('connection').textContent='Connected; outputs require explicit Arm';lastRx=Date.now()};
ws.onmessage=e=>{lastRx=Date.now();const s=JSON.parse(e.data);if(s.notice){message.textContent=s.notice;return}if(s.type==='hello'){id=s.id;for(const [k,v] of Object.entries(s.settings)){const el=form.elements[k];if(el.type==='checkbox')el.checked=v;else el.value=v}return}
owner=s.owner;document.getElementById('load').textContent=s.load===null?`— ${s.loadReason||'unavailable'}`:`${s.load.toFixed(1)}%`;
document.getElementById('meter').value=s.load??0;
document.getElementById('telemetry').textContent=`Fault: ${s.fault}\nOutputs: ${s.enabled?'ENABLED':'disabled'} | Direction: ${s.direction} | Speed: ${s.speed} full steps/s\nSG_RESULT: ${s.sg} | DIAG: ${s.diag?'HIGH':'LOW'} | DIAG rising edges: ${s.edges}\nSensing window: ${s.window?'settled':'inactive'} | Commanded position: ${s.position} full steps\nNC limit inputs: ${s.limit1}, ${s.limit2} (0 = closed)\nDRV_STATUS: ${s.drv} | GSTAT: ${s.gstat} | TSTEP: ${s.tstep}\nTemperature prewarn: ${s.otpw} | Overtemp: ${s.ot} | Short flags: ${s.shorts}\nOpen-load flags: ${s.openLoad} (may be spurious at rest) | CS_ACTUAL: ${s.cs}`;
document.getElementById('arm').disabled=s.enabled||s.fault!=='none';for(const k of ['up','down'])document.getElementById(k).disabled=!s.enabled||s.fault!=='none'||owner!==id;
};ws.onclose=()=>{active=false;owner=-1;document.getElementById('connection').textContent='Disconnected — reconnecting; re-arm required';document.getElementById('load').textContent='— disconnected';setTimeout(connect,1500)}}
for(const k of ['up','down']){const b=document.getElementById(k);b.onpointerdown=e=>{if(active)return;e.preventDefault();b.setPointerCapture(e.pointerId);active=true;send(k)};b.onpointerup=release;b.onpointercancel=release;b.onlostpointercapture=()=>{if(active)release()}}
addEventListener('blur',loseFocus);addEventListener('pagehide',loseFocus);document.addEventListener('visibilitychange',()=>{if(document.hidden)loseFocus()});addEventListener('keydown',e=>{if(e.code==='Space'&&!['INPUT','TEXTAREA'].includes(e.target.tagName)){e.preventDefault();release()}});
setInterval(()=>{if(owner===id&&!document.hidden&&Date.now()-lastRx<600)send('beat');if(Date.now()-lastRx>1000){document.getElementById('load').textContent='— stale telemetry';document.getElementById('meter').value=0}},100);
form.onsubmit=async e=>{e.preventDefault();const data=new URLSearchParams(new FormData(form));for(const k of ['stopDiag','limits'])data.set(k,form.elements[k].checked?'1':'0');try{const r=await fetch('/settings',{method:'POST',body:data});message.textContent=await r.text()}catch{message.textContent='Connection failed; settings not confirmed'}};
connect();
</script></html>
)HTML";
