'use strict';

// Null-prototype provenance tests for `security/detect-object-injection`.
//
// Run with: node --test test/harness/null-prototype.test.cjs

const assert = require('node:assert/strict');
const { test } = require('node:test');
const { Linter } = require('eslint');
const rule = require('../../rules/detect-object-injection');

const RULE_ID = 'security/detect-object-injection';
const GENERIC = 'Generic Object Injection Sink';
const VARIABLE = 'Variable Assigned to Object Injection Sink';
const FUNCTION_CALL = 'Function Call Object Injection Sink';

/**
 * Lint a snippet with the rule. `options` may be `undefined` (rule enabled
 * without options, i.e. the default), or an options object.
 * @param {string} code
 * @param {{allowNullPrototype?: boolean} | undefined} options
 * @param {Linter} [linter]
 * @returns {import('eslint').Linter.LintMessage[]}
 */
function lint(code, options, linter = new Linter()) {
  const ruleEntry =
    options === undefined ? 'error' : ['error', options];
  const config = [
    {
      plugins: { security: { rules: { 'detect-object-injection': rule } } },
      rules: { [RULE_ID]: ruleEntry },
      languageOptions: { ecmaVersion: 'latest', sourceType: 'module' },
    },
  ];
  return linter.verify(code, config);
}

/**
 * Locate the 1-based line/column of the nth occurrence of a snippet.
 * Member expressions are reported at the start of their receiver.
 * @param {string} code
 * @param {string} text
 * @param {number} [occurrence]
 * @returns {{line: number, column: number}}
 */
function positionOf(code, text, occurrence = 0) {
  let index = -1;
  for (let count = 0; count <= occurrence; count++) {
    index = code.indexOf(text, index + 1);
  }
  assert.notEqual(index, -1, `snippet not found: ${text}`);
  const before = code.slice(0, index);
  const lastNewline = before.lastIndexOf('\n');
  return { line: before.split('\n').length, column: index - lastNewline };
}

/**
 * Assert the complete report set: count, messages, and line/column.
 * @param {string} code
 * @param {{allowNullPrototype?: boolean} | undefined} options
 * @param {Array<{text: string, message: string, occurrence?: number}>} expected
 */
function expectReports(code, options, expected) {
  const messages = lint(code, options);
  assert.equal(
    messages.length,
    expected.length,
    `expected ${expected.length} report(s), got ${messages.length}: ${JSON.stringify(messages)}`,
  );
  for (let index = 0; index < expected.length; index++) {
    const message = messages[index];
    const entry = expected[index];
    const position = positionOf(code, entry.text, entry.occurrence || 0);
    assert.equal(message.message, entry.message, `message ${index}`);
    assert.equal(message.line, position.line, `line ${index}`);
    assert.equal(message.column, position.column, `column ${index}`);
  }
}

