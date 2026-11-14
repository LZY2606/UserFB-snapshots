/*
 * Scheduling contract regression.
 *
 * Three groups registered through task_support/check.cjs:
 *   "Pool dispatch", "Retry occupancy", "Rate calendar"
 *
 * Design constraints (see test/scheduling-model.md):
 * - Drives the real built Crawler.add pipeline (dist/, never source internals).
 * - HTTP is a real node:http.Server reached over an in-process duplex
 *   transport. No TCP port is opened (the sandbox forbids binding), there is
 *   no online dependency, no browser and no nock.
 * - A separate work ledger records enqueued items, reserved slots (observed
 *   as virtual dispatch timers), network attempts, user callbacks, done() and
 *   drain. Expected values are derived from this ledger only, never from the
 *   crawler's private _limiters runtime state.
 * - The virtual clock replaces only the two scheduling timers used by the
 *   built code (dist/rateLimiter/rateLimiter.js dispatch timer and
 *   dist/crawler.js retry timer). Real HTTP responses, Crawler and the parse
 *   chain run on the real event loop and are never stubbed.
 */
import http from "node:http";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { Duplex } from "node:stream";
import checkPkg from "../task_support/check.cjs";

const { group } = checkPkg;

const HERE = path.dirname(fileURLToPath(import.meta.url));
const CRAWLER_ENTRY = process.env.SCHED_CRAWLER_ENTRY
    ? path.resolve(process.env.SCHED_CRAWLER_ENTRY)
    : path.resolve(HERE, "..", "dist", "index.js");
const { default: Crawler } = await import(pathToFileURL(CRAWLER_ENTRY).href);

const BASE_URL = "http://sched.local";
const GROUP_NAMES = ["Pool dispatch", "Retry occupancy", "Rate calendar"];

const show = value => JSON.stringify(value);

function eq(c, actual, expected, label) {
    c.check(show(actual) === show(expected), `${label} -- expected ${show(expected)}, actual ${show(actual)}`);
}

function truthy(c, value, label) {
    c.check(Boolean(value), `${label} -- expected truthy, actual ${show(value)}`);
}

/* ------------------------------------------------------------------ */
/* In-process HTTP transport: real http.Server, zero TCP sockets.     */
/* ------------------------------------------------------------------ */

function duplexPair() {
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
    return [a, b];
}

/**
 * Real node:http.Server fed through paired Duplex streams instead of TCP.
 * Every request is a genuine HTTP/1.1 transaction parsed by Node itself.
 */
function createLocalServer(router) {
    const hits = [];
    const server = http.createServer(async (req, res) => {
        const rec = {
            path: req.url,
            attempt: hits.filter(h => h.path === req.url).length + 1,
            status: 0,
        };
        hits.push(rec);
        try {
            const answer = await router(req, rec);
            rec.status = answer.status;
            res.writeHead(answer.status, { "content-type": "text/plain; charset=ascii" });
            res.end(answer.body);
        } catch {
            rec.status = 500;
            if (!res.headersSent) {
                res.writeHead(500, { "content-type": "text/plain" });
                res.end("router-error-ascii");
            }
        }
    });
    const agent = {
        http: {
            addRequest(req) {
                const [clientSide, serverSide] = duplexPair();
                server.emit("connection", serverSide);
                req.onSocket(clientSide);
            },
        },
    };
    return {
        server,
        agent,
        hits,
        // Call only AFTER the virtual clock has been restored: this uses a
        // real-timer fallback and the server never owned a listening socket.
        async close() {
            await new Promise(resolve => {
                try { server.closeAllConnections(); } catch { /* no listening socket */ }
                try { server.close(() => resolve()); } catch { resolve(); }
                setTimeout(resolve, 50);
            });
        },
    };
}

function gate() {
    let release;
    const promise = new Promise(r => { release = r; });
    return { promise, release };
}

/* ------------------------------------------------------------------ */
/* Virtual clock: scheduling timers only.                              */
/* ------------------------------------------------------------------ */

const DISPATCH_FRAME = "/dist/rateLimiter/rateLimiter.js";
const RETRY_FRAME = "/dist/crawler.js";

