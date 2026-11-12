/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// Contract tests for the optional indexed monthly expansion. The small-range
// expectations come from an independent day-by-day calendar model written in
// this file, not from the production engine; the legacy iterator is only
// consulted for single-instance-per-month rules whose semantics are
// unambiguous. Large spans use explicit periodic counting instead of replay.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import ICAL from '../lib/ical/module.js';

const { Recur, RecurIterator, RecurExpansion, Time, Component, parse } = ICAL;
const T = value => Time.fromString(value);
const UTC = { isDate: false, timezone: 'UTC' };
const utcTime = (year, month, day, hour = 0, minute = 0, second = 0) =>
  new Time({ year, month, day, hour, minute, second, ...UTC });

// ---------------------------------------------------------------------------
// Independent calendar model (proleptic Gregorian, no production helpers)
// ---------------------------------------------------------------------------

// Monday-based weekday index 0..6, matching modelWeekday() below.
const WEEKDAYS = { MO: 0, TU: 1, WE: 2, TH: 3, FR: 4, SA: 5, SU: 6 };

function modelLeap(year) {
  return (year % 4 === 0 && year % 100 !== 0) || year % 400 === 0;
}

function modelDaysInMonth(year, month) {
  if (month === 2) {
    return modelLeap(year) ? 29 : 28;
  }
  return [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31][month - 1];
}

// Monday-based weekday index 0..6, via the standard day-count formula.
function modelWeekday(year, month, day) {
  let y = year;
  let m = month;
  if (m < 3) {
    m += 12;
    y -= 1;
  }
  let h = (day + Math.floor(13 * (m + 1) / 5) + y + Math.floor(y / 4) -
    Math.floor(y / 100) + Math.floor(y / 400)) % 7;
  // Zeller h: 0=Saturday .. 6=Friday; remap to Monday=0..Sunday=6.
  return (h + 5) % 7;
}

function asList(value) {
  return value === undefined || value === null ? null : [].concat(value);
}

function parseByDayEntry(entry) {
  let match = String(entry).match(/^([+-]?[1-5])?(SU|MO|TU|WE|TH|FR|SA)$/);
  assert.ok(match, 'test model: malformed BYDAY ' + entry);
  return [match[1] ? parseInt(match[1], 10) : 0, WEEKDAYS[match[2]]];
}

function modelMonthDays(rule, year, month, anchorDay) {
  let length = modelDaysInMonth(year, month);
  let parts = rule.parts;
  let monthDays = null;
  let bmd = asList(parts.BYMONTHDAY);
  if (bmd) {
    monthDays = [...new Set(bmd.map(ruleDay =>
      ruleDay > 0 ? ruleDay : length + ruleDay + 1
    ).filter(day => day >= 1 && day <= length))].sort((a, b) => a - b);
  }

  let byDay = asList(parts.BYDAY);
  if (!byDay) {
    if (monthDays) {
      return monthDays;
    }
    return anchorDay <= length ? [anchorDay] : [];
  }

  let weekDays = [];
  for (let entry of byDay) {
    let [ordinal, weekday] = parseByDayEntry(entry);
    let firstWeekday = modelWeekday(year, month, 1);
    let firstMatch = ((weekday - firstWeekday + 7) % 7) + 1;
    if (ordinal === 0) {
      for (let day = firstMatch; day <= length; day += 7) {
        weekDays.push(day);
      }
    } else if (ordinal > 0) {
      let day = firstMatch + (ordinal - 1) * 7;
      if (day <= length) {
        weekDays.push(day);
      }
    } else {
      let lastMatch = firstMatch +
        Math.floor((length - firstMatch) / 7) * 7;
      let day = lastMatch + (ordinal + 1) * 7;
      if (day >= 1) {
        weekDays.push(day);
      }
    }
  }
  weekDays = [...new Set(weekDays)].sort((a, b) => a - b);

  if (!monthDays) {
    return weekDays;
  }
  return weekDays.filter(day => monthDays.includes(day));
}

function modelTimesOfDay(rule, start) {
  let parts = rule.parts;
  let hours = asList(parts.BYHOUR) || [start.hour];
  let minutes = asList(parts.BYMINUTE) || [start.minute];
  let seconds = asList(parts.BYSECOND) || [start.second];
  let values = new Set();
  for (let hour of hours) {
    for (let minute of minutes) {
      for (let second of seconds) {
        values.add(hour * 3600 + minute * 60 + second);
      }
    }
  }
  return [...values].sort((a, b) => a - b);
}

function stamp(year, month, day, secondOfDay) {
  let pad = n => String(n).padStart(2, '0');
  return `${year}-${pad(month)}-${pad(day)}T${pad(secondOfDay / 3600 | 0)}:` +
    `${pad(secondOfDay % 3600 / 60 | 0)}:${pad(secondOfDay % 60)}Z`;
}

// The full ordered, deduplicated, trimmed and bounded occurrence sequence.
function modelSequence(ruleText, startText, maxYear) {
  let rule = Recur.fromString(ruleText);
  let start = T(startText);
  let interval = rule.interval || 1;
  let anchorMonth = start.year * 12 + (start.month - 1);
  let timesOfDay = modelTimesOfDay(rule, start);
  let hasDateParts = !!(rule.parts.BYMONTHDAY || rule.parts.BYDAY);
  let anchorDay = hasDateParts ? null : start.day;
  let setPositions = asList(rule.parts.BYSETPOS);
  let output = [];

  for (let year = start.year; year <= maxYear; year++) {
    for (let month = 1; month <= 12; month++) {
      let absoluteMonth = year * 12 + (month - 1);
      if ((absoluteMonth - anchorMonth) % interval !== 0 ||
          absoluteMonth < anchorMonth) {
        continue;
      }

      let days = modelMonthDays(rule, year, month, start.day);
      let occurrences = [];
      for (let day of days) {
        for (let secondOfDay of timesOfDay) {
          occurrences.push({ day, secondOfDay });
        }
      }
      occurrences.sort((a, b) =>
        (a.day - b.day) || (a.secondOfDay - b.secondOfDay));
      occurrences = occurrences.filter((item, index) =>
        index === 0 ||
        item.day !== occurrences[index - 1].day ||
        item.secondOfDay !== occurrences[index - 1].secondOfDay);

      let chosen = occurrences;
      if (setPositions) {
        let indices = new Set();
        for (let position of setPositions) {
          let index = position > 0 ? position - 1 : occurrences.length + position;
          if (index >= 0 && index < occurrences.length) {
            indices.add(index);
          }
        }
        chosen = [...indices].sort((a, b) => a - b)
          .map(index => occurrences[index]);
      }

      for (let item of chosen) {
        let value = utcTime(year, month, item.day,
          item.secondOfDay / 3600 | 0,
          item.secondOfDay % 3600 / 60 | 0,
          item.secondOfDay % 60);
        if (value.compare(start) < 0) {
          continue;
        }
        if (rule.until && value.compare(rule.until) > 0) {
          continue;
        }
        output.push(value.toString());
      }
    }
  }

  if (rule.count != null) {
    output = output.slice(0, rule.count);
  }
  return output;
}

