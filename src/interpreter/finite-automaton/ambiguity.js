/**
 * The MIT License (MIT)
 * Copyright (c) 2017-present Dmitry Soshnikov <dmitry.soshnikov@gmail.com>
 */

'use strict';

const parser = require('../../parser');

/**
 * Static ambiguity analysis for regular expressions.
 *
 * The analysis works on a restricted, fully supported subset of the
 * regexp syntax (see `analyzeAmbiguity`). Anything outside of the
 * subset is reported as `unsupported` with a reason and the source
 * range (or a stable structural path) of the offending node.
 *
 * Approach: the expression is normalized (groups are transparent),
 * validated, and compiled to a Thompson-style NFA whose runs are in
 * 1-to-1 correspondence with derivations (parse trees). Since every
 * quantifier operand is non-nullable, the epsilon subgraph is a DAG.
 * Runs are then compared through a "macro" automaton whose steps are
 * single character edges (with leading epsilon segments collapsed):
 *
 *   - case (A): two runs differ in their character-edge sequence.
 *     Decided on the synchronized product of the macro automaton,
 *     tracking whether the two runs have genuinely diverged. No
 *     epsilon moves exist at this level, so traversal scheduling can
 *     never be mistaken for a distinct derivation.
 *
 *   - case (B): two runs share the character-edge sequence but take
 *     different epsilon paths within one segment (an epsilon
 *     "diamond"). Decided by capped path counting on the epsilon DAG.
 *
 * The shortest, then lexicographically smallest, witness is found by
 * BFS plus greedy lexicographic reconstruction, and two distinct
 * derivations of the witness are recovered by a bounded derivation
 * enumeration on the normalized AST.
 *
 * The module keeps no state between calls: results do not depend on
 * previously analyzed regexps or automata, and caller-provided ASTs
 * are only read, never written.
 */

/**
 * Internal control-flow error carrying an `unsupported` report.
 */
class Unsupported extends Error {
  constructor(reason, message, nodeRef) {
    super(message);
    this.reason = reason;
    this.nodeRef = nodeRef;
  }
}

const ASCII_ALNUM = /^[A-Za-z0-9]$/;

/**
 * Copies a `loc` object so reports never alias a caller-provided AST.
 */
function copyLoc(node) {
  if (!node || !node.loc) {
    return null;
  }
  const {source, start, end} = node.loc;
  return {
    source,
    start: {line: start.line, column: start.column, offset: start.offset},
    end: {line: end.line, column: end.column, offset: end.offset},
  };
}

function nodeRef(node, path) {
  return {
    type: node.type,
    path,
    loc: copyLoc(node),
  };
}

function joinPath(parent, segment) {
  if (!parent) {
    return segment;
  }
  return segment.startsWith('[') ? parent + segment : parent + '.' + segment;
}

function unsupported(reason, message, node, path) {
  throw new Unsupported(reason, message, nodeRef(node, path));
}

function isEmptyAlternative(node) {
  return (
    node &&
    node.type === 'Alternative' &&
    (!node.expressions || node.expressions.length === 0)
  );
}

/**
 * Validates the supported subset and normalizes the AST: groups are
 * flattened away (they are purely structural), and every kept node
 * records its AST identity (`path`, `loc`) and nullability.
 *
 * Returns a normalized node:
 *
 *   {kind: 'char', value, nullable: false, path, loc}
 *   {kind: 'seq', items, nullable, path, loc}
 *   {kind: 'alt', left, right, nullable, path, loc}
 *   {kind: 'rep', quantifier: '*'|'+'|'?', sub, nullable, path, loc}
 */
