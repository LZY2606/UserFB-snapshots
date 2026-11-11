/**
 * mux.js
 *
 * Copyright (c) Brightcove
 * Licensed Apache-2.0 https://github.com/videojs/mux.js/blob/master/LICENSE
 *
 * A stream-based mp2t to mp4 converter. This utility can be used to
 * deliver mp4s to a SourceBuffer on platforms that support native
 * Media Source Extensions.
 */
'use strict';

var Stream = require('../utils/stream.js');
var mp4 = require('./mp4-generator.js');
var frameUtils = require('./frame-utils');
var audioFrameUtils = require('./audio-frame-utils');
var trackDecodeInfo = require('./track-decode-info');
var m2ts = require('../m2ts/m2ts.js');
var clock = require('../utils/clock');
var AdtsStream = require('../codecs/adts.js');
var H264Stream = require('../codecs/h264').H264Stream;
var AacStream = require('../aac');
var isLikelyAacData = require('../aac/utils').isLikelyAacData;
var ONE_SECOND_IN_TS = require('../utils/clock').ONE_SECOND_IN_TS;
var AUDIO_PROPERTIES = require('../constants/audio-properties.js');
var VIDEO_PROPERTIES = require('../constants/video-properties.js');

// object types
var VideoSegmentStream, AudioSegmentStream, Transmuxer, CoalesceStream;

var retriggerForStream = function(key, event) {
  event.stream = key;
  this.trigger('log', event);
};

var addPipelineLogRetriggers = function(transmuxer, pipeline) {
  var keys = Object.keys(pipeline);

  for (var i = 0; i < keys.length; i++) {
    var key = keys[i];

    // skip non-stream keys and headOfPipeline
    // which is just a duplicate
    if (key === 'headOfPipeline' || !pipeline[key].on) {
      continue;
    }

    pipeline[key].on('log', retriggerForStream.bind(transmuxer, key));
  }
};

/**
 * Compare two arrays (even typed) for same-ness
 */
var arrayEquals = function(a, b) {
  var
    i;

  if (a.length !== b.length) {
    return false;
  }

  // compare the value of each element in the array
  for (i = 0; i < a.length; i++) {
    if (a[i] !== b[i]) {
      return false;
    }
  }

  return true;
};

/**
 * Byte-wise comparison of two parameter set NAL units. Either argument may be
 * undefined, in which case they are only equal when both are undefined.
 */
var bytesEqual = function(a, b) {
  if (a === b) {
    return true;
  }
  if (!a || !b || a.byteLength !== b.byteLength) {
    return false;
  }
  for (var i = 0; i < a.byteLength; i++) {
    if (a[i] !== b[i]) {
      return false;
    }
  }
  return true;
};

/**
 * Find the boundaries of the AVC decoder configuration (SPS and PPS NAL units)
 * inside an access unit (an array of NAL units beginning with an AUD). Returns
 * null if the access unit does not carry a complete parameter set pair.
 */
var findConfigInAccessUnit = function(accessUnit) {
  var
    sps = null,
    pps = null,
    i,
    nalUnit;

  for (i = 0; i < accessUnit.length; i++) {
    nalUnit = accessUnit[i];

    if (nalUnit.nalUnitType === 'seq_parameter_set_rbsp') {
      sps = nalUnit;
    } else if (nalUnit.nalUnitType === 'pic_parameter_set_rbsp') {
      pps = nalUnit;
    }
  }

  if (!sps || !pps || !sps.config) {
    return null;
  }

  return {
    sps: sps,
    pps: pps
  };
};

/**
 * Whether a NAL unit carries slice data (NAL unit types 1 through 5).
 * Accepts both the names assigned by H264Stream and the raw
 * `nalUnitTypeCode`, which is the only information available for non-IDR
 * slices because H264Stream does not name them.
 */
var isSliceNal = function(nalUnit) {
  if (nalUnit.nalUnitType === 'slice_layer_without_partitioning_rbsp_idr' ||
      nalUnit.nalUnitType === 'slice_layer_without_partitioning_rbsp' ||
      nalUnit.nalUnitType === 'slice_data_partition_a_rbsp' ||
      nalUnit.nalUnitType === 'slice_data_partition_b_rbsp' ||
      nalUnit.nalUnitType === 'slice_data_partition_c_rbsp') {
    return true;
  }

  if (typeof nalUnit.nalUnitTypeCode === 'number') {
    var typeCode = nalUnit.nalUnitTypeCode & 0x1f;

    return typeCode >= 0x01 && typeCode <= 0x05;
  }

  return false;
};

var isIdrNal = function(nalUnit) {
  return nalUnit.nalUnitType === 'slice_layer_without_partitioning_rbsp_idr' ||
    (typeof nalUnit.nalUnitTypeCode === 'number' &&
      (nalUnit.nalUnitTypeCode & 0x1f) === 0x05);
};

/**
 * Validate a batch of NAL units for the splitVideoConfig mode. Configuration
 * updates are only supported when:
 *   - every access unit that introduces a parameter set carries both an SPS
 *     and a PPS (and the SPS has parsed dimensions),
 *   - parameter sets only appear at the start of an access unit, before any
 *     slice NAL unit,
 *   - a new parameter set only takes effect on an IDR access unit.
 * Throws an Error describing the first violation encountered.
 */
var validateSplitNals = function(nalUnits, priorConfig) {
  var
    frames = frameUtils.groupNalsIntoFrames(nalUnits),
    activeConfig = priorConfig ? {
      sps: priorConfig.sps,
      pps: priorConfig.pps
    } : null,
    i,
    j,
    frame,
    nalUnit,
    sawSlice,
    frameSps,
    framePps,
    frameHasIdr,
    changed;

  if (!frames.length) {
    return;
  }

  for (i = 0; i < frames.length; i++) {
    frame = frames[i];
    sawSlice = false;
    frameSps = null;
    framePps = null;
    frameHasIdr = false;

    for (j = 0; j < frame.length; j++) {
      nalUnit = frame[j];

      if (nalUnit.nalUnitType === 'seq_parameter_set_rbsp') {
        if (sawSlice) {
          throw new Error('splitVideoConfig: parameter sets can only change at ' +
            'the start of an access unit, but an SPS was found after a slice');
        }
        if (!nalUnit.config) {
          throw new Error('splitVideoConfig: a sequence parameter set could not ' +
            'be parsed');
        }
        frameSps = nalUnit;
      } else if (nalUnit.nalUnitType === 'pic_parameter_set_rbsp') {
        if (sawSlice) {
          throw new Error('splitVideoConfig: parameter sets can only change at ' +
            'the start of an access unit, but a PPS was found after a slice');
        }
        framePps = nalUnit;
      } else if (isSliceNal(nalUnit)) {
        sawSlice = true;
        if (isIdrNal(nalUnit)) {
          frameHasIdr = true;
        }
      }
    }

    // a parameter set introduced inside an access unit must be a complete pair
    if ((frameSps && !framePps) || (framePps && !frameSps)) {
      throw new Error('splitVideoConfig: incomplete parameter set, every access ' +
        'unit that carries an SPS or PPS must carry both');
    }

    if (frameSps && framePps) {
      if (!activeConfig) {
        activeConfig = {
          sps: frameSps,
          pps: framePps
        };
      } else {
        changed = !bytesEqual(frameSps.data, activeConfig.sps.data) ||
          !bytesEqual(framePps.data, activeConfig.pps.data);

        if (changed && !frameHasIdr) {
          throw new Error('splitVideoConfig: a new SPS/PPS configuration can only ' +
            'take effect on an IDR access unit');
        }

        if (changed) {
          activeConfig = {
            sps: frameSps,
            pps: framePps
          };
        }
      }
    } else if (!activeConfig) {
      throw new Error('splitVideoConfig: missing complete SPS/PPS parameter sets ' +
        'before any access unit was configured');
    }
  }
};

