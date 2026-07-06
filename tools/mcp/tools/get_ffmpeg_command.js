/**
 * @module tools/get_ffmpeg_command
 * MCP tool: build the FFmpeg command for a project without executing it.
 */

import { InputValidator } from '../validators/InputValidator.js';

const VALID_PRESETS = [
  'ultrafast', 'superfast', 'veryfast', 'faster', 'fast',
  'medium', 'slow', 'slower', 'veryslow',
];

export const definition = {
  name: 'get_ffmpeg_command',
  description:
    'Build the FFmpeg command that would be used to export a project to MP4, ' +
    'without executing it. Returns the full command line and args array. ' +
    'Useful for debugging, dry-runs, or running FFmpeg manually.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId: {
        type: 'string',
        description: 'Project ID.',
      },
      outputPath: {
        type: 'string',
        description: 'Output path to embed in the command. Default: "/dev/null" (dry-run).',
      },
      videoCodec:   { type: 'string', description: 'FFmpeg video codec. Default: "libx264".' },
      audioCodec:   { type: 'string', description: 'FFmpeg audio codec. Default: "aac".' },
      preset: {
        type: 'string',
        enum: VALID_PRESETS,
        description: 'Encoding preset. Default: "medium".',
      },
      crf:          { type: 'number', minimum: 0, maximum: 51, description: 'CRF quality (0–51). Default: 18.' },
      pixelFormat:  { type: 'string', description: 'Pixel format. Default: "yuv420p".' },
      videoBitrate: { type: 'string', description: 'Video bitrate (e.g. "8000k").' },
      audioBitrate: { type: 'string', description: 'Audio bitrate (e.g. "192k").' },
    },
    required: ['projectId'],
  },
};

/**
 * @param {object} args
 * @param {{ mp4ExportService: import('../services/Mp4ExportService.js').Mp4ExportService }} services
 */
export function handler(args, { mp4ExportService }) {
  InputValidator.assert(args, {
    projectId: { type: 'string', required: true, minLength: 1 },
  });

  const outputPath = args.outputPath ?? '/dev/null';
  const options    = {};
  if (args.videoCodec   !== undefined) options.videoCodec   = args.videoCodec;
  if (args.audioCodec   !== undefined) options.audioCodec   = args.audioCodec;
  if (args.preset       !== undefined) options.preset       = args.preset;
  if (args.crf          !== undefined) options.crf          = args.crf;
  if (args.pixelFormat  !== undefined) options.pixelFormat  = args.pixelFormat;
  if (args.videoBitrate !== undefined) options.videoBitrate = args.videoBitrate;
  if (args.audioBitrate !== undefined) options.audioBitrate = args.audioBitrate;

  return mp4ExportService.getFfmpegCommand(args.projectId, outputPath, options);
}
