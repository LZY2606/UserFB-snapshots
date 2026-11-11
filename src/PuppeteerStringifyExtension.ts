/**
 * @license
 * Copyright 2022 Google LLC
 * SPDX-License-Identifier: Apache-2.0
 */

import type { LineWriter } from './LineWriter.js';
import type {
  ChangeStep,
  ClickStep,
  CloseStep,
  DoubleClickStep,
  DownloadEvent,
  EmulateNetworkConditionsStep,
  HoverStep,
  KeyDownStep,
  KeyUpStep,
  NavigateStep,
  ScrollStep,
  SetViewportStep,
  Step,
  StepWithFrame,
  StepWithSelectors,
  UserFlow,
  WaitForElementStep,
  WaitForExpressionStep,
} from './Schema.js';
import { AssertedEventType, StepType } from './Schema.js';
import { StringifyExtension } from './StringifyExtension.js';

import {
  assertAllStepTypesAreHandled,
  mouseButtonMap,
  selectorToPElementSelector,
} from './SchemaUtils.js';
import { formatJSONAsJS } from './JSONUtils.js';

type TargetBrowser = 'chrome' | 'firefox';

function getDownloadEvent(step: Step): DownloadEvent | undefined {
  return step.assertedEvents?.find(
    (event): event is DownloadEvent =>
      event.type === AssertedEventType.Download
  );
}

function flowHasDownload(flow: UserFlow): boolean {
  return flow.steps.some((step) => getDownloadEvent(step) !== undefined);
}

export class PuppeteerStringifyExtension extends StringifyExtension {
  #shouldAppendWaitForElementHelper = false;
  #targetBrowser: TargetBrowser;

  constructor(targetBrowser: TargetBrowser = 'chrome') {
    super();
    this.#targetBrowser = targetBrowser;
  }

  override async beforeAllSteps(out: LineWriter, flow: UserFlow) {
    out.appendLine(
      flow.downloadPath
        ? "import { Locator, launch, CDPSessionEvent } from 'puppeteer'; // v25.0.0 or later"
        : "import { Locator, launch } from 'puppeteer'; // v25.0.0 or later"
    );
    if (flow.downloadPath) {
      out.appendLine(
        "import { mkdir, stat, access } from 'node:fs/promises';"
      );
      out.appendLine("import { join } from 'node:path';");
    }
    out.appendLine('');
    if (this.#targetBrowser === 'firefox') {
      if (flow.downloadPath || flowHasDownload(flow)) {
        throw new Error(
          'Download handoff is only supported in Chrome; Firefox exports cannot use downloadPath or download asserted events'
        );
      }
      out.appendLine(`const browser = await launch({browser: 'firefox'});`);
    } else {
      out.appendLine('const browser = await launch();');
    }
    out.appendLine('const page = await browser.newPage();');
    out.appendLine(`const timeout = ${flow.timeout || defaultTimeout};`);
    out.appendLine('page.setDefaultTimeout(timeout);');
    if (flow.downloadPath) {
      out.appendLine(`const downloadPath = ${formatJSONAsJS(
        flow.downloadPath,
        out.getIndent()
      )};`);
      out.appendLine('const completedDownloads = [];');
      out.appendLine(
        'const downloadSession = await startDownloadSession(browser, page, downloadPath);'
      );
      out.appendLine('try {').startBlock();
    }
    out.appendLine('');
    this.#shouldAppendWaitForElementHelper = false;
  }

  override async afterAllSteps(out: LineWriter, flow: UserFlow) {
    if (flow.downloadPath) {
      // A main failure wins; a cleanup-only failure still surfaces, and
      // the self-created browser is closed on every path.
      out.endBlock().appendLine('} catch (error) {').startBlock();
      out.appendLine('try {').startBlock();
      out.appendLine('await finishDownloadSession(downloadSession);');
      out.endBlock().appendLine('} catch {').startBlock();
      out.appendLine(
        '// Cleanup failure must not overwrite the main failure.'
      );
      out.endBlock().appendLine('}');
      out.appendLine('await browser.close();');
      out.appendLine('throw error;');
      out.endBlock().appendLine('}');
      out.appendLine('try {').startBlock();
      out.appendLine('await finishDownloadSession(downloadSession);');
      out.endBlock().appendLine('} finally {').startBlock();
      out.appendLine('await browser.close();');
      out.endBlock().appendLine('}');
    } else {
      out.appendLine('');
      out.appendLine('await browser.close();');
    }
    out.appendLine('');
    if (flow.downloadPath) {
      for (const line of downloadHelper.split('\n')) {
        out.appendLine(line);
      }
      out.appendLine('');
    }
    if (this.#shouldAppendWaitForElementHelper) {
      for (const line of waitForElementHelper.split('\n')) {
        out.appendLine(line);
      }
    }
  }

