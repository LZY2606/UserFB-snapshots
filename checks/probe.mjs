import * as bidi from '../src/index.js';
for(const s of ['a אב  ג','אב😀ג','\u202e😀A\u202c','𐤀A']) {
 const e=bidi.getEmbeddingLevels(s,'ltr');
 console.log(JSON.stringify({s,levels:[...e.levels],paragraphs:e.paragraphs,indices:bidi.getReorderedIndices(s,e),visual:bidi.getReorderedString(s,e)}));
 if(s==='a אב  ג')console.log('line2..5',bidi.getReorderedIndices(s,e,2,5));
}
