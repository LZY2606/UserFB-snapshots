/**
 * @module tools/add_transition
 * MCP tool: add a transition effect to a clip.
 */

import { InputValidator } from '../validators/InputValidator.js';

const VALID_TRANSITION_TYPES = [
  'crossDissolve',
  'wipeLeft',
  'wipeRight',
  'wipeUp',
  'wipeDown',
  'slide',
  'zoom',
  'dipToBlack',
  'dipToWhite',
];

export const definition = {
  name: 'add_transition',
  description:
    'Add a transition effect to a clip. The transition is attached to the specified clip. ' +
    'Optionally supply fromClipId and toClipId to link adjacent clips for exporter metadata. ' +
    'Valid transition types: crossDissolve, wipeLeft, wipeRight, wipeUp, wipeDown, slide, zoom, dipToBlack, dipToWhite.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId: { type: 'string', description: 'Project ID.' },
      clipId:    { type: 'string', description: 'Clip to attach the transition to.' },
      transitionType: {
        type: 'string',
        enum: VALID_TRANSITION_TYPES,
        description: 'Transition type. Default: crossDissolve.',
      },
      duration: {
        type: 'number',
        minimum: 0,
        description: 'Transition duration in seconds. Default: 1.',
      },
      fromClipId: {
        type: 'string',
        description: 'ID of the outgoing clip (optional — used to link the transition for exporters).',
      },
      toClipId: {
        type: 'string',
        description: 'ID of the incoming clip (optional — used to link the transition for exporters).',
      },
      easing: {
        type: 'string',
        enum: ['linear', 'easeIn', 'easeOut', 'easeInOut'],
        description: 'Easing curve. Default: easeInOut.',
      },
    },
    required: ['projectId', 'clipId'],
  },
};

/**
 * @param {object} args
 * @param {{ clipEditingService: import('../services/ClipEditingService.js').ClipEditingService }} services
 */
export function handler(args, { clipEditingService }) {
  InputValidator.assert(args, {
    projectId: { type: 'string', required: true, minLength: 1 },
    clipId:    { type: 'string', required: true, minLength: 1 },
  });

  const transitionType = args.transitionType ?? 'crossDissolve';
  const duration       = args.duration ?? 1;
  const options        = {};
  if (args.easing)     options.easing     = args.easing;
  if (args.fromClipId) options.fromClipId = args.fromClipId;
  if (args.toClipId)   options.toClipId   = args.toClipId;

  const result = clipEditingService.addTransition(
    args.projectId, args.clipId, transitionType, duration, options
  );
  return {
    ...result,
    message: `Transition "${result.transitionType}" (${result.duration}s) added to clip "${result.clipId}".`,
  };
}
