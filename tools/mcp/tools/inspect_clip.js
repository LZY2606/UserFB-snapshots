/**
 * @module tools/inspect_clip
 * MCP tool: return full detail for a single clip by ID.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'inspect_clip',
  description:
    'Inspect a single clip — returns timing, transform, effects, transitions, ' +
    'and caption summary (if applicable).',
  inputSchema: {
    type: 'object',
    properties: {
      projectId: { type: 'string', description: 'Project ID.' },
      clipId:    { type: 'string', description: 'Clip ID to inspect.' },
    },
    required: ['projectId', 'clipId'],
  },
};

/**
 * @param {object} args
 * @param {{ inspectionService: import('../services/InspectionService.js').InspectionService }} services
 */
export function handler(args, { inspectionService }) {
  InputValidator.assert(args, {
    projectId: { type: 'string', required: true, minLength: 1 },
    clipId:    { type: 'string', required: true, minLength: 1 },
  });

  const clip = inspectionService.inspectClip(args.projectId, args.clipId);
  if (!clip) {
    throw new Error(`Clip not found: "${args.clipId}"`);
  }
  return clip;
}
