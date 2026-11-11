import assert from 'node:assert/strict';
import {createRunner} from '../src/Runner.js';
import {PuppeteerRunnerExtension} from '../src/PuppeteerRunnerExtension.js';
import {parse} from '../src/SchemaUtils.js';
import {stringify} from '../src/stringify.js';
import {boundary} from './boundary.mjs';

const click = {type: 'click', selectors: ['#export'], offsetX: 1, offsetY: 1};
const flow = {title: 'report export', steps: [click,
  {type: 'waitForExpression', expression: 'window.reportConsumed = true'}]};
assert.deepEqual(parse(flow), flow);

for (const generated of [false, true]) {
  const b = boundary();
  // A normal browser action still executes and the following step is reached.
  if (!generated) {
    const runner = await createRunner(parse(flow), new PuppeteerRunnerExtension(b.browser, b.page));
    assert.equal(await runner.run(), true);
  } else {
    const code = await stringify(parse(flow));
    const body = code.replace(/^import \{ Locator, launch \} from 'puppeteer';[^\n]*\n/m, '');
    assert.notEqual(body, code, 'expected the upstream Puppeteer import');
    const AsyncFunction = Object.getPrototypeOf(async function() {}).constructor;
    await new AsyncFunction('launch', 'Locator', body)(async () => b.browser, b.Locator);
  }
  assert.equal(b.trace.filter(s => s === 'action').length, 1);
  assert.ok(b.trace.includes('next-step complete=false'));
  b.finish();
  console.log(`BASELINE_EXPORT_INCOMPLETE mode=${generated ? 'generated' : 'runner'} action=1 next-step-before-completion=true`);
}

const requested = structuredClone(flow);
requested.downloadPath = '/tmp/replay-report';
requested.steps[0].assertedEvents = [{type: 'download', url: 'https://reports.invalid/export.csv'}];
assert.throws(() => parse(requested), /Unknown assertedEvent type/);
console.log('BASELINE_DOWNLOAD_DECLARATION_REJECTED by=real-parser');
console.log('BASELINE_NORMAL_ACTION_AND_FOLLOWING_STEP_PASSED');
