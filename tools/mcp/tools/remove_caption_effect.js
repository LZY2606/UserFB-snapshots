/**
 * @module tools/remove_caption_effect
 * MCP tool: remove a caption effect by ID.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'remove_caption_effect',
  description:
    'Remove a caption effect from a caption clip by effect ID. ' +
    'Use inspect_caption to see current effect counts.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId: { type: 'string', description: 'Project ID.' },
      clipId:    { type: 'string', description: 'Caption clip ID.' },
      effectId:  { type: 'string', description: 'Effect ID to remove.' },
    },
    required: ['projectId', 'clipId', 'effectId'],
  },
};

export function handler(args, { captionService }) {
  InputValidator.assert(args, {
    projectId: { type: 'string', required: true, minLength: 1 },
    clipId:    { type: 'string', required: true, minLength: 1 },
    effectId:  { type: 'string', required: true, minLength: 1 },
  });

  return captionService.removeEffect(args.projectId, args.clipId, args.effectId);
}
