import assert from 'node:assert/strict';
import {parse} from '../packages/csv-parse/lib/index.js';
import {parse as sync} from '../packages/csv-parse/lib/sync.js';
const input='id,text\n1,"a\nb"\n2,drop\n3,终\n';
const seen=[];
const parser=parse({columns:true,raw:true,info:true,on_record(row,info){
 seen.push([row.record.id,info.lines,info.records]);
 return row.record.id==='2'?null:row;
}});
const values=[];
const done=(async()=>{for await(const v of parser)values.push(v);})();
const data=Buffer.from(input);for(let i=0;i<data.length;i+=3)parser.write(data.subarray(i,i+3));parser.end();await done;
assert.deepEqual(values.map(x=>x.record),[{id:'1',text:'a\nb'},{id:'3',text:'终'}]);
assert.deepEqual(seen,[['1',3,1],['2',4,2],['3',5,3]]);
assert.equal(values[0].raw,'1,"a\nb"\n');
assert.equal(values[1].info.lines,5);
assert.deepEqual(sync('a,b\n1,2\n',{columns:true}),[{a:'1',b:'2'}]);
console.log('CSV_PARSE_SYNC_HOOK_OK rows=2 physical-lines=5 chunk-size=3');

assert.throws(()=>sync('ok,a,b\nbad,"x"oops,z\na,b,c\nc,d,e\n',{columns:['c1','c2','c3']}),e=>e.code==='CSV_INVALID_CLOSING_QUOTE');
console.log('CSV_STRICT_FAILURE_OK code=CSV_INVALID_CLOSING_QUOTE');
