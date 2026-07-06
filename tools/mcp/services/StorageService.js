/**
 * @module StorageService
 * In-memory project and export registry with optional disk persistence.
 *
 * Acts as the single source of truth for all live objects.
 * StorageService is dependency-free by design so it can be swapped for a
 * persistent backend (Redis, SQLite, etc.) without touching any other service.
 */

import { promises as fs } from 'node:fs';
import path from 'node:path';
import os from 'node:os';

export class StorageService {
  constructor() {
    /**
     * Live Project instances keyed by projectId.
     * @type {Map<string, { project: object, meta: ProjectMeta }>}
     */
    this._projects = new Map();

    /**
     * Generated export payloads keyed by exportId.
     * @type {Map<string, ExportEntry>}
     */
    this._exports = new Map();

    /** @type {string} */
    this._baseDir = path.join(os.tmpdir(), 'videoforge-mcp');
  }

  // ── Lifecycle ────────────────────────────────────────────────────────────────

  /** @returns {string} Base directory used for file persistence. */
  get baseDir() {
    return this._baseDir;
  }

  /** Ensure the base directory exists (idempotent). */
  async ensureBaseDir() {
    await fs.mkdir(this._baseDir, { recursive: true });
  }

  // ── Projects ─────────────────────────────────────────────────────────────────

  /**
   * Register a project instance in memory.
   *
   * @param {string} projectId
   * @param {object} project  VideoForge Project instance.
   * @param {object} [meta]   Arbitrary metadata stored alongside the project.
   */
  storeProject(projectId, project, meta = {}) {
    this._projects.set(projectId, {
      project,
      meta: { ...meta, storedAt: Date.now() },
    });
  }

  /**
   * @param {string} projectId
   * @returns {object | undefined}
   */
  getProject(projectId) {
    return this._projects.get(projectId)?.project;
  }

  /**
   * @param {string} projectId
   * @returns {{ project: object, meta: object } | undefined}
   */
  getProjectEntry(projectId) {
    return this._projects.get(projectId);
  }

  /**
   * @param {string} projectId
   * @returns {boolean}
   */
  hasProject(projectId) {
    return this._projects.has(projectId);
  }

  /**
   * List all stored projects with lightweight metadata (no project instance).
   * @returns {Array<{ id: string, meta: object }>}
   */
  listProjects() {
    return [...this._projects.entries()].map(([id, { meta }]) => ({ id, meta }));
  }

  // ── Exports ──────────────────────────────────────────────────────────────────

  /**
   * @param {string} exportId
   * @param {{ content: string, format: string, projectId: string }} data
   */
  storeExport(exportId, data) {
    this._exports.set(exportId, { ...data, exportedAt: Date.now() });
  }

  /**
   * @param {string} exportId
   * @returns {{ content: string, format: string, projectId: string, exportedAt: number } | undefined}
   */
  getExport(exportId) {
    return this._exports.get(exportId);
  }

  /**
   * @param {string} exportId
   * @returns {boolean}
   */
  hasExport(exportId) {
    return this._exports.has(exportId);
  }

  /**
   * List all stored exports with summary metadata.
   * @returns {Array<{ id: string, format: string, projectId: string, exportedAt: number, size: number }>}
   */
  listExports() {
    return [...this._exports.entries()].map(([id, data]) => ({
      id,
      format: data.format,
      projectId: data.projectId,
      exportedAt: data.exportedAt,
      size: data.content.length,
    }));
  }
}
