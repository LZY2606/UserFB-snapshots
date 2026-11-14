'use strict';

// Target tests for the h2cOrigins feature. This file doubles as the test
// module (required by task_support/check.cjs) and as the fixture server
// subprocess:
//
//   node test/h2c-target.cjs serve <scenario>
//
// runs a local HTTP/2 (and HTTP/1) server that is steered by the parent
// over IPC, so every ordering is established through explicit barriers
// instead of sleeps.

const { spawn } = require('node:child_process');
const { once } = require('node:events');

if (process.argv[2] === 'serve') {
    serve(process.argv[3]);
} else {
    module.exports = { H2Session, H2Goaway, H2Crawl };
}

/* ------------------------------------------------------------------ */
/* Server role                                                         */
/* ------------------------------------------------------------------ */

function serve(scenario) {
    if (scenario === 'session') {
        serveSession();
    } else if (scenario === 'goaway') {
        serveGoaway();
    } else if (scenario === 'crawl') {
        serveCrawl();
    } else {
        console.error('unknown scenario: ' + scenario);
        process.exit(1);
    }
}

function serverHarness() {
    const commands = {};

    process.on('message', (msg) => {
        if (msg && typeof msg.type === 'string' && commands[msg.type]) {
            commands[msg.type](msg);
        }
    });

    return {
        send(msg) {
            if (process.send) {
                process.send(msg);
            }
        },
        commands
    };
}

// Scenario "session": an h2 server (initial maxConcurrentStreams: 1) behind
// a TCP proxy that holds back the client's connection preface until the
// parent releases it - so the parent can observe the client before the
// first SETTINGS frame is ever sent.
function serveSession() {
    const h2 = require('node:http2');
    const net = require('node:net');
    const { send, commands } = serverHarness();

    const streams = new Map();
    const sessions = [];
    let totalStreams = 0;
    let closedSessions = 0;

    const target = h2.createServer({ settings: { maxConcurrentStreams: 1 } });

    target.on('session', (session) => {
        sessions.push(session);
        session.on('close', () => {
            closedSessions++;
            send({ type: 'sessionClosed', closed: closedSessions, sessions: sessions.length });
        });
    });

    target.on('stream', (stream, headers) => {
        totalStreams++;
        streams.set(stream.id, { stream, path: headers[':path'], responded: false });
        send({
            type: 'stream',
            id: stream.id,
            path: headers[':path'],
            authority: headers[':authority'] || null,
            connection: 'connection' in headers ? String(headers.connection) : null,
            host: 'host' in headers ? String(headers.host) : null,
            total: totalStreams,
            sessions: sessions.length
        });
    });

    let released = false;
    const heldConnections = [];

    const proxy = net.createServer((socket) => {
        let upstream = null;
        const buffered = [];
        let notified = false;

        const connect = () => {
            if (upstream) {
                return;
            }
            upstream = net.connect(target.address().port, '127.0.0.1', () => {
                buffered.splice(0).forEach((chunk) => upstream.write(chunk));
            });
            upstream.on('data', (data) => socket.write(data));
            upstream.on('close', () => socket.destroy());
            socket.on('close', () => upstream.destroy());
        };

        socket.on('data', (chunk) => {
            if (!notified) {
                notified = true;
                send({ type: 'preface' });
            }
            if (upstream) {
                upstream.write(chunk);
            } else {
                buffered.push(chunk);
            }
        });

        if (released) {
            connect();
        } else {
            heldConnections.push(connect);
        }
    });

    target.listen(0, '127.0.0.1', () => {
        proxy.listen(0, '127.0.0.1', () => {
            send({ type: 'listening', port: proxy.address().port });
        });
    });

    commands.release = () => {
        released = true;
        heldConnections.splice(0).forEach((connect) => connect());
    };

    commands.setMax = (msg) => {
        sessions.forEach((session) => {
            if (!session.destroyed) {
                session.settings({ maxConcurrentStreams: msg.n });
            }
        });
    };

    commands.respond = (msg) => {
        const rec = streams.get(msg.id);
        if (rec && !rec.responded && !rec.stream.destroyed) {
            rec.responded = true;
            rec.stream.respond({ ':status': 200, 'content-type': 'text/plain' });
            rec.stream.end('BODY:' + rec.path);
        }
    };

    commands.respondAll = () => {
        for (const rec of streams.values()) {
            if (!rec.responded && !rec.stream.destroyed) {
                rec.responded = true;
                rec.stream.respond({ ':status': 200, 'content-type': 'text/plain' });
                rec.stream.end('BODY:' + rec.path);
            }
        }
    };

    commands.count = () => {
        send({ type: 'count', streams: totalStreams, sessions: sessions.length, closed: closedSessions });
    };

    commands.quit = () => process.exit(0);
}

