/**
 * @module TimelineConverter
 * Converts a VideoForge Project into an IntermediateTimeline (ITR).
 *
 * Feature compatibility layer:
 *   - Properties that have direct interchange equivalents are mapped.
 *   - Features with no equivalent (complex caption animations, custom effects,
 *     stagger systems, keyframe sets) are preserved in videoForgeMetadata on
 *     the affected ClipRepresentation for round-trip fidelity.
 */

import IntermediateTimeline from './IntermediateTimeline.js';
import AssetReference       from './AssetReference.js';
import TrackRepresentation  from './TrackRepresentation.js';
import ClipRepresentation   from './ClipRepresentation.js';
import EffectRepresentation from './EffectRepresentation.js';
import TransitionRepresentation from './TransitionRepresentation.js';
import CaptionRepresentation from './CaptionRepresentation.js';
import { ASSET_TYPES, CLIP_TYPES, TRACK_TYPES, EFFECT_TYPES } from '../utils/Constants.js';
import { resolveSequenceParams } from '../utils/FpsResolver.js';

class TimelineConverter {
  /**
   * Convert a VideoForge Project to an IntermediateTimeline.
   * @param {import('../core/Project.js').default} project
   * @returns {IntermediateTimeline}
   */
  convert(project) {
    const seq = resolveSequenceParams(project);
    const itr = new IntermediateTimeline({
      projectId:  project.id,
      name:       project.name ?? 'Untitled',
      fps:        seq.fps,
      width:      seq.width,
      height:     seq.height,
      sampleRate: seq.sampleRate,
      channels:   seq.channels,
      metadata: {
        videoForge: {
          version:     project.version    ?? '1.0',
          description: project.description ?? '',
          createdAt:   project.createdAt?.toISOString() ?? new Date().toISOString(),
        },
      },
    });

    // 1. Collect and deduplicate assets.
    this._convertAssets(project, itr);

    // 2. Convert tracks and their clips.
    const tracks = typeof project.getTracks === 'function'
      ? project.getTracks()
      : (project.timeline?.getTracks?.() ?? project._tracks ?? []);

    tracks.forEach((track, idx) => {
      const trackRep = this._convertTrack(track, idx, itr);
      itr.tracks.push(trackRep);
    });

    // 3. Convert markers.
    if (Array.isArray(project.markers)) {
      itr.markers = project.markers.map((m) => ({
        id:    m.id ?? String(Math.random()),
        time:  m.time   ?? 0,
        name:  m.name   ?? '',
        color: m.color  ?? '#FF0000',
        type:  m.type   ?? 'comment',
      }));
    }

    itr.computeDuration();
    return itr;
  }

  // ─── Asset conversion ─────────────────────────────────────────────────────────

  _convertAssets(project, itr) {
    const seenUids = new Set();

    // Walk all clips and extract their assets.
    const tracks = typeof project.getTracks === 'function'
      ? project.getTracks()
      : (project.timeline?.getTracks?.() ?? project._tracks ?? []);

    for (const track of tracks) {
      const clips = typeof track.getClips === 'function' ? track.getClips() : track._clips ?? [];
      for (const clip of clips) {
        if (!clip.asset) continue;
        const asset = clip.asset;
        const uid = asset.uid ?? asset.id;
        if (seenUids.has(uid)) continue;
        seenUids.add(uid);
        itr.addAsset(AssetReference.fromAsset(asset, itr.fps));
      }
    }
  }

  // ─── Track conversion ─────────────────────────────────────────────────────────

  _convertTrack(track, index, itr) {
    const clips = typeof track.getClips === 'function' ? track.getClips() : track._clips ?? [];
    const clipReps = clips.map((clip) => this._convertClip(clip, itr));

    return TrackRepresentation.fromTrack(track, index, clipReps);
  }

  // ─── Clip conversion ──────────────────────────────────────────────────────────

  _convertClip(clip, itr) {
    const effectReps   = this._convertEffects(clip);
    const transReps    = this._convertTransitions(clip);
    const captionRep   = this._convertCaption(clip);
    const vfMetadata   = this._extractVfMetadata(clip);

    const rep = ClipRepresentation.fromClip(clip, effectReps, captionRep);
    rep.transitions = transReps;
    rep.videoForgeMetadata = vfMetadata;

    // Extract CropEffect params into the ITR crop field so all exporters can
    // read a single source of truth without re-scanning the effects array.
    const cropFx = Array.isArray(clip.effects)
      ? clip.effects.find((e) => e && e.type === EFFECT_TYPES.CROP && e.enabled !== false)
      : null;
    if (cropFx) {
      rep.crop = {
        l: cropFx.params.left   ?? 0,
        r: cropFx.params.right  ?? 0,
        t: cropFx.params.top    ?? 0,
        b: cropFx.params.bottom ?? 0,
      };
    }

    return rep;
  }

