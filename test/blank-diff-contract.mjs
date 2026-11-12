// Contract tests for Store#alignDiff, the blank-node-aware change report.
// Run through the harness with the source ESM loader:
//   node --import ./harness/register.mjs harness/target.mjs
import assert from 'node:assert/strict';
import { test } from 'node:test';
import F, { termToId } from '../src/N3DataFactory.js';
import Store from '../src/N3Store.js';

const { namedNode: n, blankNode: b, literal: l, variable: v, quad: q, defaultGraph: dg } = F;
const POSITIONS = ['subject', 'predicate', 'object', 'graph'];

// JSON encoding of the four termToId values; the mandated sort key.
function keyOf(quad) {
  return JSON.stringify(POSITIONS.map(position => termToId(quad[position])));
}

// True when the quad sets of two stores coincide, independent of blank labels.
function sameQuadSet(store, quads) {
  const actual = new Set(store.getQuads().map(keyOf));
  const expected = new Set(quads.map(keyOf));
  return actual.size === expected.size && [...expected].every(key => actual.has(key));
}

function utf8Less(left, right) {
  return Buffer.compare(Buffer.from(left), Buffer.from(right)) < 0;
}

// Independent exhaustive reference: tries every injective partial mapping and
// applies the same three-level objective, sharing no search code with the store.
// Only practical for the small graphs used below; it proves optimality there.
function bruteForceAlign(beforeQuads, afterQuads) {
  function blankIdsOf(quads) {
    const ids = new Set();
    for (const quad of quads)
      for (const position of POSITIONS) {
        const term = quad[position];
        if (term.termType === 'BlankNode')
          ids.add(termToId(term));
      }
    return [...ids].sort((a, b) => Buffer.compare(Buffer.from(a), Buffer.from(b)));
  }
  const oldIds = blankIdsOf(beforeQuads);
  const newIds = blankIdsOf(afterQuads);
  const afterKeys = new Set(afterQuads.map(keyOf));
  const newTerms = new Map();
  for (const quad of afterQuads)
    for (const position of POSITIONS) {
      const term = quad[position];
      if (term.termType === 'BlankNode')
        newTerms.set(termToId(term), term);
    }

  const map = new Map();
  const used = new Set();
  let best = null;

  function mappingKey() {
    return [...map.entries()]
      .sort((a, b) => Buffer.compare(Buffer.from(a[0]), Buffer.from(b[0])));
  }
  function keyLess(left, right) {
    const length = Math.min(left.length, right.length);
    for (let i = 0; i < length; i++) {
      if (left[i][0] !== right[i][0])
        return utf8Less(left[i][0], right[i][0]);
      if (left[i][1] !== right[i][1])
        return utf8Less(left[i][1], right[i][1]);
    }
    return left.length < right.length;
  }
  function retainedUnder() {
    let count = 0;
    for (const quad of beforeQuads) {
      const parts = POSITIONS.map(position => {
        const term = quad[position];
        if (term.termType !== 'BlankNode')
          return term;
        const newId = map.get(termToId(term));
        return newId === undefined ? null : newTerms.get(newId);
      });
      if (parts.every(Boolean) && afterKeys.has(JSON.stringify(parts.map(termToId))))
        count++;
    }
    return count;
  }

  (function enumerate(index) {
    if (index === oldIds.length) {
      const retained = retainedUnder();
      const key = mappingKey();
      if (best === null || retained > best.retained ||
          (retained === best.retained && map.size < best.mapped) ||
          (retained === best.retained && map.size === best.mapped && keyLess(key, best.key)))
        best = { retained, mapped: map.size, key };
      return;
    }
    const oldId = oldIds[index];
    enumerate(index + 1); // leave this blank unmapped
    for (const newId of newIds) {
      if (!used.has(newId)) {
        used.add(newId);
        map.set(oldId, newId);
        enumerate(index + 1);
        map.delete(oldId);
        used.delete(newId);
      }
    }
  })(0);
  return best;
}