// Scenario "goaway": an h2 server (maxConcurrentStreams: 2) that sends
// GOAWAY frames with parent-chosen lastStreamIDs, and can deliver headers
// and bodies independently per path and session generation.
function serveGoaway() {
    const h2 = require('node:http2');
    const { send, commands } = serverHarness();

    const sessions = [];
    let closedSessions = 0;

    const server = h2.createServer({ settings: { maxConcurrentStreams: 2 } });

    server.on('session', (session) => {
        const rec = { session, streams: new Map(), streamCount: 0, n: sessions.length + 1 };
        sessions.push(rec);
        session.on('close', () => {
            closedSessions++;
            send({ type: 'sessionClosed', closed: closedSessions, sessions: sessions.length });
        });
    });

    server.on('stream', (stream, headers) => {
        const rec = sessions[sessions.length - 1];
        rec.streamCount++;
        rec.streams.set(stream.id, { stream, path: headers[':path'], responded: false });
        send({ type: 'stream', session: rec.n, id: stream.id, path: headers[':path'] });
    });

    function forPath(sessionN, path, fn) {
        const rec = sessions[sessionN - 1];
        if (!rec) {
            return;
        }
        for (const r of rec.streams.values()) {
            if (r.path === path) {
                fn(r);
            }
        }
    }

    commands.headers = (msg) => {
        forPath(msg.session, msg.path, (r) => {
            if (!r.responded) {
                r.responded = true;
                r.stream.respond({ ':status': 200, 'content-type': 'text/plain' });
            }
        });
    };

    commands.respond = (msg) => {
        forPath(msg.session, msg.path, (r) => {
            if (!r.responded) {
                r.responded = true;
                r.stream.respond({ ':status': 200, 'content-type': 'text/plain' });
            }
            if (!r.stream.destroyed) {
                r.stream.end('BODY:' + r.path);
            }
        });
    };

    commands.goaway = (msg) => {
        const rec = sessions[msg.session - 1];
        if (rec && !rec.session.destroyed) {
            rec.session.goaway(0, msg.lastStreamID);
        }
    };

    commands.count = () => {
        send({
            type: 'count',
            sessions: sessions.length,
            perSession: sessions.map((rec) => rec.streamCount),
            closed: closedSessions
        });
    };

    commands.quit = () => process.exit(0);

    server.listen(0, '127.0.0.1', () => {
        send({ type: 'listening', port: server.address().port });
    });
}

