/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 * Portions Copyright (C) Philipp Kewisch */

import Time from "./time.js";
import Timezone from "./timezone.js";
import Recur from "./recur.js";

/**
 * Indexed expansion for a restricted slice of RFC 5545 recurrence rules:
 * Gregorian UTC DATE-TIME values in the years 1800 through 9999, a monthly
 * frequency with a positive safe-integer INTERVAL and only the BYMONTHDAY,
 * BYDAY, BYHOUR, BYMINUTE, BYSECOND and BYSETPOS parts. Everything outside
 * that slice is rejected by the constructor instead of silently falling
 * back to the item-at-a-time iterator.
 *
 * Cost model. A 400-year Gregorian cycle holds exactly 4 800 months and only
 * 14 distinct month shapes (four lengths times seven starting weekdays). The
 * engine precomputes, once, the selected day numbers of every one of those
 * 4 800 months and the number of instances each contributes. Active months
 * repeat with a stride period of P = 4800/gcd(INTERVAL,4800) <= 4800 steps,
 * so a cumulative count along one stride period plus one period total
 * answers any "how many instances lie in this span" question with a
 * constant number of table lookups, no matter how many years or skipped
 * instances the span covers. A seek from 1800 to 9000 therefore touches the
 * precomputed cycle during construction and at most one stride period of
 * boundary months afterwards, never the months in between. Dense sub-day
 * parts stay one ordered time-of-day list (at most 86 400 entries) and
 * combine with the day list through product-index arithmetic; the
 * Cartesian product is never materialized, and BYSETPOS resolves to at most
 * 732 product indices (two times 366) per month. Every structure is bounded
 * by the 400-year cycle and the legal part ranges, so memory grows neither
 * with the distance from DTSTART, the number of skipped instances nor the
 * raw candidate count of a month.
 *
 * @module ICAL.monthly-index
 * @private
 */

export const MIN_YEAR = 1800;
export const MAX_YEAR = 9999;

const JAN_1_1800_DOW = 4; // Time weekday numbering (1 == Sunday): 1800-01-01 was a Wednesday
const CYCLE_MONTHS = 4800;

/**
 * Assert that a rule part value is an integer inside its allowed range.
 *
 * @param {String} name     Name of the BY part, for the error message
 * @param {Number} value    The value to check
 * @param {Number} min      Inclusive minimum
 * @param {Number} max      Inclusive maximum
 * @param {Boolean} nonZero When true, zero is rejected
 */
function checkInt(name, value, min, max, nonZero) {
  if (typeof value !== "number" || !Number.isInteger(value)) {
    throw new Error(name + ": integer value expected, got " + String(value));
  }
  if (value < min || value > max || (nonZero && value === 0)) {
    throw new Error(name + ': invalid value "' + value + '"');
  }
}

/**
 * Normalize a BY part that may be given as a scalar or an array.
 *
 * @param {Object} parts    The rule's parts object
 * @param {String} name     Name of the BY part
 * @param {Boolean} asString Keep string values as-is (for BYDAY)
 * @return {Array|null}     The part values, or null when absent
 */
function partList(parts, name, asString) {
  if (!(name in parts)) {
    return null;
  }
  let raw = parts[name];
  let values = Array.isArray(raw) ? raw : [raw];
  return asString ? values.map(String) : values.map(Number);
}

/**
 * All distinct times of day the sub-day parts name, sorted by second of day
 * and deduplicated. Missing parts inherit the DTSTART value.
 *
 * @param {Object} parts    The rule's parts
 * @param {Time} dtstart    The event start
 * @return {Number[]}       Sorted seconds-of-day
 */
function buildTimesOfDay(parts, dtstart) {
  let hours = partList(parts, "BYHOUR") || [dtstart.hour];
  let minutes = partList(parts, "BYMINUTE") || [dtstart.minute];
  let seconds = partList(parts, "BYSECOND") || [dtstart.second];

  for (let hour of hours) {
    checkInt("BYHOUR", hour, 0, 23, false);
  }
  for (let minute of minutes) {
    checkInt("BYMINUTE", minute, 0, 59, false);
  }
  for (let second of seconds) {
    checkInt("BYSECOND", second, 0, 59, false);
  }

  let seen = new Set();
  for (let hour of hours) {
    for (let minute of minutes) {
      for (let second of seconds) {
        seen.add(hour * 3600 + minute * 60 + second);
      }
    }
  }
  return [...seen].sort((a, b) => a - b);
}

