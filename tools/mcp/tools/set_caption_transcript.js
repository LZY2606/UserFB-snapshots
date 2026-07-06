/**
 * @module tools/set_caption_transcript
 * MCP tool: set the transcript on a caption clip and auto-segment it.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'set_caption_transcript',
  description:
    'Set the raw transcript text on a caption clip. ' +
    'The text is automatically segmented into lines. ' +
    'Optionally supply per-word timings (array of {word, start, end}) and ' +
    'maxWordsPerSegment to control line length.',
  inputSchema: {
    type: 'object',
    properties: {
      projectId:          { type: 'string', description: 'Project ID.' },
      clipId:             { type: 'string', description: 'Caption clip ID.' },
      transcript:         { type: 'string', description: 'Raw transcript text.' },
      maxWordsPerSegment: {
        type: 'number',
        minimum: 1,
        description: 'Maximum words per caption line/segment. Default: 5.',
      },
      wordTimings: {
        type: 'array',
        description: 'Optional per-word timing data (e.g. from a transcription API).',
        items: {
          type: 'object',
          properties: {
            word:  { type: 'string' },
            start: { type: 'number', description: 'Start time in seconds.' },
            end:   { type: 'number', description: 'End time in seconds.' },
          },
          required: ['word', 'start', 'end'],
        },
      },
    },
    required: ['projectId', 'clipId', 'transcript'],
  },
};

export function handler(args, { captionService }) {
  InputValidator.assert(args, {
    projectId:  { type: 'string', required: true, minLength: 1 },
    clipId:     { type: 'string', required: true, minLength: 1 },
    transcript: { type: 'string', required: true },
  });

  const options = {};
  if (args.maxWordsPerSegment !== undefined) options.maxWordsPerSegment = args.maxWordsPerSegment;
  if (args.wordTimings        !== undefined) options.wordTimings        = args.wordTimings;

  return captionService.setTranscript(args.projectId, args.clipId, args.transcript, options);
}
