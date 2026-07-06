/**
 * @module tools/export_premiere
 * MCP tool: export a VideoForge project as Premiere Pro XML (XMEML v5).
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'export_premiere',
  description:
    'Export a VideoForge project as Premiere Pro XML (XMEML v5). ' +
    'Returns an exportId usable with inspect_export and validate_export.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId: {
        type: 'string',
        description: 'Project ID to export.',
      },
      sequenceName: {
        type: 'string',
        description: 'Optional override for the Premiere sequence name.',
      },
      includeVfMetadata: {
        type: 'boolean',
        description: 'Embed VideoForge namespace metadata in the output (default: true).',
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

  const result = exportService.exportPremiere(args.projectId, {
    sequenceName:     args.sequenceName,
    includeVfMetadata: args.includeVfMetadata,
  });

  return {
    exportId:      result.exportId,
    format:        result.format,
    contentLength: result.content.length,
    message:
      `Premiere XML generated (${result.content.length} bytes). ` +
      `Use exportId "${result.exportId}" with inspect_export or validate_export.`,
  };
}
