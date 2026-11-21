require('./build.cjs');
const assert=require('node:assert/strict');
const {Readable}=require('node:stream');
const {parse,parseSync,stringifySync}=require('../.task-build/index.js');
const srt='1\n00:00:01,000 --> 00:00:02,500\n字幕🙂\n';
const nodes=parseSync(srt);
assert.equal(nodes.length,1);assert.equal(nodes[0].data.start,1000);
assert.equal(nodes[0].data.text,'字幕🙂');
assert.equal(stringifySync(nodes,{format:'SRT'}),srt);
const vtt='\ufeffWEBVTT\r\n\r\n007\r\n00:00:01.000 --> 00:00:02.500 align:start\r\n字幕🙂';
const vnodes=parseSync(vtt);
const output=stringifySync(vnodes,{format:'WebVTT'});
console.log('NORMAL_SRT_OK start=1000 end=2500 text=字幕🙂');
console.log('VTT_EXISTING_OBSERVATION '+JSON.stringify({inputBytes:Buffer.byteLength(vtt),outputBytes:Buffer.byteLength(output),identifier:vnodes.find(n=>n.type==='cue').data.identifier??null,identical:output===vtt}));
(async()=> {
  const got=[];
  await new Promise((resolve,reject)=> {
    Readable.from(Array.from(Buffer.from(srt),b=>Buffer.from([b]))).pipe(parse())
      .on('data',node=>got.push(node)).on('error',reject).on('end',resolve);
  });
  assert.deepEqual(got,nodes);
  console.log('STREAM_PRODUCTION_OK cues='+got.length);
})().catch(e=>{console.error(e);process.exitCode=1;});
