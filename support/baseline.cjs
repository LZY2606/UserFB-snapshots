'use strict';
const assert=require('node:assert/strict');
const {JSDOM}=require('./tools/node_modules/jsdom');
const {withBundle}=require('./bundle.cjs');
withBundle('src/index.ts',async built=>{
 const {default:userEvent}=require(built);
 const dom=new JSDOM('<input value="abc"><div contenteditable="true"><b>a</b><i>b</i></div>');
 const d=dom.window.document,u=userEvent.setup({document:d,delay:null});
 const input=d.querySelector('input'),ce=d.querySelector('div');
 input.focus();input.setSelectionRange(1,2);
 await u.keyboard('汉');assert.equal(input.value,'a汉c');
 ce.focus();const r=d.createRange();r.setStart(ce.firstChild.firstChild,1);r.setEnd(ce.lastChild.firstChild,1);
 d.getSelection().removeAllRanges();d.getSelection().addRange(r);
 await u.keyboard('X');assert.equal(ce.textContent,'aX');
 console.log(`IME_STARTING_API compositionStart=${typeof u.compositionStart}`);
 console.log('IME_BASELINE_OK input=a汉c contenteditable=aX');
 dom.window.close();
}).catch(e=>{console.error(e);process.exitCode=1});
