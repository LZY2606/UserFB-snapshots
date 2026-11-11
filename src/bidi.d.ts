// Type definitions for bidi-js
// A JavaScript implementation of the Unicode Bidirectional Algorithm

/**
 * One of the 23 Unicode bidirectional character type names, as returned by
 * {@link Bidi.getBidiCharTypeName}.
 */
export type BidiCharTypeName =
  | "L"
  | "R"
  | "EN"
  | "ES"
  | "ET"
  | "AN"
  | "CS"
  | "B"
  | "S"
  | "WS"
  | "ON"
  | "BN"
  | "NSM"
  | "AL"
  | "LRO"
  | "RLO"
  | "LRE"
  | "RLE"
  | "PDF"
  | "LRI"
  | "RLI"
  | "FSI"
  | "PDI";

/**
 * The resolved embedding levels for a string, as returned by
 * {@link Bidi.getEmbeddingLevels}.
 *
 * All indices used by this API are UTF-16 code unit positions. Valid
 * surrogate pairs are treated internally as single Unicode scalar values, so
 * the two code units of a pair always share the same level, and reordering
 * never separates them. A lone (unpaired) surrogate is kept as its own code
 * unit and processed as a default L-type character.
 */
export interface EmbeddingLevels {
  /**
   * A `Uint8Array` holding the calculated bidi embedding level for each
   * UTF-16 code unit in the string (`levels.length === string.length`). A
   * character is in a right-to-left scope if its level is an odd number, and
   * left-to-right if it's even. Both code units of a valid surrogate pair
   * carry the level of their shared Unicode scalar value.
   */
  levels: Uint8Array;
  /**
   * One entry per paragraph in the text (paragraphs are separated by explicit
   * breaking characters, not soft line wrapping). The `start` and `end` indices
   * are inclusive UTF-16 code unit positions, and `level` is the resolved base
   * embedding level of that paragraph.
   */
  paragraphs: Array<{ start: number; end: number; level: number }>;
}

/**
 * A `bidi` object exposing the methods for bidi processing, as returned by the
 * default-exported factory function.
 */
export interface Bidi {
  /**
   * Apply the Bidirectional Algorithm to a string, returning the resolved
   * embedding levels plus a list of each paragraph's start/end indices and
   * resolved base embedding level.
   *
   * @param string - The input string containing mixed-direction text.
   * @param baseDirection - Use `"ltr"` or `"rtl"` to force a base paragraph
   *   direction, otherwise a direction will be chosen automatically from each
   *   paragraph's contents.
   */
  getEmbeddingLevels(
    string: string,
    baseDirection?: "ltr" | "rtl" | "auto"
  ): EmbeddingLevels;

  /**
   * Given a start and end denoting a single line within a string, and a set of
   * precalculated bidi embedding levels, produce a list of `[start, end]`
   * segments whose ordering should be flipped, in sequence.
   *
   * The per-line trailing-whitespace rule (UAX#9 L1) is applied on a local
   * copy of the line's levels; the `levels` array in `embeddingLevelsResult`
   * is never modified, so lines may be processed repeatedly and in any order.
   *
   * Segment boundaries always fall between Unicode scalar values: a valid
   * surrogate pair is never split, and must be flipped as a single unit when
   * applying the segments.
   *
   * @param string - The full input string.
   * @param embeddingLevelsResult - The result object from
   *   {@link Bidi.getEmbeddingLevels}.
   * @param start - First UTF-16 code unit in a subset of the full string
   *   (inclusive). Defaults to `0`.
   * @param end - Last UTF-16 code unit in a subset of the full string
   *   (inclusive). Defaults to the last code unit.
   * @returns The list of start/end segments that should be flipped, in order.
   * @throws {RangeError} If `start`/`end` would split a valid surrogate pair.
   */
  getReorderSegments(
    string: string,
    embeddingLevelsResult: EmbeddingLevels,
    start?: number,
    end?: number
  ): number[][];

  /**
   * Given a string and its resolved embedding levels, return an array of
   * UTF-16 code unit indices in their new bidi order. The result is a
   * permutation of every original code unit position; the two code units of
   * a valid surrogate pair always remain adjacent, high surrogate first.
   *
   * @param string - The full input string.
   * @param embedLevelsResult - The result object from
   *   {@link Bidi.getEmbeddingLevels}.
   * @param start - First UTF-16 code unit in a subset of the full string
   *   (inclusive). Defaults to `0`.
   * @param end - Last UTF-16 code unit in a subset of the full string
   *   (inclusive). Defaults to the last code unit.
   * @throws {RangeError} If `start`/`end` would split a valid surrogate pair.
   */
  getReorderedIndices(
    string: string,
    embedLevelsResult: EmbeddingLevels,
    start?: number,
    end?: number
  ): number[];

  /**
   * Given a string and its resolved embedding levels, return a new string with
   * its bidi segments reordered (and right-to-left mirrored characters
   * swapped). The result contains exactly the same UTF-16 code units as the
   * input, reordered; valid surrogate pairs are never separated.
   *
   * @param string - The full input string.
   * @param embedLevelsResult - The result object from
   *   {@link Bidi.getEmbeddingLevels}.
   * @param start - First UTF-16 code unit in a subset of the full string
   *   (inclusive). Defaults to `0`.
   * @param end - Last UTF-16 code unit in a subset of the full string
   *   (inclusive). Defaults to the last code unit.
   * @throws {RangeError} If `start`/`end` would split a valid surrogate pair.
   */
  getReorderedString(
    string: string,
    embedLevelsResult: EmbeddingLevels,
    start?: number,
    end?: number
  ): string;

  /**
   * Get the bidi character type bit flag for a given character.
   */
  getBidiCharType(char: string): number;

  /**
   * Get the bidi character type name for a given character.
   */
  getBidiCharTypeName(char: string): BidiCharTypeName;

  /**
   * Get the mirrored character for a given character, if one exists, otherwise
   * `null`. Only meaningful for right-to-left characters (those whose embedding
   * level is an odd number).
   */
  getMirroredCharacter(char: string): string | null;

  /**
   * Given a string and its resolved embedding levels, build a map of indices to
   * replacement characters for any characters in right-to-left segments that
   * have defined mirrored characters.
   *
   * @param string - The full input string.
   * @param embeddingLevels - The `levels` array from
   *   {@link Bidi.getEmbeddingLevels}.
   * @param start - First UTF-16 code unit in a subset of the full string
   *   (inclusive).
   * @param end - Last UTF-16 code unit in a subset of the full string
   *   (inclusive).
   */
  getMirroredCharactersMap(
    string: string,
    embeddingLevels: Uint8Array,
    start?: number,
    end?: number
  ): Map<number, string>;

  /**
   * Get the opening bracket character corresponding to a given closing bracket
   * character, or `null` if there is none.
   */
  closingToOpeningBracket(char: string): string | null;

  /**
   * Get the closing bracket character corresponding to a given opening bracket
   * character, or `null` if there is none.
   */
  openingToClosingBracket(char: string): string | null;

  /**
   * Retrieve the canonical form of a bracket character, or `null` if there is
   * none.
   */
  getCanonicalBracket(char: string): string | null;
}

/**
 * The `bidi-js` package's only export is this factory function, which you
 * _must invoke_ to return a `bidi` object exposing the methods for bidi
 * processing.
 *
 * The factory exists so the entire module's code is wrapped within a single
 * self-contained function with no closure dependencies, which enables that
 * function to be stringified and passed into a web worker.
 */
export default function bidiFactory(): Bidi;
