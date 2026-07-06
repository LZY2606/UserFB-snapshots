/**
 * @module tools/create_project
 * MCP tool: create a new VideoForge project.
 */

import { InputValidator } from '../validators/InputValidator.js';

/** @type {import('@modelcontextprotocol/sdk/types.js').Tool} */
export const definition = {
  name: 'create_project',
  description:
    'Create a new VideoForge project with the specified settings. ' +
    'Returns a projectId used by all subsequent tools.',
  inputSchema: {
    type: 'object',
    properties: {
      name:       { type: 'string',  description: 'Project name (default: "Untitled Project").' },
      fps:        { type: 'number',  description: 'Frame rate in fps (default: 30).' },
      width:      { type: 'number',  description: 'Frame width in pixels (default: 1920).' },
      height:     { type: 'number',  description: 'Frame height in pixels (default: 1080).' },
      sampleRate: { type: 'number',  description: 'Audio sample rate in Hz (default: 48000).' },
      channels:   { type: 'number',  description: 'Audio channel count (default: 2).' },
    },
    required: [],
  },
};

const SCHEMA = {
  fps:        { type: 'number',  min: 1,    max: 240    },
  width:      { type: 'number',  min: 1,    max: 7680   },
  height:     { type: 'number',  min: 1,    max: 4320   },
  sampleRate: { type: 'number',  min: 8000, max: 192000 },
  channels:   { type: 'number',  min: 1,    max: 8      },
};

/**
 * @param {object} args
 * @param {{ projectService: import('../services/ProjectService.js').ProjectService }} services
 * @returns {object}
 */
export function handler(args, { projectService }) {
  InputValidator.assert(args, SCHEMA);

  const { projectId, project } = projectService.createProject(args);

  return {
    projectId,
    name: project.name,
    message: `Project "${project.name}" created. Use projectId "${projectId}" in subsequent calls.`,
  };
}
