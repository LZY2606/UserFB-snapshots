/**
 * @module FcpxmlExporter
 * Exports a VideoForge Project to Apple Final Cut Pro X FCPXML 1.10 format.
 *
 * Pipeline:
 *   Project → TimelineConverter → IntermediateTimeline → FcpxmlExporter → .fcpxml file
 *
 * Format reference: FCPXML 1.10 (Final Cut Pro 10.6+)
 * Time is expressed as rational fractions ("N/Ds") where D = rate denominator.
 * <resources> declares <format> (sequence properties) and <asset> (one per media file).
 * <library> → <event> → <project> → <sequence> → <spine> contains primary storyline clips.
 * Connected clips (B-roll, titles, captions) attach via lane= attribute on <clip>.
 */

import { promises as fs } from 'fs';
import path from 'path';
import Exporter from './Exporter.js';
import TimelineConverter    from '../interchange/TimelineConverter.js';
import InterchangeValidator from '../interchange/validation/InterchangeValidator.js';
import XmlBuilder           from '../interchange/utils/XmlBuilder.js';
import { escapeAttr, escapeText } from '../interchange/utils/XmlEscaper.js';
import XmlValidator         from '../interchange/utils/XmlValidator.js';
import XmlNamespaceManager  from '../interchange/utils/XmlNamespaceManager.js';
import TimeCode             from '../interchange/utils/TimeCode.js';

class FcpxmlExporter extends Exporter {
  /**
   * @param {import('../core/Project.js').default} project
   * @param {object} [options={}]
   * @param {string}  [options.fcpxmlVersion='1.10']
   * @param {boolean} [options.pretty=true]
   * @param {boolean} [options.validateInput=true]
   * @param {boolean} [options.validateOutput=true]
   * @param {boolean} [options.includeVfMetadata=true]
   * @param {string}  [options.libraryName]   - Override the <library> name attribute.
   * @param {string}  [options.eventName]     - Override the <event> name attribute.
   */
  constructor(project, options = {}) {
    super(project, options);
    this.fcpxmlVersion     = options.fcpxmlVersion     ?? '1.10';
    this.pretty            = options.pretty            ?? true;
    this.validateInput     = options.validateInput     ?? true;
    this.validateOutput    = options.validateOutput    ?? true;
    this.includeVfMetadata = options.includeVfMetadata ?? true;
    this.libraryName       = options.libraryName       ?? null;
    this.eventName         = options.eventName         ?? null;
    this._converter        = new TimelineConverter();
    this._itrValidator     = new InterchangeValidator();
    this._xmlValidator     = new XmlValidator();
    this._nsMgr            = new XmlNamespaceManager();
  }

  // ─── Exporter contract ────────────────────────────────────────────────────────

  async export(outputPath) {
    const dest = this.resolveOutputPath(outputPath, '.fcpxml');
    await fs.mkdir(path.dirname(dest), { recursive: true });
    const xml = this.toString();
    await fs.writeFile(dest, xml, 'utf8');
    return dest;
  }

  /**
   * Generate and return the FCPXML string without writing to disk.
   * @returns {string}
   */
  toString() {
    const itr = this._converter.convert(this.project);

    if (this.validateInput) {
      const result = this._itrValidator.validateTimeline(itr);
      if (!result.valid) {
        throw new Error(
          `FcpxmlExporter: ITR validation failed:\n${result.errors.join('\n')}`,
        );
      }
    }

    const xml = this._buildFcpxml(itr);

    if (this.validateOutput) {
      const xmlResult = this._xmlValidator.validateFcpXml(xml);
      if (!xmlResult.valid) {
        throw new Error(
          `FcpxmlExporter: generated XML failed validation:\n${xmlResult.errors.join('\n')}`,
        );
      }
    }

    return xml;
  }

  // ─── FCPXML document ──────────────────────────────────────────────────────────

