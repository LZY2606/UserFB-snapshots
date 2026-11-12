'use strict';
const assert = require('node:assert/strict');
const parse5 = require('parse5');
const xss = require('../lib');
const options = {whiteList: {img: ['title', 'src']}};
let calls = 0;
const ignored = xss('<img title="before">', Object.assign({}, options, {
  transformAttr: function () { calls++; return 'after'; }
}));
assert.equal(ignored, '<img title="before">');
assert.equal(calls, 0);
const dirty = '<img title="a\' data-demo=\'split">';
const clean = xss(dirty, Object.assign({}, options, {singleQuotedAttributeValue: true}));
const attrs = parse5.parseFragment(clean).childNodes[0].attrs;
assert.equal(attrs.length, 1);
assert.equal(attrs[0].name, 'title');
assert.equal(attrs[0].value, "a' data-demo='split");
assert.equal(xss('<img src="javascript:demo">', options), '<img src>');
assert.equal(xss('<img title="ordinary">', options), '<img title="ordinary">');
console.log('baseline: normal sanitizer OK; transformAttr ignored; existing single-quote boundary preserved');
