/**
 * @module tools/add_caption_effect
 * MCP tool: add a CaptionEffect to a caption clip.
 */

import { InputValidator } from '../validators/InputValidator.js';

const EFFECT_TYPES = [
  'glow', 'shadow', 'outline', 'gradient', 'neon', 'glass', 'blur', 'motionBlur',
  'backgroundBox', 'roundedBox', 'highlight', 'underline', 'strikethrough',
  'noise', 'grain', 'chromaticAberration', 'bloom', 'distortion', 'reflection',
];

export const definition = {
  name: 'add_caption_effect',
  description:
    'Add a visual effect to a caption clip. ' +
    'Available types: ' + EFFECT_TYPES.join(', ') + '. ' +
    'Common params: color (for glow/shadow/outline), blur (for glow/blur), ' +
    'width (for outline), offsetX/offsetY (for shadow), stops (for gradient), ' +
    'padding/borderRadius (for boxes).',
  inputSchema: {
    type: 'object',
    properties: {
      projectId:  { type: 'string', description: 'Project ID.' },
      clipId:     { type: 'string', description: 'Caption clip ID.' },
      effectType: {
        type: 'string',
        enum: EFFECT_TYPES,
        description: 'Effect type.',
      },

      // Common effect params
      color:        { type: 'string',  description: 'Effect color (glow, shadow, outline, highlight, etc.).' },
      blur:         { type: 'number',  minimum: 0, description: 'Blur/glow radius.' },
      width:        { type: 'number',  minimum: 0, description: 'Outline width in pixels.' },
      offsetX:      { type: 'number',  description: 'Shadow X offset in pixels.' },
      offsetY:      { type: 'number',  description: 'Shadow Y offset in pixels.' },
      spread:       { type: 'number',  description: 'Shadow spread.' },
      opacity:      { type: 'number',  minimum: 0, maximum: 1, description: 'Effect opacity.' },
      strength:     { type: 'number',  minimum: 0, maximum: 1, description: 'Effect strength (0–1).' },
      intensity:    { type: 'number',  minimum: 0, description: 'Effect intensity.' },
      padding:      { type: 'number',  minimum: 0, description: 'Background box padding in pixels.' },
      borderRadius: { type: 'number',  minimum: 0, description: 'Background box corner radius.' },
      amount:       { type: 'number',  minimum: 0, description: 'Blur amount or noise amount.' },
      glowColor:    { type: 'string',  description: 'Secondary glow color (neon effect).' },
    },
    required: ['projectId', 'clipId', 'effectType'],
  },
};

export function handler(args, { captionService }) {
  InputValidator.assert(args, {
    projectId:  { type: 'string', required: true, minLength: 1 },
    clipId:     { type: 'string', required: true, minLength: 1 },
    effectType: { type: 'string', required: true, minLength: 1 },
  });

  const params = {};
  const paramFields = [
    'color', 'blur', 'width', 'offsetX', 'offsetY', 'spread',
    'opacity', 'strength', 'intensity', 'padding', 'borderRadius',
    'amount', 'glowColor',
  ];
  for (const f of paramFields) {
    if (args[f] !== undefined) params[f] = args[f];
  }

  const result = captionService.addEffect(args.projectId, args.clipId, args.effectType, params);
  return {
    ...result,
    message: `Effect "${result.effectType}" (id: ${result.effectId}) added to clip "${result.clipId}".`,
  };
}
