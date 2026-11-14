'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const events = require('node:events');
const http = require('node:http');
const path = require('node:path');
const {fork} = require('node:child_process');

const Crawler = require('../lib');
const OriginScheduler = require('../lib/origin-scheduler');

const ORIGIN_A = 'A';
const ORIGIN_B = 'B';

function makeRecorder() {
  const requests = [];
  let active = 0;
  let peak = 0;
  const perOrigin = new Map();

  function start(origin, requestPath) {
    const entry = {origin, path: requestPath, startedAt: requests.length};
    requests.push(entry);
    active++;
    peak = Math.max(peak, active);

    if (!perOrigin.has(origin)) {
      perOrigin.set(origin, {active: 0, peak: 0});
    }

    const state = perOrigin.get(origin);
    state.active++;
    state.peak = Math.max(state.peak, state.active);
    return entry;
  }

  function end(origin, requestPath) {
    active--;
    const state = perOrigin.get(origin);
    if (state) {
      state.active--;
    }

    return {origin, path: requestPath};
  }

  return {
    requests,
    start,
    end,
    get peak() {
      return peak;
    },
    get active() {
      return active;
    },
    originPeak(origin) {
      return perOrigin.get(origin) ? perOrigin.get(origin).peak : 0;
    },
    originActive(origin) {
      return perOrigin.get(origin) ? perOrigin.get(origin).active : 0;
    },
    pageRequests() {
      return requests.filter(entry => entry.path !== '/robots.txt');
    },
    reset() {
      requests.length = 0;
      active = 0;
      peak = 0;
      perOrigin.clear();
    }
  };
}

function wait(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

function until(condition, timeoutMs = 5000, intervalMs = 10) {
  const started = Date.now();
  return new Promise((resolve, reject) => {
    (function poll() {
      let result;
      try {
        result = condition();
      } catch (error) {
        return reject(error);
      }

      if (result) {
        return resolve(result);
      }

      if (Date.now() - started > timeoutMs) {
        return reject(new Error('timed out waiting for condition'));
      }

      setTimeout(poll, intervalMs);
    })();
  });
}

// ---------------------------------------------------------------------------
// Real HTTP environment: task_support/origin-server.cjs as a child process
// ---------------------------------------------------------------------------

function startRealEnvironment() {
  const child = fork(path.resolve(__dirname, '../task_support/origin-server.cjs'), [], {
    stdio: ['ignore', 'pipe', 'pipe', 'ipc']
  });

  const recorder = makeRecorder();
  const pending = new Map();

  child.on('message', message => {
    if (message.type === 'request') {
      pending.set(message.id, recorder.start(message.origin, message.path));
    } else if (message.type === 'response-end') {
      const entry = pending.get(message.id);
      pending.delete(message.id);
      if (entry) {
        recorder.end(entry.origin, entry.path);
      }
    }
  });

  const ready = new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('server start timed out')), 5000);
    child.once('exit', code => {
      clearTimeout(timer);
      reject(new Error('origin server exited early with code ' + code));
    });
    child.on('message', message => {
      if (message.type === 'ready') {
        clearTimeout(timer);
        resolve(message.urls);
      }
    });
  });

  function send(message) {
    child.send(message);
  }

  return ready.then(urls => {
    // Tests drive holds themselves; disable the server's baseline behaviour
    // of parking A's robots.txt until an explicit release.
    child.send({type: 'auto-hold', enabled: false});

    return {
      mode: 'http',
      urls: {A: urls[0], B: urls[1]},
      recorder,
      hold(origin, requestPath) {
        send({type: 'hold', origin, path: requestPath});
      },
      release(origin, requestPath) {
        send({type: 'release', origin, path: requestPath});
      },
      delay(origin, requestPath, ms) {
        send({type: 'delay', origin, path: requestPath, ms});
      },
      robotsBody(origin, body) {
        send({type: 'robots-body', origin, body});
      },
      robotsFail(origin) {
        send({type: 'robots-fail', origin});
      },
      destroy(origin, requestPath) {
        send({type: 'destroy', origin, path: requestPath});
      },
      async close() {
        if (child.exitCode === null) {
          child.send({type: 'close'});
          await events.once(child, 'exit');
        }
      }
    };
  });
}

