/**
 * @module tools/inspect_project
 * MCP tool: inspect a VideoForge project and return a structured summary.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'inspect_project',
  description:
    'Inspect a VideoForge project. Returns fps, resolution, duration, ' +
    'track counts, clip counts, caption counts, and transition counts.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId: {
        type: 'string',
        description: 'Project ID returned by create_project or load_project.',
      },
    },
    required: ['projectId'],
  },
};

/**
 * @param {object} args
 * @param {{ inspectionService: import('../services/InspectionService.js').InspectionService }} services
 * @returns {object}
 */
export function handler(args, { inspectionService }) {
  InputValidator.assert(args, {
    projectId: { type: 'string', required: true, minLength: 1 },
  });

  return inspectionService.inspectProject(args.projectId);
}