/**
 * Parse one BYDAY entry into an ordinal (0 means every weekday) and a Time
 * weekday number.
 *
 * @param {String} entry       The raw BYDAY value
 * @return {[Number, Number]}  The ordinal and weekday number
 */
function parseByDay(entry) {
  let match = String(entry).match(/^([+-]?[1-5])?(SU|MO|TU|WE|TH|FR|SA)$/);
  if (!match) {
    throw new Error('invalid BYDAY value "' + entry + '"');
  }
  let ordinal = match[1] ? parseInt(match[1], 10) : 0;
  let weekday = {
    SU: Time.SUNDAY,
    MO: Time.MONDAY,
    TU: Time.TUESDAY,
    WE: Time.WEDNESDAY,
    TH: Time.THURSDAY,
    FR: Time.FRIDAY,
    SA: Time.SATURDAY
  }[match[2]];
  return [ordinal, weekday];
}

/**
 * The day numbers a single BYDAY term selects inside a month of the given
 * shape. An ordinal of zero selects every matching weekday, a signed ordinal
 * the nth occurrence from the respective end.
 *
 * @param {Number} ordinal      0 for every weekday, else +/-1..+/-5
 * @param {Number} weekday      Time weekday number
 * @param {Number} length       Days in the month
 * @param {Number} firstDow     Weekday number of the month's first day
 * @return {Number[]}           Selected 1-based day numbers
 */
function byDayDays(ordinal, weekday, length, firstDow) {
  let firstMatch = ((weekday - firstDow + 7) % 7) + 1;
  let days = [];

  if (ordinal === 0) {
    for (let day = firstMatch; day <= length; day += 7) {
      days.push(day);
    }
    return days;
  }

  if (ordinal > 0) {
    let day = firstMatch + (ordinal - 1) * 7;
    if (day <= length) {
      days.push(day);
    }
    return days;
  }

  let lastMatch = firstMatch + Math.floor((length - firstMatch) / 7) * 7;
  let day = lastMatch + (ordinal + 1) * 7;
  if (day >= 1) {
    days.push(day);
  }
  return days;
}

/**
 * Fold a sorted day list into the union accumulator, keeping the result
 * sorted and duplicate-free.
 *
 * @param {Number[]} union   The accumulated sorted unique days
 * @param {Number[]} days    New sorted days to merge in
 */
function mergeDays(union, days) {
  for (let day of days) {
    if (union[union.length - 1] !== day) {
      union.push(day);
    }
  }
}

/**
 * Whole days from the proleptic Gregorian epoch (year 0, January 1) to
 * January 1 of the given year.
 *
 * @param {Number} year    A Gregorian year
 * @return {Number}        Days before the year
 */
function daysBeforeYear(year) {
  let y = year - 1;
  return 365 * y + Math.floor(y / 4) - Math.floor(y / 100) + Math.floor(y / 400);
}

/**
 * The packed shape key (month length times ten plus first weekday) of an
 * absolute month numbered as year * 12 + (month - 1).
 *
 * @param {Number} absoluteMonth   The absolute month number
 * @return {Number}                The packed month-shape key
 */
function shapeKeyForMonth(absoluteMonth) {
  let year = Math.floor(absoluteMonth / 12);
  let month = (absoluteMonth % 12) + 1;
  let days = daysBeforeYear(year) - daysBeforeYear(MIN_YEAR) +
    Time.daysInYearPassedMonth[Time.isLeapYear(year) ? 1 : 0][month - 1];
  let firstDow = ((JAN_1_1800_DOW - 1 + days) % 7) + 1;
  return Time.daysInMonth(month, year) * 10 + firstDow;
}

/**
 * Resolve the selected day numbers of one month shape from the raw date
 * parts. The two date conditions take their intersection; each condition
 * already deduplicates internally, so an instant matched twice counts once.
 *
 * @param {Number[]|null} rawByMonthDay  Raw BYMONTHDAY values
 * @param {Array<[Number, Number]>|null} rawByDay  Parsed BYDAY terms
 * @param {Number} length                 Days in the month
 * @param {Number} firstDow               Weekday of the first day
 * @return {Number[]}                     Sorted unique selected day numbers
 */