  override async stringifyStep(out: LineWriter, step: Step, flow?: UserFlow) {
    const downloadEvents =
      step.assertedEvents?.filter(
        (event): event is DownloadEvent =>
          event.type === AssertedEventType.Download
      ) ?? [];
    if (downloadEvents.length > 1) {
      throw new Error('A step can assert at most one download event');
    }
    const downloadEvent = downloadEvents[0];
    if (downloadEvent) {
      if (!flow) {
        throw new Error(
          'Download asserted events require the full flow resource management and cannot be stringified as a single step; use stringify()'
        );
      }
      if (this.#targetBrowser === 'firefox') {
        throw new Error(
          'Download asserted events are not supported in Firefox exports'
        );
      }
      if (
        step.type !== StepType.Click &&
        step.type !== StepType.DoubleClick
      ) {
        throw new Error(
          `Download asserted events are only supported on ${StepType.Click} and ${StepType.DoubleClick} steps`
        );
      }
      if (
        step.assertedEvents?.some(
          (event) => event.type === AssertedEventType.Navigation
        )
      ) {
        throw new Error(
          'Download asserted events cannot be combined with navigation events'
        );
      }
      if (!flow.downloadPath) {
        throw new Error(
          'Flow with a download asserted event must define an absolute `downloadPath`'
        );
      }
      this.#appendDownloadStep(out, step, downloadEvent);
      return;
    }
    out.appendLine('{').startBlock();
    if (step.timeout !== undefined) {
      out.appendLine(`const timeout = ${step.timeout};`);
    }
    this.#appendContext(out, step);
    const waitForEvents =
      step.assertedEvents && step.type !== StepType.Navigate;
    if (waitForEvents) {
      out.appendLine('const promises = [];');
      out.appendLine('const startWaitingForEvents = () => {').startBlock();
      for (const event of step.assertedEvents!) {
        switch (event.type) {
          case AssertedEventType.Navigation: {
            out.appendLine(
              `promises.push(${
                'frame' in step && step.frame ? 'frame' : 'targetPage'
              }.waitForNavigation());`
            );
            break;
          }
          default:
            throw new Error(`Event type ${event.type} is not supported`);
        }
      }
      out.endBlock().appendLine('}');
    }

    this.#appendStepType(out, step);

    if (waitForEvents) {
      out.appendLine('await Promise.all(promises);');
    }

    out.endBlock().appendLine('}');
  }