// ---------------------------------------------------------------------------
// In-process fallback environment: stubbed http.request with the same control
// surface and recorder as the real servers. Used where binding local ports is
// not permitted; behaviour under test stays transport-agnostic.
// ---------------------------------------------------------------------------

class FakeResponse extends events.EventEmitter {
  constructor(statusCode, headers, body) {
    super();
    this.statusCode = statusCode;
    this.headers = headers;
    this._body = body;
    this.req = null;
    this._ready = false;
    this._sent = false;
  }

  destroy() {
    if (this.destroyed) {
      return;
    }

    this.destroyed = true;
    this.emit('close');
  }

  // Mirror a real IncomingMessage: the body is only delivered once a consumer
  // has attached a data listener. A delayed queue can push the listener
  // attachment past the response arrival, so the body is buffered until then.
  on(event, listener) {
    const result = super.on(event, listener);

    if (event === 'data' && this._ready && !this._sent) {
      this._send();
    }

    return result;
  }

  addListener(event, listener) {
    return this.on(event, listener);
  }

  deliver() {
    this._ready = true;

    if (this.listenerCount('data') > 0) {
      this._send();
    }
  }

  _send() {
    if (this._sent || this.destroyed) {
      return;
    }

    this._sent = true;
    this.emit('data', this._body);
    // Let the crawler attach its matching `end` listener synchronously.
    setImmediate(() => {
      if (!this.destroyed) {
        this.emit('end');
        this.emit('close');
      }
    });
  }
}
class FakeRequest extends events.EventEmitter {
  constructor(options, onResponse, handler) {
    super();
    this.options = options;
    this.aborted = false;
    this._onTimeout = null;
    setImmediate(() => {
      if (!this.aborted) {
        handler(this, options, onResponse);
      }
    });
  }

  end() {}

  setTimeout(ms, callback) {
    this._onTimeout = callback;
    return this;
  }

  abort() {
    if (this.aborted) {
      return;
    }

    this.aborted = true;
    this.emit('abort');
    this.emit('close');
  }
}

