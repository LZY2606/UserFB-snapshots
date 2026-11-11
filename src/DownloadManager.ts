/**
 * @license
 * Copyright 2022 Google LLC
 * SPDX-License-Identifier: Apache-2.0
 */

import { access, mkdir, stat } from 'node:fs/promises';
import { join } from 'node:path';
import type { Browser, CDPSession, Frame, Page } from 'puppeteer';
import { CDPSessionEvent } from 'puppeteer';
import type { CdpFrame as InternalFrame } from 'puppeteer-core/internal/cdp/Frame.js';
import { AssertedEventType, StepType } from './Schema.js';
import type { DownloadEvent, Step, UserFlow } from './Schema.js';

/**
 * A download that has been fully settled: Chrome reported it as `completed`
 * and the GUID-named file is a readable regular file inside the flow's
 * download directory.
 *
 * @public
 */
export interface CompletedDownload {
  /**
   * The browser-assigned GUID of the download. It is also the file name
   * inside `downloadPath`.
   */
  guid: string;
  /**
   * The exact resource URL reported by `Browser.downloadWillBegin`.
   */
  url: string;
  /**
   * The filename suggested by the server. It is metadata only and is never
   * used as a path on disk.
   */
  suggestedFilename: string;
  /**
   * Absolute path of the readable downloaded file (`downloadPath/GUID`).
   */
  path: string;
}

const WILL_BEGIN_EVENT = 'Browser.downloadWillBegin';
const PROGRESS_EVENT = 'Browser.downloadProgress';

interface DownloadWillBeginEvent {
  frameId: string;
  guid: string;
  url: string;
  suggestedFilename: string;
}

interface DownloadProgressEvent {
  guid: string;
  state: 'inProgress' | 'completed' | 'canceled';
  totalBytes?: number;
  receivedBytes?: number;
}

/**
 * Returns the download assertion of a click/doubleClick step after
 * validating that the configuration is executable. This is the runtime
 * counterpart of the parser checks and protects callers that build steps
 * programmatically instead of going through {@link parse}.
 *
 * @internal
 */
export function getDownloadDeclaration(
  step: Step,
  flow: UserFlow | undefined
): DownloadEvent | null {
  const downloads = step.assertedEvents?.filter(
    (candidate): candidate is DownloadEvent =>
      candidate.type === AssertedEventType.Download
  );
  if (!downloads || downloads.length === 0) {
    return null;
  }
  if (downloads.length > 1) {
    throw new Error('A step can assert at most one download event');
  }
  const event = downloads[0]!;
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
      (candidate) => candidate.type === AssertedEventType.Navigation
    )
  ) {
    throw new Error(
      'Download asserted events cannot be combined with navigation events'
    );
  }
  if (!flow) {
    throw new Error(
      'Download asserted events require a full flow run; single step execution is not supported'
    );
  }
  if (!flow.downloadPath) {
    throw new Error(
      'Flow with a download asserted event must define an absolute `downloadPath`'
    );
  }
  if (typeof event.url !== 'string' || event.url.length === 0) {
    throw new Error(
      'Download asserted event `url` must be a non-empty string'
    );
  }
  return event;
}

/**
 * Owns the browser-level CDP session and the per-step download correlation
 * state for one {@link PuppeteerRunnerExtension} instance.
 *
 * A configured flow exclusively owns the browser context of the page it
 * runs on: no download policy exists before the flow enters, and the default
 * policy is restored when the flow leaves.
 *
 * @internal
 */
export class DownloadManager {
  #session: CDPSession | null = null;
  #contextId: string | undefined;
  #downloadPath: string | null = null;
  #disconnectListener: (() => void) | null = null;
  readonly #activeSteps = new Set<StepDownloadWaiter>();

  get isActive(): boolean {
    return this.#session !== null;
  }

  /**
   * Creates the download directory, attaches a browser-level CDP session and
   * installs the `allowAndName` download policy for the page's browser
   * context.
   */
  async start(downloadPath: string, page: Page): Promise<void> {
    if (this.#session) {
      throw new Error(
        'A download-enabled flow is already running on this extension'
      );
    }
    const browser: Browser = page.browser();
    if (/Firefox/i.test(await browser.version())) {
      throw new Error(
        'Download handoff is only supported in Chrome; Firefox does not provide the required CDP download events'
      );
    }
    await mkdir(downloadPath, { recursive: true });

    const context = page.browserContext();
    const session = await browser.target().createCDPSession();
    const contextId = context.id;
    try {
      await session.send('Browser.setDownloadBehavior', {
        behavior: 'allowAndName',
        downloadPath,
        eventsEnabled: true,
        // When the context has no id (the default context), omitting the
        // parameter targets the default browser context per the CDP spec.
        ...(contextId !== undefined
          ? { browserContextId: contextId }
          : {}),
      });
    } catch (error) {
      await session.detach().catch(() => {});
      throw error;
    }
    this.#session = session;
    this.#contextId = contextId;
    this.#downloadPath = downloadPath;
    this.#disconnectListener = () => {
      const error = new Error(
        'Browser CDP session disconnected before the download settled'
      );
      for (const waiter of this.#activeSteps) {
        waiter.fail(error);
      }
    };
    session.on(CDPSessionEvent.Disconnected, this.#disconnectListener);
  }

