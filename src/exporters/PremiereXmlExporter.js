/**
 * @module PremiereXmlExporter
 * Exports a VideoForge Project to Adobe Premiere Pro FCP7XML (XMEML v5) format.
 *
 * Pipeline:
 *   Project → TimelineConverter → IntermediateTimeline → PremiereXmlExporter → .xml file
 *
 * Format reference: Adobe Premiere Pro FCP7 XML Interchange (XMEML version 5).
 * Time is expressed as integer frames. Rates use <timebase> + <ntsc> elements.
 * Each unique media file gets one <file> element; clips reference it by id.
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

class PremiereXmlExporter extends Exporter {
  /**
   * @param {import('../core/Project.js').default} project
   * @param {object} [options={}]
   * @param {boolean} [options.pretty=true]              - Indent output XML.
   * @param {boolean} [options.validateInput=true]       - Run ITR validation before exporting.
   * @param {boolean} [options.validateOutput=true]      - Run XML structural validation after generating.
   * @param {boolean} [options.includeVfMetadata=true]   - Embed VideoForge namespace metadata.
   * @param {string}  [options.sequenceName]             - Override the sequence name.
   */
  constructor(project, options = {}) {
    super(project, options);
    this.pretty            = options.pretty           ?? true;
    this.validateInput     = options.validateInput    ?? true;
    this.validateOutput    = options.validateOutput   ?? true;
    this.includeVfMetadata = options.includeVfMetadata ?? true;
    this.sequenceName      = options.sequenceName     ?? null;
    this._converter        = new TimelineConverter();
    this._itrValidator     = new InterchangeValidator();
    this._xmlValidator     = new XmlValidator();
    this._nsMgr            = new XmlNamespaceManager();
  }

  // ─── Exporter contract ────────────────────────────────────────────────────────

  async export(outputPath) {
    const dest = this.resolveOutputPath(outputPath, '.xml');
    await fs.mkdir(path.dirname(dest), { recursive: true });
    const xml = this.toString();
    await fs.writeFile(dest, xml, 'utf8');
    return dest;
  }

  /**
   * Generate and return the XMEML XML string without writing to disk.
   * @returns {string}
   */
  toString() {
    const itr = this._converter.convert(this.project);

    if (this.validateInput) {
      const result = this._itrValidator.validateTimeline(itr);
      if (!result.valid) {
        throw new Error(
          `PremiereXmlExporter: ITR validation failed:\n${result.errors.join('\n')}`,
        );
      }
    }

    const xml = this._buildXmeml(itr);

    if (this.validateOutput) {
      const xmlResult = this._xmlValidator.validatePremiereXml(xml);
      if (!xmlResult.valid) {
        throw new Error(
          `PremiereXmlExporter: generated XML failed validation:\n${xmlResult.errors.join('\n')}`,
        );
      }
    }

    return xml;
  }

  // ─── XMEML document ──────────────────────────────────────────────────────────

  _buildXmeml(itr) {
    const b = new XmlBuilder({ pretty: this.pretty });
    b.declaration('1.0', 'UTF-8');
    b.doctype('xmeml');

    b.open('xmeml', { version: '5' });
      b.open('sequence', { id: escapeAttr(`seq_${itr.projectId || 'main'}`) });
        b.leaf('name', {}, escapeText(this.sequenceName ?? itr.name));
        b.leaf('duration', {}, String(this._totalFrames(itr)));
        this._emitRate(b, itr.fps);

        b.open('timecode');
          this._emitRate(b, itr.fps);
          b.leaf('string',        {}, TimeCode.secondsToSmpte(0, itr.fps));
          b.leaf('frame',         {}, '0');
          b.leaf('displayformat', {}, this._isNtsc(itr.fps) ? 'DF' : 'NDF');
        b.close(); // timecode

        b.open('media');
          this._emitVideoSection(b, itr);
          this._emitAudioSection(b, itr);
        b.close(); // media

        if (this.includeVfMetadata) {
          this._emitVfMetadata(b, itr);
        }

      b.close(); // sequence
    b.close(); // xmeml

    return b.toString();
  }

  // ─── Video section ────────────────────────────────────────────────────────────

  _emitVideoSection(b, itr) {
    b.open('video');
      b.open('format');
        b.open('samplecharacteristics');
          b.leaf('width',  {}, String(itr.width));
          b.leaf('height', {}, String(itr.height));
          this._emitRate(b, itr.fps);
          b.leaf('codec',         {}, 'Apple ProRes 422');
          b.leaf('fielddominance', {}, 'none');
          b.leaf('colordepth',    {}, '24');
        b.close(); // samplecharacteristics
      b.close(); // format

      // Deduplicated file pool: uid → assetId (used to detect first-use).
      const emittedFiles = new Map();

      const videoTracks = itr.getVideoTracks();
      if (videoTracks.length === 0) {
        // Premiere requires at least one <track> element.
        b.open('track'); b.close();
      } else {
        for (const track of videoTracks) {
          this._emitVideoTrack(b, track, itr, emittedFiles);
        }
      }

      // Caption tracks → dedicated video tracks whose clips are <generatoritem>s.
      for (const captionTrack of itr.getCaptionTracks()) {
        this._emitCaptionTrack(b, captionTrack, itr);
      }
    b.close(); // video
  }

  _emitVideoTrack(b, track, itr, emittedFiles) {
    b.open('track');
      const clips = track.getSortedClips();
      for (let i = 0; i < clips.length; i++) {
        const clip  = clips[i];
        const asset = itr.getAsset(clip.assetId);
        this._emitClipItem(b, clip, asset, itr, i + 1, 'video', emittedFiles);
      }
      b.leaf('enabled', {}, 'TRUE');
      b.leaf('locked',  {}, track.locked ? 'TRUE' : 'FALSE');
    b.close(); // track
  }

  // ─── Caption track (as video generator track) ─────────────────────────────────

  _emitCaptionTrack(b, track, itr) {
    b.open('track');
      const clips = track.getSortedClips();
      for (const clip of clips) {
        if (clip.captionData) {
          this._emitCaptionGeneratorItem(b, clip, itr);
        }
      }
      b.leaf('enabled', {}, 'TRUE');
      b.leaf('locked',  {}, track.locked ? 'TRUE' : 'FALSE');
    b.close(); // track
  }

  // ─── Audio section ────────────────────────────────────────────────────────────

  _emitAudioSection(b, itr) {
    b.open('audio');
      b.open('format');
        b.open('samplecharacteristics');
          b.leaf('depth',      {}, '16');
          b.leaf('samplerate', {}, String(itr.sampleRate));
        b.close();
      b.close(); // format

      const audioTracks = itr.getAudioTracks();
      if (audioTracks.length === 0) {
        b.open('track'); b.close();
      } else {
        for (const track of audioTracks) {
          this._emitAudioTrack(b, track, itr);
        }
      }

      // Emit audio tracks for video clips that have embedded audio (e.g. MP4 with sound).
      for (const vtrack of itr.getVideoTracks()) {
        const embeddedAudioClips = vtrack.getSortedClips().filter(
          (c) => itr.getAsset(c.assetId)?.hasAudio,
        );
        if (embeddedAudioClips.length > 0) {
          this._emitEmbeddedAudioTrack(b, embeddedAudioClips, itr);
        }
      }
    b.close(); // audio
  }

  _emitAudioTrack(b, track, itr) {
    const emittedFiles = new Map();
    b.open('track');
      const clips = track.getSortedClips();
      for (let i = 0; i < clips.length; i++) {
        const clip  = clips[i];
        const asset = itr.getAsset(clip.assetId);
        this._emitClipItem(b, clip, asset, itr, i + 1, 'audio', emittedFiles);
      }
      b.leaf('enabled',     {}, 'TRUE');
      b.leaf('locked',      {}, track.locked ? 'TRUE' : 'FALSE');
      b.leaf('outputlevel', {}, String(this._linearToDb(track.volume)));
    b.close(); // track
  }

  _emitEmbeddedAudioTrack(b, clips, itr) {
    const emittedFiles = new Map();
    b.open('track');
      for (let i = 0; i < clips.length; i++) {
        const clip  = clips[i];
        const asset = itr.getAsset(clip.assetId);
        this._emitClipItem(b, clip, asset, itr, i + 1, 'audio', emittedFiles);
      }
      b.leaf('enabled',     {}, 'TRUE');
      b.leaf('locked',      {}, 'FALSE');
      b.leaf('outputlevel', {}, '0');
    b.close(); // track
  }

  // ─── <file> element ────────────────────────────────────────────────────────────

  _emitFile(b, asset, fps) {
    b.open('file', { id: escapeAttr(asset.id) });
      b.leaf('name',     {}, escapeText(asset.name));
      b.leaf('pathurl',  {}, escapeText(
        asset.path.startsWith('file://') ? asset.path : `file://${asset.path}`,
      ));
      b.leaf('duration', {}, String(this._secToFrames(asset.duration, fps)));
      this._emitRate(b, asset.fps || fps);

      if (asset.hasVideo || asset.isImage) {
        b.open('media');
          b.open('video');
            b.open('samplecharacteristics');
              b.leaf('width',  {}, String(asset.width  || 1920));
              b.leaf('height', {}, String(asset.height || 1080));
              this._emitRate(b, asset.fps || fps);
            b.close();
          b.close(); // video
          if (asset.hasAudio) {
            b.open('audio');
              b.leaf('channelcount', {}, String(asset.audioChannels || 2));
              b.leaf('samplerate',   {}, String(asset.sampleRate    || 48000));
              b.leaf('depth',        {}, String(asset.bitDepth      || 16));
            b.close();
          }
        b.close(); // media
      } else if (asset.isAudio) {
        b.open('media');
          b.open('audio');
            b.leaf('channelcount', {}, String(asset.audioChannels || 2));
            b.leaf('samplerate',   {}, String(asset.sampleRate    || 48000));
            b.leaf('depth',        {}, String(asset.bitDepth      || 16));
          b.close();
        b.close(); // media
      }
    b.close(); // file
  }

  // ─── <clipitem> element ────────────────────────────────────────────────────────

  _emitClipItem(b, clip, asset, itr, editOrder, mediaType, emittedFiles) {
    const fps        = itr.fps;
    const startFrame = this._secToFrames(clip.timelineStart, fps);
    const endFrame   = this._secToFrames(clip.timelineEnd,   fps);
    const inFrame    = this._secToFrames(clip.sourceStart,   fps);
    const outFrame   = this._secToFrames(clip.sourceEnd,     fps);

    b.open('clipitem', { id: escapeAttr(`${clip.id}_${mediaType}`) });
      b.leaf('masterclipid', {}, escapeText(clip.id));
      b.leaf('name',    {}, escapeText(clip.name || (asset?.name ?? clip.id)));
      b.leaf('enabled', {}, 'TRUE');
      b.leaf('start',   {}, String(startFrame));
      b.leaf('end',     {}, String(endFrame));
      b.leaf('in',      {}, String(inFrame));
      b.leaf('out',     {}, String(outFrame));

      // File reference — emit full <file> on first use, id-only reference thereafter.
      if (asset) {
        if (emittedFiles.has(asset.uid)) {
          b.open('file', { id: escapeAttr(asset.id) }); b.close();
        } else {
          emittedFiles.set(asset.uid, asset.id);
          this._emitFile(b, asset, fps);
        }
      }

      this._emitRate(b, fps);

      if (mediaType === 'video') {
        b.leaf('sourcetrack', {}, 'video');
      } else {
        b.open('sourcetrack');
          b.leaf('mediatype',  {}, 'audio');
          b.leaf('trackindex', {}, '1');
        b.close();
      }

      // Gain / volume filter
      if (mediaType === 'audio' && clip.volume !== 1) {
        b.open('filter');
          b.open('effect');
            b.leaf('name',       {}, 'Gain');
            b.leaf('effectid',   {}, 'audiolevels');
            b.leaf('effecttype', {}, 'audiolevels');
            b.open('parameter');
              b.leaf('parameterid', {}, 'level');
              b.leaf('name',  {}, 'Level');
              b.leaf('value', {}, String(this._linearToDb(clip.volume)));
            b.close();
          b.close();
        b.close(); // filter
      }

      // Balance (pan) filter for audio clips
      if (mediaType === 'audio' && clip.pan !== 0) {
        b.open('filter');
          b.open('effect');
            b.leaf('name',       {}, 'Balance');
            b.leaf('effectid',   {}, 'audiobalance');
            b.leaf('effecttype', {}, 'audiolevels');
            b.open('parameter');
              b.leaf('parameterid', {}, 'balance');
              b.leaf('name',  {}, 'Balance');
              b.leaf('value', {}, String(Math.round(clip.pan * 100)));
            b.close();
          b.close();
        b.close(); // filter
      }

      // Mute filter for audio clips
      if (mediaType === 'audio' && clip.mute) {
        b.open('filter');
          b.open('effect');
            b.leaf('name',       {}, 'Mute');
            b.leaf('effectid',   {}, 'audimute');
            b.leaf('effecttype', {}, 'audiolevels');
          b.close();
        b.close(); // filter
      }

      // Opacity + fade-in/out animation (fade effects converted to opacity keyframes)
      if (mediaType === 'video') {
        this._emitOpacityWithFades(b, clip, endFrame - startFrame, fps);
      }

      // Crop filter (video only)
      if (mediaType === 'video' && this._hasCrop(clip)) {
        this._emitCropFilter(b, clip, itr);
      }

      // Speed / time remap filter (negative value encodes reverse)
      if (clip.speed !== 1 || clip.reverse) {
        const speedVal = (clip.reverse ? -1 : 1) * clip.speed * 100;
        b.open('filter');
          b.open('effect');
            b.leaf('name',       {}, 'Time Remapping');
            b.leaf('effectid',   {}, 'timeremapping');
            b.leaf('effecttype', {}, 'motion');
            b.open('parameter');
              b.leaf('parameterid', {}, 'speed');
              b.leaf('name',  {}, 'Speed');
              b.leaf('value', {}, String(speedVal));
            b.close();
          b.close();
        b.close(); // filter
      }

      // User-defined effect filters (fade effects handled by _emitOpacityWithFades above)
      for (const effect of (clip.effects ?? [])) {
        if (effect.type !== 'fadeIn' && effect.type !== 'fadeOut') {
          this._emitEffectFilter(b, effect);
        }
      }

      // Transitions
      for (const trans of (clip.transitions ?? [])) {
        this._emitTransitionItem(b, trans, clip, itr);
      }

      // Motion (position / scale / rotation)
      if (this._hasTransform(clip)) {
        this._emitMotionFilter(b, clip, itr);
      }

      // Caption as generator item
      if (clip.isCaption && clip.captionData) {
        this._emitCaptionGeneratorItem(b, clip, itr);
      }

    b.close(); // clipitem
  }

  // ─── Opacity filter ───────────────────────────────────────────────────────────

  _emitOpacityFilter(b, opacity) {
    b.open('filter');
      b.open('effect');
        b.leaf('name',       {}, 'Opacity');
        b.leaf('effectid',   {}, 'opacity');
        b.leaf('effecttype', {}, 'motion');
        b.open('parameter');
          b.leaf('parameterid', {}, 'opacity');
          b.leaf('name',  {}, 'Opacity');
          b.leaf('value', {}, String(Math.round(opacity * 100)));
        b.close();
      b.close();
    b.close(); // filter
  }

  /**
   * Emit an opacity filter with keyframes for fade-in/out effects, or a static
   * opacity value if no fades are present. Fade effects (type 'fadeIn'/'fadeOut')
   * are converted to Premiere-compatible opacity keyframes instead of being emitted
   * as unrecognised effect names.
   * @param {XmlBuilder} b
   * @param {import('../interchange/ClipRepresentation.js').default} clip
   * @param {number} totalFrames - Total clip duration in frames.
   * @param {number} fps
   */
  _emitOpacityWithFades(b, clip, totalFrames, fps) {
    const fadeIn  = (clip.effects ?? []).find((e) => e.type === 'fadeIn');
    const fadeOut = (clip.effects ?? []).find((e) => e.type === 'fadeOut');
    const baseOpacity = (clip.opacity ?? 1) * 100;

    if (!fadeIn && !fadeOut && clip.opacity === 1) return;

    if (!fadeIn && !fadeOut) {
      this._emitOpacityFilter(b, clip.opacity);
      return;
    }

    const keyframes = [];

    if (fadeIn) {
      const fadeSecs   = fadeIn.getParam('duration', 1);
      const fadeFrames = this._secToFrames(fadeSecs, fps);
      const fromVal    = (fadeIn.getParam('fromOpacity', 0)) * 100;
      const toVal      = (fadeIn.getParam('toOpacity',   1)) * 100;
      keyframes.push({ frame: 0,          value: fromVal });
      keyframes.push({ frame: fadeFrames, value: toVal   });
    } else {
      keyframes.push({ frame: 0, value: baseOpacity });
    }

    if (fadeOut) {
      const fadeSecs     = fadeOut.getParam('duration', 1);
      const fadeFrames   = this._secToFrames(fadeSecs, fps);
      const fadeOutStart = totalFrames - fadeFrames;
      const fromVal      = (fadeOut.getParam('fromOpacity', 1)) * 100;
      const toVal        = (fadeOut.getParam('toOpacity',   0)) * 100;
      if (fadeOutStart > (keyframes[keyframes.length - 1]?.frame ?? -1)) {
        keyframes.push({ frame: fadeOutStart, value: fromVal });
      }
      keyframes.push({ frame: totalFrames, value: toVal });
    } else {
      const lastFrame = keyframes[keyframes.length - 1]?.frame ?? -1;
      if (lastFrame < totalFrames) {
        keyframes.push({ frame: totalFrames, value: baseOpacity });
      }
    }

    b.open('filter');
      b.open('effect');
        b.leaf('name',       {}, 'Opacity');
        b.leaf('effectid',   {}, 'opacity');
        b.leaf('effecttype', {}, 'motion');
        b.open('parameter');
          b.leaf('parameterid', {}, 'opacity');
          b.leaf('name',  {}, 'Opacity');
          for (const kf of keyframes) {
            b.open('keyframe');
              b.leaf('when',  {}, String(kf.frame));
              b.leaf('value', {}, String(Math.round(kf.value)));
            b.close();
          }
        b.close();
      b.close();
    b.close(); // filter
  }

  // ─── Motion filter ────────────────────────────────────────────────────────────

  _emitMotionFilter(b, clip, itr) {
    // Include crop-alignment offset so the cropped content is positioned correctly.
    const cropOffset = this._cropAlignmentOffset(clip, itr);
    const cx       = Math.round(itr.width  / 2) + (clip.position?.x ?? 0) + cropOffset.x;
    const cy       = Math.round(itr.height / 2) + (clip.position?.y ?? 0) + cropOffset.y;
    const scaleVal = Math.round((clip.scale?.x ?? 1) * 100);
    const rotation = clip.rotation ?? 0;

    b.open('filter');
      b.open('effect');
        b.leaf('name',       {}, 'Basic Motion');
        b.leaf('effectid',   {}, 'motion');
        b.leaf('effecttype', {}, 'motion');
        b.open('parameter');
          b.leaf('parameterid', {}, 'center');
          b.leaf('name',  {}, 'Center');
          b.leaf('value', {}, `${cx} ${cy}`);
        b.close();
        b.open('parameter');
          b.leaf('parameterid', {}, 'scale');
          b.leaf('name',  {}, 'Scale');
          b.leaf('value', {}, String(scaleVal));
        b.close();
        b.open('parameter');
          b.leaf('parameterid', {}, 'rotation');
          b.leaf('name',  {}, 'Rotation');
          b.leaf('value', {}, String(rotation));
        b.close();
      b.close();
    b.close(); // filter
  }

  // ─── Crop filter ──────────────────────────────────────────────────────────────

  _hasCrop(clip) {
    const c = clip.crop ?? { l: 0, r: 0, t: 0, b: 0 };
    return c.l !== 0 || c.r !== 0 || c.t !== 0 || c.b !== 0;
  }

  /**
   * Emit Premiere Pro's built-in Crop video effect.
   * Values are percentages (0–100) relative to the project canvas size.
   */
  _emitCropFilter(b, clip, itr) {
    const crop = clip.crop ?? { l: 0, r: 0, t: 0, b: 0 };
    const leftPct   = Number((crop.l / itr.width  * 100).toFixed(4));
    const rightPct  = Number((crop.r / itr.width  * 100).toFixed(4));
    const topPct    = Number((crop.t / itr.height * 100).toFixed(4));
    const bottomPct = Number((crop.b / itr.height * 100).toFixed(4));

    b.open('filter');
      b.open('effect');
        b.leaf('name',       {}, 'Crop');
        b.leaf('effectid',   {}, 'crop');
        b.leaf('effecttype', {}, 'video');
        b.leaf('mediatype',  {}, 'video');
        b.open('parameter');
          b.leaf('parameterid', {}, 'left');
          b.leaf('name',  {}, 'Left');
          b.leaf('value', {}, String(leftPct));
        b.close();
        b.open('parameter');
          b.leaf('parameterid', {}, 'right');
          b.leaf('name',  {}, 'Right');
          b.leaf('value', {}, String(rightPct));
        b.close();
        b.open('parameter');
          b.leaf('parameterid', {}, 'top');
          b.leaf('name',  {}, 'Top');
          b.leaf('value', {}, String(topPct));
        b.close();
        b.open('parameter');
          b.leaf('parameterid', {}, 'bottom');
          b.leaf('name',  {}, 'Bottom');
          b.leaf('value', {}, String(bottomPct));
        b.close();
      b.close();
    b.close(); // filter
  }

  /**
   * Compute the pixel offset that aligns the cropped content within the canvas.
   * Used by _emitMotionFilter to shift the clip center.
   * @returns {{ x: number, y: number }}
   */
  _cropAlignmentOffset(clip, itr) {
    if (!this._hasCrop(clip)) return { x: 0, y: 0 };
    const crop = clip.crop ?? { l: 0, r: 0, t: 0, b: 0 };
    const cropFx = (clip.effects ?? []).find((e) => e.type === 'crop');
    const alignment = cropFx ? (cropFx.getParam?.('alignment') ?? 'center') : 'center';
    const hShift = (crop.l - crop.r) / 2;
    const vShift = (crop.t - crop.b) / 2;
    switch (alignment) {
      case 'top':         return { x: hShift,  y: -(crop.t + crop.b) / 2 };
      case 'bottom':      return { x: hShift,  y:  (crop.t + crop.b) / 2 };
      case 'left':        return { x: -(crop.l + crop.r) / 2, y: vShift };
      case 'right':       return { x:  (crop.l + crop.r) / 2, y: vShift };
      case 'topLeft':     return { x: -(crop.l + crop.r) / 2, y: -(crop.t + crop.b) / 2 };
      case 'topRight':    return { x:  (crop.l + crop.r) / 2, y: -(crop.t + crop.b) / 2 };
      case 'bottomLeft':  return { x: -(crop.l + crop.r) / 2, y:  (crop.t + crop.b) / 2 };
      case 'bottomRight': return { x:  (crop.l + crop.r) / 2, y:  (crop.t + crop.b) / 2 };
      default:            return { x: 0, y: 0 }; // center — no offset needed
    }
  }

  // ─── Generic effect filter ────────────────────────────────────────────────────

  _emitEffectFilter(b, effect) {
    const effectId    = effect.type || effect.videoForgeType || 'customEffect';
    const effectLabel = effect.type || effect.videoForgeType || 'Effect';

    b.open('filter');
      b.open('effect');
        b.leaf('name',       {}, escapeText(effectLabel));
        b.leaf('effectid',   {}, escapeText(effectId));
        b.leaf('effecttype', {}, 'video');
        b.leaf('mediatype',  {}, 'video');

        const params = effect.parameters instanceof Map
          ? effect.parameters
          : new Map(Object.entries(effect.parameters ?? {}));

        for (const [key, val] of params) {
          b.open('parameter');
            b.leaf('parameterid', {}, escapeText(String(key)));
            b.leaf('name',  {}, escapeText(String(key)));
            b.leaf('value', {}, escapeText(String(val)));
          b.close();
        }
      b.close();
    b.close(); // filter
  }

  // ─── Transition item ──────────────────────────────────────────────────────────

  _emitTransitionItem(b, trans, clip, itr) {
    const fps       = itr.fps;
    const durFrames = this._secToFrames(trans.duration, fps);
    const cutFrame  = this._secToFrames(clip.timelineEnd, fps);

    b.open('transitionitem');
      b.leaf('rate',      {}, String(fps));
      b.leaf('start',     {}, String(cutFrame - Math.floor(durFrames / 2)));
      b.leaf('end',       {}, String(cutFrame + Math.ceil(durFrames / 2)));
      b.leaf('alignment', {}, this._mapTransAlignment(trans.alignment));
      b.open('effect');
        b.leaf('name',       {}, escapeText(_transitionLabel(trans.type)));
        b.leaf('effectid',   {}, escapeText(_transitionEffectId(trans.type)));
        b.leaf('effecttype', {}, 'transition');
        b.leaf('mediatype',  {}, 'video');
      b.close();
    b.close(); // transitionitem
  }

  // ─── Caption / generator item ─────────────────────────────────────────────────

  _emitCaptionGeneratorItem(b, clip, itr) {
    const fps   = itr.fps;
    const title = clip.captionData.toPremiereTitle(clip.timelineStart, clip.timelineEnd);
    const start = this._secToFrames(clip.timelineStart, fps);
    const end   = this._secToFrames(clip.timelineEnd,   fps);
    const color = title.fontColor;

    b.open('generatoritem', { id: escapeAttr(`gen_${clip.id}`) });
      b.leaf('name',     {}, escapeText((title.text ?? '').slice(0, 50) || 'Title'));
      b.leaf('duration', {}, String(end - start));
      b.leaf('start',    {}, String(start));
      b.leaf('end',      {}, String(end));
      b.leaf('in',       {}, '0');
      b.leaf('out',      {}, String(end - start));
      this._emitRate(b, fps);
      b.open('effect');
        b.leaf('name',       {}, 'Text');
        b.leaf('effectid',   {}, 'Text');
        b.leaf('effecttype', {}, 'generator');
        b.leaf('mediatype',  {}, 'video');

        b.open('parameter');
          b.leaf('parameterid', {}, 'str');
          b.leaf('name',  {}, 'Text');
          b.leaf('value', {}, escapeText(title.text ?? ''));
        b.close();

        b.open('parameter');
          b.leaf('parameterid', {}, 'fontsize');
          b.leaf('name',  {}, 'Size');
          b.leaf('value', {}, String(title.fontSize || 72));
        b.close();

        b.open('parameter');
          b.leaf('parameterid', {}, 'fontstyle');
          b.leaf('name',  {}, 'Style');
          b.leaf('value', {}, title.bold ? 'Bold' : 'Plain');
        b.close();

        b.open('parameter');
          b.leaf('parameterid', {}, 'fontcolor');
          b.leaf('name',  {}, 'Color');
          b.open('value');
            b.leaf('alpha', {}, '255');
            b.leaf('red',   {}, String(Math.round(color.r * 255)));
            b.leaf('green', {}, String(Math.round(color.g * 255)));
            b.leaf('blue',  {}, String(Math.round(color.b * 255)));
          b.close();
        b.close();

        b.open('parameter');
          b.leaf('parameterid', {}, 'origin');
          b.leaf('name',  {}, 'Origin');
          b.leaf('value', {}, `${title.x} ${title.y}`);
        b.close();

      b.close(); // effect

      // Caption clip-level animations → Premiere-compatible keyframe filters
      const clipDurFrames = end - start;
      const animations = clip.captionData.animations ?? [];
      if (animations.length > 0) {
        this._emitCaptionAnimationFilters(b, animations, clipDurFrames, fps, title.x, title.y, itr);
      }

      // Caption container keyframes → Premiere-compatible keyframe filters
      const kfSetData = clip.captionData.videoForgePayload?.captionKeyframeSet;
      if (kfSetData && Object.keys(kfSetData.tracks ?? {}).length > 0) {
        this._emitCaptionKeyframeFilters(b, kfSetData, fps, itr);
      }

    b.close(); // generatoritem
  }

  // ─── Caption animation filters ────────────────────────────────────────────────

  _emitCaptionAnimationFilters(b, animations, clipDurFrames, fps, normalizedX, normalizedY, itr) {
    const unsupportedFound = [];

    for (const anim of animations) {
      if (!anim.enabled) continue;

      const delayFrames    = this._secToFrames(anim.delay    ?? 0,   fps);
      const durationFrames = this._secToFrames(anim.duration ?? 0.5, fps);
      const animEnd        = delayFrames + durationFrames;
      const p              = anim.params ?? {};

      switch (anim.type) {
        case 'fade': {
          const dir         = p.direction ?? 'in';
          const fromOpacity = (p.fromOpacity ?? (dir === 'out' ? 1 : 0)) * 100;
          const toOpacity   = (p.toOpacity   ?? (dir === 'out' ? 0 : 1)) * 100;
          const kfs = delayFrames > 0
            ? [{ frame: 0, value: fromOpacity }, { frame: delayFrames, value: fromOpacity }, { frame: animEnd, value: toOpacity }]
            : [{ frame: 0, value: fromOpacity }, { frame: animEnd, value: toOpacity }];
          b.open('filter');
            b.open('effect');
              b.leaf('name', {}, 'Opacity'); b.leaf('effectid', {}, 'opacity'); b.leaf('effecttype', {}, 'motion');
              b.open('parameter');
                b.leaf('parameterid', {}, 'opacity'); b.leaf('name', {}, 'Opacity');
                for (const kf of kfs) { b.open('keyframe'); b.leaf('when', {}, String(kf.frame)); b.leaf('value', {}, String(Math.round(kf.value))); b.close(); }
              b.close();
            b.close();
          b.close();
          break;
        }

        case 'slide': {
          const dir      = p.direction ?? 'up';
          const distance = p.distance  ?? 40;
          const fade     = p.fade      ?? true;
          const cx = Math.round((normalizedX ?? 0.5) * itr.width);
          const cy = Math.round((normalizedY ?? 0.5) * itr.height);
          let dx0 = 0, dy0 = 0;
          if (dir === 'up')    dy0 = -distance;
          if (dir === 'down')  dy0 = +distance;
          if (dir === 'left')  dx0 = -distance;
          if (dir === 'right') dx0 = +distance;
          const startCenter = `${cx + dx0} ${cy + dy0}`;
          const endCenter   = `${cx} ${cy}`;
          b.open('filter');
            b.open('effect');
              b.leaf('name', {}, 'Basic Motion'); b.leaf('effectid', {}, 'motion'); b.leaf('effecttype', {}, 'motion');
              b.open('parameter');
                b.leaf('parameterid', {}, 'center'); b.leaf('name', {}, 'Center');
                if (delayFrames > 0) { b.open('keyframe'); b.leaf('when', {}, '0'); b.leaf('value', {}, startCenter); b.close(); b.open('keyframe'); b.leaf('when', {}, String(delayFrames)); b.leaf('value', {}, startCenter); b.close(); } else { b.open('keyframe'); b.leaf('when', {}, '0'); b.leaf('value', {}, startCenter); b.close(); }
                b.open('keyframe'); b.leaf('when', {}, String(animEnd)); b.leaf('value', {}, endCenter); b.close();
              b.close();
            b.close();
          b.close();
          if (fade) {
            b.open('filter');
              b.open('effect');
                b.leaf('name', {}, 'Opacity'); b.leaf('effectid', {}, 'opacity'); b.leaf('effecttype', {}, 'motion');
                b.open('parameter');
                  b.leaf('parameterid', {}, 'opacity'); b.leaf('name', {}, 'Opacity');
                  if (delayFrames > 0) { b.open('keyframe'); b.leaf('when', {}, '0'); b.leaf('value', {}, '0'); b.close(); b.open('keyframe'); b.leaf('when', {}, String(delayFrames)); b.leaf('value', {}, '0'); b.close(); } else { b.open('keyframe'); b.leaf('when', {}, '0'); b.leaf('value', {}, '0'); b.close(); }
                  b.open('keyframe'); b.leaf('when', {}, String(animEnd)); b.leaf('value', {}, '100'); b.close();
                b.close();
              b.close();
            b.close();
          }
          break;
        }

        case 'scale':
        case 'zoom': {
          const fromScale = (p.fromScale ?? (anim.type === 'zoom' && p.mode === 'out' ? 1 : anim.type === 'zoom' ? 2 : 0)) * 100;
          const toScale   = (p.toScale   ?? (anim.type === 'zoom' && p.mode === 'out' ? 0 : 1)) * 100;
          const fade      = p.fade ?? (anim.type === 'zoom');
          const kfs = delayFrames > 0
            ? [{ frame: 0, value: fromScale }, { frame: delayFrames, value: fromScale }, { frame: animEnd, value: toScale }]
            : [{ frame: 0, value: fromScale }, { frame: animEnd, value: toScale }];
          b.open('filter');
            b.open('effect');
              b.leaf('name', {}, 'Basic Motion'); b.leaf('effectid', {}, 'motion'); b.leaf('effecttype', {}, 'motion');
              b.open('parameter');
                b.leaf('parameterid', {}, 'scale'); b.leaf('name', {}, 'Scale');
                for (const kf of kfs) { b.open('keyframe'); b.leaf('when', {}, String(kf.frame)); b.leaf('value', {}, String(Math.round(kf.value))); b.close(); }
              b.close();
            b.close();
          b.close();
          if (fade) {
            const fadeFrom = (anim.type === 'zoom' && p.mode === 'out') ? 100 : 0;
            const fadeTo   = (anim.type === 'zoom' && p.mode === 'out') ? 0   : 100;
            const fadeKfs = delayFrames > 0
              ? [{ frame: 0, value: fadeFrom }, { frame: delayFrames, value: fadeFrom }, { frame: animEnd, value: fadeTo }]
              : [{ frame: 0, value: fadeFrom }, { frame: animEnd, value: fadeTo }];
            b.open('filter');
              b.open('effect');
                b.leaf('name', {}, 'Opacity'); b.leaf('effectid', {}, 'opacity'); b.leaf('effecttype', {}, 'motion');
                b.open('parameter');
                  b.leaf('parameterid', {}, 'opacity'); b.leaf('name', {}, 'Opacity');
                  for (const kf of fadeKfs) { b.open('keyframe'); b.leaf('when', {}, String(kf.frame)); b.leaf('value', {}, String(Math.round(kf.value))); b.close(); }
                b.close();
              b.close();
            b.close();
          }
          break;
        }

        case 'rotate': {
          const fromDeg = p.fromDegrees ?? -90;
          const toDeg   = p.toDegrees   ?? 0;
          const kfs = delayFrames > 0
            ? [{ frame: 0, value: fromDeg }, { frame: delayFrames, value: fromDeg }, { frame: animEnd, value: toDeg }]
            : [{ frame: 0, value: fromDeg }, { frame: animEnd, value: toDeg }];
          b.open('filter');
            b.open('effect');
              b.leaf('name', {}, 'Basic Motion'); b.leaf('effectid', {}, 'motion'); b.leaf('effecttype', {}, 'motion');
              b.open('parameter');
                b.leaf('parameterid', {}, 'rotation'); b.leaf('name', {}, 'Rotation');
                for (const kf of kfs) { b.open('keyframe'); b.leaf('when', {}, String(kf.frame)); b.leaf('value', {}, String(kf.value)); b.close(); }
              b.close();
            b.close();
          b.close();
          break;
        }

        default:
          if (!unsupportedFound.includes(anim.type)) unsupportedFound.push(anim.type);
      }
    }

    if (unsupportedFound.length > 0) {
      b.comment(`VideoForge: unsupported caption animations (preserved in vf:metadata): ${unsupportedFound.join(', ')}`);
    }
  }

  // ─── Caption keyframe filters ─────────────────────────────────────────────────

  _emitCaptionKeyframeFilters(b, kfSetData, fps, itr) {
    const tracks = kfSetData.tracks ?? {};

    if (tracks.opacity?.keyframes?.length > 0) {
      const kfs = tracks.opacity.keyframes.map((kf) => ({
        frame: this._secToFrames(kf.time, fps),
        value: Math.round((kf.properties?.opacity ?? 1) * 100),
      }));
      b.open('filter');
        b.open('effect');
          b.leaf('name', {}, 'Opacity'); b.leaf('effectid', {}, 'opacity'); b.leaf('effecttype', {}, 'motion');
          b.open('parameter');
            b.leaf('parameterid', {}, 'opacity'); b.leaf('name', {}, 'Opacity');
            for (const kf of kfs) { b.open('keyframe'); b.leaf('when', {}, String(kf.frame)); b.leaf('value', {}, String(kf.value)); b.close(); }
          b.close();
        b.close();
      b.close();
    }

    const xTrack = tracks.x;
    const yTrack = tracks.y;
    if (xTrack?.keyframes?.length > 0 || yTrack?.keyframes?.length > 0) {
      const timeMap = new Map();
      for (const kf of (xTrack?.keyframes ?? [])) {
        timeMap.set(kf.time, { x: kf.properties?.x ?? 0, y: 0 });
      }
      for (const kf of (yTrack?.keyframes ?? [])) {
        const existing = timeMap.get(kf.time) ?? { x: 0, y: 0 };
        existing.y = kf.properties?.y ?? 0;
        timeMap.set(kf.time, existing);
      }
      const halfW = Math.round(itr.width  / 2);
      const halfH = Math.round(itr.height / 2);
      b.open('filter');
        b.open('effect');
          b.leaf('name', {}, 'Basic Motion'); b.leaf('effectid', {}, 'motion'); b.leaf('effecttype', {}, 'motion');
          b.open('parameter');
            b.leaf('parameterid', {}, 'center'); b.leaf('name', {}, 'Center');
            for (const t of [...timeMap.keys()].sort((a, c) => a - c)) {
              const { x, y } = timeMap.get(t);
              b.open('keyframe');
                b.leaf('when',  {}, String(this._secToFrames(t, fps)));
                b.leaf('value', {}, `${halfW + Math.round(x)} ${halfH + Math.round(y)}`);
              b.close();
            }
          b.close();
        b.close();
      b.close();
    }

    const scaleTrack = tracks.scaleX ?? tracks.scale;
    if (scaleTrack?.keyframes?.length > 0) {
      const prop = tracks.scaleX ? 'scaleX' : 'scale';
      const kfs = scaleTrack.keyframes.map((kf) => ({
        frame: this._secToFrames(kf.time, fps),
        value: Math.round((kf.properties?.[prop] ?? 1) * 100),
      }));
      b.open('filter');
        b.open('effect');
          b.leaf('name', {}, 'Basic Motion'); b.leaf('effectid', {}, 'motion'); b.leaf('effecttype', {}, 'motion');
          b.open('parameter');
            b.leaf('parameterid', {}, 'scale'); b.leaf('name', {}, 'Scale');
            for (const kf of kfs) { b.open('keyframe'); b.leaf('when', {}, String(kf.frame)); b.leaf('value', {}, String(kf.value)); b.close(); }
          b.close();
        b.close();
      b.close();
    }

    if (tracks.rotation?.keyframes?.length > 0) {
      const kfs = tracks.rotation.keyframes.map((kf) => ({
        frame: this._secToFrames(kf.time, fps),
        value: kf.properties?.rotation ?? 0,
      }));
      b.open('filter');
        b.open('effect');
          b.leaf('name', {}, 'Basic Motion'); b.leaf('effectid', {}, 'motion'); b.leaf('effecttype', {}, 'motion');
          b.open('parameter');
            b.leaf('parameterid', {}, 'rotation'); b.leaf('name', {}, 'Rotation');
            for (const kf of kfs) { b.open('keyframe'); b.leaf('when', {}, String(kf.frame)); b.leaf('value', {}, String(kf.value)); b.close(); }
          b.close();
        b.close();
      b.close();
    }
  }

  // ─── VideoForge namespace metadata ────────────────────────────────────────────

  _emitVfMetadata(b, itr) {
    b.comment('VideoForge Metadata — preserves unsupported features for round-trip fidelity');
    const payload = {
      version:    itr.metadata?.videoForge?.version ?? '1.0',
      projectId:  itr.projectId,
      name:       itr.name,
      fps:        itr.fps,
      width:      itr.width,
      height:     itr.height,
      sampleRate: itr.sampleRate,
      captionTracks: itr.getCaptionTracks().map((t) => ({
        id:    t.id,
        name:  t.name,
        clips: t.clips.map((c) => ({
          id:             c.id,
          timelineStart:  c.timelineStart,
          timelineEnd:    c.timelineEnd,
          captionPayload: c.captionData?.videoForgePayload ?? null,
        })),
      })),
      textTracks: itr.getTextTracks().map((t) => ({
        id:    t.id,
        name:  t.name,
        clips: t.clips.map((c) => ({
          id:                 c.id,
          timelineStart:      c.timelineStart,
          timelineEnd:        c.timelineEnd,
          videoForgeMetadata: c.videoForgeMetadata,
        })),
      })),
    };

    b.open('vf:metadata', {
      [`xmlns:${this._nsMgr.vfPrefix}`]: escapeAttr(this._nsMgr.vfNamespace),
    });
      b.leaf('vf:payload', {}, escapeText(JSON.stringify(payload, null, 2)));
    b.close(); // vf:metadata
  }

  // ─── Rate helper ──────────────────────────────────────────────────────────────

  _emitRate(b, fps) {
    const { timebase, ntsc } = TimeCode.premiereRate(fps);
    b.open('rate');
      b.leaf('timebase', {}, String(timebase));
      b.leaf('ntsc',     {}, ntsc ? 'TRUE' : 'FALSE');
    b.close();
  }

  // ─── Numeric helpers ──────────────────────────────────────────────────────────

  _secToFrames(seconds, fps) {
    return Math.round((seconds ?? 0) * fps);
  }

  _totalFrames(itr) {
    return this._secToFrames(itr.duration || itr.computeDuration(), itr.fps);
  }

  _isNtsc(fps) {
    return TimeCode.premiereRate(fps).ntsc;
  }

  _linearToDb(linear) {
    if (!linear || linear <= 0) return 0;
    return Math.round(20 * Math.log10(linear) * 100) / 100;
  }

  _hasTransform(clip) {
    const hasMotion = (
      (clip.position?.x !== 0 || clip.position?.y !== 0) ||
      (clip.scale?.x    !== 1 || clip.scale?.y    !== 1) ||
      (clip.rotation ?? 0) !== 0
    );
    if (hasMotion) return true;
    // Non-center crop alignment requires a motion filter to reposition content.
    if (this._hasCrop(clip)) {
      const cropFx = (clip.effects ?? []).find((e) => e.type === 'crop');
      const alignment = cropFx ? (cropFx.getParam?.('alignment') ?? 'center') : 'center';
      if (alignment !== 'center') return true;
    }
    return false;
  }

  _mapTransAlignment(alignment) {
    const map = {
      center:     'center',
      startBlack: 'start-black',
      endBlack:   'end-black',
    };
    return map[alignment] ?? 'center';
  }
}

// ─── Transition helpers ───────────────────────────────────────────────────────

function _transitionLabel(type) {
  const map = {
    crossDissolve: 'Cross Dissolve',
    dissolveIn:    'Fade In',
    dissolveOut:   'Fade Out',
    wipe:          'Wipe',
    push:          'Push',
    slide:         'Slide',
    zoom:          'Zoom',
    dipToBlack:    'Dip to Black',
    dipToWhite:    'Dip to White',
    dipToColor:    'Dip to Color',
  };
  return map[type] ?? 'Cross Dissolve';
}

function _transitionEffectId(type) {
  const map = {
    crossDissolve: 'Cross Dissolve',
    dissolveIn:    'Additive Dissolve',
    dissolveOut:   'Additive Dissolve',
    wipe:          'Wipe',
    push:          'Push',
    slide:         'Slide',
    zoom:          'Zoom',
    dipToBlack:    'Dip to Black',
    dipToWhite:    'Dip to White',
    dipToColor:    'Dip to Color',
  };
  return map[type] ?? 'Cross Dissolve';
}

export default PremiereXmlExporter;
