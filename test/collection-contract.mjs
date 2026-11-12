// Regression contract for the RDFa collection topology.
//
// Scope:
//   RDFA_DIRECTION          - lazily completed rel/rev, through wrapper elements,
//                             plus explicit subject switching.
//   RDFA_LIST_SCOPE         - ordered inlist values, independent lists per subject,
//                             independent explicit-about inlist elements.
//   RDFA_LIST_MULTIPLICITY  - repeated resources / equal-valued literals keep every
//                             position, empty lists terminate in rdf:nil.
//   RDFA_SCOPE_TRANSFORMS   - transparent wrappers, prefix/CURIE renames and JS string
//                             splitting must not change the graph, via write/end and import.
//
// The expected graphs are hand-built from the short HTML snippets and the relations they
// declare. Graph comparison allows blank-node renaming but preserves shared structure:
// it is a whole-graph isomorphism check, never a sort over emitted blank-node labels and
// never per-triple renaming (both of which would hide topology errors).
//
// Only the public streaming RdfaParser interface is used; the parser's internal stack is
// never read. The compiled entry point is provided through CHECK_BUILD_DIR.

import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { Readable, PassThrough } from 'node:stream';
import test from 'node:test';

const require = createRequire(import.meta.url);
if (!process.env.CHECK_BUILD_DIR) {
  throw new Error('CHECK_BUILD_DIR must point at the compiled package directory');
}
const { RdfaParser } = require(`${process.env.CHECK_BUILD_DIR}/index.js`);
const { DataFactory } = require('rdf-data-factory');

const DF = new DataFactory();
const RDF = 'http://www.w3.org/1999/02/22-rdf-syntax-ns#';
const XSD = 'http://www.w3.org/2001/XMLSchema#';
const RDF_FIRST = DF.namedNode(`${RDF}first`);
const RDF_REST = DF.namedNode(`${RDF}rest`);
const RDF_NIL = DF.namedNode(`${RDF}nil`);
const BASE = 'https://e.test/';

const nn = iri => DF.namedNode(iri);
const lit = (value, language, datatype) => DF.literal(value, language || datatype || DF.namedNode(`${XSD}string`));
const quad = (s, p, o, g) => DF.quad(s, p, o, g || DF.defaultGraph());

// ---------------------------------------------------------------------------
// Parsing entry points (public stream API only)
// ---------------------------------------------------------------------------

function newParser() {
  return new RdfaParser({ baseIRI: BASE, profile: 'html' });
}

async function parseWithWriteEnd(html, chunkSize) {
  const parser = newParser();
  const quads = [];
  parser.on('data', q => quads.push(q));
  await new Promise((resolve, reject) => {
    parser.on('end', resolve);
    parser.on('error', reject);
    if (chunkSize === undefined) {
      parser.end(html);
    } else {
      for (let i = 0; i < html.length; i += chunkSize) {
        parser.write(html.slice(i, i + chunkSize));
      }
      parser.end();
    }
  });
  return quads;
}

async function parseWithImport(html, chunkSize) {
  const parser = newParser();
  const chunks = [];
  for (let i = 0; i < html.length; i += chunkSize) {
    chunks.push(html.slice(i, i + chunkSize));
  }
  const input = Readable.from(chunks);
  const output = parser.import(input);
  const quads = [];
  output.on('data', q => quads.push(q));
  await new Promise((resolve, reject) => {
    output.on('end', resolve);
    output.on('error', reject);
  });
  return quads;
}

// Split a JS string at arbitrary positions, deliberately cutting through tag names and
// attribute values. This only splits JS strings; it does not touch binary UTF-8 decoding.
function splitString(html, size) {
  const chunks = [];
  for (let i = 0; i < html.length; i += size) {
    chunks.push(html.slice(i, i + size));
  }
  return chunks;
}

// ---------------------------------------------------------------------------
// Term and graph comparison
// ---------------------------------------------------------------------------

