/**
 * @module resources/index
 * Register MCP resource handlers for videoforge:// URIs.
 *
 * Resources provide read-only access to live project and export state.
 *
 * videoforge://project/{id}   → project inspection summary (JSON)
 * videoforge://timeline/{id}  → full ITR timeline structure (JSON)
 * videoforge://export/{id}    → raw generated export content (XML or EDL)
 */

import {
  ListResourcesRequestSchema,
  ReadResourceRequestSchema,
} from '@modelcontextprotocol/sdk/types.js';

/** @type {Array<import('@modelcontextprotocol/sdk/types.js').Resource>} */
const RESOURCE_TEMPLATES = [
  {
    uriTemplate: 'videoforge://project/{id}',
    name:        'Project Metadata',
    description: 'Inspection summary for a registered VideoForge project.',
    mimeType:    'application/json',
  },
  {
    uriTemplate: 'videoforge://timeline/{id}',
    name:        'Timeline Structure',
    description: 'Full ITR timeline including all tracks, clips, and assets.',
    mimeType:    'application/json',
  },
  {
    uriTemplate: 'videoforge://export/{id}',
    name:        'Export Content',
    description: 'Raw content of a generated export (Premiere XML, FCPXML, or EDL).',
    mimeType:    'text/plain',
  },
];

/**
 * Register all resource handlers on the MCP server.
 *
 * @param {import('@modelcontextprotocol/sdk/server/index.js').Server} server
 * @param {{
 *   inspectionService: import('../services/InspectionService.js').InspectionService,
 *   exportService:     import('../services/ExportService.js').ExportService,
 * }} services
 */
export function registerResources(server, { inspectionService, exportService }) {
  server.setRequestHandler(ListResourcesRequestSchema, async () => ({
    resources: RESOURCE_TEMPLATES,
  }));

  server.setRequestHandler(ReadResourceRequestSchema, async (request) => {
    const { uri } = request.params;

    // videoforge://project/{id}
    const projectMatch = uri.match(/^videoforge:\/\/project\/(.+)$/);
    if (projectMatch) {
      const summary = inspectionService.inspectProject(projectMatch[1]);
      return {
        contents: [{
          uri,
          mimeType: 'application/json',
          text: JSON.stringify(summary, null, 2),
        }],
      };
    }

    // videoforge://timeline/{id}
    const timelineMatch = uri.match(/^videoforge:\/\/timeline\/(.+)$/);
    if (timelineMatch) {
      const timeline = inspectionService.inspectTimeline(timelineMatch[1]);
      return {
        contents: [{
          uri,
          mimeType: 'application/json',
          text: JSON.stringify(timeline, null, 2),
        }],
      };
    }

    // videoforge://export/{id}
    const exportMatch = uri.match(/^videoforge:\/\/export\/(.+)$/);
    if (exportMatch) {
      const data = exportService.getExport(exportMatch[1]);
      const mimeType = data.format === 'edl' ? 'text/plain' : 'application/xml';
      return {
        contents: [{ uri, mimeType, text: data.content }],
      };
    }

    throw new Error(`Unrecognised resource URI: "${uri}"`);
  });
}
