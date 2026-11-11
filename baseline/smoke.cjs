'use strict';
const assert = require('node:assert/strict');
const {Linter} = require('eslint');
const plugin = require('../index');
const lint = code => new Linter().verify(code,[{plugins:{security:plugin},rules:{'security/detect-object-injection':'error'}}]);
assert.equal(lint('const m=Object.create(null); m[k]=1;').length,1);
assert.equal(lint('const m={}; m[k]=1;').length,1);
assert.equal(lint('const m={}; m.fixed=1;').length,0);
console.log('BASELINE_NULL_MAP=reported ORDINARY_MAP=reported STATIC_KEY=clean');
