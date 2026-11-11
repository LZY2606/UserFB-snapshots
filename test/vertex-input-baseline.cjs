'use strict'
const fs = require('node:fs'), path = require('node:path'), vm = require('node:vm')
const modules = new Map()
function load(file) {
  file = path.resolve(file); if (modules.has(file)) return modules.get(file).exports
  const module = {exports:{}}; modules.set(file,module)
  const req = name => name === 'gl-format-compiler-error' ? (()=>{throw Error('unexpected compiler error')}) : load(path.resolve(path.dirname(file),name)+'.js')
  vm.runInThisContext('(function(require,module,exports){'+fs.readFileSync(file,'utf8')+'\n})',{filename:file})(req,module,module.exports)
  return module.exports
}
const enabled = new Set(), pointers = []
const gl = {VERTEX_SHADER:1,FRAGMENT_SHADER:2,COMPILE_STATUS:3,LINK_STATUS:4,ACTIVE_ATTRIBUTES:5,FLOAT:0x1406,
  createShader:()=>({}),shaderSource(){},compileShader(){},getShaderParameter:()=>true,isShader:()=>true,
  createProgram:()=>({}),attachShader(){},bindAttribLocation(){},linkProgram(){},isProgram:()=>true,
  getProgramParameter:(_,p)=>p===5?2:true,useProgram(){},
  enableVertexAttribArray:i=>enabled.add(i),disableVertexAttribArray:i=>enabled.delete(i),
  vertexAttribPointer:(...args)=>pointers.push(args),vertexAttrib3fv(){}}
const shader = load(path.join(__dirname,'../index.js'))(gl,'v','f',[],[
  {name:'position',type:'vec3',location:1},{name:'model',type:'mat3',location:4}])
shader.bind()
console.log('enabled after bind:',JSON.stringify([...enabled].sort()))
shader.attributes.model.pointer(gl.FLOAT,false)
console.log('matrix pointer [location,size,type,normalized,stride,offset]:',JSON.stringify(pointers))
if (JSON.stringify([...enabled].sort()) !== '[0,1,4,5,6]' || pointers[0][4] !== 9) throw Error('baseline changed; inspect behavior')
