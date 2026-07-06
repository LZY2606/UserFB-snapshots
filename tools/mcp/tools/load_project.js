/**
 * @module tools/load_project
 * MCP tool: load an existing VideoForge project from a .vfp file.
 */

import { InputValidator } from '../validators/InputValidator.js';

export const definition = {
  name: 'load_project',
  description:
    'Load an existing VideoForge project from a .vfp JSON file on disk. ' +
    'Returns a projectId for use in subsequent tool calls.',
  inputSchema: {
    type: 'object',
    properties: {
      filePath: {
        type: 'string',
        description: 'Absolute path to the .vfp project file.',
      },
    },
    required: ['filePath'],
  },
};

/**
 * @param {object} args
 * @param {{ projectService: import('../services/ProjectService.js').ProjectService }} services
 * @returns {Promise<object>}
 */
export async function handler(args, { projectService }) {
  InputValidator.assert(args, {
    filePath: { type: 'string', required: true, minLength: 1 },
  });

  const { projectId, project } = await projectService.loadProject(args.filePath);

  return {
    projectId,
    name: project.name,
    filePath: args.filePath,
    message: `Project "${project.name}" loaded from "${args.filePath}".`,
  };
}