function startFakeEnvironment() {
  const recorder = makeRecorder();
  const ports = {A: 9001, B: 9002};
  const held = new Map();
  const delays = new Map();
  const robotsOverrides = new Map();
  const robotsFailures = new Set();
  const destroyPaths = new Set();

  function requestKey(origin, requestPath) {
    return origin + ' ' + requestPath;
  }

  function makeBody(requestPath) {
    if (requestPath === '/root') {
      return Buffer.from('<a href="/child">child</a>');
    }

    if (requestPath === '/tree') {
      return Buffer.from([
        '<a href="/tree/1">1</a>',
        '<a href="/tree/2">2</a>',
        '<a href="/tree/3">3</a>'
      ].join(''));
    }

    return Buffer.from('leaf');
  }

  function handler(clientRequest, options, onResponse) {
    const origin = Number(options.port) === ports.B ? ORIGIN_B : ORIGIN_A;
    const requestPath = options.path || '/';
    const key = requestKey(origin, requestPath);
    recorder.start(origin, requestPath);

    if (destroyPaths.has(key)) {
      const error = new Error('socket hang up');
      error.code = 'ECONNRESET';
      clientRequest.emit('error', error);
      clientRequest.emit('close');
      recorder.end(origin, requestPath);
      return;
    }

    const respond = () => {
      if (clientRequest.aborted) {
        recorder.end(origin, requestPath);
        return;
      }

      let response;

      if (requestPath === '/robots.txt') {
        if (robotsFailures.has(origin)) {
          response = new FakeResponse(500, {'content-type': 'text/plain'}, Buffer.from('robots failure'));
        } else {
          response = new FakeResponse(200, {'content-type': 'text/plain'},
            Buffer.from(robotsOverrides.get(origin) || 'User-agent: *\nDisallow: /forbidden\n'));
        }
      } else if (requestPath === '/redir') {
        response = new FakeResponse(302, {location: '/leaf'}, Buffer.alloc(0));
      } else {
        const body = makeBody(requestPath);
        response = new FakeResponse(200, {
          'content-type': 'text/html',
          'content-length': String(body.length)
        }, body);
      }

      // The request-level `close` (and thus the recorder's active count and
      // the crawler's reservation release) follows the response to its end,
      // matching a real ClientRequest.
      response.once('close', () => {
        recorder.end(origin, requestPath);

        if (!clientRequest.aborted) {
          clientRequest.emit('close');
        }
      });

      response.req = clientRequest;
      onResponse(response);
      response.deliver();
    };

    if (held.has(key)) {
      held.set(key, respond);
      return;
    }

    const delayMs = delays.get(key) || 0;

    if (delayMs) {
      delays.delete(key);
      setTimeout(respond, delayMs);
    } else {
      setImmediate(respond);
    }
  }

  const originalRequest = http.request;

  http.request = function(options, callback) {
    return new FakeRequest(options, callback, handler);
  };

  return {
    mode: 'fake',
    urls: {
      A: 'http://127.0.0.1:' + ports.A,
      B: 'http://127.0.0.1:' + ports.B
    },
    recorder,
    hold(origin, requestPath) {
      held.set(requestKey(origin, requestPath), null);
    },
    release(origin, requestPath) {
      const key = requestKey(origin, requestPath);
      const respond = held.get(key);
      held.delete(key);
      if (respond) {
        respond();
      }
    },
    delay(origin, requestPath, ms) {
      delays.set(requestKey(origin, requestPath), ms);
    },
    robotsBody(origin, body) {
      robotsOverrides.set(origin, body);
    },
    robotsFail(origin) {
      robotsFailures.add(origin);
    },
    destroy(origin, requestPath) {
      destroyPaths.add(requestKey(origin, requestPath));
    },
    async close() {
      http.request = originalRequest;
      held.forEach(respond => respond && respond());
      held.clear();
    }
  };
}

async function startEnvironment() {
  try {
    return await startRealEnvironment();
  } catch (error) {
    return startFakeEnvironment();
  }
}

function makeCrawler(env, options = {}) {
  const crawler = new Crawler(env.urls.A + (options.initialPath || '/root'));
  crawler.originScheduling = true;
  crawler.maxConcurrency = options.maxConcurrency || 4;
  crawler.maxConcurrencyPerOrigin = options.maxConcurrencyPerOrigin || 1;
  crawler.interval = options.interval || 20;
  crawler.timeout = options.timeout || 5000;

  if (options.queue) {
    crawler.queue = options.queue;
  }

  if (options.respectRobotsTxt === false) {
    crawler.respectRobotsTxt = false;
  }

  crawler.on('crawlstart', () => {
    (options.extraUrls || []).forEach(url => crawler.queueURL(url));
  });

  return crawler;
}

// Queue wrapper that delays every callback and returns copies, emulating a
// database backed queue.
function makeDelayedQueue(delayMs, options = {}) {
  const base = new Crawler.queue();
  const copy = item => item ? JSON.parse(JSON.stringify(item)) : item;
  const deferred = callback => setTimeout(callback, delayMs);

  return {
    add(queueItem, force, callback) {
      base.add(queueItem, force, (error, item) => deferred(() => callback(error, copy(item))));
    },
    exists(url, callback) {
      base.exists(url, (error, count) => deferred(() => callback(error, count)));
    },
    get(index, callback) {
      base.get(index, (error, item) => deferred(() => callback(error, copy(item))));
    },
    getLength(callback) {
      base.getLength((error, length) => deferred(() => callback(error, length)));
    },
    update(id, updates, callback) {
      base.update(id, updates, (error, item) => deferred(() => {
        if (options.failUpdate && options.failUpdate(id, updates)) {
          return callback(new Error('injected queue update failure'), copy(item));
        }

        callback(error, copy(item));
      }));
    },
    countItems(comparator, callback) {
      base.countItems(comparator, (error, count) => deferred(() => callback(error, count)));
    },
    filterItems(comparator, callback) {
      base.filterItems(comparator, (error, items) =>
        deferred(() => callback(error, items.map(copy))));
    },
    oldestUnfetchedItem(callback) {
      base.oldestUnfetchedItem((error, item) =>
        deferred(() => callback(error, copy(item))));
    }
  };
}