test('provenance', async t => {
  await t.test('option is off by default and reports a null-prototype dictionary', () => {
    const code = ['const dict = Object.create(null);', 'dict[key] = 1;'].join('\n');
    expectReports(code, undefined, [{ text: 'dict[key]', message: GENERIC }]);
    expectReports(code, { allowNullPrototype: false }, [{ text: 'dict[key]', message: GENERIC }]);
    expectReports(code, { allowNullPrototype: true }, []);
  });

  await t.test('declarations, plain assignment, parentheses, and arrow function scopes carry provenance', () => {
    const code = [
      'const base = (Object.create(null));',
      'let alias = ((base));',
      'alias[key] = 1;',
      'const choose = flag => {',
      '  const local = ((Object.create(null)));',
      '  local[other] = 2;',
      '};',
    ].join('\n');
    expectReports(code, { allowNullPrototype: true }, []);
  });

  await t.test('ordinary objects, parameters, property loads, and unknown returns stay reportable', () => {
    const code = [
      'const plain = {};',
      'plain[key] = 1;',
      'function parameter(value) {',
      '  value[key] = 2;',
      '}',
      'const dict = Object.create(null);',
      'const viaProperty = dict.child;',
      'viaProperty[key] = 3;',
      'const viaUnknown = make();',
      'viaUnknown[key] = 4;',
      'const outer = Object.create(null);',
      'function useOuter() {',
      '  outer[key] = 5;',
      '}',
    ].join('\n');
    expectReports(code, { allowNullPrototype: true }, [
      { text: 'plain[key]', message: GENERIC },
      { text: 'value[key]', message: GENERIC },
      { text: 'viaProperty[key]', message: GENERIC },
      { text: 'viaUnknown[key]', message: GENERIC },
      { text: 'outer[key]', message: GENERIC },
    ]);
  });

  await t.test('conditional expressions are safe only when both allocation points are pristine', () => {
    const safe = [
      'function pick(flag) {',
      '  const chosen = flag ? Object.create(null) : Object.create(null);',
      '  chosen[key] = 1;',
      '}',
    ].join('\n');
    expectReports(safe, { allowNullPrototype: true }, []);

    const mixed = [
      'function pick(flag) {',
      '  const chosen = flag ? Object.create(null) : {};',
      '  chosen[key] = 1;',
      '}',
    ].join('\n');
    expectReports(mixed, { allowNullPrototype: true }, [{ text: 'chosen[key]', message: GENERIC }]);

    const taintedBranch = [
      'function pick(flag) {',
      '  const first = Object.create(null);',
      '  const second = Object.create(null);',
      '  const chosen = flag ? first : second;',
      '  Object.setPrototypeOf(second, {});',
      '  chosen[key] = 1;',
      '}',
    ].join('\n');
    expectReports(taintedBranch, { allowNullPrototype: true }, [{ text: 'chosen[key]', message: GENERIC }]);
  });

  await t.test('block-scoped same-name bindings are independent', () => {
    const code = [
      'const shared = Object.create(null);',
      '{',
      '  const shared = {};',
      '  shared[key] = 1;',
      '}',
      'shared[key] = 2;',
    ].join('\n');
    expectReports(code, { allowNullPrototype: true }, [{ text: 'shared[key]', message: GENERIC }]);
  });

  await t.test('shadowed Object factories and prototype operations are not trusted', () => {
    const code = [
      'function withParameter(Object) {',
      '  const dict = Object.create(null);',
      '  dict[key] = 1;',
      '}',
      'const Object = { create: () => ({}) };',
      'const dict2 = Object.create(null);',
      'dict2[key] = 2;',
    ].join('\n');
    expectReports(code, { allowNullPrototype: true }, [
      { text: 'dict[key]', message: GENERIC },
      { text: 'dict2[key]', message: GENERIC },
    ]);
  });

  await t.test('the three existing messages are unchanged for unproven receivers', () => {
    const code = [
      'const plain = {};',
      'const read = plain[key];',
      'consume(plain[key]);',
      'plain[key] = 1;',
      'const dict = Object.create(null);',
      'dict[key]();',
    ].join('\n');
    expectReports(code, { allowNullPrototype: true }, [
      { text: 'plain[key]', occurrence: 0, message: VARIABLE },
      { text: 'plain[key]', occurrence: 1, message: FUNCTION_CALL },
      { text: 'plain[key]', occurrence: 2, message: GENERIC },
      { text: 'dict[key]', message: FUNCTION_CALL },
    ]);
  });
});