/**
 * Group access-unit frames into contiguous ranges that share one SPS/PPS
 * pair. Frame durations are computed over the whole batch before splitting,
 * so the last frame of an old configuration still gets its duration from the
 * following access unit's DTS even when that access unit starts a new
 * configuration. Access units without inline parameter sets inherit the
 * currently active pair.
 */
var splitFramesByConfig = function(frames, priorConfig) {
  var
    ranges = [],
    activeConfig = priorConfig ? {
      sps: priorConfig.sps,
      pps: priorConfig.pps
    } : null,
    current = null,
    i,
    frameConfig,
    changed;

  if (activeConfig) {
    // access units without inline parameter sets continue the configuration
    // left active by the previous flush
    current = {
      sps: activeConfig.sps,
      pps: activeConfig.pps,
      frames: []
    };
  }

  for (i = 0; i < frames.length; i++) {
    frameConfig = findConfigInAccessUnit(frames[i]);

    if (frameConfig) {
      changed = !activeConfig ||
        !bytesEqual(frameConfig.sps.data, activeConfig.sps.data) ||
        !bytesEqual(frameConfig.pps.data, activeConfig.pps.data);

      if (changed) {
        if (current && current.frames.length) {
          ranges.push(current);
        }
        activeConfig = frameConfig;
        current = {
          sps: frameConfig.sps,
          pps: frameConfig.pps,
          frames: []
        };
      }
    }

    if (!current) {
      throw new Error('splitVideoConfig: missing complete SPS/PPS parameter sets');
    }

    current.frames.push(frames[i]);
  }

  if (current && current.frames.length) {
    ranges.push(current);
  }

  return ranges;
};

/**
 * Build an immutable snapshot of the video track for a configuration range.
 * The snapshot owns its parameter set, sample and timing data so that later
 * input or flushes cannot mutate data already handed to the caller.
 */
var snapshotTrackForConfig = function(track, config) {
  var
    snapshot = {
      id: track.id,
      type: track.type,
      codec: track.codec,
      timelineStartInfo: {
        dts: track.timelineStartInfo.dts,
        pts: track.timelineStartInfo.pts,
        baseMediaDecodeTime: track.timelineStartInfo.baseMediaDecodeTime
      },
      sps: [new Uint8Array(config.sps.data)],
      pps: [new Uint8Array(config.pps.data)],
      width: config.sps.config.width,
      height: config.sps.config.height,
      profileIdc: config.sps.config.profileIdc,
      levelIdc: config.sps.config.levelIdc,
      profileCompatibility: config.sps.config.profileCompatibility,
      sarRatio: config.sps.config.sarRatio
    };

  return snapshot;
};

var generateSegmentTimingInfo = function(
  baseMediaDecodeTime,
  startDts,
  startPts,
  endDts,
  endPts,
  prependedContentDuration
) {
  var
    ptsOffsetFromDts = startPts - startDts,
    decodeDuration = endDts - startDts,
    presentationDuration = endPts - startPts;

  // The PTS and DTS values are based on the actual stream times from the segment,
  // however, the player time values will reflect a start from the baseMediaDecodeTime.
  // In order to provide relevant values for the player times, base timing info on the
  // baseMediaDecodeTime and the DTS and PTS durations of the segment.
  return {
    start: {
      dts: baseMediaDecodeTime,
      pts: baseMediaDecodeTime + ptsOffsetFromDts
    },
    end: {
      dts: baseMediaDecodeTime + decodeDuration,
      pts: baseMediaDecodeTime + presentationDuration
    },
    prependedContentDuration: prependedContentDuration,
    baseMediaDecodeTime: baseMediaDecodeTime
  };
};

/**
 * Constructs a single-track, ISO BMFF media segment from AAC data
 * events. The output of this stream can be fed to a SourceBuffer
 * configured with a suitable initialization segment.
 * @param track {object} track metadata configuration
 * @param options {object} transmuxer options object
 * @param options.keepOriginalTimestamps {boolean} If true, keep the timestamps
 *        in the source; false to adjust the first segment to start at 0.
 */
AudioSegmentStream = function(track, options) {
  var
    adtsFrames = [],
    sequenceNumber,
    earliestAllowedDts = 0,
    audioAppendStartTs = 0,
    videoBaseMediaDecodeTime = Infinity;

  options = options || {};

  sequenceNumber = options.firstSequenceNumber || 0;

  AudioSegmentStream.prototype.init.call(this);

  this.push = function(data) {
    trackDecodeInfo.collectDtsInfo(track, data);

    if (track) {
      AUDIO_PROPERTIES.forEach(function(prop) {
        track[prop] = data[prop];
      });
    }

    // buffer audio data until end() is called
    adtsFrames.push(data);
  };

  this.setEarliestDts = function(earliestDts) {
    earliestAllowedDts = earliestDts;
  };

  this.setVideoBaseMediaDecodeTime = function(baseMediaDecodeTime) {
    videoBaseMediaDecodeTime = baseMediaDecodeTime;
  };

  this.setAudioAppendStart = function(timestamp) {
    audioAppendStartTs = timestamp;
  };

  this.flush = function() {
    var
      frames,
      moof,
      mdat,
      boxes,
      frameDuration,
      segmentDuration,
      videoClockCyclesOfSilencePrefixed;

    // return early if no audio data has been observed
    if (adtsFrames.length === 0) {
      this.trigger('done', 'AudioSegmentStream');
      return;
    }

    frames = audioFrameUtils.trimAdtsFramesByEarliestDts(
      adtsFrames, track, earliestAllowedDts);
    track.baseMediaDecodeTime = trackDecodeInfo.calculateTrackBaseMediaDecodeTime(
      track, options.keepOriginalTimestamps);

    // amount of audio filled but the value is in video clock rather than audio clock
    videoClockCyclesOfSilencePrefixed = audioFrameUtils.prefixWithSilence(
      track, frames, audioAppendStartTs, videoBaseMediaDecodeTime);

    // we have to build the index from byte locations to
    // samples (that is, adts frames) in the audio data
    track.samples = audioFrameUtils.generateSampleTable(frames);

    // concatenate the audio data to constuct the mdat
    mdat = mp4.mdat(audioFrameUtils.concatenateFrameData(frames));

    adtsFrames = [];

    moof = mp4.moof(sequenceNumber, [track]);
    boxes = new Uint8Array(moof.byteLength + mdat.byteLength);

    // bump the sequence number for next time
    sequenceNumber++;

    boxes.set(moof);
    boxes.set(mdat, moof.byteLength);

    trackDecodeInfo.clearDtsInfo(track);

    frameDuration = Math.ceil(ONE_SECOND_IN_TS * 1024 / track.samplerate);

    // TODO this check was added to maintain backwards compatibility (particularly with
    // tests) on adding the timingInfo event. However, it seems unlikely that there's a
    // valid use-case where an init segment/data should be triggered without associated
    // frames. Leaving for now, but should be looked into.
    if (frames.length) {
      segmentDuration = frames.length * frameDuration;

      this.trigger(
        'segmentTimingInfo',
        generateSegmentTimingInfo(
          // The audio track's baseMediaDecodeTime is in audio clock cycles, but the
          // frame info is in video clock cycles. Convert to match expectation of
          // listeners (that all timestamps will be based on video clock cycles).
          clock.audioTsToVideoTs(track.baseMediaDecodeTime, track.samplerate),
          // frame times are already in video clock, as is segment duration
          frames[0].dts,
          frames[0].pts,
          frames[0].dts + segmentDuration,
          frames[0].pts + segmentDuration,
          videoClockCyclesOfSilencePrefixed || 0
        )
      );

      this.trigger('timingInfo', {
        start: frames[0].pts,
        end: frames[0].pts + segmentDuration
      });
    }
    this.trigger('data', {track: track, boxes: boxes});
    this.trigger('done', 'AudioSegmentStream');
  };

  this.reset = function() {
    trackDecodeInfo.clearDtsInfo(track);
    adtsFrames = [];
    this.trigger('reset');
  };
};