function isBlank(term) {
  return term.termType === 'BlankNode';
}

// Stable key for a ground term. Blank nodes return null: their identity is established
// through graph structure, not through their emitted label.
function groundKey(term) {
  if (isBlank(term)) {
    return null;
  }
  if (term.termType === 'Literal') {
    return `L\x1F${term.value}\x1F${term.language}\x1F${term.datatype.termType}:${term.datatype.value}`;
  }
  return `${term.termType}\x1F${term.value}`;
}

function tupleKey(quadTerm, blankMapping) {
  const keyFor = (term) => {
    if (isBlank(term)) {
      const mapped = blankMapping.get(term.value);
      if (mapped === undefined) {
        throw new Error(`unmapped blank node _:${term.value} while building tuple key`);
      }
      return `B\x1F${mapped}`;
    }
    return groundKey(term);
  };
  return [ keyFor(quadTerm.subject), groundKey(quadTerm.predicate), keyFor(quadTerm.object), groundKey(quadTerm.graph) ].join('\x1E');
}

function staticSignature(quads, blankId) {
  // Ground-anchored incident edges make a blank node distinguishable without relying on
  // its label: predicate, direction, and the ground neighbor it is connected to.
  // Blank-to-blank links are only counted by predicate and direction; the identity of the
  // neighboring blank is resolved later by backtracking over the whole graph.
  const anchored = [];
  const blankOut = {};
  const blankIn = {};
  for (const q of quads) {
    const sBlank = isBlank(q.subject) && q.subject.value === blankId;
    const oBlank = isBlank(q.object) && q.object.value === blankId;
    if (!sBlank && !oBlank) {
      continue;
    }
    if (sBlank) {
      if (isBlank(q.object)) {
        blankOut[q.predicate.value] = (blankOut[q.predicate.value] || 0) + 1;
      } else {
        anchored.push(`out ${q.predicate.value} ${groundKey(q.object)}`);
      }
    }
    if (oBlank) {
      if (isBlank(q.subject)) {
        blankIn[q.predicate.value] = (blankIn[q.predicate.value] || 0) + 1;
      } else {
        anchored.push(`in ${groundKey(q.subject)} ${q.predicate.value}`);
      }
    }
  }
  return JSON.stringify({
    anchored: anchored.sort(),
    blankOut: Object.fromEntries(Object.entries(blankOut).sort()),
    blankIn: Object.fromEntries(Object.entries(blankIn).sort()),
  });
}

function collectBlanks(quads) {
  const ids = new Set();
  for (const q of quads) {
    if (isBlank(q.subject)) {
      ids.add(q.subject.value);
    }
    if (isBlank(q.object)) {
      ids.add(q.object.value);
    }
    if (isBlank(q.graph)) {
      ids.add(q.graph.value);
    }
  }
  return [ ...ids ];
}

// Whole-graph isomorphism with blank-node renaming. A blank node may be renamed, but two
// blanks that play different structural roles can never be collapsed into one.
function findBlankMapping(actualQuads, expectedQuads) {
  const aBlanks = collectBlanks(actualQuads);
  const eBlanks = collectBlanks(expectedQuads);
  if (aBlanks.length !== eBlanks.length) {
    return { error: `blank node count differs: actual ${aBlanks.length}, expected ${eBlanks.length}` };
  }

  const expectedTuples = expectedQuads.map(q => tupleKey(q, new Map(eBlanks.map(id => [ id, id ])))).sort();
  const eSignatures = new Map(eBlanks.map(id => [ id, staticSignature(expectedQuads, id) ]));
  const candidates = new Map();
  for (const aId of aBlanks) {
    const sig = staticSignature(actualQuads, aId);
    const matches = eBlanks.filter(eId => eSignatures.get(eId) === sig);
    if (matches.length === 0) {
      return { error: `blank node _:${aId} has no structurally matching expected blank node` };
    }
    candidates.set(aId, matches);
  }

  // Most constrained blank first, so a wrong structural choice fails as early as possible.
  const order = [ ...aBlanks ].sort((x, y) => candidates.get(x).length - candidates.get(y).length);
  const mapping = new Map();
  const used = new Set();

  const complete = () => {
    const actualTuples = actualQuads.map(q => tupleKey(q, mapping)).sort();
    if (actualTuples.length !== expectedTuples.length) {
      return false;
    }
    for (const [ i, actualTuple ] of actualTuples.entries()) {
      if (actualTuple !== expectedTuples[i]) {
        return false;
      }
    }
    return true;
  };

  const search = (index) => {
    if (index === order.length) {
      return complete() ? mapping : null;
    }
    const aId = order[index];
    for (const eId of candidates.get(aId)) {
      if (used.has(eId)) {
        continue;
      }
      mapping.set(aId, eId);
      used.add(eId);
      if (search(index + 1)) {
        return mapping;
      }
      used.delete(eId);
      mapping.delete(aId);
    }
    return null;
  };

  const found = search(0);
  return found ?
      { mapping: found } :
      { error: 'no blank-node renaming makes the actual graph equal to the expected graph' };
}