// Asserts the store result matches the exhaustive reference on every objective.
function assertOptimal(beforeQuads, otherQuads) {
  const before = new Store(beforeQuads);
  const other = new Store(otherQuads);
  const result = before.alignDiff(other);
  const expected = bruteForceAlign(before.getQuads(), other.getQuads());
  assert.equal(result.retained.length, expected.retained);
  assert.equal(result.mapping.length, expected.mapped);
  assert.deepEqual(
    result.mapping.map(([oldBlank, newBlank]) => [termToId(oldBlank), termToId(newBlank)]),
    expected.key,
  );
  assertWellFormed(result, before, other);
}

// Checks every structural guarantee shared by the result of any alignDiff call.
function assertWellFormed(result, before, other) {
  assert.ok(Array.isArray(result.mapping));
  assert.ok(Array.isArray(result.retained));
  assert.ok(Array.isArray(result.removed));
  assert.ok(Array.isArray(result.added));
  assert.ok(result.stats && Number.isInteger(result.stats.states) && result.stats.states >= 1);

  // The mapping is injective in both directions and only pairs blank nodes.
  const oldLabels = new Set();
  const newLabels = new Set();
  for (const pair of result.mapping) {
    assert.equal(pair.length, 2);
    assert.equal(pair[0].termType, 'BlankNode');
    assert.equal(pair[1].termType, 'BlankNode');
    const oldKey = termToId(pair[0]);
    const newKey = termToId(pair[1]);
    assert.equal(oldLabels.has(oldKey), false);
    assert.equal(newLabels.has(newKey), false);
    oldLabels.add(oldKey);
    newLabels.add(newKey);
  }
  // Mapping entries are sorted by the old blank label in UTF-8 byte order.
  for (let i = 1; i < result.mapping.length; i++)
    assert.ok(Buffer.compare(
      Buffer.from(termToId(result.mapping[i - 1][0])),
      Buffer.from(termToId(result.mapping[i][0])),
    ) < 0);

  const mapping = new Map(result.mapping.map(([oldBlank, newBlank]) =>
    [termToId(oldBlank), termToId(newBlank)]));

  // Each retained pair survives the substitution term by term in every position.
  const retainedBefore = new Set();
  const retainedAfter = new Set();
  for (const { before: oldQuad, after: newQuad } of result.retained) {
    for (const position of POSITIONS) {
      const oldTerm = oldQuad[position];
      const newTerm = newQuad[position];
      if (oldTerm.termType === 'BlankNode') {
        assert.equal(mapping.has(termToId(oldTerm)), true,
          'retained quad contains an unmapped blank node');
        assert.equal(mapping.get(termToId(oldTerm)), termToId(newTerm));
      }
      else
        assert.ok(oldTerm.equals(newTerm), `fixed term differs at ${position}`);
    }
    retainedBefore.add(keyOf(oldQuad));
    retainedAfter.add(keyOf(newQuad));
  }

  // Retained entries sort by the before key and then the after key.
  const retainedKeys = result.retained.map(({ before: oldQuad, after: newQuad }) =>
    [keyOf(oldQuad), keyOf(newQuad)]);
  for (let i = 1; i < retainedKeys.length; i++) {
    const previous = retainedKeys[i - 1];
    const current = retainedKeys[i];
    assert.ok(Buffer.compare(Buffer.from(previous[0]), Buffer.from(current[0])) <= 0);
    if (previous[0] === current[0])
      assert.ok(Buffer.compare(Buffer.from(previous[1]), Buffer.from(current[1])) <= 0);
  }

  // Removed and added are sorted by their quad key.
  for (const list of [result.removed, result.added]) {
    const keys = list.map(keyOf);
    for (let i = 1; i < keys.length; i++)
      assert.ok(Buffer.compare(Buffer.from(keys[i - 1]), Buffer.from(keys[i])) <= 0);
  }

  // Each partition covers its input exactly once, without overlap.
  const removedBefore = new Set(result.removed.map(keyOf));
  const addedAfter = new Set(result.added.map(keyOf));
  assert.equal(retainedBefore.size, result.retained.length);
  assert.equal(retainedAfter.size, result.retained.length);
  assert.equal(removedBefore.size, result.removed.length);
  assert.equal(addedAfter.size, result.added.length);
  for (const key of retainedBefore)
    assert.equal(removedBefore.has(key), false);
  for (const key of retainedAfter)
    assert.equal(addedAfter.has(key), false);

  const beforeKeys = new Set(before.getQuads().map(keyOf));
  const otherKeys = new Set(other.getQuads().map(keyOf));
  assert.equal(beforeKeys.size, retainedBefore.size + removedBefore.size);
  for (const key of beforeKeys)
    assert.ok(retainedBefore.has(key) || removedBefore.has(key));
  assert.equal(otherKeys.size, retainedAfter.size + addedAfter.size);
  for (const key of otherKeys)
    assert.ok(retainedAfter.has(key) || addedAfter.has(key));

  // retained.after plus added rebuilds a store exactly equal to other.
  const rebuilt = new Store();
  for (const { after } of result.retained)
    rebuilt.add(after);
  for (const quad of result.added)
    rebuilt.add(quad);
  assert.ok(rebuilt.equals(other));
  assert.ok(other.equals(rebuilt));
}