// Scenario "crawl": two h2-only origins (A and B) and one HTTP/1 origin
// (C), exercising robots.txt, cross-origin redirects, gzip discovery,
// cookies and a plain HTTP/1 fetch.
function serveCrawl() {
    const h2 = require('node:http2');
    const http = require('node:http');
    const zlib = require('node:zlib');
    const { send, commands } = serverHarness();

    const requests = { A: [], B: [], C: [] };
    const sessionCounts = { A: 0, B: 0 };
    const closedCounts = { A: 0, B: 0 };
    let portA = 0;
    let portB = 0;
    let portC = 0;

    function sendBody(stream, headers, body) {
        const buffer = Buffer.isBuffer(body) ? body : Buffer.from(body);
        stream.respond(Object.assign({ ':status': 200 }, headers, { 'content-length': buffer.length }));
        stream.end(buffer);
    }

    const serverA = h2.createServer();
    serverA.on('session', (session) => {
        sessionCounts.A++;
        session.on('close', () => {
            closedCounts.A++;
            send({ type: 'closed', server: 'A', closed: closedCounts.A });
        });
    });
    serverA.on('stream', (stream, headers) => {
        const path = headers[':path'];
        requests.A.push({ path, cookie: headers.cookie || null });

        if (path === '/robots.txt') {
            sendBody(stream, { 'content-type': 'text/plain' }, 'User-agent: *\nDisallow: /blocked\n');
        } else if (path === '/') {
            sendBody(stream, {
                'content-type': 'text/html',
                'set-cookie': ['sess=abc; Path=/']
            }, '<html><a href="/blocked">b</a><a href="/gzip">g</a>' +
                '<a href="/redir-b">rb</a><a href="/redir-c">rc</a></html>');
        } else if (path === '/gzip') {
            sendBody(stream, { 'content-type': 'text/html', 'content-encoding': 'gzip' },
                zlib.gzipSync('<html><a href="/leaf">leaf</a></html>'));
        } else if (path === '/leaf') {
            sendBody(stream, { 'content-type': 'text/plain' }, 'LEAF_OK');
        } else if (path === '/from-c') {
            sendBody(stream, { 'content-type': 'text/plain' }, 'FROM_C_OK');
        } else if (path === '/redir-b') {
            stream.respond({ ':status': 302, 'location': 'http://127.0.0.1:' + portB + '/page-b' });
            stream.end();
        } else if (path === '/redir-c') {
            stream.respond({ ':status': 302, 'location': 'http://127.0.0.1:' + portC + '/page-c' });
            stream.end();
        } else if (path === '/blocked') {
            sendBody(stream, { 'content-type': 'text/plain' }, 'BLOCKED_FETCHED');
        } else {
            stream.respond({ ':status': 404 });
            stream.end();
        }
    });

    const serverB = h2.createServer();
    serverB.on('session', (session) => {
        sessionCounts.B++;
        session.on('close', () => {
            closedCounts.B++;
            send({ type: 'closed', server: 'B', closed: closedCounts.B });
        });
    });
    serverB.on('stream', (stream, headers) => {
        const path = headers[':path'];
        requests.B.push({ path });

        if (path === '/robots.txt') {
            sendBody(stream, { 'content-type': 'text/plain' }, 'User-agent: *\nAllow: /\n');
        } else if (path === '/page-b') {
            sendBody(stream, { 'content-type': 'text/plain' }, 'B_OK');
        } else {
            stream.respond({ ':status': 404 });
            stream.end();
        }
    });

    const serverC = http.createServer((req, res) => {
        requests.C.push({ path: req.url, httpVersion: req.httpVersion });

        if (req.url === '/robots.txt') {
            res.setHeader('Content-Type', 'text/plain');
            res.end('User-agent: *\nAllow: /\n');
        } else if (req.url === '/page-c') {
            res.setHeader('Content-Type', 'text/html');
            res.end('<html><a href="/redir-a">ra</a>C_OK</html>');
        } else if (req.url === '/redir-a') {
            res.statusCode = 302;
            res.setHeader('Location', 'http://127.0.0.1:' + portA + '/from-c');
            res.end();
        } else {
            res.statusCode = 404;
            res.end();
        }
    });

    serverB.listen(0, '127.0.0.1', () => {
        portB = serverB.address().port;
        serverC.listen(0, '127.0.0.1', () => {
            portC = serverC.address().port;
            serverA.listen(0, '127.0.0.1', () => {
                portA = serverA.address().port;
                send({ type: 'listening', portA, portB, portC });
            });
        });
    });

    commands.report = () => {
        send({ type: 'report', requests, sessionCounts, closedCounts });
    };

    commands.quit = () => process.exit(0);
}

/* ------------------------------------------------------------------ */
/* Parent-side helpers                                                 */
/* ------------------------------------------------------------------ */

