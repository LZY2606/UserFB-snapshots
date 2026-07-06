#!/usr/bin/env node
/**
 * VideoForge MCP Server — entry point.
 *
 * Wires together the service layer, all tool definitions, and MCP
 * resource handlers into a single server that speaks the Model Context
 * Protocol over stdio.
 *
 * Architecture
 * ─────────────
 * server.js
 *   └── services/   (business logic, VideoForge API usage)
 *         └── tools/    (thin MCP shims — input validation + service call)
 *         └── resources/ (read-only URI access to live state)
 *
 * The MCP SDK is only referenced in this file and resources/index.js.
 * Services and tools have no dependency on the MCP SDK, keeping them
 * portable and independently testable.
 *
 * Phase tracker
 * ─────────────
 * ✅ Phase 1 — Track & Clip Factory        (8 tools)
 * ✅ Phase 2 — Clip Editing Operations     (8 tools, incl. add_crop_effect, remove_clip_effect)
 * ✅ Phase 3 — Media Controls per Type     (5 tools)
 * ✅ Phase 4 — Caption Engine Core         (8 tools)
 * ✅ Phase 5 — Caption Engine Advanced     (11 tools)
 * ✅ Phase 6 — MP4 Export Pipeline         (3 tools)
 * ⬜ Phase 7 — Validation & Quality
 * ⬜ Phase 8 — Regression Extension
 * ⬜ Phase 9 — Discovery APIs
 */

import { Server }               from '@modelcontextprotocol/sdk/server/index.js';
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js';
import {
  ListToolsRequestSchema,
  CallToolRequestSchema,
} from '@modelcontextprotocol/sdk/types.js';

// ── Services ──────────────────────────────────────────────────────────────────

import { StorageService }      from './services/StorageService.js';
import { ProjectService }      from './services/ProjectService.js';
import { InspectionService }   from './services/InspectionService.js';
import { ExportService }       from './services/ExportService.js';
import { ValidationService }   from './services/ValidationService.js';
import { RegressionService }   from './services/RegressionService.js';
import { TrackService }        from './services/TrackService.js';        // Phase 1
import { ClipService }         from './services/ClipService.js';         // Phase 1
import { ClipEditingService }  from './services/ClipEditingService.js';  // Phase 2
import { ClipStyleService }    from './services/ClipStyleService.js';    // Phase 3
import { CaptionService }     from './services/CaptionService.js';      // Phase 4+5
import { Mp4ExportService }   from './services/Mp4ExportService.js';    // Phase 6

// ── Parsers ───────────────────────────────────────────────────────────────────

import { ExportParser } from './parsers/ExportParser.js';

// ── Tools — Phase 0 (original) ────────────────────────────────────────────────

import * as createProject  from './tools/create_project.js';
import * as loadProject    from './tools/load_project.js';
import * as inspectProject from './tools/inspect_project.js';
import * as exportPremiere from './tools/export_premiere.js';
import * as exportFcpxml   from './tools/export_fcpxml.js';
import * as exportEdl      from './tools/export_edl.js';
import * as inspectExport  from './tools/inspect_export.js';
import * as validateExport from './tools/validate_export.js';
import * as runRegression  from './tools/run_regression_test.js';

// ── Tools — Phase 1 (Track & Clip Factory) ────────────────────────────────────

import * as addTrack      from './tools/add_track.js';
import * as removeTrack   from './tools/remove_track.js';
import * as reorderTracks from './tools/reorder_tracks.js';
import * as inspectTrack  from './tools/inspect_track.js';
import * as addClip       from './tools/add_clip.js';
import * as removeClip    from './tools/remove_clip.js';
import * as inspectClip   from './tools/inspect_clip.js';
import * as listProjects  from './tools/list_projects.js';

// ── Tools — Phase 2 (Clip Editing Operations) ─────────────────────────────────

import * as trimClip      from './tools/trim_clip.js';
import * as moveClip      from './tools/move_clip.js';
import * as splitClip     from './tools/split_clip.js';
import * as copyClip      from './tools/copy_clip.js';
import * as addFade       from './tools/add_fade.js';
import * as addTransition    from './tools/add_transition.js';
import * as addCropEffect    from './tools/add_crop_effect.js';
import * as removeClipEffect from './tools/remove_clip_effect.js';

// ── Tools — Phase 3 (Media Controls per Type) ─────────────────────────────────

import * as setVideoOptions   from './tools/set_video_options.js';
import * as setAudioOptions   from './tools/set_audio_options.js';
import * as setImageTransform from './tools/set_image_transform.js';
import * as setTextStyle      from './tools/set_text_style.js';
import * as setShapeStyle     from './tools/set_shape_style.js';

// ── Tools — Phase 4 (Caption Engine Core) ────────────────────────────────────

import * as inspectCaption        from './tools/inspect_caption.js';
import * as setCaptionTranscript  from './tools/set_caption_transcript.js';
import * as applyCaptionPreset    from './tools/apply_caption_preset.js';
import * as setCaptionStyle       from './tools/set_caption_style.js';
import * as setCaptionLayout      from './tools/set_caption_layout.js';
import * as listCaptionPresets    from './tools/list_caption_presets.js';
import * as getActiveCaptionWords from './tools/get_active_caption_words.js';
import * as addCaptionAnimation   from './tools/add_caption_animation.js';

// ── Tools — Phase 5 (Caption Engine Advanced) ─────────────────────────────────