  #appendDownloadStep(
    out: LineWriter,
    step: ClickStep | DoubleClickStep,
    event: DownloadEvent
  ): void {
    out.appendLine('{').startBlock();
    if (step.timeout !== undefined) {
      out.appendLine(`const timeout = ${step.timeout};`);
    }
    this.#appendContext(out, step);
    const frameExpression = step.frame ? 'frame' : 'targetPage.mainFrame()';
    out.appendLine('const downloadResult = await waitForDownload({')
      .startBlock();
    out.appendLine('session: downloadSession.session,');
    out.appendLine('downloadPath,');
    out.appendLine(`frameId: ${frameExpression}._id,`);
    out.appendLine(
      `url: ${formatJSONAsJS(event.url, out.getIndent())},`
    );
    out.appendLine('timeout,');
    out.appendLine('action: async (onAction, signal) => {').startBlock();
    out.appendLine('return await Locator.race([').startBlock();
    out.appendLine(
      step.selectors
        .map((s) => {
          return `${
            step.frame ? 'frame' : 'targetPage'
          }.locator(${formatJSONAsJS(
            selectorToPElementSelector(s),
            out.getIndent()
          )})`;
        })
        .join(',\n')
    );
    out.endBlock().appendLine('])');
    out.startBlock().appendLine('.setTimeout(timeout)');
    out.appendLine(`.on('action', () => onAction())`);
    if (step.type === StepType.DoubleClick) {
      out.appendLine('.click({').startBlock();
      out.appendLine('signal,');
      out.appendLine('count: 2,');
      if (step.duration) {
        out.appendLine(`delay: ${step.duration},`);
      }
      if (step.button) {
        out.appendLine(
          `button: '${mouseButtonMap.get(step.button)}',`
        );
      }
    } else {
      out.appendLine('.click({').startBlock();
      out.appendLine('signal,');
      if (step.duration) {
        out.appendLine(`delay: ${step.duration},`);
      }
      if (step.button) {
        out.appendLine(
          `button: '${mouseButtonMap.get(step.button)}',`
        );
      }
    }
    out.appendLine('offset: {');
    out.appendLine(`  x: ${step.offsetX},`);
    out.appendLine(`  y: ${step.offsetY},`);
    out.appendLine('},');
    out.endBlock().appendLine('});');
    out.endBlock();
    out.endBlock().appendLine('},');
    out.endBlock().appendLine('});');
    out.appendLine('completedDownloads.push(downloadResult);');
    out.endBlock().appendLine('}');
  }

  #appendTarget(out: LineWriter, target: string): void {
    if (target === 'main') {
      out.appendLine('const targetPage = page;');
    } else {
      out.appendLine(
        `const target = await browser.waitForTarget(t => t.url() === ${formatJSONAsJS(
          target,
          out.getIndent()
        )}, { timeout });`
      );
      out.appendLine('const targetPage = await target.page();');
      out.appendLine('targetPage.setDefaultTimeout(timeout);');
    }
  }

  #appendFrame(out: LineWriter, path: number[]): void {
    out.appendLine('let frame = targetPage.mainFrame();');
    for (const index of path) {
      out.appendLine(`frame = frame.childFrames()[${index}];`);
    }
  }

  #appendContext(out: LineWriter, step: StepWithFrame): void {
    // TODO fix optional target: should it be main?
    this.#appendTarget(out, step.target || 'main');
    // TODO fix optional frame: should it be required?
    if (step.frame) {
      this.#appendFrame(out, step.frame);
    }
  }

  #appendLocators(
    out: LineWriter,
    step: StepWithSelectors,
    action: () => void
  ) {
    out.appendLine('await Locator.race([').startBlock();
    out.appendLine(
      step.selectors
        .map((s) => {
          return `${
            step.frame ? 'frame' : 'targetPage'
          }.locator(${formatJSONAsJS(
            selectorToPElementSelector(s),
            out.getIndent()
          )})`;
        })
        .join(',\n')
    );
    out.endBlock().appendLine('])');
    out.startBlock().appendLine('.setTimeout(timeout)');
    if (step.assertedEvents?.length) {
      out.appendLine(`.on('action', () => startWaitingForEvents())`);
    }
    action();
    out.endBlock();
  }

  #appendClickStep(out: LineWriter, step: ClickStep): void {
    this.#appendLocators(out, step, () => {
      out.appendLine('.click({');
      if (step.duration) {
        out.appendLine(`  delay: ${step.duration},`);
      }
      if (step.button) {
        out.appendLine(`  button: '${mouseButtonMap.get(step.button)}',`);
      }
      out.appendLine('  offset: {');
      out.appendLine(`    x: ${step.offsetX},`);
      out.appendLine(`    y: ${step.offsetY},`);
      out.appendLine('  },');
      out.appendLine('});');
    });
  }

  #appendDoubleClickStep(out: LineWriter, step: DoubleClickStep): void {
    this.#appendLocators(out, step, () => {
      out.appendLine('.click({');
      out.appendLine(`  count: 2,`);
      if (step.duration) {
        out.appendLine(`  delay: ${step.duration},`);
      }
      if (step.button) {
        out.appendLine(`  button: '${mouseButtonMap.get(step.button)}',`);
      }
      out.appendLine('  offset: {');
      out.appendLine(`    x: ${step.offsetX},`);
      out.appendLine(`    y: ${step.offsetY},`);
      out.appendLine('  },');
      out.appendLine('});');
    });
  }

  #appendHoverStep(out: LineWriter, step: HoverStep): void {
    this.#appendLocators(out, step, () => {
      out.appendLine('.hover();');
    });
  }

  #appendChangeStep(out: LineWriter, step: ChangeStep): void {
    this.#appendLocators(out, step, () => {
      out.appendLine(`.fill(${formatJSONAsJS(step.value, out.getIndent())});`);
    });
  }

  #appendEmulateNetworkConditionsStep(
    out: LineWriter,
    step: EmulateNetworkConditionsStep
  ): void {
    out.appendLine('await targetPage.emulateNetworkConditions({');
    out.appendLine(`  offline: ${!step.download && !step.upload},`);
    out.appendLine(`  downloadThroughput: ${step.download},`);
    out.appendLine(`  uploadThroughput: ${step.upload},`);
    out.appendLine(`  latency: ${step.latency},`);
    out.appendLine('});');
  }

  #appendKeyDownStep(out: LineWriter, step: KeyDownStep): void {
    out.appendLine(
      `await targetPage.keyboard.down(${formatJSONAsJS(
        step.key,
        out.getIndent()
      )});`
    );
  }

  #appendKeyUpStep(out: LineWriter, step: KeyUpStep): void {
    out.appendLine(
      `await targetPage.keyboard.up(${formatJSONAsJS(
        step.key,
        out.getIndent()
      )});`
    );
  }

  #appendCloseStep(out: LineWriter, step: CloseStep): void {
    out.appendLine('await targetPage.close()');
  }

  #appendViewportStep(out: LineWriter, step: SetViewportStep): void {
    out.appendLine(
      `await targetPage.setViewport(${formatJSONAsJS(
        {
          width: step.width,
          height: step.height,
        },
        out.getIndent()
      )})`
    );
  }

  #appendScrollStep(out: LineWriter, step: ScrollStep): void {
    if ('selectors' in step) {
      this.#appendLocators(out, step, () => {
        out.appendLine(
          `.scroll({ scrollTop: ${step.y}, scrollLeft: ${step.x}});`
        );
      });
    } else {
      out.appendLine(
        `await targetPage.evaluate((x, y) => { window.scroll(x, y); }, ${step.x}, ${step.y})`
      );
    }
  }

  #appendStepType(out: LineWriter, step: Step): void {
    switch (step.type) {
      case StepType.Click:
        return this.#appendClickStep(out, step);
      case StepType.DoubleClick:
        return this.#appendDoubleClickStep(out, step);
      case StepType.Hover:
        return this.#appendHoverStep(out, step);
      case StepType.Change:
        return this.#appendChangeStep(out, step);
      case StepType.EmulateNetworkConditions:
        return this.#appendEmulateNetworkConditionsStep(out, step);
      case StepType.KeyDown:
        return this.#appendKeyDownStep(out, step);
      case StepType.KeyUp:
        return this.#appendKeyUpStep(out, step);
      case StepType.Close:
        return this.#appendCloseStep(out, step);
      case StepType.SetViewport:
        return this.#appendViewportStep(out, step);
      case StepType.Scroll:
        return this.#appendScrollStep(out, step);
      case StepType.Navigate:
        return this.#appendNavigationStep(out, step);
      case StepType.WaitForElement:
        return this.#appendWaitForElementStep(out, step);
      case StepType.WaitForExpression:
        return this.#appendWaitExpressionStep(out, step);
      case StepType.CustomStep:
        return; // TODO: implement these
      default:
        return assertAllStepTypesAreHandled(step);
    }
  }

  #appendNavigationStep(out: LineWriter, step: NavigateStep): void {
    out.appendLine(
      `await targetPage.goto(${formatJSONAsJS(step.url, out.getIndent())});`
    );
  }

  #appendWaitExpressionStep(
    out: LineWriter,
    step: WaitForExpressionStep
  ): void {
    out.appendLine(
      `await ${
        step.frame ? 'frame' : 'targetPage'
      }.waitForFunction(${formatJSONAsJS(
        step.expression,
        out.getIndent()
      )}, { timeout });`
    );
  }

  #appendWaitForElementStep(out: LineWriter, step: WaitForElementStep): void {
    this.#shouldAppendWaitForElementHelper = true;
    out.appendLine(
      `await waitForElement(${formatJSONAsJS(step, out.getIndent())}, ${
        step.frame ? 'frame' : 'targetPage'
      }, timeout);`
    );
  }
}

