const assert = require('node:assert/strict');
const create = require('../vao');
function backend(native) {
 let current={offset:null}; const gl={ARRAY_BUFFER:34962,ELEMENT_ARRAY_BUFFER:34963,
 FLOAT:5126,UNSIGNED_SHORT:5123,MAX_VERTEX_ATTRIBS:34921,
 getParameter(){return 2;},getExtension(){return null;},
 bindBuffer(){},enableVertexAttribArray(){},disableVertexAttribArray(){},
 vertexAttribPointer(i,n,t,z,s,o){current.offset=o;},
 vertexAttrib2f(i,a,b){gl.constant=[a,b];},
 drawArrays(){},drawElements(){}};
 gl.state=()=>current;
 if(native) Object.assign(gl,{createVertexArray(){return {offset:null};},
 bindVertexArray(v){current=v||{};},deleteVertexArray(){}});
 return gl;
}
for(const native of [false,true]) {
 const gl=backend(native), descriptor={buffer:{bind(){}},size:2,offset:0};
 const vao=create(gl,[descriptor]); descriptor.offset=8; vao.bind();
 assert.equal(gl.state().offset,native?0:8);
 console.log(`${native?'native':'emulated'} offset after caller edit=${gl.state().offset}`);
 if(native) {
  const before=gl.state(); const b=create(gl,[{buffer:{bind(){}},size:2}]);
  vao.bind(); b.update([{buffer:{bind(){}},size:2,offset:4}]);
  assert.notEqual(gl.state(),before);
  console.log('native updating inactive B unbinds active A');
 }
}