AudioSegmentStream.prototype = new Stream();

/**
 * Constructs a single-track, ISO BMFF media segment from H264 data
 * events. The output of this stream can be fed to a SourceBuffer
 * configured with a suitable initialization segment.
 * @param track {object} track metadata configuration
 * @param options {object} transmuxer options object
 * @param options.alignGopsAtEnd {boolean} If true, start from the end of the
 *        gopsToAlignWith list when attempting to align gop pts
 * @param options.keepOriginalTimestamps {boolean} If true, keep the timestamps
 *        in the source; false to adjust the first segment to start at 0.
 */
VideoSegmentStream = function(track, options) {
  var
    sequenceNumber,
    nalUnits = [],
    gopsToAlignWith = [],
    config,
    pps,
    // the SPS/PPS pair active at the end of the most recent split-mode flush,
    // so a later flush can start with access units that omit inline params
    activeSplitConfig = null;

  options = options || {};

  sequenceNumber = options.firstSequenceNumber || 0;

  VideoSegmentStream.prototype.init.call(this);

  delete track.minPTS;

  this.gopCache_ = [];

  /**
    * Constructs a ISO BMFF segment given H264 nalUnits
    * @param {Object} nalUnit A data event representing a nalUnit
    * @param {String} nalUnit.nalUnitType
    * @param {Object} nalUnit.config Properties for a mp4 track
    * @param {Uint8Array} nalUnit.data The nalUnit bytes
    * @see lib/codecs/h264.js
   **/
  this.push = function(nalUnit) {
    trackDecodeInfo.collectDtsInfo(track, nalUnit);

    // In splitVideoConfig mode the track config is captured per configuration
    // range at flush time, so leave the shared track object untouched here.
    if (!options.splitVideoConfig) {
      // record the track config
      if (nalUnit.nalUnitType === 'seq_parameter_set_rbsp' && !config) {
        config = nalUnit.config;
        track.sps = [nalUnit.data];

        VIDEO_PROPERTIES.forEach(function(prop) {
          track[prop] = config[prop];
        }, this);
      }

      if (nalUnit.nalUnitType === 'pic_parameter_set_rbsp' &&
          !pps) {
        pps = nalUnit.data;
        track.pps = [nalUnit.data];
      }
    }

    // buffer video until flush() is called
    nalUnits.push(nalUnit);
  };

  /**
    * Pass constructed ISO BMFF track and boxes on to the
    * next stream in the pipeline
   **/
  this.flush = function() {
    if (options.splitVideoConfig) {
      return this.flushSplitVideoConfig_();
    }

    var
      frames,
      gopForFusion,
      gops,
      moof,
      mdat,
      boxes,
      prependedContentDuration = 0,
      firstGop,
      lastGop;

    // Throw away nalUnits at the start of the byte stream until
    // we find the first AUD
    while (nalUnits.length) {
      if (nalUnits[0].nalUnitType === 'access_unit_delimiter_rbsp') {
        break;
      }
      nalUnits.shift();
    }

    // Return early if no video data has been observed
    if (nalUnits.length === 0) {
      this.resetStream_();
      this.trigger('done', 'VideoSegmentStream');
      return;
    }

    // Organize the raw nal-units into arrays that represent
    // higher-level constructs such as frames and gops
    // (group-of-pictures)
    frames = frameUtils.groupNalsIntoFrames(nalUnits);
    gops = frameUtils.groupFramesIntoGops(frames);

    // If the first frame of this fragment is not a keyframe we have
    // a problem since MSE (on Chrome) requires a leading keyframe.
    //
    // We have two approaches to repairing this situation:
    // 1) GOP-FUSION:
    //    This is where we keep track of the GOPS (group-of-pictures)
    //    from previous fragments and attempt to find one that we can
    //    prepend to the current fragment in order to create a valid
    //    fragment.
    // 2) KEYFRAME-PULLING:
    //    Here we search for the first keyframe in the fragment and
    //    throw away all the frames between the start of the fragment
    //    and that keyframe. We then extend the duration and pull the
    //    PTS of the keyframe forward so that it covers the time range
    //    of the frames that were disposed of.
    //
    // #1 is far prefereable over #2 which can cause "stuttering" but
    // requires more things to be just right.
    if (!gops[0][0].keyFrame) {
      // Search for a gop for fusion from our gopCache
      gopForFusion = this.getGopForFusion_(nalUnits[0], track);

      if (gopForFusion) {
        // in order to provide more accurate timing information about the segment, save
        // the number of seconds prepended to the original segment due to GOP fusion
        prependedContentDuration = gopForFusion.duration;

        gops.unshift(gopForFusion);
        // Adjust Gops' metadata to account for the inclusion of the
        // new gop at the beginning
        gops.byteLength += gopForFusion.byteLength;
        gops.nalCount += gopForFusion.nalCount;
        gops.pts = gopForFusion.pts;
        gops.dts = gopForFusion.dts;
        gops.duration += gopForFusion.duration;
      } else {
        // If we didn't find a candidate gop fall back to keyframe-pulling
        gops = frameUtils.extendFirstKeyFrame(gops);
      }
    }

    // Trim gops to align with gopsToAlignWith
    if (gopsToAlignWith.length) {
      var alignedGops;

      if (options.alignGopsAtEnd) {
        alignedGops = this.alignGopsAtEnd_(gops);
      } else {
        alignedGops = this.alignGopsAtStart_(gops);
      }

      if (!alignedGops) {
        // save all the nals in the last GOP into the gop cache
        this.gopCache_.unshift({
          gop: gops.pop(),
          pps: track.pps,
          sps: track.sps
        });

        // Keep a maximum of 6 GOPs in the cache
        this.gopCache_.length = Math.min(6, this.gopCache_.length);

        // Clear nalUnits
        nalUnits = [];

        // return early no gops can be aligned with desired gopsToAlignWith
        this.resetStream_();
        this.trigger('done', 'VideoSegmentStream');
        return;
      }

      // Some gops were trimmed. clear dts info so minSegmentDts and pts are correct
      // when recalculated before sending off to CoalesceStream
      trackDecodeInfo.clearDtsInfo(track);

      gops = alignedGops;
    }

    trackDecodeInfo.collectDtsInfo(track, gops);

    // First, we have to build the index from byte locations to
    // samples (that is, frames) in the video data
    track.samples = frameUtils.generateSampleTable(gops);

    // Concatenate the video data and construct the mdat
    mdat = mp4.mdat(frameUtils.concatenateNalData(gops));

    track.baseMediaDecodeTime = trackDecodeInfo.calculateTrackBaseMediaDecodeTime(
      track, options.keepOriginalTimestamps);

    this.trigger('processedGopsInfo', gops.map(function(gop) {
      return {
        pts: gop.pts,
        dts: gop.dts,
        byteLength: gop.byteLength
      };
    }));

    firstGop = gops[0];
    lastGop = gops[gops.length - 1];

    this.trigger(
      'segmentTimingInfo',
      generateSegmentTimingInfo(
        track.baseMediaDecodeTime,
        firstGop.dts,
        firstGop.pts,
        lastGop.dts + lastGop.duration,
        lastGop.pts + lastGop.duration,
        prependedContentDuration));

    this.trigger('timingInfo', {
      start: gops[0].pts,
      end: gops[gops.length - 1].pts + gops[gops.length - 1].duration
    });

    // save all the nals in the last GOP into the gop cache
    this.gopCache_.unshift({
      gop: gops.pop(),
      pps: track.pps,
      sps: track.sps
    });

    // Keep a maximum of 6 GOPs in the cache
    this.gopCache_.length = Math.min(6, this.gopCache_.length);

    // Clear nalUnits
    nalUnits = [];

    this.trigger('baseMediaDecodeTime', track.baseMediaDecodeTime);
    this.trigger('timelineStartInfo', track.timelineStartInfo);

    moof = mp4.moof(sequenceNumber, [track]);

    // it would be great to allocate this array up front instead of
    // throwing away hundreds of media segment fragments
    boxes = new Uint8Array(moof.byteLength + mdat.byteLength);

    // Bump the sequence number for next time
    sequenceNumber++;

    boxes.set(moof);
    boxes.set(mdat, moof.byteLength);

    this.trigger('data', {track: track, boxes: boxes});

    this.resetStream_();

    // Continue with the flush process now
    this.trigger('done', 'VideoSegmentStream');
  };

  /**
    * splitVideoConfig flush path. One outer flush can emit several video data
    * events, in input order, each carrying an initialization configuration
    * that matches the samples inside it. All validation happens before any
    * data event is emitted, and a single done event fires only after every
    * data event has been delivered.
   **/
  this.flushSplitVideoConfig_ = function() {
    var
      ranges,
      range,
      event,
      frames,
      i;

    // Throw away nalUnits at the start of the byte stream until
    // we find the first AUD
    while (nalUnits.length) {
      if (nalUnits[0].nalUnitType === 'access_unit_delimiter_rbsp') {
        break;
      }
      nalUnits.shift();
    }

    // An empty flush produces no media fragment and no data events.
    if (nalUnits.length === 0) {
      this.resetStream_();
      this.trigger('done', 'VideoSegmentStream');
      return;
    }

    // Validate the whole batch up front so that, on failure, no data from this
    // batch is emitted. The stream state is reset afterwards so that a later
    // reset() allows legal input to be processed.
    try {
      validateSplitNals(nalUnits, activeSplitConfig);
    } catch (err) {
      nalUnits = [];
      this.resetStream_();
      this.trigger('error', err);
      this.trigger('done', 'VideoSegmentStream');
      return;
    }

    // Group the whole batch into access-unit frames before splitting so that
    // the duration of the final frame of one configuration is still derived
    // from the DTS of the access unit that starts the next configuration.
    frames = frameUtils.groupNalsIntoFrames(nalUnits);

    ranges = splitFramesByConfig(frames, activeSplitConfig);
    nalUnits = [];

    for (i = 0; i < ranges.length; i++) {
      range = ranges[i];
      event = this.buildSplitFragment_(range);

      if (event) {
        this.trigger('data', event);
      }

      // track the configuration now in force even if a range produced no box
      activeSplitConfig = {
        sps: range.sps,
        pps: range.pps
      };
    }

    this.resetStream_();

    // Exactly one done per outer flush, after every data event.
    this.trigger('done', 'VideoSegmentStream');
  };

  /**
    * Turn one contiguous, single-configuration range of access-unit frames
    * into a video data event. Uses the same frame-utils grouping, GOP fusion
    * and mp4-generator boxes as the legacy path, but against a private track
    * snapshot so emitted data can never be mutated by later input.
    *
    * Returns null when the range contributes no samples (for example, when
    * its only GOP was saved into the GOP cache for cross-flush fusion).
   **/
  this.buildSplitFragment_ = function(range) {
    var
      snapshot = snapshotTrackForConfig(track, range),
      gops = frameUtils.groupFramesIntoGops(range.frames),
      gopForFusion,
      moof,
      mdat,
      boxes,
      firstGop,
      lastGop;

    // GOP fusion is only allowed with an identical SPS/PPS pair. The cache
    // is keyed on parameter set bytes, so a stale, differently-configured GOP
    // can never be prepended to this range.
    if (!gops[0][0].keyFrame) {
      gopForFusion = this.getGopForFusion_(range.frames[0][0], snapshot);

      if (gopForFusion) {
        gops.unshift(gopForFusion);
        gops.byteLength += gopForFusion.byteLength;
        gops.nalCount += gopForFusion.nalCount;
        gops.pts = gopForFusion.pts;
        gops.dts = gopForFusion.dts;
        gops.duration += gopForFusion.duration;
      } else {
        // fall back to the same keyframe-pulling repair as the legacy path
        gops = frameUtils.extendFirstKeyFrame(gops);
      }
    }

    if (!gops.length) {
      return null;
    }

    trackDecodeInfo.collectDtsInfo(snapshot, gops);

    snapshot.samples = frameUtils.generateSampleTable(gops);
    mdat = mp4.mdat(frameUtils.concatenateNalData(gops));

    snapshot.baseMediaDecodeTime = trackDecodeInfo.calculateTrackBaseMediaDecodeTime(
      snapshot, options.keepOriginalTimestamps);

    firstGop = gops[0];
    lastGop = gops[gops.length - 1];

    this.trigger(
      'segmentTimingInfo',
      generateSegmentTimingInfo(
        snapshot.baseMediaDecodeTime,
        firstGop.dts,
        firstGop.pts,
        lastGop.dts + lastGop.duration,
        lastGop.pts + lastGop.duration,
        0));

    this.trigger('timingInfo', {
      start: firstGop.pts,
      end: lastGop.pts + lastGop.duration
    });

    this.trigger('processedGopsInfo', gops.map(function(gop) {
      return {
        pts: gop.pts,
        dts: gop.dts,
        byteLength: gop.byteLength
      };
    }));

    // Save the last GOP for cross-flush fusion, tagged with this range's
    // exact parameter set bytes.
    this.cacheSplitGop_(gops.pop(), snapshot);

    this.trigger('baseMediaDecodeTime', snapshot.baseMediaDecodeTime);
    this.trigger('timelineStartInfo', snapshot.timelineStartInfo);

    moof = mp4.moof(sequenceNumber, [snapshot]);
    boxes = new Uint8Array(moof.byteLength + mdat.byteLength);

    // sequence numbers keep increasing across configuration ranges and flushes
    sequenceNumber++;

    boxes.set(moof);
    boxes.set(mdat, moof.byteLength);

    return {
      track: snapshot,
      boxes: boxes
    };
  };

  // Store a GOP for cross-flush fusion. The cached parameter set bytes are
  // copied so later input cannot rewrite a configuration already cached.
  this.cacheSplitGop_ = function(gop, snapshot) {
    this.gopCache_.unshift({
      gop: gop,
      pps: [new Uint8Array(snapshot.pps[0])],
      sps: [new Uint8Array(snapshot.sps[0])]
    });

    // Keep a maximum of 6 GOPs in the cache
    this.gopCache_.length = Math.min(6, this.gopCache_.length);
  };

  this.reset = function() {
    this.resetStream_();
    nalUnits = [];
    this.gopCache_.length = 0;
    gopsToAlignWith.length = 0;
    activeSplitConfig = null;
    this.trigger('reset');
  };

  this.resetStream_ = function() {
    trackDecodeInfo.clearDtsInfo(track);

    // reset config and pps because they may differ across segments
    // for instance, when we are rendition switching
    config = undefined;
    pps = undefined;
  };

  // Search for a candidate Gop for gop-fusion from the gop cache and
  // return it or return null if no good candidate was found
  this.getGopForFusion_ = function(nalUnit, trackOverride) {
    var
      halfSecond = 45000, // Half-a-second in a 90khz clock
      allowableOverlap = 10000, // About 3 frames @ 30fps
      nearestDistance = Infinity,
      dtsDistance,
      nearestGopObj,
      currentGop,
      currentGopObj,
      i,
      // splitVideoConfig passes a private per-configuration snapshot so a
      // cached GOP is only matched against its own SPS/PPS; the legacy path
      // keeps using the shared track object
      fusionTrack = trackOverride || track;

    // Search for the GOP nearest to the beginning of this nal unit
    for (i = 0; i < this.gopCache_.length; i++) {
      currentGopObj = this.gopCache_[i];
      currentGop = currentGopObj.gop;

      // Reject Gops with different SPS or PPS
      if (!(fusionTrack.pps && arrayEquals(fusionTrack.pps[0], currentGopObj.pps[0])) ||
          !(fusionTrack.sps && arrayEquals(fusionTrack.sps[0], currentGopObj.sps[0]))) {
        continue;
      }

      // Reject Gops that would require a negative baseMediaDecodeTime
      if (currentGop.dts < fusionTrack.timelineStartInfo.dts) {
        continue;
      }

      // The distance between the end of the gop and the start of the nalUnit
      dtsDistance = (nalUnit.dts - currentGop.dts) - currentGop.duration;

      // Only consider GOPS that start before the nal unit and end within
      // a half-second of the nal unit
      if (dtsDistance >= -allowableOverlap &&
          dtsDistance <= halfSecond) {

        // Always use the closest GOP we found if there is more than
        // one candidate
        if (!nearestGopObj ||
            nearestDistance > dtsDistance) {
          nearestGopObj = currentGopObj;
          nearestDistance = dtsDistance;
        }
      }
    }

    if (nearestGopObj) {
      return nearestGopObj.gop;
    }
    return null;
  };

  // trim gop list to the first gop found that has a matching pts with a gop in the list
  // of gopsToAlignWith starting from the START of the list
  this.alignGopsAtStart_ = function(gops) {
    var alignIndex, gopIndex, align, gop, byteLength, nalCount, duration, alignedGops;

    byteLength = gops.byteLength;
    nalCount = gops.nalCount;
    duration = gops.duration;
    alignIndex = gopIndex = 0;

    while (alignIndex < gopsToAlignWith.length && gopIndex < gops.length) {
      align = gopsToAlignWith[alignIndex];
      gop = gops[gopIndex];

      if (align.pts === gop.pts) {
        break;
      }

      if (gop.pts > align.pts) {
        // this current gop starts after the current gop we want to align on, so increment
        // align index
        alignIndex++;
        continue;
      }

      // current gop starts before the current gop we want to align on. so increment gop
      // index
      gopIndex++;
      byteLength -= gop.byteLength;
      nalCount -= gop.nalCount;
      duration -= gop.duration;
    }

    if (gopIndex === 0) {
      // no gops to trim
      return gops;
    }

    if (gopIndex === gops.length) {
      // all gops trimmed, skip appending all gops
      return null;
    }

    alignedGops = gops.slice(gopIndex);
    alignedGops.byteLength = byteLength;
    alignedGops.duration = duration;
    alignedGops.nalCount = nalCount;
    alignedGops.pts = alignedGops[0].pts;
    alignedGops.dts = alignedGops[0].dts;

    return alignedGops;
  };

  // trim gop list to the first gop found that has a matching pts with a gop in the list
  // of gopsToAlignWith starting from the END of the list
  this.alignGopsAtEnd_ = function(gops) {
    var alignIndex, gopIndex, align, gop, alignEndIndex, matchFound;

    alignIndex = gopsToAlignWith.length - 1;
    gopIndex = gops.length - 1;
    alignEndIndex = null;
    matchFound = false;

    while (alignIndex >= 0 && gopIndex >= 0) {
      align = gopsToAlignWith[alignIndex];
      gop = gops[gopIndex];

      if (align.pts === gop.pts) {
        matchFound = true;
        break;
      }

      if (align.pts > gop.pts) {
        alignIndex--;
        continue;
      }

      if (alignIndex === gopsToAlignWith.length - 1) {
        // gop.pts is greater than the last alignment candidate. If no match is found
        // by the end of this loop, we still want to append gops that come after this
        // point
        alignEndIndex = gopIndex;
      }

      gopIndex--;
    }

    if (!matchFound && alignEndIndex === null) {
      return null;
    }

    var trimIndex;

    if (matchFound) {
      trimIndex = gopIndex;
    } else {
      trimIndex = alignEndIndex;
    }

    if (trimIndex === 0) {
      return gops;
    }

    var alignedGops = gops.slice(trimIndex);
    var metadata = alignedGops.reduce(function(total, gop) {
      total.byteLength += gop.byteLength;
      total.duration += gop.duration;
      total.nalCount += gop.nalCount;
      return total;
    }, { byteLength: 0, duration: 0, nalCount: 0 });

    alignedGops.byteLength = metadata.byteLength;
    alignedGops.duration = metadata.duration;
    alignedGops.nalCount = metadata.nalCount;
    alignedGops.pts = alignedGops[0].pts;
    alignedGops.dts = alignedGops[0].dts;

    return alignedGops;
  };

  this.alignGopsWith = function(newGopsToAlignWith) {
    gopsToAlignWith = newGopsToAlignWith;
  };
};