function selectDays(rawByMonthDay, rawByDay, length, firstDow) {
  let monthDays = null;
  if (rawByMonthDay) {
    monthDays = [];
    for (let rule of rawByMonthDay) {
      let day = rule > 0 ? rule : length + rule + 1;
      if (day >= 1 && day <= length) {
        monthDays.push(day);
      }
    }
    monthDays.sort((a, b) => a - b);
    let unique = [];
    for (let day of monthDays) {
      if (unique[unique.length - 1] !== day) {
        unique.push(day);
      }
    }
    monthDays = unique;
  }

  if (!rawByDay) {
    return monthDays || [];
  }

  let weekDays = [];
  for (let [ordinal, weekday] of rawByDay) {
    mergeDays(weekDays, byDayDays(ordinal, weekday, length, firstDow));
  }
  weekDays.sort((a, b) => a - b);
  let weekUnique = [];
  for (let day of weekDays) {
    if (weekUnique[weekUnique.length - 1] !== day) {
      weekUnique.push(day);
    }
  }
  weekDays = weekUnique;

  if (!monthDays) {
    return weekDays;
  }

  let selected = [];
  for (let day of weekDays) {
    if (monthDays.includes(day)) {
      selected.push(day);
    }
  }
  return selected;
}

/**
 * The first index whose value is at least the target (lower bound).
 *
 * @param {Number[]} sorted    A sorted numeric array
 * @param {Number} target      The target value
 * @return {Number}            Insertion index in [0, sorted.length]
 */
function lowerBound(sorted, target) {
  let low = 0;
  let high = sorted.length;
  while (low < high) {
    let mid = (low + high) >> 1;
    if (sorted[mid] < target) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }
  return low;
}

/**
 * Validate an external UTC DATE-TIME argument against the supported range.
 *
 * @param {Time} time       The time to validate
 * @param {String} label    What the time is, for error messages
 * @return {Time}           The same time, unchanged
 */
export function checkUtcDateTime(time, label) {
  if (!(time instanceof Time) || time.isDate ||
      time.zone !== Timezone.utcTimezone) {
    throw new Error(label + " must be a UTC DATE-TIME ICAL.Time");
  }
  if (time.year < MIN_YEAR || time.year > MAX_YEAR) {
    throw new Error(
      label + " year must be between " + MIN_YEAR + " and " + MAX_YEAR
    );
  }
  return time;
}

/**
 * Number of distinct month entries the BYSETPOS positions name for a month
 * holding total ordered occurrences.
 *
 * @param {Number[]} positions   The signed BYSETPOS values
 * @param {Number} total         Size of the month's ordered occurrence set
 * @return {Number}              Number of distinct valid positions
 */
function countSetPositions(positions, total) {
  let indices = new Set();
  for (let position of positions) {
    let index = position > 0 ? position - 1 : total + position;
    if (index >= 0 && index < total) {
      indices.add(index);
    }
  }
  return indices.size;
}

/**
 * Greatest common divisor.
 *
 * @param {Number} a    A positive integer
 * @param {Number} b    A positive integer
 * @return {Number}     The gcd
 */
function gcd(a, b) {
  while (b) {
    [a, b] = [b, a % b];
  }
  return a;
}

/**
 * Reduce a rule and its start time to the bounded specification the indexed
 * engine works with.
 *
 * @param {Recur} rule       The recurrence rule
 * @param {Time} dtstart     The event start
 * @return {Object}          The normalized engine specification
 */