function spawnServer(scenario) {
    const child = spawn(process.execPath, [__filename, 'serve', scenario], {
        stdio: ['ignore', 'inherit', 'inherit', 'ipc']
    });

    const backlog = [];
    const waiters = [];

    child.on('message', (msg) => {
        for (let i = 0; i < waiters.length; i++) {
            if (waiters[i].pred(msg)) {
                const waiter = waiters.splice(i, 1)[0];
                waiter.resolve(msg);
                return;
            }
        }
        backlog.push(msg);
    });

    child.on('exit', (code) => {
        const error = new Error('server child exited unexpectedly (code ' + code + ')');
        while (waiters.length) {
            waiters.shift().reject(error);
        }
    });

    const api = {
        send: (msg) => child.send(msg),
        wait: (type, pred) => {
            const matches = (msg) => msg.type === type && (!pred || pred(msg));
            for (let i = 0; i < backlog.length; i++) {
                if (matches(backlog[i])) {
                    return Promise.resolve(backlog.splice(i, 1)[0]);
                }
            }
            return new Promise((resolve, reject) => waiters.push({ pred: matches, resolve, reject }));
        },
        request: (msg, replyType) => {
            const reply = api.wait(replyType);
            child.send(msg);
            return reply;
        },
        quit: async () => {
            if (child.exitCode !== null || child.killed) {
                return;
            }
            const exited = once(child, 'exit');
            try {
                child.send({ type: 'quit' });
            } catch (error) {
                // child already gone
            }
            await exited;
        }
    };

    return api;
}

// Records event payloads and lets tests await them by predicate, whether
// they fired before or after the wait started.
function eventLog() {
    const items = [];
    const waiters = [];

    return {
        items,
        push(item) {
            for (let i = 0; i < waiters.length; i++) {
                if (waiters[i](item)) {
                    waiters.splice(i, 1)[0];
                    return;
                }
            }
            items.push(item);
        },
        wait(pred) {
            for (let i = 0; i < items.length; i++) {
                if (pred(items[i])) {
                    return Promise.resolve(items.splice(i, 1)[0]);
                }
            }
            return new Promise((resolve) => waiters.push((item) => {
                if (pred(item)) {
                    resolve(item);
                    return true;
                }
                return false;
            }));
        }
    };
}

function queueItems(crawler) {
    return new Promise((resolve, reject) => {
        crawler.queue.filterItems({}, (error, items) => {
            if (error) {
                reject(error);
            } else {
                resolve(items);
            }
        });
    });
}

/* ------------------------------------------------------------------ */
/* Test groups                                                         */
/* ------------------------------------------------------------------ */

