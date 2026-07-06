/**
 * @module tools/export_mp4
 * MCP tool: export a VideoForge project to an MP4 file via FFmpeg.
 */

import { InputValidator } from '../validators/InputValidator.js';

const VALID_PRESETS = [
  'ultrafast', 'superfast', 'veryfast', 'faster', 'fast',
  'medium', 'slow', 'slower', 'veryslow',
];

export const definition = {
  name: 'export_mp4',
  description:
    'Export a VideoForge project to an MP4 file by running FFmpeg. ' +
    'Requires FFmpeg to be installed on the system. ' +
    'Returns an exportId that can be used with inspect_mp4_export. ' +
    'Supported clip types: video, audio, image. Text, shape, and caption clips are skipped by the renderer.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId: {
        type: 'string',
        description: 'Project ID to export.',
      },
      outputPath: {
        type: 'string',
        description: 'Absolute path for the output .mp4 file (e.g. "/tmp/output.mp4").',
      },
      videoCodec: {
        type: 'string',
        description: 'FFmpeg video codec. Default: "libx264".',
      },
      audioCodec: {
        type: 'string',
        description: 'FFmpeg audio codec. Default: "aac".',
      },
      preset: {
        type: 'string',
        enum: VALID_PRESETS,
        description: 'FFmpeg encoding preset (speed vs quality). Default: "medium".',
      },
      crf: {
        type: 'number',
        minimum: 0,
        maximum: 51,
        description: 'Constant Rate Factor — quality (0 = lossless, 51 = worst). Default: 18.',
      },
      pixelFormat: {
        type: 'string',
        description: 'Pixel format. Default: "yuv420p".',
      },
      videoBitrate: {
        type: 'string',
        description: 'Target video bitrate (e.g. "8000k"). Overrides CRF if set.',
      },
      audioBitrate: {
        type: 'string',
        description: 'Target audio bitrate (e.g. "192k"). Default: "192k".',
      },
    },
    required: ['projectId', 'outputPath'],
  },
};

/**
 * @param {object} args
 * @param {{ mp4ExportService: import('../services/Mp4ExportService.js').Mp4ExportService }} services
 */
export async function handler(args, { mp4ExportService }) {
  InputValidator.assert(args, {
    projectId:  { type: 'string', required: true, minLength: 1 },
    outputPath: { type: 'string', required: true, minLength: 1 },
  });

  const options = {};
  if (args.videoCodec   !== undefined) options.videoCodec   = args.videoCodec;
  if (args.audioCodec   !== undefined) options.audioCodec   = args.audioCodec;
  if (args.preset       !== undefined) options.preset       = args.preset;
  if (args.crf          !== undefined) options.crf          = args.crf;
  if (args.pixelFormat  !== undefined) options.pixelFormat  = args.pixelFormat;
  if (args.videoBitrate !== undefined) options.videoBitrate = args.videoBitrate;
  if (args.audioBitrate !== undefined) options.audioBitrate = args.audioBitrate;

  const result = await mp4ExportService.exportMp4(args.projectId, args.outputPath, options);
  return {
    ...result,
    message: result.success
      ? `MP4 exported successfully to "${result.output}" (${(result.fileSize / 1024 / 1024).toFixed(2)} MB, ${result.duration.toFixed(2)}s).`
      : `MP4 export failed for project "${args.projectId}".`,
  };
}