function buildMonthlyIndexSpec(rule, dtstart) {
  if (!(rule instanceof Recur)) {
    throw new Error("indexed iterator requires an ICAL.Recur rule");
  }
  checkUtcDateTime(dtstart, "dtstart");

  if (rule.freq !== "MONTHLY") {
    throw new Error("indexed iterator only supports FREQ=MONTHLY, got " + rule.freq);
  }

  let interval = rule.interval;
  if (typeof interval !== "number" || !Number.isSafeInteger(interval) || interval < 1) {
    throw new Error("indexed iterator requires a positive safe integer INTERVAL");
  }

  let supported = new Set([
    "BYMONTHDAY", "BYDAY", "BYHOUR", "BYMINUTE", "BYSECOND", "BYSETPOS"
  ]);
  for (let part of Object.keys(rule.parts)) {
    if (!supported.has(part)) {
      throw new Error("indexed iterator does not support the " + part + " part");
    }
  }
  if (Object.keys(rule).some(key => key.toUpperCase() === "RSCALE")) {
    throw new Error("indexed iterator does not support RSCALE");
  }

  if (rule.count != null && rule.until) {
    throw new Error("indexed iterator does not support COUNT and UNTIL together");
  }

  let count = null;
  if (rule.count != null) {
    checkInt("COUNT", rule.count, 1, Number.MAX_SAFE_INTEGER, false);
    count = rule.count;
  }

  let until = null;
  if (rule.until) {
    until = checkUtcDateTime(rule.until, "UNTIL").clone();
  }

  let rawByMonthDay = partList(rule.parts, "BYMONTHDAY");
  if (rawByMonthDay) {
    for (let value of rawByMonthDay) {
      checkInt("BYMONTHDAY", value, -31, 31, true);
    }
  }

  let rawByDay = null;
  if ("BYDAY" in rule.parts) {
    rawByDay = partList(rule.parts, "BYDAY", true).map(parseByDay);
    for (let [ordinal] of rawByDay) {
      if (ordinal !== 0) {
        checkInt("BYDAY ordinal", ordinal, -5, 5, true);
      }
    }
  }

  let setPositions = partList(rule.parts, "BYSETPOS");
  if (setPositions) {
    for (let position of setPositions) {
      checkInt("BYSETPOS", position, -366, 366, true);
    }
  }

  let timesOfDay = buildTimesOfDay(rule.parts, dtstart);
  let hasDateParts = !!(rawByMonthDay || rawByDay);
  let anchorDay = hasDateParts ? null : dtstart.day;
  let anchorMonth = dtstart.year * 12 + (dtstart.month - 1);

  // The bounded month table: one sorted day list for every month of one
  // 400-year Gregorian cycle, plus each month's selected instance count.
  let cycleDays = new Array(CYCLE_MONTHS);
  let selectedCount = new Uint32Array(CYCLE_MONTHS);
  let perDay = timesOfDay.length;

  for (let i = 0; i < CYCLE_MONTHS; i++) {
    let absoluteMonth = MIN_YEAR * 12 + i;
    let shape = shapeKeyForMonth(absoluteMonth);
    let length = Math.floor(shape / 10);
    let firstDow = shape % 10;
    let days;

    if (anchorDay) {
      // No date selection part: the DTSTART day number is the rule's day,
      // and months that do not contain it contribute nothing.
      days = anchorDay <= length ? [anchorDay] : [];
    } else {
      days = selectDays(rawByMonthDay, rawByDay, length, firstDow);
    }

    cycleDays[i] = days;
    let total = days.length * perDay;
    selectedCount[i] = setPositions
      ? countSetPositions(setPositions, total)
      : total;
  }

  // The active-month stride repeats every periodSteps strides, and its
  // residues inside the cycle are every g-th month from the anchor residue.
  let g = gcd(interval, CYCLE_MONTHS);
  let periodSteps = CYCLE_MONTHS / g;

  // Cumulative full-month selected counts along one stride period plus the
  // period total: span counts then take constant time.
  let strideCum = new Array(periodSteps + 1);
  strideCum[0] = 0;
  let neverProduces = true;
  for (let k = 0; k < periodSteps; k++) {
    let absoluteMonth = anchorMonth + k * interval;
    let residue = (((absoluteMonth - MIN_YEAR * 12) % CYCLE_MONTHS) +
      CYCLE_MONTHS) % CYCLE_MONTHS;
    let amount = selectedCount[residue];
    if (amount > 0) {
      neverProduces = false;
    }
    strideCum[k + 1] = strideCum[k] + amount;
  }

  return {
    dtstart: dtstart.clone(),
    interval,
    count,
    until,
    timesOfDay,
    setPositions,
    rawByMonthDay,
    rawByDay,
    anchorDay,
    anchorMonth,
    cycleDays,
    selectedCount,
    perDay,
    periodSteps,
    strideCum,
    periodTotal: strideCum[periodSteps],
    neverProduces
  };
}

/**
 * The selected day numbers of any month, reusing the cycle table inside the
 * 400-year window and deriving only boundary months outside it directly.
 *
 * @param {Object} spec          The engine specification
 * @param {Number} absoluteMonth The absolute month number
 * @return {Number[]}            Sorted selected day numbers
 */
function daysForMonth(spec, absoluteMonth) {
  let offset = absoluteMonth - MIN_YEAR * 12;
  if (offset >= 0 && offset < CYCLE_MONTHS) {
    return spec.cycleDays[offset];
  }
  let shape = shapeKeyForMonth(absoluteMonth);
  let length = Math.floor(shape / 10);
  if (spec.anchorDay) {
    return spec.anchorDay <= length ? [spec.anchorDay] : [];
  }
  return selectDays(
    spec.rawByMonthDay,
    spec.rawByDay,
    length,
    shape % 10
  );
}

