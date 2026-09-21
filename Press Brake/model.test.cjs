const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const B=require('./ui/model.js');
function fixture(){const db=B.defaults();Object.assign(B.tool(db,'punch'),{height:20});Object.assign(B.tool(db,'die'),{height:10,opening:8});db.tools.maxDepth=50;const b={id:'1',name:'Test',angle:90,width:20,final:-45,x:5,useX:true,calibrated:true,notes:'',manualPositions:false,punchId:'p1',dieId:'d1'};b.setup=B.signature(db,b.punchId,b.dieId);db.bends=[b];return {db,b,c:{...B.settings,toolMax:5000},s:{zeroed:[true,true],position:[0,0]}};}
test('browser JavaScript parses',()=>{for(const f of ['app.js','model.js'])new vm.Script(fs.readFileSync('ui/'+f,'utf8'),{filename:f});});
test('guided plan derives offset and clamp from tooling geometry from any homed start',()=>{const {b,db,c,s}=fixture(),p=B.plan(b,db,c,s);assert.deepEqual(p.map(m=>m.target),[-3800,500,-4380,-4500,-4380,-500]);assert.deepEqual(p.map(m=>m.gesture),['down','pair','pair','pair','pair','up']);assert.equal(p[3].autoContinue,true);assert.equal(B.geometry(db,'p1','d1',b).start,-5);assert.equal(B.pulses(.005),1);});
test('manual offset and clamp positions replace calculated geometry',()=>{const {b,db,c,s}=fixture();Object.assign(b,{manualPositions:true,offset:-12,clamp:-20,final:-22});const p=B.plan(b,db,c,s);assert.deepEqual(p.map(m=>m.target),[-1200,500,-2000,-2200,-2000,-500]);});
test('acknowledged geometry and travel warnings produce force moves',()=>{const {b,db,c,s}=fixture();Object.assign(b,{manualPositions:true,offset:21,clamp:15.2,final:-25});c.maxTravel=250;assert.throws(()=>B.plan(b,db,c,s),e=>Boolean(e.overrideable&&e.warnings.length>=2));const p=B.plan(b,db,c,s,{override:true});assert.ok(p.every(m=>m.force));});
for(const [name,change,match]of [
 ['missing home',f=>f.s.zeroed[0]=false,/homes/],
 ['unhomed gauge',f=>f.s.zeroed[1]=false,/homes/],
 ['changed tooling',f=>B.tool(f.db,'punch').height=21,/changed/],
 ['unconfirmed calibration',f=>f.b.calibrated=false,/Confirm/],
 ['incorrect bottom',f=>f.b.final=-43,/expected bend order/],
 ['collision cap',f=>f.b.final=-51,/depth limit/],
 ['unapplied cap',f=>f.c.toolMax=4900,/not applied/],
 ['command budget',f=>f.c.maxTravel=100,/budget/],
 ['bad number',f=>f.b.final=NaN,/Bottom position/]
])test('blocks '+name,()=>{const f=fixture();change(f);assert.throws(()=>B.plan(f.b,f.db,f.c,f.s),match);});
test('legacy library migrates tools and invalidates old setup signatures',()=>{const old={version:1,tools:{punch:{name:'P',height:10},die:{name:'D',height:20,opening:8},maxDepth:0},material:{name:'Steel',tensile:45000,radiusPercent:15,springback:.7},bends:[],nextId:1},db=B.validateLibrary(old);assert.equal(db.version,2);assert.equal(B.tool(db,'punch').name,'P');assert.equal(db.machine.punchToTable,74);});
test('stationary telemetry cannot complete an unacknowledged command',()=>{const g=new B.MoveGate();g.begin(1,0,10,0);assert.equal(g.status({fault:'none',enabled:true,axis:-1,completedId:1,position:[10,0]},10),false);});
test('acknowledgement plus matching target completes only correct command',()=>{const g=new B.MoveGate();g.begin(3,0,10,0);g.result({id:3,ok:true});const s={fault:'none',enabled:true,axis:-1,moveId:0,completedId:2,position:[10,0]};assert.equal(g.status(s,50),false);s.completedId=3;assert.equal(g.status(s,60),true);assert.equal(g.pending,null);});
test('reject, fault, timeout, and wrong position cannot advance phase',()=>{for(const mode of ['reject','fault','timeout','position','stopped']){const g=new B.MoveGate();g.begin(1,0,20,0);if(mode==='reject'){assert.throws(()=>g.result({id:1,ok:false,message:'Travel rejected'}),/Travel/);continue;}if(mode!=='timeout')g.result({id:1,ok:true});const s={fault:mode==='fault'?'limit':'none',enabled:true,axis:-1,completedId:mode==='position'?1:0,moveId:0,position:[0,0]};assert.throws(()=>g.status(s,2000));assert.equal(g.pending,null);}});
test('configuration defaults and validation match revised motion scale',()=>{assert.equal(B.settings.microsteps,4);assert.equal(B.settings.vAccel,200);assert.equal(B.settings.hAccel,200);assert.throws(()=>B.validateSettings({...B.settings,microsteps:3}));assert.throws(()=>B.validateSettings({...B.settings,stopDiag:'true'}));assert.throws(()=>B.validateSettings({...B.settings,toolMax:160001}));assert.equal(B.stepsPerMm({...B.settings,microsteps:16}),400);});
test('backup rejects unsupported schema',()=>{assert.throws(()=>B.validateLibrary({version:0}));});