async function H2Session(c) {
    const Crawler = require('../lib');

    // --- h2cOrigins configuration validation (no network involved) ---
    const defaults = new Crawler('http://example.com/');
    c.check(Array.isArray(defaults.h2cOrigins) && defaults.h2cOrigins.length === 0,
        'h2cOrigins defaults to an empty array');

    const invalid = [
        'https://example.com',
        'http://example.com/path',
        'http://example.com/?q=1',
        'http://example.com/#frag',
        'http://user:pass@example.com',
        'not a url'
    ];

    for (const entry of invalid) {
        const probe = new Crawler('http://example.com/');
        probe.h2cOrigins = [entry];
        let threw = false;
        try {
            probe.start();
        } catch (error) {
            threw = true;
        }
        c.check(threw, 'start() rejects invalid h2cOrigins entry: ' + entry);
        c.check(!probe.running, 'crawler never starts with an invalid h2cOrigins entry');
    }

    const norm = new Crawler('http://example.com/');
    norm.h2cOrigins = ['http://example.com', 'http://example.com:80', 'http://example.com:8080/'];
    c.check(norm._h2cOriginForQueueItem({ protocol: 'http', url: 'http://example.com:80/x' }) === 'http://example.com',
        'explicit port 80 and the default port normalize to the same origin');
    c.check(norm._h2cOriginForQueueItem({ protocol: 'http', url: 'http://example.com:8080/x' }) === 'http://example.com:8080',
        'explicit non-default port matches its origin');
    c.check(norm._h2cOriginForQueueItem({ protocol: 'http', url: 'http://elsewhere.com/' }) === false,
        'unlisted origins stay on HTTP/1');
    c.check(norm._h2cOriginForQueueItem({ protocol: 'https', url: 'https://example.com/' }) === false,
        'https URLs never use h2c');

    // --- live session behaviour: SETTINGS gating and limit changes ---
    const server = spawnServer('session');

    try {
        const listening = await server.wait('listening');
        const origin = 'http://127.0.0.1:' + listening.port;

        const crawler = new Crawler(origin + '/a');
        crawler.h2cOrigins = [origin];
        crawler.respectRobotsTxt = false;
        crawler.interval = 5;
        crawler.maxConcurrency = 3;
        // Queue the extra pages once the initial URL is queued, so the
        // fetch order is /a, /b, /c, /d, /e
        crawler.once('crawlstart', () => {
            crawler.queueURL(origin + '/b');
            crawler.queueURL(origin + '/c');
            crawler.queueURL(origin + '/d');
            crawler.queueURL(origin + '/e');
        });

        const starts = eventLog();
        const completes = eventLog();
        const clientErrors = eventLog();
        crawler.on('fetchstart', (queueItem) => starts.push(queueItem.path));
        crawler.on('fetchcomplete', (queueItem, body) => completes.push({ path: queueItem.path, body: body.toString() }));
        crawler.on('fetchclienterror', (queueItem, error) => clientErrors.push({ path: queueItem.path, code: error.code }));

        const complete = once(crawler, 'complete');
        crawler.start();

        // The proxy holds the connection preface, so the client is
        // connected but has not seen any SETTINGS frame yet
        await server.wait('preface');
        await starts.wait((path) => path === '/c');

        const entry = crawler._h2cClient.sessions[origin];
        c.check(!!entry, 'an h2c session exists for the origin');
        c.check(entry.ready === false && entry.active === 0,
            'no stream is created before the first remote SETTINGS');
        c.check(entry.pending.length === 3, 'in-flight requests wait for a stream slot');
        c.check(crawler._openRequests.length === 3,
            'requests waiting for a stream slot count toward maxConcurrency');

        server.send({ type: 'release' });

        const s1 = await server.wait('stream');
        c.check(s1.id === 1 && s1.path === '/a', 'first stream (id 1) fetches /a');
        c.check(s1.authority === '127.0.0.1:' + listening.port, 'Host maps to :authority');
        c.check(s1.connection === null && s1.host === null,
            'no Connection/Host hop-by-hop headers on the h2 stream');
        c.check(s1.total === 1, 'initial maxConcurrentStreams=1 admits exactly one stream');

        // Raising the limit lets waiting requests stream
        server.send({ type: 'setMax', n: 3 });
        const s2 = await server.wait('stream');
        const s3 = await server.wait('stream');
        c.check(s2.id === 3 && s2.path === '/b', 'raised limit admits /b on stream 3');
        c.check(s3.id === 5 && s3.path === '/c', 'raised limit admits /c on stream 5');

        // Lowering the limit below the active count: existing streams
        // continue, new streams wait for a slot
        server.send({ type: 'setMax', n: 1 });
        server.send({ type: 'respond', id: 1 });
        await completes.wait((item) => item.path === '/a');
        await starts.wait((path) => path === '/d');
        const count1 = await server.request({ type: 'count' }, 'count');
        c.check(count1.streams === 3, 'lowered limit blocks new streams while two are active');
        c.check(count1.sessions === 1, 'the origin is served by a single session');

        // Raising again lets the waiting request stream
        server.send({ type: 'setMax', n: 5 });
        const s4 = await server.wait('stream');
        c.check(s4.id === 7 && s4.path === '/d', 'raised limit lets the waiting request stream');
        server.send({ type: 'respondAll' });
        const s5 = await server.wait('stream');
        c.check(s5.id === 9 && s5.path === '/e', 'last request streams on the same session');
        server.send({ type: 'respondAll' });

        await complete;

        c.check(clientErrors.items.length === 0, 'no fetchclienterror during the crawl');
        c.check(starts.items.length === 5, 'fetchstart fired exactly once per request');
        c.check(completes.items.length === 5, 'all five pages complete');
        for (const path of ['/a', '/b', '/c', '/d', '/e']) {
            const hit = completes.items.find((item) => item.path === path);
            c.check(!!hit && hit.body === 'BODY:' + path, 'delivered body for ' + path);
        }

        const items = await queueItems(crawler);
        c.check(items.length === 5 && items.every((item) => item.fetched && item.status === 'downloaded'),
            'queue fully downloaded after complete');

        const closed1 = await server.wait('sessionClosed');
        c.check(closed1.closed === 1 && closed1.sessions === 1, 'session closed after a normal complete');

        // --- stop(true) aborts in-flight and waiting requests, closes sessions ---
        const crawler2 = new Crawler(origin + '/x');
        crawler2.h2cOrigins = [origin];
        crawler2.respectRobotsTxt = false;
        crawler2.interval = 5;
        crawler2.maxConcurrency = 2;
        crawler2.once('crawlstart', () => {
            crawler2.queueURL(origin + '/y');
        });

        const starts2 = eventLog();
        const clientErrors2 = eventLog();
        crawler2.on('fetchstart', (queueItem) => starts2.push(queueItem.path));
        crawler2.on('fetchclienterror', (queueItem, error) => clientErrors2.push({ path: queueItem.path, code: error.code }));
        crawler2.start();

        const sx = await server.wait('stream', (msg) => msg.path === '/x');
        c.check(sx.id === 1 && sx.sessions === 2, 'a new crawl uses a new session generation');

        await starts2.wait((path) => path === '/y');
        crawler2.stop(true);

        c.check(crawler2._openRequests.length === 0,
            'stop(true) terminates in-flight and stream-waiting requests');
        c.check(!crawler2._h2cClient, 'stop(true) tears down the h2c client');
        const closed2 = await server.wait('sessionClosed', (msg) => msg.closed === 2);
        c.check(closed2.sessions === 2, 'second session closed after stop(true)');
        c.check(clientErrors2.items.length === 0, 'aborted requests do not raise fetchclienterror');
    } finally {
        await server.quit();
    }
}