/**
 * The product indices a month's BYSETPOS names, sorted and deduplicated.
 *
 * @param {Object} spec    The engine specification
 * @param {Number} total   Size of the month's ordered occurrence set
 * @return {Number[]|null} Sorted selected product indices, or null for all
 */
function selectedProductIndices(spec, total) {
  if (!spec.setPositions) {
    return null;
  }
  let indices = new Set();
  for (let position of spec.setPositions) {
    let index = position > 0 ? position - 1 : total + position;
    if (index >= 0 && index < total) {
      indices.add(index);
    }
  }
  return [...indices].sort((a, b) => a - b);
}

/**
 * Build the cursor describing the deliverable occurrences of one active
 * month. The cursor stores product indices rather than expanded values, so
 * its size stays bounded even for a month holding millions of raw
 * candidates.
 *
 * @param {Object} spec     The engine specification
 * @param {Number} step     Zero-based stride step of the month
 * @return {Object|null}    The cursor, or null when the month is out of range
 */
function buildCursor(spec, step) {
  let month = spec.anchorMonth + step * spec.interval;
  let year = Math.floor(month / 12);
  if (year > MAX_YEAR) {
    return null;
  }

  let days = daysForMonth(spec, month);
  let total = days.length * spec.perDay;
  let lo = 0;
  let hi = total - 1;

  if (step === 0) {
    // The anchor month is trimmed at DTSTART, which the rule numbers from.
    // Only the anchor day itself is trimmed by time of day; later selected
    // days of the same month keep every time of day.
    let dayIdx = lowerBound(days, spec.dtstart.day);
    if (dayIdx === days.length) {
      hi = -1;
    } else {
      lo = dayIdx * spec.perDay;
      if (days[dayIdx] === spec.dtstart.day) {
        let todIdx = lowerBound(
          spec.timesOfDay,
          spec.dtstart.hour * 3600 + spec.dtstart.minute * 60 + spec.dtstart.second
        );
        lo += todIdx;
      }
      if (lo > hi) {
        hi = -1;
      }
    }
  }

  if (spec.until && hi >= 0) {
    let untilMonth = spec.until.year * 12 + (spec.until.month - 1);
    if (month > untilMonth) {
      return null;
    }
    if (month === untilMonth) {
      let dayIdx = lowerBound(days, spec.until.day);
      if (dayIdx === days.length || days[dayIdx] > spec.until.day) {
        // UNTIL names a day after the last selected day: keep that whole day.
        dayIdx -= 1;
        if (dayIdx < 0) {
          hi = -1;
        } else {
          hi = dayIdx * spec.perDay + spec.perDay - 1;
        }
      } else {
        // UNTIL is inclusive, so the time-of-day equal to it still counts.
        let todIdx = lowerBound(
          spec.timesOfDay,
          spec.until.hour * 3600 + spec.until.minute * 60 + spec.until.second
        );
        if (todIdx === spec.perDay ||
            spec.timesOfDay[todIdx] >
              spec.until.hour * 3600 + spec.until.minute * 60 + spec.until.second) {
          todIdx -= 1;
        }
        if (todIdx < 0) {
          // No time of day of the UNTIL day qualifies: keep the preceding
          // selected day instead of dropping the whole month.
          hi = dayIdx > 0
            ? (dayIdx - 1) * spec.perDay + spec.perDay - 1
            : -1;
        } else {
          hi = dayIdx * spec.perDay + todIdx;
        }
      }
    }
  }

  let picks = null;
  if (hi >= 0) {
    picks = selectedProductIndices(spec, total);
    if (picks) {
      picks = picks.slice(lowerBound(picks, lo), lowerBound(picks, hi + 1));
    }
  }

  return { month, step, days, lo, hi, picks, pos: 0 };
}

/**
 * Number of occurrences still available in a cursor.
 *
 * @param {Object} cursor    A month cursor
 * @return {Number}          Remaining entries including the current position
 */
function cursorRemaining(cursor) {
  if (!cursor || cursor.hi < cursor.lo) {
    return 0;
  }
  if (cursor.picks) {
    return cursor.picks.length - cursor.pos;
  }
  return cursor.hi - cursor.lo + 1 - cursor.pos;
}

/**
 * The product index under the cursor at its current position.
 *
 * @param {Object} cursor    A month cursor
 * @return {Number}          The selected product index
 */
