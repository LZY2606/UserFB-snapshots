/**
 * @module tools/add_caption_animation
 * MCP tool: add a caption animation at the clip, line, word, or character level.
 */

import { InputValidator } from '../validators/InputValidator.js';

const ANIMATION_TYPES = [
  'fade', 'slide', 'scale', 'rotate', 'bounce', 'pop', 'pulse', 'shake',
  'wobble', 'wave', 'swing', 'flip', 'typewriter', 'karaoke', 'reveal',
  'scramble', 'elastic', 'glitch', 'highlight', 'zoom', 'blurReveal', 'stagger',
];

export const definition = {
  name: 'add_caption_animation',
  description:
    'Add a caption animation to a caption clip. ' +
    'Choose the target level: "caption" (whole clip), "lines" (each segment), ' +
    '"words" (each word), or "characters" (each character). ' +
    'Common animation types: fade, slide, scale, pop, typewriter, blurReveal, karaoke. ' +
    'All 22 types: ' + ANIMATION_TYPES.join(', ') + '.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId:     { type: 'string', description: 'Project ID.' },
      clipId:        { type: 'string', description: 'Caption clip ID.' },
      animationType: {
        type: 'string',
        enum: ANIMATION_TYPES,
        description: 'Animation type.',
      },
      target: {
        type: 'string',
        enum: ['caption', 'lines', 'words', 'characters'],
        description: 'Level at which to apply the animation. Default: "caption".',
      },

      // Common animation options
      duration:  { type: 'number', minimum: 0, description: 'Animation duration in seconds. Default: 0.5.' },
      delay:     { type: 'number', minimum: 0, description: 'Animation delay in seconds. Default: 0.' },
      easing:    { type: 'string', description: 'Easing: linear, easeIn, easeOut, easeInOut, spring, etc.' },
      loop:      { type: 'boolean', description: 'Loop the animation.' },
      reverse:   { type: 'boolean', description: 'Play animation in reverse.' },
      stagger:   { type: 'number', description: 'Per-element stagger delay in seconds.' },

      // Type-specific common options
      direction:  { type: 'string', enum: ['in', 'out', 'inOut', 'up', 'down', 'left', 'right'], description: 'Direction (fade: in/out/inOut; slide: up/down/left/right).' },
      distance:   { type: 'number', description: 'Slide distance in pixels.' },
      fromScale:  { type: 'number', description: 'Start scale (scale, pop, zoom animations).' },
      toScale:    { type: 'number', description: 'End scale.' },
      intensity:  { type: 'number', description: 'Effect intensity (shake, glitch).' },
      amplitude:  { type: 'number', description: 'Wave amplitude in pixels.' },
      color:      { type: 'string', description: 'Highlight or karaoke fill color.' },
      fillColor:  { type: 'string', description: 'Karaoke fill color.' },
      fillStyle:  { type: 'string', enum: ['leftToRight', 'word', 'character'], description: 'Karaoke fill style.' },
      fromBlur:   { type: 'number', description: 'Starting blur radius (blurReveal).' },
      toBlur:     { type: 'number', description: 'Ending blur radius (blurReveal).' },
      overshoot:  { type: 'number', description: 'Overshoot scale for pop/elastic.' },
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

  const target  = args.target ?? 'caption';
  const options = {};
  const optFields = [
    'duration', 'delay', 'easing', 'loop', 'reverse', 'stagger',
    'direction', 'distance', 'fromScale', 'toScale', 'intensity', 'amplitude',
    'color', 'fillColor', 'fillStyle', 'fromBlur', 'toBlur', 'overshoot',
  ];
  for (const f of optFields) {
    if (args[f] !== undefined) options[f] = args[f];
  }

  const result = captionService.addAnimation(
    args.projectId, args.clipId, args.animationType, target, options
  );
  return {
    ...result,
    message: `Animation "${result.animationType}" added to "${result.target}" level of clip "${result.clipId}".`,
  };
}
