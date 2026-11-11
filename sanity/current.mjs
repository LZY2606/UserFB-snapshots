import assert from 'node:assert/strict';
import preprocess from './src/preprocessor/index.js';
import { parse } from './src/index.js';
const simple = preprocess('#define F(x) ((x) * 2.0)\nfloat value = F(3.0);\n');
assert.equal(simple.trim(), 'float value = ((3.0) * 2.0);');
assert.equal(parse(simple, { quiet: true }).type, 'program');
const conditional = preprocess('#define Q 3\n#if defined(Q) && Q > 1\nfloat value = 2.0;\n#else\nfloat value = 0.0;\n#endif\n');
assert.equal(conditional.trim(), 'float value = 2.0;');
const alias = preprocess('#define B(x) (x + 1)\n#define A B\nA(2)\n').trim();
const recursive = preprocess('#define foo(x) bar(x)\n#define bar(x) foo(x)\nbar(foo(1.0))\n').trim();
console.log('observed-alias='+alias+'; expected=(2 + 1)');
console.log('observed-recursive='+recursive+'; expected=bar(foo(1.0))');
console.log('GLSL_BASELINE_OK expanded-and-parsed=2');
if (process.argv[2] === '--repro') {
 assert.equal(alias, '(2 + 1)');
 assert.equal(recursive, 'bar(foo(1.0))');
}