VideoSegmentStream.prototype = new Stream();

/**
 * A Stream that can combine multiple streams (ie. audio & video)
 * into a single output segment for MSE. Also supports audio-only
 * and video-only streams.
 * @param options {object} transmuxer options object
 * @param options.keepOriginalTimestamps {boolean} If true, keep the timestamps
 *        in the source; false to adjust the first segment to start at media timeline start.
 */
CoalesceStream = function(options, metadataStream) {
  // Number of Tracks per output segment
  // If greater than 1, we combine multiple
  // tracks into a single segment
  this.numberOfTracks = 0;
  this.metadataStream = metadataStream;

  options = options || {};

  if (typeof options.remux !== 'undefined') {
    this.remuxTracks = !!options.remux;
  } else {
    this.remuxTracks = true;
  }

  if (typeof options.keepOriginalTimestamps === 'boolean') {
    this.keepOriginalTimestamps = options.keepOriginalTimestamps;
  } else {
    this.keepOriginalTimestamps = false;
  }

  // In splitVideoConfig mode a single outer flush can arrive as several video
  // fragments, each with its own init segment. They are buffered here, in
  // input order, and re-emitted as separate data events on flush.
  this.splitVideoConfig = !!options.splitVideoConfig;
  this.pendingSplitVideo = [];
  this.splitRejected = false;
  this.splitRejectDone_ = false;

  this.pendingTracks = [];
  this.videoTrack = null;
  this.pendingBoxes = [];
  this.pendingCaptions = [];
  this.pendingMetadata = [];
  this.pendingBytes = 0;
  this.emittedTracks = 0;

  CoalesceStream.prototype.init.call(this);

  // Take output from multiple
  this.push = function(output) {
    // buffer incoming captions until the associated video segment
    // finishes
    if (output.content || output.text) {
      return this.pendingCaptions.push(output);
    }
    // buffer incoming id3 tags until the final flush
    if (output.frames) {
      return this.pendingMetadata.push(output);
    }

    // In splitVideoConfig mode every video fragment carries its own matching
    // initialization data and is emitted independently on flush.
    if (this.splitVideoConfig && output.track &&
        output.track.type === 'video') {
      return this.pendingSplitVideo.push(output);
    }

    // Add this track to the list of pending tracks and store
    // important information required for the construction of
    // the final segment
    this.pendingTracks.push(output.track);
    this.pendingBytes += output.boxes.byteLength;

    // TODO: is there an issue for this against chrome?
    // We unshift audio and push video because
    // as of Chrome 75 when switching from
    // one init segment to another if the video
    // mdat does not appear after the audio mdat
    // only audio will play for the duration of our transmux.
    if (output.track.type === 'video') {
      this.videoTrack = output.track;
      this.pendingBoxes.push(output.boxes);
    }
    if (output.track.type === 'audio') {
      this.audioTrack = output.track;
      this.pendingBoxes.unshift(output.boxes);
    }
  };
};

