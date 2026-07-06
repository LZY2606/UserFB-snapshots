/**
 * @module tools/add_chroma_key_effect
 * MCP tool: remove a solid background colour (green/blue screen) from a clip.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'add_chroma_key_effect',
  description:
    'Remove a solid background colour (green screen, blue screen, or any flat backdrop) ' +
    'from a video clip, making matching pixels transparent. ' +
    'Works on video clips and is applied by FFmpeg\'s chromakey filter on MP4 export. ' +
    'Returns the chroma key effect ID for later removal.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId: { type: 'string', description: 'Project ID.' },
      clipId:    { type: 'string', description: 'Clip ID to key.' },
      color: {
        type: 'string',
        description: 'Background colour to remove (hex, e.g. "#00FF00", or CSS colour name). Default: "#00FF00".',
      },
      similarity: {
        type: 'number',
        minimum: 0,
        maximum: 1,
        description:
          'How close a pixel\'s colour must be to `color` to be keyed out, in [0, 1]. ' +
          'Higher values key out a wider range of shades. Default: 0.10.',
      },
      blend: {
        type: 'number',
        minimum: 0,
        maximum: 1,
        description:
          'Softness of the key edge, in [0, 1]. Higher values produce a smoother ' +
          'transition between opaque and transparent pixels. Default: 0.10.',
      },
      yuv: {
        type: 'boolean',
        description:
          'Match colour similarity in YUV space instead of RGB. Generally gives cleaner ' +
          'edges on compressed (YUV-encoded) source footage. Default: false.',
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

  const result = clipEditingService.addChromaKey(args.projectId, args.clipId, {
    color:      args.color,
    similarity: args.similarity,
    blend:      args.blend,
    yuv:        args.yuv,
  });

  return {
    ...result,
    message:
      `Chroma key effect (id: ${result.effectId}) added to clip "${result.clipId}" — ` +
      `color:${result.params.color} similarity:${result.params.similarity} ` +
      `blend:${result.params.blend} yuv:${result.params.yuv}.`,
  };
}
