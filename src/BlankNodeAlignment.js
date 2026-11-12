// Blank-node-aware alignment between two finite RDF 1.1 stores.
// N3Store.alignDiff delegates the search to this module, which never mutates
// either input: it only reads quads and works on plain identifiers.
import { Buffer } from 'buffer';
import { termToId } from './N3DataFactory';

const SUBJECT = 0;
const PREDICATE = 1;
const OBJECT = 2;
const GRAPH = 3;
const POSITIONS = [SUBJECT, PREDICATE, OBJECT, GRAPH];

// RDF 1.1 terms supported in a finite store. Variables and quoted triples are
// deliberately absent: they must be reported instead of being stringified.
const SUPPORTED_TERMS = new Set(['NamedNode', 'BlankNode', 'Literal', 'DefaultGraph']);

function termAt(quad, position) {
  switch (position) {
  case SUBJECT:   return quad.subject;
  case PREDICATE: return quad.predicate;
  case OBJECT:    return quad.object;
  default:        return quad.graph;
  }
}

// Compares strings by their UTF-8 byte order, independent of the platform locale.
function utf8Compare(left, right) {
  return Buffer.compare(Buffer.from(left), Buffer.from(right));
}

// The sort key of a quad is the JSON encoding of its four termToId values.
function quadKey(quad) {
  return JSON.stringify(POSITIONS.map(position => termToId(termAt(quad, position))));
}

// Rejects terms outside the RDF 1.1 scope before any identifier is derived.
function validateQuads(quads, label) {
  for (let i = 0; i < quads.length; i++) {
    const quad = quads[i];
    for (let p = 0; p < POSITIONS.length; p++) {
      const term = termAt(quad, POSITIONS[p]);
      if (!term || !SUPPORTED_TERMS.has(term.termType)) {
        const type = term && term.termType ? term.termType : 'unknown';
        throw new Error(`alignDiff does not support terms of type "${type}" in ${label}`);
      }
    }
  }
}

// Replaces repeated quads by a single entry, since stores are quad sets.
function deduplicate(quads) {
  const seen = new Set();
  const result = [];
  for (let i = 0; i < quads.length; i++) {
    const key = quadKey(quads[i]);
    if (!seen.has(key)) {
      seen.add(key);
      result.push(quads[i]);
    }
  }
  return result;
}

// Builds the fixed-term signature of a quad. Blank positions are null so they
// are never mistaken for equal labels, and records every blank term in use.
function prepareQuad(quad, blankTerms) {
  const signature = new Array(POSITIONS.length);
  const blankPositions = [];
  for (let p = 0; p < POSITIONS.length; p++) {
    const term = termAt(quad, POSITIONS[p]);
    if (term.termType === 'BlankNode') {
      signature[p] = null;
      blankPositions.push(p);
      blankTerms.set(termToId(term), term);
    }
    else
      signature[p] = termToId(term);
  }
  return { signature, blankPositions };
}

// Two quads can only be retained together when every fixed term at the same
// position is equal and blank positions line up across subject, object and graph.
function quadsCompatible(left, right) {
  for (let p = 0; p < POSITIONS.length; p++) {
    const a = left.signature[p];
    const b = right.signature[p];
    if (a === null || b === null) {
      if (a !== null || b !== null)
        return false;
    }
    else if (a !== b)
      return false;
  }
  return true;
}

// Canonicalizes a mapping as old/new identifier pairs sorted by old label bytes.
function sortedMappingPairs(mapping) {
  return Array.from(mapping, ([oldId, newId]) => [oldId, newId])
    .sort((left, right) => utf8Compare(left[0], right[0]));
}

// Tie-breaker between complete mappings that score equally on both objectives.
// The pair sequence is compared lexicographically in UTF-8 byte order.
function mappingIsSmaller(leftPairs, rightPairs) {
  const length = Math.min(leftPairs.length, rightPairs.length);
  for (let i = 0; i < length; i++) {
    const oldOrder = utf8Compare(leftPairs[i][0], rightPairs[i][0]);
    if (oldOrder !== 0)
      return oldOrder < 0;
    const newOrder = utf8Compare(leftPairs[i][1], rightPairs[i][1]);
    if (newOrder !== 0)
      return newOrder < 0;
  }
  return leftPairs.length < rightPairs.length;
}