function cursorIndex(cursor) {
  return cursor.picks ? cursor.picks[cursor.pos] : cursor.lo + cursor.pos;
}

/**
 * Rebuild a UTC Time from a cursor and a product index.
 *
 * @param {Object} spec       The engine specification
 * @param {Object} cursor     The month cursor
 * @param {Number} productIdx The product index
 * @return {Time}             A fresh UTC DATE-TIME
 */
function timeAt(spec, cursor, productIdx) {
  let dayIdx = Math.floor(productIdx / spec.perDay);
  let todIdx = productIdx % spec.perDay;
  let secondOfDay = spec.timesOfDay[todIdx];
  let year = Math.floor(cursor.month / 12);
  let month = (cursor.month % 12) + 1;

  return new Time({
    year,
    month,
    day: cursor.days[dayIdx],
    hour: Math.floor(secondOfDay / 3600),
    minute: Math.floor((secondOfDay % 3600) / 60),
    second: secondOfDay % 60,
    isDate: false,
    zone: Timezone.utcTimezone
  });
}

/**
 * The first product index of a month at or after a day/second-of-day target.
 *
 * @param {Object} spec      The engine specification
 * @param {Object} cursor    A month cursor
 * @param {Number} day       Target day of month
 * @param {Number} second    Target second of day
 * @return {Number}          A product index in [0, month total]
 */
function productIndexAt(spec, cursor, day, second) {
  let dayIdx = lowerBound(cursor.days, day);
  if (dayIdx === cursor.days.length) {
    return cursor.days.length * spec.perDay;
  }
  let todIdx = lowerBound(spec.timesOfDay, second);
  return dayIdx * spec.perDay + todIdx;
}

/**
 * Position a cursor at its first deliverable product index at or past a
 * product index, returning how many selected entries were discarded.
 *
 * @param {Object} cursor       A month cursor
 * @param {Number} productIdx   The lower bound product index
 * @return {Number}             Entries skipped inside the cursor
 */
function seekCursor(cursor, productIdx) {
  if (cursor.picks) {
    // Seeking never moves backward: clamp to the current read position.
    let at = Math.max(cursor.pos, lowerBound(cursor.picks, productIdx));
    let skipped = Math.max(0, at - cursor.pos);
    cursor.pos = at;
    return skipped;
  }
  let next = Math.max(cursor.lo + cursor.pos, productIdx);
  let skipped = Math.max(0, next - (cursor.lo + cursor.pos));
  cursor.pos = next - cursor.lo;
  return skipped;
}

/**
 * Sum the full-month selected counts of stride steps [from, to). The stride
 * pattern repeats every periodSteps, so whole periods cost one multiply and
 * the residual costs two cumulative lookups.
 *
 * @param {Object} spec    The engine specification
 * @param {Number} from    First stride step of the span
 * @param {Number} to      One past the last stride step of the span
 * @return {Number}        Number of occurrences the span holds in total
 */
function countSteps(spec, from, to) {
  if (to <= from) {
    return 0;
  }
  let span = to - from;
  let period = spec.periodSteps;
  let fullPeriods = Math.floor(span / period);
  let r1 = from % period;
  let tail = span % period;
  let r2 = r1 + tail;
  let total = fullPeriods * spec.periodTotal;
  if (r2 <= period) {
    total += spec.strideCum[r2] - spec.strideCum[r1];
  } else {
    total += spec.periodTotal - spec.strideCum[r1] +
      spec.strideCum[r2 - period];
  }
  return total;
}

/**
 * Indexed counterpart of {@link RecurIterator} for the supported monthly
 * rule slice. Instances are created through RecurIterator with the indexed
 * option and expose the same next/last/completed surface plus seek.
 *
 * @memberof ICAL
 */
class IndexedRecurIterator {
  /**
   * @param {Object} options       Iterator options
   * @param {Recur} options.rule   The recurrence rule
   * @param {Time} options.dtstart The event start
   */
  constructor(options) {
    if (options.indexedState) {
      this._resume(options.indexedState);
      return;
    }

    this.rule = options.rule;
    this.spec = buildMonthlyIndexSpec(options.rule, options.dtstart);
    this.step = 0;
    this.consumed = 0;
    this.completed = false;
    this.last = null;
    this.cursor = null;
    this.head = null;
    // Production-path work counters: how many month cursors were built and
    // how many span-count lookups were performed. Both stay bounded by one
    // stride period regardless of the distance jumped.
    this.cursorBuilds = 0;
    this.spanCountCalls = 0;

    if (this.spec.until && this.spec.until.compare(this.spec.dtstart) < 0) {
      this.completed = true;
      return;
    }

    if (this.spec.neverProduces) {
      this.completed = true;
    } else {
      this._loadNextCursor();
      this._refreshHead();
    }
  }

