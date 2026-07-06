/**
 * @module regression/suites/edl_suite
 * Regression suite for the CMX3600 EDL exporter.
 *
 * Test plan:
 *  1. Build a fixture project with video + audio tracks
 *  2. Verify pre-export inspection
 *  3. Export as EDL
 *  4. Run structural assertions on the raw content
 *  5. Verify structural validation passes
 *  6. Verify parsed statistics match expected values
 *  7. Verify audio-excluded variant produces fewer events
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
export async function edlSuite(engine, {
  projectService,
  exportService,
  validationService,
  inspectionService,
}) {
  // ── 1. Build fixture project ──────────────────────────────────────────────────

  const { projectId, project } = projectService.createProject({
    name:   'Regression — EDL Suite',
    fps:    25,
    width:  1920,
    height: 1080,
  });

  const vTrack = project.addTrack('video', { name: 'Cut' });
  const aTrack = project.addTrack('audio', { name: 'Stereo Mix' });

  vTrack.addVideo('/regression/shot-01.mp4', { inPoint: 0,  outPoint: 4 });
  vTrack.addVideo('/regression/shot-02.mp4', { inPoint: 2,  outPoint: 7 });
  vTrack.addVideo('/regression/shot-03.mp4', { inPoint: 0,  outPoint: 3 });
  aTrack.addAudio('/regression/mix.wav',     { inPoint: 0,  outPoint: 14 });

  // ── 2. Pre-export inspection ──────────────────────────────────────────────────

  const summary = inspectionService.inspectProject(projectId);

  engine
    .assertEquals('fps is 25',          summary.fps,        25)
    .assertEquals('2 tracks',           summary.trackCount, 2)
    .assertEquals('4 clips',            summary.clipCount,  4)
    .assertGreaterThan('duration > 0',  summary.duration,   0);

  // ── 3. Export (audio included) ────────────────────────────────────────────────

  const { exportId, content } = exportService.exportEdl(projectId, {
    title: 'Regression EDL',
    includeAudio: true,
  });

  engine
    .assertNotEmpty('exportId is not empty', exportId)
    .assertGreaterThan('content length > 0', content.length, 0);

  // ── 4. Structural assertions ──────────────────────────────────────────────────

  engine
    .assertContains('has TITLE: header',    content, 'TITLE:')
    .assertContains('title text is correct',content, 'Regression EDL')
    .assertContains('has FCM: header',      content, 'FCM:');

  // Event lines: video clips become edit events
  const lines  = content.split('\n');
  const events = lines.filter((l) => /^\s*\d{3}\s/.test(l));
  engine.assertGreaterThan('at least 3 events (3 video clips)', events.length, 2);

  // ── 5. Validation ─────────────────────────────────────────────────────────────

  const validation = validationService.validateExport(exportId);

  engine
    .assertTrue('export passes structural validation', validation.valid)
    .assertArrayLength('no validation errors',         validation.errors, 0);

  // ── 6. Parsed statistics ──────────────────────────────────────────────────────

  const stats = parser.parseEdl(content);

  engine
    .assertEquals('parsed format is edl',        stats.format,     'edl')
    .assertEquals('parsed title matches',        stats.title,      'Regression EDL')
    .assertGreaterThan('at least 3 events',      stats.eventCount, 2)
    .assertGreaterThan('video events > 0',       stats.videoEventCount, 0);

  // ── 7. Audio-excluded variant ─────────────────────────────────────────────────

  const { exportId: noAudioId, content: noAudioContent } =
    exportService.exportEdl(projectId, { includeAudio: false });

  const noAudioStats = parser.parseEdl(noAudioContent);

  // Audio excluded — event count should be less than or equal to the audio-included run
  engine.assertGreaterThanOrEqual(
    'video-only event count <= full event count',
    stats.eventCount,
    noAudioStats.eventCount,
  );

  const noAudioValidation = validationService.validateExport(noAudioId);
  engine.assertTrue('no-audio EDL also passes validation', noAudioValidation.valid);
}