  _buildFcpxml(itr) {
    const b = new XmlBuilder({ pretty: this.pretty });
    b.declaration('1.0', 'UTF-8');
    b.doctype('fcpxml');

    b.open('fcpxml', { version: this.fcpxmlVersion });

      // ── <resources> ──
      b.open('resources');
        this._emitResources(b, itr);
      b.close(); // resources

      // ── <library> → <event> → <project> ──
      b.open('library', { location: '', name: escapeAttr(this.libraryName ?? itr.name) });
        b.open('event', { name: escapeAttr(this.eventName ?? itr.name) });
          this._emitProject(b, itr);
        b.close(); // event
      b.close(); // library

      if (this.includeVfMetadata) {
        this._emitVfMetadataComment(b, itr);
      }

    b.close(); // fcpxml

    return b.toString();
  }

  // ─── <resources> ─────────────────────────────────────────────────────────────

  _emitResources(b, itr) {
    // Format for the primary sequence
    b.leaf('format', {
      id:            'r_seq_format',
      name:          `FFVideoFormat${itr.height}p${Math.round(itr.fps)}`,
      frameDuration: TimeCode.fcpFrameDuration(itr.fps),
      width:         String(itr.width),
      height:        String(itr.height),
      colorSpace:    'Rec. 709',
    });

    // One <asset> per unique media file
    for (const asset of itr.assets) {
      this._emitAssetResource(b, asset, itr.fps);
    }

    // Title effect resource (used for caption/text clips)
    b.leaf('effect', {
      id:   'r_effect_title',
      name: 'Basic Title',
      uid:  '.../Titles.localized/Bumper:Opener.localized/Basic Title.localized/Basic Title.moti',
      type: 'motion',
    });
  }

  _emitAssetResource(b, asset, projectFps) {
    const fps = asset.fps || projectFps;
    b.open('asset', {
      id:            escapeAttr(asset.id),
      name:          escapeAttr(asset.name),
      uid:           escapeAttr(asset.uid),
      src:           escapeAttr(
        asset.path.startsWith('file://') ? asset.path : `file://${asset.path}`,
      ),
      duration:      new TimeCode(asset.duration || 0, fps).toFcpRationalDuration(),
      start:         '0s',
      hasVideo:      asset.hasVideo ? '1' : '0',
      hasAudio:      asset.hasAudio ? '1' : '0',
    });

      if (asset.hasVideo || asset.isImage) {
        b.leaf('media-rep', {
          kind: 'original-media',
          src:  escapeAttr(
            asset.path.startsWith('file://') ? asset.path : `file://${asset.path}`,
          ),
        });
      }

    b.close(); // asset
  }

  // ─── <project> → <sequence> → <spine> ────────────────────────────────────────

  _emitProject(b, itr) {
    b.open('project', {
      name: escapeAttr(itr.name),
      uid:  escapeAttr(itr.projectId || 'videoforge_project'),
    });

      const duration = new TimeCode(
        itr.duration || itr.computeDuration(),
        itr.fps,
      ).toFcpRationalDuration();

      b.open('sequence', {
        format:        'r_seq_format',
        duration,
        tcStart:       '0s',
        tcFormat:      this._tcFormat(itr.fps),
        audioLayout:   'stereo',
        audioRate:     String(itr.sampleRate),
      });

        b.open('spine');
          this._emitSpine(b, itr);
        b.close(); // spine

      b.close(); // sequence
    b.close(); // project
  }

  // ─── <spine> — primary storyline ─────────────────────────────────────────────