  /**
   * The start date of the event.
   * @type {Time}
   */
  get dtstart() {
    return this.spec.dtstart;
  }

  /**
   * Global number of occurrences delivered, counted from DTSTART.
   * @type {Number}
   */
  get occurrence_number() {
    return this.consumed;
  }

  /**
   * Advance step until a cursor with deliverable entries is found or the
   * rule is exhausted. Empty active months do not consume COUNT.
   *
   * @return {Boolean} Whether a usable cursor was loaded
   * @private
   */
  _loadNextCursor() {
    let guard = 0;
    while (guard++ <= this.spec.periodSteps) {
      let cursor = buildCursor(this.spec, this.step);
      this.cursorBuilds++;
      if (cursor && cursorRemaining(cursor) > 0) {
        this.cursor = cursor;
        return true;
      }
      this.step++;
    }
    this.completed = true;
    this.cursor = null;
    return false;
  }

  /**
   * Set the head to the Time under the current cursor.
   *
   * @private
   */
  _refreshHead() {
    this.head = timeAt(this.spec, this.cursor, cursorIndex(this.cursor));
  }

  /**
   * Deliver the current head and move past it, honoring the global COUNT.
   *
   * @return {Time|null} The delivered Time, or null when exhausted
   * @private
   */
  _takeHead() {
    let result = this.head;
    this.last = result;
    this.consumed++;
    this.cursor.pos++;

    if (this.spec.count != null && this.consumed >= this.spec.count) {
      this.completed = true;
      this.cursor = null;
      this.head = null;
      return result;
    }

    if (cursorRemaining(this.cursor) === 0) {
      this.step++;
      if (!this._loadNextCursor()) {
        this.head = null;
        return result;
      }
    }
    this._refreshHead();
    return result;
  }

  /**
   * Return the next occurrence, or null once the rule is exhausted.
   *
   * @return {Time|null} A fresh UTC DATE-TIME, or null
   */
  next() {
    if (this.completed || !this.head) {
      this.completed = true;
      return null;
    }
    return this._takeHead();
  }

  /**
   * Discard every undelivered occurrence before the bound, then return and
   * consume the first one at or past it. Seek only moves forward; a
   * finished iterator stays finished, and a bound before the next pending
   * occurrence delivers that pending occurrence.
   *
   * @param {Time} lowerBound    A UTC DATE-TIME in the supported year range
   * @return {Time|null}         The first occurrence at or past the bound
   */
  seek(lowerBound) {
    if (!this.positionToBound(lowerBound)) {
      return null;
    }
    return this._takeHead();
  }

  /**
   * Move the read-ahead head to the first occurrence at or past the bound
   * without consuming it. Every discarded occurrence is still charged to the
   * global COUNT. Used by the expansion merger, which has to coordinate the
   * heads of several rule iterators before delivering anything.
   *
   * @param {Time} lowerBound    A UTC DATE-TIME in the supported year range
   * @return {Boolean}           Whether a head at or past the bound exists
   */
  positionToBound(lowerBound) {
    checkUtcDateTime(lowerBound, "seek bound");
    if (this.completed || !this.head) {
      return false;
    }

    let boundMonth = lowerBound.year * 12 + (lowerBound.month - 1);
    let boundSecond = lowerBound.hour * 3600 +
      lowerBound.minute * 60 + lowerBound.second;

    // The cursor only holds occurrences not yet delivered, so a bound in an
    // earlier month leaves the pending head in place.
    if (boundMonth < this.cursor.month) {
      return true;
    }

    if (boundMonth === this.cursor.month) {
      this.consumed += seekCursor(
        this.cursor,
        productIndexAt(this.spec, this.cursor, lowerBound.day, boundSecond)
      );
      if (this._countReached()) {
        return false;
      }
      if (cursorRemaining(this.cursor) > 0) {
        this._refreshHead();
        return true;
      }
      this.step++;
      return this._loadNextCursor() && ((this._refreshHead()), true);
    }

    // Drop the rest of the buffered month, then jump whole stride steps
    // without visiting them.
    this.consumed += cursorRemaining(this.cursor);
    if (this._countReached()) {
      return false;
    }

    let targetStep = this.step + Math.max(
      1,
      Math.ceil(
        (boundMonth - this.cursor.month) / this.spec.interval
      )
    );

    if (this._pastHorizon(targetStep)) {
      this._finish();
      return false;
    }

    this.consumed += countSteps(this.spec, this.step + 1, targetStep);
    this.spanCountCalls++;
    if (this._countReached()) {
      return false;
    }

    this.step = targetStep;
    return this._positionFromStep(boundMonth, lowerBound.day, boundSecond);
  }

