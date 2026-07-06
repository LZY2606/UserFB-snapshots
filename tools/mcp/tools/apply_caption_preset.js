/**
 * @module tools/apply_caption_preset
 * MCP tool: apply one of the 10 built-in caption presets.
 */

import { InputValidator } from '../validators/InputValidator.js';

const PRESET_NAMES = [
  'hormozi', 'mrbeast', 'podcast', 'news', 'documentary',
  'karaoke', 'minimal', 'gaming', 'luxury', 'corporate',
];

export const definition = {
  name: 'apply_caption_preset',
  description:
    'Apply a built-in caption preset to a caption clip. ' +
    'A preset sets the style, layout, animations, and effects all at once. ' +
    'Use list_caption_presets to see descriptions. ' +
    'Available: hormozi, mrbeast, podcast, news, documentary, karaoke, minimal, gaming, luxury, corporate.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId:  { type: 'string', description: 'Project ID.' },
      clipId:     { type: 'string', description: 'Caption clip ID.' },
      presetName: {
        type: 'string',
        enum: PRESET_NAMES,
        description: 'Name of the preset to apply.',
      },
    },
    required: ['projectId', 'clipId', 'presetName'],
  },
};

export function handler(args, { captionService }) {
  InputValidator.assert(args, {
    projectId:  { type: 'string', required: true, minLength: 1 },
    clipId:     { type: 'string', required: true, minLength: 1 },
    presetName: { type: 'string', required: true, minLength: 1 },
  });

  const result = captionService.applyPreset(args.projectId, args.clipId, args.presetName);
  return {
    ...result,
    message: `Preset "${result.presetName}" applied to clip "${result.clipId}".`,
  };
}
