/**
 * @module tools/add_fade
 * MCP tool: add a fade-in or fade-out effect to a clip.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'add_fade',
  description:
    'Add a fade-in or fade-out effect to a clip. ' +
    'Direction must be "in" or "out". Duration defaults to 1 second.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId: { type: 'string', description: 'Project ID.' },
      clipId:    { type: 'string', description: 'Clip ID to add the fade to.' },
      direction: {
        type: 'string',
        enum: ['in', 'out'],
        description: '"in" for fade-in at the clip start, "out" for fade-out at the clip end.',
      },
      duration: {
        type: 'number',
        minimum: 0,
        description: 'Fade duration in seconds. Default: 1.',
      },
      easing: {
        type: 'string',
        enum: ['linear', 'easeIn', 'easeOut', 'easeInOut'],
        description: 'Easing curve for the fade. Default: linear.',
      },
    },
    required: ['projectId', 'clipId', 'direction'],
  },
};

/**
 * @param {object} args
 * @param {{ clipEditingService: import('../services/ClipEditingService.js').ClipEditingService }} services
 */
export function handler(args, { clipEditingService }) {
  InputValidator.assert(args, {
    projectId: { type: 'string', required: true, minLength: 1 },
    clipId:    { type: 'string', required: true, minLength: 1 },
    direction: { type: 'string', required: true },
  });

  const duration = args.duration ?? 1;
  const options  = {};
  if (args.easing) options.easing = args.easing;

  const result = clipEditingService.addFade(
    args.projectId, args.clipId, args.direction, duration, options
  );
  return {
    ...result,
    message: `Fade-${result.direction} (${result.duration}s) added to clip "${result.clipId}".`,
  };
}
