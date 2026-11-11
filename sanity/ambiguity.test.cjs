/**
 * Sanity tests for `fa.analyzeAmbiguity`.
 *
 * Small benign inputs only. Cross-checks the analyzer against an
 * independent bounded AST derivation enumeration implemented below.
 */

'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');

const regexpTree = require('../src/regexp-tree');
const {fa} = regexpTree;

// ---------------------------------------------------------------------------
// Independent bounded derivation enumeration (test-only implementation).
// Works directly on the raw AST; groups are transparent, captures ignored.
// ---------------------------------------------------------------------------

function* derive(node, string, i, j) {
  switch (node.type) {
    case 'Char':
      if (j === i + 1 && string[i] === node.value) {
        yield 1;
      }
      return;
    case 'Group':
      yield* derive(node.expression, string, i, j);
      return;
    case 'Alternative': {
      if (node.expressions.length === 0) {
        if (i === j) {
          yield 1;
        }
        return;
      }
      yield* deriveSeq(node.expressions, string, i, j);
      return;
    }
    case 'Disjunction':
      yield* derive(node.left, string, i, j);
      yield* derive(node.right, string, i, j);
      return;
    case 'Repetition': {
      const kind = node.quantifier.kind;
      const min = kind === '+' ? 1 : 0;
      const max = kind === '?' ? 1 : Infinity;
      yield* deriveRep(node, string, i, j, min, max);
      return;
    }
    default:
      throw new Error(`enumerator does not support ${node.type}`);
  }
}

function* deriveSeq(items, string, i, j) {
  if (items.length === 0) {
    if (i === j) {
      yield 1;
    }
    return;
  }
  const [first, ...rest] = items;
  for (let k = i; k <= j; k++) {
    for (const _ of derive(first, string, i, k)) {
      for (const __ of deriveSeq(rest, string, k, j)) {
        yield 1;
      }
    }
  }
}

function* deriveRep(node, string, i, j, min, max) {
  if (min === 0 && i === j) {
    yield 1;
  }
  if (max === 0) {
    return;
  }
  for (let k = i + 1; k <= j; k++) {
    for (const _ of derive(node.expression, string, i, k)) {
      for (const __ of deriveRep(
        node,
        string,
        k,
        j,
        Math.max(0, min - 1),
        max === Infinity ? Infinity : max - 1
      )) {
        yield 1;
      }
    }
  }
}

function countDerivations(ast, string, cap) {
  let count = 0;
  for (const _ of derive(ast.body, string, 0, string.length)) {
    if (++count >= cap) {
      break;
    }
  }
  return count;
}

/**
 * Shortest, then lexicographically smallest, string (over `alphabet`,
 * up to `maxLength`) having at least two distinct derivations.
 */
function bruteForceWitness(ast, alphabet, maxLength) {
  let level = [''];
  for (let length = 0; length <= maxLength; length++) {
    const next = [];
    for (const string of level) {
      if (countDerivations(ast, string, 2) >= 2) {
        return string;
      }
      for (const char of alphabet) {
        next.push(string + char);
      }
    }
    level = next;
  }
  return null;
}

// ---------------------------------------------------------------------------
// Derivation report validation helpers.
// ---------------------------------------------------------------------------

/**
 * Asserts a reported derivation tree consistently consumes
 * `witness[i..j)`, checking spans, chars, choices and counts.
 */
function assertValidDerivation(derivation, witness, i, j) {
  assert.deepStrictEqual(derivation.span, [i, j]);
  assert.equal(typeof derivation.path, 'string');
  switch (derivation.type) {
    case 'Char':
      assert.equal(j, i + 1);
      assert.equal(witness[i], derivation.value);
      return;
    case 'Alternative': {
      let at = i;
      for (const child of derivation.children) {
        assertValidDerivation(child, witness, at, child.span[1]);
        at = child.span[1];
      }
      assert.equal(at, j);
      return;
    }
    case 'Disjunction':
      assert.ok(derivation.choice === 'left' || derivation.choice === 'right');
      assertValidDerivation(derivation.child, witness, i, j);
      return;
    case 'Repetition': {
      assert.ok(['*', '+', '?'].includes(derivation.quantifier));
      assert.equal(derivation.count, derivation.instances.length);
      if (derivation.quantifier === '+') {
        assert.ok(derivation.count >= 1);
      }
      if (derivation.quantifier === '?') {
        assert.ok(derivation.count <= 1);
      }
      let at = i;
      for (const instance of derivation.instances) {
        assertValidDerivation(instance, witness, at, instance.span[1]);
        at = instance.span[1];
      }
      assert.equal(at, j);
      return;
    }
    default:
      assert.fail(`unexpected derivation node ${derivation.type}`);
  }
}