function installVirtualClock(start = 1000) {
    const realSetTimeout = globalThis.setTimeout;
    const realClearTimeout = globalThis.clearTimeout;
    const realDateNow = Date.now.bind(Date);
    const turn = () => new Promise(resolve => realSetTimeout(resolve, 0));

    let now = start;
    let seq = 0;
    const entries = [];
    let onTimer = null;

    const callerKind = () => {
        const stack = new Error().stack;
        if (stack.includes(DISPATCH_FRAME)) return "dispatch";
        if (stack.includes(RETRY_FRAME)) return "retry";
        return null;
    };

    class VirtualHandle {
        constructor(id, kind) { this.id = id; this.kind = kind; }
    }

    globalThis.setTimeout = (fn, ms = 0, ...args) => {
        const kind = callerKind();
        if (!kind) return realSetTimeout(fn, ms, ...args);
        const handle = new VirtualHandle(++seq, kind);
        const entry = {
            handle, kind, scheduledAt: now, due: now + ms,
            fn, args, firedAt: null, cancelled: false,
        };
        entries.push(entry);
        if (onTimer) onTimer(entry);
        return handle;
    };
    globalThis.clearTimeout = handle => {
        const entry = handle instanceof VirtualHandle ? entries.find(e => e.handle === handle) : null;
        if (entry) entry.cancelled = true;
        else realClearTimeout(handle);
    };
    Date.now = () => now;

    async function advance(ms) {
        const target = now + ms;
        for (;;) {
            const pending = entries
                .filter(e => e.firedAt === null && !e.cancelled)
                .sort((a, b) => a.due - b.due);
            if (!pending.length || pending[0].due > target) break;
            const entry = pending[0];
            now = entry.due;
            entry.firedAt = now;
            entry.fn(...entry.args);
            await turn(); // let the real HTTP parser / promise chain progress
        }
        now = target;
        await turn();
    }

    return {
        advance,
        turn,
        entries,
        realSetTimeout,
        set onTimer(fn) { onTimer = fn; },
        get now() { return now; },
        restore() {
            globalThis.setTimeout = realSetTimeout;
            globalThis.clearTimeout = realClearTimeout;
            Date.now = realDateNow;
        },
    };
}

/* ------------------------------------------------------------------ */
/* Work ledger: completely separate from crawler internals.           */
/* ------------------------------------------------------------------ */

function createLedger(clock) {
    const log = [];
    const tasks = new Map();
    const pendingDispatch = []; // dispatch timers armed but not yet matched to a preRequest
    const pendingRetry = []; // retry timers armed but not yet matched to a second request
    const record = (type, data = {}) => log.push({ type, at: clock.now, ...data });

    return {
        log,
        tasks,
        // A dispatch timer being armed is the externally observable instant the
        // pool reserved a slot (the source increments runningSize and arms the
        // timer together). A retry timer records the slot-held retry wait.
        timerArmed(entry) {
            if (entry.kind === "dispatch") {
                pendingDispatch.push(entry);
                record("reserve", { timerId: entry.handle.id, due: entry.due });
            } else {
                pendingRetry.push(entry);
                record("retryWait", { timerId: entry.handle.id, due: entry.due });
            }
        },
        enqueue(task) {
            tasks.set(task.id, {
                ...task,
                reservations: [],
                pre: [], requestEvents: [], hits: [], callbacks: [], dones: [],
            });
            record("enqueue", { id: task.id, path: task.path, pool: task.pool, priority: task.priority });
        },
        schedule(options) { record("schedule", { id: options.userParams?.id ?? null }); },
        pre(options) {
            const task = tasks.get(options.userParams.id);
            const entry = pendingDispatch.shift();
            // Only a fresh pool dispatch is a new reservation. A retried attempt
            // re-enters _execute directly and therefore finds no dispatch timer.
            if (entry) task.reservations.push({ at: entry.scheduledAt, due: entry.due, timerId: entry.handle.id });
            task.pre.push(clock.now);
            record("preRequest", { id: task.id, attempt: task.pre.length });
        },
        request(options) {
            const task = tasks.get(options.userParams.id);
            // A pending retry timer belongs to the same logical request's next attempt.
            if (pendingRetry.length) {
                const entry = pendingRetry.shift();
                record("retryBound", { id: task.id, timerId: entry.handle.id });
            }
            task.requestEvents.push({ at: clock.now, retries: options.retries, ref: options });
            record("request", { id: task.id, retries: options.retries });
        },
        hit(rec, id) {
            rec.at = clock.now; // the server mutates rec.status once it answers
            record("hit", { path: rec.path, attempt: rec.attempt, id });
            if (id) tasks.get(id).hits.push(rec);
        },
        callbackEntry(id, err, res) {
            const task = tasks.get(id);
            task.callbacks.push({ at: clock.now, error: Boolean(err), status: err ? null : res.statusCode });
            record("callback", { id, error: Boolean(err) });
        },
        done(id) {
            tasks.get(id).dones.push(clock.now);
            record("done", { id });
        },
        drain() { record("drain", {}); },
        waiting(pool) {
            return [...tasks.values()].filter(t => t.pool === pool && t.pre.length === 0).map(t => t.id);
        },
        dispatchTime(id) { return tasks.get(id).pre[0]; },
        drainCount() { return log.filter(e => e.type === "drain").length; },
    };
}

