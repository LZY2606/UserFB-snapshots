/**
 * @module FilterGraphBuilder
 * Builds the FFmpeg -filter_complex string and corresponding -i input flags
 * from an IntermediateTimeline (ITR).
 *
 * V1 scope: trim (via input-side seeking), speed, reverse, fadeIn, fadeOut,
 * volume, mute.  Gaps between clips are not padded — clips play back-to-back.
 *
 * Embedded audio: video clips whose asset has audioChannels > 0 contribute
 * their :a stream from the same input.  Embedded audio segments are concatenated
 * in timeline order, then mixed with any explicit audio-track clips via amix.
 *
 * V1 limitation: if only SOME video clips in a sequence have embedded audio,
 * the embedded-audio concat will be shorter than the video; silent gaps are
 * not padded.
 */

class FilterGraphBuilder {
  /**
   * @param {import('../../interchange/IntermediateTimeline.js').default} itr
   */
  constructor(itr) {
    this._itr = itr;
  }

  /**
   * Build the filter graph.
   * @returns {{
   *   inputArgs: string[],
   *   filterComplex: string,
   *   videoMap: string|null,
   *   audioMap: string|null,
   * }}
   */
  build() {
    const videoClips = this._itr.getVideoTracks()
      .flatMap(t => t.getSortedClips());
    const audioClips = this._itr.getAudioTracks()
      .flatMap(t => t.getSortedClips());

    const inputArgs   = [];
    const filterParts = [];
    let inputIdx = 0;

    // ── Video segments + embedded audio where available ───────────────────────

    const vLabels    = [];
    const embALabels = [];  // audio labels sourced from video clip inputs

    for (const clip of videoClips) {
      const asset = this._itr.getAsset(clip.assetId);
      if (!asset) continue;

      inputArgs.push(...this._inputFlags(clip, asset.path));

      // Video filter chain
      const vLabel = `v${vLabels.length}`;
      filterParts.push(
        `[${inputIdx}:v]${this._videoFilters(clip).join(',')}[${vLabel}]`,
      );
      vLabels.push(`[${vLabel}]`);

      // Audio stream from the same input (only when asset carries audio)
      if (asset.audioChannels > 0) {
        const vaLabel = `va${embALabels.length}`;
        filterParts.push(
          `[${inputIdx}:a]${this._audioFilters(clip).join(',')}[${vaLabel}]`,
        );
        embALabels.push(`[${vaLabel}]`);
      }

      inputIdx++;
    }

    // ── Explicit audio-track segments ─────────────────────────────────────────

    const expALabels = [];

    for (const clip of audioClips) {
      const asset = this._itr.getAsset(clip.assetId);
      if (!asset) continue;

      inputArgs.push(...this._inputFlags(clip, asset.path));

      const aLabel = `a${expALabels.length}`;
      filterParts.push(
        `[${inputIdx}:a]${this._audioFilters(clip).join(',')}[${aLabel}]`,
      );
      expALabels.push(`[${aLabel}]`);
      inputIdx++;
    }

    // ── Video: concat or pass through ─────────────────────────────────────────

    let videoMap = null;
    if (vLabels.length > 1) {
      filterParts.push(
        `${vLabels.join('')}concat=n=${vLabels.length}:v=1:a=0[vout]`,
      );
      videoMap = '[vout]';
    } else if (vLabels.length === 1) {
      videoMap = vLabels[0];
    }

    // ── Audio: concat embedded segments, then mix everything ──────────────────

    const allALabels = [];

    if (embALabels.length > 1) {
      // Sequential embedded audio (from video clips) — concatenate in order
      filterParts.push(
        `${embALabels.join('')}concat=n=${embALabels.length}:v=0:a=1[embaout]`,
      );
      allALabels.push('[embaout]');
    } else if (embALabels.length === 1) {
      allALabels.push(embALabels[0]);
    }

    allALabels.push(...expALabels);

    let audioMap = null;
    if (allALabels.length > 1) {
      filterParts.push(
        `${allALabels.join('')}amix=inputs=${allALabels.length}:duration=longest[aout]`,
      );
      audioMap = '[aout]';
    } else if (allALabels.length === 1) {
      audioMap = allALabels[0];
    }

    return {
      inputArgs,
      filterComplex: filterParts.join(';'),
      videoMap,
      audioMap,
    };
  }

  // ── Private helpers ───────────────────────────────────────────────────────

  /** Build -ss / -t / -i flags for one clip. */
  _inputFlags(clip, filePath) {
    const args = ['-ss', String(clip.sourceStart)];
    const srcDur = clip.sourceEnd - clip.sourceStart;
    if (isFinite(srcDur) && srcDur > 0) {
      args.push('-t', String(srcDur));
    }
    args.push('-i', filePath);
    return args;
  }

