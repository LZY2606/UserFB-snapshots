/**
 * @license
 * Copyright 2022 Google LLC
 * SPDX-License-Identifier: Apache-2.0
 */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { mkdtemp, mkdir, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { CDPSessionEvent } from 'puppeteer-core';

import { AssertedEventType, StepType } from './src/Schema.js';
import type { Step, UserFlow } from './src/Schema.js';
import { parse } from './src/SchemaUtils.js';
import { createRunner, Runner } from './src/Runner.js';
import {
  PuppeteerRunnerExtension,
  PuppeteerRunnerOwningBrowserExtension,
} from './src/PuppeteerRunnerExtension.js';
import { PuppeteerStringifyExtension } from './src/PuppeteerStringifyExtension.js';
import {
  parseSourceMap,
  stringify,
  stringifyStep,
  stripSourceMap,
} from './src/stringify.js';


const DOWNLOAD_URL = 'https://reports.example/export.csv';
const OTHER_URL = 'https://reports.example/other.csv';

function makeAbortError(): Error {
  const error = new Error('The operation was aborted');
  error.name = 'AbortError';
  return error;
}

interface RaceInvocation {
  settled: boolean;
  opts: { signal?: AbortSignal };
  resolve: () => void;
  reject: (error: Error) => void;
}

interface FakeRaceOptions {
  manualAction?: boolean;
  autoResolve?: boolean;
  onAction?: () => void;
}

class FakeRace {
  readonly #callbacks: Array<() => void> = [];
  readonly invocations: RaceInvocation[] = [];
  locators: unknown[] = [];
  readonly #manualAction: boolean;
  readonly #autoResolve: boolean;
  readonly #onAction?: () => void;
  #timeout: number | undefined;

  constructor(options: FakeRaceOptions = {}) {
    this.#manualAction = options.manualAction ?? false;
    this.#autoResolve = options.autoResolve ?? false;
    this.#onAction = options.onAction;
  }

  setTimeout(timeout: number): this {
    this.#timeout = timeout;
    return this;
  }

  on(name: string, fn: () => void): this {
    if (name === 'action') {
      this.#callbacks.push(fn);
    }
    return this;
  }

  #emitAction(): void {
    for (const fn of this.#callbacks) {
      fn();
    }
    this.#onAction?.();
  }

  /** Fires the action notification on demand (manual action mode). */
  flushAction(): void {
    this.#emitAction();
  }

  /** Simulates a repeated action notification. */
  reemitAction(): void {
    this.#emitAction();
  }

  click(opts: { signal?: AbortSignal }): Promise<void> {
    const invocation: RaceInvocation = {
      settled: false,
      opts,
      resolve: () => {},
      reject: () => {},
    };
    this.invocations.push(invocation);
    if (!this.#manualAction) {
      this.#emitAction();
    }
    return new Promise<void>((resolve, reject) => {
      let timeoutId: NodeJS.Timeout | undefined;
      invocation.resolve = () => {
        if (!invocation.settled) {
          invocation.settled = true;
          clearTimeout(timeoutId);
          resolve();
        }
      };
      invocation.reject = (error: Error) => {
        if (!invocation.settled) {
          invocation.settled = true;
          clearTimeout(timeoutId);
          reject(error);
        }
      };
      opts.signal?.addEventListener('abort', () => {
        invocation.reject(makeAbortError());
      });
      if (this.#timeout !== undefined) {
        const timeoutMs = this.#timeout;
        timeoutId = setTimeout(() => {
          invocation.reject(
            new Error(`Timed out after ${timeoutMs} ms`)
          );
        }, timeoutMs);
      }
      if (this.#autoResolve) {
        queueMicrotask(() => {
          invocation.resolve();
        });
      }
    });
  }
}

class FakeSession extends EventEmitter {
  readonly sent: Array<{ method: string; params: Record<string, unknown> }> = [];
  readonly cancelGuids: string[] = [];
  readonly sendFailures = new Map<string, Error>();
  readonly sendFailurePredicates: Array<
    (method: string, params: Record<string, unknown>) => Error | undefined
  > = [];
  detached = false;

  async send(
    method: string,
    params: Record<string, unknown> = {}
  ): Promise<Record<string, never>> {
    this.sent.push({ method, params });
    for (const predicate of this.sendFailurePredicates) {
      const failure = predicate(method, params);
      if (failure) {
        throw failure;
      }
    }
    const failure = this.sendFailures.get(method);
    if (failure) {
      throw failure;
    }
    if (method === 'Browser.cancelDownload') {
      this.cancelGuids.push(params.guid as string);
    }
    return {};
  }

  async detach(): Promise<void> {
    const failure = this.sendFailures.get('detach');
    if (failure) {
      throw failure;
    }
    this.detached = true;
  }

  disconnect(): void {
    this.emit(CDPSessionEvent.Disconnected, undefined);
  }
}

interface Harness {
  trace: string[];
  session: FakeSession;
  races: FakeRace[];
  browser: any;
  page: any;
  targetPage: any;
  childFrame: any;
  Locator: { race: (locators: unknown[]) => FakeRace };
  launch: () => Promise<any>;
  whenRace: () => Promise<FakeRace>;
}