function normalize(node, path) {
  if (!node || typeof node.type !== 'string') {
    throw new Unsupported(
      'unsupported-node',
      'Expected an AST node',
      {type: (node && node.type) || null, path, loc: null}
    );
  }

  switch (node.type) {
    case 'Char': {
      if (node.kind !== 'simple') {
        unsupported(
          'non-simple-char',
          `Only simple chars are supported, got kind "${node.kind}"`,
          node,
          path
        );
      }
      if (!ASCII_ALNUM.test(node.value)) {
        unsupported(
          'non-ascii-alphanumeric-char',
          `Only ASCII letters and digits are supported, got ${JSON.stringify(node.value)}`,
          node,
          path
        );
      }
      return {
        kind: 'char',
        astType: 'Char',
        value: node.value,
        nullable: false,
        path,
        loc: copyLoc(node),
      };
    }

    case 'Alternative': {
      if (!node.expressions || node.expressions.length === 0) {
        unsupported(
          'empty-alternative',
          'Empty alternatives are not supported',
          node,
          path
        );
      }
      const items = node.expressions.map((child, index) =>
        normalize(child, joinPath(path, `expressions[${index}]`))
      );
      return {
        kind: 'seq',
        astType: 'Alternative',
        items,
        nullable: items.every(item => item.nullable),
        path,
        loc: copyLoc(node),
      };
    }

    case 'Disjunction': {
      for (const side of ['left', 'right']) {
        if (!node[side] || isEmptyAlternative(node[side])) {
          unsupported(
            'empty-disjunction-branch',
            `Disjunction ${side} branch is empty`,
            node,
            path
          );
        }
      }
      const left = normalize(node.left, joinPath(path, 'left'));
      const right = normalize(node.right, joinPath(path, 'right'));
      return {
        kind: 'alt',
        astType: 'Disjunction',
        left,
        right,
        nullable: left.nullable || right.nullable,
        path,
        loc: copyLoc(node),
      };
    }

    case 'Group': {
      if (!node.expression) {
        unsupported(
          'empty-group',
          'Empty groups are not supported',
          node,
          path
        );
      }
      // Groups are transparent: capturing is only structural here.
      return normalize(node.expression, joinPath(path, 'expression'));
    }

    case 'Repetition': {
      const quantifier = node.quantifier;
      if (quantifier.kind === 'Range') {
        unsupported(
          'range-quantifier',
          'Bounded {n,m} quantifiers are not supported',
          node,
          path
        );
      }
      if (!['*', '+', '?'].includes(quantifier.kind)) {
        unsupported(
          'unsupported-quantifier',
          `Unsupported quantifier kind "${quantifier.kind}"`,
          node,
          path
        );
      }
      const sub = normalize(node.expression, joinPath(path, 'expression'));
      if (sub.nullable) {
        unsupported(
          'nullable-quantifier-operand',
          `Quantifier "${quantifier.kind}" operand can match the empty string`,
          node,
          path
        );
      }
      return {
        kind: 'rep',
        astType: 'Repetition',
        quantifier: quantifier.kind,
        sub,
        nullable: quantifier.kind !== '+',
        path,
        loc: copyLoc(node),
      };
    }

    case 'CharacterClass':
      unsupported(
        'character-class',
        'Character classes are not supported',
        node,
        path
      );
      break;

    case 'Assertion':
      unsupported(
        'assertion',
        'Assertions are not supported',
        node,
        path
      );
      break;

    case 'Backreference':
      unsupported(
        'backreference',
        'Backreferences are not supported',
        node,
        path
      );
      break;

    default:
      unsupported(
        'unsupported-node',
        `AST node type "${node.type}" is not supported`,
        node,
        path
      );
  }
}

/**
 * Builds a Thompson-style NFA from a normalized node. Runs of this
 * automaton correspond 1-to-1 to derivations of the expression.
 */
function buildNFA(root) {
  const eps = [];
  const charEdges = [];

  function newState() {
    eps.push([]);
    return eps.length - 1;
  }

  function addEps(from, to) {
    eps[from].push(to);
  }

  function build(node) {
    const entry = newState();
    const exit = newState();
    switch (node.kind) {
      case 'char': {
        charEdges.push({
          id: charEdges.length,
          source: entry,
          target: exit,
          label: node.value,
        });
        break;
      }
      case 'seq': {
        let prev = entry;
        for (const item of node.items) {
          const [subEntry, subExit] = build(item);
          addEps(prev, subEntry);
          prev = subExit;
        }
        addEps(prev, exit);
        break;
      }
      case 'alt': {
        const [leftEntry, leftExit] = build(node.left);
        const [rightEntry, rightExit] = build(node.right);
        addEps(entry, leftEntry);
        addEps(entry, rightEntry);
        addEps(leftExit, exit);
        addEps(rightExit, exit);
        break;
      }
      case 'rep': {
        const [subEntry, subExit] = build(node.sub);
        addEps(entry, subEntry);
        if (node.quantifier !== '+') {
          // '*' and '?' allow skipping the operand entirely.
          addEps(entry, exit);
        }
        if (node.quantifier !== '?') {
          // '*' and '+' loop back.
          addEps(subExit, subEntry);
        }
        addEps(subExit, exit);
        break;
      }
    }
    return [entry, exit];
  }

  const [start, accept] = build(root);
  return {eps, charEdges, start, accept, count: eps.length};
}