async function H2Goaway(c) {
    const Crawler = require('../lib');
    const server = spawnServer('goaway');

    try {
        const listening = await server.wait('listening');
        const origin = 'http://127.0.0.1:' + listening.port;

        const crawler = new Crawler(origin + '/old');
        crawler.h2cOrigins = [origin];
        crawler.respectRobotsTxt = false;
        crawler.interval = 5;
        crawler.maxConcurrency = 3;
        // Queue after the initial URL so the fetch order is /old, /moved, /unlucky
        crawler.once('crawlstart', () => {
            crawler.queueURL(origin + '/moved');
            crawler.queueURL(origin + '/unlucky');
        });

        const starts = eventLog();
        const completes = eventLog();
        const clientErrors = eventLog();
        crawler.on('fetchstart', (queueItem) => starts.push(queueItem.path));
        crawler.on('fetchcomplete', (queueItem, body) => completes.push({ path: queueItem.path, body: body.toString() }));
        crawler.on('fetchclienterror', (queueItem, error) => clientErrors.push({ path: queueItem.path, code: error.code }));

        const complete = once(crawler, 'complete');
        crawler.start();

        // Session 1 admits two streams; /unlucky waits for a slot
        const m1 = await server.wait('stream');
        const m2 = await server.wait('stream');
        c.check(m1.session === 1 && m1.id === 1 && m1.path === '/old', 'session 1 stream 1 fetches /old');
        c.check(m2.session === 1 && m2.id === 3 && m2.path === '/moved', 'session 1 stream 3 fetches /moved');

        // Deliver response headers for /old, then GOAWAY the whole
        // session: /old must finish on session 1 and never be replayed
        server.send({ type: 'headers', session: 1, path: '/old' });
        server.send({ type: 'goaway', session: 1, lastStreamID: 0 });

        // /moved (no headers yet) and /unlucky (no stream yet) migrate to
        // a new session generation
        const m3 = await server.wait('stream', (msg) => msg.session === 2);
        const m4 = await server.wait('stream', (msg) => msg.session === 2 && msg.id !== m3.id);
        const migrated = {};
        migrated[m3.path] = m3.id;
        migrated[m4.path] = m4.id;
        c.check(migrated['/moved'] === 1, 'refused stream migrates first onto session 2');
        c.check(migrated['/unlucky'] === 3, 'never-streamed request migrates onto session 2');

        // The old-generation stream finishes on the old session
        server.send({ type: 'respond', session: 1, path: '/old' });
        const oldDone = await completes.wait((item) => item.path === '/old');
        c.check(oldDone.body === 'BODY:/old', 'old-generation stream delivers after GOAWAY');
        const count1 = await server.request({ type: 'count' }, 'count');
        c.check(count1.perSession[0] === 2 && count1.perSession[1] === 2,
            'header-delivered request is not replayed on the new session');

        // A second GOAWAY excludes /unlucky again: the logical request
        // used up its single resend and fails instead of reconnecting
        server.send({ type: 'goaway', session: 2, lastStreamID: 1 });
        const unluckyError = await clientErrors.wait((item) => item.path === '/unlucky');
        c.check(unluckyError.code === 'H2_GOAWAY_EXCLUDED',
            'second GOAWAY exclusion ends the request via fetchclienterror');

        server.send({ type: 'respond', session: 2, path: '/moved' });
        await complete;

        const count2 = await server.request({ type: 'count' }, 'count');
        c.check(count2.sessions === 2, 'no endless reconnect after the second GOAWAY');
        c.check(starts.items.length === 3, 'migration never repeats fetchstart');
        c.check(completes.items.length === 2, 'two pages complete');
        const movedDone = completes.items.find((item) => item.path === '/moved');
        c.check(!!movedDone && movedDone.body === 'BODY:/moved', 'surviving stream delivers on session 2');

        const items = await queueItems(crawler);
        const byPath = {};
        items.forEach((item) => {
            byPath[item.path] = item;
        });
        c.check(items.length === 3, 'queue holds exactly the three logical requests');
        c.check(byPath['/old'].status === 'downloaded' && byPath['/moved'].status === 'downloaded',
            'old-generation and migrated requests downloaded');
        c.check(byPath['/unlucky'].fetched === true && byPath['/unlucky'].status === 'failed',
            'twice-excluded request marked failed');

        const closed = await server.wait('sessionClosed', (msg) => msg.closed === 2);
        c.check(closed.sessions === 2, 'both session generations closed after complete');
    } finally {
        await server.quit();
    }
}