function drainIndexed(ruleText, startText, cap) {
  let iterator = Recur.fromString(ruleText)
    .iterator(T(startText), { indexed: true });
  let output = [];
  let value;
  while ((value = iterator.next())) {
    output.push(value.toString());
    if (output.length >= cap) {
      break;
    }
  }
  return output;
}

function component(lines) {
  return new Component(parse(lines.join('\r\n')));
}

// ---------------------------------------------------------------------------
// Group 1: calendar expansion against the independent day-by-day model
// ---------------------------------------------------------------------------

test('indexed-calendar', () => {
  let rules = [
    'FREQ=MONTHLY;BYMONTHDAY=1,15',
    'FREQ=MONTHLY;BYMONTHDAY=-1',
    'FREQ=MONTHLY;BYMONTHDAY=31',
    'FREQ=MONTHLY;BYMONTHDAY=29',
    'FREQ=MONTHLY;BYMONTHDAY=-1,1',
    'FREQ=MONTHLY;BYMONTHDAY=15,-15',
    'FREQ=MONTHLY;BYDAY=MO',
    'FREQ=MONTHLY;BYDAY=MO,TU,WE,TH,FR',
    'FREQ=MONTHLY;BYDAY=SA,SU',
    'FREQ=MONTHLY;BYDAY=1MO,-1MO',
    'FREQ=MONTHLY;BYDAY=5MO',
    'FREQ=MONTHLY;BYDAY=-5FR',
    'FREQ=MONTHLY;BYDAY=MO;BYMONTHDAY=13',
    'FREQ=MONTHLY;BYDAY=TH;BYMONTHDAY=31',
    'FREQ=MONTHLY;BYDAY=MO,WE,FR;BYMONTHDAY=1,2,3,10,15,20,31',
    'FREQ=MONTHLY;BYDAY=SA,SU;BYMONTHDAY=-7,-1,1',
    'FREQ=MONTHLY;BYMONTHDAY=1;BYHOUR=9;BYMINUTE=30',
    'FREQ=MONTHLY;BYDAY=FR;BYHOUR=0,12;BYMINUTE=0,30;BYSECOND=0,30',
    'FREQ=MONTHLY;BYDAY=MO,TU,WE,TH,FR;BYSETPOS=1,-1',
    'FREQ=MONTHLY;BYDAY=MO;BYSETPOS=-2',
    'FREQ=MONTHLY;BYMONTHDAY=1,15;BYSETPOS=2',
    'FREQ=MONTHLY;BYSETPOS=1',
    'FREQ=MONTHLY;BYSETPOS=-1',
    'FREQ=MONTHLY;INTERVAL=2;BYMONTHDAY=15',
    'FREQ=MONTHLY;INTERVAL=3;BYDAY=FR',
    'FREQ=MONTHLY;INTERVAL=13;BYDAY=MO',
    'FREQ=MONTHLY;BYMONTHDAY=1,15;UNTIL=20020615T090000Z',
    'FREQ=MONTHLY;BYDAY=FR;BYHOUR=0,12;UNTIL=20010101T000000Z',
    'FREQ=MONTHLY;BYMONTHDAY=-1;COUNT=11',
    'FREQ=MONTHLY;BYDAY=MO,TU,WE,TH,FR;BYSETPOS=1;COUNT=13'
  ];
  let starts = [
    '2000-01-01T09:00:00Z',
    '2003-03-15T12:30:45Z',
    '1999-12-31T23:59:59Z',
    '2020-02-29T08:00:00Z'
  ];

  for (let ruleText of rules) {
    for (let startText of starts) {
      let rule = Recur.fromString(ruleText);
      let maxYear = rule.count != null ? 2012 : 2006;
      // The independent model only covers up to maxYear, so a start date past
      // that window cannot be compared here (it is exercised elsewhere).
      if (T(startText).year > maxYear) {
        continue;
      }
      let expected = modelSequence(ruleText, startText, maxYear);
      let actual = drainIndexed(ruleText, startText, expected.length);
      assert.deepEqual(actual, expected, `rule ${ruleText} start ${startText}`);
    }
  }

  // Explicit calendar landmarks: month ends, leap and non-leap centuries.
  let monthEnds = Recur.fromString('FREQ=MONTHLY;BYMONTHDAY=-1')
    .iterator(T('1899-11-30T00:00:00Z'), { indexed: true });
  let landmarks = [];
  for (let i = 0; i < 6; i++) {
    landmarks.push(monthEnds.next().toString());
  }
  assert.deepEqual(landmarks, [
    '1899-11-30T00:00:00Z', '1899-12-31T00:00:00Z',
    '1900-01-31T00:00:00Z', '1900-02-28T00:00:00Z',
    '1900-03-31T00:00:00Z', '1900-04-30T00:00:00Z'
  ]);

  let leapDay = Recur.fromString('FREQ=MONTHLY;BYMONTHDAY=29')
    .iterator(T('1999-01-29T00:00:00Z'), { indexed: true });
  let leapResults = [];
  for (let i = 0; i < 5; i++) {
    leapResults.push(leapDay.next().toString());
  }
  assert.deepEqual(leapResults, [
    '1999-01-29T00:00:00Z', '1999-03-29T00:00:00Z',
    '1999-04-29T00:00:00Z', '1999-05-29T00:00:00Z',
    '1999-06-29T00:00:00Z'
  ]);
  let skippedFebruary = leapResults.every(value => !value.startsWith('1999-02'));
  assert.ok(skippedFebruary, 'February 1999 has no 29th');

  let leapCentury = Recur.fromString('FREQ=MONTHLY;BYMONTHDAY=29')
    .iterator(T('2000-01-29T00:00:00Z'), { indexed: true });
  assert.equal(leapCentury.next().toString(), '2000-01-29T00:00:00Z');
  assert.equal(leapCentury.next().toString(), '2000-02-29T00:00:00Z');

  let nonLeapCentury = Recur.fromString('FREQ=MONTHLY;BYMONTHDAY=29')
    .iterator(T('2100-01-29T00:00:00Z'), { indexed: true });
  assert.equal(nonLeapCentury.next().toString(), '2100-01-29T00:00:00Z');
  assert.equal(nonLeapCentury.next().toString(), '2100-03-29T00:00:00Z');

  // First-month trimming with a mid-month start and BYSETPOS positions.
  let trimmed = Recur.fromString('FREQ=MONTHLY;BYDAY=MO,TU,WE,TH,FR;BYSETPOS=1,-1')
    .iterator(T('2003-03-15T12:30:45Z'), { indexed: true });
  let trimmedExpected = modelSequence(
    'FREQ=MONTHLY;BYDAY=MO,TU,WE,TH,FR;BYSETPOS=1,-1',
    '2003-03-15T12:30:45Z', 2003
  );
  assert.deepEqual(trimmedExpected, drainIndexed(
    'FREQ=MONTHLY;BYDAY=MO,TU,WE,TH,FR;BYSETPOS=1,-1',
    '2003-03-15T12:30:45Z', trimmedExpected.length
  ));
  assert.equal(trimmed.next().toString(), '2003-03-31T12:30:45Z');

  // WKST is accepted and does not change monthly weekday results.
  let withWkst = drainIndexed('FREQ=MONTHLY;BYDAY=MO;WKST=SU',
    '2000-01-01T09:00:00Z', 30);
  let withoutWkst = drainIndexed('FREQ=MONTHLY;BYDAY=MO',
    '2000-01-01T09:00:00Z', 30);
  assert.deepEqual(withWkst, withoutWkst);

  // Cross-check only unambiguous single-instance-per-month rules against the
  // legacy iterator.
  let singleInstanceRules = [
    'FREQ=MONTHLY',
    'FREQ=MONTHLY;BYMONTHDAY=15',
    'FREQ=MONTHLY;BYMONTHDAY=-1',
    'FREQ=MONTHLY;BYDAY=1MO',
    'FREQ=MONTHLY;INTERVAL=3'
  ];
  for (let ruleText of singleInstanceRules) {
    for (let startText of ['2000-01-01T09:00:00Z', '2003-03-15T12:30:45Z']) {
      let legacy = Recur.fromString(ruleText).iterator(T(startText));
      let indexed = Recur.fromString(ruleText)
        .iterator(T(startText), { indexed: true });
      for (let i = 0; i < 80; i++) {
        let expected = legacy.next();
        let actual = indexed.next();
        if (expected === null) {
          assert.equal(actual, null, `${ruleText} ended together`);
          break;
        }
        assert.equal(actual.toString(), expected.toString(),
          `${ruleText} ${startText} #${i}`);
      }
    }
  }
});