// Assert a parsed graph equals a hand-built expected graph up to blank-node renaming.
function assertGraphEqual(actualQuads, expectedQuads, label) {
  const result = findBlankMapping(actualQuads, expectedQuads);
  if (result.error) {
    assert.fail(`${label}: ${result.error}\nactual:\n${formatQuads(actualQuads)}\nexpected:\n${formatQuads(expectedQuads)}`);
  }
}

function formatQuads(quads) {
  const show = term => isBlank(term) ?
`_:${term.value}` :
      (term.termType === 'Literal' ?
`"${term.value}"${term.language ? `@${term.language}` : ''}^^${term.datatype.value}` :
      `<${term.value}>`);
  return quads.map(q => `  ${show(q.subject)} ${show(q.predicate)} ${show(q.object)}`).join('\n');
}

// ---------------------------------------------------------------------------
// Targeted edge and list assertions (precise, indexed failure messages)
// ---------------------------------------------------------------------------

function edgeExists(quads, subject, predicate, object) {
  return quads.some(q =>
    q.subject.termType === subject.termType && q.subject.value === subject.value &&
    q.predicate.value === predicate.value &&
    q.object.termType === object.termType && q.object.value === object.value &&
    (object.termType !== 'Literal' ||
      (q.object.language === object.language &&
        q.object.datatype.termType === object.datatype.termType &&
        q.object.datatype.value === object.datatype.value)) &&
    q.graph.termType === 'DefaultGraph');
}

function assertEdge(quads, subject, predicate, object, edgeIndex) {
  if (!edgeExists(quads, subject, predicate, object)) {
    const show = term => term.termType === 'Literal' ? `"${term.value}"` : `<${term.value}>`;
    assert.fail(`edge #${edgeIndex} missing: ${show(subject)} <${predicate.value}> ${show(object)}`);
  }
}

function assertNoEdge(quads, subject, predicate, object, edgeIndex) {
  if (edgeExists(quads, subject, predicate, object)) {
    assert.fail(`edge #${edgeIndex} must not exist: <${subject.value}> <${predicate.value}> <${object.value}>`);
  }
}

