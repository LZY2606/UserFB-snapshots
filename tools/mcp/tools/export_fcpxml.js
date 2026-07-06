/**
 * @module tools/export_fcpxml
 * MCP tool: export a VideoForge project as FCPXML 1.10.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'export_fcpxml',
  description:
    'Export a VideoForge project as Final Cut Pro XML (FCPXML 1.10). ' +
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
        description: 'Absolute path to write the .fcpxml file to (e.g. "/tmp/sequence.fcpxml"). Optional — omit to only cache the content in-memory.',
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
 * @returns {Promise<object>}
 */
export async function handler(args, { exportService }) {
  InputValidator.assert(args, {
    projectId: { type: 'string', required: true, minLength: 1 },
  });

  const result = await exportService.exportFcpxml(args.projectId, {
    libraryName:       args.libraryName,
    eventName:         args.eventName,
    includeVfMetadata: args.includeVfMetadata,
    outputPath:        args.outputPath,
  });

  return {
    exportId:      result.exportId,
    format:        result.format,
    contentLength: result.content.length,
    outputPath:    result.outputPath,
    message: result.outputPath
      ? `FCPXML written to "${result.outputPath}" (${result.content.length} bytes). ` +
        `Use exportId "${result.exportId}" with inspect_export or validate_export.`
      : `FCPXML generated (${result.content.length} bytes). ` +
        `Use exportId "${result.exportId}" with inspect_export or validate_export.`,
  };
}
