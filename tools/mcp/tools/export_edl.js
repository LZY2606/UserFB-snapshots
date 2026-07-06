/**
 * @module tools/export_edl
 * MCP tool: export a VideoForge project as a CMX3600 EDL.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'export_edl',
  description:
    'Export a VideoForge project as a CMX3600 Edit Decision List (EDL). ' +
    'Returns an exportId usable with inspect_export and validate_export. ' +
    'Pass outputPath to also write the EDL to a file on disk.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId: {
        type: 'string',
        description: 'Project ID to export.',
      },
      outputPath: {
        type: 'string',
        description: 'Absolute path to write the .edl file to (e.g. "/tmp/sequence.edl"). Optional — omit to only cache the content in-memory.',
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
 * @returns {Promise<object>}
 */
export async function handler(args, { exportService }) {
  InputValidator.assert(args, {
    projectId: { type: 'string', required: true, minLength: 1 },
  });

  const result = await exportService.exportEdl(args.projectId, {
    title:        args.title,
    includeAudio: args.includeAudio,
    outputPath:   args.outputPath,
  });

  return {
    exportId:      result.exportId,
    format:        result.format,
    contentLength: result.content.length,
    outputPath:    result.outputPath,
    message: result.outputPath
      ? `EDL written to "${result.outputPath}" (${result.content.length} bytes). ` +
        `Use exportId "${result.exportId}" with inspect_export or validate_export.`
      : `EDL generated (${result.content.length} bytes). ` +
        `Use exportId "${result.exportId}" with inspect_export or validate_export.`,
  };
}
