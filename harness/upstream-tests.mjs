// Run the unchanged upstream synchronous iterator tests with Node's test API.
// This adapter supplies only the five assertion methods used by that file.
import assert from 'node:assert';
import { describe, it, beforeEach } from 'node:test';
import ICAL from '../lib/ical/module.js';

globalThis.ICAL = ICAL;
globalThis.suite = describe;
globalThis.test = it;
globalThis.setup = beforeEach;
globalThis.assert = {
  equal: assert.equal,
  deepEqual: assert.deepEqual,
  ok: assert.ok,
  throws: assert.throws,
  isTrue: (value, message) => assert.strictEqual(value, true, message)
};
await import('../test/recur_iterator_test.js');