/**
 * Structural signature of a derivation: only real choices (disjunction
 * sides, repetition instance counts), ignoring grouping shells and
 * node identities.
 */
function choiceSignature(derivation) {
  switch (derivation.type) {
    case 'Char':
      return ['char'];
    case 'Alternative':
      return ['seq', ...derivation.children.map(choiceSignature)];
    case 'Disjunction':
      return ['alt', derivation.choice, choiceSignature(derivation.child)];
    case 'Repetition':
      return [
        'rep',
        derivation.count,
        ...derivation.instances.map(choiceSignature),
      ];
    default:
      assert.fail(`unexpected derivation node ${derivation.type}`);
  }
}

function assertAmbiguousReport(report, witness) {
  assert.equal(report.status, 'ambiguous');
  assert.equal(report.witness, witness);
  assert.equal(report.derivations.length, 2);
  const [first, second] = report.derivations;
  // Both derivations consume the whole witness.
  assertValidDerivation(first, witness, 0, witness.length);
  assertValidDerivation(second, witness, 0, witness.length);
  // ...and they differ in a real branch choice or repetition split.
  assert.notDeepStrictEqual(choiceSignature(first), choiceSignature(second));
  // Reports are JSON-serializable.
  assert.deepStrictEqual(JSON.parse(JSON.stringify(report)), report);
}

// ---------------------------------------------------------------------------
// Tests.
// ---------------------------------------------------------------------------

test('repeated branch: /a|a/ derives "a" two ways', () => {
  const report = fa.analyzeAmbiguity('/a|a/');
  assertAmbiguousReport(report, 'a');

  const choices = report.derivations.map(d => d.choice).sort();
  assert.deepStrictEqual(choices, ['left', 'right']);
  for (const derivation of report.derivations) {
    assert.equal(derivation.type, 'Disjunction');
    assert.equal(derivation.path, 'body');
    assert.equal(derivation.child.type, 'Char');
    assert.equal(derivation.child.value, 'a');
  }

  // Cross-check against the independent bounded enumeration.
  const ast = regexpTree.parse('/a|a/');
  assert.equal(bruteForceWitness(ast, ['a'], 2), 'a');
});

test('repetition split: /(a|aa)+/ splits "aa" two ways', () => {
  const report = fa.analyzeAmbiguity('/(a|aa)+/');
  assertAmbiguousReport(report, 'aa');

  const counts = report.derivations.map(d => d.count).sort();
  assert.deepStrictEqual(counts, [1, 2]);

  // One instance of two chars: the "aa" branch, consuming [0, 2).
  const single = report.derivations.find(d => d.count === 1);
  assert.equal(single.instances[0].choice, 'right');
  assert.deepStrictEqual(single.instances[0].span, [0, 2]);

  // Two instances of one char: the "a" branch twice, [0, 1) and [1, 2).
  const double = report.derivations.find(d => d.count === 2);
  assert.deepStrictEqual(
    double.instances.map(instance => instance.choice),
    ['left', 'left']
  );
  assert.deepStrictEqual(
    double.instances.map(instance => instance.span),
    [[0, 1], [1, 2]]
  );

  // Node identities map back to AST positions.
  assert.equal(single.path, 'body');
  assert.equal(single.loc.source, '(a|aa)+');
  assert.equal(single.instances[0].path, 'body.expression.expression');

  // Cross-check against the independent bounded enumeration.
  const ast = regexpTree.parse('/(a|aa)+/');
  assert.equal(bruteForceWitness(ast, ['a'], 4), 'aa');
});

test('genuinely unambiguous expressions report unambiguous', () => {
  // A back edge alone is not ambiguity.
  assert.deepStrictEqual(fa.analyzeAmbiguity('/(a|b)+/'), {status: 'unambiguous'});
  assert.deepStrictEqual(fa.analyzeAmbiguity('/ab(c|de)f/'), {status: 'unambiguous'});
  assert.deepStrictEqual(fa.analyzeAmbiguity('/a?b(c|d)+e/'), {status: 'unambiguous'});
  assert.deepStrictEqual(fa.analyzeAmbiguity('/(a|b)*a(a|b)(a|b)/'), {
    status: 'unambiguous',
  });

  // The bounded enumeration agrees there is no short ambiguous string.
  const ast = regexpTree.parse('/(a|b)+/');
  assert.equal(bruteForceWitness(ast, ['a', 'b'], 5), null);
});

