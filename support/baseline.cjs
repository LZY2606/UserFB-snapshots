const assert=require('node:assert/strict');
const fs=require('node:fs');const os=require('node:os');const path=require('node:path');
const {bundle,environment}=require('./build.cjs');
process.env.NODE_PATH=environment.NODE_PATH;require('node:module').Module._initPaths();
const {JSDOM}=require('jsdom');
(async()=>{
 const temp=fs.mkdtempSync(path.join(os.tmpdir(),'user-event-baseline-'));
 const dom=new JSDOM('<div id="source" draggable="true">record</div><div id="target">drop</div><button>confirm</button><input>');
 try{
  const output=path.join(temp,'user-event.cjs');bundle('src/index.ts',output);
  const userEvent=require(output).default;const document=dom.window.document;
  const user=userEvent.setup({document});const counts={pointerdown:0,pointerup:0,dragstart:0,drop:0,click:0};
  for(const type of Object.keys(counts))document.body.addEventListener(type,()=>counts[type]++);
  await user.pointer([{target:document.querySelector('#source'),keys:'[MouseLeft>]'},
   {target:document.querySelector('#target')},{keys:'[/MouseLeft]'}]);
  assert.deepEqual(counts,{pointerdown:1,pointerup:1,dragstart:0,drop:0,click:1});
  console.log('USER_EVENT_BASELINE_OK pointerdown=1 pointerup=1 dragstart=0 drop=0 click=1');
  let clicked=0;document.querySelector('button').addEventListener('click',()=>clicked++);
  await user.click(document.querySelector('button'));await user.type(document.querySelector('input'),'abc');
  assert.equal(clicked,1);assert.equal(document.querySelector('input').value,'abc');
  await user.keyboard('[ControlLeft>]');await user.keyboard('[/ControlLeft]');
  console.log('USER_EVENT_LEGACY_CLICK_TYPE_OK');
 }finally{dom.window.close();fs.rmSync(temp,{recursive:true,force:true});}
})().catch(e=>{console.error(e);process.exitCode=1;});
