/**
 * @module tools/remove_caption_animation
 * MCP tool: remove a caption animation by ID.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'remove_caption_animation',
  description:
    'Remove a caption animation from a caption clip by animation ID. ' +
    'Use inspect_caption to see current animation counts.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId:   { type: 'string', description: 'Project ID.' },
      clipId:      { type: 'string', description: 'Caption clip ID.' },
      animationId: { type: 'string', description: 'Animation ID to remove.' },
    },
    required: ['projectId', 'clipId', 'animationId'],
  },
};

export function handler(args, { captionService }) {
  InputValidator.assert(args, {
    projectId:   { type: 'string', required: true, minLength: 1 },
    clipId:      { type: 'string', required: true, minLength: 1 },
    animationId: { type: 'string', required: true, minLength: 1 },
  });

  return captionService.removeAnimation(args.projectId, args.clipId, args.animationId);
}