test('shortest witness, lexicographic tie-break, and beyond enumeration', () => {
  // Both "zz" and "aa" are ambiguous; the lexicographically smaller
  // shortest witness wins.
  assertAmbiguousReport(fa.analyzeAmbiguity('/(zz|zz)|(aa|aa)/'), 'aa');
  assertAmbiguousReport(fa.analyzeAmbiguity('/(ba|ba)|(ab|ab)/'), 'ab');

  // The shortest witness here is longer than the bounded enumeration
  // below, so the result cannot come from brute-force enumeration.
  const source = '/abcdefgh|abcdefgh/';
  const report = fa.analyzeAmbiguity(source);
  assertAmbiguousReport(report, 'abcdefgh');
  const ast = regexpTree.parse(source);
  assert.equal(bruteForceWitness(ast, ['a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'], 6), null);
});

test('unsupported constructs report reason and source range', () => {
  const cases = [
    ['/a{2,3}/', 'range-quantifier', 'Repetition'],
    ['/[ab]+/', 'character-class', 'CharacterClass'],
    [/a/i, 'unsupported-flags', 'RegExp'],
    ['/(a*)+/', 'nullable-quantifier-operand', 'Repetition'],
    ['/(a?)?/', 'nullable-quantifier-operand', 'Repetition'],
    ['/\\ba/', 'assertion', 'Assertion'],
    ['/(a)\\1/', 'backreference', 'Backreference'],
    ['/a|/', 'empty-disjunction-branch', 'Disjunction'],
    ['/./', 'non-simple-char', 'Char'],
    ['/a-/', 'non-ascii-alphanumeric-char', 'Char'],
  ];
  for (const [regexp, reason, nodeType] of cases) {
    const report = fa.analyzeAmbiguity(regexp);
    assert.equal(report.status, 'unsupported', `${regexp}`);
    assert.equal(report.reason, reason, `${regexp}`);
    assert.equal(report.node.type, nodeType, `${regexp}`);
    assert.equal(typeof report.node.path, 'string', `${regexp}`);
    assert.equal(typeof report.message, 'string', `${regexp}`);
    // Parsed from a string/RegExp: source range is available.
    assert.ok(report.node.loc, `${regexp}`);
    assert.equal(typeof report.node.loc.start.offset, 'number', `${regexp}`);
    assert.ok(report.node.loc.end.offset > report.node.loc.start.offset, `${regexp}`);
    assert.deepStrictEqual(JSON.parse(JSON.stringify(report)), report);
  }

  // AST without locations: still analyzed, located by structural path.
  const ast = regexpTree.parse('/a{2}/');
  const report = fa.analyzeAmbiguity(ast);
  assert.equal(report.status, 'unsupported');
  assert.equal(report.reason, 'range-quantifier');
  assert.equal(report.node.loc, null);
  assert.equal(report.node.path, 'body');
});

test('caller isolation: AST untouched, reports independent and stable', () => {
  // AST without locations.
  const ast = regexpTree.parse('/(a|aa)+/');
  const snapshot = JSON.stringify(ast);

  // Pollute shared state: build automata and analyze other regexps first.
  fa.toNFA('/x|x/');
  fa.toDFA('/y|y/');
  fa.analyzeAmbiguity('/q|q/');

  const first = fa.analyzeAmbiguity(ast);
  const second = fa.analyzeAmbiguity(ast);

  // The caller's AST was not written.
  assert.equal(JSON.stringify(ast), snapshot);

  // Stable across calls, but not the same object.
  assert.notStrictEqual(first, second);
  assert.deepStrictEqual(first, second);

  // Missing locations fall back to stable structural paths.
  assert.equal(first.derivations[0].loc, null);
  assert.equal(first.derivations[0].path, 'body');

  // Mutating one report does not affect another, or future reports.
  first.witness = 'MUTATED';
  first.derivations[0].span[0] = 99;
  first.derivations[0].instances.length = 0;
  assert.equal(second.witness, 'aa');
  assert.deepStrictEqual(second.derivations[0].span, [0, 2]);
  const third = fa.analyzeAmbiguity(ast);
  assert.deepStrictEqual(third, second);

  // Result does not depend on analysis order or prior analyses.
  fa.analyzeAmbiguity('/z|z/');
  fa.toDFA('/(w|w)+/');
  assert.deepStrictEqual(fa.analyzeAmbiguity(ast), second);

  // JSON round-trip.
  assert.deepStrictEqual(JSON.parse(JSON.stringify(second)), second);
});