  /**
   * Restores the default download policy, removes every listener and
   * detaches the session. Errors raised by teardown are returned so that a
   * caller can surface them when there is no more important error.
   */
  async finish(): Promise<Error | null> {
    const session = this.#session;
    if (!session) {
      return null;
    }
    const contextId = this.#contextId;
    this.#session = null;
    this.#downloadPath = null;
    this.#contextId = undefined;
    if (this.#disconnectListener) {
      session.off(CDPSessionEvent.Disconnected, this.#disconnectListener);
      this.#disconnectListener = null;
    }
    const errors: unknown[] = [];
    try {
      await session.send('Browser.setDownloadBehavior', {
        behavior: 'default',
        eventsEnabled: false,
        ...(contextId !== undefined
          ? { browserContextId: contextId }
          : {}),
      });
    } catch (error) {
      errors.push(error);
    }
    try {
      await session.detach();
    } catch (error) {
      errors.push(error);
    }
    if (errors.length) {
      return new Error('Download session teardown failed', {
        cause: errors[0],
      });
    }
    return null;
  }

  /**
   * Runs a locator action and waits for its correlated download, treating
   * the action and the settled file as two independent completion
   * conditions: both have to succeed before the step may complete.
   */
  async runActionWithDownload(options: {
    action: (onAction: () => void, signal: AbortSignal) => Promise<void>;
    frame: Frame;
    event: DownloadEvent;
    timeout: number;
  }): Promise<CompletedDownload> {
    const session = this.#session;
    const downloadPath = this.#downloadPath;
    if (!session || !downloadPath) {
      throw new Error(
        'Download session is not established; run the step as part of a full flow'
      );
    }
    const frameId = (options.frame as unknown as InternalFrame)._id;
    const waiter = new StepDownloadWaiter({
      session,
      downloadPath,
      frameId,
      event: options.event,
      timeout: options.timeout,
    });
    this.#activeSteps.add(waiter);
    try {
      return await waiter.run(options.action);
    } finally {
      this.#activeSteps.delete(waiter);
    }
  }
}

interface StepDownloadWaiterOptions {
  session: CDPSession;
  downloadPath: string;
  frameId: string;
  event: DownloadEvent;
  timeout: number;
}

class StepDownloadWaiter {
  #session: CDPSession;
  #downloadPath: string;
  #frameId: string;
  #event: DownloadEvent;
  #timeout: number;

  #settled = false;
  #actionStarted = false;
  #actionSettled = false;
  #claimedGuid: string | null = null;
  #beginEvent: DownloadWillBeginEvent | null = null;
  #downloadSettled = false;
  #completedDownload: CompletedDownload | null = null;
  #cancelSent = false;
  #browserCanceled = false;
  #timeoutId: NodeJS.Timeout | null = null;
  readonly #abortController = new AbortController();

  #resolve!: (result: CompletedDownload) => void;
  #reject!: (error: Error) => void;
  readonly #promise: Promise<CompletedDownload>;

