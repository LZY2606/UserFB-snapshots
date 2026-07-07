/**
 * @module tools/export_premiere
 * MCP tool: export a VideoForge project as Premiere Pro XML (XMEML v5).
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'export_premiere',
  description:
    'Export a VideoForge project as Premiere Pro XML (XMEML v5). ' +
    'Returns an exportId usable with inspect_export and validate_export. ' +
    'Pass outputPath to also write the XML to a file on disk.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId: {
        type: 'string',
        description: 'Project ID to export.',
      },
      outputPath: {
        type: 'string',
        description: 'Absolute path to write the .xml file to (e.g. "/tmp/sequence.xml"). Optional — omit to only cache the content in-memory.',
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
 * @returns {Promise<object>}
 */
export async function handler(args, { exportService }) {
  InputValidator.assert(args, {
    projectId: { type: 'string', required: true, minLength: 1 },
  });

  const result = await exportService.exportPremiere(args.projectId, {
    sequenceName:      args.sequenceName,
    includeVfMetadata: args.includeVfMetadata,
    outputPath:        args.outputPath,
  });

  return {
    exportId:      result.exportId,
    format:        result.format,
    contentLength: result.content.length,
    outputPath:    result.outputPath,
    message: result.outputPath
      ? `Premiere XML written to "${result.outputPath}" (${result.content.length} bytes). ` +
        `Use exportId "${result.exportId}" with inspect_export or validate_export.`
      : `Premiere XML generated (${result.content.length} bytes). ` +
        `Use exportId "${result.exportId}" with inspect_export or validate_export.`,
  };
}