// Follow a list from its head and verify every cell. Throws at the first bad cell position.
function assertList(quads, head, expectedValues, listLabel) {
  const seen = new Set();
  let current = head;
  for (const [ i, wanted ] of expectedValues.entries()) {
    if (current.termType !== 'BlankNode') {
      assert.fail(`${listLabel} cell #${i}: expected a blank-node cell, found ${current.termType}:${current.value}`);
    }
    if (seen.has(current.value)) {
      assert.fail(`${listLabel} cell #${i}: list cycles back to _:${current.value}`);
    }
    seen.add(current.value);

    const firsts = quads.filter(q => q.subject.value === current.value && q.predicate.equals(RDF_FIRST));
    const rests = quads.filter(q => q.subject.value === current.value && q.predicate.equals(RDF_REST));
    if (firsts.length !== 1) {
      assert.fail(`${listLabel} cell #${i}: expected exactly one rdf:first, found ${firsts.length}`);
    }
    if (rests.length !== 1) {
      assert.fail(`${listLabel} cell #${i}: expected exactly one rdf:rest, found ${rests.length}`);
    }
    const first = firsts[0].object;
    if (first.termType !== wanted.termType || first.value !== wanted.value ||
      (wanted.termType === 'Literal' &&
        (first.language !== wanted.language || first.datatype.value !== wanted.datatype.value))) {
      assert.fail(`${listLabel} cell #${i}: rdf:first expected ${formatTerm(wanted)}, got ${formatTerm(first)}`);
    }
    current = rests[0].object;
  }
  if (current.termType !== 'NamedNode' || current.value !== RDF_NIL.value) {
    assert.fail(`${listLabel} tail after cell #${expectedValues.length - 1}: expected rdf:nil, got ${formatTerm(current)}`);
  }
  return seen;
}

function formatTerm(term) {
  if (term.termType === 'Literal') {
    return `"${term.value}"${term.language ? `@${term.language}` : ''}^^${term.datatype.value}`;
  }
  return `${term.termType}:${term.value}`;
}

function listHeads(quads, subject, predicate) {
  return quads
    .filter(q => q.subject.equals(subject) && q.predicate.equals(predicate))
    .map(q => q.object);
}

// Every rdf:rest blank node must be a cell that itself has rdf:first/rdf:rest (no dangling).
function assertNoDanglingCells(quads, label) {
  const cells = new Set(quads.filter(q => q.predicate.equals(RDF_FIRST)).map(q => q.subject.value));
  let pos = 0;
  for (const q of quads.filter(q => q.predicate.equals(RDF_REST))) {
    if (isBlank(q.object) && !cells.has(q.object.value)) {
      assert.fail(`${label} rest position #${pos}: rdf:rest points at dangling _:${q.object.value} without rdf:first`);
    }
    pos++;
  }
}

// ---------------------------------------------------------------------------
// Test 1: delayed rel/rev completion and explicit subject switching
// ---------------------------------------------------------------------------

test('RDFA_DIRECTION', async(t) => {
  await t.test('pending rel and rev both complete through wrapper elements', async() => {
    const html = '<div about="https://e.test/s" rel="urn:fwd" rev="urn:bwd">' +
      '<section><span><b><a about="https://e.test/o"></a></b></span></section></div>';
    const actual = await parseWithWriteEnd(html);

    const s = nn('https://e.test/s');
    const o = nn('https://e.test/o');
    const fwd = nn('urn:fwd');
    const bwd = nn('urn:bwd');

    // Concrete subject/predicate/object checks; counts alone cannot prove direction.
    assertEdge(actual, s, fwd, o, 1);
    assertEdge(actual, o, bwd, s, 2);
    assert.equal(actual.length, 2, 'wrapper elements must not produce additional relations');
    for (const q of actual) {
      assert.equal(q.graph.termType, 'DefaultGraph', 'all edges must land in the default graph');
    }

    const expected = [
      quad(s, fwd, o),
      quad(o, bwd, s),
    ];
    assertGraphEqual(actual, expected, 'RDFA_DIRECTION pending rel/rev');
  });

  await t.test('an explicit about switch completes the pending edges at the switch', async() => {
    const html = '<div about="https://e.test/s1" rel="urn:fwd" rev="urn:bwd"><section>' +
      '<div about="https://e.test/mid"><span><a about="https://e.test/o"></a></span></div>' +
      '</section></div>';
    const actual = await parseWithWriteEnd(html);

    const s1 = nn('https://e.test/s1');
    const mid = nn('https://e.test/mid');
    const o = nn('https://e.test/o');
    const fwd = nn('urn:fwd');
    const bwd = nn('urn:bwd');

    // The first explicit new subject completes both pending relations; the deeper resource
    // must not be wired to the original subject.
    assertEdge(actual, s1, fwd, mid, 1);
    assertEdge(actual, mid, bwd, s1, 2);
    assertNoEdge(actual, s1, fwd, o, 3);
    assertNoEdge(actual, o, bwd, s1, 4);

    const expected = [
      quad(s1, fwd, mid),
      quad(mid, bwd, s1),
    ];
    assertGraphEqual(actual, expected, 'RDFA_DIRECTION explicit subject switch');
  });

  await t.test('the same pending edges are collected identically via write/end and import', async() => {
    const html = '<div about="https://e.test/s" rel="urn:fwd" rev="urn:bwd">' +
      '<section><span><a about="https://e.test/o"></a></span></section></div>';
    const viaWrite = await parseWithWriteEnd(html, 5);
    const viaImport = await parseWithImport(html, 5);
    const expected = [
      quad(nn('https://e.test/s'), nn('urn:fwd'), nn('https://e.test/o')),
      quad(nn('https://e.test/o'), nn('urn:bwd'), nn('https://e.test/s')),
    ];
    assertGraphEqual(viaWrite, expected, 'RDFA_DIRECTION write/end');
    assertGraphEqual(viaImport, expected, 'RDFA_DIRECTION import');
  });
});

