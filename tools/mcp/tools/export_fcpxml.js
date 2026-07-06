/**
 * @module tools/export_fcpxml
 * MCP tool: export a VideoForge project as FCPXML 1.10.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'export_fcpxml',
  description:
    'Export a VideoForge project as Final Cut Pro XML (FCPXML 1.10). ' +
    'Returns an exportId usable with inspect_export and validate_export.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId: {
        type: 'string',
        description: 'Project ID to export.',
      },
      libraryName: {
        type: 'string',
        description: 'Optional FCP library name override.',
      },
      eventName: {
        type: 'string',
        description: 'Optional FCP event name override.',
      },
      includeVfMetadata: {
        type: 'boolean',
        description: 'Embed VideoForge namespace metadata (default: true).',
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

  const result = exportService.exportFcpxml(args.projectId, {
    libraryName:       args.libraryName,
    eventName:         args.eventName,
    includeVfMetadata: args.includeVfMetadata,
  });

  return {
    exportId:      result.exportId,
    format:        result.format,
    contentLength: result.content.length,
    message:
      `FCPXML generated (${result.content.length} bytes). ` +
      `Use exportId "${result.exportId}" with inspect_export or validate_export.`,
  };
}