import * as addCaptionEffect          from './tools/add_caption_effect.js';
import * as removeCaptionAnimation    from './tools/remove_caption_animation.js';
import * as removeCaptionEffect       from './tools/remove_caption_effect.js';
import * as addCaptionKeyframe        from './tools/add_caption_keyframe.js';
import * as highlightCaptionWord      from './tools/highlight_caption_word.js';
import * as highlightCaptionKeywords  from './tools/highlight_caption_keywords.js';
import * as clearCaptionHighlights    from './tools/clear_caption_highlights.js';
import * as buildCaptionKaraoke       from './tools/build_caption_karaoke.js';
import * as animateCaptionWords       from './tools/animate_caption_words.js';
import * as animateCaptionCharacters  from './tools/animate_caption_characters.js';
import * as setCaptionSegmentStyle    from './tools/set_caption_segment_style.js';

// ── Tools — Phase 6 (MP4 Export Pipeline) ─────────────────────────────────────

import * as exportMp4         from './tools/export_mp4.js';
import * as getFfmpegCommand  from './tools/get_ffmpeg_command.js';
import * as inspectMp4Export  from './tools/inspect_mp4_export.js';

// ── Resources ─────────────────────────────────────────────────────────────────

import { registerResources } from './resources/index.js';

// ─────────────────────────────────────────────────────────────────────────────
// Service graph (pure dependency injection — no singletons leaked out)
// ─────────────────────────────────────────────────────────────────────────────

const storage            = new StorageService();
const projectService     = new ProjectService(storage);
const inspectionService  = new InspectionService(projectService);
const exportService      = new ExportService(projectService, storage);
const validationService  = new ValidationService(projectService, storage);
const regressionService  = new RegressionService(
  projectService, exportService, validationService, inspectionService,
);
const trackService       = new TrackService(projectService);         // Phase 1
const clipService        = new ClipService(projectService);          // Phase 1
const clipEditingService = new ClipEditingService(projectService);   // Phase 2
const clipStyleService   = new ClipStyleService(projectService);     // Phase 3
const captionService     = new CaptionService(projectService);       // Phase 4+5
const mp4ExportService   = new Mp4ExportService(projectService);     // Phase 6
const exportParser       = new ExportParser();

/** Services bag passed into every tool handler. */
const services = {
  storage,
  projectService,
  inspectionService,
  exportService,
  validationService,
  regressionService,
  trackService,
  clipService,
  clipEditingService,
  clipStyleService,
  captionService,
  mp4ExportService,
  exportParser,
};

// ─────────────────────────────────────────────────────────────────────────────
// Tool registry
// ─────────────────────────────────────────────────────────────────────────────

const ALL_TOOLS = [
  // Phase 0 — Foundation
  createProject,
  loadProject,
  inspectProject,
  exportPremiere,
  exportFcpxml,
  exportEdl,
  inspectExport,
  validateExport,
  runRegression,

  // Phase 1 — Track & Clip Factory
  addTrack,
  removeTrack,
  reorderTracks,
  inspectTrack,
  addClip,
  removeClip,
  inspectClip,
  listProjects,

  // Phase 2 — Clip Editing Operations
  trimClip,
  moveClip,
  splitClip,
  copyClip,
  addFade,
  addTransition,
  addCropEffect,
  removeClipEffect,

  // Phase 3 — Media Controls per Type
  setVideoOptions,
  setAudioOptions,
  setImageTransform,
  setTextStyle,
  setShapeStyle,

  // Phase 4 — Caption Engine Core
  inspectCaption,
  setCaptionTranscript,
  applyCaptionPreset,
  setCaptionStyle,
  setCaptionLayout,
  listCaptionPresets,
  getActiveCaptionWords,
  addCaptionAnimation,

  // Phase 5 — Caption Engine Advanced
  addCaptionEffect,
  removeCaptionAnimation,
  removeCaptionEffect,
  addCaptionKeyframe,
  highlightCaptionWord,
  highlightCaptionKeywords,
  clearCaptionHighlights,
  buildCaptionKaraoke,
  animateCaptionWords,
  animateCaptionCharacters,
  setCaptionSegmentStyle,

  // Phase 6 — MP4 Export Pipeline
  exportMp4,
  getFfmpegCommand,
  inspectMp4Export,
];

/** @type {Map<string, Function>} */
const toolHandlers = new Map(
  ALL_TOOLS.map((t) => [t.definition.name, t.handler]),
);

// ─────────────────────────────────────────────────────────────────────────────
// MCP Server
// ─────────────────────────────────────────────────────────────────────────────

const server = new Server(
  { name: 'videoforge-mcp', version: '1.10.0' },
  { capabilities: { tools: {}, resources: {} } },
);

server.setRequestHandler(ListToolsRequestSchema, async () => ({
  tools: ALL_TOOLS.map((t) => t.definition),
}));

server.setRequestHandler(CallToolRequestSchema, async (request) => {
  const { name, arguments: args } = request.params;

  const handler = toolHandlers.get(name);
  if (!handler) {
    return {
      content: [{ type: 'text', text: JSON.stringify({ error: `Unknown tool: "${name}"` }) }],
      isError: true,
    };
  }

  try {
    const result = await handler(args ?? {}, services);
    return {
      content: [{ type: 'text', text: JSON.stringify(result, null, 2) }],
    };
  } catch (err) {
    return {
      content: [{ type: 'text', text: JSON.stringify({ error: err.message }) }],
      isError: true,
    };
  }
});

registerResources(server, { inspectionService, exportService });

// ─────────────────────────────────────────────────────────────────────────────
// Start
// ─────────────────────────────────────────────────────────────────────────────

const transport = new StdioServerTransport();
await server.connect(transport);

console.error('[VideoForge MCP] Server running on stdio — 52 tools active');