// ---------------------------------------------------------------------------
// Test 2: ordered inlist scope and independent explicit-about lists
// ---------------------------------------------------------------------------

test('RDFA_LIST_SCOPE', async(t) => {
  const predicate = nn('urn:m');
  const html =
    '<div about="https://e.test/s1"><ol rel="urn:m" inlist="">' +
    '<li><a href="a"></a></li></ol></div>' +
    '<div about="https://e.test/s2"><ol rel="urn:m" inlist="">' +
    '<li><a href="b"></a></li></ol></div>' +
    '<div about="https://e.test/s3">' +
    '<span about="https://e.test/s3" rel="urn:m" inlist="" resource="https://e.test/x"></span>' +
    '<span about="https://e.test/s3" rel="urn:m" inlist="" resource="https://e.test/y"></span>' +
    '</div>';

  await t.test('the same predicate keeps independent lists on different subjects', async() => {
    const actual = await parseWithWriteEnd(html);
    const s1 = nn('https://e.test/s1');
    const s2 = nn('https://e.test/s2');
    const s3 = nn('https://e.test/s3');

    const heads1 = listHeads(actual, s1, predicate);
    const heads2 = listHeads(actual, s2, predicate);
    const heads3 = listHeads(actual, s3, predicate);
    assert.equal(heads1.length, 1, 'list position <https://e.test/s1> <urn:m>: subject must own exactly one list');
    assert.equal(heads2.length, 1, 'list position <https://e.test/s2> <urn:m>: subject must own exactly one list');
    assert.equal(heads3.length, 2, 'list position <https://e.test/s3> <urn:m>: two explicit-about inlist elements must stay two independent lists');

    assertList(actual, heads1[0], [ nn('https://e.test/a') ], 's1 list');
    assertList(actual, heads2[0], [ nn('https://e.test/b') ], 's2 list');

    // The two s3 lists are single-item lists with distinct cell blank nodes; a shared
    // subject IRI must never merge them into one two-item list.
    assert.equal(heads3[0].termType, 'BlankNode');
    assert.equal(heads3[1].termType, 'BlankNode');
    assert.notEqual(heads3[0].value, heads3[1].value, 's3 list heads must be distinct blank nodes');
    const cellsX = assertList(actual, heads3[0], [ nn('https://e.test/x') ], 's3 first list');
    const cellsY = assertList(actual, heads3[1], [ nn('https://e.test/y') ], 's3 second list');
    for (const cell of cellsX) {
      assert.equal(cellsY.has(cell), false, `s3 lists must not share cell _:${cell}`);
    }
    assertNoDanglingCells(actual, 'RDFA_LIST_SCOPE');

    // Independent hand-built expectation, one quad list per declared relation.
    const expected = [];
    const buildSingle = (subject, value) => {
      const cell = DF.blankNode();
      expected.push(quad(cell, RDF_FIRST, value));
      expected.push(quad(cell, RDF_REST, RDF_NIL));
      expected.push(quad(subject, predicate, cell));
    };
    buildSingle(s1, nn('https://e.test/a'));
    buildSingle(s2, nn('https://e.test/b'));
    buildSingle(s3, nn('https://e.test/x'));
    buildSingle(s3, nn('https://e.test/y'));
    assertGraphEqual(actual, expected, 'RDFA_LIST_SCOPE independent lists');
  });

  await t.test('chunked input through import preserves the same list scope', async() => {
    const viaImport = await parseWithImport(html, 11);
    const s3 = nn('https://e.test/s3');
    assert.equal(listHeads(viaImport, s3, predicate).length, 2, 'import entry must also retain two independent s3 lists');
    assertNoDanglingCells(viaImport, 'RDFA_LIST_SCOPE import');
  });
});

