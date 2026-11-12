import {run} from 'node:test';
import {fileURLToPath} from 'node:url';
const required=new Set(['stream_early_parse','octet_boundaries','transfer_decoders','source_ownership','source_errors']);
const passed=new Set();let output='',failed=false,summary;
const stream=run({files:[fileURLToPath(new URL('./stream-target.test.ts',import.meta.url))],execArgv:['--import',fileURLToPath(new URL('./offline-loader.mjs',import.meta.url))],concurrency:1});
for await(const event of stream){
 const d=event.data;
 if(event.type==='test:stdout' || event.type==='test:stderr'){output+=d.message;process.stdout.write(d.message);}
 if(event.type==='test:pass'){if(d.skip || d.todo)failed=true;else passed.add(d.name);}
 if(event.type==='test:fail'){failed=true;console.error(d.name,d.details?.error);}
 if(event.type==='test:summary')summary=d;
}
if(!summary || !summary.success || summary.counts.skipped || summary.counts.todo || summary.counts.cancelled)failed=true;
for(const name of required){if(!passed.has(name)||!output.includes('CHECK_CONTENT_CHECK:'+name)){console.error('missing executed content assertions:',name);failed=true;}}
if(failed)process.exitCode=1;else console.log('TARGET_STREAM_PIPELINE_OK cases=5');
