/**
 * @module tools/export_edl
 * MCP tool: export a VideoForge project as a CMX3600 EDL.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'export_edl',
  description:
    'Export a VideoForge project as a CMX3600 Edit Decision List (EDL). ' +
    'Returns an exportId usable with inspect_export and validate_export.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId: {
        type: 'string',
        description: 'Project ID to export.',
      },
      title: {
        type: 'string',
        description: 'EDL title line override (defaults to project name).',
      },
      includeAudio: {
        type: 'boolean',
        description: 'Include audio events in the EDL (default: true).',
      },
    },
    required: ['projectId'],
  },
};

/**
 * @param {object} args
 * @param {{ exportService: import('../services/ExportService.js').ExportService }} services
 * @returns {object}
 */
export function handler(args, { exportService }) {
  InputValidator.assert(args, {
    projectId: { type: 'string', required: true, minLength: 1 },
  });

  const result = exportService.exportEdl(args.projectId, {
    title:        args.title,
    includeAudio: args.includeAudio,
  });

  return {
    exportId:      result.exportId,
    format:        result.format,
    contentLength: result.content.length,
    message:
      `EDL generated (${result.content.length} bytes). ` +
      `Use exportId "${result.exportId}" with inspect_export or validate_export.`,
  };
}
