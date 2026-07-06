/**
 * @module tools/set_caption_segment_style
 * MCP tool: override the style on a specific caption segment.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'set_caption_segment_style',
  description:
    'Override style properties on a specific caption segment (line) by its 0-based index. ' +
    'Segment style overrides the clip-level style for that line only. ' +
    'Use inspect_caption to see segment indices and text.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId:    { type: 'string', description: 'Project ID.' },
      clipId:       { type: 'string', description: 'Caption clip ID.' },
      segmentIndex: { type: 'number', minimum: 0, description: 'Segment index (0-based).' },

      // Style overrides
      fontFamily:    { type: 'string',  description: 'Font family.' },
      fontSize:      { type: 'number',  minimum: 1, description: 'Font size in pixels.' },
      fontWeight:    { description: 'Font weight (100–900 or "bold"/"normal").' },
      fill:          { type: 'string',  description: 'Text fill color.' },
      textAlign:     { type: 'string',  enum: ['left', 'center', 'right', 'justify'] },
      textTransform: { type: 'string',  enum: ['none', 'uppercase', 'lowercase', 'capitalize'] },
      letterSpacing: { type: 'number',  description: 'Letter spacing in pixels.' },
    },
    required: ['projectId', 'clipId', 'segmentIndex'],
  },
};

export function handler(args, { captionService }) {
  InputValidator.assert(args, {
    projectId:    { type: 'string', required: true, minLength: 1 },
    clipId:       { type: 'string', required: true, minLength: 1 },
    segmentIndex: { type: 'number', required: true },
  });

  const styleProps = {};
  const fields = [
    'fontFamily', 'fontSize', 'fontWeight', 'fill',
    'textAlign', 'textTransform', 'letterSpacing',
  ];
  for (const f of fields) {
    if (args[f] !== undefined) styleProps[f] = args[f];
  }

  return captionService.setSegmentStyle(
    args.projectId, args.clipId, args.segmentIndex, styleProps
  );
}
