import {EventEmitter} from 'node:events';

// A controllable browser boundary, not an implementation of download waiting.
export function boundary() {
  const events = new EventEmitter();
  const trace = [];
  let complete = false;
  const frame = {
    _id: 'frame-report',
    childFrames: () => [],
    _client: () => ({send: async () => {}}),
    locator(selector) { return {selector}; },
    async waitForFunction() { trace.push(`next-step complete=${complete}`); },
    async goto() {},
  };
  const race = () => {
    const hooks = [];
    return {
      setTimeout() { return this; },
      on(name, fn) { if (name === 'action') hooks.push(fn); return this; },
      async click() {
        trace.push('action');
        for (const fn of hooks) fn();
        events.emit('Browser.downloadWillBegin', {
          guid: 'download-report', frameId: frame._id,
          url: 'https://reports.invalid/export.csv', suggestedFilename: 'report.csv',
        });
        events.emit('Browser.downloadProgress', {
          guid: 'download-report', state: 'inProgress', totalBytes: 20, receivedBytes: 4,
        });
      },
    };
  };
  const session = Object.assign(events, {
    async send(method, params) { trace.push(method); return {}; },
    async detach() { trace.push('detach'); },
  });
  const page = {
    ...frame,
    mainFrame: () => frame,
    locatorRace: race,
    browserContext: () => ({id: 'context-report'}),
    setDefaultTimeout() {},
  };
  const browser = {
    target: () => ({createCDPSession: async () => session}),
    async newPage() { return page; },
    async close() { trace.push('browser-close'); },
  };
  return {
    browser, page, frame, session, trace, Locator: {race},
    finish() {
      complete = true;
      events.emit('Browser.downloadProgress', {
        guid: 'download-report', state: 'completed', totalBytes: 20, receivedBytes: 20,
      });
    },
  };
}
