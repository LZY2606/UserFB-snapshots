/**
 * @module tools/validate_export
 * MCP tool: validate the structure of a generated export.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'validate_export',
  description:
    'Validate the structure of a generated export. ' +
    'Returns { valid, errors[], warnings[] }.',
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
 * @param {{ validationService: import('../services/ValidationService.js').ValidationService }} services
 * @returns {{ valid: boolean, errors: string[], warnings: string[] }}
 */
export function handler(args, { validationService }) {
  InputValidator.assert(args, {
    exportId: { type: 'string', required: true, minLength: 1 },
  });

  return validationService.validateExport(args.exportId);
}
