const assert = require('node:assert/strict');
const {Type} = require('../lib');
const writer = Type.forSchema({type:'record',name:'Event',fields:[
 {name:'id',type:'int'},{name:'payload',type:{type:'array',items:'string'}}]});
const reader = Type.forSchema({type:'record',name:'Event',fields:[
 {name:'payload',type:{type:'array',items:'string'}},
 {name:'id',type:'long'},{name:'source',type:'string',default:'offline'}]});
const b = writer.toBuffer({id:17,payload:['a','雪']});
const r = reader.createResolver(writer);
const v = reader.fromBuffer(b,r);
assert.deepEqual(v.payload,['a','雪']); assert.equal(v.id,17);
assert.equal(v.source,'offline');
assert.deepEqual(reader.fromBuffer(reader.toBuffer(v)),v);
assert.equal(writer.decode(b).offset,b.length);
assert.throws(()=>Type.forSchema('boolean').createResolver(writer));
console.log('AVSC_BASELINE_OK fields=3 items=2 resolver=compatible');
