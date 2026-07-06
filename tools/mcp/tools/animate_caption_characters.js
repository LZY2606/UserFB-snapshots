/**
 * @module tools/animate_caption_characters
 * MCP tool: apply an animation to all characters across all segments of a caption clip.
 */

import { InputValidator } from '../validators/InputValidator.js';

const ANIMATION_TYPES = [
  'fade', 'slide', 'scale', 'rotate', 'bounce', 'pop', 'pulse', 'shake',
  'wobble', 'wave', 'swing', 'flip', 'typewriter', 'karaoke', 'reveal',
  'scramble', 'elastic', 'glitch', 'highlight', 'zoom', 'blurReveal', 'stagger',
];

export const definition = {
  name: 'animate_caption_characters',
  description:
    'Apply an animation to every character across all segments of a caption clip. ' +
    'Ideal for typewriter, scramble, wave, and per-character effects. ' +
    'Use "stagger" to cascade the animation across characters. ' +
    'Animation types: ' + ANIMATION_TYPES.join(', ') + '.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId:     { type: 'string', description: 'Project ID.' },
      clipId:        { type: 'string', description: 'Caption clip ID.' },
      animationType: { type: 'string', enum: ANIMATION_TYPES, description: 'Animation type.' },
      duration:  { type: 'number', minimum: 0, description: 'Animation duration per character in seconds.' },
      delay:     { type: 'number', minimum: 0, description: 'Initial delay before first character animates.' },
      stagger:   { type: 'number', minimum: 0, description: 'Per-character delay in seconds. Default: 0.03.' },
      easing:    { type: 'string', description: 'Easing curve.' },
      loop:      { type: 'boolean', description: 'Loop the animation.' },
      direction: { type: 'string', description: 'Direction (fade: in/out; slide: up/down/left/right).' },
    },
    required: ['projectId', 'clipId', 'animationType'],
  },
};

export function handler(args, { captionService }) {
  InputValidator.assert(args, {
    projectId:     { type: 'string', required: true, minLength: 1 },
    clipId:        { type: 'string', required: true, minLength: 1 },
    animationType: { type: 'string', required: true, minLength: 1 },
  });

  const options = {};
  for (const f of ['duration', 'delay', 'stagger', 'easing', 'loop', 'direction']) {
    if (args[f] !== undefined) options[f] = args[f];
  }

  return captionService.animateCharacters(args.projectId, args.clipId, args.animationType, options);
}