// ---------------------------------------------------------------------------
// Test 3: multiplicity, ordering, empty lists and literal term shape
// ---------------------------------------------------------------------------

test('RDFA_LIST_MULTIPLICITY', async(t) => {
  const predicate = nn('urn:m');

  await t.test('repeated resources and equal literals keep every list position', async() => {
    const html = '<div about="https://e.test/s">' +
      '<span rel="urn:m" inlist="" resource="https://e.test/r"></span>' +
      '<span rel="urn:m" inlist="" resource="https://e.test/r"></span>' +
      '<span property="urn:m" inlist="" content="v"></span>' +
      '<span property="urn:m" inlist="" content="v"></span>' +
      '</div>';
    const actual = await parseWithWriteEnd(html);
    const s = nn('https://e.test/s');
    const heads = listHeads(actual, s, predicate);
    assert.equal(heads.length, 1, 'all four occurrences feed one ordered list');

    const v = lit('v', '', DF.namedNode(`${XSD}string`));
    const cells = assertList(actual, heads[0], [
      nn('https://e.test/r'),
      nn('https://e.test/r'),
      v,
      v,
    ], 'repeat list');

    // Two equal occurrences mean two physical cells: no early term-equality de-duplication.
    assert.equal(cells.size, 4, 'four occurrences must produce four distinct cells');
    const firsts = actual.filter(q => q.predicate.equals(RDF_FIRST));
    assert.equal(firsts.filter(q => q.object.value === 'https://e.test/r').length, 2, 'the repeated IRI must occupy two rdf:first positions');
    assert.equal(firsts.filter(q => q.object.termType === 'Literal' && q.object.value === 'v').length, 2, 'the equal-valued literal must occupy two rdf:first positions');
    assertNoDanglingCells(actual, 'RDFA_LIST_MULTIPLICITY');

    const expected = [];
    const chain = [ nn('https://e.test/r'), nn('https://e.test/r'), v, v ];
    const nodes = chain.map(() => DF.blankNode());
    for (let i = 0; i < nodes.length; i++) {
      expected.push(quad(nodes[i], RDF_FIRST, chain[i]));
      expected.push(quad(nodes[i], RDF_REST, i < nodes.length - 1 ? nodes[i + 1] : RDF_NIL));
    }
    expected.push(quad(s, predicate, nodes[0]));
    assertGraphEqual(actual, expected, 'RDFA_LIST_MULTIPLICITY repeated values');
  });

  await t.test('an empty inlist maps directly to rdf:nil without cells', async() => {
    const html = '<div about="https://e.test/s"><span rel="urn:empty" inlist=""></span></div>';
    const actual = await parseWithWriteEnd(html);
    const s = nn('https://e.test/s');
    const empty = nn('urn:empty');
    assertEdge(actual, s, empty, RDF_NIL, 1);
    assert.equal(actual.filter(q => q.predicate.equals(RDF_FIRST)).length, 0, 'an empty list must not emit rdf:first cells');
    assert.equal(actual.filter(q => q.predicate.equals(RDF_REST)).length, 0, 'an empty list must not emit rdf:rest cells');
    assert.equal(actual.length, 1);
    assertGraphEqual(actual, [ quad(s, empty, RDF_NIL) ], 'RDFA_LIST_MULTIPLICITY empty list');
  });

  await t.test('language and datatype are preserved term-by-term inside list cells', async() => {
    const html = '<div about="https://e.test/s2">' +
      '<span property="urn:m2" inlist="" content="chat" xml:lang="en"></span>' +
      '<span property="urn:m2" inlist="" content="42" datatype="https://e.test/dt"></span></div>';
    const actual = await parseWithWriteEnd(html);
    const s2 = nn('https://e.test/s2');
    const p2 = nn('urn:m2');
    const heads = listHeads(actual, s2, p2);
    assert.equal(heads.length, 1);
    const tagged = DF.literal('chat', 'en');
    const typed = DF.literal('42', nn('https://e.test/dt'));
    assertList(actual, heads[0], [ tagged, typed ], 'typed list');

    // Explicit RDF/JS term field checks beyond value equality.
    const firsts = actual.filter(q => q.predicate.equals(RDF_FIRST)).map(q => q.object);
    assert.equal(firsts[0].termType, 'Literal');
    assert.equal(firsts[0].language, 'en');
    assert.equal(firsts[0].datatype.termType, 'NamedNode');
    assert.equal(firsts[0].datatype.value, 'http://www.w3.org/1999/02/22-rdf-syntax-ns#langString');
    assert.equal(firsts[1].language, '');
    assert.equal(firsts[1].datatype.value, 'https://e.test/dt');
    assertNoDanglingCells(actual, 'RDFA_LIST_MULTIPLICITY typed');
  });
});