function createHarness(
  options: {
    manualAction?: boolean;
    onAction?: () => void;
    version?: string;
  } = {}
): Harness {
  const trace: string[] = [];
  const session = new FakeSession();
  const races: FakeRace[] = [];
  const raceReady: Array<(race: FakeRace) => void> = [];

  const makeFrame = (id: string, children: any[] = []) => {
    const frame: any = {
      _id: id,
      childFrames: () => children,
      locator: (selector: string) => ({ selector }),
      _client: () => ({ send: async () => ({}) }),
      waitForFunction: async () => {
        trace.push(`next-step:${id}`);
      },
    };
    return frame;
  };

  const childFrame = makeFrame('frame-child');
  const mainFrame = makeFrame('frame-main', [childFrame]);
  const targetFrame = makeFrame('frame-target');

  const makePage = (frame: any) => {
    const page: any = {
      mainFrame: () => frame,
      browser: () => browser,
      browserContext: () => ({ id: 'ctx-report' }),
      setDefaultTimeout() {},
      waitForFrame: async () => frame,
      waitForFunction: async () => {
        trace.push(`next-step:${frame._id}`);
      },
      locator: (selector: string) => ({ selector }),
      locatorRace: (locators: unknown[]) => {
        const race = new FakeRace({
          manualAction: options.manualAction,
          autoResolve: options.autoResolve,
          onAction: options.onAction,
        });
        race.locators = locators;
        races.push(race);
        raceReady.shift()?.(race);
        return race;
      },
    };
    frame.page = page;
    return page;
  };

  const page = makePage(mainFrame);
  const targetPage = makePage(targetFrame);

  const browser: any = {
    version: async () => options.version ?? 'Chrome/130.0.0.0',
    newPage: async () => page,
    target: () => ({
      createCDPSession: async () => session,
    }),
    waitForTarget: async () => ({
      page: async () => targetPage,
    }),
    close: async () => {
      trace.push('browser-close');
    },
  };

  return {
    trace,
    session,
    races,
    browser,
    page,
    targetPage,
    childFrame,
    Locator: {
      race: (locators: unknown[]) => page.locatorRace(locators),
    },
    launch: async () => browser,
    whenRace: () =>
      new Promise<FakeRace>((resolve) => {
        raceReady.push(resolve);
      }),
  };
}

function downloadFlow(
  downloadPath: string,
  stepOverrides: Record<string, unknown> = {},
  flowOverrides: Record<string, unknown> = {}
): UserFlow {
  return parse({
    title: 'report export',
    downloadPath,
    timeout: 5000,
    ...flowOverrides,
    steps: [
      {
        type: StepType.Click,
        selectors: ['#export'],
        offsetX: 1,
        offsetY: 1,
        assertedEvents: [
          { type: AssertedEventType.Download, url: DOWNLOAD_URL },
        ],
        ...stepOverrides,
      },
      {
        type: StepType.WaitForExpression,
        expression: 'window.reportConsumed = true',
      },
    ],
  });
}

function willBegin(
  harness: Harness,
  guid: string,
  frameId: string,
  url: string = DOWNLOAD_URL,
  suggestedFilename = 'report.csv'
): void {
  harness.session.emit('Browser.downloadWillBegin', {
    guid,
    frameId,
    url,
    suggestedFilename,
  });
}

function progress(
  harness: Harness,
  guid: string,
  state: 'inProgress' | 'completed' | 'canceled',
  extra: Record<string, unknown> = {}
): void {
  harness.session.emit('Browser.downloadProgress', {
    guid,
    state,
    totalBytes: 20,
    receivedBytes: state === 'completed' ? 20 : 4,
    ...extra,
  });
}

async function makeDownloadFile(
  dir: string,
  guid: string,
  contents = 'csv-bytes'
): Promise<string> {
  const filePath = join(dir, guid);
  await writeFile(filePath, contents);
  return filePath;
}

async function withTempDir(prefix: string): Promise<string> {
  return mkdtemp(join(tmpdir(), `replay-${prefix}-`));
}

async function runWithRunner(
  harness: Harness,
  flow: UserFlow,
  extension?: PuppeteerRunnerExtension
): Promise<{
  runner: Runner;
  extension: PuppeteerRunnerExtension;
  promise: Promise<boolean>;
}> {
  const ext =
    extension ?? new PuppeteerRunnerExtension(harness.browser, harness.page);
  const runner = await createRunner(flow, ext);
  return { runner, extension: ext, promise: runner.run() };
}

async function runGeneratedScript(
  harness: Harness,
  flow: UserFlow
): Promise<{ completedDownloads: any[] }> {
  const code = await stringify(flow);
  assert.ok(
    !code.includes('@puppeteer/replay'),
    'generated scripts must not import private Replay helpers'
  );
  // The exported artifact is a real module-style script; strip its import
  // lines and bind the exact same imports the module would load.
  const body =
    code.replace(/^import [^\n]*\n/gm, '').trimEnd() +
    '\nglobalThis.__completedDownloads = completedDownloads;\n';
  const AsyncFunction = Object.getPrototypeOf(
    async function () {}
  ).constructor;
  const fn = new AsyncFunction(
    'launch',
    'Locator',
    'CDPSessionEvent',
    'mkdir',
    'stat',
    'access',
    'join',
    body
  );
  const fs = await import('node:fs/promises');
  const nodePath = await import('node:path');
  await fn(
    harness.launch,
    harness.Locator,
    CDPSessionEvent,
    fs.mkdir,
    fs.stat,
    fs.access,
    nodePath.join
  );
  return {
    completedDownloads: (globalThis as any).__completedDownloads,
  };
}