CoalesceStream.prototype = new Stream();
CoalesceStream.prototype.flush = function(flushSource) {
  var
    offset = 0,
    event = {
      captions: [],
      captionStreams: {},
      metadata: [],
      info: {}
    },
    caption,
    id3,
    initSegment,
    timelineStartPts = 0,
    i;

  // splitVideoConfig is video-only. The video segment stream delivers one
  // flush for the whole outer flush, possibly carrying several buffered
  // fragments: re-emit each as its own data event, in input order, then a
  // single done. An ineligible (audio-bearing) stream emits one done and
  // nothing else. Other flush sources fall through to the legacy behavior.
  if (this.splitVideoConfig && flushSource === 'VideoSegmentStream') {
    this.emitSplitVideo_();
    return;
  }

  if (this.splitVideoConfig && this.splitRejected) {
    if (!this.splitRejectDone_) {
      this.splitRejectDone_ = true;
      this.trigger('done');
    }
    return;
  }

  if (this.pendingTracks.length < this.numberOfTracks) {
    if (flushSource !== 'VideoSegmentStream' &&
        flushSource !== 'AudioSegmentStream') {
      // Return because we haven't received a flush from a data-generating
      // portion of the segment (meaning that we have only recieved meta-data
      // or captions.)
      return;
    } else if (this.remuxTracks) {
      // Return until we have enough tracks from the pipeline to remux (if we
      // are remuxing audio and video into a single MP4)
      return;
    } else if (this.pendingTracks.length === 0) {
      // In the case where we receive a flush without any data having been
      // received we consider it an emitted track for the purposes of coalescing
      // `done` events.
      // We do this for the case where there is an audio and video track in the
      // segment but no audio data. (seen in several playlists with alternate
      // audio tracks and no audio present in the main TS segments.)
      this.emittedTracks++;

      if (this.emittedTracks >= this.numberOfTracks) {
        this.trigger('done');
        this.emittedTracks = 0;
      }
      return;
    }
  }

  if (this.videoTrack) {
    timelineStartPts = this.videoTrack.timelineStartInfo.pts;
    VIDEO_PROPERTIES.forEach(function(prop) {
      event.info[prop] = this.videoTrack[prop];
    }, this);
  } else if (this.audioTrack) {
    timelineStartPts = this.audioTrack.timelineStartInfo.pts;
    AUDIO_PROPERTIES.forEach(function(prop) {
      event.info[prop] = this.audioTrack[prop];
    }, this);
  }

  if (this.videoTrack || this.audioTrack) {
    if (this.pendingTracks.length === 1) {
      event.type = this.pendingTracks[0].type;
    } else {
      event.type = 'combined';
    }

    this.emittedTracks += this.pendingTracks.length;

    initSegment = mp4.initSegment(this.pendingTracks);

    // Create a new typed array to hold the init segment
    event.initSegment = new Uint8Array(initSegment.byteLength);

    // Create an init segment containing a moov
    // and track definitions
    event.initSegment.set(initSegment);

    // Create a new typed array to hold the moof+mdats
    event.data = new Uint8Array(this.pendingBytes);

    // Append each moof+mdat (one per track) together
    for (i = 0; i < this.pendingBoxes.length; i++) {
      event.data.set(this.pendingBoxes[i], offset);
      offset += this.pendingBoxes[i].byteLength;
    }

    // Translate caption PTS times into second offsets to match the
    // video timeline for the segment, and add track info
    for (i = 0; i < this.pendingCaptions.length; i++) {
      caption = this.pendingCaptions[i];
      caption.startTime = clock.metadataTsToSeconds(
        caption.startPts, timelineStartPts, this.keepOriginalTimestamps);
      caption.endTime = clock.metadataTsToSeconds(
        caption.endPts, timelineStartPts, this.keepOriginalTimestamps);

      event.captionStreams[caption.stream] = true;
      event.captions.push(caption);
    }

    // Translate ID3 frame PTS times into second offsets to match the
    // video timeline for the segment
    for (i = 0; i < this.pendingMetadata.length; i++) {
      id3 = this.pendingMetadata[i];
      id3.cueTime = clock.metadataTsToSeconds(
        id3.pts, timelineStartPts, this.keepOriginalTimestamps);

      event.metadata.push(id3);
    }

    // We add this to every single emitted segment even though we only need
    // it for the first
    event.metadata.dispatchType = this.metadataStream.dispatchType;

    // Reset stream state
    this.pendingTracks.length = 0;
    this.videoTrack = null;
    this.pendingBoxes.length = 0;
    this.pendingCaptions.length = 0;
    this.pendingBytes = 0;
    this.pendingMetadata.length = 0;

    // Emit the built segment
    // We include captions and ID3 tags for backwards compatibility,
    // ideally we should send only video and audio in the data event
    this.trigger('data', event);
    // Emit each caption to the outside world
    // Ideally, this would happen immediately on parsing captions,
    // but we need to ensure that video data is sent back first
    // so that caption timing can be adjusted to match video timing
    for (i = 0; i < event.captions.length; i++) {
      caption = event.captions[i];

      this.trigger('caption', caption);
    }
    // Emit each id3 tag to the outside world
    // Ideally, this would happen immediately on parsing the tag,
    // but we need to ensure that video data is sent back first
    // so that ID3 frame timing can be adjusted to match video timing
    for (i = 0; i < event.metadata.length; i++) {
      id3 = event.metadata[i];

      this.trigger('id3Frame', id3);
    }
  }

  // Only emit `done` if all tracks have been flushed and emitted
  if (this.emittedTracks >= this.numberOfTracks) {
    this.trigger('done');
    this.emittedTracks = 0;
  }
};