/**
 * Number of epsilon paths between every pair of states, capped at 2.
 * The epsilon subgraph is a DAG: every loop introduced by a
 * repetition passes through its operand, which is guaranteed to be
 * non-nullable and therefore contains a character transition.
 */
function computeEpsilonPaths(eps, count) {
  const indegree = new Array(count).fill(0);
  for (let u = 0; u < count; u++) {
    for (const v of eps[u]) {
      indegree[v]++;
    }
  }
  const topo = [];
  const queue = [];
  for (let u = 0; u < count; u++) {
    if (indegree[u] === 0) {
      queue.push(u);
    }
  }
  while (queue.length > 0) {
    const u = queue.shift();
    topo.push(u);
    for (const v of eps[u]) {
      if (--indegree[v] === 0) {
        queue.push(v);
      }
    }
  }

  const paths = Array.from({length: count}, () => new Array(count).fill(0));
  for (let s = 0; s < count; s++) {
    paths[s][s] = 1;
    for (const u of topo) {
      const current = paths[s][u];
      if (current === 0) {
        continue;
      }
      for (const v of eps[u]) {
        paths[s][v] = Math.min(2, paths[s][v] + current);
      }
    }
  }
  return paths;
}

/**
 * BFS distances from a set of sources over directed edges.
 * `edges` maps a vertex to an array of {to, label}.
 */
function bfsDistances(edges, sources) {
  const dist = new Map();
  const queue = [];
  for (const source of sources) {
    if (!dist.has(source)) {
      dist.set(source, 0);
      queue.push(source);
    }
  }
  while (queue.length > 0) {
    const vertex = queue.shift();
    for (const edge of edges.get(vertex) || []) {
      if (!dist.has(edge.to)) {
        dist.set(edge.to, dist.get(vertex) + 1);
        queue.push(edge.to);
      }
    }
  }
  return dist;
}

function reverseEdges(edges) {
  const reversed = new Map();
  for (const [from, list] of edges) {
    for (const edge of list) {
      let list2 = reversed.get(edge.to);
      if (!list2) {
        list2 = [];
        reversed.set(edge.to, list2);
      }
      list2.push({to: from, label: edge.label});
    }
  }
  return reversed;
}

/**
 * Lexicographically smallest string of length `length` leading from
 * any of `starts` to a vertex with `distToTarget` 0. `distToTarget`
 * holds BFS distances to the target set; `alphabet` is sorted.
 */
function lexMinPath(edges, distToTarget, starts, length, alphabet) {
  let frontier = starts.filter(v => distToTarget.get(v) === length);
  const chars = [];
  let remaining = length;
  while (remaining > 0) {
    for (const label of alphabet) {
      const next = new Set();
      for (const vertex of frontier) {
        for (const edge of edges.get(vertex) || []) {
          if (edge.label === label && distToTarget.get(edge.to) === remaining - 1) {
            next.add(edge.to);
          }
        }
      }
      if (next.size > 0) {
        chars.push(label);
        frontier = [...next];
        break;
      }
    }
    remaining--;
  }
  return chars.join('');
}

/**
 * Finds the shortest (then lexicographically smallest) string that
 * has two distinct NFA runs, or null if the NFA is unambiguous.
 */