  /** Filter chain for a video stream. */
  _videoFilters(clip) {
    const srcDur = clip.sourceEnd - clip.sourceStart;
    const filters = ['setpts=PTS-STARTPTS'];

    if (clip.reverse) {
      filters.push('reverse', 'setpts=PTS-STARTPTS');
    }

    const speed = clip.speed || 1;
    if (speed !== 1) {
      filters.push(`setpts=PTS/${speed}`);
    }

    // Crop + alignment
    const crop = clip.crop ?? { l: 0, r: 0, t: 0, b: 0 };
    const { l, r, t, b } = crop;
    if (l || r || t || b) {
      const cropFx = this._findEffect(clip, 'crop');
      const alignment = cropFx ? (cropFx.getParam('alignment') ?? 'center') : 'center';
      const [px, py] = _cropPadding(l, r, t, b, alignment);
      filters.push(`crop=iw-${l}-${r}:ih-${t}-${b}:${l}:${t}`);
      filters.push(`pad=iw+${l}+${r}:ih+${t}+${b}:${px}:${py}:black`);
    }

    // Chroma key (background colour removal)
    const chromaKey = this._findEffect(clip, 'chromaKey');
    if (chromaKey) {
      const color      = _toFfmpegColor(chromaKey.getParam('color') ?? '#00FF00');
      const similarity = chromaKey.getParam('similarity') ?? 0.10;
      const blend      = chromaKey.getParam('blend') ?? 0.10;
      const yuv        = chromaKey.getParam('yuv') ?? false;
      filters.push(`chromakey=color=${color}:similarity=${similarity}:blend=${blend}:yuv=${yuv ? 1 : 0}`);
    }

    const outputDur = isFinite(srcDur) ? srcDur / speed : null;

    const fadeIn = this._findEffect(clip, 'fadeIn');
    if (fadeIn) {
      const d = fadeIn.getParam('duration') ?? 1;
      filters.push(`fade=t=in:st=0:d=${d}`);
    }

    const fadeOut = this._findEffect(clip, 'fadeOut');
    if (fadeOut && outputDur !== null) {
      const d = fadeOut.getParam('duration') ?? 1;
      filters.push(`fade=t=out:st=${Math.max(0, outputDur - d)}:d=${d}`);
    }

    return filters;
  }

  /**
   * Filter chain for an audio stream.
   * Used for both explicit AudioClip tracks and embedded audio from VideoClips.
   */
  _audioFilters(clip) {
    const srcDur = clip.sourceEnd - clip.sourceStart;
    const filters = ['asetpts=PTS-STARTPTS'];

    if (clip.reverse) {
      filters.push('areverse', 'asetpts=PTS-STARTPTS');
    }

    const speed = clip.speed || 1;
    if (speed !== 1) {
      filters.push(..._atempoChain(speed));
    }

    if (clip.mute) {
      filters.push('volume=0');
    } else if (clip.volume != null && clip.volume !== 1) {
      filters.push(`volume=${clip.volume}`);
    }

    const outputDur = isFinite(srcDur) ? srcDur / speed : null;

    const fadeIn = this._findEffect(clip, 'fadeIn');
    if (fadeIn) {
      const d = fadeIn.getParam('duration') ?? 1;
      filters.push(`afade=t=in:st=0:d=${d}`);
    }

    const fadeOut = this._findEffect(clip, 'fadeOut');
    if (fadeOut && outputDur !== null) {
      const d = fadeOut.getParam('duration') ?? 1;
      filters.push(`afade=t=out:st=${Math.max(0, outputDur - d)}:d=${d}`);
    }

    return filters;
  }

  _findEffect(clip, type) {
    if (!Array.isArray(clip.effects)) return null;
    return clip.effects.find(e => e.type === type && e.enabled !== false) ?? null;
  }
}

/**
 * Compute the pad filter X/Y offsets that place cropped content at `alignment`
 * within the restored full-size canvas.
 * @param {number} l - Left crop pixels.
 * @param {number} r - Right crop pixels.
 * @param {number} t - Top crop pixels.
 * @param {number} b - Bottom crop pixels.
 * @param {string} alignment - CROP_ALIGNMENT value.
 * @returns {[number|string, number|string]} [padX, padY]
 */
function _cropPadding(l, r, t, b, alignment) {
  const cx = l + r;
  const cy = t + b;
  switch (alignment) {
    case 'top':         return [Math.floor(cx / 2), 0];
    case 'bottom':      return [Math.floor(cx / 2), cy];
    case 'left':        return [0,  Math.floor(cy / 2)];
    case 'right':       return [cx, Math.floor(cy / 2)];
    case 'topLeft':     return [0,  0];
    case 'topRight':    return [cx, 0];
    case 'bottomLeft':  return [0,  cy];
    case 'bottomRight': return [cx, cy];
    default:            return [Math.floor(cx / 2), Math.floor(cy / 2)]; // center
  }
}

/**
 * Normalise a colour value for use in an FFmpeg filter argument.
 * FFmpeg accepts named colours (e.g. "green") and "0xRRGGBB" hex — but not the
 * CSS-style "#RRGGBB" form, so translate a leading "#" to "0x".
 * @param {string} color
 * @returns {string}
 */
function _toFfmpegColor(color) {
  return color.startsWith('#') ? `0x${color.slice(1)}` : color;
}

/**
 * Build an atempo filter chain for the given rate.
 * atempo only accepts [0.5, 2.0]; chain multiple filters for values outside that range.
 * @param {number} rate
 * @returns {string[]}
 */
function _atempoChain(rate) {
  const filters = [];
  let r = rate;
  while (r > 2.0) {
    filters.push('atempo=2.0');
    r /= 2.0;
  }
  while (r < 0.5) {
    filters.push('atempo=0.5');
    r /= 0.5;
  }
  if (Math.abs(r - 1.0) > 1e-9) {
    filters.push(`atempo=${Number(r.toFixed(6))}`);
  }
  return filters;
}

export default FilterGraphBuilder;
