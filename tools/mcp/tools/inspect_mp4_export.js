/**
 * @module tools/inspect_mp4_export
 * MCP tool: retrieve the result of a previously executed MP4 export.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'inspect_mp4_export',
  description:
    'Retrieve the result of a previously executed export_mp4 call. ' +
    'Returns success status, output path, file size, duration, and encoding options used.',
  inputSchema: {
    type: 'object',
    properties: {
      exportId: {
        type: 'string',
        description: 'Export ID returned by export_mp4.',
      },
    },
    required: ['exportId'],
  },
};

/**
 * @param {object} args
 * @param {{ mp4ExportService: import('../services/Mp4ExportService.js').Mp4ExportService }} services
 */
export function handler(args, { mp4ExportService }) {
  InputValidator.assert(args, {
    exportId: { type: 'string', required: true, minLength: 1 },
  });

  return mp4ExportService.getExportResult(args.exportId);
}