function createCrawler(options) {
    return new Crawler({
        homogeneous: false,
        skipDuplicates: false,
        jQuery: false,
        silence: true,
        ...options,
    });
}

let taskSeq = 0;

/**
 * Build one queued task and bind every ledger touchpoint.
 * - hold: gate awaited inside the user callback BEFORE done(), proving the
 *   slot remains reserved until the user releases it.
 * - onCallback: hook run inside the user callback (for reentrant add()).
 */
function addTracked(crawler, ledger, harness, spec) {
    const id = spec.id ?? `task-${++taskSeq}`;
    const options = {
        url: BASE_URL + spec.path,
        rateLimiterId: spec.pool,
        priority: spec.priority,
        agent: harness.agent,
        retries: spec.retries,
        retryInterval: spec.retryInterval,
        userParams: { id, path: spec.path },
        preRequest: (opts, next) => { ledger.pre(opts); next(); },
        callback: async (err, res, done) => {
            const task = ledger.tasks.get(id);
            let doneCalls = 0;
            const finish = () => {
                doneCalls += 1;
                ledger.done(id);
                done();
            };
            task.finish = finish;
            task.doneCalls = () => doneCalls;
            task.error = err;
            task.lastRes = res;
            ledger.callbackEntry(id, err, err ? { statusCode: null } : res);
            if (spec.hold) await spec.hold.promise;
            if (spec.onCallback) await spec.onCallback({ err, res, finish, id, crawler, ledger, harness });
            if (doneCalls === 0) finish();
        },
    };
    ledger.enqueue({
        id, path: spec.path, pool: spec.pool, priority: spec.priority,
        options, retries: spec.retries,
    });
    harness.pathToId[spec.path] = id;
    crawler.add(options);
    return id;
}

async function settleUntilDrain(clock, flag, turns = 500, step = 25) {
    for (let i = 0; i < turns; i += 1) {
        if (flag.done) return;
        await clock.advance(step);
    }
    throw new Error("settle budget exhausted before drain");
}

function bindDrain(crawler, ledger) {
    const flag = { done: false };
    crawler.on("drain", () => { ledger.drain(); flag.done = true; });
    return flag;
}

function nextDrain(crawler) {
    const flag = { done: false };
    crawler.once("drain", () => { flag.done = true; });
    return flag;
}

