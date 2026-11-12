import { run } from 'node:test';
import { fileURLToPath } from 'node:url';
const required=new Set(['blank alignment','optimal changes','term boundaries','store isolation']);
const passed=new Set(); let failed=false, executed=0;
const stream=run({files:[fileURLToPath(new URL('../test/blank-diff-contract.mjs',import.meta.url))],execArgv:['--import',fileURLToPath(new URL('./register.mjs',import.meta.url))]});
for await(const event of stream){
 const d=event.data;
 if(event.type==='test:fail'){failed=true;console.error(d.name,d.details?.error);}
 if(event.type==='test:pass'&&required.has(d.name)) {
  if(d.skip||d.todo) failed=true;else {passed.add(d.name);executed++;console.log('PASS',d.name);}
 }
}
if(failed||passed.size!==required.size){console.error('missing/failed target groups', [...required].filter(x=>!passed.has(x)));process.exit(1);}
await import('./baseline.mjs');
console.log(`TARGET_COMPLETE groups=${executed}`);
