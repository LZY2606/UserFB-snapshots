/**
 * @module tools/highlight_caption_keywords
 * MCP tool: highlight all words matching any of the given keywords.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'highlight_caption_keywords',
  description:
    'Highlight all words in a caption clip that match any of the supplied keywords. ' +
    'Matching is case-insensitive. Optionally override the highlight color.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId: { type: 'string', description: 'Project ID.' },
      clipId:    { type: 'string', description: 'Caption clip ID.' },
      keywords: {
        type: 'array',
        items: { type: 'string' },
        minItems: 1,
        description: 'Keywords to highlight (case-insensitive).',
      },
      fillColor: { type: 'string', description: 'Highlight fill color. Default: "#FFD700" (gold).' },
    },
    required: ['projectId', 'clipId', 'keywords'],
  },
};

export function handler(args, { captionService }) {
  InputValidator.assert(args, {
    projectId: { type: 'string', required: true, minLength: 1 },
    clipId:    { type: 'string', required: true, minLength: 1 },
  });

  if (!Array.isArray(args.keywords) || args.keywords.length === 0) {
    throw new Error('"keywords" must be a non-empty array of strings.');
  }

  const styleOverride = {};
  if (args.fillColor !== undefined) styleOverride.fill = args.fillColor;

  return captionService.highlightKeywords(
    args.projectId, args.clipId, args.keywords, styleOverride
  );
}
