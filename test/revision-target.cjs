"use strict";

// Target tests for Crawler.addRevision().
//
// Every exported group receives the harness `check` and drives a real local
// HTTP server. Ordering is established with manual barriers (held responses,
// a predicate-based request recorder and the drain event), not fixed sleeps.
// An optional second argument ({ fake: true }) injects an in-process
// transport so the exact same group logic can run where binding a local TCP
// port is forbidden. The task harness always calls the groups with `check`.

const http = require("node:http");
const path = require("node:path");
const { Duplex } = require("node:stream");
const { pathToFileURL } = require("node:url");

let CrawlerClass;
async function loadCrawler() {
    if (!CrawlerClass) {
        const moduleUrl = pathToFileURL(path.resolve("dist/index.js")).href;
        CrawlerClass = (await import(moduleUrl)).default;
    }
    return CrawlerClass;
}

const tick = () => new Promise(resolve => setImmediate(resolve));
const ticks = async n => {
    for (let i = 0; i < n; i += 1) await tick();
};

function barrier() {
    let release;
    const promise = new Promise(resolve => {
        release = resolve;
    });
    return { promise, release };
}

// Request log with predicate-based arrival waits (event driven, no polling).
function makeRecorder() {
    const requests = [];
    const waiters = new Set();
    const notify = () => {
        for (const waiter of [...waiters]) waiter();
    };
    return {
        requests,
        record(snapshot) {
            requests.push(snapshot);
            notify();
        },
        until(predicate, label) {
            return new Promise((resolve, reject) => {
                const timer = setTimeout(() => {
                    waiters.delete(check);
                    reject(new Error(`timed out waiting for: ${label}`));
                }, 10000);
                const check = () => {
                    const hit = requests.find(predicate);
                    if (hit) {
                        clearTimeout(timer);
                        waiters.delete(check);
                        resolve(hit);
                    }
                };
                waiters.add(check);
                check();
            });
        },
        count(predicate) {
            return requests.filter(predicate).length;
        },
    };
}

function snapshotOf(req, res) {
    const url = new URL(req.url, "http://localhost");
    const snapshot = {
        method: req.method,
        path: url.pathname,
        headers: { ...req.headers },
        startedAt: Date.now(),
        endedAt: null,
    };
    res.on("finish", () => {
        snapshot.endedAt = Date.now();
    });
    return snapshot;
}

function reply(res, status, body) {
    res.writeHead(status, { "Content-Type": "text/plain" });
    res.end(body);
}

// Default context: a real server on an ephemeral local port.
async function serverContext(handler) {
    const server = http.createServer(handler);
    await new Promise((resolve, reject) => {
        server.once("error", reject);
        server.listen(0, "127.0.0.1", resolve);
    });
    const port = server.address().port;
    return {
        agent: undefined,
        base: urlPath => `http://127.0.0.1:${port}${urlPath}`,
        close: () => new Promise(resolve => server.close(resolve)),
    };
}

// Paired duplex streams standing in for one TCP connection (no real port).
function socketPair() {
    let peer;
    const a = new Duplex({
        read() {},
        write(chunk, _enc, cb) { peer.push(chunk); cb(); },
        final(cb) { peer.push(null); cb(); },
    });
    const b = new Duplex({
        read() {},
        write(chunk, _enc, cb) { a.push(chunk); cb(); },
        final(cb) { a.push(null); cb(); },
    });
    peer = b;
    for (const s of [a, b]) {
        s.setNoDelay = () => {};
        s.setKeepAlive = () => {};
        s.setTimeout = () => s;
        s.ref = () => s;
        s.unref = () => s;
    }
    return [a, b];
}

function fakeRes(socket) {
    const listeners = {};
    return {
        status: 200,
        headers: {},
        writeHead(status, headers) {
            this.status = status;
            this.headers = { ...headers };
        },
        end(body = "") {
            const payload = Buffer.from(body);
            socket.write(`HTTP/1.1 ${this.status} ${http.STATUS_CODES[this.status] || "OK"}\r\n`);
            for (const [key, value] of Object.entries({ ...this.headers, "Content-Length": payload.length })) {
                socket.write(`${key}: ${value}\r\n`);
            }
            socket.write("\r\n");
            socket.write(payload);
            socket.end();
            (listeners.finish || []).forEach(fn => fn());
        },
        on(event, fn) {
            (listeners[event] ||= []).push(fn);
        },
    };
}