test('alias-invalidation', async t => {
  await t.test('Object.setPrototypeOf through an alias invalidates every alias', () => {
    const code = [
      'const first = Object.create(null);',
      'const second = first;',
      'Object.setPrototypeOf(first, {});',
      'first[key] = 1;',
      'second[key] = 2;',
    ].join('\n');
    expectReports(code, { allowNullPrototype: true }, [
      { text: 'first[key]', message: GENERIC },
      { text: 'second[key]', message: GENERIC },
    ]);
  });

  await t.test('__proto__ writes invalidate aliases', () => {
    const code = [
      'const first = Object.create(null);',
      'const second = first;',
      'second.__proto__ = {};',
      'first[key] = 1;',
    ].join('\n');
    expectReports(code, { allowNullPrototype: true }, [{ text: 'first[key]', message: GENERIC }]);
  });

  await t.test('calls, method receivers, external writes, and closures invalidate proofs', () => {
    const code = [
      'const viaCall = Object.create(null);',
      'sink(viaCall);',
      'viaCall[key] = 1;',
      'const viaMethod = Object.create(null);',
      'viaMethod.run();',
      'viaMethod[key] = 2;',
      'const viaWrite = Object.create(null);',
      'holder.data = viaWrite;',
      'viaWrite[key] = 3;',
      'const viaCapture = Object.create(null);',
      'function capture() { return viaCapture; }',
      'viaCapture[key] = 4;',
    ].join('\n');
    expectReports(code, { allowNullPrototype: true }, [
      { text: 'viaCall[key]', message: GENERIC },
      { text: 'viaMethod[key]', message: GENERIC },
      { text: 'viaWrite[key]', message: GENERIC },
      { text: 'viaCapture[key]', message: GENERIC },
    ]);
  });

  await t.test('rebinding one alias to an ordinary object does not affect the other alias', () => {
    const code = [
      'const original = Object.create(null);',
      'let alias = original;',
      'alias = {};',
      'alias[key] = 1;',
      'original[key] = 2;',
    ].join('\n');
    expectReports(code, { allowNullPrototype: true }, [{ text: 'alias[key]', message: GENERIC }]);
  });

  await t.test('rebinding to a fresh dictionary proves the new binding but not the old alias', () => {
    const code = [
      'let old = Object.create(null);',
      'let alias = old;',
      'Object.setPrototypeOf(old, {});',
      'alias[key] = 1;',
      'old[key] = 2;',
      'old = Object.create(null);',
      'old[key] = 3;',
      'alias[key] = 4;',
    ].join('\n');
    expectReports(code, { allowNullPrototype: true }, [
      { text: 'alias[key]', occurrence: 0, message: GENERIC },
      { text: 'old[key]', occurrence: 0, message: GENERIC },
      { text: 'alias[key]', occurrence: 1, message: GENERIC },
    ]);
  });

  await t.test('storing an ordinary object in a dictionary does not prove the stored object', () => {
    const code = [
      'const dict = Object.create(null);',
      'dict.child = {};',
      'const child = dict.child;',
      'child[key] = 1;',
    ].join('\n');
    expectReports(code, { allowNullPrototype: true }, [{ text: 'child[key]', message: GENERIC }]);
  });

  await t.test('a direct allocation in a declaration is not itself an escape', () => {
    const code = ['const dict = Object.create(null);', 'dict[key] = 1;'].join('\n');
    expectReports(code, { allowNullPrototype: true }, []);
  });
});

