/**
 * @module tools/list_projects
 * MCP tool: list all projects currently registered in the session.
 */

export const definition = {
  name: 'list_projects',
  description:
    'List all VideoForge projects registered in this MCP session. ' +
    'Returns projectId, name, source (created | loaded), and storedAt timestamp.',
  inputSchema: {
    type: 'object',
    properties: {},
    required: [],
  },
};

/**
 * @param {object} _args
 * @param {{ projectService: import('../services/ProjectService.js').ProjectService }} services
 */
export function handler(_args, { projectService }) {
  const projects = projectService.listProjects();
  return {
    count: projects.length,
    projects,
    message: projects.length === 0
      ? 'No projects registered. Use create_project or load_project.'
      : `${projects.length} project(s) in session.`,
  };
}
