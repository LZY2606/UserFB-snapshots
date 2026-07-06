/**
 * @module tools/inspect_caption
 * MCP tool: inspect a caption clip's full state.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'inspect_caption',
  description:
    'Return full details for a caption clip: transcript, segments, word count, ' +
    'style summary, layout summary, animation count, effect count, and keyframe state.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId: { type: 'string', description: 'Project ID.' },
      clipId:    { type: 'string', description: 'Caption clip ID.' },
    },
    required: ['projectId', 'clipId'],
  },
};

export function handler(args, { captionService }) {
  InputValidator.assert(args, {
    projectId: { type: 'string', required: true, minLength: 1 },
    clipId:    { type: 'string', required: true, minLength: 1 },
  });

  return captionService.inspectCaption(args.projectId, args.clipId);
}