test('download correlation', async () => {
  const dir = await withTempDir('correlation');
  const harness = createHarness({ manualAction: true });

  // Parser keeps the new configuration and rejects illegal combinations.
  assert.deepEqual(
    parse({
      title: 't',
      downloadPath: dir,
      steps: [
        {
          type: 'click',
          selectors: ['#x'],
          offsetX: 1,
          offsetY: 1,
          assertedEvents: [{ type: 'download', url: DOWNLOAD_URL }],
        },
      ],
    }).steps[0]!.assertedEvents,
    [{ type: AssertedEventType.Download, url: DOWNLOAD_URL }]
  );
  for (const invalid of [
    {
      title: 't',
      steps: [
        {
          type: 'click',
          selectors: ['#x'],
          offsetX: 1,
          offsetY: 1,
          assertedEvents: [
            { type: 'download', url: DOWNLOAD_URL },
            { type: 'navigation' },
          ],
        },
      ],
    },
    {
      title: 't',
      downloadPath: dir,
      steps: [
        {
          type: 'click',
          selectors: ['#x'],
          offsetX: 1,
          offsetY: 1,
          assertedEvents: [
            { type: 'download', url: DOWNLOAD_URL },
            { type: 'download', url: OTHER_URL },
          ],
        },
      ],
    },
    {
      title: 't',
      downloadPath: dir,
      steps: [
        {
          type: 'hover',
          selectors: ['#x'],
          assertedEvents: [{ type: 'download', url: DOWNLOAD_URL }],
        },
      ],
    },
  ]) {
    assert.throws(() => parse(invalid), Error);
  }

  const flow = downloadFlow(dir);
  const { promise } = await runWithRunner(harness, flow);
  const race = await harness.whenRace();

  // A download announced before the first action notification cannot be
  // claimed, even when frame and URL match.
  willBegin(harness, 'guid-too-early', 'frame-main');
  assert.equal(harness.session.cancelGuids.length, 0);

  race.flushAction();
  const invocation = race.invocations[0]!;

  // Sibling frame with the same URL and same frame with another URL cannot
  // claim the step; neither can progress events for unrelated GUIDs.
  willBegin(harness, 'guid-sibling', 'frame-child');
  willBegin(harness, 'guid-other-url', 'frame-main', OTHER_URL);
  progress(harness, 'guid-sibling', 'inProgress');
  progress(harness, 'guid-other-url', 'completed');

  // A second action notification must not rebuild the wait or replace the
  // claim.
  race.reemitAction();

  willBegin(harness, 'guid-main', 'frame-main');
  progress(harness, 'guid-main', 'inProgress');

  // A late duplicate begin for another GUID cannot steal the claim.
  willBegin(harness, 'guid-late', 'frame-main');
  progress(harness, 'guid-late', 'inProgress');

  // The follow-up step must not run while the download is still in
  // progress, even if the click already returned.
  invocation.resolve();
  await Promise.resolve();
  assert.deepEqual(harness.trace, []);

  await makeDownloadFile(dir, 'guid-main');
  progress(harness, 'guid-main', 'completed');
  await promise;

  assert.deepEqual(harness.trace, ['next-step:frame-main']);
  // Late progress for a foreign GUID after settlement is a no-op.
  progress(harness, 'guid-late', 'completed');
  assert.equal(
    harness.session.listenerCount('Browser.downloadWillBegin'),
    0
  );
  assert.equal(
    harness.session.listenerCount('Browser.downloadProgress'),
    0
  );
  assert.deepEqual(harness.session.cancelGuids, []);

  // Independent target page: correlation uses the target frame's CDP id.
  {
    const targetDir = await withTempDir('correlation-target');
    const h2 = createHarness();
    const targetFlow = downloadFlow(
      targetDir,
      { target: 'https://other.example/page' },
      { timeout: 5000 }
    );
    const run = await runWithRunner(h2, targetFlow);
    await h2.whenRace();
    // The main-frame begin arrives first and must be ignored.
    willBegin(h2, 'guid-wrong-frame', 'frame-main');
    willBegin(h2, 'guid-target', 'frame-target');
    await makeDownloadFile(targetDir, 'guid-target');
    progress(h2, 'guid-target', 'completed');
    h2.races[0]!.invocations[0]!.resolve();
    await run.promise;
    assert.equal(
      run.extension.completedDownloads[0]!.guid,
      'guid-target',
      'only the begin matching the target frame CDP id is claimed'
    );
  }

  // Same-process nested frame reached through the frame selector.
  {
    const nestedDir = await withTempDir('correlation-nested');
    const h3 = createHarness();
    const nestedFlow = downloadFlow(nestedDir, { frame: [0] });
    const run = await runWithRunner(h3, nestedFlow);
    await h3.whenRace();
    willBegin(h3, 'guid-main-wrong', 'frame-main');
    willBegin(h3, 'guid-nested', 'frame-child');
    await makeDownloadFile(nestedDir, 'guid-nested');
    progress(h3, 'guid-nested', 'completed');
    h3.races[0]!.invocations[0]!.resolve();
    await run.promise;
    assert.equal(
      run.extension.completedDownloads[0]!.guid,
      'guid-nested',
      'the begin from the nested action frame is claimed'
    );
  }

  // doubleClick runs the same correlation path as click.
  {
    const dblDir = await withTempDir('correlation-double');
    const h4 = createHarness();
    const dblFlow = downloadFlow(dblDir, {
      type: StepType.DoubleClick,
    });
    const run = await runWithRunner(h4, dblFlow);
    await h4.whenRace();
    willBegin(h4, 'guid-double', 'frame-main');
    await makeDownloadFile(dblDir, 'guid-double');
    progress(h4, 'guid-double', 'completed');
    h4.races[0]!.invocations[0]!.resolve();
    await run.promise;
    assert.equal(
      run.extension.completedDownloads[0]!.guid,
      'guid-double'
    );
  }

  await rm(dir, { recursive: true, force: true });
});

