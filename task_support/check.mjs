import assert from 'node:assert/strict';
import { test, after } from 'node:test';
const required = ['defaults','origins','live-edits','persistence','compatibility'];
const complete = new Set();
after(() => {
  assert.deepEqual([...complete].sort(), [...required].sort(), 'every required group must complete');
  console.log('TASK_SUITE_COMPLETE groups=5');
});
export function checked(name, body) {
  assert.ok(required.includes(name), 'unknown group');
  return test(name, async () => {
    assert.ok(!complete.has(name), 'duplicate group');
    let checks=0;
    const check = (actual, expected) => { checks++; assert.deepStrictEqual(actual,expected); };
    await body({check});
    assert.ok(checks>0,'group executed no assertions');
    complete.add(name);
    console.log(`TASK_GROUP_DONE ${name} checks=${checks}`);
  });
}
