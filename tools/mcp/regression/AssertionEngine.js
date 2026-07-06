/**
 * @module AssertionEngine
 * Lightweight, non-throwing assertion collector for regression suites.
 *
 * All assertion methods record a result and return `this` so assertions
 * can be chained.  Failures do not throw — the final report is evaluated
 * by RegressionRunner once the suite finishes.
 *
 * @example
 * const engine = new AssertionEngine();
 * engine
 *   .assertEquals('fps preserved', actual.fps, 24)
 *   .assertContains('has xmeml root', xml, '<xmeml')
 *   .assertGreaterThan('clip count > 0', stats.clipItemCount, 0);
 */

export class AssertionEngine {
  constructor() {
    /** @type {AssertionRecord[]} */
    this._assertions = [];
  }

  // ── State ─────────────────────────────────────────────────────────────────────

  /** Reset all recorded assertions (used between test runs). */
  reset() {
    this._assertions = [];
  }

  /** @returns {AssertionRecord[]} */
  get assertions() {
    return [...this._assertions];
  }

  /** @returns {number} */
  get passCount() {
    return this._assertions.filter((a) => a.passed).length;
  }

  /** @returns {number} */
  get failCount() {
    return this._assertions.filter((a) => !a.passed).length;
  }

  /** @returns {boolean} true iff all assertions passed and at least one was recorded */
  get passed() {
    return this._assertions.length > 0 && this.failCount === 0;
  }

  // ── Assertion methods ─────────────────────────────────────────────────────────

  /** @returns {this} */
  assertEquals(description, actual, expected) {
    return this._push(description, actual === expected, expected, actual);
  }

  /** @returns {this} */
  assertNotEquals(description, actual, notExpected) {
    return this._push(description, actual !== notExpected, `!== ${notExpected}`, actual);
  }

  /** @returns {this} */
  assertTrue(description, value) {
    return this._push(description, Boolean(value), true, value);
  }

  /** @returns {this} */
  assertFalse(description, value) {
    return this._push(description, !value, false, value);
  }

  /**
   * Assert actual > min.
   * @returns {this}
   */
  assertGreaterThan(description, actual, min) {
    const passed = typeof actual === 'number' && actual > min;
    return this._push(description, passed, `> ${min}`, actual);
  }

  /**
   * Assert actual >= min.
   * @returns {this}
   */
  assertGreaterThanOrEqual(description, actual, min) {
    const passed = typeof actual === 'number' && actual >= min;
    return this._push(description, passed, `>= ${min}`, actual);
  }

  /**
   * Assert content contains substring.
   * @returns {this}
   */
  assertContains(description, content, substring) {
    const passed = typeof content === 'string' && content.includes(substring);
    return this._push(description, passed, `contains "${substring}"`, passed ? '✓' : 'not found');
  }

  /**
   * Assert value is not null/undefined/empty-string/empty-array.
   * @returns {this}
   */
  assertNotEmpty(description, value) {
    const passed =
      value !== null &&
      value !== undefined &&
      value !== '' &&
      !(Array.isArray(value) && value.length === 0);
    return this._push(description, passed, 'non-empty', value);
  }

  /**
   * Assert array has exact length.
   * @returns {this}
   */
  assertArrayLength(description, arr, expectedLength) {
    const actual = Array.isArray(arr) ? arr.length : null;
    return this._push(description, actual === expectedLength, expectedLength, actual);
  }

  /**
   * Assert min <= actual <= max.
   * @returns {this}
   */
  assertInRange(description, actual, min, max) {
    const passed = typeof actual === 'number' && actual >= min && actual <= max;
    return this._push(description, passed, `[${min}–${max}]`, actual);
  }

  // ── Internal ──────────────────────────────────────────────────────────────────

  /**
   * @param {string} description
   * @param {boolean} passed
   * @param {*} expected
   * @param {*} actual
   * @returns {this}
   */
  _push(description, passed, expected, actual) {
    this._assertions.push({ description, passed, expected, actual });
    return this;
  }
}

/**
 * @typedef {{ description: string, passed: boolean, expected: *, actual: * }} AssertionRecord
 */