async function runToCompletion(crawler) {
  const completed = events.once(crawler, 'complete');
  crawler.start();
  await completed;
  return crawler;
}

function queueStats(queue) {
  return new Promise((resolve, reject) => {
    queue.getLength((error, length) => {
      if (error) {
        return reject(error);
      }

      queue.countItems({fetched: true}, (countError, count) => {
        if (countError) {
          return reject(countError);
        }

        resolve({length, count});
      });
    });
  });
}

// ---------------------------------------------------------------------------
// Group 1: Origin fairness
// ---------------------------------------------------------------------------

test('Origin fairness', async t => {
  await t.test('origin keys collapse default ports and normalise casing', () => {
    const keyOf = item => OriginScheduler.originKeyOf(item);

    assert.equal(
      keyOf({protocol: 'HTTP', host: 'Example.COM', port: 80}),
      keyOf({protocol: 'http', host: 'example.com', port: undefined})
    );
    assert.equal(
      keyOf({protocol: 'https', host: 'example.com', port: 443}),
      keyOf({protocol: 'https', host: 'example.com', port: null})
    );
    assert.notEqual(
      keyOf({protocol: 'http', host: 'example.com', port: 8080}),
      keyOf({protocol: 'http', host: 'example.com', port: 80})
    );
    assert.notEqual(
      keyOf({protocol: 'http', host: 'a.example.com', port: 80}),
      keyOf({protocol: 'http', host: 'b.example.com', port: 80})
    );
  });

  await t.test('rotation order follows first appearance in the queue', () => {
    const decisions = [];
    const scheduler = new OriginScheduler({
      queue: new Crawler.queue(),
      maxConcurrency: 10,
      maxConcurrencyPerOrigin: 1,
      respectRobotsTxt: false,
      execute(decision) {
        decisions.push(decision.origin);
      }
    });

    const item = (origin, id) => ({
      id,
      status: 'queued',
      protocol: 'http',
      host: '127.0.0.1',
      port: origin === 'A' ? 9001 : 9002
    });

    scheduler.noteItem(item('B', 1));
    scheduler.noteItem(item('A', 2));
    scheduler.noteItem(item('B', 3));
    assert.deepEqual(scheduler._order, [
      'http://127.0.0.1:9002',
      'http://127.0.0.1:9001'
    ]);
  });

  await t.test('alternates origins, keeps FIFO per origin and honours both caps', async () => {
    const env = await startEnvironment();
    const crawler = makeCrawler(env, {
      maxConcurrency: 4,
      maxConcurrencyPerOrigin: 1,
      extraUrls: [
        env.urls.B + '/root',
        env.urls.A + '/tree/1',
        env.urls.B + '/child',
        env.urls.A + '/tree/2',
        env.urls.B + '/tree/1'
      ]
    });

    try {
      await runToCompletion(crawler);

      const pages = env.recorder.pageRequests();
      const origins = pages.map(entry => entry.origin);

      // Strict alternation once pages start: no origin takes two slots in a row
      // while the other still has queued work.
      for (let i = 1; i < origins.length; i++) {
        assert.notEqual(origins[i], origins[i - 1],
          'consecutive page requests came from the same origin: ' + origins.join(','));
      }

      // FIFO within each origin, comparing against enqueue order. /child on A
      // is discovered from /root and therefore joins the end of A's queue.
      const orderOf = origin => pages.filter(entry => entry.origin === origin)
        .map(entry => entry.path);

      assert.deepEqual(orderOf('A'), ['/root', '/tree/1', '/tree/2', '/child']);
      assert.deepEqual(orderOf('B'), ['/root', '/child', '/tree/1']);

      assert.ok(env.recorder.peak <= 4, 'global peak ' + env.recorder.peak);
      assert.equal(env.recorder.originPeak('A'), 1);
      assert.equal(env.recorder.originPeak('B'), 1);

      const scheduler = crawler._originScheduler;
      assert.equal(scheduler._slots.size, 0);
      assert.equal(scheduler._reservedIds.size, 0);
      assert.equal(scheduler._states[scheduler._order[0]].active, 0);
    } finally {
      crawler.stop(true);
      await env.close();
    }
  });

  await t.test('a capped origin cannot block origins behind it', async () => {
    const env = await startEnvironment();
    env.hold('A', '/root');

    const crawler = makeCrawler(env, {
      maxConcurrency: 4,
      maxConcurrencyPerOrigin: 1,
      extraUrls: [
        env.urls.B + '/root',
        env.urls.B + '/child',
        env.urls.B + '/tree/1'
      ]
    });

    try {
      crawler.start();

      await until(() => env.recorder.requests.some(entry =>
        entry.origin === 'B' && entry.path === '/tree/1'));

      // A is still holding its single slot...
      assert.equal(env.recorder.originActive('A'), 1);
      // ...yet B cycled through all of its pages.
      assert.deepEqual(
        env.recorder.pageRequests().filter(entry => entry.origin === 'B')
          .map(entry => entry.path),
        ['/root', '/child', '/tree/1']
      );

      env.release('A', '/root');
      await events.once(crawler, 'complete');
    } finally {
      crawler.stop(true);
      await env.close();
    }
  });

  await t.test('per-origin cap above one allows bounded same-origin concurrency', async () => {
    const env = await startEnvironment();
    env.delay('A', '/tree/1', 40);
    env.delay('A', '/tree/2', 40);
    env.delay('A', '/tree/3', 40);

    const crawler = makeCrawler(env, {
      maxConcurrency: 6,
      maxConcurrencyPerOrigin: 3,
      extraUrls: [
        env.urls.A + '/tree/1',
        env.urls.A + '/tree/2',
        env.urls.A + '/tree/3',
        env.urls.B + '/root'
      ]
    });

    try {
      await runToCompletion(crawler);
      assert.equal(env.recorder.originPeak('A'), 3);
      assert.ok(env.recorder.peak <= 6);
    } finally {
      crawler.stop(true);
      await env.close();
    }
  });

  await t.test('validates maxConcurrencyPerOrigin when start is called', () => {
    const invalidValues = [0, -1, 1.5, '2', null, NaN];

    for (const value of invalidValues) {
      const crawler = new Crawler('http://127.0.0.1:9001/root');
      crawler.originScheduling = true;
      crawler.maxConcurrencyPerOrigin = value;
      assert.throws(() => crawler.start(), /positive integer/);
      assert.equal(crawler.running, undefined);
    }

    const crawler = new Crawler('http://127.0.0.1:9001/root');
    crawler.originScheduling = true;
    crawler.maxConcurrencyPerOrigin = 2;
    crawler.on('crawlstart', () => crawler.stop(true));
    crawler.start();
    assert.equal(crawler.maxConcurrencyPerOrigin, 2);
    setImmediate(() => clearInterval(crawler.crawlIntervalID));
  });

  await t.test('leaves the legacy runloop untouched when disabled', () => {
    const crawler = new Crawler('http://127.0.0.1:9001/root');
    assert.equal(crawler.originScheduling, false);
    crawler.on('crawlstart', () => crawler.stop(true));
    crawler.start();
    assert.equal(crawler._originScheduler, null);
    // Legacy scheduling keeps its original start/stop semantics; tidy up the
    // runloop this short-lived test deliberately leaves behind.
    setImmediate(() => clearInterval(crawler.crawlIntervalID));
  });
});