// In-process transport used only for sandboxed verification. Recording is
// left to the group handler, exactly as it is for the real server path.
function fakeContext(handler) {
    class FakeAgent extends http.Agent {
        createConnection(opts) {
            const [client, server] = socketPair();
            server.on("data", () => {});
            const req = { method: opts.method, url: opts.path, headers: { ...opts.headers } };
            const res = fakeRes(server);
            Promise.resolve()
                .then(() => handler(req, res))
                .catch(err => client.destroy(err));
            return client;
        }
    }
    return {
        agent: { http: new FakeAgent() },
        base: urlPath => `http://local.test${urlPath}`,
        close: async () => {},
    };
}

async function makeContext(options, handler) {
    if (options && options.fake) return fakeContext(handler);
    return serverContext(handler);
}

// ---------------------------------------------------------------------------
// Group 1: reception and merging while nothing has reached the network yet.
// ---------------------------------------------------------------------------
async function runPending(check, options) {
    const recorder = makeRecorder();
    const blocker = barrier();
    const sameGate = barrier();
    let sameActive = 0;
    let sameMaxConcurrent = 0;

    const handler = (req, res) => {
        const url = new URL(req.url, "http://localhost");
        recorder.record(snapshotOf(req, res));
        if (url.pathname.startsWith("/blocker")) {
            blocker.promise.then(() => reply(res, 200, "blocker"));
            return;
        }
        if (url.pathname === "/same") {
            sameActive += 1;
            sameMaxConcurrent = Math.max(sameMaxConcurrent, sameActive);
            sameGate.promise.then(() => {
                reply(res, 200, `same:${req.headers["x-key"]}`);
                sameActive -= 1;
            });
            return;
        }
        if (url.pathname === "/item") {
            reply(res, 200, `v${req.headers["x-rev"]}`);
            return;
        }
        if (url.pathname === "/validation") {
            reply(res, 200, "ok");
        }
    };
    const ctx = await makeContext(options, handler);
    try {
        const Crawler = await loadCrawler();
        const crawler = new Crawler({
            maxConnections: 3,
            retries: 0,
            jQuery: false,
            silence: true,
            agent: ctx.agent,
        });
        const requestEvents = [];
        crawler.on("request", opts => {
            requestEvents.push({
                path: new URL(opts.url).pathname,
                rateLimiterId: opts.rateLimiterId,
                priority: opts.priority,
                rev: opts.headers ? opts.headers["x-rev"] : undefined,
            });
        });
        let drainCount = 0;
        crawler.on("drain", () => {
            drainCount += 1;
        });

        // Synchronous TypeError contract; nothing existing may be disturbed.
        const invalid = [
            () => crawler.addRevision("", 1, { url: ctx.base("/validation") }),
            () => crawler.addRevision("k", -1, { url: ctx.base("/validation") }),
            () => crawler.addRevision("k", 1.2, { url: ctx.base("/validation") }),
            () => crawler.addRevision("k", Number.MAX_SAFE_INTEGER + 1, { url: ctx.base("/validation") }),
            () => crawler.addRevision("k", "1", { url: ctx.base("/validation") }),
            () => crawler.addRevision("k", 1, { uri: 7 }),
            () => crawler.addRevision("k", 1, { url: () => ctx.base("/validation") }),
            () => crawler.addRevision("k", 1, { html: "<p>" }),
            () => crawler.addRevision("k", 1, { url: ctx.base("/validation"), preRequest: (o, done) => done() }),
            () => crawler.addRevision("k", 1, { url: ctx.base("/validation"), method: "POST" }),
            () => crawler.addRevision("k", 1, [ctx.base("/validation")]),
            () => crawler.addRevision("k", 1, null),
            () => crawler.addRevision("k", 1, {}),
        ];
        for (const fn of invalid) {
            let threw = false;
            try {
                fn();
            } catch (err) {
                threw = err instanceof TypeError;
            }
            check.check(threw, "invalid arguments must throw TypeError synchronously");
        }

        const callbacks = { item: [], same: [], validation: [] };
        // Saturate limiter 0 (the product's final limiter) with held requests,
        // so every revision below stays queued until the blockers release.
        for (let i = 0; i < 3; i += 1) {
            crawler.add({
                url: ctx.base(`/blocker-${i}`),
                callback(error, response, done) {
                    check.check(!error, "blocker request failed");
                    done();
                },
            });
        }
        await recorder.until(r => r.path === "/blocker-2", "third blocker arrival");
        await ticks(2);

        const opt1 = {
            url: ctx.base("/item"), headers: { "x-rev": "1" }, priority: 3, rateLimiterId: 5,
            callback(error, response, done) {
                check.check(false, "superseded revision 1 callback must not run");
                callbacks.item.push(response.body);
                done();
            },
        };
        const opt2 = {
            url: ctx.base("/item"), headers: { "x-rev": "2" }, priority: 1, rateLimiterId: 3,
            callback(error, response, done) {
                check.check(false, "superseded revision 2 callback must not run");
                callbacks.item.push(response.body);
                done();
            },
        };
        const opt3 = {
            url: ctx.base("/item"), headers: { "x-rev": "3", "x-mutate": "orig" }, priority: 0, rateLimiterId: 0,
            callback(error, response, done) {
                check.check(!error, "newest revision callback failed");
                callbacks.item.push(response.body);
                done();
            },
        };
        check.check(crawler.addRevision("product", 1, opt1) === true, "revision 1 accepted");
        check.check(crawler.addRevision("product", 2, opt2) === true, "revision 2 accepted");
        check.check(crawler.addRevision("product", 3, opt3) === true, "revision 3 accepted");
        check.check(crawler.addRevision("product", 3, opt3) === false, "equal revision must be rejected");
        check.check(crawler.addRevision("product", 2, opt2) === false, "older revision must be rejected");

        // Reception-time copy: later caller mutations cannot reach the wire.
        opt3.headers["x-rev"] = "999";
        opt3.headers["x-mutate"] = "changed";
        opt3.priority = 9;

        // Distinct business keys on one URL each fetch, in parallel slots of
        // their own limiter (limiter 0 is fully held by the blockers above).
        for (const key of ["alpha", "beta"]) {
            crawler.addRevision(key, 1, {
                url: ctx.base("/same"),
                rateLimiterId: 9,
                headers: { "x-key": key },
                callback(error, response, done) {
                    check.check(!error, `same ${key} failed`);
                    callbacks.same.push(response.body);
                    done();
                },
            });
        }
        await recorder.until(r => r.path === "/same" && r.headers["x-key"] === "alpha", "same alpha arrival");
        await recorder.until(r => r.path === "/same" && r.headers["x-key"] === "beta", "same beta arrival");
        await ticks(2);
        check.check(sameMaxConcurrent === 2, "distinct keys must occupy two pool slots concurrently");
        check.check(recorder.count(r => r.path === "/item") === 0, "merged revision must not fetch while blocked");

        // A valid call after the validation failures still works.
        const validAccepted = crawler.addRevision("validation-key", 0, {
            uri: ctx.base("/validation"),
            callback(error, response, done) {
                check.check(!error, "validation-key request failed");
                callbacks.validation.push(response.body);
                done();
            },
        });
        check.check(validAccepted === true, "valid revision accepted after rejected calls");

        blocker.release();
        const itemRequest = await recorder.until(r => r.path === "/item", "merged item arrival");
        check.check(recorder.count(r => r.path === "/item") === 1, "only one queued position for the merged key");
        check.check(itemRequest.method === "GET", "revision request must be GET");
        check.check(itemRequest.headers["x-rev"] === "3", "newest revision config must be sent");
        check.check(itemRequest.headers["x-mutate"] === "orig", "options must be copied at reception");
        const itemEvent = requestEvents.find(e => e.path === "/item");
        check.check(!!itemEvent, "request event for the merged item is missing");
        check.check(itemEvent.rateLimiterId === 0, "updated rateLimiterId must take effect");
        check.check(itemEvent.priority === 0, "updated priority must take effect");

        sameGate.release();
        await new Promise(resolve => crawler.once("drain", resolve));
        check.check(drainCount === 1, "exactly one drain for the pending group");
        check.check(JSON.stringify(callbacks.item) === JSON.stringify(["v3"]), "only the newest revision callback runs");
        check.check(callbacks.same.length === 2, "both distinct keys callback exactly once");
        check.check(callbacks.validation.length === 1, "validation-key callback runs once");
        check.check(recorder.requests.every(r => r.method === "GET"), "all requests are GET");
    } finally {
        await ctx.close();
    }
}