// ---------------------------------------------------------------------------
// Group 2: global COUNT numbering, UNTIL and distance jumping
// ---------------------------------------------------------------------------

test('counted-jump', () => {
  // The task's headline example: COUNT is exhausted before the window.
  let counted = Recur.fromString('FREQ=MONTHLY;BYMONTHDAY=1,15;COUNT=5')
    .iterator(T('2000-01-01T09:00:00Z'), { indexed: true });
  assert.equal(counted.seek(T('2000-03-02T00:00:00Z')), null);
  assert.equal(counted.completed, true);
  assert.equal(counted.next(), null);

  // COUNT numbers every global instance from DTSTART, including skipped ones.
  let five = Recur.fromString('FREQ=MONTHLY;BYMONTHDAY=1,15;COUNT=5')
    .iterator(T('2000-01-01T09:00:00Z'), { indexed: true });
  assert.equal(five.seek(T('2000-02-01T00:00:00Z')).toString(),
    '2000-02-01T09:00:00Z');
  assert.equal(five.occurrence_number, 3);
  assert.equal(five.next().toString(), '2000-02-15T09:00:00Z');
  assert.equal(five.occurrence_number, 4);
  assert.equal(five.next().toString(), '2000-03-01T09:00:00Z');
  assert.equal(five.occurrence_number, 5);
  assert.equal(five.next(), null);

  // A seek past the COUNT horizon consumes the rest and ends the iterator.
  let jumpedOut = Recur.fromString('FREQ=MONTHLY;BYMONTHDAY=1;COUNT=10')
    .iterator(T('2000-01-01T09:00:00Z'), { indexed: true });
  assert.equal(jumpedOut.seek(T('2001-06-01T00:00:00Z')), null);
  assert.equal(jumpedOut.completed, true);
  assert.equal(jumpedOut.seek(T('2001-01-01T00:00:00Z')), null);

  // UNTIL is inclusive: the boundary instant is still delivered.
  let until = Recur.fromString('FREQ=MONTHLY;BYMONTHDAY=1;UNTIL=20000501T090000Z')
    .iterator(T('2000-01-01T09:00:00Z'), { indexed: true });
  let untilOutput = [];
  let value;
  while ((value = until.next())) {
    untilOutput.push(value.toString());
  }
  assert.deepEqual(untilOutput, [
    '2000-01-01T09:00:00Z', '2000-02-01T09:00:00Z',
    '2000-03-01T09:00:00Z', '2000-04-01T09:00:00Z',
    '2000-05-01T09:00:00Z'
  ]);
  let untilSeek = Recur.fromString('FREQ=MONTHLY;BYMONTHDAY=1;UNTIL=20000501T090000Z')
    .iterator(T('2000-01-01T09:00:00Z'), { indexed: true });
  assert.equal(untilSeek.seek(T('2000-05-01T09:00:00Z')).toString(),
    '2000-05-01T09:00:00Z');
  assert.equal(untilSeek.next(), null);

  // Empty months do not consume COUNT: the 31st appears only in long months.
  let thirtyOne = Recur.fromString('FREQ=MONTHLY;BYMONTHDAY=31;COUNT=3')
    .iterator(T('2000-01-31T00:00:00Z'), { indexed: true });
  assert.deepEqual([
    thirtyOne.next().toString(),
    thirtyOne.next().toString(),
    thirtyOne.next().toString(),
    thirtyOne.next()
  ], [
    '2000-01-31T00:00:00Z', '2000-03-31T00:00:00Z',
    '2000-05-31T00:00:00Z', null
  ]);

  // A rule with no possible month (February, anchored with INTERVAL=12,
  // asking for the 30th) never produces anything.
  let never = Recur.fromString('FREQ=MONTHLY;INTERVAL=12;BYMONTHDAY=30')
    .iterator(T('2000-02-01T00:00:00Z'), { indexed: true });
  assert.equal(never.completed, true);
  assert.equal(never.next(), null);
  assert.equal(never.seek(T('2050-02-01T00:00:00Z')), null);

  // Explicit periodic count: month-end rule fires exactly once per month, so
  // the first instance at or in 9000 has global ordinal (9000-1800)*12 + 1.
  let monthEnd = Recur.fromString('FREQ=MONTHLY;BYMONTHDAY=-1')
    .iterator(T('1800-01-31T00:00:00Z'), { indexed: true });
  let firstAt9000 = monthEnd.seek(T('9000-01-01T00:00:00Z'));
  assert.equal(firstAt9000.toString(), '9000-01-31T00:00:00Z');
  assert.equal(monthEnd.occurrence_number, (9000 - 1800) * 12 + 1);
  let tail = [];
  for (let i = 0; i < 3; i++) {
    tail.push(monthEnd.next().toString());
  }
  assert.deepEqual(tail, [
    '9000-02-28T00:00:00Z', '9000-03-31T00:00:00Z', '9000-04-30T00:00:00Z'
  ]);

  // INTERVAL=13: active months satisfy (m - anchor) mod 13 == 0. Derive the
  // expected ordinal independently instead of replaying months.
  let interval13 = Recur.fromString('FREQ=MONTHLY;INTERVAL=13;BYMONTHDAY=1')
    .iterator(T('2000-01-01T00:00:00Z'), { indexed: true });
  let bound = T('2050-06-01T00:00:00Z');
  let hit = interval13.seek(bound);
  let boundMonth = 2050 * 12 + 5;
  let anchorMonth = 2000 * 12;
  let expectedStep = Math.ceil((boundMonth - anchorMonth) / 13);
  let hitMonth = hit.year * 12 + (hit.month - 1);
  assert.equal((hitMonth - anchorMonth) % 13, 0);
  assert.equal(hitMonth, anchorMonth + expectedStep * 13);
  assert.equal(interval13.occurrence_number, expectedStep + 1);
  assert.equal(hit.toString(), '2050-12-01T00:00:00Z');

  // INTERVAL=4801 shares no factor with the 4800-month Gregorian cycle, so
  // its phase never aligns with a cycle boundary; the jump must still land
  // on a valid stride month and continue afterwards.
  let interval4801 = Recur.fromString('FREQ=MONTHLY;INTERVAL=4801;BYMONTHDAY=1')
    .iterator(T('2000-01-01T00:00:00Z'), { indexed: true });
  let first = interval4801.next();
  let second = interval4801.next();
  assert.equal(first.toString(), '2000-01-01T00:00:00Z');
  assert.equal(second.toString(), '2400-02-01T00:00:00Z');
  let far = interval4801.seek(T('5000-01-01T00:00:00Z'));
  let farMonth = far.year * 12 + (far.month - 1);
  assert.equal((farMonth - anchorMonth) % 4801, 0);
  assert.ok(far.compare(T('5000-01-01T00:00:00Z')) >= 0);
  let farNext = interval4801.next();
  let farNextMonth = farNext.year * 12 + (farNext.month - 1);
  assert.equal(farNextMonth - farMonth, 4801);

  // Dense rule with BYSETPOS=1,-1: only two instances per month, at the first
  // and last weekday of the month with the extreme times of day.
  let denseText = 'FREQ=MONTHLY;BYDAY=MO,TU,WE,TH,FR;' +
    'BYHOUR=0,6,12,18;BYMINUTE=0,15,30,45;BYSECOND=0,10,20,30,40,50;' +
    'BYSETPOS=1,-1';
  let dense = Recur.fromString(denseText)
    .iterator(T('1800-01-01T09:00:00Z'), { indexed: true });
  let denseHit = dense.seek(T('9000-01-01T00:00:00Z'));
  assert.deepEqual([denseHit.toString(), dense.next().toString()], [
    '9000-01-01T00:00:00Z', '9000-01-31T18:45:50Z'
  ]);
  // Independently verify January 9000: the 1st is a Wednesday and the 31st
  // is a Friday, both weekdays, so both endpoints are selected.
  assert.equal(modelWeekday(9000, 1, 1), WEEKDAYS.WE);
  assert.equal(modelWeekday(9000, 1, 31), WEEKDAYS.FR);
  let denseTail = [];
  for (let i = 0; i < 4; i++) {
    denseTail.push(dense.next().toString());
  }
  assert.deepEqual(denseTail, [
    '9000-02-03T00:00:00Z', '9000-02-28T18:45:50Z',
    '9000-03-03T00:00:00Z', '9000-03-31T18:45:50Z'
  ]);

  // Same density without BYSETPOS: every candidate is delivered in order.
  let denseAllText = 'FREQ=MONTHLY;BYDAY=MO,TU,WE,TH,FR;' +
    'BYHOUR=0,6,12,18;BYMINUTE=0,15,30,45;BYSECOND=0,10,20,30,40,50';
  let denseAll = Recur.fromString(denseAllText)
    .iterator(T('1800-01-01T09:00:00Z'), { indexed: true });
  let allHit = denseAll.seek(T('9000-01-01T00:00:00Z'));
  assert.equal(allHit.toString(), '9000-01-01T00:00:00Z');
  let previous = allHit;
  for (let i = 0; i < 20; i++) {
    let current = denseAll.next();
    assert.ok(current.compare(previous) > 0, 'dense output stays ordered');
    previous = current;
  }
  // 21 weekdays times 96 times of day are available in January 9000 alone.
  assert.ok(denseAll.occurrence_number > (9000 - 1800) * 12 * 96);

  // Repeated seek on the same bound keeps consuming forward, and next() after
  // seek continues exactly where the stream would have been.
  let repeatable = Recur.fromString('FREQ=MONTHLY;BYMONTHDAY=1,15')
    .iterator(T('2000-01-01T09:00:00Z'), { indexed: true });
  let twin = Recur.fromString('FREQ=MONTHLY;BYMONTHDAY=1,15')
    .iterator(T('2000-01-01T09:00:00Z'), { indexed: true });
  let boundValue = T('2003-06-01T00:00:00Z');
  let seekedA = repeatable.seek(boundValue);
  let seekedB = repeatable.seek(boundValue);
  let twinA = twin.seek(boundValue);
  let twinNext = twin.next();
  assert.equal(seekedA.toString(), twinA.toString());
  assert.equal(seekedB.toString(), twinNext.toString());
  assert.ok(seekedB.compare(seekedA) > 0);

  // Seeking backward cannot reopen consumed instances.
  let forwardOnly = Recur.fromString('FREQ=MONTHLY;BYMONTHDAY=1')
    .iterator(T('2000-01-01T09:00:00Z'), { indexed: true });
  let at2005 = forwardOnly.seek(T('2005-01-01T00:00:00Z'));
  let back = forwardOnly.seek(T('1990-01-01T00:00:00Z'));
  assert.ok(back.compare(at2005) > 0);

  // Returned Times are independent of later iteration and caller mutation.
  let isolated = Recur.fromString('FREQ=MONTHLY;BYMONTHDAY=1,15')
    .iterator(T('2000-01-01T09:00:00Z'), { indexed: true });
  let firstResult = isolated.next();
  let snapshot = firstResult.toString();
  firstResult.year = 1900;
  firstResult.hour = 4;
  assert.equal(snapshot, '2000-01-01T09:00:00Z');
  assert.equal(isolated.next().toString(), '2000-01-15T09:00:00Z');
  let mutableBound = T('2000-03-01T00:00:00Z');
  let boundHit = isolated.seek(mutableBound);
  mutableBound.day = 20;
  assert.equal(boundHit.toString(), '2000-03-01T09:00:00Z');
});

