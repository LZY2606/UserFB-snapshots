/**
 * @module tools/set_video_options
 * MCP tool: set video clip playback and audio options.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'set_video_options',
  description:
    'Set playback and audio options on a video clip: volume (0–2), muted, ' +
    'playbackRate (speed multiplier), reversed.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId:    { type: 'string', description: 'Project ID.' },
      clipId:       { type: 'string', description: 'Video clip ID.' },
      volumeLevel:  { type: 'number', minimum: 0, maximum: 2, description: 'Volume level (0–2). 1 = original.' },
      muted:        { type: 'boolean', description: 'Mute or unmute the clip.' },
      playbackRate: { type: 'number', minimum: 0, description: 'Playback speed multiplier. 1 = normal, 2 = 2× speed.' },
      reversed:     { type: 'boolean', description: 'Play the clip in reverse.' },
    },
    required: ['projectId', 'clipId'],
  },
};

/**
 * @param {object} args
 * @param {{ clipStyleService: import('../services/ClipStyleService.js').ClipStyleService }} services
 */
export function handler(args, { clipStyleService }) {
  InputValidator.assert(args, {
    projectId: { type: 'string', required: true, minLength: 1 },
    clipId:    { type: 'string', required: true, minLength: 1 },
  });

  return clipStyleService.setVideoOptions(args.projectId, args.clipId, {
    volumeLevel:  args.volumeLevel,
    muted:        args.muted,
    playbackRate: args.playbackRate,
    reversed:     args.reversed,
  });
}
