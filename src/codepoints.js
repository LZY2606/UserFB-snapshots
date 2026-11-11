/**
 * Helpers for treating valid UTF-16 surrogate pairs as single Unicode scalar
 * values, while keeping all public indexing in UTF-16 code units.
 *
 * A lone (unpaired) surrogate is kept as its own code unit and is processed
 * as an ordinary default L-type character; it is never dropped.
 */

/**
 * True when the code unit at `index` is the high surrogate of a valid
 * surrogate pair, i.e. it is immediately followed by a low surrogate.
 *
 * @param {string} string
 * @param {number} index
 * @return {boolean}
 */
export function isHighSurrogateAt (string, index) {
  const cu = string.charCodeAt(index)
  return cu >= 0xD800 && cu <= 0xDBFF &&
    (string.charCodeAt(index + 1) & 0xFC00) === 0xDC00
}

/**
 * True when the code unit at `index` is the low surrogate half of a valid
 * surrogate pair, i.e. it immediately follows a high surrogate.
 *
 * @param {string} string
 * @param {number} index
 * @return {boolean}
 */
export function isLowSurrogateOfPair (string, index) {
  return (string.charCodeAt(index) & 0xFC00) === 0xDC00 &&
    isHighSurrogateAt(string, index - 1)
}

/**
 * Return the single-character string holding the Unicode scalar value at the
 * given code-unit position (a two-code-unit string for a supplementary-plane
 * scalar, a one-code-unit string otherwise).
 *
 * @param {string} string
 * @param {number} index
 * @return {string}
 */
export function scalarAt (string, index) {
  return isHighSurrogateAt(string, index)
    ? string.slice(index, index + 2)
    : string[index]
}