// ---------------------------------------------------------------------------
// Test 4: equivalent transforms, both entry points, and parser isolation
// ---------------------------------------------------------------------------

test('RDFA_SCOPE_TRANSFORMS', async(t) => {
  // One compact page exercising a pending rel/rev pair and a two-item inlist.
  const htmlBase = '<html prefix="ex: https://e.test/vocab#"><body about="https://e.test/s">' +
    '<div rel="ex:knows" rev="ex:knownBy"><section><span><a about="https://e.test/o"></a></span></section></div>' +
    '<span rel="ex:items" inlist="" resource="https://e.test/one"></span>' +
    '<span rel="ex:items" inlist="" resource="https://e.test/two"></span>' +
    '</body></html>';

  // Transparent wrapper elements carry no RDFa attributes and must add no relations.
  const htmlWrapped = '<html prefix="ex: https://e.test/vocab#"><body about="https://e.test/s">' +
    '<section><div rel="ex:knows" rev="ex:knownBy"><div><span><b><a about="https://e.test/o"></a></b></span></div></div></section>' +
    '<div><span rel="ex:items" inlist="" resource="https://e.test/one"></span></div>' +
    '<div><span rel="ex:items" inlist="" resource="https://e.test/two"></span></div>' +
    '</body></html>';

  // Rename the prefix and rewrite every CURIE; the expanded IRIs must be identical.
  const htmlRenamed = '<html prefix="voc: https://e.test/vocab#"><body about="https://e.test/s">' +
    '<div rel="voc:knows" rev="voc:knownBy"><section><span><a about="https://e.test/o"></a></span></section></div>' +
    '<span rel="voc:items" inlist="" resource="https://e.test/one"></span>' +
    '<span rel="voc:items" inlist="" resource="https://e.test/two"></span>' +
    '</body></html>';

  // Hand-built expectation derived from the declared relations, not from the parser output.
  const s = nn('https://e.test/s');
  const o = nn('https://e.test/o');
  const knows = nn('https://e.test/vocab#knows');
  const knownBy = nn('https://e.test/vocab#knownBy');
  const items = nn('https://e.test/vocab#items');
  const cellOne = DF.blankNode();
  const cellTwo = DF.blankNode();
  const expected = [
    quad(s, knows, o),
    quad(o, knownBy, s),
    quad(cellOne, RDF_FIRST, nn('https://e.test/one')),
    quad(cellOne, RDF_REST, cellTwo),
    quad(cellTwo, RDF_FIRST, nn('https://e.test/two')),
    quad(cellTwo, RDF_REST, RDF_NIL),
    quad(s, items, cellOne),
  ];

  const variants = [
    [ 'base', htmlBase ],
    [ 'transparent wrappers', htmlWrapped ],
    [ 'renamed prefix and CURIEs', htmlRenamed ],
  ];

  await t.test('every variant matches the hand-built expectation through both entries', async() => {
    const graphs = [];
    for (const [ name, html ] of variants) {
      // Chunk size 3 cuts inside tag names, attribute names and attribute values.
      const chunks = splitString(html, 3);
      assert.ok(chunks.length > 5, 'the page must actually be split into many JS chunks');

      const viaWrite = await parseWithWriteEnd(html, 3);
      const viaImport = await parseWithImport(html, 3);
      assertGraphEqual(viaWrite, expected, `transform "${name}" via write/end`);
      assertGraphEqual(viaImport, expected, `transform "${name}" via import`);
      graphs.push([ name, viaWrite ], [ name, viaImport ]);
    }
    // Every transformed graph must also equal each other, not just each its own expectation.
    for (let i = 1; i < graphs.length; i++) {
      assertGraphEqual(graphs[i][1], graphs[0][1], `transform "${graphs[i][0]}" differs from "${graphs[0][0]}"`);
    }
  });

  await t.test('a fresh parser on a new page keeps no subject or list from the previous page', async() => {
    const firstPage = await parseWithWriteEnd(htmlBase);
    assertGraphEqual(firstPage, expected, 'first page baseline');

    const secondHtml = '<div about="https://e.test/other">' +
      '<span rel="urn:onlyHere" inlist="" resource="https://e.test/z"></span></div>';
    // Deliberately a brand-new parser instance, as required when switching pages.
    const secondPage = await parseWithWriteEnd(secondHtml);

    const other = nn('https://e.test/other');
    const onlyHere = nn('urn:onlyHere');
    const heads = listHeads(secondPage, other, onlyHere);
    assert.equal(heads.length, 1, 'the new page must start from an empty list context');
    assertList(secondPage, heads[0], [ nn('https://e.test/z') ], 'new page list');

    const leaked = secondPage.filter(q =>
      q.subject.value === s.value || q.object.value === o.value ||
      q.predicate.value === knows.value || q.predicate.value === items.value);
    assert.equal(leaked.length, 0, 'no subject, predicate or list cell of the previous page may survive');

    const secondExpected = [];
    const cell = DF.blankNode();
    secondExpected.push(quad(cell, RDF_FIRST, nn('https://e.test/z')));
    secondExpected.push(quad(cell, RDF_REST, RDF_NIL));
    secondExpected.push(quad(other, onlyHere, cell));
    assertGraphEqual(secondPage, secondExpected, 'fresh parser page isolation');
  });

  await t.test('write/end and import of an unsplit stream agree with chunked streams', async() => {
    const whole = await parseWithWriteEnd(htmlRenamed);
    const input = new PassThrough();
    const output = newParser().import(input);
    const imported = [];
    output.on('data', q => imported.push(q));
    const done = new Promise((resolve, reject) => {
      output.on('end', resolve);
      output.on('error', reject);
      input.end(htmlRenamed);
    });
    await done;
    assertGraphEqual(whole, expected, 'unsplit write/end');
    assertGraphEqual(imported, expected, 'unsplit import');
  });
});
