/**
 * @module RegressionRunner
 * Discovers and executes named regression suites, collects structured reports.
 *
 * Each suite is a function:
 *   async (engine: AssertionEngine, services: Services) => void
 *
 * Errors thrown by a suite are caught and recorded in the report's `error`
 * field so a single failing suite does not abort the entire run.
 */

import { AssertionEngine } from './AssertionEngine.js';
import { premiereSuite }   from './suites/premiere_suite.js';
import { fcpxmlSuite }     from './suites/fcpxml_suite.js';
import { edlSuite }        from './suites/edl_suite.js';

/** @type {Record<string, Function>} */
const SUITES = {
  premiere: premiereSuite,
  fcpxml:   fcpxmlSuite,
  edl:      edlSuite,
};

export class RegressionRunner {
  /**
   * @param {object} services  All services, passed through to each suite.
   */
  constructor(services) {
    this._services = services;
  }

  // ── Public API ────────────────────────────────────────────────────────────────

  /** @returns {string[]} */
  listSuites() {
    return Object.keys(SUITES);
  }

  /**
   * Run a single named suite.
   *
   * @param {string} suiteName
   * @returns {Promise<SuiteReport>}
   */
  async runSuite(suiteName) {
    const suite = SUITES[suiteName];
    if (!suite) {
      throw new Error(
        `Unknown suite: "${suiteName}". Available: ${this.listSuites().join(', ')}`,
      );
    }

    const engine = new AssertionEngine();
    const t0 = Date.now();
    let error = null;

    try {
      await suite(engine, this._services);
    } catch (err) {
      error = err.message;
    }

    return this._buildReport(suiteName, engine, Date.now() - t0, error);
  }

  /**
   * Run all suites and aggregate results.
   *
   * @returns {Promise<AggregateReport>}
   */
  async runAll() {
    const t0 = Date.now();
    const suiteReports = [];

    for (const name of this.listSuites()) {
      suiteReports.push(await this.runSuite(name));
    }

    const total  = suiteReports.reduce((s, r) => s + r.total,  0);
    const passed = suiteReports.reduce((s, r) => s + r.passed, 0);
    const failed = suiteReports.reduce((s, r) => s + r.failed, 0);

    return {
      suite:      'all',
      total,
      passed,
      failed,
      success:    failed === 0,
      durationMs: Date.now() - t0,
      suites:     suiteReports,
    };
  }

  // ── Internal ──────────────────────────────────────────────────────────────────

  /**
   * @param {string} name
   * @param {AssertionEngine} engine
   * @param {number} durationMs
   * @param {string|null} error
   * @returns {SuiteReport}
   */
  _buildReport(name, engine, durationMs, error) {
    return {
      suite:      name,
      total:      engine.assertions.length,
      passed:     engine.passCount,
      failed:     engine.failCount,
      success:    engine.passed && error === null,
      durationMs,
      error,
      assertions: engine.assertions.map((a) => ({
        description: a.description,
        passed:      a.passed,
        expected:    a.expected,
        actual:      a.actual,
      })),
    };
  }
}

/**
 * @typedef {{ suite: string, total: number, passed: number, failed: number,
 *             success: boolean, durationMs: number, error: string|null,
 *             assertions: Array }} SuiteReport
 *
 * @typedef {{ suite: 'all', total: number, passed: number, failed: number,
 *             success: boolean, durationMs: number, suites: SuiteReport[] }} AggregateReport
 */