module.exports.revisionPending = (check, options = null) => runPending(check, options);

// ---------------------------------------------------------------------------
// Group 2: an older revision is already downloading when a newer one arrives.
// ---------------------------------------------------------------------------
async function runInFlight(check, options) {
    const recorder = makeRecorder();
    const holdSuccess = barrier();
    const holdError = barrier();

    const handler = (req, res) => {
        const url = new URL(req.url, "http://localhost");
        recorder.record(snapshotOf(req, res));
        if (url.pathname === "/fly") {
            // The first revision is held in flight; the successor answers at once.
            if (req.headers["x-rev"] === "1") {
                holdSuccess.promise.then(() => reply(res, 200, "v1"));
            } else {
                reply(res, 200, `v${req.headers["x-rev"]}`);
            }
            return;
        }
        if (url.pathname === "/fly-err") {
            if (req.headers["x-rev"] === "1") {
                holdError.promise.then(() => reply(res, 500, "boom"));
            } else {
                reply(res, 200, `v${req.headers["x-rev"]}`);
            }
            return;
        }
        reply(res, 200, `v${req.headers["x-rev"]}`);
    };
    const ctx = await makeContext(options, handler);
    try {
        const Crawler = await loadCrawler();
        const crawler = new Crawler({
            maxConnections: 2,
            retries: 0,
            jQuery: false,
            silence: true,
            agent: ctx.agent,
        });
        let drainCount = 0;
        crawler.on("drain", () => {
            drainCount += 1;
        });

        // Variant A: the superseded download would have succeeded.
        const callbacksA = { old: [], latest: [] };
        crawler.addRevision("fly-key", 1, {
            url: ctx.base("/fly"),
            headers: { "x-rev": "1" },
            callback(error, response, done) {
                callbacksA.old.push(error ? "error" : response.body);
                done();
            },
        });
        const firstA = await recorder.until(r => r.path === "/fly" && r.headers["x-rev"] === "1", "rev1 flying");
        crawler.addRevision("fly-key", 2, {
            url: ctx.base("/fly"),
            headers: { "x-rev": "2" },
            callback(error, response, done) {
                check.check(!error, "rev2 callback failed");
                callbacksA.latest.push(response.body);
                done();
            },
        });
        await ticks(3);
        check.check(recorder.count(r => r.path === "/fly" && r.headers["x-rev"] === "2") === 0,
            "successor must wait while the old download is in flight");
        holdSuccess.release();
        const secondA = await recorder.until(r => r.path === "/fly" && r.headers["x-rev"] === "2", "rev2 arrival");
        await new Promise(resolve => crawler.once("drain", resolve));
        check.check(callbacksA.old.length === 0, "superseded success callback must be dropped");
        check.check(JSON.stringify(callbacksA.latest) === JSON.stringify(["v2"]), "only newest callback fires (A)");
        check.check(secondA.startedAt >= firstA.endedAt, "successor starts only after the old download ends");
        check.check(drainCount === 1, "one drain across the success handoff");

        // Variant B: the superseded download fails; its error is dropped too.
        const callbacksB = { old: [], latest: [] };
        crawler.addRevision("err-key", 1, {
            url: ctx.base("/fly-err"),
            headers: { "x-rev": "1" },
            callback(error, response, done) {
                callbacksB.old.push(error ? "error" : response.body);
                done();
            },
        });
        const firstB = await recorder.until(r => r.path === "/fly-err" && r.headers["x-rev"] === "1", "rev1 error flying");
        crawler.addRevision("err-key", 2, {
            url: ctx.base("/fly-err"),
            headers: { "x-rev": "2" },
            callback(error, response, done) {
                check.check(!error, "rev2 callback after failed old revision failed");
                callbacksB.latest.push(response.body);
                done();
            },
        });
        holdError.release();
        const secondB = await recorder.until(r => r.path === "/fly-err" && r.headers["x-rev"] === "2", "rev2 after error arrival");
        await new Promise(resolve => crawler.once("drain", resolve));
        check.check(callbacksB.old.length === 0, "superseded error callback must be dropped");
        check.check(JSON.stringify(callbacksB.latest) === JSON.stringify(["v2"]), "only newest callback fires (B)");
        check.check(secondB.startedAt >= firstB.endedAt, "successor starts only after the failed download ends");
        check.check(recorder.count(r => r.path === "/fly-err" && r.headers["x-rev"] === "1") === 1,
            "the failed old revision is not retried");
        check.check(drainCount === 2, "one drain per completed handoff");
    } finally {
        await ctx.close();
    }
}