test('blank alignment', () => {
  // A ring with a single content change: the five link quads are retained
  // through a single consistent renaming; only the changed attribute moves.
  const link = n('urn:link');
  const leftQuads = [];
  const rightQuads = [];
  for (let i = 0; i < 5; i++) {
    leftQuads.push(q(b(`a${i}`), link, b(`a${(i + 1) % 5}`)));
    rightQuads.push(q(b(`c${i}`), link, b(`c${(i + 1) % 5}`)));
  }
  leftQuads.push(q(b('a0'), n('urn:attr'), l('x')));
  rightQuads.push(q(b('c0'), n('urn:attr'), l('changed')));
  const before = new Store(leftQuads);
  const other = new Store(rightQuads);
  const result = before.alignDiff(other);

  assertWellFormed(result, before, other);
  assert.equal(result.retained.length, 5);
  assert.equal(result.removed.length, 1);
  assert.equal(result.added.length, 1);
  assert.equal(result.mapping.length, 5);
  assert.deepEqual(result.mapping.map(([oldBlank, newBlank]) => [oldBlank.value, newBlank.value]),
    [['a0', 'c0'], ['a1', 'c1'], ['a2', 'c2'], ['a3', 'c3'], ['a4', 'c4']]);
  assert.ok(result.removed[0].object.equals(l('x')));
  assert.ok(result.added[0].object.equals(l('changed')));

  // A blank node acting as both graph and subject keeps a single identity.
  const sameRole = new Store([
    q(b('g1'), n('urn:p'), l('in blank graph'), b('g1')),
    q(b('g1'), n('urn:q'), b('g1'), b('g1')),
  ]);
  const sameRoleRenamed = new Store([
    q(b('h7'), n('urn:p'), l('in blank graph'), b('h7')),
    q(b('h7'), n('urn:q'), b('h7'), b('h7')),
  ]);
  const roleResult = sameRole.alignDiff(sameRoleRenamed);
  assertWellFormed(roleResult, sameRole, sameRoleRenamed);
  assert.equal(roleResult.retained.length, 2);
  assert.equal(roleResult.mapping.length, 1);
  assert.equal(roleResult.mapping[0][0].value, 'g1');
  assert.equal(roleResult.mapping[0][1].value, 'h7');

  // Symmetric subgraph: several mappings score equally, so the byte-order
  // tie-break on the full old/new pair sequence decides deterministically.
  const symmetric = new Store([
    q(b('a'), n('urn:p'), l('same')),
    q(b('b'), n('urn:p'), l('same')),
  ]);
  const symmetricRenamed = new Store([
    q(b('x'), n('urn:p'), l('same')),
    q(b('y'), n('urn:p'), l('same')),
  ]);
  const symmetricResult = symmetric.alignDiff(symmetricRenamed);
  assertWellFormed(symmetricResult, symmetric, symmetricRenamed);
  assert.equal(symmetricResult.retained.length, 2);
  assert.deepEqual(
    symmetricResult.mapping.map(([oldBlank, newBlank]) => [oldBlank.value, newBlank.value]),
    [['a', 'x'], ['b', 'y']],
  );

  // Equal labels must not be assumed identical: structurally distinct blanks
  // sharing a label are aligned by structure, not by their name.
  const sameLabelLeft = new Store([q(b('shared'), n('urn:left'), l('only here'))]);
  const sameLabelRight = new Store([q(b('shared'), n('urn:right'), l('only there'))]);
  const sameLabelResult = sameLabelLeft.alignDiff(sameLabelRight);
  assertWellFormed(sameLabelResult, sameLabelLeft, sameLabelRight);
  assert.equal(sameLabelResult.retained.length, 0);
  assert.equal(sameLabelResult.removed.length, 1);
  assert.equal(sameLabelResult.added.length, 1);
  assert.equal(sameLabelResult.mapping.length, 0);

  // Blanks that cannot participate in any retained quad stay unmapped.
  const stray = new Store([q(b('lonely'), n('urn:p'), l('v'))]);
  const noMatch = new Store([q(b('other'), n('urn:p'), l('w'))]);
  const strayResult = stray.alignDiff(noMatch);
  assertWellFormed(strayResult, stray, noMatch);
  assert.equal(strayResult.mapping.length, 0);
  assert.equal(strayResult.retained.length, 0);
});