  // ─── Effect conversion ────────────────────────────────────────────────────────

  _convertEffects(clip) {
    if (!Array.isArray(clip.effects)) return [];
    return clip.effects
      .filter((e) => e && e.type !== 'transition')
      .map((e) => EffectRepresentation.fromEffect(e));
  }

  // ─── Transition conversion ────────────────────────────────────────────────────

  _convertTransitions(clip) {
    if (!Array.isArray(clip.effects)) return [];
    const transitions = clip.effects.filter(
      (e) => e && (e.type === 'transition' || e.transitionType),
    );

    return transitions.map((t) =>
      TransitionRepresentation.fromTransition(t, clip.id, ''),
    );
  }

  // ─── Caption conversion ───────────────────────────────────────────────────────

  _convertCaption(clip) {
    // CaptionClip has a 'segments' property and extends Clip with CLIP_TYPES.TEXT
    const isCaptionClip = clip.type === CLIP_TYPES.TEXT && Array.isArray(clip.segments);
    if (!isCaptionClip) return null;
    return CaptionRepresentation.fromCaptionClip(clip);
  }

  // ─── VideoForge metadata extraction ──────────────────────────────────────────

  /**
   * Extract properties that have no interchange equivalent into a metadata bag.
   * This preserves them for round-trip reconstruction.
   * @param {object} clip
   * @returns {object}
   */
  _extractVfMetadata(clip) {
    const meta = {};

    // Custom clip-level keyframes
    if (clip.keyframes && Object.keys(clip.keyframes).length > 0) {
      meta.keyframes = clip.keyframes;
    }

    // Caption-specific advanced data
    if (clip.captionAnimations?.length > 0) {
      meta.captionAnimations = clip.captionAnimations.map((a) =>
        typeof a.toJSON === 'function' ? a.toJSON() : a,
      );
    }
    if (clip.captionEffects?.length > 0) {
      meta.captionEffects = clip.captionEffects.map((e) =>
        typeof e.toJSON === 'function' ? e.toJSON() : e,
      );
    }
    if (clip.captionKeyframeSet) {
      meta.captionKeyframeSet = typeof clip.captionKeyframeSet.toJSON === 'function'
        ? clip.captionKeyframeSet.toJSON()
        : clip.captionKeyframeSet;
    }

    // Per-character and per-word animations (too granular for interchange formats)
    if (Array.isArray(clip.segments) && clip.segments.length > 0) {
      const hasWordAnimations = clip.segments.some((seg) =>
        (seg.words ?? []).some((w) => (w.animations ?? []).length > 0),
      );
      const hasCharAnimations = clip.segments.some((seg) =>
        (seg.words ?? []).some((w) =>
          (w.characters ?? []).some((c) => (c.animations ?? []).length > 0),
        ),
      );
      if (hasWordAnimations || hasCharAnimations) {
        meta.hasGranularAnimations = true;
        meta.note = 'Word/character-level animations preserved in captionData.videoForgePayload';
      }
    }

    // Custom speed ramps / variable speed
    if (clip.speedRamp) meta.speedRamp = clip.speedRamp;

    // Custom blend mode
    if (clip.blendMode && clip.blendMode !== 'normal') meta.blendMode = clip.blendMode;

    // Nested sequence reference
    if (clip.nestedSequenceId) meta.nestedSequenceId = clip.nestedSequenceId;

    // TextClip-specific properties (type='text' without a segments array — not a CaptionClip)
    if (clip.type === 'text' && !Array.isArray(clip.segments) && clip.text !== undefined) {
      meta.text       = clip.text;
      meta.fontFamily = clip._fontFamily;
      meta.fontSize   = clip._fontSizeValue;
      meta.color      = clip._colorValue;
      meta.bgColor    = clip._bgColor;
      meta.align      = clip._alignValue;
      meta.bold       = clip._bold;
      meta.italic     = clip._italic;
    }

    // ShapeClip-specific properties
    if (clip.type === 'shape' && clip.shapeType !== undefined) {
      meta.shapeType   = clip.shapeType;
      meta.shapeWidth  = clip._width;
      meta.shapeHeight = clip._height;
      meta.fillColor   = clip._fillColor;
      meta.strokeColor = clip._strokeColorValue;
      meta.strokeWidth = clip._strokeWidthValue;
      meta.rotationDeg = clip._rotationDeg;
      meta.cornerRadius= clip._cornerRadius;
    }

    return meta;
  }
}

export default TimelineConverter;
