(function(root,factory){const api=factory();if(typeof module==='object')module.exports=api;else root.Brake=api;})(globalThis,()=>{
  'use strict';
  const defaults=()=>({version:1,tools:{punch:{name:'My punch',height:0},die:{name:'My die',height:0,opening:0},maxDepth:0},material:{name:'Cold Rolled Steel',tensile:45000,radiusPercent:15,springback:0.7},bends:[],nextId:1});
  const settings={microsteps:8,current:600,hold:50,precharge:400,vSpeed:250,vAccel:100,hSpeed:250,hAccel:100,start:20,maxTravel:9600,sgthrs:0,senseMin:200,settle:300,vMax:5000,hMax:9600,toolMax:0,homeSpeed:200,homeBackoff:200,homeSkew:200,stopDiag:false,vLimitHoming:false,hLimitHoming:false};
  const fields=[['microsteps','Microstepping','×',1,256],['current','Run current / motor','mA RMS',300,1000],['hold','Hold current','%',30,100],['precharge','Current precharge','ms',100,2000],['vSpeed','Y maximum speed','steps/s',20,10000],['vAccel','Y acceleration','steps/s²',10,40000],['hSpeed','X maximum speed','steps/s',20,10000],['hAccel','X acceleration','steps/s²',10,40000],['start','Start / final speed','steps/s',5,2000],['maxTravel','Maximum move budget','steps',1,1000000],['vMax','Y machine travel','steps',1,160000],['hMax','X machine travel','steps',1,307200],['toolMax','Installed tool maximum Y','steps',0,160000],['homeSpeed','Switch homing speed','steps/s',20,10000],['homeBackoff','Switch backoff','steps',20,12800],['homeSkew','Maximum homing squaring correction','steps',1,12800],['sgthrs','StallGuard threshold','',0,255],['senseMin','DIAG minimum speed','steps/s',50,10000],['settle','DIAG settling time','ms',100,2000]];
  function numeric(value,name,min=0,max=1e6){if(value===''||value===null||typeof value==='boolean')throw Error(name+' is required.');const n=Number(value);if(!Number.isFinite(n)||n<min||n>max)throw Error(name+' must be between '+min+' and '+max+'.');return n;}
  function stepsPerMm(c=settings){return 25*c.microsteps;}
  function pulses(mm,c=settings){return Math.round(numeric(mm,'Position',-1000,1000)*stepsPerMm(c));}
  function validateSettings(c){for(const [k,label,,lo,hi] of fields){numeric(c[k],label,lo,hi);if(!Number.isInteger(c[k]))throw Error(label+' must be a whole number.');}if(![1,2,4,8,16,32,64,128,256].includes(c.microsteps))throw Error('Microstepping must be 1, 2, 4, 8, 16, 32, 64, 128, or 256.');const scale=stepsPerMm(c);if(c.start>Math.min(c.vSpeed,c.hSpeed))throw Error('Start speed exceeds a stage speed.');if(c.vMax>25*scale||c.hMax>48*scale)throw Error('Machine travel exceeds 25 mm Y or 48 mm X at this microstep setting.');if(c.toolMax>c.vMax)throw Error('Tool maximum exceeds Y travel.');if(c.homeBackoff>Math.min(c.vMax,c.hMax))throw Error('Home backoff exceeds travel.');for(const k of ['stopDiag','vLimitHoming','hLimitHoming'])if(typeof c[k]!=='boolean')throw Error('Invalid '+k);return c;}
  function validateLibrary(db){if(db.version!==1||!db.tools?.punch||!db.tools?.die||!db.material||!Array.isArray(db.bends)||db.bends.length>100)throw Error('Unsupported library file.');for(const tool of [db.tools.punch,db.tools.die]){if(typeof tool.name!=='string'||tool.name.length>160)throw Error('Invalid tool name.');numeric(tool.height,'Tool height',0,2000);}numeric(db.tools.die.opening,'V opening',0,1000);numeric(db.tools.maxDepth,'Tool depth limit',0,25);if(typeof db.material.name!=='string'||db.material.name.length>160)throw Error('Invalid material name.');numeric(db.material.tensile??45000,'Material tensile strength',0,1000000);numeric(db.material.radiusPercent??15,'Inside bend radius percentage',0,1000);numeric(db.material.springback,'Material springback',0,90);for(const b of db.bends){if(typeof b.id!=='string'||typeof b.name!=='string'||b.name.length>160)throw Error('Invalid bend.');for(const k of ['width','angle','x'])numeric(b[k],k,0,2000);const depths=['approach','clamp','final','retract'];if(b.final>0&&depths.every(k=>Number.isFinite(b[k])&&b[k]>=0)){for(const k of depths)b[k]=-b[k];b.calibrated=false;}for(const k of depths)numeric(b[k],k,-2000,0);if(typeof b.useX!=='boolean')throw Error('Invalid gauge selection.');}return db;}
  function signature(db){return JSON.stringify([db.tools,db.material]);}
  function plan(b,db,c,status){
    validateLibrary(db); validateSettings(c);
    if(!b)throw Error('Select a saved bend.');
    if(b.setup!==signature(db))throw Error('Tooling/material changed. Review and save this bend again.');
    if(!status?.zeroed?.[0] || (b.useX&&!status.zeroed[1]))throw Error('Set required Y/X homes first.');
    numeric(b.width,'Bend width',0.1,2000);numeric(b.angle,'Included angle',1,179);
    if(!db.tools.punch.height||!db.tools.die.height||!db.tools.die.opening)throw Error('Enter installed tool heights and V opening.');
    if(!b.calibrated)throw Error('Confirm these taught depths were checked for this angle/material.');
    const cap=pulses(db.tools.maxDepth,c);
    if(!cap || c.toolMax!==cap)throw Error('Apply the installed tooling depth limit to the controller first.');
    const [approach,clamp,final,retract]=['approach','clamp','final','retract'].map(k=>pulses(b[k],c));
    if(!(approach<=0&&clamp<=approach&&final<clamp&&retract<=0&&retract>=clamp))throw Error('Depth order: approach ≥ clamp > final; retract must be at or above clamp.');
    if(Math.min(approach,clamp,final,retract)<-Math.min(c.vMax,cap))throw Error('Bend exceeds installed tooling or machine depth limit.');
    const moves=[];
    if(b.useX)moves.push({phase:'Position backgauge',axis:'h',target:pulses(b.x,c)});
    moves.push({phase:'Approach',axis:'v',target:approach},{phase:'Clamp',axis:'v',target:clamp},{phase:'Bend',axis:'v',target:final},{phase:'Retract',axis:'v',target:retract});
    const pos=[...status.position];for(const m of moves){const i=m.axis==='v'?0:1;if((i===0&&(m.target>0||m.target< -Math.min(c.vMax,cap)))||(i===1&&(m.target<0||m.target>c.hMax))||Math.abs(m.target-pos[i])>c.maxTravel)throw Error('A move exceeds the travel envelope or command budget.');m.gesture=i===0?(m.target<pos[i]?'down':'up'):(m.target<pos[i]?'back':'forward');pos[i]=m.target;}
    return moves;
  }
  // Shared with automated tests. A result acknowledgement AND matching completion are mandatory.
  class MoveGate {
    constructor(){this.pending=null;}
    begin(id,axis,target,now=Date.now()){if(this.pending)throw Error('A move is already pending.');this.pending={id,axis,target,accepted:false,at:now};}
    result(r){if(!this.pending||r.id!==this.pending.id)return; if(!r.ok){this.pending=null;throw Error(r.message||'Move rejected.');}this.pending.accepted=true;}
    status(s,now=Date.now()){
      const p=this.pending;if(!p)return false;
      if(s.fault!=='none'||!s.enabled){this.pending=null;throw Error(s.fault!=='none'?s.fault:'Outputs disabled.');}
      if(!p.accepted&&now-p.at>1500){this.pending=null;throw Error('No acknowledgement; stopped.');}
      if(p.accepted&&s.completedId===p.id&&s.axis===-1){if(p.target!==null&&s.position[p.axis]!==p.target){this.pending=null;throw Error('Completion position mismatch.');}this.pending=null;return true;}
      if(p.accepted&&s.axis===-1&&s.moveId!==p.id&&now-p.at>1000){this.pending=null;throw Error('Move stopped before completion.');}
      return false;
    }
    cancel(){this.pending=null;}
  }
  return {defaults,settings,fields,numeric,pulses,validateSettings,validateLibrary,signature,plan,MoveGate,stepsPerMm};
});
