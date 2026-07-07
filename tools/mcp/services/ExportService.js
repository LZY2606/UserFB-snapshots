/**
 * @module ExportService
 * Run VideoForge exporters, cache the generated content, and optionally
 * write it to disk.
 *
 * Every successful export is stored in StorageService under a deterministic
 * exportId so downstream tools (inspect_export, validate_export) can
 * retrieve it by ID. When an outputPath is supplied, the exporter's own
 * export() method also writes the content to that file.
 */

import { PremiereXmlExporter, FcpxmlExporter, EdlExporter } from 'videoforge';

export class ExportService {
  /**
   * @param {import('./ProjectService.js').ProjectService} projectService
   * @param {import('./StorageService.js').StorageService} storage
   */
  constructor(projectService, storage) {
    this._projects = projectService;
    this._storage = storage;
  }

  // ── Export methods ────────────────────────────────────────────────────────────

  /**
   * Export a project as Premiere Pro XML (XMEML v5).
   *
   * @param {string} projectId
   * @param {object} [options={}]
   * @param {string}  [options.sequenceName]           Override the sequence name.
   * @param {boolean} [options.includeVfMetadata=true] Embed VideoForge namespace metadata.
   * @param {string}  [options.outputPath]             If given, also write the XML to this file.
   * @returns {Promise<{ exportId: string, content: string, format: 'premiere', outputPath: string|null }>}
   */
  async exportPremiere(projectId, options = {}) {
    const project = this._projects.getProject(projectId);

    const exporter = new PremiereXmlExporter(project, {
      pretty: true,
      validateInput: false,
      validateOutput: false,
      includeVfMetadata: options.includeVfMetadata ?? true,
      sequenceName: options.sequenceName,
    });

    const content = exporter.toString();
    const exportId = this._makeId(projectId, 'premiere');
    this._storage.storeExport(exportId, { content, format: 'premiere', projectId });

    const outputPath = options.outputPath ? await exporter.export(options.outputPath) : null;

    return { exportId, content, format: 'premiere', outputPath };
  }

  /**
   * Export a project as FCPXML 1.10.
   *
   * @param {string} projectId
   * @param {object} [options={}]
   * @param {string}  [options.libraryName]
   * @param {string}  [options.eventName]
   * @param {boolean} [options.includeVfMetadata=true]
   * @param {string}  [options.outputPath]  If given, also write the XML to this file.
   * @returns {Promise<{ exportId: string, content: string, format: 'fcpxml', outputPath: string|null }>}
   */
  async exportFcpxml(projectId, options = {}) {
    const project = this._projects.getProject(projectId);

    const exporter = new FcpxmlExporter(project, {
      pretty: true,
      validateInput: false,
      validateOutput: false,
      includeVfMetadata: options.includeVfMetadata ?? true,
      libraryName: options.libraryName,
      eventName: options.eventName,
    });

    const content = exporter.toString();
    const exportId = this._makeId(projectId, 'fcpxml');
    this._storage.storeExport(exportId, { content, format: 'fcpxml', projectId });

    const outputPath = options.outputPath ? await exporter.export(options.outputPath) : null;

    return { exportId, content, format: 'fcpxml', outputPath };
  }

  /**
   * Export a project as a CMX3600 EDL.
   *
   * @param {string} projectId
   * @param {object} [options={}]
   * @param {string}  [options.title]              Override the EDL title.
   * @param {boolean} [options.includeAudio=true]
   * @param {string}  [options.outputPath]  If given, also write the EDL to this file.
   * @returns {Promise<{ exportId: string, content: string, format: 'edl', outputPath: string|null }>}
   */
  async exportEdl(projectId, options = {}) {
    const project = this._projects.getProject(projectId);

    const exporter = new EdlExporter(project, {
      validateInput: false,
      includeAudio: options.includeAudio ?? true,
      includeComments: true,
      title: options.title ?? project.name ?? 'VideoForge Export',
    });

    const content = exporter.toString();
    const exportId = this._makeId(projectId, 'edl');
    this._storage.storeExport(exportId, { content, format: 'edl', projectId });

    const outputPath = options.outputPath ? await exporter.export(options.outputPath) : null;

    return { exportId, content, format: 'edl', outputPath };
  }

  // ── Retrieval ─────────────────────────────────────────────────────────────────

  /**
   * @param {string} exportId
   * @returns {{ content: string, format: string, projectId: string, exportedAt: number }}
   * @throws {Error} if not found
   */
  getExport(exportId) {
    const data = this._storage.getExport(exportId);
    if (!data) {
      throw new Error(
        `Export not found: "${exportId}". ` +
        'Generate one first with export_premiere, export_fcpxml, or export_edl.',
      );
    }
    return data;
  }

  /** @returns {Array} */
  listExports() {
    return this._storage.listExports();
  }

  // ── Internal ──────────────────────────────────────────────────────────────────

  /**
   * Build a deterministic, readable export ID.
   * @param {string} projectId
   * @param {string} format
   * @returns {string}
   */
  _makeId(projectId, format) {
    const suffix = projectId.slice(-8);
    return `export_${format}_${suffix}_${Date.now()}`;
  }
}