function checkPerTaskInvariants(c, ledger) {
    for (const [id, task] of ledger.tasks) {
        eq(c, task.requestEvents.length, task.hits.length, `${id}: one server hit per request event`);
        for (let i = 0; i < task.hits.length; i += 1) {
            truthy(c, task.requestEvents[i] && task.requestEvents[i].at <= task.hits[i].at,
                `${id} attempt ${i + 1}: request event before wire hit`);
        }
        // Exactly one pool reservation per logical task: retries keep the slot
        // and therefore never create a second reservation.
        eq(c, task.reservations.length, 1, `${id}: exactly one reserved slot for the task's whole life`);
        truthy(c, task.reservations[0].at <= task.pre[0],
            `${id}: the slot is reserved before the first preRequest`);
        truthy(c, task.pre[0] <= task.reservations[0].due,
            `${id}: the first dispatch happens no earlier than the reservation calendar point`);
        truthy(c, task.callbacks.length === 1, `${id}: exactly one user callback`);
        truthy(c, task.hits.length >= 1, `${id}: at least one network attempt`);
        truthy(c, task.callbacks[0].at >= task.hits[task.hits.length - 1].at,
            `${id}: callback after the final response`);
        eq(c, task.doneCalls(), 1, `${id}: done() called exactly once`);
        truthy(c, task.dones[0] >= task.callbacks[0].at, `${id}: done after callback`);
    }
    // Relative order: a callback/done can never appear before its own request.
    const seenRequests = new Set();
    for (const event of ledger.log) {
        if (event.type === "request") seenRequests.add(event.id);
        if (event.type === "callback" || event.type === "done") {
            truthy(c, seenRequests.has(event.id), `${event.type} for ${event.id} cannot precede its request`);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Group 1: Pool dispatch, priority/FIFO, reservation, reentrant add.  */
/* ------------------------------------------------------------------ */

group("Pool dispatch", async c => {
    console.log("GROUP_START Pool dispatch");
    const clock = installVirtualClock(1000);
    const harness = { agent: null, pathToId: {} };
    const ledger = createLedger(clock);
    clock.onTimer = entry => ledger.timerArmed(entry);
    const blockerGate = gate();
    const specialRoutes = {
        "/blocker": async () => { await blockerGate.promise; return { status: 200, body: "ascii:blocker" }; },
    };
    const local = createLocalServer(async (req, rec) => {
        const id = harness.pathToId[req.url] ?? null;
        ledger.hit(rec, id);
        if (specialRoutes[req.url]) return specialRoutes[req.url](rec);
        return { status: 200, body: `ascii:${req.url}` };
    });
    harness.agent = local.agent;
    const crawler = createCrawler({ maxConnections: 1, retries: 0 });
    crawler.on("schedule", options => ledger.schedule(options));
    crawler.on("request", options => ledger.request(options));
    const drained = bindDrain(crawler, ledger);

    try {
        addTracked(crawler, ledger, harness, { id: "blocker", path: "/blocker", pool: 1, priority: 2 });
        await clock.advance(0);
        eq(c, local.hits.map(h => h.path), ["/blocker"], "blocker reserved and dispatched immediately");

        addTracked(crawler, ledger, harness, { id: "alpha", path: "/alpha", pool: 1, priority: 2 });
        addTracked(crawler, ledger, harness, { id: "beta", path: "/beta", pool: 1, priority: 2 });
        addTracked(crawler, ledger, harness, {
            id: "urgent", path: "/urgent", pool: 1, priority: 0,
            onCallback: () => {
                // Reentrant add() inside a user callback: joins the current
                // queue tail and is dispatched after everything already queued.
                addTracked(crawler, ledger, harness, { id: "extra", path: "/extra", pool: 1, priority: 2 });
            },
        });
        addTracked(crawler, ledger, harness, { id: "parallel", path: "/parallel", pool: 2, priority: 1 });
        await clock.advance(0);

        // A reserved slot (plus its armed timer) cannot be stolen by a later
        // high-priority task; priority only reorders what is still waiting.
        eq(c, local.hits.map(h => h.path), ["/blocker", "/parallel"],
            "later tasks cannot steal the reserved pool-1 slot; pool-2 still moves");
        eq(c, ledger.waiting(1), ["alpha", "beta", "urgent"], "pool-1 waiting ledger while the slot is held");
        truthy(c, ledger.tasks.get("blocker").callbacks.length === 0,
            "blocker response (and callback) stay gated on the server side");
        for (let i = 0; i < 50; i += 1) await clock.advance(100); // 5000 virtual ms
        eq(c, local.hits.filter(h => ledger.tasks.get(harness.pathToId[h.path])?.pool === 1).map(h => h.path),
            ["/blocker"], "virtual time alone never dispatches a waiting task into a held slot");
        truthy(c, ledger.tasks.get("parallel").dones.length === 1,
            "the other pool completes request -> callback -> done while pool-1 is held");

        blockerGate.release();
        await settleUntilDrain(clock, drained);

        // Cross-pool relative ordering, checked on the ledger timeline.
        const parallelDoneAt = ledger.tasks.get("parallel").dones[0];
        const blockerCallbackAt = ledger.tasks.get("blocker").callbacks[0].at;
        truthy(c, parallelDoneAt < blockerCallbackAt,
            "pool-2 request/callback/done all precede the still-gated blocker callback");

        const poolOneHitOrder = local.hits
            .filter(h => ledger.tasks.get(harness.pathToId[h.path])?.pool === 1)
            .map(h => h.path);
        eq(c, poolOneHitOrder, ["/blocker", "/urgent", "/alpha", "/beta", "/extra"],
            "pool-1 wire order: reserved blocker, then priority, then same-priority FIFO, then reentrant tail");
        eq(c, local.hits.map(h => h.path), ["/blocker", "/parallel", "/urgent", "/alpha", "/beta", "/extra"],
            "full wire path order including the independent pool, exact paths and counts");
        eq(c, ledger.log.filter(e => e.type === "request").map(e => e.id),
            ["blocker", "parallel", "urgent", "alpha", "beta", "extra"],
            "request events follow dispatch order, not just final callback totals");
        eq(c, ledger.log.filter(e => e.type === "schedule").length, ledger.tasks.size,
            "every add() produced exactly one schedule ledger entry");
        eq(c, ledger.log.filter(e => e.type === "reserve").length, ledger.tasks.size,
            "exactly one slot reservation per queued task (none for still-waiting or send() tasks)");
        // Reservation order inside pool 1 is the same as the wire dispatch order.
        // Timer ids are monotonic at arm time, which breaks same-instant ties
        // that the reservation timestamp alone cannot order.
        eq(c, [...ledger.tasks.values()].filter(t => t.pool === 1)
            .sort((a, b) => a.reservations[0].timerId - b.reservations[0].timerId).map(t => t.path),
            ["/blocker", "/urgent", "/alpha", "/beta", "/extra"],
            "pool-1 reservation timeline matches the priority/FIFO dispatch order");

        checkPerTaskInvariants(c, ledger);
        eq(c, ledger.drainCount(), 1, "exactly one drain");
        eq(c, ledger.log.at(-1).type, "drain", "drain is the final ledger event");

        // send() bypasses the shared pools: it is not a dispatch entry in this
        // ledger even though the real server receives one more HTTP request.
        const sendHitsBefore = local.hits.length;
        const scheduleBefore = ledger.log.filter(e => e.type === "schedule").length;
        const sendResult = await crawler.send({ url: BASE_URL + "/send-only", agent: local.agent });
        eq(c, sendResult.body, "ascii:/send-only", "send() still traverses the real HTTP stack");
        eq(c, local.hits.length, sendHitsBefore + 1, "send() reaches the server exactly once");
        eq(c, local.hits.at(-1).path, "/send-only", "send() wire hit is the send-only path");
        eq(c, ledger.log.filter(e => e.type === "schedule").length, scheduleBefore,
            "send() creates no pool schedule ledger entry");
        eq(c, ledger.drainCount(), 1, "send() does not produce a pool drain");
        await clock.turn();
    } finally {
        clock.restore();
        await local.close();
    }
});

/* ------------------------------------------------------------------ */
/* Group 2: Retry occupancy, exhaustion, cross-pool independence.      */
/* ------------------------------------------------------------------ */

group("Retry occupancy", async c => {
    console.log("GROUP_START Retry occupancy");
    const clock = installVirtualClock(2000);
    const harness = { agent: null, pathToId: {} };
    const ledger = createLedger(clock);
    clock.onTimer = entry => ledger.timerArmed(entry);
    const secondAttemptGate = gate();
    const callbackGate = gate();
    const specialRoutes = {
        "/flaky": async rec => rec.attempt === 1
            ? { status: 503, body: "ascii:fail" }
            : (await secondAttemptGate.promise, { status: 200, body: "ascii:flaky-ok" }),
        "/doomed": async () => ({ status: 503, body: "ascii:doomed" }),
        "/got-off": async () => ({ status: 503, body: "ascii:got-off" }),
    };
    const local = createLocalServer(async (req, rec) => {
        const id = harness.pathToId[req.url] ?? null;
        ledger.hit(rec, id);
        if (specialRoutes[req.url]) return specialRoutes[req.url](rec);
        return { status: 200, body: `ascii:${req.url}` };
    });
    harness.agent = local.agent;
    const crawler = createCrawler({ maxConnections: 1, retries: 1, retryInterval: 100 });
    crawler.on("schedule", options => ledger.schedule(options));
    crawler.on("request", options => ledger.request(options));
    const drained = bindDrain(crawler, ledger);

    try {
        addTracked(crawler, ledger, harness, {
            id: "flaky", path: "/flaky", pool: 1, priority: 2,
            // The success callback waits here: the slot must stay reserved
            // between "response handed to user" and the user's done().
            hold: callbackGate,
        });
        addTracked(crawler, ledger, harness, { id: "high", path: "/high", pool: 1, priority: 0 });
        addTracked(crawler, ledger, harness, { id: "p2-a", path: "/p2-a", pool: 2, priority: 1 });
        addTracked(crawler, ledger, harness, { id: "p2-b", path: "/p2-b", pool: 2, priority: 1 });
        await clock.advance(0);

        // First attempt fails immediately; the outer Crawler retry arms a
        // virtual 100ms timer while the pool slot stays with this request.
        eq(c, ledger.tasks.get("flaky").hits.map(h => h.status), [503], "first flaky attempt is a real wire 503");
        const retryTimer = clock.entries.find(e => e.kind === "retry");
        truthy(c, Boolean(retryTimer), "outer retry timer scheduled by Crawler._handler");
        eq(c, retryTimer.due - retryTimer.scheduledAt, 100, "retryInterval controls the outer retry wait");
        eq(c, ledger.log.filter(e => e.type === "retryWait").length, 1,
            "the retry wait is booked as a held-slot ledger event, not a new reservation");
        eq(c, ledger.tasks.get("flaky").reservations.length, 1,
            "the retried request keeps its original slot reservation across both attempts");
        eq(c, ledger.waiting(1), ["high"], "high-priority task stays queued during the retry wait");

        await clock.advance(50); // halfway through the retry window (t = 2050)
        eq(c, local.hits.filter(h => ledger.tasks.get(harness.pathToId[h.path])?.pool === 1).map(h => h.path),
            ["/flaky"], "pool-1 slot remains reserved mid-retry; high cannot steal it");
        truthy(c, ledger.tasks.get("p2-a").dones.length === 1 && ledger.tasks.get("p2-b").dones.length === 1,
            "pool-2 drains both tasks while pool-1 is parked in retry");
        const poolTwoDoneAt = Math.max(ledger.tasks.get("p2-a").dones[0], ledger.tasks.get("p2-b").dones[0]);
        truthy(c, poolTwoDoneAt < retryTimer.due, "one pool's callback pause never blocks the other pool");

        await clock.advance(60); // t = 2110: crosses t+100, the retry timer fires
        eq(c, ledger.tasks.get("flaky").pre.length, 2, "second attempt re-runs the original preRequest");
        eq(c, ledger.tasks.get("flaky").requestEvents.map(e => e.retries), [1, 0],
            "request events show outer retries decrementing 1 -> 0 on one logical request");
        eq(c, local.hits.filter(h => h.path === "/flaky").length, 2, "two real HTTP attempts for one logical request");
        truthy(c, ledger.tasks.get("flaky").requestEvents.every(e => e.ref === ledger.tasks.get("flaky").options),
            "both attempts reuse the original options object");
        eq(c, ledger.waiting(1), ["high"], "high remains queued while the second response is gated");
        eq(c, ledger.tasks.get("flaky").callbacks.length, 0, "no user callback before the gated response is released");

        secondAttemptGate.release();
        await clock.advance(0);
        eq(c, ledger.tasks.get("flaky").callbacks.length, 1, "the success response is handed to the user callback");
        eq(c, local.hits.filter(h => h.path === "/high").length, 0,
            "entering the callback does not release the slot: high is still waiting before done()");

        callbackGate.release(); // user finally calls done()
        // done() arms the next dispatch timer inside the final turn of the
        // first advance, so a second zero-length advance is required to fire it.
        await clock.advance(0);
        await clock.advance(0);
        eq(c, local.hits.filter(h => h.path === "/high").length, 1,
            "high dispatches only after flaky's done() releases the slot");

        await settleUntilDrain(clock, drained);
        eq(c, local.hits.filter(h => ledger.tasks.get(harness.pathToId[h.path])?.pool === 1).map(h => h.path),
            ["/flaky", "/flaky", "/high"], "pool-1 wire order: both attempts first, queued high afterwards");
        eq(c, ledger.log.filter(e => e.type === "callback").map(e => e.id),
            ["p2-a", "p2-b", "flaky", "high"], "callback ordering across both pools");
        checkPerTaskInvariants(c, ledger);
        eq(c, ledger.drainCount(), 1, "main retry scenario drains exactly once");

        // Retry exhaustion: terminal failure preserves the original options.
        const doomedFlag = nextDrain(crawler);
        const doomedId = addTracked(crawler, ledger, harness, { id: "doomed", path: "/doomed", pool: 1, priority: 2 });
        const doomedOptions = ledger.tasks.get(doomedId).options;
        await settleUntilDrain(clock, doomedFlag, 200, 25);
        const doomedTask = ledger.tasks.get("doomed");
        eq(c, doomedTask.hits.map(h => h.status), [503, 503], "exhaustion still performs exactly two wire attempts");
        eq(c, doomedTask.callbacks.length, 1, "one terminal error callback once retries are exhausted");
        truthy(c, Boolean(doomedTask.error), "terminal callback receives the transport error");
        truthy(c, Boolean(doomedTask.lastRes && doomedTask.lastRes.options === doomedOptions),
            "terminal failure hands back the preserved options object");
        eq(c, doomedOptions.retries, 0, "exhausted options record retries = 0");
        eq(c, doomedOptions.userParams.id, "doomed", "userParams preserved through every attempt");
        eq(c, doomedOptions.url, BASE_URL + "/doomed", "original url preserved on terminal failure");
        eq(c, doomedTask.doneCalls(), 1, "done() still called exactly once on terminal failure");

        // Got's own retry is disabled (alignOptions sets retry.limit = 0):
        // retries=0 means exactly one wire hit, surfaced as an outer error.
        const gotOffFlag = nextDrain(crawler);
        addTracked(crawler, ledger, harness, { id: "got-off", path: "/got-off", pool: 2, priority: 1, retries: 0 });
        await settleUntilDrain(clock, gotOffFlag, 100, 25);
        eq(c, ledger.tasks.get("got-off").hits.length, 1, "retries=0 produces exactly one wire hit (got retry disabled)");
        truthy(c, Boolean(ledger.tasks.get("got-off").error), "the 503 surfaces as an outer Crawler error");
        eq(c, ledger.tasks.get("got-off").doneCalls(), 1, "done once on the got-off failure");
    } finally {
        clock.restore();
        await local.close();
    }
});

/* ------------------------------------------------------------------ */
/* Group 3: Rate calendar and mid-flight setLimiter semantics.         */
/* ------------------------------------------------------------------ */

group("Rate calendar", async c => {
    console.log("GROUP_START Rate calendar");
    const clock = installVirtualClock(1000);
    const harness = { agent: null, pathToId: {} };
    const ledger = createLedger(clock);
    clock.onTimer = entry => ledger.timerArmed(entry);
    const aGate = gate();
    const specialRoutes = {
        "/cal-a": async () => { await aGate.promise; return { status: 200, body: "ascii:cal-a" }; },
    };
    const local = createLocalServer(async (req, rec) => {
        const id = harness.pathToId[req.url] ?? null;
        ledger.hit(rec, id);
        if (specialRoutes[req.url]) return specialRoutes[req.url](rec);
        return { status: 200, body: `ascii:${req.url}` };
    });
    harness.agent = local.agent;
    // rateLimit > 0 forces per-pool concurrency to one at construction.
    const crawler = createCrawler({ rateLimit: 100, retries: 0, priorityLevels: 10 });
    crawler.on("schedule", options => ledger.schedule(options));
    crawler.on("request", options => ledger.request(options));
    const drained = bindDrain(crawler, ledger);

    try {
        addTracked(crawler, ledger, harness, { id: "cal-a", path: "/cal-a", pool: 1, priority: 2 });
        await clock.advance(0);
        eq(c, ledger.dispatchTime("cal-a"), 1000, "first reservation sends immediately on the calendar");

        // Pinned moment required by the contract: the predecessor holds the
        // slot while a same-priority and a higher-priority task are waiting.
        addTracked(crawler, ledger, harness, { id: "cal-b", path: "/cal-b", pool: 1, priority: 2 });
        addTracked(crawler, ledger, harness, { id: "cal-c", path: "/cal-c", pool: 1, priority: 0 });
        addTracked(crawler, ledger, harness, { id: "p2-a", path: "/p2-a", pool: 2, priority: 1 });
        addTracked(crawler, ledger, harness, { id: "p2-b", path: "/p2-b", pool: 2, priority: 1 });
        await clock.advance(0);
        eq(c, ledger.waiting(1), ["cal-b", "cal-c"], "both waiting classes queue behind the held pool-1 slot");
        eq(c, local.hits.filter(h => h.path === "/cal-a").length, 1, "pool-1 concurrency is one: nothing else in flight");
        eq(c, ledger.dispatchTime("p2-a"), 1000, "pool-2 first reservation is independent and immediate");

        await clock.advance(50); // t = 1050
        const pendingSnapshot = clock.entries
            .filter(e => e.firedAt === null && !e.cancelled)
            .map(e => ({ kind: e.kind, due: e.due, id: e.handle.id }));
        crawler.setLimiter(1, "rateLimit", 400);
        const pendingAfterSet = clock.entries
            .filter(e => e.firedAt === null && !e.cancelled)
            .map(e => ({ kind: e.kind, due: e.due, id: e.handle.id }));
        eq(c, pendingAfterSet, pendingSnapshot,
            "setLimiter neither reschedules an armed timer nor clears the existing nextRequestTime");
        eq(c, ledger.waiting(1), ["cal-b", "cal-c"], "setLimiter dispatches nothing by itself");

        // Release the held slot BEFORE the old calendar point (1100). The
        // residual 50ms gap computed at reservation time must still be honored.
        aGate.release();
        await clock.advance(0); // t = 1050: cal-a responds, callback and done run; cal-c timer is armed for 1100
        eq(c, ledger.tasks.get("cal-c").pre.length, 0, "cal-c does not fire immediately when the slot frees early");
        eq(c, local.hits.filter(h => ledger.tasks.get(harness.pathToId[h.path])?.pool === 1).map(h => h.path),
            ["/cal-a"], "only cal-a has hit the wire right after the early release");

        await clock.advance(50); // t = 1100: the old nextRequestTime point
        eq(c, ledger.dispatchTime("cal-c"), 1100,
            "post-update reservation honors the pre-existing nextRequestTime rather than sending now");
        eq(c, local.hits.filter(h => ledger.tasks.get(harness.pathToId[h.path])?.pool === 1).map(h => h.path),
            ["/cal-a", "/cal-c"], "higher-priority cal-c is reserved before same-priority cal-b");
        eq(c, ledger.dispatchTime("p2-b"), 1100, "pool-2 keeps its own 100ms calendar while pool-1 is held");

        await clock.advance(390); // t = 1490: cal-b must not fire yet
        eq(c, local.hits.filter(h => h.path === "/cal-b").length, 0, "cal-b waits for the new 400ms calendar spacing");
        await clock.advance(20); // t = 1510
        eq(c, ledger.dispatchTime("cal-b"), 1500, "cal-b reservation uses the updated rateLimit (1100 + 400)");

        // Concurrency-one invariant: with the pool serialized, each
        // reservation can only be made at or after the previous done().
        const poolOneTasks = [...ledger.tasks.values()]
            .filter(t => t.pool === 1)
            .sort((a, b) => a.pre[0] - b.pre[0]);
        for (let i = 1; i < poolOneTasks.length; i += 1) {
            truthy(c, poolOneTasks[i].pre[0] >= poolOneTasks[i - 1].dones[0],
                `${poolOneTasks[i].id} reserved no earlier than ${poolOneTasks[i - 1].id}'s done()`);
        }

        await settleUntilDrain(clock, drained);
        eq(c, [ledger.dispatchTime("cal-a"), ledger.dispatchTime("cal-c"), ledger.dispatchTime("cal-b")],
            [1000, 1100, 1500], "full pool-1 send calendar");
        eq(c, [ledger.dispatchTime("p2-a"), ledger.dispatchTime("p2-b")], [1000, 1100],
            "pool-2 calendar is untouched by the pool-1 setLimiter call");
        eq(c, ledger.log.filter(e => e.type === "reserve").length, 5,
            "one reservation per queued calendar task, none added by setLimiter itself");
        eq(c, ledger.log.filter(e => e.type === "retryWait").length, 0,
            "a retries=0 calendar scenario never books a retry wait");
        checkPerTaskInvariants(c, ledger);
        eq(c, ledger.drainCount(), 1, "calendar scenario drains exactly once");
        eq(c, ledger.log.at(-1).type, "drain", "drain remains the final ledger event");
    } finally {
        clock.restore();
        await local.close();
    }
});

export {
    GROUP_NAMES,
    Crawler,
    BASE_URL,
    createCrawler,
    createLocalServer,
    createLedger,
    installVirtualClock,
    gate,
};