  _emitSpine(b, itr) {
    const fps          = itr.fps;
    const videoTracks  = itr.getVideoTracks();
    const audioTracks  = itr.getAudioTracks();
    const captionTracks = itr.getCaptionTracks();

    if (videoTracks.length === 0 && audioTracks.length === 0) {
      const dur = new TimeCode(itr.duration || itr.computeDuration(), fps).toFcpRationalDuration();
      b.leaf('gap', { name: 'Empty', offset: '0s', duration: dur, start: '0s' });
      return;
    }

    // The primary video track defines the spine.
    // All secondary video tracks become connected clips (positive lane numbers).
    // Audio tracks become connected clips with negative lane numbers.
    // Caption/text tracks also become connected clips.

    const primaryTrack = videoTracks[0] ?? audioTracks[0];
    const primaryClips = primaryTrack ? primaryTrack.getSortedClips() : [];

    // Build the timeline as a sequence of clips/gaps.
    // Gaps fill holes between clips.
    let cursor = 0;

    for (const clip of primaryClips) {
      // Fill gap before this clip
      if (clip.timelineStart > cursor) {
        const gapDur = new TimeCode(clip.timelineStart - cursor, fps).toFcpRational();
        const gapOff = new TimeCode(cursor, fps).toFcpRational();
        b.leaf('gap', {
          name:     'Gap',
          offset:   gapOff,
          duration: gapDur,
          start:    '0s',
        });
      }

      const asset = itr.getAsset(clip.assetId);
      this._emitSpineClip(b, clip, asset, itr, videoTracks, audioTracks, captionTracks, 1);
      cursor = clip.timelineEnd;
    }

    // If primary track is audio-only, handle audio spine
    if (videoTracks.length === 0 && audioTracks.length > 0) {
      cursor = 0;
      const audioClips = audioTracks[0].getSortedClips();
      for (const clip of audioClips) {
        if (clip.timelineStart > cursor) {
          const gapDur = new TimeCode(clip.timelineStart - cursor, fps).toFcpRational();
          const gapOff = new TimeCode(cursor, fps).toFcpRational();
          b.leaf('gap', { name: 'Gap', offset: gapOff, duration: gapDur, start: '0s' });
        }
        const asset = itr.getAsset(clip.assetId);
        this._emitAudioClip(b, clip, asset, itr, null);
        cursor = clip.timelineEnd;
      }
    }
  }

  // ─── Spine clip (primary video) ───────────────────────────────────────────────

  _emitSpineClip(b, clip, asset, itr, videoTracks, audioTracks, captionTracks, laneBase) {
    const fps      = itr.fps;
    const offset   = new TimeCode(clip.timelineStart, fps).toFcpRational();
    const duration = new TimeCode(clip.timelineDuration, fps).toFcpRational();
    const start    = new TimeCode(clip.sourceStart, fps).toFcpRational();

    const clipAttrs = {
      name:     escapeAttr(clip.name || (asset?.name ?? 'Clip')),
      ref:      escapeAttr(clip.assetId || asset?.id || 'r_seq_format'),
      offset,
      duration,
      start,
      ...(clip.opacity !== 1 ? { opacity: String(clip.opacity) } : {}),
    };

    const hasChildren = this._clipHasChildren(clip, itr, videoTracks, audioTracks, captionTracks);

    if (hasChildren) {
      b.open('clip', clipAttrs);
        this._emitClipChildren(b, clip, itr, videoTracks, audioTracks, captionTracks);
      b.close();
    } else {
      b.leaf('clip', clipAttrs);
    }
  }

  _clipHasChildren(clip, itr, videoTracks, audioTracks, captionTracks) {
    if (videoTracks.length > 1) return true;
    if (audioTracks.length > 0 && itr.getVideoTracks().length > 0) return true;
    if (captionTracks.length > 0) return true;
    if (clip.effects?.length > 0) return true;
    if (clip.speed !== 1) return true;
    if (this._hasCrop(clip)) return true;
    return false;
  }

