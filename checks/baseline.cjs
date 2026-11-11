const assert = require('node:assert/strict')
const mesh = require('../index.js')
function area(m) {
  return m.cells.reduce((sum,c) => {
    const [a,b,d] = c.map(i=>m.positions[i])
    return sum+Math.abs((b[0]-a[0])*(d[1]-a[1])-(b[1]-a[1])*(d[0]-a[0]))/2
  },0)
}
const outer='M0 0H10V10H0Z'
const same=outer+'M2 2H8V8H2Z'
const reverse=outer+'M2 2V8H8V2Z'
const opt={normalize:false,randomization:0,simplify:0}
const a=area(mesh(outer,opt)), b=area(mesh(same,opt)), c=area(mesh(reverse,opt))
const ignored=area(mesh(same,{...opt,fillRule:'nonzero'}))
assert.equal(a,100); assert.equal(b,64); assert.equal(c,64); assert.equal(ignored,64)
console.log(`baseline outer=${a} same-direction=${b} opposite-direction=${c}; fillRule option ignored=${ignored}`)
