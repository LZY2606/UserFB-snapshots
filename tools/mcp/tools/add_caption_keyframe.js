/**
 * @module tools/add_caption_keyframe
 * MCP tool: add a keyframe to a caption clip's keyframe set.
 */

import { InputValidator } from '../validators/InputValidator.js';

const KEYFRAMEABLE_PROPERTIES = [
  'x', 'y', 'scaleX', 'scaleY', 'rotation', 'opacity', 'blur',
  'color', 'fill', 'stroke', 'strokeWidth',
  'shadowBlur', 'shadowOffsetX', 'shadowOffsetY',
  'letterSpacing', 'tracking', 'lineHeight', 'backgroundOpacity',
  'glowBlur', 'glowStrength', 'fontSize', 'skewX', 'skewY',
];

const EASING_TYPES = [
  'linear', 'easeIn', 'easeOut', 'easeInOut',
  'easeInBack', 'easeOutBack', 'easeInOutBack',
  'easeInBounce', 'easeOutBounce',
  'easeInElastic', 'easeOutElastic',
  'spring', 'snap', 'overshoot',
];

export const definition = {
  name: 'add_caption_keyframe',
  description:
    'Add a keyframe to a caption clip\'s keyframe set. ' +
    'Keyframes animate clip-level properties over time. ' +
    'Keyframeable properties: ' + KEYFRAMEABLE_PROPERTIES.join(', ') + '.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId: { type: 'string', description: 'Project ID.' },
      clipId:    { type: 'string', description: 'Caption clip ID.' },
      property: {
        type: 'string',
        enum: KEYFRAMEABLE_PROPERTIES,
        description: 'Property to animate.',
      },
      time:  { type: 'number', minimum: 0, description: 'Keyframe time in seconds (relative to clip start).' },
      value: { description: 'Target value at this keyframe (number or color string depending on property).' },
      easing: {
        type: 'string',
        enum: EASING_TYPES,
        description: 'Easing curve from this keyframe to the next. Default: linear.',
      },
    },
    required: ['projectId', 'clipId', 'property', 'time', 'value'],
  },
};

export function handler(args, { captionService }) {
  InputValidator.assert(args, {
    projectId: { type: 'string', required: true, minLength: 1 },
    clipId:    { type: 'string', required: true, minLength: 1 },
    property:  { type: 'string', required: true, minLength: 1 },
    time:      { type: 'number', required: true },
  });

  const easing = args.easing ?? 'linear';
  return captionService.addKeyframe(
    args.projectId, args.clipId, args.property, args.time, args.value, easing
  );
}
