/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// Locate a late window of an old monthly series without replaying every
// instance from DTSTART. The indexed monthly iterator jumps straight to the
// first occurrence at or after the window start, then the ordinary next()
// calls read just the window. Run with: node examples/monthly_index.mjs

import ICAL from '../lib/ical/module.js';

const { Recur, Time } = ICAL;
const at = value => Time.fromString(value);

// A dense weekday rule with sub-day parts and a month-end BYSETPOS. The
// indexed engine keeps the Cartesian product implicit, so the jump is cheap
// even though each month holds thousands of raw candidates.
const rule = Recur.fromString([
  'FREQ=MONTHLY',
  'BYDAY=MO,TU,WE,TH,FR',
  'BYHOUR=0,6,12,18',
  'BYMINUTE=0,15,30,45',
  'BYSECOND=0,10,20,30,40,50',
  'BYSETPOS=1,-1'
].join(';'));

const start = at('1800-01-01T09:00:00Z');
const iterator = rule.iterator(start, { indexed: true });

const windowStart = at('9000-01-01T00:00:00Z');
const windowEnd = at('9000-03-01T00:00:00Z');

// seek() discards every undelivered instance before the bound and returns the
// first one at or past it. The global COUNT, when present, still counts the
// discarded instances, so no rule semantics change.
let occurrence = iterator.seek(windowStart);
const window = [];
while (occurrence && occurrence.compare(windowEnd) < 0) {
  window.push(occurrence.toString());
  occurrence = iterator.next();
}

console.log(`window starts at ${windowStart.toString()} and holds ` +
  `${window.length} occurrences:`);
for (const value of window) {
  console.log('  ' + value);
}

// A COUNT-limited rule shows why the jump cannot be faked by moving DTSTART:
// the count is global from the original DTSTART, so seeking past the count
// ends the rule instead of restarting the numbering.
const counted = Recur.fromString('FREQ=MONTHLY;BYMONTHDAY=1,15;COUNT=5')
  .iterator(at('2000-01-01T09:00:00Z'), { indexed: true });
console.log('\nCOUNT=5 rule, seeking to 2000-03-02 returns: ' +
  String(counted.seek(at('2000-03-02T00:00:00Z'))));