test('download settlement', async () => {
  // Order A: the file settles while the click is still pending.
  {
    const dir = await withTempDir('settle-a');
    const harness = createHarness();
    const flow = downloadFlow(dir);
    const { extension, promise } = await runWithRunner(harness, flow);
    const race = await harness.whenRace();

    willBegin(harness, 'guid-a', 'frame-main');
    progress(harness, 'guid-a', 'inProgress');
    const filePath = await makeDownloadFile(dir, 'guid-a');
    progress(harness, 'guid-a', 'completed');
    await Promise.resolve();
    // Click has not returned yet; the next step must wait.
    assert.deepEqual(harness.trace, []);
    race.invocations[0]!.resolve();
    await promise;

    assert.deepEqual([...extension.completedDownloads], [
      {
        guid: 'guid-a',
        url: DOWNLOAD_URL,
        suggestedFilename: 'report.csv',
        path: filePath,
      },
    ]);
    assert.deepEqual(harness.trace, ['next-step:frame-main']);
  }

  // Order B: the click returns before the file finishes downloading.
  {
    const dir = await withTempDir('settle-b');
    const harness = createHarness();
    const flow = downloadFlow(dir);
    const { extension, promise } = await runWithRunner(harness, flow);
    const race = await harness.whenRace();

    willBegin(harness, 'guid-b', 'frame-main');
    race.invocations[0]!.resolve();
    await Promise.resolve();
    assert.deepEqual(harness.trace, []);

    // Equal byte counts while still inProgress must not complete the step.
    progress(harness, 'guid-b', 'inProgress', {
      totalBytes: 20,
      receivedBytes: 20,
    });
    await Promise.resolve();
    assert.deepEqual(harness.trace, []);

    const filePath = await makeDownloadFile(dir, 'guid-b');
    progress(harness, 'guid-b', 'completed');
    await promise;

    assert.deepEqual([...extension.completedDownloads], [
      {
        guid: 'guid-b',
        url: DOWNLOAD_URL,
        suggestedFilename: 'report.csv',
        path: filePath,
      },
    ]);
  }

  // begin + completed emitted synchronously from the action notification.
  {
    const dir = await withTempDir('settle-sync');
    const harness = createHarness({
      autoResolve: true,
      onAction: () => {
        willBegin(harness, 'guid-sync', 'frame-main');
        progress(harness, 'guid-sync', 'completed');
      },
    });
    // The GUID file already exists when the events fire.
    await writeFile(join(dir, 'guid-sync'), 'csv-bytes');
    const flow = downloadFlow(dir);
    const { extension, promise } = await runWithRunner(harness, flow);
    await promise;
    assert.equal(extension.completedDownloads.length, 1);
    assert.equal(extension.completedDownloads[0]!.guid, 'guid-sync');
  }

  // A `completed` event without a readable regular file fails the step.
  {
    const dir = await withTempDir('settle-missing');
    const harness = createHarness();
    const flow = downloadFlow(dir, { timeout: 3000 });
    const { promise } = await runWithRunner(harness, flow);
    await harness.whenRace();
    willBegin(harness, 'guid-missing', 'frame-main');
    progress(harness, 'guid-missing', 'completed');
    harness.races[0]!.invocations[0]!.resolve();
    await assert.rejects(promise, /not readable/);
    assert.deepEqual(harness.session.cancelGuids, ['guid-missing']);
  }

  // A directory occupying the GUID name is not a regular file.
  {
    const dir = await withTempDir('settle-directory');
    const harness = createHarness();
    const flow = downloadFlow(dir);
    const { promise } = await runWithRunner(harness, flow);
    await harness.whenRace();
    willBegin(harness, 'guid-dir', 'frame-main');
    await mkdir(join(dir, 'guid-dir'));
    progress(harness, 'guid-dir', 'completed');
    harness.races[0]!.invocations[0]!.resolve();
    await assert.rejects(promise, /not a regular file/);
  }

  // Published records survive a failure of a later step.
  {
    const dir = await withTempDir('settle-survive');
    const harness = createHarness();
    const flow = parse({
      title: 'report export',
      downloadPath: dir,
      steps: [
        {
          type: StepType.Click,
          selectors: ['#export'],
          offsetX: 1,
          offsetY: 1,
          assertedEvents: [
            { type: AssertedEventType.Download, url: DOWNLOAD_URL },
          ],
        },
        {
          type: StepType.Click,
          selectors: ['.does-not-exist'],
          offsetX: 1,
          offsetY: 1,
          timeout: 50,
        },
      ],
    });
    const { extension, promise } = await runWithRunner(harness, flow);
    await harness.whenRace();
    willBegin(harness, 'guid-keep', 'frame-main');
    await makeDownloadFile(dir, 'guid-keep');
    progress(harness, 'guid-keep', 'completed');
    harness.races[0]!.invocations[0]!.resolve();
    await assert.rejects(promise);
    assert.equal(extension.completedDownloads.length, 1);
    assert.equal(extension.completedDownloads[0]!.guid, 'guid-keep');
  }
});

