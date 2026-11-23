import sax from '../vendor/sax/lib/sax.js';
import {fromParser} from '../src/main/js/doc.js';
import {generateISD} from '../src/main/js/isd.js';
export {generateISD};
export const ns='http://www.w3.org/ns/ttml#styling ';
export function parse(xml) {
 return fromParser(xml,{error:m=>{throw new Error(m)},fatal:m=>{throw new Error(m)}},null,sax.parser(true,{xmlns:true}));
}
export function wrap(body,head='') {
 return '<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling">'+head+'<body>'+body+'</body></tt>';
}
export function find(isd,id) {
 if(isd.id===id||isd.kind===id) return isd;
 for(const c of isd.contents||[]){const x=find(c,id);if(x)return x;}
 return null;
}