  _emitClipChildren(b, primaryClip, itr, videoTracks, audioTracks, captionTracks) {
    const fps = itr.fps;

    // Speed time-map for the primary clip
    if (primaryClip.speed !== 1) {
      const tlDur  = new TimeCode(primaryClip.timelineDuration, fps).toFcpRational();
      const srcDur = new TimeCode(primaryClip.timelineDuration * primaryClip.speed, fps).toFcpRational();
      b.open('timeMap');
        b.leaf('timept', { time: '0s', value: '0s', interp: 'linear' });
        b.leaf('timept', { time: tlDur, value: srcDur, interp: 'linear' });
      b.close();
    }

    // Secondary video tracks → positive lanes
    for (let i = 1; i < videoTracks.length; i++) {
      for (const clip of videoTracks[i].getSortedClips()) {
        if (clip.timelineStart >= primaryClip.timelineStart &&
            clip.timelineEnd   <= primaryClip.timelineEnd) {
          const asset = itr.getAsset(clip.assetId);
          this._emitConnectedVideoClip(b, clip, asset, itr, i);
        }
      }
    }

    // Audio tracks → negative lanes
    // Emit each audio clip once, attached to the first spine clip whose window
    // contains the audio clip's start point. The FCPXML `offset` attribute is
    // absolute on the primary storyline, so FCP positions it correctly even when
    // the audio clip extends beyond the parent spine clip's end.
    for (let i = 0; i < audioTracks.length; i++) {
      for (const clip of audioTracks[i].getSortedClips()) {
        if (clip.timelineStart >= primaryClip.timelineStart &&
            clip.timelineStart <  primaryClip.timelineEnd) {
          const asset = itr.getAsset(clip.assetId);
          this._emitAudioClip(b, clip, asset, itr, -(i + 1));
        }
      }
    }

    // Caption tracks → connected title clips
    for (let i = 0; i < captionTracks.length; i++) {
      for (const clip of captionTracks[i].getSortedClips()) {
        if (clip.timelineStart >= primaryClip.timelineStart &&
            clip.timelineEnd   <= primaryClip.timelineEnd) {
          this._emitTitleClip(b, clip, itr, -(i + 1 + audioTracks.length));
        }
      }
    }

    // Crop adjustment
    if (this._hasCrop(primaryClip)) {
      this._emitCropAdjust(b, primaryClip, itr);
    }

    // Clip-level effects as adjust-* elements (skip crop — handled above)
    for (const effect of (primaryClip.effects ?? [])) {
      if (effect.type !== 'crop') {
        this._emitClipEffect(b, effect, itr);
      }
    }
  }

  // ─── Connected video clip ─────────────────────────────────────────────────────

  _emitConnectedVideoClip(b, clip, asset, itr, lane) {
    const fps    = itr.fps;
    const offset = new TimeCode(clip.timelineStart, itr.fps).toFcpRational();
    const dur    = new TimeCode(clip.timelineDuration, fps).toFcpRational();
    const start  = new TimeCode(clip.sourceStart, fps).toFcpRational();

    b.leaf('clip', {
      name:     escapeAttr(clip.name || (asset?.name ?? 'Clip')),
      ref:      escapeAttr(clip.assetId || asset?.id || 'r_seq_format'),
      lane:     String(lane),
      offset,
      duration: dur,
      start,
      ...(clip.opacity !== 1 ? { opacity: String(clip.opacity) } : {}),
    });
  }

  // ─── Audio clip ───────────────────────────────────────────────────────────────

  _emitAudioClip(b, clip, asset, itr, lane) {
    const fps    = itr.fps;
    const offset = new TimeCode(clip.timelineStart, fps).toFcpRational();
    const dur    = new TimeCode(clip.timelineDuration, fps).toFcpRational();
    const start  = new TimeCode(clip.sourceStart, fps).toFcpRational();

    const hasChildren = clip.volume !== 1 || clip.mute || clip.pan !== 0 || clip.speed !== 1;

    const attrs = {
      name:     escapeAttr(clip.name || (asset?.name ?? 'Audio')),
      ref:      escapeAttr(clip.assetId || asset?.id || ''),
      offset,
      duration: dur,
      start,
      ...(lane != null ? { lane: String(lane) } : {}),
      ...(hasChildren ? { audioRole: 'dialogue' } : {}),
    };

    if (hasChildren) {
      b.open('audio-clip', attrs);
        if (clip.mute) {
          b.leaf('adjust-volume', { amount: '-96' });
        } else if (clip.volume !== 1) {
          b.leaf('adjust-volume', { amount: String(this._linearToDb(clip.volume)) });
        }
        if (clip.pan !== 0) {
          b.leaf('adjust-panner', { amount: String(Math.round(clip.pan * 100)) });
        }
        if (clip.speed !== 1) {
          const tlDur  = new TimeCode(clip.timelineDuration, fps).toFcpRational();
          const srcDur = new TimeCode(clip.timelineDuration * clip.speed, fps).toFcpRational();
          b.open('timeMap');
            b.leaf('timept', { time: '0s', value: '0s', interp: 'linear' });
            b.leaf('timept', { time: tlDur, value: srcDur, interp: 'linear' });
          b.close();
        }
      b.close();
    } else {
      b.leaf('audio-clip', attrs);
    }
  }

