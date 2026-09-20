(function(root,factory){const api=factory();if(typeof module==='object')module.exports=api;else root.Brake=api;})(globalThis,()=>{
  'use strict';
  const stepsPerMm=200;
  const defaults=()=>({version:1,tools:{punch:{name:'My punch',part:'',length:0,height:0,width:0,tipRadius:0,angle:88},die:{name:'My die',part:'',length:0,height:0,width:0,opening:0,angle:88},maxDepth:0},material:{name:'Aluminum flashing',thickness:0.2,springback:0,notes:'Calibrate using the actual 0.2 mm flashing roll.'},bends:[],nextId:1});
  const settings={current:600,hold:50,precharge:400,vSpeed:250,vAccel:100,hSpeed:250,hAccel:100,start:20,maxTravel:9600,sgthrs:0,senseMin:200,settle:300,vMax:5000,hMax:9600,toolMax:0,homeSpeed:200,homeBackoff:200,homeSkew:200,stopDiag:false,limitHoming:false};
  const fields=[['current','Run current / motor','mA RMS',300,1000],['hold','Hold current','%',30,100],['precharge','Current precharge','ms',100,2000],['vSpeed','Y maximum speed','pulses/s',20,600],['vAccel','Y acceleration','pulses/s²',10,1000],['hSpeed','X maximum speed','pulses/s',20,600],['hAccel','X acceleration','pulses/s²',10,1000],['start','Start / final speed','pulses/s',5,100],['maxTravel','Maximum move budget','pulses',1,100000],['vMax','Y machine travel','pulses',1,5000],['hMax','X machine travel','pulses',1,9600],['toolMax','Installed tool maximum Y','pulses',0,5000],['homeSpeed','Switch homing speed','pulses/s',20,400],['homeBackoff','Switch backoff','pulses',20,400],['homeSkew','Maximum homing squaring correction','pulses',1,400],['sgthrs','StallGuard threshold','',0,255],['senseMin','DIAG minimum speed','pulses/s',50,600],['settle','DIAG settling time','ms',100,2000]];
  function numeric(value,name,min=0,max=1e6){if(value===''||value===null||typeof value==='boolean')throw Error(name+' is required.');const n=Number(value);if(!Number.isFinite(n)||n<min||n>max)throw Error(name+' must be between '+min+' and '+max+'.');return n;}
  function pulses(mm){return Math.round(numeric(mm,'Position',-1000,1000)*stepsPerMm);}
  function validateSettings(c){for(const [k,label,,lo,hi] of fields){numeric(c[k],label,lo,hi);if(!Number.isInteger(c[k]))throw Error(label+' must be a whole number.');}if(c.start>Math.min(c.vSpeed,c.hSpeed))throw Error('Start speed exceeds a stage speed.');if(c.toolMax>c.vMax)throw Error('Tool maximum exceeds Y travel.');if(c.homeBackoff>Math.min(c.vMax,c.hMax))throw Error('Home backoff exceeds travel.');for(const k of ['stopDiag','limitHoming'])if(typeof c[k]!=='boolean')throw Error('Invalid '+k);return c;}
  function validateLibrary(db){if(db.version!==1||!db.tools?.punch||!db.tools?.die||!db.material||!Array.isArray(db.bends)||db.bends.length>100)throw Error('Unsupported library file.');for(const tool of [db.tools.punch,db.tools.die]){for(const k of ['name','part'])if(typeof tool[k]!=='string'||tool[k].length>160)throw Error('Invalid tool name.');for(const k of ['length','height','width'])numeric(tool[k],k,0,2000);numeric(tool.angle,'Tool angle',1,179);}numeric(db.tools.punch.tipRadius,'Tip radius',0,100);numeric(db.tools.die.opening,'Die opening',0,1000);numeric(db.tools.maxDepth,'Tool depth limit',0,25);numeric(db.material.thickness,'Thickness',0.001,20);numeric(db.material.springback,'Springback',0,90);if(typeof db.material.name!=='string'||db.material.name.length>160)throw Error('Invalid material name.');for(const b of db.bends){if(typeof b.id!=='string'||typeof b.name!=='string'||b.name.length>160)throw Error('Invalid bend.');for(const k of ['width','angle','approach','clamp','final','retract','x'])numeric(b[k],k,0,2000);if(typeof b.useX!=='boolean')throw Error('Invalid gauge selection.');}return db;}
  function signature(db){return JSON.stringify([db.tools,db.material]);}
  function plan(b,db,c,status){
    validateLibrary(db); validateSettings(c);
    if(!b)throw Error('Select a saved bend.');
    if(b.setup!==signature(db))throw Error('Tooling/material changed. Review and save this bend again.');
    if(!status?.zeroed?.[0] || (b.useX&&!status.zeroed[1]))throw Error('Set required Y/X homes first.');
    const width=numeric(b.width,'Bend width',0.1,2000),angle=numeric(b.angle,'Included angle',1,179);
    if(width>Math.min(db.tools.punch.length,db.tools.die.length))throw Error('Bend width exceeds usable punch/die length.');
    if(!db.tools.punch.height||!db.tools.die.height||!db.tools.die.opening)throw Error('Enter installed tool heights and V opening.');
    if(angle<Math.max(db.tools.punch.angle,db.tools.die.angle))throw Error('Requested angle is smaller than a tooling included angle.');
    if(!b.calibrated)throw Error('Confirm these taught depths were checked for this angle/material.');
    const cap=pulses(db.tools.maxDepth);
    if(!cap || c.toolMax!==cap)throw Error('Apply the installed tooling depth limit to the controller first.');
    const [approach,clamp,final,retract]=['approach','clamp','final','retract'].map(k=>pulses(b[k]));
    if(!(approach>=0&&clamp>=approach&&final>clamp&&retract>=0&&retract<=clamp))throw Error('Depth order: approach ≤ clamp < final; retract must be at or above clamp.');
    if(Math.max(approach,clamp,final,retract)>Math.min(c.vMax,cap))throw Error('Bend exceeds installed tooling or machine depth limit.');
    const moves=[];
    if(b.useX)moves.push({phase:'Position backgauge',axis:'h',target:pulses(b.x),gesture:'pair'});
    moves.push({phase:'Approach',axis:'v',target:approach,gesture:'pair'},{phase:'Clamp',axis:'v',target:clamp,gesture:'down'},{phase:'Bend',axis:'v',target:final,gesture:'down'},{phase:'Retract',axis:'v',target:retract,gesture:'up'});
    const pos=[...status.position];for(const m of moves){const i=m.axis==='v'?0:1;if(m.target<0||m.target>(i===0?Math.min(c.vMax,cap):c.hMax)||Math.abs(m.target-pos[i])>c.maxTravel)throw Error('A move exceeds the travel envelope or command budget.');pos[i]=m.target;}
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