CoalesceStream.prototype.setRemux = function(val) {
  this.remuxTracks = val;
};

/**
 * Emit every buffered splitVideoConfig video fragment, in order, as its own
 * data event with an init segment generated from that fragment's track
 * snapshot. Always concludes with a single done for the outer flush, even
 * when there were no fragments (an empty flush produces no data event).
 */
CoalesceStream.prototype.emitSplitVideo_ = function() {
  var
    i,
    output,
    track,
    initSegment,
    event;

  for (i = 0; i < this.pendingSplitVideo.length; i++) {
    output = this.pendingSplitVideo[i];
    track = output.track;
    initSegment = mp4.initSegment([track]);

    event = {
      type: 'video',
      info: {},
      captions: [],
      captionStreams: {},
      metadata: [],
      initSegment: new Uint8Array(initSegment.byteLength),
      data: output.boxes
    };
    event.initSegment.set(initSegment);

    VIDEO_PROPERTIES.forEach(function(prop) {
      event.info[prop] = track[prop];
    });

    this.trigger('data', event);
  }

  this.pendingSplitVideo.length = 0;
  this.trigger('done');
};

CoalesceStream.prototype.reset = function() {
  this.pendingSplitVideo.length = 0;
  this.splitRejected = false;
  this.splitRejectDone_ = false;
  this.pendingTracks.length = 0;
  this.videoTrack = null;
  this.audioTrack = null;
  this.pendingBoxes.length = 0;
  this.pendingCaptions.length = 0;
  this.pendingBytes = 0;
  this.pendingMetadata.length = 0;
  this.emittedTracks = 0;
  this.trigger('reset');
};
/**
 * A Stream that expects MP2T binary data as input and produces
 * corresponding media segments, suitable for use with Media Source
 * Extension (MSE) implementations that support the ISO BMFF byte
 * stream format, like Chrome.
 */