test('control-flow', async t => {
  await t.test('if/else successors are proven only when every reaching branch is proven', () => {
    const bothSafe = [
      'function read(flag) {',
      '  const dict = Object.create(null);',
      '  if (flag) {',
      '    let left = dict;',
      '    left[key] = 1;',
      '  } else {',
      '    let right = dict;',
      '    right[key] = 2;',
      '  }',
      '}',
    ].join('\n');
    expectReports(bothSafe, { allowNullPrototype: true }, []);

    const oneUnsafe = [
      'function read(flag) {',
      '  if (flag) {',
      '    const dict = Object.create(null);',
      '    dict[key] = 1;',
      '  } else {',
      '    const dict = {};',
      '    dict[key] = 2;',
      '  }',
      '}',
    ].join('\n');
    expectReports(oneUnsafe, { allowNullPrototype: true }, [{ text: 'dict[key]', occurrence: 1, message: GENERIC }]);
  });

  await t.test('branches that return or throw do not participate in the successor', () => {
    const returns = [
      'function read(flag) {',
      '  const dict = Object.create(null);',
      '  if (flag) {',
      '    Object.setPrototypeOf(dict, {});',
      '    return;',
      '  }',
      '  dict[key] = 1;',
      '}',
    ].join('\n');
    expectReports(returns, { allowNullPrototype: true }, []);

    const throws = [
      'function read(flag) {',
      '  const dict = Object.create(null);',
      '  if (flag) {',
      '    dict = {};',
      '    throw new Error("stop");',
      '  }',
      '  dict[key] = 1;',
      '}',
    ].join('\n');
    expectReports(throws, { allowNullPrototype: true }, []);
  });

  await t.test('loop and try bodies cannot reuse stale proofs', () => {
    const loopReassign = [
      'function read(more) {',
      '  let dict = Object.create(null);',
      '  while (more()) {',
      '    dict = {};',
      '  }',
      '  dict[key] = 1;',
      '}',
    ].join('\n');
    expectReports(loopReassign, { allowNullPrototype: true }, [{ text: 'dict[key]', message: GENERIC }]);

    const invalidatedBeforeLoop = [
      'function read(limit) {',
      '  const dict = Object.create(null);',
      '  Object.setPrototypeOf(dict, {});',
      '  for (let index = 0; index < limit; index++) {',
      '    dict[key] = index;',
      '  }',
      '}',
    ].join('\n');
    expectReports(invalidatedBeforeLoop, { allowNullPrototype: true }, [{ text: 'dict[key]', message: GENERIC }]);

    const safeTry = [
      'function read() {',
      '  const dict = Object.create(null);',
      '  try {',
      '    dict[key] = 1;',
      '  } catch (error) {',
      '    return;',
      '  }',
      '  dict[key] = 2;',
      '}',
    ].join('\n');
    expectReports(safeTry, { allowNullPrototype: true }, []);
  });

  await t.test('an unknown call in one branch only invalidates its own receivers', () => {
    const code = [
      'function read(flag) {',
      '  const escaped = Object.create(null);',
      '  if (flag) {',
      '    unknown(escaped);',
      '  }',
      '  const fresh = Object.create(null);',
      '  fresh[key] = 1;',
      '  escaped[key] = 2;',
      '}',
    ].join('\n');
    expectReports(code, { allowNullPrototype: true }, [{ text: 'escaped[key]', message: GENERIC }]);
  });
});

test('isolation', async t => {
  await t.test('repeated analyses on one Linter instance do not leak state', () => {
    const safeCode = ['const dict = Object.create(null);', 'dict[key] = 1;'].join('\n');
    const unsafeCode = ['const dict = {};', 'dict[key] = 1;'].join('\n');
    const linter = new Linter();

    assert.equal(lint(safeCode, { allowNullPrototype: true }, linter).length, 0);
    assert.equal(lint(unsafeCode, { allowNullPrototype: true }, linter).length, 1);
    assert.equal(lint(safeCode, { allowNullPrototype: true }, linter).length, 0);

    // Invalidating one run must not contaminate the next one.
    const taintingCode = [
      'const dict = Object.create(null);',
      'Object.setPrototypeOf(dict, {});',
      'dict[key] = 1;',
    ].join('\n');
    assert.equal(lint(taintingCode, { allowNullPrototype: true }, linter).length, 1);
    assert.equal(lint(safeCode, { allowNullPrototype: true }, linter).length, 0);
  });

  await t.test('separate Linter instances agree', () => {
    const code = ['const dict = Object.create(null);', 'dict[key] = 1;'].join('\n');
    assert.equal(lint(code, { allowNullPrototype: true }).length, 0);
    assert.equal(lint(code, { allowNullPrototype: true }).length, 0);
  });

  await t.test('toggling the rule on and off does not reuse previous decisions', () => {
    const code = ['const dict = Object.create(null);', 'dict[key] = 1;'].join('\n');
    const linter = new Linter();
    assert.equal(lint(code, { allowNullPrototype: false }, linter).length, 1);
    assert.equal(lint(code, { allowNullPrototype: true }, linter).length, 0);
    assert.equal(lint(code, { allowNullPrototype: false }, linter).length, 1);
    assert.equal(lint(code, undefined, linter).length, 1);
  });

  await t.test('same-name variables in separate function scopes stay independent', () => {
    const code = [
      'function safe() {',
      '  const value = Object.create(null);',
      '  value[key] = 1;',
      '}',
      'function unsafe() {',
      '  const value = {};',
      '  value[key] = 2;',
      '}',
    ].join('\n');
    expectReports(code, { allowNullPrototype: true }, [{ text: 'value[key]', occurrence: 1, message: GENERIC }]);
  });
});