test('optimal changes', () => {
  // An association where per-node local similarity would pair the wrong blanks;
  // only the globally consistent mapping retains all three quads.
  const before = new Store([
    q(b('a'), n('urn:p'), l('shared-value')),
    q(b('a'), n('urn:r'), b('b')),
    q(b('b'), n('urn:p'), l('shared-value')),
  ]);
  // Both new blanks carry the same literal locally; the r-arc decides who is who.
  const other = new Store([
    q(b('x'), n('urn:p'), l('shared-value')),
    q(b('x'), n('urn:r'), b('y')),
    q(b('y'), n('urn:p'), l('shared-value')),
  ]);
  const result = before.alignDiff(other);
  assertWellFormed(result, before, other);
  assert.equal(result.retained.length, 3);
  assert.deepEqual(result.mapping.map(([oldBlank, newBlank]) => [oldBlank.value, newBlank.value]),
    [['a', 'x'], ['b', 'y']]);

  // Minimizing mapped nodes: an extra mapping retains nothing, so it is dropped.
  const sparseBefore = new Store([q(b('a'), n('urn:p'), l('v'))]);
  const sparseOther = new Store([q(b('x'), n('urn:p'), l('v')), q(b('y'), n('urn:z'), l('w'))]);
  const sparse = sparseBefore.alignDiff(sparseOther);
  assertWellFormed(sparse, sparseBefore, sparseOther);
  assert.equal(sparse.retained.length, 1);
  assert.equal(sparse.mapping.length, 1);
  assert.equal(sparse.mapping[0][1].value, 'x');
  assert.equal(sparse.added.length, 1);

  // Independent chains with all labels replaced: the benchmark case must stay
  // far below the full-permutation space while retaining every quad.
  const chainsBefore = new Store();
  const chainsAfter = new Store();
  const predicate = n('urn:p');
  for (let i = 0; i < 80; i++) {
    chainsBefore.add(q(b(`old${i}`), predicate, l(`value-${i}`)));
    chainsAfter.add(q(b(`new${i}`), predicate, l(`value-${i}`)));
  }
  const chains = chainsBefore.alignDiff(chainsAfter);
  assertWellFormed(chains, chainsBefore, chainsAfter);
  assert.equal(chains.retained.length, 80);
  assert.equal(chains.removed.length, 0);
  assert.equal(chains.added.length, 0);
  assert.equal(chains.mapping.length, 80);
  assert.ok(chains.stats.states < 10000,
    `expected pruning, but expanded ${chains.stats.states} states`);

  // Duplicate quads are treated as a set: repeated input changes nothing.
  const duplicateQuad = q(b('a'), n('urn:p'), l('v'), n('urn:g'));
  const duplicated = new Store([duplicateQuad, duplicateQuad, duplicateQuad]);
  const duplicatedOther = new Store([q(b('z'), n('urn:p'), l('v'), n('urn:g'))]);
  const deduped = duplicated.alignDiff(duplicatedOther);
  assertWellFormed(deduped, duplicated, duplicatedOther);
  assert.equal(deduped.retained.length, 1);
  assert.equal(duplicated.size, 1);

  // Fixed-only quads are retained without needing any blank mapping.
  const fixed = new Store([q(n('urn:s'), n('urn:p'), l('v'), n('urn:g'))]);
  const fixedResult = fixed.alignDiff(new Store([q(n('urn:s'), n('urn:p'), l('v'), n('urn:g'))]));
  assertWellFormed(fixedResult, fixed, fixed);
  assert.equal(fixedResult.retained.length, 1);
  assert.equal(fixedResult.mapping.length, 0);

  // Different named graphs never merge, even with identical terms inside.
  const graphA = new Store([q(b('a'), n('urn:p'), l('v'), n('urn:g1'))]);
  const graphB = new Store([q(b('a'), n('urn:p'), l('v'), n('urn:g2'))]);
  const graphResult = graphA.alignDiff(graphB);
  assertWellFormed(graphResult, graphA, graphB);
  assert.equal(graphResult.retained.length, 0);
  assert.equal(graphResult.removed.length, 1);
  assert.equal(graphResult.added.length, 1);

  // Small random graphs must match the independent exhaustive reference,
  // including the tie-breaking rules. Deterministic seed keeps the run stable.
  let seed = 0x5eed1234;
  function random() {
    seed = (seed * 1664525 + 1013904223) >>> 0;
    return seed / 4294967296;
  }
  const integerType = n('http://www.w3.org/2001/XMLSchema#integer');
  function randomQuads() {
    const blanks = [b('r0'), b('r1'), b('r2'), b('r3')];
    const predicates = [n('urn:p1'), n('urn:p2'), n('urn:p3')];
    const fixed = [n('urn:s'), n('urn:o'), l('a'), l('b', 'en'), l('7', integerType)];
    const graphs = [dg(), n('urn:g1'), b('r1')];
    function pickTerm() {
      return random() < 0.45 ?
        blanks[Math.floor(random() * blanks.length)] :
        fixed[Math.floor(random() * fixed.length)];
    }
    const quads = [];
    for (let i = 0; i < 6; i++) {
      let subject = pickTerm();
      if (subject.termType === 'Literal')
        subject = n('urn:s');
      quads.push(q(
        subject,
        predicates[Math.floor(random() * predicates.length)],
        pickTerm(),
        graphs[Math.floor(random() * graphs.length)],
      ));
    }
    return [...new Map(quads.map(quad => [keyOf(quad), quad])).values()];
  }
  function rename(quads) {
    const renamed = new Map();
    return quads.map(quad => {
      const parts = {};
      for (const position of POSITIONS) {
        const term = quad[position];
        if (term.termType === 'BlankNode') {
          const id = termToId(term);
          if (!renamed.has(id))
            renamed.set(id, b(`w${renamed.size}`));
          parts[position] = renamed.get(id);
        }
        else
          parts[position] = term;
      }
      return q(parts.subject, parts.predicate, parts.object, parts.graph);
    });
  }
  for (let trial = 0; trial < 300; trial++) {
    const left = randomQuads();
    const right = trial % 2 === 0 ? rename(left) : rename(randomQuads());
    assertOptimal(left, right);
  }
});

