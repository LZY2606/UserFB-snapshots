/**
 * @module tools/build_caption_karaoke
 * MCP tool: configure a caption clip for karaoke playback.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'build_caption_karaoke',
  description:
    'Configure a caption clip for karaoke-style playback. ' +
    'Words are progressively filled with color as they are spoken. ' +
    'Requires word timings to be set via set_caption_transcript.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId:    { type: 'string', description: 'Project ID.' },
      clipId:       { type: 'string', description: 'Caption clip ID.' },
      fillColor: {
        type: 'string',
        description: 'Karaoke fill color applied to spoken words. Default: "#FFD700" (gold).',
      },
      fillStyle: {
        type: 'string',
        enum: ['leftToRight', 'word', 'character'],
        description: '"leftToRight" = progressive fill, "word" = whole-word pop, "character" = per-character fill. Default: leftToRight.',
      },
      highlightBar: {
        type: 'boolean',
        description: 'Show a progress bar beneath the active word. Default: true.',
      },
    },
    required: ['projectId', 'clipId'],
  },
};

export function handler(args, { captionService }) {
  InputValidator.assert(args, {
    projectId: { type: 'string', required: true, minLength: 1 },
    clipId:    { type: 'string', required: true, minLength: 1 },
  });

  const options = {};
  if (args.fillColor    !== undefined) options.fillColor    = args.fillColor;
  if (args.fillStyle    !== undefined) options.fillStyle    = args.fillStyle;
  if (args.highlightBar !== undefined) options.highlightBar = args.highlightBar;

  const result = captionService.buildKaraoke(args.projectId, args.clipId, options);
  return {
    ...result,
    message: `Karaoke mode configured for clip "${result.clipId}".`,
  };
}