/**
 * Finds the injective blank-node correspondence between two finite RDF stores
 * that retains the largest number of quads, and derives the content-level diff.
 *
 * Neither store is mutated; all returned arrays are independent copies.
 *
 * @param {Iterable} beforeStore the earlier capture
 * @param {Iterable} afterStore the later capture
 * @returns {{mapping: Array, retained: Array, removed: Array, added: Array, stats: object}}
 */
export function alignStores(beforeStore, afterStore) {
  const rawBefore = Array.from(beforeStore);
  const rawAfter = Array.from(afterStore);
  // Reject out-of-scope terms before deriving any identifier, so a variable or
  // quoted triple is never stringified into an ordinary node by mistake.
  validateQuads(rawBefore, 'the first store');
  validateQuads(rawAfter, 'the other store');
  const before = deduplicate(rawBefore);
  const after = deduplicate(rawAfter);

  const beforeBlanks = new Map();
  const afterBlanks = new Map();
  const beforeData = before.map(quad => prepareQuad(quad, beforeBlanks));
  const afterData = after.map(quad => prepareQuad(quad, afterBlanks));

  // Every compatible quad pair is a candidate for retention. A pair records the
  // blank-to-blank points its quads impose at each shared blank position.
  const pairs = [];
  const pairsByOld = new Map();
  const candidateSets = new Map();
  for (let i = 0; i < beforeData.length; i++) {
    for (let j = 0; j < afterData.length; j++) {
      const left = beforeData[i];
      const right = afterData[j];
      if (quadsCompatible(left, right)) {
        const points = [];
        const distinctOld = new Set();
        for (let p = 0; p < left.blankPositions.length; p++) {
          const position = left.blankPositions[p];
          const oldId = termToId(termAt(before[i], position));
          const newId = termToId(termAt(after[j], position));
          points.push({ oldId, newId });
          distinctOld.add(oldId);
          if (!candidateSets.has(oldId))
            candidateSets.set(oldId, new Set());
          candidateSets.get(oldId).add(newId);
        }

        const pairIndex = pairs.length;
        pairs.push({ oldIndex: i, newIndex: j, points, left: distinctOld.size, ok: true });
        for (const oldId of distinctOld) {
          if (!pairsByOld.has(oldId))
            pairsByOld.set(oldId, []);
          pairsByOld.get(oldId).push(pairIndex);
        }
      }
    }
  }

  // Candidate lists are sorted once for deterministic branch order.
  const candidates = new Map();
  for (const [oldId, newIds] of candidateSets)
    candidates.set(oldId, Array.from(newIds).sort(utf8Compare));

  // The search assigns one old blank at a time. Blanks without any candidate
  // can never be mapped, so they stay unmapped and merely keep pairs open.
  const pending = new Set(candidates.keys());
  const mapping = new Map();
  const used = new Set();
  // Quad pairs without blanks are retained from the start: no assignment can
  // retire them, and they must count in every upper bound.
  let retainedCount = pairs.reduce((count, pair) => count + (pair.left === 0 ? 1 : 0), 0);
  let best = null;
  const stats = { states: 0 };

  // A candidate is feasible when its blank node is still unused. Alternative
  // quad pairs never hard-constrain a mapping: pairs contradicting the choice
  // are simply retired when the assignment is applied. Only injectivity binds.
  function candidateFeasible(newId) {
    return !used.has(newId);
  }

  function applyMapping(oldId, newId) {
    mapping.set(oldId, newId);
    used.add(newId);
    pending.delete(oldId);
    const snapshot = [];
    const indexes = pairsByOld.get(oldId) || [];
    for (let k = 0; k < indexes.length; k++) {
      const pair = pairs[indexes[k]];
      if (pair.ok) {
        snapshot.push([pair, pair.left, pair.ok]);
        let consistent = true;
        for (let p = 0; p < pair.points.length; p++) {
          const point = pair.points[p];
          if (point.oldId === oldId && point.newId !== newId) {
            consistent = false;
            break;
          }
        }
        if (!consistent)
          pair.ok = false;
        else {
          pair.left--;
          if (pair.left === 0)
            retainedCount++;
        }
      }
    }
    return snapshot;
  }

  function undoMapping(oldId, newId, snapshot) {
    for (let k = 0; k < snapshot.length; k++) {
      const [pair, left, ok] = snapshot[k];
      if (pair.ok && pair.left === 0 && left > 0)
        retainedCount--;
      pair.left = left;
      pair.ok = ok;
    }
    pending.add(oldId);
    used.delete(newId);
    mapping.delete(oldId);
  }

  function applyUnmapped(oldId) {
    pending.delete(oldId);
    const snapshot = [];
    const indexes = pairsByOld.get(oldId) || [];
    for (let k = 0; k < indexes.length; k++) {
      const pair = pairs[indexes[k]];
      if (pair.ok) {
        snapshot.push([pair, pair.left, pair.ok]);
        pair.ok = false;
      }
    }
    return snapshot;
  }

  function undoUnmapped(oldId, snapshot) {
    for (let k = 0; k < snapshot.length; k++) {
      const [pair, left, ok] = snapshot[k];
      pair.left = left;
      pair.ok = ok;
    }
    pending.add(oldId);
  }

  function evaluateLeaf() {
    const retainedPairs = [];
    for (let i = 0; i < pairs.length; i++) {
      if (pairs[i].ok && pairs[i].left === 0)
        retainedPairs.push(i);
    }
    const mapped = mapping.size;
    const candidatePairs = sortedMappingPairs(mapping);
    const improves = best === null ||
      retainedPairs.length > best.retained ||
      (retainedPairs.length === best.retained && mapped < best.mapped) ||
      (retainedPairs.length === best.retained && mapped === best.mapped &&
        mappingIsSmaller(candidatePairs, best.mapping));
    if (improves) {
      best = {
        retained: retainedPairs.length,
        mapped,
        pairs: retainedPairs,
        mapping: candidatePairs,
      };
    }
  }

  // A greedy map-first completion seeds a valid incumbent, so the exhaustive
  // search can prune hopeless branches against a real upper bound immediately.
  function seedIncumbent() {
    const changes = [];
    while (pending.size > 0) {
      stats.states++;
      let chosen = null;
      let chosenNew = null;
      let fewestOptions = Infinity;
      for (const oldId of pending) {
        let optionCount = 0;
        let firstOption = null;
        const candidateList = candidates.get(oldId);
        for (let k = 0; k < candidateList.length; k++) {
          if (candidateFeasible(candidateList[k])) {
            optionCount++;
            if (firstOption === null)
              firstOption = candidateList[k];
          }
        }
        if (optionCount < fewestOptions ||
            (optionCount === fewestOptions &&
             (chosen === null || utf8Compare(oldId, chosen) < 0))) {
          fewestOptions = optionCount;
          chosen = oldId;
          chosenNew = firstOption;
        }
      }
      if (chosenNew !== null) {
        const snapshot = applyMapping(chosen, chosenNew);
        changes.push([true, chosen, chosenNew, snapshot]);
      }
      else {
        const snapshot = applyUnmapped(chosen);
        changes.push([false, chosen, null, snapshot]);
      }
    }
    evaluateLeaf();
    for (let k = changes.length - 1; k >= 0; k--) {
      const [mappedBranch, oldId, newId, snapshot] = changes[k];
      if (mappedBranch)
        undoMapping(oldId, newId, snapshot);
      else
        undoUnmapped(oldId, snapshot);
    }
  }

  // Admissible upper bound on additionally retained quads. Two alive pairs
  // cannot both be retained when they use the same old quad or the same new
  // quad, so the maximum is a bipartite matching between quad indices.
  // Blank-node consistency can only lower this number, never raise it.
  function augmentMatching(oldIndex, adjacency, matchForNew, visited) {
    const neighbors = adjacency.get(oldIndex);
    for (let k = 0; k < neighbors.length; k++) {
      const newIndex = neighbors[k];
      if (!visited.has(newIndex)) {
        visited.add(newIndex);
        const other = matchForNew.get(newIndex);
        if (other === undefined || augmentMatching(other, adjacency, matchForNew, visited)) {
          matchForNew.set(newIndex, oldIndex);
          return true;
        }
      }
    }
    return false;
  }

  function matchingBound() {
    const adjacency = new Map();
    for (let i = 0; i < pairs.length; i++) {
      const pair = pairs[i];
      if (pair.ok && pair.left > 0) {
        if (!adjacency.has(pair.oldIndex))
          adjacency.set(pair.oldIndex, []);
        adjacency.get(pair.oldIndex).push(pair.newIndex);
      }
    }
    const matchForNew = new Map();
    const visited = new Set();
    let matching = 0;
    for (const oldIndex of adjacency.keys()) {
      visited.clear();
      if (augmentMatching(oldIndex, adjacency, matchForNew, visited))
        matching++;
    }
    return matching;
  }

  function recurse() {
    stats.states++;

    const bound = retainedCount + matchingBound();
    if (best !== null &&
        (bound < best.retained ||
         (bound === best.retained && mapping.size > best.mapped)))
      return;

    // Minimum-remaining-values keeps the most constrained blank next, which
    // prunes branches early; ties fall back to the old label byte order.
    let chosen = null;
    let chosenOptions = null;
    for (const oldId of pending) {
      const options = [];
      const candidateList = candidates.get(oldId);
      for (let k = 0; k < candidateList.length; k++) {
        if (candidateFeasible(candidateList[k]))
          options.push(candidateList[k]);
      }
      if (chosen === null || options.length < chosenOptions.length ||
          (options.length === chosenOptions.length && utf8Compare(oldId, chosen) < 0)) {
        chosen = oldId;
        chosenOptions = options;
      }
    }

    if (chosen === null) {
      evaluateLeaf();
      return;
    }

    // Mapping branches first reach a strong incumbent quickly, so later
    // branches can be cut by the upper bound. The unmapped branch comes last.
    for (let k = 0; k < chosenOptions.length; k++) {
      const newId = chosenOptions[k];
      const snapshot = applyMapping(chosen, newId);
      recurse();
      undoMapping(chosen, newId, snapshot);
    }
    const unmappedSnapshot = applyUnmapped(chosen);
    recurse();
    undoUnmapped(chosen, unmappedSnapshot);
  }

  seedIncumbent();
  recurse();

  const bestPairs = best ? best.pairs : [];
  const retainedOld = new Set();
  const retainedNew = new Set();
  const retained = bestPairs.map(pairIndex => {
    const pair = pairs[pairIndex];
    retainedOld.add(pair.oldIndex);
    retainedNew.add(pair.newIndex);
    return { before: before[pair.oldIndex], after: after[pair.newIndex] };
  });
  retained.sort((left, right) => {
    const beforeOrder = utf8Compare(quadKey(left.before), quadKey(right.before));
    return beforeOrder !== 0 ? beforeOrder :
      utf8Compare(quadKey(left.after), quadKey(right.after));
  });

  const removed = [];
  for (let i = 0; i < before.length; i++) {
    if (!retainedOld.has(i))
      removed.push(before[i]);
  }
  const added = [];
  for (let i = 0; i < after.length; i++) {
    if (!retainedNew.has(i))
      added.push(after[i]);
  }
  removed.sort((left, right) => utf8Compare(quadKey(left), quadKey(right)));
  added.sort((left, right) => utf8Compare(quadKey(left), quadKey(right)));

  const mappingResult = best ? best.mapping.map(([oldId, newId]) =>
    [beforeBlanks.get(oldId), afterBlanks.get(newId)]) : [];

  return {
    mapping: mappingResult,
    retained,
    removed,
    added,
    stats: { states: stats.states },
  };
}
