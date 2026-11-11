import errors from '../errors';

/**
 * Calculates the R (repetition) value for a live stream (for the final segment
 * in a manifest where the r value is negative 1)
 *
 * @param {Object} attributes
 *        Object containing all inherited attributes from parent elements with attribute
 *        names as keys
 * @param {number} time
 *        current time (typically the total time up until the final segment)
 * @param {number} duration
 *        duration property for the given <S />
 *
 * @return {number}
 *        R value to reach the end of the given period
 */
const getLiveRValue = (attributes, time, duration) => {
  const {
    NOW,
    clientOffset,
    availabilityStartTime,
    timescale = 1,
    periodStart = 0,
    minimumUpdatePeriod = 0
  } = attributes;
  const now = (NOW + clientOffset) / 1000;
  const periodStartWC = availabilityStartTime + periodStart;
  const periodEndWC = now + minimumUpdatePeriod;
  const periodDuration = periodEndWC - periodStartWC;

  return Math.ceil(((periodDuration * timescale) - time) / duration);
};

/**
 * Throws when a value used to expand a SegmentTimeline cannot be represented as
 * a safe integer. Keeps the expansion loop free of precision loss and endless
 * loops.
 *
 * @param {number} value
 *        Value to validate
 * @param {string} [errorCode]
 *        Error code to throw with when the value is not a safe integer
 */
const assertSafeInteger = (value, errorCode = errors.SEGMENT_TIME_UNSAFE_INTEGER) => {
  if (!Number.isSafeInteger(value)) {
    throw new Error(errorCode);
  }
};

/**
 * Determines the end of the current Period on the media clock, in @timescale
 * units, for a static MPD.
 *
 * The Period end (presentation clock) is inherited from the parsed Period
 * information: it comes from the Period's own @duration, the next Period's
 * @start, or mediaPresentationDuration for the last Period. It is converted to
 * the Representation's media clock, since S@t and the $Time$ template
 * identifier share the media clock rather than the presentation clock:
 *
 *   mediaEnd = presentationTimeOffset + (periodEnd - periodStart) * timescale
 *
 * @param {Object} attributes
 *        Object containing all inherited attributes from parent elements with attribute
 *        names as keys
 *
 * @return {number}
 *         Media time of the Period end, in @timescale units
 */
const getStaticPeriodMediaEnd = (attributes) => {
  const {
    sourceDuration,
    periodStart = 0,
    periodEnd,
    timescale = 1,
    presentationTimeOffset = 0
  } = attributes;

  let mediaEnd;

  if (typeof periodEnd === 'number') {
    // The Period end is resolved during attribute inheritance and is on the
    // presentation clock; shift it onto the Representation's media clock.
    mediaEnd = presentationTimeOffset + ((periodEnd - periodStart) * timescale);
  } else if (sourceDuration > 0) {
    // Callers that only know the whole Media Presentation duration keep the
    // historical media-clock interpretation.
    mediaEnd = sourceDuration * timescale;
  } else {
    // A trailing r="-1" with no derivable Period end cannot be expanded.
    throw new Error(errors.PERIOD_END_UNSPECIFIED);
  }

  return mediaEnd;
};

/**
 * Uses information provided by SegmentTemplate.SegmentTimeline to determine segment
 * timing and duration
 *
 * @param {Object} attributes
 *        Object containing all inherited attributes from parent elements with attribute
 *        names as keys
 * @param {Object[]} segmentTimeline
 *        List of objects representing the attributes of each S element contained within
 *
 * @return {{number: number, duration: number, time: number, timeline: number}[]}
 *         List of Objects with segment timing and duration info
 */