function findAmbiguousWitness(nfa) {
  const {eps, charEdges, start, accept, count} = nfa;
  const paths = computeEpsilonPaths(eps, count);

  const alphabet = [...new Set(charEdges.map(edge => edge.label))].sort();

  // Macro automaton over "boundary" states (the start state and the
  // char-edge targets). A macro step consumes exactly one character:
  // an epsilon segment, one char edge, and stops at the edge target.
  const boundaries = [...new Set([start, ...charEdges.map(e => e.target)])];
  const finals = new Set(boundaries.filter(b => paths[b][accept] >= 1));

  const macro = new Map();
  const addMacro = (from, to, label, edgeId) => {
    let list = macro.get(from);
    if (!list) {
      list = [];
      macro.set(from, list);
    }
    list.push({to, label, edgeId});
  };
  for (const boundary of boundaries) {
    for (const edge of charEdges) {
      if (paths[boundary][edge.source] >= 1) {
        addMacro(boundary, edge.target, edge.label, edge.id);
      }
    }
  }

  const reversedMacro = reverseEdges(macro);
  const distFromStart = bfsDistances(macro, [start]);
  const distToFinal = bfsDistances(reversedMacro, [...finals]);

  // Case (A): two runs with different character-edge sequences.
  // Product of the macro automaton with itself; the divergence flag
  // becomes 1 exactly when the two runs take different char edges.
  const vertexId = (u, v, d) => (u * count + v) * 2 + (u !== v ? 1 : d);
  const startVertex = vertexId(start, start, 0);
  const isTarget = vertex => {
    const d = vertex % 2;
    const u = Math.floor(vertex / 2 / count);
    const v = Math.floor(vertex / 2) % count;
    return d === 1 && finals.has(u) && finals.has(v);
  };

  const product = new Map();
  const visitedProduct = new Set([startVertex]);
  const addProductEdge = (from, to, label) => {
    let list = product.get(from);
    if (!list) {
      list = [];
      product.set(from, list);
    }
    list.push({to, label});
  };
  {
    const queue = [startVertex];
    while (queue.length > 0) {
      const vertex = queue.shift();
      const d = vertex % 2;
      const u = Math.floor(vertex / 2 / count);
      const v = Math.floor(vertex / 2) % count;
      for (const e1 of macro.get(u) || []) {
        for (const e2 of macro.get(v) || []) {
          if (e1.label !== e2.label) {
            continue;
          }
          const diverged = d === 1 || e1.edgeId !== e2.edgeId ? 1 : 0;
          const next = vertexId(e1.to, e2.to, diverged);
          addProductEdge(vertex, next, e1.label);
          if (!visitedProduct.has(next)) {
            visitedProduct.add(next);
            queue.push(next);
          }
        }
      }
    }
  }

  const distToTargetA = bfsDistances(reverseEdges(product), [
    ...[...visitedProduct].filter(isTarget),
  ]);
  const lengthA = distToTargetA.has(startVertex)
    ? distToTargetA.get(startVertex)
    : Infinity;
  const candidateA =
    lengthA === Infinity
      ? null
      : lexMinPath(product, distToTargetA, [startVertex], lengthA, alphabet);

  // Case (B): two runs sharing the character-edge sequence but taking
  // different epsilon paths within one segment (an epsilon diamond).
  // Segment endpoints are boundaries on the left and char-edge
  // sources or the accept state on the right.
  let candidateB = null;
  let lengthB = Infinity;
  const distToBoundary = new Map();
  for (const p of boundaries) {
    const prefixLength = distFromStart.get(p);
    if (prefixLength === undefined) {
      continue;
    }
    const segmentEnds = [...charEdges.map(e => e.source), accept];
    for (const q of segmentEnds) {
      if (paths[p][q] < 2) {
        continue;
      }
      // Suffix: either the run is done (q is the accept state), or it
      // continues with q's own char edge and a macro path to finals.
      let suffixLength;
      let suffixString;
      if (q === accept) {
        suffixLength = 0;
        suffixString = '';
      } else {
        const edge = charEdges.find(e => e.source === q);
        const rest = distToFinal.get(edge.target);
        if (rest === undefined) {
          continue;
        }
        suffixLength = 1 + rest;
        suffixString =
          edge.label +
          lexMinPath(macro, distToFinal, [edge.target], rest, alphabet);
      }
      const total = prefixLength + suffixLength;
      if (total > lengthB) {
        continue;
      }
      if (!distToBoundary.has(p)) {
        distToBoundary.set(p, bfsDistances(reversedMacro, [p]));
      }
      const prefixString = lexMinPath(
        macro,
        distToBoundary.get(p),
        [start],
        prefixLength,
        alphabet
      );
      const candidate = prefixString + suffixString;
      if (total < lengthB || candidate < candidateB) {
        lengthB = total;
        candidateB = candidate;
      }
    }
  }

  if (candidateA === null) {
    return candidateB;
  }
  if (candidateB === null || lengthA < lengthB) {
    return candidateA;
  }
  if (lengthB < lengthA) {
    return candidateB;
  }
  return candidateA < candidateB ? candidateA : candidateB;
}

/**
 * Lazily enumerates derivations of `node` consuming exactly
 * `string[i..j)`. Quantifier operands are guaranteed non-nullable,
 * so every repetition instance consumes at least one character and
 * the enumeration terminates.
 */
function* derive(node, string, i, j) {
  switch (node.kind) {
    case 'char': {
      if (j === i + 1 && string[i] === node.value) {
        yield {node, span: [i, j]};
      }
      return;
    }
    case 'seq': {
      for (const children of deriveSeq(node.items, string, i, j)) {
        yield {node, span: [i, j], children};
      }
      return;
    }
    case 'alt': {
      for (const child of derive(node.left, string, i, j)) {
        yield {node, span: [i, j], choice: 'left', child};
      }
      for (const child of derive(node.right, string, i, j)) {
        yield {node, span: [i, j], choice: 'right', child};
      }
      return;
    }
    case 'rep': {
      const min = node.quantifier === '+' ? 1 : 0;
      const max = node.quantifier === '?' ? 1 : Infinity;
      yield* deriveRep(node, string, i, j, min, max);
      return;
    }
  }
}

