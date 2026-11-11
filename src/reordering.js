import { getBidiCharTypeAt, TRAILING_TYPES } from './charTypes.js'
import { getMirroredCharacter } from './mirroring.js'
import { isHighSurrogateAt, isLowSurrogateOfPair } from './codepoints.js'

/**
 * Normalize and validate an inclusive UTF-16 code unit range. Defaults cover
 * the whole string. A range that cuts a valid surrogate pair in half is
 * rejected, since a line boundary may never split a Unicode scalar value.
 *
 * @param {string} string
 * @param {number|undefined} start
 * @param {number|undefined} end
 * @return {{start: number, end: number}}
 */
function normalizeRange (string, start, end) {
  const strLen = string.length
  start = Math.max(0, start == null ? 0 : +start)
  end = Math.min(strLen - 1, end == null ? strLen - 1 : +end)

  if (start <= end) {
    // `start` may not land on the low surrogate half of a pair ...
    if (isLowSurrogateOfPair(string, start)) {
      throw new RangeError(
        `Range start ${start} splits a surrogate pair in a string of length ${strLen}`
      )
    }
    // ... and `end` may not land on the high surrogate half of a pair.
    if (isHighSurrogateAt(string, end)) {
      throw new RangeError(
        `Range end ${end} splits a surrogate pair in a string of length ${strLen}`
      )
    }
  }
  return { start, end }
}

/**
 * Given a start and end denoting a single line within a string, and a set of precalculated
 * bidi embedding levels, produce a list of segments whose ordering should be flipped, in sequence.
 *
 * The L1 trailing-whitespace rule is applied per line here, on a local copy of the line's
 * levels; the levels returned by `getEmbeddingLevels` are never modified. Segment boundaries
 * always fall between Unicode scalar values, so a surrogate pair is never split.
 *
 * @param {string} string - the full input string
 * @param {GetEmbeddingLevelsResult} embeddingLevelsResult - the result object from getEmbeddingLevels
 * @param {number} [start] - first code unit in a subset of the full string (inclusive)
 * @param {number} [end] - last code unit in a subset of the full string (inclusive)
 * @return {number[][]} - the list of start/end segments that should be flipped, in order.
 */
export function getReorderSegments(string, embeddingLevelsResult, start, end) {
  ({ start, end } = normalizeRange(string, start, end))

  const segments = []
  embeddingLevelsResult.paragraphs.forEach(paragraph => {
    const lineStart = Math.max(start, paragraph.start)
    const lineEnd = Math.min(end, paragraph.end)
    if (lineStart < lineEnd) {
      // Local slice for mutation; the shared resolved levels array is never touched.
      const lineLevels = embeddingLevelsResult.levels.slice(lineStart, lineEnd + 1)

      // 3.4 L1.4: Reset any sequence of whitespace characters and/or isolate formatting characters at the
      // end of this line to the paragraph level. Both code units of a surrogate pair always carry the same
      // type (and level), so this scan resets them together or stops before them.
      for (let i = lineEnd; i >= lineStart && (getBidiCharTypeAt(string, i) & TRAILING_TYPES); i--) {
        lineLevels[i - lineStart] = paragraph.level
      }

      // L2. From the highest level found in the text to the lowest odd level on each line, including intermediate levels
      // not actually present in the text, reverse any contiguous sequence of characters that are at that level or higher.
      let maxLevel = paragraph.level
      let minOddLevel = Infinity
      for (let i = 0; i < lineLevels.length; i++) {
        const level = lineLevels[i]
        if (level > maxLevel) maxLevel = level
        if (level < minOddLevel) minOddLevel = level | 1
      }
      for (let lvl = maxLevel; lvl >= minOddLevel; lvl--) {
        for (let i = 0; i < lineLevels.length; i++) {
          if (lineLevels[i] >= lvl) {
            const segStart = i
            while (i + 1 < lineLevels.length && lineLevels[i + 1] >= lvl) {
              i++
            }
            if (i > segStart) {
              // Equal levels on both halves of a surrogate pair guarantee these
              // boundaries lie between scalar values, never inside a pair.
              segments.push([segStart + lineStart, i + lineStart])
            }
          }
        }
      }
    }
  })
  return segments
}

/**
 * @param {string} string
 * @param {GetEmbeddingLevelsResult} embedLevelsResult
 * @param {number} [start]
 * @param {number} [end]
 * @return {string} the new string with bidi segments reordered
 */
export function getReorderedString(string, embedLevelsResult, start, end) {
  const indices = getReorderedIndices(string, embedLevelsResult, start, end)
  const chars = new Array(string.length)
  indices.forEach((charIndex, i) => {
    chars[i] = (
      (embedLevelsResult.levels[charIndex] & 1) ? getMirroredCharacter(string[charIndex]) : null
    ) || string[charIndex]
  })
  return chars.join('')
}

/**
 * @param {string} string
 * @param {GetEmbeddingLevelsResult} embedLevelsResult
 * @param {number} [start]
 * @param {number} [end]
 * @return {number[]} an array with UTF-16 code unit indices in their new bidi order.
 *         It is a permutation of every original code unit position; the two units
 *         of a surrogate pair always remain adjacent, high surrogate first.
 */
export function getReorderedIndices(string, embedLevelsResult, start, end) {
  const segments = getReorderSegments(string, embedLevelsResult, start, end)

  // Work at scalar granularity: one entry per Unicode scalar value, holding the
  // leading code unit of that scalar. Valid surrogate pairs are reversed as a
  // single block instead of having their two code units swapped.
  const scalarLeaders = []
  const scalarIndexOf = new Uint32Array(string.length)
  for (let i = 0; i < string.length; i++) {
    scalarIndexOf[i] = scalarLeaders.length
    scalarLeaders.push(i)
    if (isHighSurrogateAt(string, i)) {
      scalarIndexOf[i + 1] = scalarLeaders.length - 1
      i++
    }
  }

  // Reverse each segment in order, over scalar positions
  const orderedLeaders = scalarLeaders.slice()
  segments.forEach(([segStart, segEnd]) => {
    let lo = scalarIndexOf[segStart]
    let hi = scalarIndexOf[segEnd]
    while (lo < hi) {
      const tmp = orderedLeaders[lo]
      orderedLeaders[lo] = orderedLeaders[hi]
      orderedLeaders[hi] = tmp
      lo++
      hi--
    }
  })

  // Expand scalars back into a permutation over all code unit positions
  const indices = []
  for (let s = 0; s < orderedLeaders.length; s++) {
    const leader = orderedLeaders[s]
    indices.push(leader)
    if (isHighSurrogateAt(string, leader)) {
      indices.push(leader + 1)
    }
  }
  return indices
}