Transmuxer = function(options) {
  var
    self = this,
    hasFlushed = true,
    videoTrack,
    audioTrack;

  Transmuxer.prototype.init.call(this);

  options = options || {};
  this.baseMediaDecodeTime = options.baseMediaDecodeTime || 0;
  this.transmuxPipeline_ = {};

  // splitVideoConfig only supports a video-only, non-remuxed pipeline. An
  // audio track cannot be represented alongside a stream that may emit
  // several configurations, so require an explicit remux:false rather than
  // silently falling back to the remuxing default.
  if (options.splitVideoConfig && options.remux !== false) {
    throw new Error('splitVideoConfig is only supported with remux set to false');
  }

  this.setupAacPipeline = function() {
    var pipeline = {};
    this.transmuxPipeline_ = pipeline;

    pipeline.type = 'aac';
    pipeline.metadataStream = new m2ts.MetadataStream();

    // set up the parsing pipeline
    pipeline.aacStream = new AacStream();
    pipeline.audioTimestampRolloverStream = new m2ts.TimestampRolloverStream('audio');
    pipeline.timedMetadataTimestampRolloverStream = new m2ts.TimestampRolloverStream('timed-metadata');
    pipeline.adtsStream = new AdtsStream();
    pipeline.coalesceStream = new CoalesceStream(options, pipeline.metadataStream);
    pipeline.headOfPipeline = pipeline.aacStream;

    pipeline.aacStream
      .pipe(pipeline.audioTimestampRolloverStream)
      .pipe(pipeline.adtsStream);
    pipeline.aacStream
      .pipe(pipeline.timedMetadataTimestampRolloverStream)
      .pipe(pipeline.metadataStream)
      .pipe(pipeline.coalesceStream);

    pipeline.metadataStream.on('timestamp', function(frame) {
      pipeline.aacStream.setTimestamp(frame.timeStamp);
    });

    pipeline.aacStream.on('data', function(data) {
      if ((data.type !== 'timed-metadata' && data.type !== 'audio') || pipeline.audioSegmentStream) {
        return;
      }

      audioTrack = audioTrack || {
        timelineStartInfo: {
          baseMediaDecodeTime: self.baseMediaDecodeTime
        },
        codec: 'adts',
        type: 'audio'
      };
      // hook up the audio segment stream to the first track with aac data
      pipeline.coalesceStream.numberOfTracks++;
      pipeline.audioSegmentStream = new AudioSegmentStream(audioTrack, options);

      pipeline.audioSegmentStream.on('log', self.getLogTrigger_('audioSegmentStream'));

      pipeline.audioSegmentStream.on('timingInfo',
        self.trigger.bind(self, 'audioTimingInfo'));

      // Set up the final part of the audio pipeline
      pipeline.adtsStream
        .pipe(pipeline.audioSegmentStream)
        .pipe(pipeline.coalesceStream);

      // emit pmt info
      self.trigger('trackinfo', {
        hasAudio: !!audioTrack,
        hasVideo: !!videoTrack
      });
    });

    // Re-emit any data coming from the coalesce stream to the outside world
    pipeline.coalesceStream.on('data', this.trigger.bind(this, 'data'));
    // Let the consumer know we have finished flushing the entire pipeline
    pipeline.coalesceStream.on('done', this.trigger.bind(this, 'done'));

    addPipelineLogRetriggers(this, pipeline);
  };

  this.setupTsPipeline = function() {
    var pipeline = {};
    this.transmuxPipeline_ = pipeline;

    pipeline.type = 'ts';
    pipeline.metadataStream = new m2ts.MetadataStream();

    // set up the parsing pipeline
    pipeline.packetStream = new m2ts.TransportPacketStream();
    pipeline.parseStream = new m2ts.TransportParseStream();
    pipeline.elementaryStream = new m2ts.ElementaryStream();
    pipeline.timestampRolloverStream = new m2ts.TimestampRolloverStream();
    pipeline.adtsStream = new AdtsStream();
    pipeline.h264Stream = new H264Stream();
    pipeline.captionStream = new m2ts.CaptionStream(options);
    pipeline.coalesceStream = new CoalesceStream(options, pipeline.metadataStream);
    pipeline.headOfPipeline = pipeline.packetStream;

    // disassemble MPEG2-TS packets into elementary streams
    pipeline.packetStream
      .pipe(pipeline.parseStream)
      .pipe(pipeline.elementaryStream)
      .pipe(pipeline.timestampRolloverStream);

    // !!THIS ORDER IS IMPORTANT!!
    // demux the streams
    pipeline.timestampRolloverStream
      .pipe(pipeline.h264Stream);

    pipeline.timestampRolloverStream
      .pipe(pipeline.adtsStream);

    pipeline.timestampRolloverStream
      .pipe(pipeline.metadataStream)
      .pipe(pipeline.coalesceStream);

    // Hook up CEA-608/708 caption stream
    pipeline.h264Stream.pipe(pipeline.captionStream)
      .pipe(pipeline.coalesceStream);

    pipeline.elementaryStream.on('data', function(data) {
      var i;

      if (data.type === 'metadata') {
        i = data.tracks.length;

        // scan the tracks listed in the metadata
        while (i--) {
          if (!videoTrack && data.tracks[i].type === 'video') {
            videoTrack = data.tracks[i];
            videoTrack.timelineStartInfo.baseMediaDecodeTime = self.baseMediaDecodeTime;
          } else if (!audioTrack && data.tracks[i].type === 'audio') {
            audioTrack = data.tracks[i];
            audioTrack.timelineStartInfo.baseMediaDecodeTime = self.baseMediaDecodeTime;
          }
        }

        // splitVideoConfig is video-only; refuse to run rather than silently
        // discarding an audio track. This happens before any data is emitted.
        if (options.splitVideoConfig && audioTrack &&
            !pipeline.splitConfigAudioRejected) {
          pipeline.splitConfigAudioRejected = true;
          pipeline.coalesceStream.splitRejected = true;
          self.trigger(
            'error',
            new Error('splitVideoConfig is only supported for video-only streams, ' +
              'an audio track was signaled'));
        }

        // hook up the video segment stream to the first track with h264 data.
        // When an audio track makes splitVideoConfig ineligible the video
        // stream is not started either: the combination is rejected above and
        // no media fragment is emitted rather than dropping the audio.
        if (videoTrack && !pipeline.videoSegmentStream &&
            !(options.splitVideoConfig && audioTrack)) {
          pipeline.coalesceStream.numberOfTracks++;
          pipeline.videoSegmentStream = new VideoSegmentStream(videoTrack, options);
          pipeline.videoSegmentStream.on('log', self.getLogTrigger_('videoSegmentStream'));

          // surface configuration validation errors to Transmuxer consumers
          pipeline.videoSegmentStream.on('error', self.trigger.bind(self, 'error'));

          pipeline.videoSegmentStream.on('timelineStartInfo', function(timelineStartInfo) {
            // When video emits timelineStartInfo data after a flush, we forward that
            // info to the AudioSegmentStream, if it exists, because video timeline
            // data takes precedence.  Do not do this if keepOriginalTimestamps is set,
            // because this is a particularly subtle form of timestamp alteration.
            // splitVideoConfig is video-only and never creates an AudioSegmentStream.
            if (audioTrack && pipeline.audioSegmentStream &&
                !options.keepOriginalTimestamps) {
              audioTrack.timelineStartInfo = timelineStartInfo;
              // On the first segment we trim AAC frames that exist before the
              // very earliest DTS we have seen in video because Chrome will
              // interpret any video track with a baseMediaDecodeTime that is
              // non-zero as a gap.
              pipeline.audioSegmentStream.setEarliestDts(timelineStartInfo.dts - self.baseMediaDecodeTime);
            }
          });

          pipeline.videoSegmentStream.on('processedGopsInfo',
            self.trigger.bind(self, 'gopInfo'));
          pipeline.videoSegmentStream.on('segmentTimingInfo',
            self.trigger.bind(self, 'videoSegmentTimingInfo'));

          pipeline.videoSegmentStream.on('baseMediaDecodeTime', function(baseMediaDecodeTime) {
            if (audioTrack && pipeline.audioSegmentStream) {
              pipeline.audioSegmentStream.setVideoBaseMediaDecodeTime(baseMediaDecodeTime);
            }
          });

          pipeline.videoSegmentStream.on('timingInfo',
            self.trigger.bind(self, 'videoTimingInfo'));

          // Set up the final part of the video pipeline
          pipeline.h264Stream
            .pipe(pipeline.videoSegmentStream)
            .pipe(pipeline.coalesceStream);
        }

        if (audioTrack && !pipeline.audioSegmentStream &&
            !options.splitVideoConfig) {
          // hook up the audio segment stream to the first track with aac data
          pipeline.coalesceStream.numberOfTracks++;
          pipeline.audioSegmentStream = new AudioSegmentStream(audioTrack, options);
          pipeline.audioSegmentStream.on('log', self.getLogTrigger_('audioSegmentStream'));

          pipeline.audioSegmentStream.on('timingInfo',
            self.trigger.bind(self, 'audioTimingInfo'));
          pipeline.audioSegmentStream.on('segmentTimingInfo',
            self.trigger.bind(self, 'audioSegmentTimingInfo'));

          // Set up the final part of the audio pipeline
          pipeline.adtsStream
            .pipe(pipeline.audioSegmentStream)
            .pipe(pipeline.coalesceStream);
        }

        // emit pmt info
        self.trigger('trackinfo', {
          hasAudio: !!audioTrack,
          hasVideo: !!videoTrack
        });
      }
    });

    // Re-emit any data coming from the coalesce stream to the outside world
    pipeline.coalesceStream.on('data', this.trigger.bind(this, 'data'));
    pipeline.coalesceStream.on('id3Frame', function(id3Frame) {
      id3Frame.dispatchType = pipeline.metadataStream.dispatchType;

      self.trigger('id3Frame', id3Frame);
    });
    pipeline.coalesceStream.on('caption', this.trigger.bind(this, 'caption'));
    // Let the consumer know we have finished flushing the entire pipeline
    pipeline.coalesceStream.on('done', this.trigger.bind(this, 'done'));

    addPipelineLogRetriggers(this, pipeline);
  };

  // hook up the segment streams once track metadata is delivered
  this.setBaseMediaDecodeTime = function(baseMediaDecodeTime) {
    var pipeline = this.transmuxPipeline_;

    if (!options.keepOriginalTimestamps) {
      this.baseMediaDecodeTime = baseMediaDecodeTime;
    }

    if (audioTrack) {
      audioTrack.timelineStartInfo.dts = undefined;
      audioTrack.timelineStartInfo.pts = undefined;
      trackDecodeInfo.clearDtsInfo(audioTrack);
      if (pipeline.audioTimestampRolloverStream) {
        pipeline.audioTimestampRolloverStream.discontinuity();
      }
    }
    if (videoTrack) {
      if (pipeline.videoSegmentStream) {
        pipeline.videoSegmentStream.gopCache_ = [];
      }
      videoTrack.timelineStartInfo.dts = undefined;
      videoTrack.timelineStartInfo.pts = undefined;
      trackDecodeInfo.clearDtsInfo(videoTrack);
      pipeline.captionStream.reset();
    }

    if (pipeline.timestampRolloverStream) {
      pipeline.timestampRolloverStream.discontinuity();
    }
  };

  this.setAudioAppendStart = function(timestamp) {
    if (audioTrack) {
      this.transmuxPipeline_.audioSegmentStream.setAudioAppendStart(timestamp);
    }
  };

  this.setRemux = function(val) {
    var pipeline = this.transmuxPipeline_;

    if (options.splitVideoConfig && val !== false) {
      throw new Error('splitVideoConfig is only supported with remux set to false');
    }

    options.remux = val;

    if (pipeline && pipeline.coalesceStream) {
      pipeline.coalesceStream.setRemux(val);
    }
  };

  this.alignGopsWith = function(gopsToAlignWith) {
    if (videoTrack && this.transmuxPipeline_.videoSegmentStream) {
      this.transmuxPipeline_.videoSegmentStream.alignGopsWith(gopsToAlignWith);
    }
  };

  this.getLogTrigger_ = function(key) {
    var self = this;

    return function(event) {
      event.stream = key;
      self.trigger('log', event);
    };
  };
  // feed incoming data to the front of the parsing pipeline
  this.push = function(data) {
    if (hasFlushed) {
      var isAac = isLikelyAacData(data);

      if (isAac && this.transmuxPipeline_.type !== 'aac') {
        this.setupAacPipeline();
      } else if (!isAac && this.transmuxPipeline_.type !== 'ts') {
        this.setupTsPipeline();
      }

      hasFlushed = false;
    }
    this.transmuxPipeline_.headOfPipeline.push(data);
  };

  // flush any buffered data
  this.flush = function() {
    hasFlushed = true;
    // Start at the top of the pipeline and flush all pending work
    this.transmuxPipeline_.headOfPipeline.flush();
  };

  this.endTimeline = function() {
    this.transmuxPipeline_.headOfPipeline.endTimeline();
  };

  this.reset = function() {
    if (this.transmuxPipeline_.headOfPipeline) {
      this.transmuxPipeline_.headOfPipeline.reset();
    }
  };

  // Caption data has to be reset when seeking outside buffered range
  this.resetCaptions = function() {
    if (this.transmuxPipeline_.captionStream) {
      this.transmuxPipeline_.captionStream.reset();
    }
  };

};
Transmuxer.prototype = new Stream();

module.exports = {
  Transmuxer: Transmuxer,
  VideoSegmentStream: VideoSegmentStream,
  AudioSegmentStream: AudioSegmentStream,
  AUDIO_PROPERTIES: AUDIO_PROPERTIES,
  VIDEO_PROPERTIES: VIDEO_PROPERTIES,
  // exported for testing
  generateSegmentTimingInfo: generateSegmentTimingInfo
};
