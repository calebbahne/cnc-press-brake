const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const B=require('./ui/model.js');
function fixture(){const db=B.defaults();Object.assign(db.tools.punch,{length:100,height:20});Object.assign(db.tools.die,{length:100,height:10,opening:8});db.tools.maxDepth=10;const b={id:'1',name:'Test',angle:90,width:20,approach:-1,clamp:-2,final:-2.5,retract:-1,x:5,useX:true,calibrated:true,setup:B.signature(db)};db.bends=[b];return {db,b,c:{...B.settings,toolMax:2000},s:{zeroed:[true,true],position:[0,0]}};}
test('browser JavaScript parses',()=>{for(const f of ['app.js','model.js'])new vm.Script(fs.readFileSync('ui/'+f,'utf8'),{filename:f});});
test('plan contains negative punch targets and matching direction keys',()=>{const {b,db,c,s}=fixture(),p=B.plan(b,db,c,s);assert.deepEqual(p.map(m=>m.target),[1000,-200,-400,-500,-200]);assert.deepEqual(p.map(m=>m.gesture),['forward','down','down','down','up']);assert.equal(B.pulses(.005),1);});
for(const [name,change,match]of [
 ['missing home',f=>f.s.zeroed[0]=false,/homes/],
 ['unhomed gauge',f=>f.s.zeroed[1]=false,/homes/],
 ['changed tooling',f=>f.db.tools.punch.height=21,/changed/],
 ['unconfirmed calibration',f=>f.b.calibrated=false,/Confirm/],
 ['incorrect depth order',f=>f.b.final=-1,/Depth order/],
 ['collision cap',f=>f.b.final=-11,/depth limit/],
 ['unapplied cap',f=>f.c.toolMax=1900,/Apply/],
 ['command budget',f=>f.c.maxTravel=100,/budget/],
 ['bad number',f=>f.b.final=NaN,/final/]
])test('blocks '+name,()=>{const f=fixture();change(f);assert.throws(()=>B.plan(f.b,f.db,f.c,f.s),match);});
test('stationary telemetry cannot complete an unacknowledged command',()=>{const g=new B.MoveGate();g.begin(1,0,10,0);assert.equal(g.status({fault:'none',enabled:true,axis:-1,completedId:1,position:[10,0]},10),false);});
test('acknowledgement plus matching target completes only correct command',()=>{const g=new B.MoveGate();g.begin(3,0,10,0);g.result({id:3,ok:true});const s={fault:'none',enabled:true,axis:-1,moveId:0,completedId:2,position:[10,0]};assert.equal(g.status(s,50),false);s.completedId=3;assert.equal(g.status(s,60),true);assert.equal(g.pending,null);});
test('reject, fault, timeout, and wrong position cannot advance phase',()=>{for(const mode of ['reject','fault','timeout','position','stopped']){const g=new B.MoveGate();g.begin(1,0,20,0);if(mode==='reject'){assert.throws(()=>g.result({id:1,ok:false,message:'Travel rejected'}),/Travel/);continue;}if(mode!=='timeout')g.result({id:1,ok:true});const s={fault:mode==='fault'?'limit':'none',enabled:true,axis:-1,completedId:mode==='position'?1:0,moveId:0,position:[0,0]};assert.throws(()=>g.status(s,2000));assert.equal(g.pending,null);}});
test('configuration rejects fractional pulses, invalid microsteps and non-boolean sensing',()=>{assert.throws(()=>B.validateSettings({...B.settings,vMax:1.5}));assert.throws(()=>B.validateSettings({...B.settings,microsteps:3}));assert.throws(()=>B.validateSettings({...B.settings,stopDiag:'true'}));assert.throws(()=>B.validateSettings({...B.settings,toolMax:40001}));assert.equal(B.stepsPerMm({...B.settings,microsteps:16}),400);});
test('backup rejects unsupported schema',()=>{assert.throws(()=>B.validateLibrary({version:0}));});