const defaultTimeout = 5000;

/**
 * Self-contained runtime emitted into exported scripts. It intentionally
 * does not import any private Replay helper; the correlation and settlement
 * rules here must stay identical to the ones implemented by
 * `DownloadManager` for direct replay.
 */
const downloadHelper = `async function startDownloadSession(browser, page, downloadPath) {
  await mkdir(downloadPath, { recursive: true });
  const session = await browser.target().createCDPSession();
  const contextId = page.browserContext().id;
  await session.send('Browser.setDownloadBehavior', {
    behavior: 'allowAndName',
    downloadPath,
    eventsEnabled: true,
    ...(contextId !== undefined
      ? { browserContextId: contextId }
      : {}),
  });
  return { session, contextId };
}

async function finishDownloadSession(downloadSession) {
  const { session, contextId } = downloadSession;
  try {
    await session.send('Browser.setDownloadBehavior', {
      behavior: 'default',
      eventsEnabled: false,
      ...(contextId !== undefined
        ? { browserContextId: contextId }
        : {}),
    });
  } finally {
    await session.detach();
  }
}

async function waitForDownload({
  session,
  downloadPath,
  frameId,
  url,
  timeout,
  action,
}) {
  const abortController = new AbortController();
  let settled = false;
  let actionStarted = false;
  let actionSettled = false;
  let claimedGuid = null;
  let beginEvent = null;
  let downloadSettled = false;
  let completedDownload = null;
  let cancelSent = false;
  let browserCanceled = false;
  let timeoutId;
  let resolveDone;
  let rejectDone;
  const done = new Promise((resolve, reject) => {
    resolveDone = resolve;
    rejectDone = reject;
  });

  const removeListeners = () => {
    session.off('Browser.downloadWillBegin', onWillBegin);
    session.off('Browser.downloadProgress', onProgress);
    session.off(CDPSessionEvent.Disconnected, onDisconnected);
  };

  const fail = error => {
    if (settled) {
      return;
    }
    settled = true;
    removeListeners();
    clearTimeout(timeoutId);
    if (!actionSettled) {
      abortController.abort();
    }
    if (claimedGuid && !cancelSent && !browserCanceled) {
      cancelSent = true;
      session
        .send('Browser.cancelDownload', { guid: claimedGuid })
        .catch(() => {});
    }
    rejectDone(error);
  };

  const onDisconnected = () =>
    fail(
      new Error(
        'Browser CDP session disconnected before the download settled'
      )
    );

  const onWillBegin = event => {
    if (settled || !actionStarted || claimedGuid !== null) {
      return;
    }
    if (event.frameId !== frameId || event.url !== url) {
      return;
    }
    claimedGuid = event.guid;
    beginEvent = event;
  };

  const settleCompletedDownload = async () => {
    if (!claimedGuid || !beginEvent || downloadSettled) {
      return;
    }
    downloadSettled = true;
    const filePath = join(downloadPath, claimedGuid);
    let stats;
    try {
      stats = await stat(filePath);
    } catch (error) {
      fail(
        new Error(\`Settled download is not readable at \${filePath}\`, {
          cause: error,
        })
      );
      return;
    }
    if (!stats.isFile()) {
      fail(
        new Error(\`Download path \${filePath} is not a regular file\`)
      );
      return;
    }
    try {
      await access(filePath);
    } catch (error) {
      fail(
        new Error(\`Settled download is not readable at \${filePath}\`, {
          cause: error,
        })
      );
      return;
    }
    completedDownload = {
      guid: claimedGuid,
      url: beginEvent.url,
      suggestedFilename: beginEvent.suggestedFilename,
      path: filePath,
    };
    maybeResolve();
  };

  const onProgress = event => {
    if (settled || !claimedGuid || event.guid !== claimedGuid) {
      return;
    }
    if (event.state === 'canceled') {
      // The browser already finished the download as canceled; it is not
      // pending and must not be canceled again.
      browserCanceled = true;
      fail(
        new Error(
          \`Download \${claimedGuid} was canceled by the browser\`
        )
      );
      return;
    }
    if (event.state === 'completed') {
      void settleCompletedDownload();
    }
  };

  const maybeResolve = () => {
    if (
      settled ||
      !actionSettled ||
      !downloadSettled ||
      !completedDownload
    ) {
      return;
    }
    settled = true;
    resolveDone(completedDownload);
  };

  session.on('Browser.downloadWillBegin', onWillBegin);
  session.on('Browser.downloadProgress', onProgress);
  session.on(CDPSessionEvent.Disconnected, onDisconnected);
  // One total budget for action + download, starting before the action.
  timeoutId = setTimeout(
    () =>
      fail(
        new Error(
          \`Timed out after \${timeout} ms waiting for download \${url}\`
        )
      ),
    timeout
  );

  let actionPromise;
  try {
    actionPromise = action(
      () => {
        actionStarted = true;
      },
      abortController.signal
    );
  } catch (error) {
    actionPromise = Promise.reject(error);
  }
  actionPromise.then(
    () => {
      actionSettled = true;
      maybeResolve();
    },
    error => {
      actionSettled = true;
      fail(
        error instanceof Error
          ? error
          : new Error('Locator action failed', { cause: error })
      );
    }
  );

  try {
    return await done;
  } finally {
    removeListeners();
    clearTimeout(timeoutId);
    await actionPromise.catch(() => {});
  }
}`;