async function H2Crawl(c) {
    const Crawler = require('../lib');
    const server = spawnServer('crawl');

    try {
        const listening = await server.wait('listening');
        const originA = 'http://127.0.0.1:' + listening.portA;
        const originB = 'http://127.0.0.1:' + listening.portB;
        const originC = 'http://127.0.0.1:' + listening.portC;

        const crawler = new Crawler(originA + '/');
        crawler.h2cOrigins = [originA, originB];
        crawler.interval = 5;
        crawler.maxConcurrency = 4;

        const completes = eventLog();
        const disallowed = eventLog();
        const redirects = eventLog();
        const clientErrors = eventLog();
        const robotsErrors = eventLog();
        crawler.on('fetchcomplete', (queueItem, body) => completes.push({ url: queueItem.url, path: queueItem.path, body: body.toString() }));
        crawler.on('fetchdisallowed', (queueItem) => disallowed.push(queueItem.url));
        crawler.on('fetchredirect', (queueItem, redirectQueueItem) => redirects.push({ from: queueItem.path, to: redirectQueueItem.url }));
        crawler.on('fetchclienterror', (queueItem, error) => clientErrors.push({ path: queueItem.path, code: error.code }));
        crawler.on('robotstxterror', (error) => robotsErrors.push(String(error)));

        const complete = once(crawler, 'complete');
        crawler.start();
        await complete;

        const bodyFor = (url) => {
            const hit = completes.items.find((item) => item.url === url);
            return hit ? hit.body : null;
        };

        c.check((bodyFor(originA + '/') || '').indexOf('/gzip') !== -1, 'index page delivered over h2');
        c.check(bodyFor(originA + '/gzip') === '<html><a href="/leaf">leaf</a></html>',
            'gzip page decompressed over h2');
        c.check(bodyFor(originA + '/leaf') === 'LEAF_OK', 'link discovered inside the gzip page was fetched');
        c.check(bodyFor(originB + '/page-b') === 'B_OK', 'cross-origin h2 redirect target fetched over h2');
        c.check((bodyFor(originC + '/page-c') || '').indexOf('C_OK') !== -1, 'redirect to HTTP/1 origin fetched over HTTP/1');
        c.check(bodyFor(originA + '/from-c') === 'FROM_C_OK', 'HTTP/1 to h2 redirect target fetched over h2');
        c.check(completes.items.length === 6, 'exactly six pages complete');

        c.check(disallowed.items.indexOf(originA + '/blocked') !== -1,
            'robots-disallowed page triggers fetchdisallowed');
        c.check(robotsErrors.items.length === 0, 'no robots.txt errors');
        c.check(clientErrors.items.length === 0, 'no client errors during the crawl');

        c.check(redirects.items.some((r) => r.from === '/redir-b' && r.to === originB + '/page-b'),
            'redirect to h2 origin B queued');
        c.check(redirects.items.some((r) => r.from === '/redir-c' && r.to === originC + '/page-c'),
            'redirect to HTTP/1 origin C queued');
        c.check(redirects.items.some((r) => r.from === '/redir-a' && r.to === originA + '/from-c'),
            'redirect from HTTP/1 origin back to h2 origin queued');

        // Server-side view of the crawl
        const report = await server.request({ type: 'report' }, 'report');
        const pathsA = report.requests.A.map((r) => r.path);
        c.check(pathsA.indexOf('/blocked') === -1, 'disallowed page was never requested');
        c.check(pathsA.indexOf('/robots.txt') !== -1, 'robots.txt fetched over h2');
        for (const path of ['/', '/gzip', '/leaf', '/from-c', '/redir-b', '/redir-c']) {
            c.check(pathsA.indexOf(path) !== -1, 'server A saw a request for ' + path);
        }
        const gzipRequest = report.requests.A.find((r) => r.path === '/gzip');
        c.check(!!gzipRequest && /sess=abc/.test(gzipRequest.cookie || ''),
            'cookie jar applies to h2 requests');
        c.check(report.sessionCounts.A === 1, 'origin A used a single h2 session');

        const pathsB = report.requests.B.map((r) => r.path).sort();
        c.check(pathsB.join(',') === '/page-b,/robots.txt', 'origin B served robots.txt and page');
        c.check(report.sessionCounts.B === 1, 'origin B used its own independent session');

        const pathsC = report.requests.C.map((r) => r.path).sort();
        c.check(pathsC.join(',') === '/page-c,/redir-a,/robots.txt', 'HTTP/1 origin fetched normally');
        c.check(report.requests.C.every((r) => r.httpVersion === '1.1'), 'origin C really spoke HTTP/1');

        // Final queue state
        const items = await queueItems(crawler);
        c.check(!items.some((item) => item.url === originA + '/blocked'),
            'disallowed page never entered the queue');
        c.check(items.every((item) => item.fetched), 'every queued item was fetched');
        const redirectItem = items.find((item) => item.url === originA + '/redir-b');
        c.check(!!redirectItem && redirectItem.status === 'redirected',
            'redirect source recorded as redirected');

        // Sessions close after a normal complete
        await server.wait('closed', (msg) => msg.server === 'A' && msg.closed === 1);
        await server.wait('closed', (msg) => msg.server === 'B' && msg.closed === 1);
        c.check(true, 'h2 sessions for both origins closed after complete');

        // An h2c-configured origin that only speaks HTTP/1 fails rather
        // than silently downgrading to HTTP/1
        const crawler2 = new Crawler(originC + '/page-c');
        crawler2.h2cOrigins = [originC];
        crawler2.respectRobotsTxt = false;
        crawler2.interval = 5;
        const clientErrors2 = eventLog();
        const completes2 = eventLog();
        crawler2.on('fetchclienterror', (queueItem, error) => clientErrors2.push({ code: error.code || error.message }));
        crawler2.on('fetchcomplete', (queueItem) => completes2.push(queueItem.path));
        const complete2 = once(crawler2, 'complete');
        crawler2.start();
        await complete2;
        c.check(clientErrors2.items.length === 1, 'h2c against an HTTP/1-only origin fails');
        c.check(completes2.items.length === 0, 'no silent HTTP/1 fallback delivery');
    } finally {
        await server.quit();
    }
}
