const assert = require('node:assert/strict');
const {spawnSync} = require('node:child_process');
let count=0;
global.describe=(_,fn)=>fn();
global.it=(_,fn)=>{fn();count++;};
global.expect=value=>({toBe:expected=>assert.strictEqual(value,expected),
  toBeInstanceOf:ctor=>assert.ok(value instanceof ctor)});
require('../src/interpreter/finite-automaton/__tests__/fa-test');
assert.equal(count,5);
console.log('FA_OLD_TESTS_OK cases=5');
const tree=require('../src/regexp-tree');
const ast=tree.parse('/a(b|c)+/', {captureLocations:true});
const nfa=tree.fa.toNFA(ast), dfa=tree.fa.toDFA(ast);
for (const [s,yes] of [['abcb',true],['a',false],['ad',false]]) {
 assert.equal(nfa.matches(s),yes); assert.equal(dfa.matches(s),yes);
}
console.log('REGEXP_GRAPH_BASELINE_OK');
if(process.argv[2]==='target') {
 assert.ok(require('node:fs').statSync('sanity/ambiguity.test.cjs').size>0);
 const p=spawnSync(process.execPath,['--test','sanity/ambiguity.test.cjs'],{encoding:'utf8'});
 process.stdout.write(p.stdout);process.stderr.write(p.stderr);
 assert.equal(p.status,0);assert.match(p.stdout,/# pass 6\b/);assert.match(p.stdout,/# skipped 0\b/);
 console.log('REGEXP_AMBIGUITY_OK cases=6');
}
