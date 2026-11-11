'use strict'
const fs=require('node:fs'),vm=require('node:vm'),path=require('node:path')
const moduleBox={exports:{}}
vm.runInThisContext('(function(require,module,exports){'+fs.readFileSync(path.join(__dirname,'../fbo.js'),'utf8')+'\n})')(
  name=>{if(name==='gl-texture2d')return ()=>{throw Error('texture path not used')};throw Error(name)},moduleBox,moduleBox.exports)
function context(limit) {
  const calls=[],bindings=new Map(),ext={MAX_COLOR_ATTACHMENTS_WEBGL:20,MAX_DRAW_BUFFERS_WEBGL:21,
    drawBuffersWEBGL:a=>calls.push(a.slice())}
  let serial=0
  const gl={FRAMEBUFFER_BINDING:1,RENDERBUFFER_BINDING:2,TEXTURE_BINDING_2D:3,MAX_RENDERBUFFER_SIZE:4,
    FRAMEBUFFER:5,RENDERBUFFER:6,TEXTURE_2D:7,COLOR_ATTACHMENT0:100,NONE:0,RGBA4:8,
    UNSIGNED_BYTE:9,FRAMEBUFFER_COMPLETE:10,
    getExtension:n=>n==='WEBGL_draw_buffers'?ext:null,
    getParameter:p=>p===20||p===21?limit:p===4?64:bindings.get(p)||null,
    createFramebuffer:()=>({id:++serial}),createRenderbuffer:()=>({id:++serial}),
    bindFramebuffer:(_,x)=>bindings.set(1,x),bindRenderbuffer:(_,x)=>bindings.set(2,x),
    bindTexture:(_,x)=>bindings.set(3,x),renderbufferStorage(){},framebufferRenderbuffer(){},
    checkFramebufferStatus:()=>10}
  return {gl,calls}
}
for (const limit of [4,2]) {
  const c=context(limit)
  moduleBox.exports(c.gl,2,2,{color:0,depth:false})
  console.log(JSON.stringify({limit,drawBuffers:c.calls}))
}