module.exports.revisionInFlight = (check, options = null) => runInFlight(check, options);

// ---------------------------------------------------------------------------
// Group 3: updates landing inside the retry backoff invalidate it at once.
// ---------------------------------------------------------------------------
async function runRetry(check, options) {
    const recorder = makeRecorder();
    const retryInterval = 1200;
    // rev2 fails once, then succeeds, exercising its own fresh retry budget.
    const rev2FailuresLeft = { value: 1 };

    const handler = (req, res) => {
        const url = new URL(req.url, "http://localhost");
        recorder.record(snapshotOf(req, res));
        if (url.pathname === "/backoff") {
            if (req.headers["x-rev"] === "1") {
                reply(res, 500, "boom");
                return;
            }
            if (rev2FailuresLeft.value > 0) {
                rev2FailuresLeft.value -= 1;
                reply(res, 503, "transient");
                return;
            }
            reply(res, 200, "v2");
            return;
        }
        reply(res, 200, "ok");
    };
    const ctx = await makeContext(options, handler);
    try {
        const Crawler = await loadCrawler();
        const crawler = new Crawler({
            maxConnections: 1,
            retries: 2,
            retryInterval,
            jQuery: false,
            silence: true,
            agent: ctx.agent,
        });
        let drainCount = 0;
        crawler.on("drain", () => {
            drainCount += 1;
        });
        const callbacks = { old: [], latest: [] };

        crawler.addRevision("retry-key", 1, {
            url: ctx.base("/backoff"),
            headers: { "x-rev": "1" },
            callback(error, response, done) {
                callbacks.old.push(error ? "error" : response.body);
                done();
            },
        });
        await recorder.until(r => r.path === "/backoff" && r.headers["x-rev"] === "1", "rev1 first failure");
        // The first failure parks the attempt inside retryInterval with the slot held.
        await ticks(5);
        check.check(recorder.count(r => r.path === "/backoff" && r.headers["x-rev"] === "1") === 1,
            "old revision must be waiting in backoff, not retrying yet");

        const advancedAt = Date.now();
        crawler.addRevision("retry-key", 2, {
            url: ctx.base("/backoff"),
            headers: { "x-rev": "2" },
            retries: 1,
            callback(error, response, done) {
                check.check(!error, "rev2 ultimately failed");
                callbacks.latest.push(response.body);
                done();
            },
        });
        // Successor must move immediately rather than waiting out the old backoff.
        await recorder.until(r => r.path === "/backoff" && r.headers["x-rev"] === "2", "rev2 dispatched at once");
        check.check(Date.now() - advancedAt < retryInterval / 2, "old backoff must not delay the successor");
        await new Promise(resolve => crawler.once("drain", resolve));
        check.check(JSON.stringify(callbacks.latest) === JSON.stringify(["v2"]),
            "rev2 succeeds using its own retry budget");
        check.check(callbacks.old.length === 0, "old revision callback never fires");

        // Outlive the dead backoff: its timer must never resurrect revision 1.
        const oldHitsBefore = recorder.count(r => r.path === "/backoff" && r.headers["x-rev"] === "1");
        await new Promise(resolve => setTimeout(resolve, retryInterval + 300));
        const oldHitsAfter = recorder.count(r => r.path === "/backoff" && r.headers["x-rev"] === "1");
        check.check(oldHitsBefore === 1 && oldHitsAfter === 1, "invalidated retry timer never fires");
        check.check(recorder.count(r => r.path === "/backoff" && r.headers["x-rev"] === "2") === 2,
            "rev2 used one retry then succeeded");
        check.check(drainCount === 1, "no drain flicker during the backoff handoff");
    } finally {
        await ctx.close();
    }
}

