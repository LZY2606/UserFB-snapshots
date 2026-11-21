// A group succeeds only after its callback returns and executes real assertions.
import assert from 'node:assert/strict';
import { AsyncLocalStorage } from 'node:async_hooks';
const scope=new AsyncLocalStorage();
const cases=[];
export function group(name,body) {
  assert.equal(typeof name,'string'); assert.equal(typeof body,'function');
  assert(!cases.some(c=>c.name===name),'duplicate group'); cases.push({name,body});
}
export function check(condition,message='check failed') {
  const state=scope.getStore(); assert(state?.active,'check must execute inside group');
  state.checks++; assert.ok(condition,message);
}
export async function execute(expected) {
  assert.deepEqual(cases.map(c=>c.name).sort(),[...expected].sort(),'required groups missing or extra');
  for(const c of cases) {
    const state={active:true,checks:0};
    try { await scope.run(state,()=>c.body()); } finally { state.active=false; }
    assert(state.checks>0,`${c.name}: zero assertions`);
    console.log(`GROUP ${c.name} checks=${state.checks} COMPLETE`);
  }
  console.log(`TARGET_COMPLETE groups=${cases.length}`);
}
