import {readFileSync,writeFileSync,mkdirSync,mkdtempSync,rmSync} from 'node:fs';
import {tmpdir} from 'node:os';import {join,resolve} from 'node:path';import {spawnSync} from 'node:child_process';
const mutations=[['cache','t->cache[2] = t->cache[1];','t->cache[2] = t->cache[0];'],['half','t[skip + s->w + 3] = ((unpack & 0xFF00) >> 8);','t[skip + s->w + 3] = (unpack & 0x00FF);'],['palette','memmove(&s->palette[i][0], &oldPalette[src][0], count * 3);','memmove(&s->palette[i][0], &s->palette[src][0], count * 3);']];
const source=readFileSync('smacker.c','utf8');mkdirSync('task-results',{recursive:true});
for(const [name,old,replacement] of mutations){const dir=mkdtempSync(join(tmpdir(),'smk-mutant-'));try{
 if(source.split(old).length!==2)throw Error('mutation anchor not unique: '+name);
 const changed=join(dir,'smacker.c');writeFileSync(changed,source.replace(old,replacement));
 const build=spawnSync('bash',['task_support/build.sh','task_support/target_main.cpp','tests/task_video_model.cpp'],{encoding:'utf8',env:{...process.env,task_build:dir,TASK_SMACKER_SOURCE:changed}});
 writeFileSync(`task-results/${name}.build.log`,(build.stdout||'')+(build.stderr||'')+`\nEXIT=${build.status}\n`);
 if(build.status!==0)throw Error('mutant build failed: '+name);
 const test=spawnSync(join(dir,'run'),[],{encoding:'utf8'});const output=(test.stdout||'')+(test.stderr||'');
 writeFileSync(`task-results/${name}.test.log`,output+`\nEXIT=${test.status}\n`);
 if(test.status!==42||!output.includes('TASK_ASSERT '))throw Error('not rejected by a content assertion: '+name);
 console.log('MUTANT_CONTENT_REJECTED '+name);
 }finally{rmSync(dir,{recursive:true,force:true});}}