test('download cleanup', async () => {
  // Total timeout: no begin means no cancel and no leftover listeners.
  {
    const dir = await withTempDir('cleanup-timeout');
    const harness = createHarness();
    const flow = downloadFlow(dir, { timeout: 50 });
    const { promise } = await runWithRunner(harness, flow);
    await assert.rejects(promise, /Timed out/);
    assert.deepEqual(harness.session.cancelGuids, []);
    assert.equal(
      harness.session.listenerCount('Browser.downloadWillBegin'),
      0
    );
    assert.equal(
      harness.session.listenerCount('Browser.downloadProgress'),
      0
    );
    assert.equal(harness.session.detached, true);
    assert.ok(
      !harness.trace.includes('browser-close'),
      'direct replay must not close the caller browser'
    );
    const behaviors = harness.session.sent.filter(
      (call) => call.method === 'Browser.setDownloadBehavior'
    );
    assert.equal(behaviors[0]!.params.behavior, 'allowAndName');
    assert.equal(
      behaviors[0]!.params.browserContextId,
      'ctx-report',
      'the policy is scoped to the page browser context'
    );
    assert.equal(behaviors[1]!.params.behavior, 'default');
    assert.equal(
      behaviors[1]!.params.browserContextId,
      'ctx-report',
      'teardown restores the same context to default'
    );
  }

  // Timeout after a claim aborts the locator action and cancels the
  // pending claimed download exactly once.
  {
    const dir = await withTempDir('cleanup-timeout-claimed');
    const harness = createHarness();
    const flow = downloadFlow(dir, { timeout: 40 });
    const { promise } = await runWithRunner(harness, flow);
    await harness.whenRace();
    willBegin(harness, 'guid-timeout-cancel', 'frame-main');
    const invocation = harness.races[0]!.invocations[0]!;
    await assert.rejects(promise, /Timed out/);
    assert.deepEqual(harness.session.cancelGuids, [
      'guid-timeout-cancel',
    ]);
    assert.equal(invocation.opts.signal?.aborted, true);
    assert.equal(
      harness.session.listenerCount('Browser.downloadProgress'),
      0
    );
    assert.equal(harness.session.detached, true);
    // A locator that ignores the abort signal and succeeds late cannot
    // publish a second result; a late completed progress cannot either.
    invocation.resolve();
    await makeDownloadFile(dir, 'guid-timeout-cancel');
    progress(harness, 'guid-timeout-cancel', 'completed');
    await Promise.resolve();
    assert.equal(
      harness.session.listenerCount('Browser.downloadWillBegin'),
      0
    );
  }

  // The default browser context (id === undefined) is targeted by
  // omitting the context parameter, both on setup and restore.
  {
    const dir = await withTempDir('cleanup-default-context');
    const harness = createHarness();
    harness.page.browserContext = () => ({ id: undefined });
    const extension = new PuppeteerRunnerExtension(
      harness.browser,
      harness.page
    );
    const runner = await createRunner(downloadFlow(dir), extension);
    const run = runner.run();
    await harness.whenRace();
    willBegin(harness, 'guid-default-ctx', 'frame-main');
    await makeDownloadFile(dir, 'guid-default-ctx');
    progress(harness, 'guid-default-ctx', 'completed');
    harness.races[0]!.invocations[0]!.resolve();
    await run;
    const behaviors = harness.session.sent.filter(
      (call) => call.method === 'Browser.setDownloadBehavior'
    );
    assert.equal(
      Object.hasOwn(behaviors[0]!.params, 'browserContextId'),
      false
    );
    assert.equal(
      Object.hasOwn(behaviors[1]!.params, 'browserContextId'),
      false
    );
  }

  // A normal (non-download) flow still runs on the same extension
  // afterwards and installs no download policy of its own.
  {
    const dir = await withTempDir('cleanup-reuse');
    const harness = createHarness({ autoResolve: true });
    const extension = new PuppeteerRunnerExtension(
      harness.browser,
      harness.page
    );
    const downloadRunner = await createRunner(downloadFlow(dir), extension);
    const run1 = downloadRunner.run();
    await harness.whenRace();
    willBegin(harness, 'guid-reuse', 'frame-main');
    await makeDownloadFile(dir, 'guid-reuse');
    progress(harness, 'guid-reuse', 'completed');
    await run1;

    const normalFlow = parse({
      title: 'plain',
      steps: [
        {
          type: StepType.Click,
          selectors: ['#normal'],
          offsetX: 1,
          offsetY: 1,
        },
      ],
    });
    const runner2 = await createRunner(normalFlow, extension);
    assert.equal(await runner2.run(), true);
    assert.equal(
      harness.session.sent.filter(
        (call) => call.method === 'Browser.setDownloadBehavior'
      ).length,
      2,
      'the second run neither creates nor restores a download session'
    );
    assert.ok(!harness.trace.includes('browser-close'));
  }

  // completedDownloads is cleared at the start of every run.
  {
    const dir = await withTempDir('cleanup-clear');
    const harness = createHarness({ autoResolve: true });
    const extension = new PuppeteerRunnerExtension(
      harness.browser,
      harness.page
    );
    const first = await createRunner(downloadFlow(dir), extension);
    const run1 = first.run();
    await harness.whenRace();
    willBegin(harness, 'guid-first', 'frame-main');
    await makeDownloadFile(dir, 'guid-first');
    progress(harness, 'guid-first', 'completed');
    await run1;
    assert.equal(extension.completedDownloads.length, 1);

    const second = await createRunner(
      parse({
        title: 'plain',
        steps: [
          {
            type: StepType.Click,
            selectors: ['#normal'],
            offsetX: 1,
            offsetY: 1,
          },
        ],
      }),
      extension
    );
    await second.run();
    assert.deepEqual([...extension.completedDownloads], []);
  }

  // Browser-reported cancel fails the step; an already canceled download
  // is not pending and must not be canceled again.
  {
    const dir = await withTempDir('cleanup-canceled');
    const harness = createHarness();
    const flow = downloadFlow(dir);
    const { promise } = await runWithRunner(harness, flow);
    await harness.whenRace();
    willBegin(harness, 'guid-canceled', 'frame-main');
    progress(harness, 'guid-canceled', 'canceled');
    harness.races[0]!.invocations[0]!.resolve();
    await assert.rejects(promise, /canceled/);
    assert.deepEqual(harness.session.cancelGuids, []);
  }

  // Action error keeps its object identity and cancels the claimed download.
  {
    const dir = await withTempDir('cleanup-action-error');
    const harness = createHarness();
    const flow = downloadFlow(dir);
    const { promise } = await runWithRunner(harness, flow);
    await harness.whenRace();
    willBegin(harness, 'guid-action', 'frame-main');
    const originalError = new Error('locator exploded');
    harness.races[0]!.invocations[0]!.reject(originalError);
    const error = await promise.then(
      () => {
        throw new Error('expected rejection');
      },
      (error) => error
    );
    assert.equal(error, originalError);
    assert.deepEqual(harness.session.cancelGuids, ['guid-action']);
  }

  // Session disconnect fails an in-flight step.
  {
    const dir = await withTempDir('cleanup-disconnect');
    const harness = createHarness();
    const flow = downloadFlow(dir);
    const { promise } = await runWithRunner(harness, flow);
    await harness.whenRace();
    willBegin(harness, 'guid-disconnect', 'frame-main');
    harness.session.disconnect();
    await assert.rejects(promise, /disconnected/);
    assert.deepEqual(harness.session.cancelGuids, ['guid-disconnect']);
    assert.equal(
      harness.session.listenerCount('Browser.downloadProgress'),
      0
    );
  }

  // Session setup failure: beforeAllSteps fails, session is detached and
  // afterAllSteps still runs.
  {
    const dir = await withTempDir('cleanup-setup-fail');
    const harness = createHarness();
    harness.session.sendFailures.set(
      'Browser.setDownloadBehavior',
      new Error('boom during setup')
    );
    class CountingExtension extends PuppeteerRunnerExtension {
      afterAllCount = 0;
      override async afterAllSteps() {
        this.afterAllCount++;
        await super.afterAllSteps();
      }
    }
    const extension = new CountingExtension(harness.browser, harness.page);
    const runner = await createRunner(downloadFlow(dir), extension);
    await assert.rejects(runner.run(), /boom during setup/);
    assert.equal(harness.session.detached, true);
    assert.equal(extension.afterAllCount, 1);
  }

  // A partial beforeAllSteps failure that happens after the super hook
  // established the download session still releases everything, and
  // afterAllSteps runs exactly once with the original error preserved.
  {
    const dir = await withTempDir('cleanup-beforeall-partial');
    const harness = createHarness();
    const partialError = new Error('user beforeAllSteps exploded');
    class PartialExtension extends PuppeteerRunnerExtension {
      afterAllCount = 0;
      override async beforeAllSteps(flow?: UserFlow) {
        await super.beforeAllSteps(flow);
        throw partialError;
      }
      override async afterAllSteps() {
        this.afterAllCount++;
        await super.afterAllSteps();
      }
    }
    const extension = new PartialExtension(harness.browser, harness.page);
    const runner = await createRunner(downloadFlow(dir), extension);
    const error = await runner.run().then(
      () => {
        throw new Error('expected rejection');
      },
      (error) => error
    );
    assert.equal(error, partialError);
    assert.equal(extension.afterAllCount, 1);
    assert.equal(harness.session.detached, true);
    assert.equal(harness.races.length, 0, 'no step action ever starts');
    const behaviors = harness.session.sent.filter(
      (call) => call.method === 'Browser.setDownloadBehavior'
    );
    assert.equal(behaviors[0]!.params.behavior, 'allowAndName');
    assert.equal(behaviors[1]!.params.behavior, 'default');
  }

  // Teardown failure is visible when it is the only failure.
  {
    const dir = await withTempDir('cleanup-teardown-fail');
    const harness = createHarness();
    const flow = downloadFlow(dir);
    const { promise } = await runWithRunner(harness, flow);
    await harness.whenRace();
    willBegin(harness, 'guid-teardown', 'frame-main');
    await makeDownloadFile(dir, 'guid-teardown');
    progress(harness, 'guid-teardown', 'completed');
    harness.races[0]!.invocations[0]!.resolve();
    harness.session.sendFailures.set(
      'Browser.setDownloadBehavior',
      new Error('restore failed')
    );
    harness.session.sendFailures.set('detach', new Error('detach failed'));
    await assert.rejects(promise, /teardown failed/);
  }

  // Teardown failure must not overwrite the main step error.
  {
    const dir = await withTempDir('cleanup-main-wins');
    const harness = createHarness();
    const flow = downloadFlow(dir, { timeout: 50 });
    const { promise } = await runWithRunner(harness, flow);
    // Fail only the teardown (second) policy call; setup must still work.
    let policyCalls = 0;
    harness.session.sendFailurePredicates.push((method) => {
      if (method === 'Browser.setDownloadBehavior') {
        policyCalls++;
        if (policyCalls > 1) {
          return new Error('restore failed');
        }
      }
      if (method === 'detach') {
        return new Error('detach failed');
      }
      return undefined;
    });
    await assert.rejects(promise, /Timed out/);
  }

  // Abort between steps still releases the session and runs afterAllSteps
  // exactly once; afterEachStep only ran for the successful step.
  {
    const dir = await withTempDir('cleanup-abort');
    const harness = createHarness();
    class AbortExtension extends PuppeteerRunnerExtension {
      runner: Runner | null = null;
      counts = { beforeAll: 0, afterAll: 0, afterEach: 0 };
      override async beforeAllSteps(flow?: UserFlow) {
        this.counts.beforeAll++;
        await super.beforeAllSteps(flow);
      }
      override async afterAllSteps() {
        this.counts.afterAll++;
        await super.afterAllSteps();
      }
      override async afterEachStep() {
        this.counts.afterEach++;
        this.runner!.abort();
      }
    }
    const extension = new AbortExtension(harness.browser, harness.page);
    const runner = await createRunner(downloadFlow(dir), extension);
    extension.runner = runner;
    const run = runner.run();
    await harness.whenRace();
    willBegin(harness, 'guid-abort', 'frame-main');
    await makeDownloadFile(dir, 'guid-abort');
    progress(harness, 'guid-abort', 'completed');
    harness.races[0]!.invocations[0]!.resolve();
    assert.equal(await run, false);
    assert.deepEqual(extension.counts, {
      beforeAll: 1,
      afterAll: 1,
      afterEach: 1,
    });
    assert.equal(harness.session.detached, true);
    assert.equal(
      harness.session.listenerCount('Browser.downloadWillBegin'),
      0
    );
  }

  // Firefox rejects the new configuration explicitly.
  {
    const dir = await withTempDir('cleanup-firefox');
    const harness = createHarness({ version: 'Firefox/130.0' });
    const { promise } = await runWithRunner(harness, downloadFlow(dir));
    await assert.rejects(promise, /Firefox/);
  }

  // Single step execution and stringify reject download declarations.
  {
    const dir = await withTempDir('cleanup-single');
    const harness = createHarness();
    const extension = new PuppeteerRunnerExtension(
      harness.browser,
      harness.page
    );
    const runner = await createRunner(extension);
    const step = parse({
      title: 't',
      downloadPath: dir,
      steps: [
        {
          type: 'click',
          selectors: ['#x'],
          offsetX: 1,
          offsetY: 1,
          assertedEvents: [
            { type: AssertedEventType.Download, url: DOWNLOAD_URL },
          ],
        },
      ],
    }).steps[0]! as Step;
    await assert.rejects(runner.runStep(step), /full flow run/);
    await assert.rejects(stringifyStep(step), /single step/);
  }

  // Callers that bypass parse still cannot run an illegal click: the
  // action must never start; the flow-level session is still established
  // and released cleanly and the validation error surfaces.
  for (const [label, badStep] of [
    [
      'download+navigation',
      {
        type: StepType.Click,
        selectors: ['#x'],
        offsetX: 1,
        offsetY: 1,
        assertedEvents: [
          { type: AssertedEventType.Download, url: DOWNLOAD_URL },
          { type: AssertedEventType.Navigation },
        ],
      },
    ],
    [
      'download-on-hover',
      {
        type: StepType.Hover,
        selectors: ['#x'],
        assertedEvents: [
          { type: AssertedEventType.Download, url: DOWNLOAD_URL },
        ],
      },
    ],
  ] as const) {
    const dir = await withTempDir('cleanup-illegal-programmatic');
    const harness = createHarness();
    const flow = {
      title: 'hand built',
      downloadPath: dir,
      steps: [badStep],
    } as unknown as UserFlow;
    const runner = await createRunner(
      flow,
      new PuppeteerRunnerExtension(harness.browser, harness.page)
    );
    await assert.rejects(runner.run(), Error, label);
    assert.equal(harness.races.length, 0, `${label}: action never starts`);
    assert.equal(harness.session.detached, true, label);
    const behaviors = harness.session.sent.filter(
      (call) => call.method === 'Browser.setDownloadBehavior'
    );
    assert.equal(behaviors.length, 2, `${label}: session set up and restored`);
    assert.equal(behaviors[1]!.params.behavior, 'default', label);
  }

  // Two download assertions on one step are rejected at runtime too.
  {
    const dir = await withTempDir('cleanup-two-downloads');
    const harness = createHarness();
    const flow = {
      title: 'hand built',
      downloadPath: dir,
      steps: [
        {
          type: StepType.Click,
          selectors: ['#x'],
          offsetX: 1,
          offsetY: 1,
          assertedEvents: [
            { type: AssertedEventType.Download, url: DOWNLOAD_URL },
            { type: AssertedEventType.Download, url: OTHER_URL },
          ],
        },
      ],
    } as unknown as UserFlow;
    const runner = await createRunner(
      flow,
      new PuppeteerRunnerExtension(harness.browser, harness.page)
    );
    await assert.rejects(runner.run(), /at most one download/);
    assert.equal(harness.races.length, 0);
    assert.equal(harness.session.detached, true);
  }

  // The extension that owns its browser always closes it, even on failure.
  {
    const dir = await withTempDir('cleanup-owning');
    const harness = createHarness();
    const extension = new PuppeteerRunnerOwningBrowserExtension(
      harness.browser,
      harness.page
    );
    const runner = await createRunner(downloadFlow(dir), extension);
    const run = runner.run();
    await harness.whenRace();
    willBegin(harness, 'guid-owning', 'frame-main');
    progress(harness, 'guid-owning', 'canceled');
    await assert.rejects(run);
    assert.ok(harness.trace.includes('browser-close'));
  }
});