const waitForElementHelper = `async function waitForElement(step, frame, timeout) {
  const {
    count = 1,
    operator = '>=',
    visible = true,
    properties,
    attributes,
  } = step;
  const compFn = {
    '==': (a, b) => a === b,
    '>=': (a, b) => a >= b,
    '<=': (a, b) => a <= b,
  }[operator];
  await waitForFunction(async () => {
    const elements = await querySelectorsAll(step.selectors, frame);
    let result = compFn(elements.length, count);
    const elementsHandle = await frame.evaluateHandle((...elements) => {
      return elements;
    }, ...elements);
    await Promise.all(elements.map((element) => element.dispose()));
    if (result && (properties || attributes)) {
      result = await elementsHandle.evaluate(
        (elements, properties, attributes) => {
          for (const element of elements) {
            if (attributes) {
              for (const [name, value] of Object.entries(attributes)) {
                if (element.getAttribute(name) !== value) {
                  return false;
                }
              }
            }
            if (properties) {
              if (!isDeepMatch(properties, element)) {
                return false;
              }
            }
          }
          return true;

          function isDeepMatch(a, b) {
            if (a === b) {
              return true;
            }
            if ((a && !b) || (!a && b)) {
              return false;
            }
            if (!(a instanceof Object) || !(b instanceof Object)) {
              return false;
            }
            for (const [key, value] of Object.entries(a)) {
              if (!isDeepMatch(value, b[key])) {
                return false;
              }
            }
            return true;
          }
        },
        properties,
        attributes
      );
    }
    await elementsHandle.dispose();
    return result === visible;
  }, timeout);
}

async function querySelectorsAll(selectors, frame) {
  for (const selector of selectors) {
    const result = await querySelectorAll(selector, frame);
    if (result.length) {
      return result;
    }
  }
  return [];
}

async function querySelectorAll(selector, frame) {
  if (!Array.isArray(selector)) {
    selector = [selector];
  }
  if (!selector.length) {
    throw new Error('Empty selector provided to querySelectorAll');
  }
  let elements = [];
  for (let i = 0; i < selector.length; i++) {
    const part = selector[i];
    if (i === 0) {
      elements = await frame.$$(part);
    } else {
      const tmpElements = elements;
      elements = [];
      for (const el of tmpElements) {
        elements.push(...(await el.$$(part)));
      }
    }
    if (elements.length === 0) {
      return [];
    }
    if (i < selector.length - 1) {
      const tmpElements = [];
      for (const el of elements) {
        const newEl = (await el.evaluateHandle(el => el.shadowRoot ? el.shadowRoot : el)).asElement();
        if (newEl) {
          tmpElements.push(newEl);
        }
      }
      elements = tmpElements;
    }
  }
  return elements;
}

async function waitForFunction(fn, timeout) {
  let isActive = true;
  const timeoutId = setTimeout(() => {
    isActive = false;
  }, timeout);
  while (isActive) {
    const result = await fn();
    if (result) {
      clearTimeout(timeoutId);
      return;
    }
    await new Promise(resolve => setTimeout(resolve, 100));
  }
  throw new Error('Timed out');
}`;