  /**
   * Mark the iterator exhausted and clear its read-ahead state.
   *
   * @private
   */
  _finish() {
    this.completed = true;
    this.cursor = null;
    this.head = null;
  }

  /**
   * Whether a stride step lies past the UNTIL month or the year 9999.
   *
   * @param {Number} step    The stride step to check
   * @return {Boolean}       True when nothing at or past the step can occur
   * @private
   */
  _pastHorizon(step) {
    let month = this.spec.anchorMonth + step * this.spec.interval;
    if (Math.floor(month / 12) > MAX_YEAR) {
      return true;
    }
    if (this.spec.until) {
      let untilMonth = this.spec.until.year * 12 + (this.spec.until.month - 1);
      if (month > untilMonth) {
        return true;
      }
    }
    return false;
  }

  /**
   * Whether the global COUNT has been consumed.
   *
   * @return {Boolean}
   * @private
   */
  _countReached() {
    if (this.spec.count != null && this.consumed >= this.spec.count) {
      this.completed = true;
      this.cursor = null;
      this.head = null;
      return true;
    }
    return false;
  }

  /**
   * Walk from the given stride step to the first occurrence at or past the
   * bound, materializing at most one stride period of month cursors.
   *
   * @param {Number} boundMonth    Absolute month number of the bound
   * @param {Number} boundDay      Bound day of month
   * @param {Number} boundSecond   Bound second of day
   * @return {Boolean}             Whether a head at or past the bound exists
   * @private
   */
  _positionFromStep(boundMonth, boundDay, boundSecond) {
    let guard = 0;
    while (guard++ <= this.spec.periodSteps) {
      if (this._pastHorizon(this.step)) {
        break;
      }

      let cursor = buildCursor(this.spec, this.step);
      this.cursorBuilds++;
      if (cursor && cursorRemaining(cursor) > 0) {
        this.cursor = cursor;
        if (cursor.month === boundMonth) {
          this.consumed += seekCursor(
            cursor,
            productIndexAt(this.spec, cursor, boundDay, boundSecond)
          );
          if (this._countReached()) {
            return false;
          }
          if (cursorRemaining(cursor) === 0) {
            this.step++;
            continue;
          }
        }
        this._refreshHead();
        return true;
      }
      this.step++;
    }

    this._finish();
    return false;
  }

  /**
   * Serialize the indexed state. Resuming never replays from DTSTART.
   *
   * @return {Object}
   */
  toJSON() {
    return {
      indexed: true,
      initialized: true,
      rule: this.rule.toJSON(),
      dtstart: this.spec.dtstart.toJSON(),
      step: this.step,
      consumed: this.consumed,
      completed: this.completed,
      last: this.last ? this.last.toJSON() : null,
      cursor: this.cursor ? {
        month: this.cursor.month,
        pos: this.cursor.pos,
        lo: this.cursor.lo,
        hi: this.cursor.hi,
        picks: this.cursor.picks
      } : null
    };
  }

  /**
   * Restore indexed state from a previous toJSON result without replaying.
   *
   * @param {Object} state    The serialized state
   * @private
   */
  _resume(state) {
    let rule = new Recur(state.rule);
    this.rule = rule;
    this.spec = buildMonthlyIndexSpec(rule, new Time(state.dtstart));
    this.step = state.step;
    this.consumed = state.consumed;
    this.completed = state.completed;
    this.last = state.last ? new Time(state.last) : null;
    this.cursorBuilds = 0;
    this.spanCountCalls = 0;

    if (this.completed) {
      this.cursor = null;
      this.head = null;
      return;
    }

    this.cursor = buildCursor(this.spec, this.step);
    this.cursorBuilds++;
    this.cursor.pos = state.cursor.pos;
    this.head = timeAt(this.spec, this.cursor, cursorIndex(this.cursor));
  }
}

export {
  IndexedRecurIterator
};
