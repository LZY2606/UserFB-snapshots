/**
 * @module tools/reorder_tracks
 * MCP tool: reorder tracks on a project by supplying the desired ID sequence.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'reorder_tracks',
  description:
    'Reorder tracks on a project by supplying an array of track IDs in the desired order. ' +
    'Track IDs absent from the array are appended at the end in their original order.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId: { type: 'string', description: 'Project ID.' },
      orderedIds: {
        type: 'array',
        items: { type: 'string' },
        description: 'Track IDs in the desired order (bottom → top).',
      },
    },
    required: ['projectId', 'orderedIds'],
  },
};

/**
 * @param {object} args
 * @param {{ trackService: import('../services/TrackService.js').TrackService }} services
 */
export function handler(args, { trackService }) {
  InputValidator.assert(args, {
    projectId: { type: 'string', required: true, minLength: 1 },
  });

  if (!Array.isArray(args.orderedIds) || args.orderedIds.length === 0) {
    throw new Error('"orderedIds" must be a non-empty array of track ID strings.');
  }

  const tracks = trackService.reorderTracks(args.projectId, args.orderedIds);
  return {
    tracks,
    message: `Tracks reordered. New order: ${tracks.map((t) => `"${t.name}"`).join(', ')}.`,
  };
}