  constructor(options: StepDownloadWaiterOptions) {
    this.#session = options.session;
    this.#downloadPath = options.downloadPath;
    this.#frameId = options.frameId;
    this.#event = options.event;
    this.#timeout = options.timeout;
    this.#promise = new Promise((resolve, reject) => {
      this.#resolve = resolve;
      this.#reject = reject;
    });
  }

  async run(
    action: (onAction: () => void, signal: AbortSignal) => Promise<void>
  ): Promise<CompletedDownload> {
    this.#session.on(
      WILL_BEGIN_EVENT,
      this.#onWillBegin as (...args: unknown[]) => void
    );
    this.#session.on(
      PROGRESS_EVENT,
      this.#onProgress as (...args: unknown[]) => void
    );
    // One total budget for action + download, starting before the action.
    // Begin/progress events never reset it.
    this.#timeoutId = setTimeout(() => {
      this.#fail(
        new Error(
          `Timed out after ${this.#timeout} ms waiting for download ${this.#event.url}`
        )
      );
    }, this.#timeout);

    let actionPromise: Promise<void>;
    try {
      actionPromise = action(
        () => {
          this.#actionStarted = true;
        },
        this.#abortController.signal
      );
    } catch (error) {
      actionPromise = Promise.reject(error);
    }
    actionPromise.then(
      () => {
        this.#actionSettled = true;
        this.#maybeResolve();
      },
      (error: unknown) => {
        this.#actionSettled = true;
        this.#fail(
          error instanceof Error
            ? error
            : new Error('Locator action failed', { cause: error })
        );
      }
    );

    try {
      return await this.#promise;
    } finally {
      this.#removeListeners();
      if (this.#timeoutId !== null) {
        clearTimeout(this.#timeoutId);
        this.#timeoutId = null;
      }
      // Catch the action's late settlement: the step has already settled
      // and no late success or progress may publish a second result.
      await actionPromise.catch(() => {});
    }
  }

  fail(error: Error): void {
    this.#fail(error);
  }

  #onWillBegin = (event: DownloadWillBeginEvent): void => {
    if (this.#settled || !this.#actionStarted) {
      return;
    }
    // Only the first begin matching the action's frame ID and the exact
    // resource URL may claim this step. Duplicate or late events can never
    // replace the claimed GUID.
    if (this.#claimedGuid !== null) {
      return;
    }
    if (event.frameId !== this.#frameId || event.url !== this.#event.url) {
      return;
    }
    this.#claimedGuid = event.guid;
    this.#beginEvent = event;
  };

  #onProgress = (event: DownloadProgressEvent): void => {
    if (this.#settled) {
      return;
    }
    if (!this.#claimedGuid || event.guid !== this.#claimedGuid) {
      return;
    }
    if (event.state === 'canceled') {
      // The browser itself already finished the download as canceled; it is
      // no longer pending and must not be canceled again.
      this.#browserCanceled = true;
      this.#fail(
        new Error(
          `Download ${this.#claimedGuid} was canceled by the browser`
        )
      );
      return;
    }
    if (event.state === 'completed') {
      // Equal byte counts alone never settle the step; only the explicit
      // `completed` state for the claimed GUID does.
      void this.#settleCompletedDownload();
    }
  };

  async #settleCompletedDownload(): Promise<void> {
    const guid = this.#claimedGuid;
    const begin = this.#beginEvent;
    if (!guid || !begin || this.#downloadSettled) {
      return;
    }
    this.#downloadSettled = true;
    const filePath = join(this.#downloadPath, guid);
    let stats;
    try {
      stats = await stat(filePath);
    } catch (error) {
      this.#fail(
        new Error(`Settled download is not readable at ${filePath}`, {
          cause: error,
        })
      );
      return;
    }
    if (!stats.isFile()) {
      this.#fail(
        new Error(`Download path ${filePath} is not a regular file`)
      );
      return;
    }
    try {
      await access(filePath);
    } catch (error) {
      this.#fail(
        new Error(`Settled download is not readable at ${filePath}`, {
          cause: error,
        })
      );
      return;
    }
    this.#completedDownload = {
      guid,
      url: begin.url,
      suggestedFilename: begin.suggestedFilename,
      path: filePath,
    };
    this.#maybeResolve();
  }

  #maybeResolve(): void {
    if (
      this.#settled ||
      !this.#actionSettled ||
      !this.#downloadSettled ||
      !this.#completedDownload
    ) {
      return;
    }
    this.#settled = true;
    this.#resolve(this.#completedDownload);
  }

  #fail(error: Error): void {
    if (this.#settled) {
      return;
    }
    this.#settled = true;
    this.#removeListeners();
    if (this.#timeoutId !== null) {
      clearTimeout(this.#timeoutId);
      this.#timeoutId = null;
    }
    // Terminate the still-running locator action. Its rejection is caught
    // in `run` and cannot overwrite this failure.
    if (!this.#actionSettled) {
      this.#abortController.abort();
    }
    // At most one cancel, and only for the download this step actually
    // claimed while it is still pending. Unclaimed downloads belong to
    // somebody else; an already browser-canceled download is not pending.
    if (this.#claimedGuid && !this.#cancelSent && !this.#browserCanceled) {
      this.#cancelSent = true;
      void this.#session
        .send('Browser.cancelDownload', { guid: this.#claimedGuid })
        .catch(() => {});
    }
    this.#reject(error);
  }

  #removeListeners(): void {
    this.#session.off(
      WILL_BEGIN_EVENT,
      this.#onWillBegin as (...args: unknown[]) => void
    );
    this.#session.off(
      PROGRESS_EVENT,
      this.#onProgress as (...args: unknown[]) => void
    );
  }
}
