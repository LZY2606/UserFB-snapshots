/**
 * @module tools/add_track
 * MCP tool: add a track to an existing VideoForge project.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'add_track',
  description:
    'Add a new track (video, audio, image, text, or shape) to a VideoForge project. ' +
    'Returns the new trackId.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId: { type: 'string',  description: 'Project ID.' },
      type: {
        type: 'string',
        description: 'Track type: "video" | "audio" | "image" | "text" | "shape".',
        enum: ['video', 'audio', 'image', 'text', 'shape'],
      },
      name:    { type: 'string',  description: 'Human-readable track label (optional).' },
      volume:  { type: 'number',  description: 'Master volume 0–2 (default: 1).' },
      muted:   { type: 'boolean', description: 'Start muted (default: false).' },
      solo:    { type: 'boolean', description: 'Solo mode (default: false).' },
      locked:  { type: 'boolean', description: 'Prevent edits (default: false).' },
      visible: { type: 'boolean', description: 'Track visibility (default: true).' },
    },
    required: ['projectId', 'type'],
  },
};

/**
 * @param {object} args
 * @param {{ trackService: import('../services/TrackService.js').TrackService }} services
 */
export function handler(args, { trackService }) {
  InputValidator.assert(args, {
    projectId: { type: 'string', required: true, minLength: 1 },
    type: {
      type: 'string',
      required: true,
      enum: ['video', 'audio', 'image', 'text', 'shape'],
    },
    volume:  { type: 'number',  min: 0, max: 2 },
  });

  const result = trackService.addTrack(args.projectId, args.type, {
    name:    args.name,
    volume:  args.volume,
    muted:   args.muted,
    solo:    args.solo,
    locked:  args.locked,
    visible: args.visible,
  });

  return {
    ...result,
    message: `Track "${result.name}" (${result.type}) added at index ${result.index}. trackId: "${result.trackId}".`,
  };
}
