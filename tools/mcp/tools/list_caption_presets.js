/**
 * @module tools/list_caption_presets
 * MCP tool: list all available built-in caption presets.
 */

export const definition = {
  name: 'list_caption_presets',
  description:
    'List all 10 built-in caption presets with names and descriptions. ' +
    'Use the name with apply_caption_preset to apply a preset to a clip.',
  inputSchema: {
    type: 'object',
    properties: {},
    required: [],
  },
};

export function handler(_args, { captionService }) {
  return captionService.listPresets();
}
