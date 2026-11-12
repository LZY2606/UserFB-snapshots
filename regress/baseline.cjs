const assert = require('node:assert/strict');
const { RdfaParser } = require(process.env.CHECK_BUILD_DIR + '/index.js');
const rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#';
async function parse(html) {
 const p=new RdfaParser({baseIRI:'https://e.test/', profile:'html'});
 const quads=[]; p.on('data', q=>quads.push(q));
 await new Promise((resolve,reject)=>{ p.on('end',resolve); p.on('error',reject); p.end(html); });
 return quads;
}
(async()=>{
 const direct=await parse('<div about="https://e.test/s" rel="urn:knows"><i><a about="https://e.test/o"></a></i></div>');
 assert(direct.some(q=>q.subject.value==='https://e.test/s' && q.predicate.value==='urn:knows' && q.object.value==='https://e.test/o'));
 const reverse=await parse('<div about="https://e.test/s" rev="urn:knows"><i><a about="https://e.test/o"></a></i></div>');
 assert(reverse.some(q=>q.subject.value==='https://e.test/o' && q.object.value==='https://e.test/s'));
 const list=await parse('<div about="https://e.test/s"><ol rel="urn:members" inlist=""><li><a href="one"></a></li><li><a href="two"></a></li></ol></div>');
 const cells=list.filter(q=>q.predicate.value===rdf+'first');
 assert.equal(cells.length,2); assert(cells.some(q=>q.object.value==='https://e.test/one'));
 assert.equal(list.filter(q=>q.predicate.value===rdf+'rest' && q.object.value===rdf+'nil').length,1);
 console.log('BASELINE_RDFA_CONTEXT_OK direct='+direct.length+' reverse='+reverse.length+' list='+list.length);
})().catch(e=>{console.error(e);process.exitCode=1;});
