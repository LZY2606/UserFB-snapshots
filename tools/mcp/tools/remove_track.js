/**
 * @module tools/remove_track
 * MCP tool: remove a track (and all its clips) from a project.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'remove_track',
  description: 'Remove a track and all its clips from a VideoForge project.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId: { type: 'string', description: 'Project ID.' },
      trackId:   { type: 'string', description: 'Track ID to remove.' },
    },
    required: ['projectId', 'trackId'],
  },
};

/**
 * @param {object} args
 * @param {{ trackService: import('../services/TrackService.js').TrackService }} services
 */
export function handler(args, { trackService }) {
  InputValidator.assert(args, {
    projectId: { type: 'string', required: true, minLength: 1 },
    trackId:   { type: 'string', required: true, minLength: 1 },
  });

  const result = trackService.removeTrack(args.projectId, args.trackId);
  return {
    ...result,
    message: `Track "${result.trackId}" removed.`,
  };
}
