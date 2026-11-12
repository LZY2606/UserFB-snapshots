import assert from 'node:assert/strict';
import { run } from 'node:test';
import { stat } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';

const mode = process.argv[2];
assert.ok(mode === 'target' || mode === 'old', 'choose target or old');
const file = fileURLToPath(new URL(mode === 'target'
  ? '../test/monthly_index_contract.mjs'
  : './upstream-tests.mjs', import.meta.url));
assert.ok((await stat(file)).size > 0, `empty test entry: ${file}`);
const required = new Set(mode === 'target' ? [
  'indexed-calendar', 'counted-jump', 'merged-expansion', 'state-and-cost'
] : []);
const seen = new Set();
const suites = new Set();
let tests = 0, failed = 0, skipped = 0, summaries = 0, registered = 0;
for await (const event of run({ files: [file], concurrency: 1, timeout: 60000 })) {
  const data = event.data;
  if (event.type === 'test:enqueue' && data.nesting === 0) ++registered;
  if (event.type === 'test:stdout' || event.type === 'test:stderr') {
    process.stdout.write(data.message);
  }
  if (event.type === 'test:pass') {
    if (data.details?.type === 'suite' && !data.skip && !data.todo) suites.add(data.name);
    if (data.skip || data.todo) ++skipped;
    if (data.details?.type === 'test') ++tests;
    if (data.nesting === 0 && data.details?.type === 'test' && !data.skip && !data.todo) {
      assert.ok(!seen.has(data.name), `duplicate top-level name: ${data.name}`);
      seen.add(data.name);
    }
  }
  if (event.type === 'test:fail') {
    ++failed;
    console.error(`FAIL ${data.name}: ${data.details?.error?.message || 'unknown error'}`);
  }
  if (event.type === 'test:summary') {
    ++summaries;
    if (data.counts) {
      assert.equal(data.counts.failed, 0, 'test summary has failures');
      assert.equal(data.counts.skipped, 0, 'test summary has skips');
      assert.equal(data.counts.todo, 0, 'test summary has todo');
    }
  }
}
assert.ok(summaries > 0, 'runner did not complete');
assert.equal(failed, 0, 'failed tests');
assert.equal(skipped, 0, 'skip/todo tests are not a completed run');
assert.ok(tests > 0, 'zero tests executed');
for (const name of required) assert.ok(seen.has(name), `missing executed group: ${name}`);
if (mode === 'target') assert.ok(registered >= required.size, 'target entry ended early');
if (mode === 'old') {
  assert.ok(suites.has('recur_iterator'), 'upstream suite did not finish');
  assert.ok(tests >= 159, 'upstream iterator cases missing');
}
console.log(`ICAL_${mode.toUpperCase()}_TESTS_OK executed=${tests} groups=${[...seen].join(',')}`);