test('download generated parity', async () => {
  const dir = await withTempDir('parity');

  // Same correlation and settlement rules in the exported standalone
  // script, including both completion orders.
  for (const order of ['file-first', 'click-first'] as const) {
    const harness = createHarness();
    const flow = downloadFlow(dir);
    const run = runGeneratedScript(harness, flow);
    await harness.whenRace();
    const guid = `guid-generated-${order}`;
    willBegin(harness, guid, 'frame-main');
    // Interference from sibling frames and other GUIDs.
    willBegin(harness, 'guid-noise', 'frame-child');
    progress(harness, 'guid-noise', 'completed');
    const filePath = await makeDownloadFile(dir, guid);
    if (order === 'file-first') {
      progress(harness, guid, 'completed');
      harness.races[0]!.invocations[0]!.resolve();
    } else {
      harness.races[0]!.invocations[0]!.resolve();
      progress(harness, guid, 'completed');
    }
    const { completedDownloads } = await run;
    assert.deepEqual(completedDownloads, [
      {
        guid,
        url: DOWNLOAD_URL,
        suggestedFilename: 'report.csv',
        path: filePath,
      },
    ]);
    assert.deepEqual(
      harness.trace.filter((entry) => entry.startsWith('next-step:')),
      ['next-step:frame-main']
    );
    assert.equal(harness.session.detached, true);
    assert.ok(harness.trace.includes('browser-close'));
  }

  // Synchronous begin/completed within the action notification.
  {
    const harness = createHarness({
      autoResolve: true,
      onAction: () => {
        willBegin(harness, 'guid-generated-sync', 'frame-main');
        progress(harness, 'guid-generated-sync', 'completed');
      },
    });
    await writeFile(join(dir, 'guid-generated-sync'), 'csv-bytes');
    const flow = downloadFlow(dir);
    const { completedDownloads } = await runGeneratedScript(harness, flow);
    assert.equal(completedDownloads[0]!.guid, 'guid-generated-sync');
  }

  // Failure path: a browser cancel surfaces, does not re-cancel, and
  // cleanup and browser close still happen.
  {
    const harness = createHarness();
    const flow = downloadFlow(dir);
    const run = runGeneratedScript(harness, flow);
    await harness.whenRace();
    willBegin(harness, 'guid-generated-fail', 'frame-main');
    progress(harness, 'guid-generated-fail', 'canceled');
    harness.races[0]!.invocations[0]!.resolve();
    await assert.rejects(run, /canceled/);
    assert.deepEqual(harness.session.cancelGuids, []);
    assert.equal(harness.session.detached, true);
    assert.ok(harness.trace.includes('browser-close'));
  }

  // Timeout path without a claimed download never cancels anybody else.
  {
    const harness = createHarness();
    const flow = downloadFlow(dir, { timeout: 50 });
    const run = runGeneratedScript(harness, flow);
    await assert.rejects(run, /Timed out/);
    assert.deepEqual(harness.session.cancelGuids, []);
    assert.equal(harness.session.detached, true);
  }

  // A teardown failure on an otherwise successful exported run is visible
  // and the browser is still closed.
  {
    const harness = createHarness();
    const flow = downloadFlow(dir);
    let policyCalls = 0;
    harness.session.sendFailurePredicates.push((method) => {
      if (method === 'Browser.setDownloadBehavior') {
        policyCalls++;
        if (policyCalls > 1) {
          return new Error('restore failed');
        }
      }
      return undefined;
    });
    const run = runGeneratedScript(harness, flow);
    await harness.whenRace();
    const guid = 'guid-generated-teardown';
    willBegin(harness, guid, 'frame-main');
    await makeDownloadFile(dir, guid);
    progress(harness, guid, 'completed');
    harness.races[0]!.invocations[0]!.resolve();
    await assert.rejects(run, /restore failed/);
    assert.ok(harness.trace.includes('browser-close'));
  }

  // When both the step and teardown fail, the main step error wins and
  // the browser is still closed.
  {
    const harness = createHarness();
    const flow = downloadFlow(dir, { timeout: 50 });
    let policyCalls = 0;
    harness.session.sendFailurePredicates.push((method) => {
      if (method === 'Browser.setDownloadBehavior') {
        policyCalls++;
        if (policyCalls > 1) {
          return new Error('restore failed');
        }
      }
      return undefined;
    });
    const run = runGeneratedScript(harness, flow);
    await assert.rejects(run, /Timed out/);
    assert.ok(harness.trace.includes('browser-close'));
  }

  // Source map step ranges still point at the generated step lines.
  {
    const flow = downloadFlow(dir);
    const code = await stringify(flow);
    const map = parseSourceMap(code)!;
    const lines = stripSourceMap(code).split('\n');
    const [, start0, length0, start1, length1] = map;
    const step0Lines = lines.slice(start0, start0 + length0);
    const step1Lines = lines.slice(start1, start1 + length1);
    assert.ok(step0Lines.some((line) => line.includes('waitForDownload(')));
    assert.ok(step0Lines.some((line) => line.includes(DOWNLOAD_URL)));
    assert.ok(step1Lines.some((line) => line.includes('waitForFunction')));
  }

  // Firefox exports reject the download configuration at generation time
  // (both downloadPath alone and a download asserted event).
  for (const firefoxFlow of [
    {
      title: 'ff',
      downloadPath: dir,
      steps: [],
    } as unknown as UserFlow,
    downloadFlow(dir),
  ]) {
    await assert.rejects(
      stringify(firefoxFlow, {
        extension: new PuppeteerStringifyExtension('firefox'),
      }),
      /Firefox/
    );
  }

  await rm(dir, { recursive: true, force: true });
});
