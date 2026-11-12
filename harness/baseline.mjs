// Public starting-point observation; no indexed implementation or target oracle.
import assert from 'node:assert/strict';
import ICAL from '../lib/ical/module.js';

const time = value => ICAL.Time.fromString(value);
const start = time('2000-01-01T09:00:00Z');
const rule = ICAL.Recur.fromString(
  'FREQ=MONTHLY;BYDAY=MO,TU,WE,TH,FR;COUNT=20000'
);
const iterator = rule.iterator(start);
const bound = time('2004-01-01T00:00:00Z');
let value, emitted = 0;
while ((value = iterator.next())) {
  ++emitted;
  if (value.compare(bound) >= 0) break;
}
assert.equal(value.toString(), '2004-01-01T09:00:00Z');
assert.equal(emitted, 1044);
console.log(`SEQUENTIAL_WINDOW next_calls=${emitted} first=${value.toString()}`);

// The existing BYSETPOS path holds few objects but still visits every input
// occurrence. Observe that distinction without changing production code.
const originalNext = ICAL.RecurIterator.prototype.next;
let calls = 0;
ICAL.RecurIterator.prototype.next = function(...args) {
  ++calls;
  return originalNext.apply(this, args);
};
let selected;
try {
  const dense = ICAL.Recur.fromString(
    'FREQ=MONTHLY;BYDAY=MO,TU,WE,TH,FR;BYHOUR=0,6,12,18;BYMINUTE=0,15,30,45;BYSECOND=0,10,20,30,40,50;BYSETPOS=-1;COUNT=2'
  ).iterator(start);
  selected = [dense.next().toString(), dense.next().toString()];
} finally {
  ICAL.RecurIterator.prototype.next = originalNext;
}
assert.deepEqual(selected, ['2000-01-31T18:45:50Z', '2000-02-29T18:45:50Z']);
console.log(`SETPOS_VISITS next_calls=${calls} output=${JSON.stringify(selected)}`);

const component = new ICAL.Component(ICAL.parse([
  'BEGIN:VEVENT', 'UID:monthly-import@example.invalid',
  'DTSTART:20000101T090000Z',
  'RRULE:FREQ=MONTHLY;BYMONTHDAY=1,15;COUNT=6',
  'RDATE:20000110T090000Z,20000201T090000Z',
  'EXDATE:20000115T090000Z', 'END:VEVENT', ''
].join('\r\n')));
const expansion = new ICAL.RecurExpansion({
  component, dtstart: component.getFirstPropertyValue('dtstart')
});
const expanded = [];
while ((value = expansion.next())) {
  expanded.push(value.toString());
  assert.ok(expanded.length <= 10);
}
assert.deepEqual(expanded, [
  '2000-01-01T09:00:00Z', '2000-01-10T09:00:00Z',
  '2000-02-01T09:00:00Z', '2000-02-15T09:00:00Z',
  '2000-03-01T09:00:00Z', '2000-03-15T09:00:00Z'
]);
console.log(`EXISTING_EXPANSION=${JSON.stringify(expanded)}`);
console.log('ICAL_MONTHLY_STARTPOINT_OK');