function* deriveSeq(items, string, i, j) {
  if (items.length === 0) {
    if (i === j) {
      yield [];
    }
    return;
  }
  const [first, ...rest] = items;
  for (let k = i; k <= j; k++) {
    for (const head of derive(first, string, i, k)) {
      for (const tail of deriveSeq(rest, string, k, j)) {
        yield [head, ...tail];
      }
    }
  }
}

function* deriveRep(node, string, i, j, min, max) {
  if (min === 0 && i === j) {
    yield {node, span: [i, j], count: 0, instances: []};
  }
  if (max === 0) {
    return;
  }
  // Operand is non-nullable: every instance consumes >= 1 char.
  for (let k = i + 1; k <= j; k++) {
    for (const instance of derive(node.sub, string, i, k)) {
      for (const rest of deriveRep(
        node,
        string,
        k,
        j,
        Math.max(0, min - 1),
        max === Infinity ? Infinity : max - 1
      )) {
        yield {
          node,
          span: [i, j],
          count: 1 + rest.count,
          instances: [instance, ...rest.instances],
        };
      }
    }
  }
}

/**
 * Converts an internal derivation into a JSON-serializable report
 * tree. Every node carries the identity of the originating AST node
 * (structural `path`, plus `loc` when available) and the span of the
 * witness it consumed.
 */
function derivationToJSON(derivation) {
  const {node} = derivation;
  const out = {
    type: node.astType,
    path: node.path,
    loc: node.loc,
    span: [...derivation.span],
  };
  switch (node.kind) {
    case 'char':
      out.value = node.value;
      break;
    case 'seq':
      out.children = derivation.children.map(derivationToJSON);
      break;
    case 'alt':
      out.choice = derivation.choice;
      out.child = derivationToJSON(derivation.child);
      break;
    case 'rep':
      out.quantifier = node.quantifier;
      out.count = derivation.count;
      out.instances = derivation.instances.map(derivationToJSON);
      break;
  }
  return out;
}

/**
 * Analyzes a regular expression for ambiguity: whether some string
 * has two genuinely different derivations (parse trees).
 *
 * @param string | RegExp | AST regexp
 *
 * @return a JSON-serializable report:
 *
 *   {status: 'unambiguous'}
 *
 *   {status: 'ambiguous', witness, derivations: [first, second]}
 *
 *   {status: 'unsupported', reason, message, node: {type, path, loc}}
 */
function analyzeAmbiguity(regexp) {
  let ast;
  if (regexp && typeof regexp === 'object' && typeof regexp.type === 'string') {
    // Caller-provided AST: only read, never written.
    ast = regexp;
  } else {
    ast = parser.parse(`${regexp}`, {captureLocations: true});
  }

  let body;
  let bodyPath;
  if (ast.type === 'RegExp') {
    if (ast.flags) {
      return toUnsupportedReport(
        new Unsupported(
          'unsupported-flags',
          `Regular expressions with flags are not supported, got "${ast.flags}"`,
          nodeRef(ast, '')
        )
      );
    }
    if (!ast.body) {
      return toUnsupportedReport(
        new Unsupported(
          'empty-body',
          'Empty regular expressions are not supported',
          nodeRef(ast, '')
        )
      );
    }
    body = ast.body;
    bodyPath = 'body';
  } else {
    body = ast;
    bodyPath = '';
  }

  let root;
  try {
    root = normalize(body, bodyPath);
  } catch (error) {
    if (error instanceof Unsupported) {
      return toUnsupportedReport(error);
    }
    throw error;
  }

  const nfa = buildNFA(root);
  const witness = findAmbiguousWitness(nfa);

  if (witness === null) {
    return {status: 'unambiguous'};
  }

  const derivations = [];
  for (const derivation of derive(root, witness, 0, witness.length)) {
    derivations.push(derivationToJSON(derivation));
    if (derivations.length === 2) {
      break;
    }
  }

  return {
    status: 'ambiguous',
    witness,
    derivations,
  };
}

function toUnsupportedReport(error) {
  return {
    status: 'unsupported',
    reason: error.reason,
    message: error.message,
    node: error.nodeRef,
  };
}

module.exports = analyzeAmbiguity;