  // ─── Title clip (for captions) ────────────────────────────────────────────────

  _emitTitleClip(b, clip, itr, lane) {
    const fps    = itr.fps;
    const offset = new TimeCode(clip.timelineStart, fps).toFcpRational();
    const dur    = new TimeCode(clip.timelineDuration, fps).toFcpRational();

    const titleData = clip.captionData?.toFcpTitle(
      clip.timelineStart,
      clip.timelineEnd,
      String(lane),
    ) ?? { text: '', fontSize: 72, color: '#FFFFFF', bold: false, name: 'Basic Title' };

    b.open('title', {
      name:     escapeAttr(titleData.name ?? 'Basic Title'),
      ref:      'r_effect_title',
      lane:     String(lane),
      offset,
      duration: dur,
    });

      // Text param
      b.open('param', { name: 'Text', key: '9999/10003/10003/…/1/100/101' });
        b.open('text');
          b.open('text-style', {
            ref:      'ts1',
            font:     'Helvetica Neue',
            fontSize: String(titleData.fontSize ?? 72),
            bold:     titleData.bold ? '1' : '0',
            italic:   titleData.italic ? '1' : '0',
            fontColor: _hexToFcpColor(titleData.color ?? '#FFFFFF'),
          });
            b.text(escapeText(titleData.text ?? ''));
          b.close();
        b.close(); // text
      b.close(); // param

      // Alignment
      if (titleData.horizontalAlign) {
        b.leaf('param', {
          name:  'Alignment',
          key:   '9999/10003/10003/…/1/100/102',
          value: titleData.horizontalAlign === 'left' ? '0' : titleData.horizontalAlign === 'right' ? '2' : '1',
        });
      }

      // Vertical position
      if (titleData.verticalAlign) {
        b.leaf('param', {
          name:  'Position',
          key:   '9999/10003/10003/…/1/100/103',
          value: titleData.verticalAlign === 'top' ? '0.85' : titleData.verticalAlign === 'bottom' ? '0.15' : '0.5',
        });
      }

      // VideoForge metadata for round-trip
      if (this.includeVfMetadata && clip.captionData?.videoForgePayload) {
        b.comment(`VideoForge Caption Payload: ${JSON.stringify({
          presetName: clip.captionData.presetName,
          animationCount: clip.captionData.animations?.length ?? 0,
          effectCount: clip.captionData.effects?.length ?? 0,
        })}`);
      }

    b.close(); // title
  }

  // ─── Crop adjustment ─────────────────────────────────────────────────────────

  _hasCrop(clip) {
    const c = clip.crop ?? { l: 0, r: 0, t: 0, b: 0 };
    return c.l !== 0 || c.r !== 0 || c.t !== 0 || c.b !== 0;
  }

  /**
   * Emit FCPXML <adjust-crop> (trim mode) and optional <adjust-transform> for
   * non-center alignment.  Crop values are normalized (0–1) fractions of the
   * canvas dimensions.
   */
  _emitCropAdjust(b, clip, itr) {
    const crop = clip.crop ?? { l: 0, r: 0, t: 0, b: 0 };
    const leftN   = (crop.l / itr.width).toFixed(6);
    const rightN  = (crop.r / itr.width).toFixed(6);
    const topN    = (crop.t / itr.height).toFixed(6);
    const bottomN = (crop.b / itr.height).toFixed(6);

    b.open('adjust-crop', { mode: 'trim', enabled: '1' });
      b.leaf('trim-rect', { left: leftN, right: rightN, top: topN, bottom: bottomN });
    b.close(); // adjust-crop

    // Alignment: shift content position so it matches the requested edge.
    const cropFx = (clip.effects ?? []).find((e) => e.type === 'crop');
    const alignment = cropFx ? (cropFx.getParam?.('alignment') ?? 'center') : 'center';
    if (alignment !== 'center') {
      const [ox, oy] = _fcpCropOffset(crop, alignment);
      if (ox !== 0 || oy !== 0) {
        b.leaf('adjust-transform', { position: `${ox} ${oy}` });
      }
    }
  }