export const parseByTimeline = (attributes, segmentTimeline) => {
  const {
    type,
    minimumUpdatePeriod = 0,
    media = '',
    sourceDuration,
    timescale = 1,
    startNumber = 1,
    periodStart: timeline
  } = attributes;
  const segments = [];
  let time = -1;

  if (timescale <= 0) {
    throw new Error(errors.TIMESCALE_INVALID);
  }

  for (let sIndex = 0; sIndex < segmentTimeline.length; sIndex++) {
    const S = segmentTimeline[sIndex];
    const duration = S.d;
    const repeat = S.r || 0;
    const segmentTime = S.t || 0;

    // S@d is mandatory on every S element and must be positive. A non-positive
    // value would stall the repeat loop or make it run forever.
    if (!(duration > 0)) {
      throw new Error(errors.SEGMENT_DURATION_INVALID);
    }

    assertSafeInteger(duration);
    assertSafeInteger(segmentTime);
    assertSafeInteger(repeat);

    if (time < 0) {
      // first segment
      time = segmentTime;
    }

    if (segmentTime && segmentTime > time) {
      // discontinuity

      // TODO: How to handle this type of discontinuity
      // timeline++ here would treat it like HLS discontuity and content would
      // get appended without gap
      // E.G.
      //  <S t="0" d="1" />
      //  <S d="1" />
      //  <S d="1" />
      //  <S t="5" d="1" />
      // would have $Time$ values of [0, 1, 2, 5]
      // should this be appened at time positions [0, 1, 2, 3],(#EXT-X-DISCONTINUITY)
      // or [0, 1, 2, gap, gap, 5]? (#EXT-X-GAP)
      // does the value of sourceDuration consider this when calculating arbitrary
      // negative @r repeat value?
      // E.G. Same elements as above with this added at the end
      //  <S d="1" r="-1" />
      //  with a sourceDuration of 10
      // Would the 2 gaps be included in the time duration calculations resulting in
      // 8 segments with $Time$ values of [0, 1, 2, 5, 6, 7, 8, 9] or 10 segments
      // with $Time$ values of [0, 1, 2, 5, 6, 7, 8, 9, 10, 11] ?

      time = segmentTime;
    }

    let count;

    if (repeat < 0) {
      const nextS = sIndex + 1;

      if (nextS === segmentTimeline.length) {
        // last segment
        if (type === 'dynamic' &&
            minimumUpdatePeriod > 0 &&
            media.indexOf('$Number$') > 0) {
          count = getLiveRValue(attributes, time, duration);
        } else if (type === 'static') {
          // A trailing r="-1" repeats the S up to the end of the current
          // Period, expressed on the same media clock as S@t.
          const periodMediaEnd = getStaticPeriodMediaEnd(attributes);

          if (!Number.isFinite(periodMediaEnd)) {
            throw new Error(errors.SEGMENT_TIME_UNSAFE_INTEGER);
          }

          // Only segments whose start is strictly before the Period end are
          // emitted. ceil keeps a final start that does not evenly divide the
          // remaining duration, while an exact fit does not emit a segment at
          // the end boundary itself.
          count = Math.ceil((periodMediaEnd - time) / duration);
        } else {
          // Dynamic manifests without the live availability window keep their
          // previous sourceDuration based behavior.
          // TODO: This may be incorrect depending on conclusion of TODO above
          count = ((sourceDuration * timescale) - time) / duration;
        }
      } else {
        // Another S with an explicit @t terminates this repeat group.
        count = (segmentTimeline[nextS].t - time) / duration;
      }
    } else {
      count = repeat + 1;
    }

    if (!Number.isFinite(count) ||
        !Number.isSafeInteger(Math.ceil(count))) {
      // A NaN or oversized repeat would either produce no segments or loop
      // effectively forever; reject the manifest instead of returning a
      // partial list.
      throw new Error(errors.SEGMENT_TIME_UNSAFE_INTEGER);
    }

    if (count < 0) {
      // a repeat group starting after its own end simply produces no segments
      count = 0;
    }

    const end = startNumber + segments.length + count;
    let number = startNumber + segments.length;

    while (number < end) {
      assertSafeInteger(time);
      segments.push({ number, duration: duration / timescale, time, timeline });
      time += duration;
      number++;
    }
  }

  return segments;
};
