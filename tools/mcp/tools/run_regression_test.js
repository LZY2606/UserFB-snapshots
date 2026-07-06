/**
 * @module tools/run_regression_test
 * MCP tool: execute a VideoForge regression test suite.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'run_regression_test',
  description:
    'Execute a VideoForge regression test suite. ' +
    'Returns a pass/fail report with per-assertion detail.',
  inputSchema: {
    type: 'object',
    properties: {
      suite: {
        type: 'string',
        description: '"premiere", "fcpxml", "edl", or "all" (default: "all").',
        enum: ['premiere', 'fcpxml', 'edl', 'all'],
      },
    },
    required: [],
  },
};

/**
 * @param {object} args
 * @param {{ regressionService: import('../services/RegressionService.js').RegressionService }} services
 * @returns {Promise<object>}
 */
export async function handler(args, { regressionService }) {
  InputValidator.assert(args, {
    suite: { type: 'string', enum: ['premiere', 'fcpxml', 'edl', 'all'] },
  });

  const suiteName = args.suite ?? 'all';
  return regressionService.runSuite(suiteName);
}
