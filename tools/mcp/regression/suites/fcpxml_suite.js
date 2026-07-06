/**
 * @module regression/suites/fcpxml_suite
 * Regression suite for the FCPXML 1.10 exporter.
 *
 * Test plan:
 *  1. Build a fixture project with video + text + caption tracks
 *  2. Verify pre-export inspection
 *  3. Export as FCPXML
 *  4. Run structural assertions on the raw content
 *  5. Verify structural validation passes
 *  6. Verify parsed statistics
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
export async function fcpxmlSuite(engine, {
  projectService,
  exportService,
  validationService,
  inspectionService,
}) {
  // ── 1. Build fixture project ──────────────────────────────────────────────────

  const { projectId, project } = projectService.createProject({
    name:   'Regression — FCPXML Suite',
    fps:    30,
    width:  3840,
    height: 2160,
  });

  const vTrack = project.addTrack('video', { name: 'Main Video' });
  const tTrack = project.addTrack('text',  { name: 'Lower Thirds' });
  const aTrack = project.addTrack('audio', { name: 'Dialogue' });

  vTrack.addVideo('/regression/hero.mp4',      { inPoint: 0, outPoint: 10 });
  vTrack.addImage('/regression/logo.png',      { outPoint: 5 });
  tTrack.addText('Opening Title', { outPoint: 3, fontFamily: 'Arial', fontSizeValue: 72 });
  aTrack.addAudio('/regression/dialogue.wav',  { inPoint: 0, outPoint: 10 });

  // ── 2. Pre-export inspection ──────────────────────────────────────────────────

  const summary = inspectionService.inspectProject(projectId);

  engine
    .assertEquals('fps is 30',         summary.fps,        30)
    .assertEquals('width is 3840',     summary.width,      3840)
    .assertEquals('height is 2160',    summary.height,     2160)
    .assertEquals('3 tracks',          summary.trackCount, 3)
    .assertEquals('4 clips',           summary.clipCount,  4)
    .assertGreaterThan('duration > 0', summary.duration,   0);

  // ── 3. Export ─────────────────────────────────────────────────────────────────

  const { exportId, content } = exportService.exportFcpxml(projectId, {
    libraryName: 'Regression Library',
    eventName:   'Regression Event',
  });

  engine
    .assertNotEmpty('exportId is not empty', exportId)
    .assertGreaterThan('content length > 0', content.length, 0);

  // ── 4. Structural assertions ──────────────────────────────────────────────────

  engine
    .assertContains('has <fcpxml> root',           content, '<fcpxml')
    .assertContains('FCPXML version is 1.10',      content, 'version="1.10"')
    .assertContains('has <resources>',             content, '<resources')
    .assertContains('has <library>',               content, '<library')
    .assertContains('has <sequence>',              content, '<sequence')
    .assertContains('fps preserved in format',     content, '30');

  // ── 5. Validation ─────────────────────────────────────────────────────────────

  const validation = validationService.validateExport(exportId);

  engine
    .assertTrue('export passes structural validation', validation.valid)
    .assertArrayLength('no validation errors',         validation.errors, 0);

  // ── 6. Parsed statistics ──────────────────────────────────────────────────────

  const stats = parser.parseFcpxml(content);

  engine
    .assertEquals('parsed format is fcpxml',      stats.format,          'fcpxml')
    .assertEquals('parsed version is 1.10',       stats.fcpxmlVersion,   '1.10')
    .assertGreaterThan('at least 1 sequence',             stats.sequenceCount,               0)
    .assertGreaterThan('assets referenced',               stats.assetCount,                  0)
    // VideoForge FCPXML exporter emits <clip> inside the spine
    .assertGreaterThan('at least 1 clip in spine',        stats.clipCount + stats.assetClipCount, 0);
}
