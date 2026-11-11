/**
 * Helpers for tracking which UTF-16 code unit ranges of the input string
 * each output glyph originates from during layout.
 *
 * A source range list is an array of half-open [start, end] pairs, sorted by
 * start, with overlapping or adjacent ranges coalesced.
 */

/**
 * Merges any number of source range lists into a single sorted, coalesced
 * list. Null/undefined lists are ignored. The result always contains fresh
 * arrays, so callers can mutate it without affecting the inputs.
 *
 * @param {...Array<Array<number>>} lists
 * @return {Array<Array<number>>}
 */
export function mergeRangeLists(...lists) {
  let ranges = [];
  for (let list of lists) {
    if (list) {
      for (let range of list) {
        ranges.push([range[0], range[1]]);
      }
    }
  }

  ranges.sort((a, b) => a[0] - b[0] || a[1] - b[1]);

  let merged = [];
  for (let [start, end] of ranges) {
    let last = merged[merged.length - 1];
    if (last && start <= last[1]) {
      last[1] = Math.max(last[1], end);
    } else {
      merged.push([start, end]);
    }
  }

  return merged;
}
