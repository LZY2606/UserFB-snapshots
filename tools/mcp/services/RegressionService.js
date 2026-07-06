/**
 * @module RegressionService
 * Orchestrate named regression suites and aggregate their reports.
 *
 * Thin façade over RegressionRunner — keeps the service layer uniform
 * and avoids exposing the runner directly to MCP tools.
 */

import { RegressionRunner } from '../regression/RegressionRunner.js';

export class RegressionService {
  /**
   * @param {import('./ProjectService.js').ProjectService} projectService
   * @param {import('./ExportService.js').ExportService} exportService
   * @param {import('./ValidationService.js').ValidationService} validationService
   * @param {import('./InspectionService.js').InspectionService} inspectionService
   */
  constructor(projectService, exportService, validationService, inspectionService) {
    this._runner = new RegressionRunner({
      projectService,
      exportService,
      validationService,
      inspectionService,
    });
  }

  /**
   * Run a named suite or all suites.
   *
   * @param {string} [suiteName='all']  'premiere' | 'fcpxml' | 'edl' | 'all'
   * @returns {Promise<RegressionReport>}
   */
  async runSuite(suiteName = 'all') {
    if (suiteName === 'all') return this._runner.runAll();
    return this._runner.runSuite(suiteName);
  }

  /**
   * @returns {string[]}
   */
  listSuites() {
    return this._runner.listSuites();
  }
}
