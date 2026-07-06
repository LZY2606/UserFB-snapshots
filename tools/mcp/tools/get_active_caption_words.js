/**
 * @module tools/get_active_caption_words
 * MCP tool: return words active at a given timeline time (karaoke / highlight queries).
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'get_active_caption_words',
  description:
    'Return the caption words that are active (visible) at a given absolute timeline time. ' +
    'Useful for building karaoke UIs, highlight queries, or verifying word timing.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId: { type: 'string', description: 'Project ID.' },
      clipId:    { type: 'string', description: 'Caption clip ID.' },
      time:      { type: 'number', minimum: 0, description: 'Absolute timeline time in seconds.' },
    },
    required: ['projectId', 'clipId', 'time'],
  },
};

export function handler(args, { captionService }) {
  InputValidator.assert(args, {
    projectId: { type: 'string', required: true, minLength: 1 },
    clipId:    { type: 'string', required: true, minLength: 1 },
    time:      { type: 'number', required: true },
  });

  return captionService.getActiveWords(args.projectId, args.clipId, args.time);
}