// ---------------------------------------------------------------------------
// Group 2: Robots isolation
// ---------------------------------------------------------------------------

test('Robots isolation', async t => {
  await t.test('slow robots.txt on one origin does not block another origin', async () => {
    const env = await startEnvironment();
    env.hold('A', '/robots.txt');

    const crawler = makeCrawler(env, {
      maxConcurrency: 4,
      extraUrls: [env.urls.B + '/root']
    });

    try {
      crawler.start();

      await until(() => env.recorder.requests.some(entry =>
        entry.origin === 'B' && entry.path === '/child'));

      const aRequests = env.recorder.requests.filter(entry => entry.origin === 'A');
      assert.deepEqual(aRequests.map(entry => entry.path), ['/robots.txt']);
      assert.equal(env.recorder.originActive('A'), 1, 'robots request holds a slot');

      // B completed its whole origin while A's robots.txt is pending.
      assert.deepEqual(
        env.recorder.pageRequests().filter(entry => entry.origin === 'B')
          .map(entry => entry.path),
        ['/root', '/child']
      );

      env.release('A', '/robots.txt');
      await events.once(crawler, 'complete');

      assert.equal(
        env.recorder.requests.filter(entry => entry.path === '/robots.txt').length,
        2
      );
    } finally {
      crawler.stop(true);
      await env.close();
    }
  });

  await t.test('robots.txt is fetched once per origin regardless of queue depth', async () => {
    const env = await startEnvironment();
    const crawler = makeCrawler(env, {
      maxConcurrency: 4,
      extraUrls: [
        env.urls.A + '/tree/1',
        env.urls.A + '/tree/2',
        env.urls.B + '/root',
        env.urls.B + '/child'
      ]
    });

    try {
      await runToCompletion(crawler);
      const robots = env.recorder.requests.filter(entry => entry.path === '/robots.txt');
      assert.deepEqual(robots.map(entry => entry.origin).sort(), ['A', 'B']);
    } finally {
      crawler.stop(true);
      await env.close();
    }
  });

  await t.test('robotstxterror keeps the origin crawlable', async () => {
    const env = await startEnvironment();
    env.robotsFail('B');

    const robotsErrors = [];
    const crawler = makeCrawler(env, {
      maxConcurrency: 4,
      extraUrls: [env.urls.B + '/root']
    });
    crawler.on('robotstxterror', error => robotsErrors.push(error));

    try {
      await runToCompletion(crawler);
      assert.equal(robotsErrors.length, 1);
      assert.ok(env.recorder.requests.some(entry =>
        entry.origin === 'B' && entry.path === '/root'));
      assert.ok(env.recorder.requests.some(entry =>
        entry.origin === 'B' && entry.path === '/child'));
    } finally {
      crawler.stop(true);
      await env.close();
    }
  });

  await t.test('rules loaded afterwards allow and reject pages', async () => {
    const env = await startEnvironment();
    env.robotsBody('A', 'User-agent: *\nDisallow: /forbidden\n');

    const crawler = makeCrawler(env, {
      maxConcurrency: 4,
      extraUrls: [
        env.urls.A + '/forbidden',
        env.urls.A + '/child',
        env.urls.B + '/root'
      ]
    });

    const disallowed = [];
    crawler.on('fetchdisallowed', item => disallowed.push(item.url));

    try {
      await runToCompletion(crawler);
      assert.ok(disallowed.some(url => url.endsWith('/forbidden')));
      assert.ok(!env.recorder.requests.some(entry => entry.path === '/forbidden'));
      assert.ok(env.recorder.requests.some(entry => entry.path === '/child'));
    } finally {
      crawler.stop(true);
      await env.close();
    }
  });

  await t.test('robots requests count towards global and per-origin concurrency', async () => {
    const env = await startEnvironment();
    env.hold('A', '/robots.txt');
    env.hold('B', '/robots.txt');

    const crawler = makeCrawler(env, {
      maxConcurrency: 2,
      extraUrls: [env.urls.B + '/root']
    });

    try {
      crawler.start();
      await until(() => env.recorder.active === 2);
      assert.equal(env.recorder.peak, 2);
      assert.equal(env.recorder.originActive('A'), 1);
      assert.equal(env.recorder.originActive('B'), 1);
      assert.equal(env.recorder.pageRequests().length, 0);

      env.release('A', '/robots.txt');
      env.release('B', '/robots.txt');
      await events.once(crawler, 'complete');
    } finally {
      crawler.stop(true);
      await env.close();
    }
  });
});