test('term boundaries', () => {
  // Language tags, their case, and datatypes participate in exact equality.
  const languageBefore = new Store([q(b('a'), n('urn:p'), l('hello', 'en'))]);
  const languageOther = new Store([q(b('z'), n('urn:p'), l('hello', 'fr'))]);
  const languageResult = languageBefore.alignDiff(languageOther);
  assertWellFormed(languageResult, languageBefore, languageOther);
  assert.equal(languageResult.retained.length, 0);

  const sameLanguage = languageBefore.alignDiff(
    new Store([q(b('z'), n('urn:p'), l('hello', 'EN'))]));
  assert.equal(sameLanguage.retained.length, 1);

  const integer = n('http://www.w3.org/2001/XMLSchema#integer');
  const doubleType = n('http://www.w3.org/2001/XMLSchema#double');
  const datatypeBefore = new Store([q(b('a'), n('urn:p'), l('1', integer))]);
  const datatypeOther = new Store([q(b('z'), n('urn:p'), l('1', doubleType))]);
  const datatypeResult = datatypeBefore.alignDiff(datatypeOther);
  assertWellFormed(datatypeResult, datatypeBefore, datatypeOther);
  assert.equal(datatypeResult.retained.length, 0);

  const sameDatatype = datatypeBefore.alignDiff(new Store([q(b('z'), n('urn:p'), l('1', integer))]));
  assert.equal(sameDatatype.retained.length, 1);

  // The default graph is an ordinary fixed term in the graph position.
  const defaultBefore = new Store([q(b('a'), n('urn:p'), l('v'), dg())]);
  const defaultOther = new Store([q(b('z'), n('urn:p'), l('v'), dg())]);
  const defaultResult = defaultBefore.alignDiff(defaultOther);
  assertWellFormed(defaultResult, defaultBefore, defaultOther);
  assert.equal(defaultResult.retained.length, 1);

  // Empty stores produce empty partitions and still report one visited state.
  const empty = new Store().alignDiff(new Store());
  assert.deepEqual(empty.mapping, []);
  assert.deepEqual(empty.retained, []);
  assert.deepEqual(empty.removed, []);
  assert.deepEqual(empty.added, []);
  assert.equal(empty.stats.states, 1);

  // A blank node used as the graph is aligned like any other blank position.
  const blankGraphBefore = new Store([q(n('urn:s'), n('urn:p'), l('v'), b('graph'))]);
  const blankGraphOther = new Store([q(n('urn:s'), n('urn:p'), l('v'), b('renamed'))]);
  const blankGraphResult = blankGraphBefore.alignDiff(blankGraphOther);
  assertWellFormed(blankGraphResult, blankGraphBefore, blankGraphOther);
  assert.equal(blankGraphResult.retained.length, 1);
  assert.equal(blankGraphResult.mapping[0][0].value, 'graph');
  assert.equal(blankGraphResult.mapping[0][1].value, 'renamed');

  // Variables are explicitly unsupported rather than stringified into nodes.
  assert.throws(
    () => new Store().alignDiff({
      [Symbol.iterator]: function* () { yield q(v('x'), n('urn:p'), l('v')); },
    }),
    /does not support terms of type "Variable"/,
  );
  assert.throws(
    () => new Store([q(b('a'), n('urn:p'), l('v'))]).alignDiff({
      [Symbol.iterator]: function* () { yield q(n('urn:s'), n('urn:p'), v('y')); },
    }),
    /Variable/,
  );

  // Quoted triples are out of scope for this RDF 1.1 alignment as well.
  const quoted = q(n('urn:s'), n('urn:p'), n('urn:o'));
  assert.throws(
    () => new Store().alignDiff({
      [Symbol.iterator]: function* () { yield q(quoted, n('urn:p'), l('v')); },
    }),
    /does not support terms of type "Quad"/,
  );
});

