import assert from 'node:assert/strict';
import {createServer} from 'node:http';
import {mkdtemp, mkdir, readFile} from 'node:fs/promises';
import {resolve, join} from 'node:path';
import {launch, Locator} from 'puppeteer-core';
import {createRunner} from '../src/Runner.js';
import {PuppeteerRunnerExtension} from '../src/PuppeteerRunnerExtension.js';
import {stringifyStep} from '../src/stringify.js';

const executablePath = process.env.REPLAY_CHROME;
if (!executablePath) throw new Error('Set REPLAY_CHROME to a local Chrome executable; no browser is downloaded');
const dir = await mkdtemp(resolve('.build/chrome-'));
const payload = Buffer.alloc(32768, 0x61);
let pendingResponse;
const server = createServer((req, res) => {
  if (req.url === '/export.csv') {
    pendingResponse = res;
    res.writeHead(200, {'Content-Type': 'text/csv', 'Content-Length': payload.length,
      'Content-Disposition': 'attachment; filename="report.csv"'});
    res.write(payload.subarray(0, 1024));
  } else {
    res.end('<!doctype html><button id="normal" onclick="window.normal=(window.normal||0)+1">normal</button><a id="export" download href="/export.csv">export</a>');
  }
});
await new Promise(r => server.listen(0, '127.0.0.1', r));
let browser;
try {
  browser = await launch({executablePath, headless: true, userDataDir: join(dir, 'profile'),
    args: ['--disable-crash-reporter', '--disable-breakpad', '--no-first-run']});
  console.log(`CHROME_VERSION ${await browser.version()}`);
  const page = await browser.newPage();
  const session = await browser.target().createCDPSession();
  const downloadPath = join(dir, 'downloads'); await mkdir(downloadPath);
  await session.send('Browser.setDownloadBehavior', {behavior: 'allowAndName', downloadPath, eventsEnabled: true});
  const url = `http://127.0.0.1:${server.address().port}`;
  const nextEvent = name => new Promise(r => session.once(name, r));
  for (const generated of [false, true]) {
    await page.goto(url);
    const normalStep = {type: 'click', selectors: ['#normal'], offsetX: 1, offsetY: 1};
    const click = {type: 'click', selectors: ['#export'], offsetX: 1, offsetY: 1};
    const next = {type: 'waitForExpression', expression: 'window.consumed = true'};
    const began = nextEvent('Browser.downloadWillBegin');
    let completed = false;
    let owned;
    let settle;
    const done = new Promise(r => { settle = r; });
    const progress = event => {
      if (event.guid === owned && event.state === 'completed') { completed = true; settle(event); }
    };
    session.on('Browser.downloadProgress', progress);
    if (!generated) {
      const runner = await createRunner({title: 'report export', steps: [normalStep, click, next]}, new PuppeteerRunnerExtension(browser, page));
      assert.equal(await runner.run(), true);
    } else {
      const AsyncFunction = Object.getPrototypeOf(async function() {}).constructor;
      for (const step of [normalStep, click, next]) {
        await new AsyncFunction('page', 'browser', 'Locator', 'timeout', await stringifyStep(step))(page, browser, Locator, 2000);
      }
    }
    const begin = await began; owned = begin.guid;
    assert.equal(begin.frameId, page.mainFrame()._id);
    assert.equal(begin.url, url + '/export.csv');
    assert.equal(begin.suggestedFilename, 'report.csv');
    assert.equal(await page.evaluate(() => window.normal), 1);
    assert.equal(await page.evaluate(() => window.consumed), true);
    assert.equal(completed, false);
    assert.ok(pendingResponse && !pendingResponse.writableEnded);
    console.log(`CHROME_EXPORT_INCOMPLETE mode=${generated ? 'generated' : 'runner'} action=1 next-step-before-completion=true frame-id-matched=true`);
    pendingResponse.end(payload.subarray(1024));
    const end = await done;
    assert.equal(end.receivedBytes, payload.length);
    assert.deepEqual(await readFile(join(downloadPath, owned)), payload);
    session.off('Browser.downloadProgress', progress);
    console.log(`CHROME_DOWNLOAD_COMPLETED mode=${generated ? 'generated' : 'runner'} bytes=${payload.length} guid-file-verified=true`);
  }
  await session.detach();
} finally {
  if (pendingResponse && !pendingResponse.writableEnded) pendingResponse.end(payload.subarray(1024));
  await browser?.close();
  await new Promise(r => server.close(r));
}