// ---------------------------------------------------------------------------
// Group 3: Async queue lifecycle
// ---------------------------------------------------------------------------

test('Async queue lifecycle', async t => {
  await t.test('completes through a slow, copy-returning queue without duplicates', async () => {
    const env = await startEnvironment();
    const queue = makeDelayedQueue(4);
    const crawler = makeCrawler(env, {
      queue,
      maxConcurrency: 4,
      extraUrls: [
        env.urls.A + '/tree/1',
        env.urls.B + '/root',
        env.urls.B + '/child'
      ]
    });

    const fetched = [];
    crawler.on('fetchcomplete', item => fetched.push(item.url));

    try {
      await runToCompletion(crawler);

      const pages = env.recorder.pageRequests();
      const paths = pages.map(entry => entry.origin + entry.path);
      assert.equal(new Set(paths).size, paths.length, 'a page was fetched more than once');

      const expected = new Set([
        'A/root', 'A/tree/1', 'A/child', 'B/root', 'B/child'
      ]);
      assert.deepEqual(new Set(pages.map(entry => entry.origin + entry.path)), expected);

      const stats = await queueStats(queue);
      assert.equal(stats.count, stats.length);

      await wait(20);
      assert.equal(crawler._originScheduler._slots.size, 0);
    } finally {
      crawler.stop(true);
      await env.close();
    }
  });

  await t.test('HTTP errors release the slot and do not starve later items', async () => {
    const env = await startEnvironment();
    env.destroy('A', '/tree/1');

    const crawler = makeCrawler(env, {
      maxConcurrency: 2,
      extraUrls: [
        env.urls.A + '/tree/1',
        env.urls.A + '/tree/2',
        env.urls.B + '/root'
      ]
    });

    const clientErrors = [];
    crawler.on('fetchclienterror', item => clientErrors.push(item.url));

    try {
      await runToCompletion(crawler);
      assert.equal(clientErrors.length, 1);
      assert.ok(env.recorder.requests.some(entry => entry.path === '/tree/2'));
      assert.ok(env.recorder.requests.some(entry => entry.origin === 'B' && entry.path === '/root'));
      assert.equal(crawler._originScheduler._slots.size, 0);
    } finally {
      crawler.stop(true);
      await env.close();
    }
  });

  await t.test('redirects release the old slot and schedule the new entry', async () => {
    const env = await startEnvironment();
    const crawler = makeCrawler(env, {
      maxConcurrency: 2,
      initialPath: '/redir'
    });

    const redirects = [];
    crawler.on('fetchredirect', item => redirects.push(item.url));

    try {
      await runToCompletion(crawler);
      assert.equal(redirects.length, 1);
      assert.ok(env.recorder.requests.some(entry => entry.path === '/leaf'));
      assert.equal(crawler._originScheduler._slots.size, 0);
    } finally {
      crawler.stop(true);
      await env.close();
    }
  });

  await t.test('asynchronous queue write failures never leak slots', async () => {
    const env = await startEnvironment();
    let failedOnce = false;
    const queue = makeDelayedQueue(2, {
      failUpdate(id, updates) {
        if (!failedOnce && updates && updates.status === 'downloaded') {
          failedOnce = true;
          return true;
        }

        return false;
      }
    });

    const queueErrors = [];
    const crawler = makeCrawler(env, {
      queue,
      maxConcurrency: 2,
      extraUrls: [env.urls.B + '/root']
    });
    crawler.on('queueerror', error => queueErrors.push(error));

    try {
      crawler.start();

      // The failed write keeps that item unsettled (the queue backend rejected
      // the terminal update, exactly as in the legacy behaviour), so the crawl
      // can't reach `complete`; the contract under test is that its slot and
      // every other slot still get returned.
      await until(() => queueErrors.length === 1 &&
        crawler._originScheduler._slots.size === 0);

      assert.equal(crawler._originScheduler._reservedIds.size, 0);
      assert.equal(crawler._openRequests.length, 0);

      // Other work scheduled after the failure still runs.
      crawler.queueURL(env.urls.A + '/tree/2');
      await until(() => env.recorder.requests.some(entry =>
        entry.origin === 'A' && entry.path === '/tree/2'));
    } finally {
      crawler.stop(true);
      await env.close();
    }
  });

  await t.test('stop(false) freezes dispatch, in-flight work settles and start resumes', async () => {
    const env = await startEnvironment();
    const queue = makeDelayedQueue(3);
    const crawler = makeCrawler(env, {
      queue,
      maxConcurrency: 4,
      initialPath: '/tree',
      extraUrls: [env.urls.B + '/root']
    });

    env.hold('A', '/tree');
    env.hold('B', '/root');

    try {
      crawler.start();
      await until(() => env.recorder.originActive('A') === 1 &&
        env.recorder.originActive('B') === 1);

      crawler.stop(false);
      const requestsAtStop = env.recorder.requests.length;
      await wait(120);
      assert.equal(env.recorder.requests.length, requestsAtStop,
        'a late callback started a request after stop');

      env.release('A', '/tree');
      env.release('B', '/root');

      await until(() => env.recorder.active === 0);
      assert.equal(env.recorder.requests.length, requestsAtStop);

      crawler.start();
      await events.once(crawler, 'complete');

      const fetchedPaths = env.recorder.pageRequests().map(entry => entry.origin + entry.path);
      for (const expected of ['A/tree', 'A/tree/1', 'A/tree/2', 'A/tree/3', 'B/root', 'B/child']) {
        assert.ok(fetchedPaths.includes(expected), 'missing ' + expected);
      }

      assert.equal(crawler._originScheduler._slots.size, 0);
    } finally {
      crawler.stop(true);
      await env.close();
    }
  });

  await t.test('late scheduling callbacks after stop cannot send anything', async () => {
    const env = await startEnvironment();
    const queue = makeDelayedQueue(25);
    const crawler = makeCrawler(env, {queue});

    try {
      crawler.start();
      crawler.stop(false);
      await wait(120);
      assert.equal(env.recorder.requests.length, 0);

      crawler.start();
      await events.once(crawler, 'complete');
      assert.ok(env.recorder.requests.length > 0);
      assert.equal(crawler._originScheduler._slots.size, 0);
    } finally {
      crawler.stop(true);
      await env.close();
    }
  });

  await t.test('complete waits for reservations, robots work and pending queue writes', async () => {
    const env = await startEnvironment();
    const queue = makeDelayedQueue(6);
    const crawler = makeCrawler(env, {
      queue,
      maxConcurrency: 3,
      initialPath: '/tree',
      extraUrls: [env.urls.B + '/root']
    });

    const completed = [];
    crawler.on('fetchcomplete', item => completed.push(item.url));

    try {
      await runToCompletion(crawler);

      const stats = await queueStats(queue);
      assert.equal(stats.count, stats.length);

      const paths = env.recorder.pageRequests().map(entry => entry.origin + entry.path);
      for (const expected of ['A/tree', 'A/tree/1', 'A/tree/2', 'A/tree/3', 'B/root', 'B/child']) {
        assert.ok(paths.includes(expected), 'missing ' + expected);
      }

      assert.equal(completed.length, env.recorder.pageRequests().length);
      assert.equal(crawler._pendingQueueAdds, 0);
      assert.equal(crawler._originScheduler._slots.size, 0);
    } finally {
      crawler.stop(true);
      await env.close();
    }
  });
});
