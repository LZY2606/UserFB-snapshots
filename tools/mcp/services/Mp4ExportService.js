/**
 * @module Mp4ExportService
 * MP4 export pipeline: execute FFmpeg renders and retrieve FFmpeg commands.
 */

import { Mp4Exporter } from 'videoforge';

export class Mp4ExportService {
  /**
   * @param {import('./ProjectService.js').ProjectService} projectService
   */
  constructor(projectService) {
    this._projects = projectService;
    /** @type {Map<string, object>} */
    this._results = new Map();
  }

  // ── Internal ──────────────────────────────────────────────────────────────────

  _makeExportId(projectId) {
    return `mp4_${projectId.slice(-8)}_${Date.now()}`;
  }

  _buildExporterOptions(options = {}) {
    const opts = {};
    if (options.videoCodec   !== undefined) opts.videoCodec   = options.videoCodec;
    if (options.audioCodec   !== undefined) opts.audioCodec   = options.audioCodec;
    if (options.preset       !== undefined) opts.preset       = options.preset;
    if (options.crf          !== undefined) opts.crf          = options.crf;
    if (options.pixelFormat  !== undefined) opts.pixelFormat  = options.pixelFormat;
    if (options.videoBitrate !== undefined) opts.videoBitrate = options.videoBitrate;
    if (options.audioBitrate !== undefined) opts.audioBitrate = options.audioBitrate;
    return opts;
  }

  // ── Operations ───────────────────────────────────────────────────────────────

  /**
   * Export a project to MP4 by running FFmpeg.
   * Stores the result under an exportId for later retrieval.
   *
   * @param {string} projectId
   * @param {string} outputPath  Absolute path for the output .mp4 file.
   * @param {object} options     { videoCodec?, audioCodec?, preset?, crf?,
   *                               pixelFormat?, videoBitrate?, audioBitrate? }
   * @returns {Promise<object>}
   */
  async exportMp4(projectId, outputPath, options = {}) {
    const project      = this._projects.getProject(projectId);
    const exporterOpts = this._buildExporterOptions(options);
    const exporter     = new Mp4Exporter(project, exporterOpts);

    const result   = await exporter.export(outputPath);
    const exportId = this._makeExportId(projectId);

    const stored = {
      exportId,
      projectId,
      outputPath,
      success:   result.success,
      output:    result.output,
      duration:  result.duration,
      fileSize:  result.fileSize,
      options:   exporterOpts,
      createdAt: new Date().toISOString(),
    };
    this._results.set(exportId, stored);

    return stored;
  }

  /**
   * Build the FFmpeg command for a project without executing it.
   *
   * @param {string} projectId
   * @param {string} outputPath   Target path used in the command (default: /dev/null).
   * @param {object} options      Same codec/quality options as exportMp4.
   * @returns {object}  { command, args, commandLine }
   */
  getFfmpegCommand(projectId, outputPath = '/dev/null', options = {}) {
    const project      = this._projects.getProject(projectId);
    const exporterOpts = this._buildExporterOptions(options);
    const exporter     = new Mp4Exporter(project, exporterOpts);

    const args = exporter.buildCommand(outputPath);

    return {
      command:     'ffmpeg',
      args,
      commandLine: ['ffmpeg', ...args].join(' '),
      argCount:    args.length,
    };
  }

  /**
   * Retrieve a previously stored MP4 export result.
   *
   * @param {string} exportId
   * @returns {object}
   */
  getExportResult(exportId) {
    const result = this._results.get(exportId);
    if (!result) throw new Error(`MP4 export not found: "${exportId}"`);
    return result;
  }

  /**
   * List all stored MP4 export results (summary view).
   * @returns {object[]}
   */
  listExports() {
    return [...this._results.values()].map((r) => ({
      exportId:  r.exportId,
      projectId: r.projectId,
      outputPath: r.outputPath,
      success:   r.success,
      fileSize:  r.fileSize,
      createdAt: r.createdAt,
    }));
  }
}
