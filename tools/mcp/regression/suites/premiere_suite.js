/**
 * @module regression/suites/premiere_suite
 * Regression suite for the Premiere XML (XMEML v5) exporter.
 *
 * Test plan:
 *  1. Build a fixture project with video + audio tracks
 *  2. Verify project inspection before export (fps, tracks, clips)
 *  3. Export as Premiere XML
 *  4. Run structural assertions on the raw content
 *  5. Verify the export passes validate_export checks
 *  6. Verify parsed statistics match expected values
 */

import { ExportParser } from '../../parsers/ExportParser.js';

const parser = new ExportParser();

/**
 * @param {import('../AssertionEngine.js').AssertionEngine} engine
 * @param {{
 *   projectService:    import('../../services/ProjectService.js').ProjectService,
 *   exportService:     import('../../services/ExportService.js').ExportService,
 *   validationService: import('../../services/ValidationService.js').ValidationService,
 *   inspectionService: import('../../services/InspectionService.js').InspectionService,
 * }} services
 */
export async function premiereSuite(engine, {
  projectService,
  exportService,
  validationService,
  inspectionService,
}) {
  // ── 1. Build fixture project ──────────────────────────────────────────────────

  const { projectId, project } = projectService.createProject({
    name: 'Regression — Premiere Suite',
    fps:    24,
    width:  1920,
    height: 1080,
  });

  const vTrack = project.addTrack('video', { name: 'Video 1' });
  const aTrack = project.addTrack('audio', { name: 'Audio 1' });

  vTrack.addVideo('/regression/clip-a.mp4', { inPoint: 0, outPoint: 5 });
  vTrack.addVideo('/regression/clip-b.mp4', { inPoint: 0, outPoint: 3 });
  aTrack.addAudio('/regression/music.mp3',  { inPoint: 0, outPoint: 8 });

  // ── 2. Pre-export inspection ──────────────────────────────────────────────────

  const summary = inspectionService.inspectProject(projectId);

  engine
    .assertEquals('fps is 24',                  summary.fps,        24)
    .assertEquals('width is 1920',              summary.width,      1920)
    .assertEquals('height is 1080',             summary.height,     1080)
    .assertEquals('2 tracks',                   summary.trackCount, 2)
    .assertEquals('3 clips',                    summary.clipCount,  3)
    .assertGreaterThan('duration > 0',          summary.duration,   0)
    .assertEquals('1 video track',              summary.videoTrackCount, 1)
    .assertEquals('1 audio track',              summary.audioTrackCount, 1);

  // ── 3. Export ─────────────────────────────────────────────────────────────────

  const { exportId, content } = exportService.exportPremiere(projectId, {
    sequenceName: 'Regression Sequence',
  });

  engine
    .assertNotEmpty('exportId is not empty', exportId)
    .assertGreaterThan('content length > 0', content.length, 0);

  // ── 4. Structural assertions ──────────────────────────────────────────────────

  engine
    .assertContains('has <xmeml> root',            content, '<xmeml')
    .assertContains('XMEML version is 5',          content, 'version="5"')
    .assertContains('has <sequence>',              content, '<sequence')
    .assertContains('has <video> section',         content, '<video>')
    .assertContains('has <audio> section',         content, '<audio>')
    .assertContains('has <clipitem>',              content, '<clipitem')
    .assertContains('fps preserved as timebase',   content, '<timebase>24</timebase>');

  // ── 5. Validation ─────────────────────────────────────────────────────────────

  const validation = validationService.validateExport(exportId);

  engine
    .assertTrue('export passes structural validation', validation.valid)
    .assertArrayLength('no validation errors',         validation.errors, 0);

  // ── 6. Parsed statistics ──────────────────────────────────────────────────────

  const stats = parser.parsePremiere(content);

  engine
    .assertEquals('parsed format is premiere',     stats.format,         'premiere')
    .assertEquals('parsed XMEML version is 5',     stats.xmemlVersion,   '5')
    .assertGreaterThan('at least 1 sequence',       stats.sequenceCount,  0)
    .assertGreaterThanOrEqual('2+ clip items',      stats.clipItemCount,  2);
}