test('store isolation', () => {
  function build() {
    return [
      q(b('a'), n('urn:p'), b('b'), n('urn:g')),
      q(b('b'), n('urn:q'), l('text', 'en'), n('urn:g')),
      q(b('a'), n('urn:r'), l('1', n('http://www.w3.org/2001/XMLSchema#integer')), dg()),
    ];
  }
  const beforeQuads = build();
  const otherQuads = [
    q(b('x'), n('urn:p'), b('y'), n('urn:g')),
    q(b('y'), n('urn:q'), l('text', 'en'), n('urn:g')),
    q(b('x'), n('urn:r'), l('1', n('http://www.w3.org/2001/XMLSchema#integer')), dg()),
  ];

  // Insertion order must not influence the outcome.
  const beforeA = new Store(beforeQuads);
  const beforeB = new Store([...beforeQuads].reverse());
  const otherA = new Store(otherQuads);
  const otherB = new Store([...otherQuads].reverse());
  const resultA = beforeA.alignDiff(otherA);
  const resultB = beforeB.alignDiff(otherB);
  assertWellFormed(resultA, beforeA, otherA);
  assertWellFormed(resultB, beforeB, otherB);
  assert.deepEqual(
    resultA.mapping.map(([oldBlank, newBlank]) => [oldBlank.value, newBlank.value]),
    resultB.mapping.map(([oldBlank, newBlank]) => [oldBlank.value, newBlank.value]),
  );
  assert.deepEqual(resultA.retained.map(({ before: quad, after }) => [keyOf(quad), keyOf(after)]),
    resultB.retained.map(({ before: quad, after }) => [keyOf(quad), keyOf(after)]));
  assert.deepEqual(resultA.removed.map(keyOf), resultB.removed.map(keyOf));
  assert.deepEqual(resultA.added.map(keyOf), resultB.added.map(keyOf));
  assert.equal(resultA.stats.states, resultB.stats.states);

  // Sharing an entity index between stores must not change the alignment.
  const sharedIndex = beforeA._entityIndex;
  const sharedBefore = new Store(beforeQuads, { entityIndex: sharedIndex });
  const sharedOther = new Store(otherQuads, { entityIndex: sharedIndex });
  const sharedResult = sharedBefore.alignDiff(sharedOther);
  assertWellFormed(sharedResult, sharedBefore, sharedOther);
  assert.deepEqual(
    sharedResult.mapping.map(([oldBlank, newBlank]) => [oldBlank.value, newBlank.value]),
    resultA.mapping.map(([oldBlank, newBlank]) => [oldBlank.value, newBlank.value]),
  );

  // Inputs are untouched during and after the call.
  const beforeSnapshot = beforeA.getQuads().map(keyOf).sort();
  const otherSnapshot = otherA.getQuads().map(keyOf).sort();
  const beforeSize = beforeA.size;
  const otherSize = otherA.size;
  beforeA.alignDiff(otherA);
  assert.deepEqual(beforeA.getQuads().map(keyOf).sort(), beforeSnapshot);
  assert.deepEqual(otherA.getQuads().map(keyOf).sort(), otherSnapshot);
  assert.equal(beforeA.size, beforeSize);
  assert.equal(otherA.size, otherSize);

  // Existing semantics stay label-based and are untouched by the new method.
  assert.equal(beforeA.difference(otherA).size, beforeA.size);
  assert.equal(otherA.difference(beforeA).size, otherA.size);
  assert.equal(beforeA.equals(otherA), false);
  assert.equal(beforeA.match(null, n('urn:p'), null, n('urn:g')).size, 1);

  // The returned arrays are independent copies: editing them changes nothing.
  const result = beforeA.alignDiff(otherA);
  const retainedCopy = result.retained.length;
  result.mapping.pop();
  result.retained.pop();
  result.removed.push(beforeQuads[0]);
  result.added.push(otherQuads[0]);
  const again = beforeA.alignDiff(otherA);
  assert.equal(again.retained.length, retainedCopy);
  assert.equal(again.mapping.length, resultA.mapping.length);
  assert.deepEqual(beforeA.getQuads().map(keyOf).sort(), beforeSnapshot);
  assert.ok(sameQuadSet(otherA, otherQuads));

  // A second call on the same stores reproduces the first result exactly.
  assert.deepEqual(
    again.mapping.map(([oldBlank, newBlank]) => [termToId(oldBlank), termToId(newBlank)]),
    resultA.mapping.map(([oldBlank, newBlank]) => [termToId(oldBlank), termToId(newBlank)]),
  );
});
