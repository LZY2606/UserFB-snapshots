/**
 * @module tools/inspect_track
 * MCP tool: return detailed info for a single track, including all its clips.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'inspect_track',
  description:
    'Inspect a track — returns volume, muted, locked, visible, duration, ' +
    'clip count, and full per-clip detail.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId: { type: 'string', description: 'Project ID.' },
      trackId:   { type: 'string', description: 'Track ID to inspect.' },
    },
    required: ['projectId', 'trackId'],
  },
};

/**
 * @param {object} args
 * @param {{ inspectionService: import('../services/InspectionService.js').InspectionService }} services
 */
export function handler(args, { inspectionService }) {
  InputValidator.assert(args, {
    projectId: { type: 'string', required: true, minLength: 1 },
    trackId:   { type: 'string', required: true, minLength: 1 },
  });

  return inspectionService.inspectTrack(args.projectId, args.trackId);
}