// ---------------------------------------------------------------------------
// Group 3: merged RRULE/RDATE/EXDATE expansion in indexed mode
// ---------------------------------------------------------------------------

test('merged-expansion', () => {
  let merged = component([
    'BEGIN:VEVENT', 'UID:merge', 'DTSTART:20000101T090000Z',
    'RRULE:FREQ=MONTHLY;BYMONTHDAY=1,15;COUNT=6',
    'RRULE:FREQ=MONTHLY;BYMONTHDAY=10;COUNT=3',
    'RDATE:20000110T090000Z,20000201T090000Z,19991231T090000Z',
    'EXDATE:20000115T090000Z,20000210T090000Z', 'END:VEVENT', ''
  ]);
  let expansion = new RecurExpansion({
    component: merged,
    dtstart: merged.getFirstPropertyValue('dtstart'),
    indexed: true
  });
  let output = [];
  let value;
  while ((value = expansion.next())) {
    output.push(value.toString());
  }
  assert.deepEqual(output, [
    '1999-12-31T09:00:00Z', '2000-01-01T09:00:00Z',
    '2000-01-10T09:00:00Z', '2000-02-01T09:00:00Z',
    '2000-02-15T09:00:00Z', '2000-03-01T09:00:00Z',
    '2000-03-10T09:00:00Z', '2000-03-15T09:00:00Z'
  ]);

  // Each rule's COUNT is charged even where EXDATE or a merge removed its
  // instance. The second rule's 2000-02-10 instance was excluded but still
  // its second count, so it only has the March instance left.
  assert.equal(output.filter(item => item.endsWith('T09:00:00Z') &&
    item.slice(8, 10) === '10').length, 2);

  // RDATE-only events work and may precede DTSTART.
  let rdateOnly = component([
    'BEGIN:VEVENT', 'UID:rdate', 'DTSTART:20000101T090000Z',
    'RDATE:20010101T000000Z,20000601T000000Z', 'END:VEVENT', ''
  ]);
  let rdateExpansion = new RecurExpansion({
    component: rdateOnly,
    dtstart: rdateOnly.getFirstPropertyValue('dtstart'),
    indexed: true
  });
  let rdates = [];
  while ((value = rdateExpansion.next())) {
    rdates.push(value.toString());
  }
  assert.deepEqual(rdates, ['2000-06-01T00:00:00Z', '2001-01-01T00:00:00Z']);

  // No RRULE and no RDATE: DTSTART is the sole candidate and still passes
  // through EXDATE filtering.
  let solo = component([
    'BEGIN:VEVENT', 'UID:solo', 'DTSTART:20000101T090000Z',
    'END:VEVENT', ''
  ]);
  let soloExpansion = new RecurExpansion({
    component: solo,
    dtstart: solo.getFirstPropertyValue('dtstart'),
    indexed: true
  });
  assert.equal(soloExpansion.next().toString(), '2000-01-01T09:00:00Z');
  assert.equal(soloExpansion.next(), null);

  let soloExcluded = component([
    'BEGIN:VEVENT', 'UID:solo-ex', 'DTSTART:20000101T090000Z',
    'EXDATE:20000101T090000Z', 'END:VEVENT', ''
  ]);
  let excludedExpansion = new RecurExpansion({
    component: soloExcluded,
    dtstart: soloExcluded.getFirstPropertyValue('dtstart'),
    indexed: true
  });
  assert.equal(excludedExpansion.next(), null);
  assert.equal(excludedExpansion.complete, true);

  // Seek moves every source together, drops stale read-ahead values and
  // jumps a long run of EXDATEs without any attempt-count protection.
  let exdateLines = [
    'BEGIN:VEVENT', 'UID:exrun', 'DTSTART:20000101T090000Z',
    'RRULE:FREQ=MONTHLY;BYMONTHDAY=1',
    'RDATE:20050115T090000Z'
  ];
  for (let i = 0; i < 600; i++) {
    let d = new Date(Date.UTC(2004, 0, 1) + i * 86400000);
    let stamp = d.getUTCFullYear() +
      String(d.getUTCMonth() + 1).padStart(2, '0') +
      String(d.getUTCDate()).padStart(2, '0') + 'T090000Z';
    exdateLines.push('EXDATE:' + stamp);
  }
  exdateLines.push('END:VEVENT', '');
  let exrun = component(exdateLines);
  let exrunExpansion = new RecurExpansion({
    component: exrun,
    dtstart: exrun.getFirstPropertyValue('dtstart'),
    indexed: true
  });
  // 600 consecutive EXDATEs cover 2004-01-01 through 2005-08-23, so the
  // first surviving monthly instance after the jump is September 2005.
  let seeked = exrunExpansion.seek(T('2004-01-01T00:00:00Z'));
  assert.equal(seeked.toString(), '2005-09-01T09:00:00Z');
  assert.equal(exrunExpansion.next().toString(), '2005-10-01T09:00:00Z');

  // A rule exhausted by COUNT cannot be reopened by an expansion seek.
  let countedComponent = component([
    'BEGIN:VEVENT', 'UID:counted', 'DTSTART:20000101T090000Z',
    'RRULE:FREQ=MONTHLY;BYMONTHDAY=1;COUNT=3',
    'EXDATE:20000301T090000Z', 'END:VEVENT', ''
  ]);
  let countedExpansion = new RecurExpansion({
    component: countedComponent,
    dtstart: countedComponent.getFirstPropertyValue('dtstart'),
    indexed: true
  });
  assert.equal(countedExpansion.seek(T('2000-04-01T00:00:00Z')), null);
  assert.equal(countedExpansion.complete, true);

  // A merged duplicate is delivered once, and merging consumes both sources.
  let duplicate = component([
    'BEGIN:VEVENT', 'UID:dup', 'DTSTART:20000101T090000Z',
    'RRULE:FREQ=MONTHLY;BYMONTHDAY=10',
    'RDATE:20000110T090000Z', 'END:VEVENT', ''
  ]);
  let duplicateExpansion = new RecurExpansion({
    component: duplicate,
    dtstart: duplicate.getFirstPropertyValue('dtstart'),
    indexed: true
  });
  assert.equal(duplicateExpansion.next().toString(), '2000-01-10T09:00:00Z');
  assert.equal(duplicateExpansion.next().toString(), '2000-02-10T09:00:00Z');

  // Out-of-scope component inputs are rejected at construction.
  let rejects = [
    ['DATE DTSTART', [
      'BEGIN:VEVENT', 'UID:a', 'DTSTART;VALUE=DATE:20000101',
      'RRULE:FREQ=MONTHLY', 'END:VEVENT', ''
    ]],
    ['BYYEARDAY', [
      'BEGIN:VEVENT', 'UID:a', 'DTSTART:20000101T090000Z',
      'RRULE:FREQ=MONTHLY;BYYEARDAY=1', 'END:VEVENT', ''
    ]],
    ['YEARLY', [
      'BEGIN:VEVENT', 'UID:a', 'DTSTART:20000101T090000Z',
      'RRULE:FREQ=YEARLY', 'END:VEVENT', ''
    ]],
    ['DATE RDATE', [
      'BEGIN:VEVENT', 'UID:a', 'DTSTART:20000101T090000Z',
      'RDATE;VALUE=DATE:20000102', 'END:VEVENT', ''
    ]],
    ['PERIOD RDATE', [
      'BEGIN:VEVENT', 'UID:a', 'DTSTART:20000101T090000Z',
      'RDATE:20000102T090000Z/PT1H', 'END:VEVENT', ''
    ]]
  ];
  for (let [name, lines] of rejects) {
    let bad = component(lines);
    assert.throws(() => new RecurExpansion({
      component: bad,
      dtstart: bad.getFirstPropertyValue('dtstart'),
      indexed: true
    }), undefined, name);
  }

  // Legacy expansion keeps working and refuses seek explicitly.
  let legacy = new RecurExpansion({
    component: merged,
    dtstart: merged.getFirstPropertyValue('dtstart')
  });
  assert.throws(() => legacy.seek(T('2004-01-01T00:00:00Z')), /seek/);
  assert.equal(legacy.next().toString(), '1999-12-31T09:00:00Z');
});

