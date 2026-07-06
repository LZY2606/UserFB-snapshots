/**
 * @module tools/set_audio_options
 * MCP tool: set audio clip controls.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'set_audio_options',
  description:
    'Set audio controls on an audio clip: volume (0–2), pan (-1 to +1), ' +
    'playbackRate (speed multiplier), muted.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId:    { type: 'string', description: 'Project ID.' },
      clipId:       { type: 'string', description: 'Audio clip ID.' },
      volumeLevel:  { type: 'number', minimum: 0, maximum: 2, description: 'Volume level (0–2). 1 = original.' },
      panValue:     { type: 'number', minimum: -1, maximum: 1, description: 'Stereo pan: -1 = left, 0 = center, +1 = right.' },
      playbackRate: { type: 'number', minimum: 0, description: 'Playback speed multiplier.' },
      muted:        { type: 'boolean', description: 'Mute or unmute the clip.' },
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

  return clipStyleService.setAudioOptions(args.projectId, args.clipId, {
    volumeLevel:  args.volumeLevel,
    panValue:     args.panValue,
    playbackRate: args.playbackRate,
    muted:        args.muted,
  });
}
