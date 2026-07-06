/**
 * @module tools/clear_caption_highlights
 * MCP tool: clear all word highlights on a caption clip.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'clear_caption_highlights',
  description: 'Clear all word highlights on a caption clip.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId: { type: 'string', description: 'Project ID.' },
      clipId:    { type: 'string', description: 'Caption clip ID.' },
    },
    required: ['projectId', 'clipId'],
  },
};

export function handler(args, { captionService }) {
  InputValidator.assert(args, {
    projectId: { type: 'string', required: true, minLength: 1 },
    clipId:    { type: 'string', required: true, minLength: 1 },
  });

  return captionService.clearHighlights(args.projectId, args.clipId);
}
