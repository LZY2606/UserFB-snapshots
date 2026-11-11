const assert=require('node:assert/strict');
const micro=require('../src');
const prefix='<div itemscope itemtype="https://schema.org/Product">';
const shared='<div itemscope itemprop="seller brand" itemtype="https://schema.org/Organization"><span itemprop="name">ACME</span><span itemprop="name">ACME</span></div>';
for(const strict of [true,false]){
 const product=micro.toJson(prefix+shared+'</div>',{strict}).items[0];
 const seller=product.properties.seller[0];
 assert.equal(seller,product.properties.brand[0]);
 assert.deepEqual(seller.properties.name,['ACME','ACME']);
 assert.deepEqual(seller.type,['https://schema.org/Organization']);
}
const html='<div itemscope itemtype="https://a.test/Owner" itemref="shared shared"></div>'+
 '<div itemscope itemtype="https://b.test/Owner" itemref="shared"></div>'+
 '<span id="shared" itemscope itemprop="node"><span itemprop="name">N</span></span>';
const items=micro.toJson(html,{strict:true}).items;
assert.equal(items.length,2);
const first=items[0].properties.node[0],second=items[1].properties.node[0];
assert.equal(first,second);
assert.deepEqual(first.properties['https://a.test/name'],['N']);
assert.deepEqual(first.properties['https://b.test/name'],['N']);
const repeated='<div itemscope itemtype="https://schema.org/Thing"><span itemprop="name">X</span><span itemprop="name">X</span></div>';
assert.deepEqual(micro.toJson(repeated,{strict:true}).items[0].properties.name,['X','X']);
const cycle='<main itemscope itemtype="https://schema.org/Thing" itemref="a"></main><div id="a" itemscope itemprop="next" itemref="b"><span itemprop="name">A</span></div><div id="b" itemscope itemprop="next" itemref="a"></div>';
assert.throws(()=>micro.toJson(cycle,{strict:true}),/Cyclic structure/);
const again=micro.toJson(prefix+shared+'</div>',{strict:true}).items[0];
assert.deepEqual(again.properties.seller[0].properties.name,['ACME','ACME']);
console.log('microdata occurrence identity and inherited vocabulary checks passed');