module.exports.revisionRetry = (check, options = null) => runRetry(check, options);

// ---------------------------------------------------------------------------
// Group 4: callback/done settlement and high-water-mark lifecycle.
// ---------------------------------------------------------------------------
async function runRelease(check, options) {
    const recorder = makeRecorder();
    const firstCallbackGate = barrier();

    const handler = (req, res) => {
        const url = new URL(req.url, "http://localhost");
        recorder.record(snapshotOf(req, res));
        if (url.pathname === "/lifecycle") {
            reply(res, 200, `v${req.headers["x-rev"]}`);
            return;
        }
        reply(res, 200, "ok");
    };
    const ctx = await makeContext(options, handler);
    try {
        const Crawler = await loadCrawler();
        const crawler = new Crawler({
            maxConnections: 1,
            retries: 0,
            jQuery: false,
            silence: true,
            agent: ctx.agent,
        });
        let drainCount = 0;
        crawler.on("drain", () => {
            drainCount += 1;
        });
        const callbacks = [];
        let firstDone;

        crawler.addRevision("life-key", 1, {
            url: ctx.base("/lifecycle"),
            headers: { "x-rev": "1" },
            callback(error, response, done) {
                check.check(!error, "rev1 callback failed");
                callbacks.push("cb1");
                // Updates arriving while the callback is still running: two of
                // them must collapse into the newest revision only.
                crawler.addRevision("life-key", 2, {
                    url: ctx.base("/lifecycle"),
                    headers: { "x-rev": "2" },
                    callback() {
                        check.check(false, "superseded rev2 callback must not run");
                        arguments[2]();
                    },
                });
                crawler.addRevision("life-key", 3, {
                    url: ctx.base("/lifecycle"),
                    headers: { "x-rev": "3" },
                    callback(err, res, d) {
                        check.check(!err, "rev3 callback failed");
                        callbacks.push(`cb3:${res.body}`);
                        d();
                    },
                });
                firstDone = done;
                firstCallbackGate.release();
            },
        });
        await firstCallbackGate.promise;
        await ticks(3);
        check.check(recorder.count(r => r.path === "/lifecycle" && r.headers["x-rev"] === "3") === 0,
            "successor must wait for the running callback's done");
        check.check(drainCount === 0, "no drain while a callback is still open");

        // Repeated done settles exactly once.
        firstDone();
        firstDone();
        firstDone();
        await recorder.until(r => r.path === "/lifecycle" && r.headers["x-rev"] === "3", "rev3 after done");
        await new Promise(resolve => crawler.once("drain", resolve));
        check.check(JSON.stringify(callbacks) === JSON.stringify(["cb1", "cb3:v3"]),
            "callback-time updates collapse to the newest one");
        check.check(recorder.count(r => r.path === "/lifecycle") === 2, "one download per settled revision");
        check.check(drainCount === 1, "drain fires once after the whole lifecycle");

        // The high-water mark outlives the drained state.
        check.check(crawler.addRevision("life-key", 3, {
            url: ctx.base("/lifecycle"),
            headers: { "x-rev": "3" },
        }) === false, "equal revision after drain is still rejected");
        check.check(crawler.addRevision("life-key", 10, {
            url: ctx.base("/lifecycle"),
            headers: { "x-rev": "10" },
            callback(err, res, done) {
                check.check(!err, "rev10 callback failed");
                callbacks.push("cb10");
                done();
            },
        }) === true, "a newer revision after drain is accepted");
        await new Promise(resolve => crawler.once("drain", resolve));
        check.check(callbacks.includes("cb10"), "post-drain newer revision runs");
        check.check(drainCount === 2, "each completed lifecycle drains once");
    } finally {
        await ctx.close();
    }
}

module.exports.revisionRelease = (check, options = null) => runRelease(check, options);
