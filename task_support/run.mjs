import { spawnSync } from 'node:child_process';
import { stat } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import assert from 'node:assert/strict';
const root=fileURLToPath(new URL('../',import.meta.url));
process.chdir(root);
const mode=process.argv[2]??'baseline';
const groups=['clock','transition','composition'];
function run(args) {
  const r=spawnSync(process.execPath,args,{cwd:root,encoding:'utf8',timeout:120000,maxBuffer:8*1024*1024});
  process.stdout.write(r.stdout??''); process.stderr.write(r.stderr??'');
  if(r.error||r.signal||r.status!==0) process.exit(r.status||1);
  return r.stdout;
}
if(mode==='child') {
  const st=await stat('tests/cpu_preview.mjs'); assert(st.isFile()&&st.size>0,'missing or empty target');
  await import(new URL('../tests/cpu_preview.mjs',import.meta.url));
  const {execute}=await import('./check.mjs'); await execute(groups);
} else {
  assert(mode==='baseline'||mode==='target','invalid mode');
  if(mode==='target') {
    const output=run([path.join(root,'task_support/run.mjs'),'child']);
    for(const group of groups) assert(new RegExp(`^GROUP ${group} checks=[1-9][0-9]* COMPLETE$`,'m').test(output));
    assert(/^TARGET_COMPLETE groups=3$/m.test(output),'target did not return normally');
  }
  run(['task_support/baseline.mjs']);
}
