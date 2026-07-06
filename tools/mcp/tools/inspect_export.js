/**
 * @module tools/inspect_export
 * MCP tool: inspect a generated export for structural statistics.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'inspect_export',
  description:
    'Inspect a generated export. Returns track counts, clip counts, keyframe counts, ' +
    'caption counts, and format-specific structure statistics.',
  inputSchema: {
    type: 'object',
    properties: {
      exportId: {
        type: 'string',
        description: 'Export ID returned by export_premiere, export_fcpxml, or export_edl.',
      },
    },
    required: ['exportId'],
  },
};

/**
 * @param {object} args
 * @param {{
 *   exportService: import('../services/ExportService.js').ExportService,
 *   exportParser:  import('../parsers/ExportParser.js').ExportParser
 * }} services
 * @returns {object}
 */
export function handler(args, { exportService, exportParser }) {
  InputValidator.assert(args, {
    exportId: { type: 'string', required: true, minLength: 1 },
  });

  const data = exportService.getExport(args.exportId);

  let stats;
  switch (data.format) {
    case 'premiere': stats = exportParser.parsePremiere(data.content); break;
    case 'fcpxml':   stats = exportParser.parseFcpxml(data.content);   break;
    case 'edl':      stats = exportParser.parseEdl(data.content);      break;
    default:
      throw new Error(`Unknown export format: "${data.format}"`);
  }

  return {
    exportId:    args.exportId,
    projectId:   data.projectId,
    exportedAt:  data.exportedAt,
    ...stats,
  };
}