  // ─── Clip-level effect ────────────────────────────────────────────────────────

  _emitClipEffect(b, effect, itr) {
    const effectId = effect.type || effect.videoForgeType || 'customEffect';

    b.open('filter-video', {
      name: escapeAttr(effect.type || 'Effect'),
      ref:  escapeAttr(effectId),
    });

      const params = effect.parameters instanceof Map
        ? effect.parameters
        : new Map(Object.entries(effect.parameters ?? {}));

      for (const [key, val] of params) {
        b.leaf('param', {
          name:  escapeAttr(String(key)),
          key:   escapeAttr(String(key)),
          value: escapeAttr(String(val)),
        });
      }

    b.close(); // filter-video
  }

  // ─── Metadata block ───────────────────────────────────────────────────────────

  _emitVfMetadataComment(b, itr) {
    const textTracks = itr.getTextTracks();
    const allClips   = itr.getAllClips();
    const payload = {
      version:    itr.metadata?.videoForge?.version ?? '1.0',
      projectId:  itr.projectId,
      fps:        itr.fps,
      width:      itr.width,
      height:     itr.height,
      sampleRate: itr.sampleRate,
      textTracks: textTracks.map((t) => ({
        id:    t.id,
        name:  t.name,
        clips: t.clips.map((c) => ({
          id:                 c.id,
          timelineStart:      c.timelineStart,
          timelineEnd:        c.timelineEnd,
          videoForgeMetadata: c.videoForgeMetadata,
        })),
      })),
      unsupportedFeatures: [
        ...(allClips.some((c) => c.reverse) ? ['reverse'] : []),
        ...(textTracks.length > 0 ? ['text-clips-as-metadata-only'] : []),
      ],
    };
    b.comment(`VideoForge Export Metadata: ${JSON.stringify(payload)}`);
  }

  // ─── Helpers ──────────────────────────────────────────────────────────────────

  _tcFormat(fps) {
    return TimeCode.premiereRate(fps).ntsc ? 'DF' : 'NDF';
  }

  _linearToDb(linear) {
    if (!linear || linear <= 0) return 0;
    return Math.round(20 * Math.log10(linear) * 100) / 100;
  }
}

// ─── Crop helpers ─────────────────────────────────────────────────────────────

/**
 * Return the FCPXML position offset (in pixels) needed to shift the cropped
 * content to the requested alignment within the original canvas.
 * @param {{ l:number, r:number, t:number, b:number }} crop
 * @param {string} alignment
 * @returns {[number, number]} [x, y]
 */
function _fcpCropOffset(crop, alignment) {
  const hHalf = (crop.l - crop.r) / 2;
  const vHalf = (crop.t - crop.b) / 2;
  const hFull = (crop.l + crop.r) / 2;
  const vFull = (crop.t + crop.b) / 2;
  switch (alignment) {
    case 'top':         return [hHalf,   vFull];
    case 'bottom':      return [hHalf,  -vFull];
    case 'left':        return [hFull,   vHalf];
    case 'right':       return [-hFull,  vHalf];
    case 'topLeft':     return [hFull,   vFull];
    case 'topRight':    return [-hFull,  vFull];
    case 'bottomLeft':  return [hFull,  -vFull];
    case 'bottomRight': return [-hFull, -vFull];
    default:            return [0, 0];
  }
}

// ─── Color helpers ────────────────────────────────────────────────────────────

/**
 * Convert #RRGGBB to FCPXML color string "R G B 1" (normalized 0–1, alpha last).
 * @param {string} hex
 * @returns {string}
 */
function _hexToFcpColor(hex) {
  const m = String(hex).replace('#', '').match(/([0-9a-fA-F]{2})([0-9a-fA-F]{2})([0-9a-fA-F]{2})/);
  if (!m) return '1 1 1 1';
  const r = (parseInt(m[1], 16) / 255).toFixed(4);
  const g = (parseInt(m[2], 16) / 255).toFixed(4);
  const bl = (parseInt(m[3], 16) / 255).toFixed(4);
  return `${r} ${g} ${bl} 1`;
}

export default FcpxmlExporter;
