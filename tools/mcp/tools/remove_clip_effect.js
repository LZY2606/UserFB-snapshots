/**
 * @module tools/remove_clip_effect
 * MCP tool: remove any effect (fade, transition, crop) from a clip by effect ID.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'remove_clip_effect',
  description:
    'Remove an effect from a clip by its effect ID. ' +
    'Works for any clip-level effect: fade-in, fade-out, transition, crop, etc. ' +
    'Use inspect_clip to see the current effects array with their IDs.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId: { type: 'string', description: 'Project ID.' },
      clipId:    { type: 'string', description: 'Clip ID.' },
      effectId:  { type: 'string', description: 'Effect ID to remove (from inspect_clip effects array).' },
    },
    required: ['projectId', 'clipId', 'effectId'],
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
    effectId:  { type: 'string', required: true, minLength: 1 },
  });

  const result = clipEditingService.removeClipEffect(
    args.projectId, args.clipId, args.effectId
  );
  return {
    ...result,
    message: `Effect "${result.effectId}" removed from clip "${result.clipId}". ${result.totalEffects} effect(s) remaining.`,
  };
}
