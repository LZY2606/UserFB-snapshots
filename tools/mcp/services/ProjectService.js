/**
 * @module ProjectService
 * Create, load, save, and retrieve VideoForge projects.
 *
 * All persistence is delegated to StorageService.  This service owns the
 * relationship between a projectId and a live Project instance.
 */

import path from 'node:path';
import { Project } from 'videoforge';

export class ProjectService {
  /**
   * @param {import('./StorageService.js').StorageService} storage
   */
  constructor(storage) {
    this._storage = storage;
  }

  // ── Factory ──────────────────────────────────────────────────────────────────

  /**
   * Create a new VideoForge project and register it in storage.
   *
   * @param {object} [options={}]
   * @param {string} [options.name='Untitled Project']
   * @param {number} [options.fps=30]
   * @param {number} [options.width=1920]
   * @param {number} [options.height=1080]
   * @param {number} [options.sampleRate=48000]
   * @param {number} [options.channels=2]
   * @returns {{ projectId: string, project: Project }}
   */
  createProject(options = {}) {
    const project = new Project({
      name: options.name ?? 'Untitled Project',
      fps: options.fps,
      width: options.width,
      height: options.height,
      sampleRate: options.sampleRate,
      channels: options.channels,
    });

    this._storage.storeProject(project.id, project, {
      name: project.name,
      source: 'created',
    });

    return { projectId: project.id, project };
  }

  // ── Persistence ──────────────────────────────────────────────────────────────

  /**
   * Load a VideoForge project from a .vfp JSON file and register it.
   *
   * @param {string} filePath  Absolute path to the .vfp file.
   * @returns {Promise<{ projectId: string, project: Project }>}
   */
  async loadProject(filePath) {
    const project = await Project.load(filePath);

    this._storage.storeProject(project.id, project, {
      name: project.name,
      source: 'loaded',
      filePath,
    });

    return { projectId: project.id, project };
  }

  /**
   * Persist a registered project to disk.
   *
   * @param {string} projectId
   * @param {string} [filePath]  Defaults to <baseDir>/<projectId>.vfp.
   * @returns {Promise<string>}  Resolved output path.
   */
  async saveProject(projectId, filePath) {
    const project = this._require(projectId);
    await this._storage.ensureBaseDir();

    const targetPath = filePath
      ?? path.join(this._storage.baseDir, `${projectId}.vfp`);

    return project.save(targetPath);
  }

  // ── Retrieval ────────────────────────────────────────────────────────────────

  /**
   * @param {string} projectId
   * @returns {Project}
   * @throws {Error} if the project is not registered
   */
  getProject(projectId) {
    return this._require(projectId);
  }

  /**
   * @returns {Array<{ id: string, meta: object }>}
   */
  listProjects() {
    return this._storage.listProjects();
  }

  // ── Internal ─────────────────────────────────────────────────────────────────

  /**
   * @param {string} projectId
   * @returns {Project}
   */
  _require(projectId) {
    const project = this._storage.getProject(projectId);
    if (!project) {
      throw new Error(
        `Project not found: "${projectId}". ` +
        'Register it first with create_project or load_project.',
      );
    }
    return project;
  }
}
