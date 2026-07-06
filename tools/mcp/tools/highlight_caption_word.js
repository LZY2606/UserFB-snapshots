/**
 * @module tools/highlight_caption_word
 * MCP tool: highlight a word by its global index across all segments.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'highlight_caption_word',
  description:
    'Highlight a specific word in a caption clip by its global word index ' +
    '(0-based, counting across all segments). ' +
    'Optionally override the highlight color (default: gold #FFD700).',
  inputSchema: {
    type: 'object',
    properties: {
      projectId:  { type: 'string',  description: 'Project ID.' },
      clipId:     { type: 'string',  description: 'Caption clip ID.' },
      wordIndex:  { type: 'number',  minimum: 0, description: 'Global word index (0-based).' },
      fillColor:  { type: 'string',  description: 'Highlight fill color. Default: "#FFD700" (gold).' },
    },
    required: ['projectId', 'clipId', 'wordIndex'],
  },
};

export function handler(args, { captionService }) {
  InputValidator.assert(args, {
    projectId: { type: 'string', required: true, minLength: 1 },
    clipId:    { type: 'string', required: true, minLength: 1 },
    wordIndex: { type: 'number', required: true },
  });

  const styleOverride = {};
  if (args.fillColor !== undefined) styleOverride.fill = args.fillColor;

  return captionService.highlightWord(
    args.projectId, args.clipId, args.wordIndex, styleOverride
  );
}