// ---------------------------------------------------------------------------
// Group 4: serialization round trips and bounded-work observations
// ---------------------------------------------------------------------------

test('state-and-cost', () => {
  let ruleText = 'FREQ=MONTHLY;BYMONTHDAY=1,15';
  let startText = '2000-01-01T09:00:00Z';

  // Round trip at the initial state, after next, after seek and when done.
  function twin() {
    return Recur.fromString(ruleText).iterator(T(startText), { indexed: true });
  }
  function continuation(iterator) {
    let restored = new RecurIterator(JSON.parse(JSON.stringify(iterator)));
    let output = [];
    let value;
    while ((value = restored.next()) && output.length < 400) {
      output.push(value.toString());
    }
    return output;
  }

  let initial = twin();
  let initialTwin = twin();
  initialTwin.next();
  assert.deepEqual(continuation(initial), continuation(twin()));

  let afterNext = twin();
  afterNext.next();
  afterNext.next();
  let expectedAfterNext = [];
  let probe = twin();
  probe.next();
  probe.next();
  let probeValue;
  while ((probeValue = probe.next()) && expectedAfterNext.length < 400) {
    expectedAfterNext.push(probeValue.toString());
  }
  assert.deepEqual(continuation(afterNext), expectedAfterNext);

  let afterSeek = twin();
  afterSeek.seek(T('2010-06-01T00:00:00Z'));
  let seekProbe = twin();
  let seekHit = seekProbe.seek(T('2010-06-01T00:00:00Z'));
  let secondHit = seekProbe.next();
  let seekRest = [];
  let seekValue;
  while ((seekValue = seekProbe.next()) && seekRest.length < 400) {
    seekRest.push(seekValue.toString());
  }
  let restored = new RecurIterator(JSON.parse(JSON.stringify(afterSeek)));
  // seek() consumed the bound hit, so the restored iterator resumes at the
  // following instance.
  assert.equal(restored.next().toString(), secondHit.toString());
  let restoredRest = [];
  while ((seekValue = restored.next()) && restoredRest.length < 400) {
    restoredRest.push(seekValue.toString());
  }
  assert.deepEqual(restoredRest, seekRest);

  let countedRule = Recur.fromString('FREQ=MONTHLY;BYMONTHDAY=1,15;COUNT=4')
    .iterator(T(startText), { indexed: true });
  while (countedRule.next()) {
    // exhaust
  }
  assert.equal(countedRule.completed, true);
  let restoredDone = new RecurIterator(JSON.parse(JSON.stringify(countedRule)));
  assert.equal(restoredDone.next(), null);
  assert.equal(restoredDone.completed, true);
  assert.equal(restoredDone.seek(T('2001-01-01T00:00:00Z')), null);

  // Resuming indexed state performs no replay from DTSTART: the restored
  // iterator builds one cursor and can still make a distant jump directly.
  let resumable = twin();
  resumable.next();
  let restored2 = new RecurIterator(JSON.parse(JSON.stringify(resumable)));
  assert.equal(restored2.indexed.cursorBuilds, 1);
  let distant = restored2.seek(T('9000-01-01T00:00:00Z'));
  assert.equal(distant.toString(), '9000-01-01T09:00:00Z');
  assert.equal(restored2.indexed.spanCountCalls, 1);

  // Expansion round trips, including a merged duplicate state and the done
  // state, stay identical to a non-serialized twin.
  let expComponent = component([
    'BEGIN:VEVENT', 'UID:exp', 'DTSTART:20000101T090000Z',
    'RRULE:FREQ=MONTHLY;BYMONTHDAY=1,15',
    'RDATE:20000110T090000Z', 'EXDATE:20000215T090000Z',
    'END:VEVENT', ''
  ]);
  let expFactory = () => new RecurExpansion({
    component: expComponent,
    dtstart: expComponent.getFirstPropertyValue('dtstart'),
    indexed: true
  });
  let expA = expFactory();
  expA.next();
  expA.next();
  let expB = new RecurExpansion(JSON.parse(JSON.stringify(expA)));
  let expTwin = expFactory();
  expTwin.next();
  expTwin.next();
  for (let i = 0; i < 100; i++) {
    assert.equal((expB.next() || 'end').toString(),
      (expTwin.next() || 'end').toString());
  }

  let expSeek = expFactory();
  expSeek.seek(T('2010-06-01T00:00:00Z'));
  let expSeekRestored = new RecurExpansion(JSON.parse(JSON.stringify(expSeek)));
  let expSeekTwin = expFactory();
  expSeekTwin.seek(T('2010-06-01T00:00:00Z'));
  for (let i = 0; i < 50; i++) {
    assert.equal((expSeekRestored.next() || 'end').toString(),
      (expSeekTwin.next() || 'end').toString());
  }

  let expDoneFactory = () => {
    let countedExpComponent = component([
      'BEGIN:VEVENT', 'UID:exp-done', 'DTSTART:20000101T090000Z',
      'RRULE:FREQ=MONTHLY;BYMONTHDAY=1;COUNT=2', 'END:VEVENT', ''
    ]);
    return new RecurExpansion({
      component: countedExpComponent,
      dtstart: countedExpComponent.getFirstPropertyValue('dtstart'),
      indexed: true
    });
  };
  let countedExp = expDoneFactory();
  while (countedExp.next()) {
    // exhaust
  }
  assert.equal(countedExp.complete, true);
  let restoredExpDone = new RecurExpansion(JSON.parse(JSON.stringify(countedExp)));
  assert.equal(restoredExpDone.next(), null);
  assert.equal(restoredExpDone.complete, true);

  // Legacy serialized state keeps using the legacy path after restoration.
  let legacyIterator = Recur.fromString('FREQ=MONTHLY;BYMONTHDAY=15')
    .iterator(T('2000-01-01T09:00:00Z'));
  legacyIterator.next();
  let restoredLegacy = new RecurIterator(JSON.parse(JSON.stringify(legacyIterator)));
  assert.equal(restoredLegacy.indexed, null);
  assert.equal(restoredLegacy.next().toString(), '2000-02-15T09:00:00Z');
  assert.throws(() => restoredLegacy.seek(T('2001-01-01T00:00:00Z')), /seek/);

  // Legacy iterator seek always throws, even before any next() call.
  assert.throws(() => Recur.fromString('FREQ=MONTHLY')
    .iterator(T('2000-01-01T09:00:00Z')).seek(T('2001-01-01T00:00:00Z')), /seek/);

  // Bounded work on the real production path. The 1800 -> 9000 jump spans
  // 86 400 months but builds only a handful of month cursors and performs a
  // single constant-time span count.
  let engine = Recur.fromString('FREQ=MONTHLY;BYMONTHDAY=-1')
    .iterator(T('1800-01-31T00:00:00Z'), { indexed: true });
  let spanMonths = (9000 - 1800) * 12;
  let buildsBefore = engine.indexed.cursorBuilds;
  engine.seek(T('9000-01-01T00:00:00Z'));
  let buildsForSeek = engine.indexed.cursorBuilds - buildsBefore;
  assert.ok(buildsForSeek <= 5, `cursor builds ${buildsForSeek} stay bounded`);
  assert.ok(buildsForSeek < spanMonths, 'no month-by-month walk');
  assert.equal(engine.indexed.spanCountCalls, 1);

  // The non-aligned INTERVAL=4801 phase jumps directly as well.
  let phaseEngine = Recur.fromString('FREQ=MONTHLY;INTERVAL=4801;BYMONTHDAY=1')
    .iterator(T('2000-01-01T00:00:00Z'), { indexed: true });
  phaseEngine.seek(T('7000-01-01T00:00:00Z'));
  assert.ok(phaseEngine.indexed.cursorBuilds <= 4802);
  assert.equal(phaseEngine.indexed.spanCountCalls, 1);

  // Dense parts never materialize the Cartesian product: the time-of-day
  // list is bounded by the seconds in a day and BYSETPOS cursors hold only
  // the named positions.
  let denseText = 'FREQ=MONTHLY;BYDAY=MO,TU,WE,TH,FR;' +
    'BYHOUR=0,6,12,18;BYMINUTE=0,15,30,45;BYSECOND=0,10,20,30,40,50;' +
    'BYSETPOS=1,-1';
  let denseEngine = Recur.fromString(denseText)
    .iterator(T('1800-01-01T09:00:00Z'), { indexed: true });
  let spec = denseEngine.indexed.spec;
  assert.equal(spec.timesOfDay.length, 4 * 4 * 6);
  assert.ok(spec.timesOfDay.length <= 86400);
  assert.equal(spec.cycleDays.length, 4800);
  assert.ok(spec.strideCum.length <= 4801);
  denseEngine.seek(T('9000-01-01T00:00:00Z'));
  let denseCursor = denseEngine.indexed.cursor;
  assert.ok(denseCursor.picks.length <= 2);
  assert.ok(denseCursor.picks.length <= 2 * 366);

  // Without BYSETPOS the cursor carries no position array at all; indices
  // are arithmetic even for the full 86 400 times of day.
  let fullDay = Recur.fromString('FREQ=MONTHLY;BYMONTHDAY=1;BYHOUR=0;' +
    'BYMINUTE=0;BYSECOND=0')
    .iterator(T('1800-01-01T00:00:00Z'), { indexed: true });
  assert.equal(fullDay.indexed.cursor.picks, null);
  let allSeconds = new Recur({
    freq: 'MONTHLY',
    byhour: [0],
    byminute: [0],
    bysecond: Array.from({ length: 60 }, (_, i) => i)
  }).iterator(T('1800-01-01T00:00:00Z'), { indexed: true });
  assert.equal(allSeconds.indexed.spec.timesOfDay.length, 60);
  assert.equal(allSeconds.indexed.cursor.picks, null);

  // Structural rejection of out-of-range and non-integer values.
  let throwing = [
    'FREQ=YEARLY',
    'FREQ=DAILY',
    'FREQ=MONTHLY;BYMONTH=1',
    'FREQ=MONTHLY;BYYEARDAY=1',
    'FREQ=MONTHLY;BYWEEKNO=1',
    'FREQ=MONTHLY;BYMONTHDAY=0',
    'FREQ=MONTHLY;BYMONTHDAY=32',
    'FREQ=MONTHLY;BYMONTHDAY=-32',
    'FREQ=MONTHLY;BYSETPOS=0',
    'FREQ=MONTHLY;BYSETPOS=367',
    'FREQ=MONTHLY;BYDAY=6MO',
    'FREQ=MONTHLY;BYDAY=0MO',
    'FREQ=MONTHLY;BYHOUR=24',
    'FREQ=MONTHLY;BYMINUTE=60',
    'FREQ=MONTHLY;BYSECOND=61',
    'FREQ=MONTHLY;COUNT=1;UNTIL=20010101T000000Z'
  ];
  for (let ruleText of throwing) {
    assert.throws(() => Recur.fromString(ruleText)
      .iterator(T('2000-01-01T09:00:00Z'), { indexed: true }), undefined, ruleText);
  }
  assert.throws(() => {
    let rule = new Recur({ freq: 'MONTHLY' });
    rule.interval = 1.5;
    rule.iterator(T('2000-01-01T09:00:00Z'), { indexed: true });
  });
  assert.throws(() => Recur.fromString('FREQ=MONTHLY')
    .iterator(new Time({ year: 2000, month: 1, day: 1, isDate: true }),
      { indexed: true }));
  assert.throws(() => Recur.fromString('FREQ=MONTHLY')
    .iterator(T('1799-12-31T00:00:00Z'), { indexed: true }));
  let rangeOk = Recur.fromString('FREQ=MONTHLY')
    .iterator(T('1800-01-01T00:00:00Z'), { indexed: true });
  assert.throws(() => rangeOk.seek(T('1799-01-01T00:00:00Z')));
  assert.throws(() => rangeOk.seek(
    new Time({ year: 2001, month: 1, day: 1, isDate: true })));

  // Direct construction accepts the indexed option exactly like the helper.
  let direct = new RecurIterator({
    rule: Recur.fromString('FREQ=MONTHLY;BYMONTHDAY=1'),
    dtstart: T('2000-01-01T09:00:00Z'),
    indexed: true
  });
  assert.equal(direct.next().toString(), '2000-01-01T09:00:00Z');
  assert.equal(direct.next().toString(), '2000-02-01T09:00:00Z');

  // Instances past year 9999 end the rule instead of overflowing.
  let edge = Recur.fromString('FREQ=MONTHLY;BYMONTHDAY=1')
    .iterator(T('9999-08-01T09:00:00Z'), { indexed: true });
  assert.equal(edge.next().toString(), '9999-08-01T09:00:00Z');
  let later = edge.seek(T('9999-12-02T00:00:00Z'));
  assert.equal(later, null);
  assert.equal(edge.completed, true);
});
