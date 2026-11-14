"use strict";

const http = require("node:http");
const fs = require("node:fs");
const path = require("node:path");
const { once } = require("node:events");

const Crawler = require("../");
const { group } = require("../task_support/check.cjs");

const workDir = path.resolve(__dirname, "..", ".task-build", "checkpoint-tests");
fs.mkdirSync(workDir, { recursive: true });

function delay(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

async function until(fn, message, timeout = 10000) {
    const start = Date.now();
    while (!fn()) {
        if (Date.now() - start > timeout) {
            throw new Error("Timed out waiting for: " + message);
        }
        await delay(5);
    }
}

function makeCrawler(url) {
    const crawler = new Crawler(url);
    crawler.respectRobotsTxt = false;
    crawler.interval = 5;
    crawler.httpAgent = new http.Agent();
    crawler.httpsAgent = new http.Agent();
    return crawler;
}

function listen(server) {
    server.listen(0, "127.0.0.1");
    return once(server, "listening").then(() => server.address().port);
}

async function shutdown(crawlers, server) {
    for (const crawler of crawlers) {
        try { crawler.stop(true); } catch (e) { /* already stopped */ }
        try { crawler.httpAgent.destroy(); } catch (e) { /* ignore */ }
        try { crawler.httpsAgent.destroy(); } catch (e) { /* ignore */ }
    }
    if (server) {
        try { server.closeAllConnections(); } catch (e) { /* ignore */ }
        await new Promise(resolve => server.close(resolve));
    }
}

group("Quiescent capture", async (c) => {
    const requests = [];
    const server = http.createServer((req, res) => {
        requests.push(req.url);
        res.setHeader("Content-Type", "text/html");
        if (req.url === "/") {
            res.end('<a href="/a">a</a><a href="/slow">s</a>');
        } else if (req.url === "/slow") {
            setTimeout(() => res.end('<a href="/late">l</a><a href="/async">b</a>'), 150);
        } else if (req.url === "/empty") {
            res.end("no links here");
        } else {
            res.end("collected:" + req.url);
        }
    });
    const port = await listen(server);
    const base = `http://127.0.0.1:${port}`;
    const crawlers = [];

    try {
        // Main scenario: checkpoint while a page is in flight. Links found in
        // that page's body, plus asynchronously admitted URLs, must all be
        // settled and captured as queued items, and a wait() hold must block
        // the copy until it is released.
        const c1 = makeCrawler(base + "/");
        crawlers.push(c1);
        c1.maxConcurrency = 2;
        c1.addFetchCondition((queueItem, referrer, cb) => setTimeout(() => cb(null, true), 25));

        const complete1 = once(c1, "complete");
        const release = c1.wait();
        c1.start();
        await until(() => requests.includes("/slow"), "server received /slow request");

        const f1 = path.join(workDir, "quiescent-1.json");
        const f2 = path.join(workDir, "quiescent-2.json");
        let firstDone = false;
        let requestsAtFirstCallback = null;
        const secondDone = new Promise((resolve, reject) => {
            c1.checkpoint(f1, (err) => {
                if (err) {
                    return reject(err);
                }
                firstDone = true;
                requestsAtFirstCallback = requests.slice();
                // A follow-up operation can be started from inside the callback
                c1.checkpoint(f2, (err2) => err2 ? reject(err2) : resolve());
            });
        });

        // Concurrent operations are rejected without disturbing the first one
        const busyCheckpoint = await new Promise(ok =>
            c1.checkpoint(path.join(workDir, "busy.json"), ok));
        c.check(busyCheckpoint && busyCheckpoint.code === "ERR_CHECKPOINT_BUSY",
            "concurrent checkpoint rejected with ERR_CHECKPOINT_BUSY");
        const busyRestore = await new Promise(ok => c1.restoreCheckpoint(f1, ok));
        c.check(busyRestore && busyRestore.code === "ERR_CHECKPOINT_BUSY",
            "restore during checkpoint rejected with ERR_CHECKPOINT_BUSY");

        // Links discovered in the in-flight /slow response get admitted to
        // the queue while the barrier is up
        await until(() =>
            c1.queue.some(item => item.path === "/late") &&
            c1.queue.some(item => item.path === "/async"),
            "links from in-flight page admitted");
        await until(() => c1._openRequests.length === 0, "in-flight requests drained");
        await delay(60);
        c.check(firstDone === false, "checkpoint waits for wait() holds before copying");
        release();
        await secondDone;
        c.check(firstDone === true, "first checkpoint completed after wait() released");

        const snap = JSON.parse(fs.readFileSync(f1, "utf8"));
        c.check(snap.version === 1, "checkpoint file is version 1");
        c.check(snap.initialURL === base + "/", "initialURL stored in checkpoint");
        c.check(snap.host === "127.0.0.1", "host stored in checkpoint");
        c.check(snap.protocol === "http", "protocol stored in checkpoint");
        c.check(snap.port === port, "port stored in checkpoint");
        c.check(snap._isFirstRequest === false, "_isFirstRequest stored in checkpoint");

        const byPath = {};
        snap.queue.forEach(item => { byPath[item.path] = item; });
        c.check(byPath["/late"] && byPath["/late"].status === "queued",
            "link from in-flight page body captured as queued");
        c.check(byPath["/async"] && byPath["/async"].status === "queued",
            "asynchronously admitted link captured as queued");
        c.check(byPath["/slow"] && byPath["/slow"].fetched === true,
            "in-flight page settled before the state was copied");
        c.check(byPath["/slow"].stateData && byPath["/slow"].stateData.code === 200,
            "stateData of settled page preserved");
        c.check(!requestsAtFirstCallback.includes("/late") &&
            !requestsAtFirstCallback.includes("/async"),
            "no new requests dispatched while the checkpoint was captured");
        c.check(JSON.parse(fs.readFileSync(f2, "utf8")).version === 1,
            "second checkpoint started from the first callback succeeded");

        await complete1;
        c.check(requests.includes("/late") && requests.includes("/async"),
            "crawler resumed dispatching after the checkpoint");
        c.check(c1.queue.every(item => item.fetched),
            "all queued items fetched after resuming");

        // stop() interleaved with a checkpoint: the save still completes, but
        // the crawler must not automatically restart.
        const c6 = makeCrawler(base + "/slow");
        crawlers.push(c6);
        const slowSeen = requests.filter(url => url === "/slow").length;
        c6.start();
        await until(() => requests.filter(url => url === "/slow").length > slowSeen,
            "c6 requested /slow");
        const f6 = path.join(workDir, "stop-interleave.json");
        const c6Done = new Promise((resolve, reject) =>
            c6.checkpoint(f6, err => err ? reject(err) : resolve()));
        c6.stop();
        await c6Done;
        c.check(c6.running === false, "crawler stopped during checkpoint stays stopped");
        c.check(JSON.parse(fs.readFileSync(f6, "utf8")).queue.length >= 1,
            "checkpoint file written even though stop() interleaved");
        const requestsAfterStop = requests.length;
        await delay(120);
        c.check(requests.length === requestsAfterStop,
            "no requests dispatched after stop() during checkpoint");

        // An empty queue can be saved, and a crawler that was never started
        // stays stopped.
        const c4 = makeCrawler(base + "/empty");
        crawlers.push(c4);
        const f4 = path.join(workDir, "empty.json");
        const emptyErr = await new Promise(ok => c4.checkpoint(f4, ok));
        c.check(!emptyErr, "checkpoint of an empty queue succeeds");
        const emptySnap = JSON.parse(fs.readFileSync(f4, "utf8"));
        c.check(Array.isArray(emptySnap.queue) && emptySnap.queue.length === 0,
            "empty queue saved to checkpoint file");
        c.check(!c4.running, "never-started crawler stays stopped after checkpoint");

        // start() called while a checkpoint is being saved must not breach
        // the barrier; admissions during the barrier are still captured.
        const hold4 = c4.wait();
        const f4b = path.join(workDir, "empty-start.json");
        const complete4 = once(c4, "complete");
        const emptyRequestsBefore = requests.filter(url => url === "/empty").length;
        const c4bDone = new Promise((resolve, reject) =>
            c4.checkpoint(f4b, err => err ? reject(err) : resolve()));
        c4.start();
        await delay(80);
        c.check(requests.filter(url => url === "/empty").length === emptyRequestsBefore,
            "start() during checkpoint does not dispatch past the barrier");
        hold4();
        await c4bDone;
        const snap4b = JSON.parse(fs.readFileSync(f4b, "utf8"));
        c.check(snap4b.queue.length === 1 && snap4b.queue[0].status === "queued",
            "URL admitted during the barrier captured as queued");
        await complete4;
        c.check(requests.includes("/empty"),
            "user's own start takes effect once the checkpoint has finished");

        // While the barrier is up, the crawl loop must not emit "complete".
        const c5 = makeCrawler(base + "/empty");
        crawlers.push(c5);
        let completed5 = false;
        c5.on("complete", () => { completed5 = true; });
        const complete5 = once(c5, "complete");
        const hold5 = c5.wait();
        c5.start();
        await until(() => c5.queue[0] && c5.queue[0].fetched === true,
            "c5 fetched its only page");
        const f5 = path.join(workDir, "barrier-complete.json");
        const c5Done = new Promise((resolve, reject) =>
            c5.checkpoint(f5, err => err ? reject(err) : resolve()));
        c5.crawl(); // would emit "complete" if the barrier didn't block it
        c.check(completed5 === false, "complete is not emitted while checkpoint is saved");
        c.check(c5.running === true, "crawler still running during checkpoint");
        hold5();
        await c5Done;
        await complete5;
        c.check(completed5 === true, "complete fires once the checkpoint has finished");

        // Non-FetchQueue backends are rejected asynchronously, leaving the
        // running state untouched.
        const c7 = makeCrawler(base + "/");
        crawlers.push(c7);
        c7.queue = { not: "a FetchQueue" };
        const queueErr = await new Promise(ok =>
            c7.checkpoint(path.join(workDir, "no-queue.json"), ok));
        c.check(queueErr && queueErr.code === "ERR_CHECKPOINT_QUEUE",
            "checkpoint with foreign queue backend rejected with ERR_CHECKPOINT_QUEUE");
        const queueRestoreErr = await new Promise(ok => c7.restoreCheckpoint(f1, ok));
        c.check(queueRestoreErr && queueRestoreErr.code === "ERR_CHECKPOINT_QUEUE",
            "restore with foreign queue backend rejected with ERR_CHECKPOINT_QUEUE");
        c.check(!c7.running, "running state unchanged after queue backend error");
    } finally {
        await shutdown(crawlers, server);
    }
});

group("Atomic restore", async (c) => {
    const requests = [];
    const server = http.createServer((req, res) => {
        requests.push(req.url);
        res.setHeader("Content-Type", "text/html");
        if (req.url === "/home") {
            res.end('<a href="/p1">1</a><a href="/p2">2</a>');
        } else {
            res.end("collected:" + req.url);
        }
    });
    const port = await listen(server);
    const base = `http://127.0.0.1:${port}`;
    const crawlers = [];

    try {
        // Build a snapshot containing forced duplicate URL entries
        const E = makeCrawler(base + "/home");
        crawlers.push(E);
        const doneE = once(E, "complete");
        E.start();
        await doneE;
        E.queueURL(base + "/p1", undefined, true);
        E.queueURL(base + "/p1", undefined, true);
        await until(() => E.queue.length === 5, "forced duplicates added");

        const F = path.join(workDir, "atomic.json");
        const snapErr = await new Promise(ok => E.checkpoint(F, ok));
        c.check(!snapErr, "snapshot for restore tests written");
        c.check(JSON.parse(fs.readFileSync(F, "utf8")).queue.length === 5,
            "forced duplicate entries kept in the snapshot");

        // Clean restore into a fresh, stopped instance
        const B = makeCrawler(base + "/home");
        crawlers.push(B);
        const oldQueue = B.queue, oldCookies = B.cookies, oldCache = B.cache;
        let queueaddFired = false;
        B.on("queueadd", () => { queueaddFired = true; });
        const requestsBeforeRestore = requests.length;
        const restoreErr = await new Promise(ok => B.restoreCheckpoint(F, ok));
        c.check(!restoreErr, "restore succeeds on a stopped, idle crawler");
        c.check(B.queue !== oldQueue && B.queue.length === 5,
            "queue atomically replaced, not appended");
        c.check(B.host === E.host && B.initialURL === E.initialURL,
            "host and initialURL restored");
        c.check(B._isFirstRequest === false, "_isFirstRequest restored");
        c.check(B.cookies === oldCookies && B.cache === oldCache,
            "cookie jar and cache instances untouched");
        c.check(requests.length === requestsBeforeRestore,
            "restore performs no HTTP requests");
        c.check(queueaddFired === false, "restore does not emit queueadd");
        c.check(!B.running, "restore does not start the crawl loop");

        // Deduplication index and unfetched cursor are rebuilt
        const p1Count = await new Promise(ok => B.queue.exists(base + "/p1", (e, v) => ok(v)));
        c.check(p1Count === 1, "dedupe index rebuilt by URL");
        let duplicateFired = false;
        B.on("queueduplicate", () => { duplicateFired = true; });
        B.queueURL(base + "/p2");
        await until(() => duplicateFired, "queueduplicate fired for restored URL");
        c.check(B.queue.length === 5, "duplicate URL not added after restore");
        const oldest = await new Promise(ok => B.queue.oldestUnfetchedItem((e, item) => ok(item)));
        c.check(oldest && oldest.path === "/p1", "unfetched-item cursor rebuilt");

        // Restore on a running crawler is rejected and changes nothing
        const C = makeCrawler(base + "/home");
        crawlers.push(C);
        C.start();
        const busyErr = await new Promise(ok => C.restoreCheckpoint(F, ok));
        c.check(busyErr && busyErr.code === "ERR_CHECKPOINT_BUSY",
            "restore on a running crawler rejected with ERR_CHECKPOINT_BUSY");
        c.check(C.queue.length !== 5, "busy restore leaves the queue untouched");
        C.stop(true);

        // Invalid files never change the instance
        const D = makeCrawler(base + "/home");
        crawlers.push(D);
        const dQueue = D.queue, dHost = D.host, dInitialURL = D.initialURL;
        const validFields = {
            version: 1,
            initialURL: base + "/home",
            host: "127.0.0.1",
            protocol: "http",
            port: port,
            _isFirstRequest: false
        };
        async function badRestore(contents, message) {
            const file = path.join(workDir, "bad-" + Math.random().toString(16).slice(2) + ".json");
            fs.writeFileSync(file, contents);
            const err = await new Promise(ok => D.restoreCheckpoint(file, ok));
            c.check(!!err, message);
            c.check(D.queue === dQueue && (D.queue.length || 0) === 0 &&
                D.host === dHost && D.initialURL === dInitialURL,
                "instance unchanged: " + message);
        }
        await badRestore("this is not json{", "garbage JSON rejected");
        await badRestore(JSON.stringify(Object.assign({}, validFields, { version: 2 })),
            "unsupported version rejected");
        await badRestore(JSON.stringify(Object.assign({}, validFields, {
            queue: [{ id: 3, url: base + "/p1", status: "queued", fetched: false }]
        })), "non-contiguous queue ids rejected");
        await badRestore(JSON.stringify(Object.assign({}, validFields, {
            queue: [
                { id: 0, url: base + "/p1", status: "queued", fetched: false },
                { id: 0, url: base + "/p2", status: "queued", fetched: false }
            ]
        })), "duplicate queue ids rejected");
        await badRestore(JSON.stringify(Object.assign({}, validFields, {
            queue: [{ id: 0, url: base + "/p1", status: "bogus", fetched: false }]
        })), "illegal queue item status rejected");
        await badRestore(JSON.stringify(Object.assign({}, validFields, {
            queue: [{ id: 0, url: base + "/p1", status: "queued", fetched: "yes" }]
        })), "wrong field type rejected");
        const missingInitialURL = Object.assign({}, validFields, { queue: [] });
        delete missingInitialURL.initialURL;
        await badRestore(JSON.stringify(missingInitialURL), "missing initialURL rejected");
        const missingErr = await new Promise(ok =>
            D.restoreCheckpoint(path.join(workDir, "does-not-exist.json"), ok));
        c.check(!!missingErr, "unreadable checkpoint file rejected");
        c.check(D.queue === dQueue && (D.queue.length || 0) === 0,
            "instance unchanged after read failure");

        // A failed checkpoint keeps the original file and cleans up its
        // temporary file
        const readOnlyDir = path.join(workDir, "readonly-dir");
        fs.mkdirSync(readOnlyDir, { recursive: true });
        const keepFile = path.join(readOnlyDir, "keep.json");
        const keepErr1 = await new Promise(ok => E.checkpoint(keepFile, ok));
        c.check(!keepErr1, "initial checkpoint written");
        const originalContents = fs.readFileSync(keepFile, "utf8");
        fs.chmodSync(readOnlyDir, 0o555);
        let keepErr2;
        try {
            keepErr2 = await new Promise(ok => E.checkpoint(keepFile, ok));
        } finally {
            fs.chmodSync(readOnlyDir, 0o755);
        }
        c.check(!!keepErr2, "checkpoint into unwritable directory fails");
        c.check(fs.readFileSync(keepFile, "utf8") === originalContents,
            "original checkpoint file preserved on failure");
        c.check(fs.readdirSync(readOnlyDir).filter(n => n.includes(".tmp-")).length === 0,
            "temporary file cleaned up on write failure");

        // Rename failure (target is an existing directory) also cleans up
        const dirTarget = path.join(workDir, "a-directory");
        fs.mkdirSync(dirTarget, { recursive: true });
        const dirErr = await new Promise(ok => E.checkpoint(dirTarget, ok));
        c.check(!!dirErr, "checkpoint onto a directory path fails");
        c.check(fs.statSync(dirTarget).isDirectory(), "original directory preserved");
        c.check(fs.readdirSync(workDir).filter(n => n.includes(".tmp-")).length === 0,
            "temporary file cleaned up on rename failure");
    } finally {
        await shutdown(crawlers, server);
    }
});

group("Resume discovery", async (c) => {
    const requests = [];
    const server = http.createServer((req, res) => {
        requests.push(req.url);
        if (req.url === "/robots.txt") {
            res.setHeader("Content-Type", "text/plain");
            res.end("User-agent: *\nAllow: /\n");
            return;
        }
        res.setHeader("Content-Type", "text/html");
        if (req.url === "/index") {
            res.end('<a href="/x">x</a><a href="/y">y</a>');
        } else if (req.url === "/x") {
            res.end('<a href="/z">z</a><a href="/index">home</a>');
        } else {
            res.end("collected:" + req.url);
        }
    });
    const port = await listen(server);
    const base = `http://127.0.0.1:${port}`;
    const crawlers = [];

    try {
        // Phase 1: fetch only the index page, stop, and checkpoint with the
        // discovered links still queued.
        const G = makeCrawler(base + "/index");
        crawlers.push(G);
        G.maxConcurrency = 1;
        G.on("fetchcomplete", (queueItem) => {
            if (queueItem.path === "/index") {
                G.stop();
            }
        });
        G.start();
        await until(() => G.queue.length === 3, "discovered links admitted");
        const F3 = path.join(workDir, "resume.json");
        const snapErr = await new Promise(ok => G.checkpoint(F3, ok));
        c.check(!snapErr, "phase 1 checkpoint written");
        c.check(!G.running, "stopped crawler stays stopped after checkpoint");
        c.check(requests.filter(url => url === "/index").length === 1,
            "phase 1 fetched the index page");
        c.check(!requests.includes("/x") && !requests.includes("/y"),
            "phase 1 fetched nothing beyond the index page");

        // Phase 2: restore into a fresh instance and resume the crawl
        const H = new Crawler(base + "/index");
        crawlers.push(H);
        H.interval = 5;
        H.httpAgent = new http.Agent();
        H.httpsAgent = new http.Agent();
        const requestsBeforeRestore = requests.length;
        const restoreErr = await new Promise(ok => H.restoreCheckpoint(F3, ok));
        c.check(!restoreErr, "restore succeeds");
        c.check(H.queue.length === 3, "restored queue replaces the empty one");
        c.check(requests.length === requestsBeforeRestore,
            "restore itself sends no requests");
        const completeH = once(H, "complete");
        H.start();
        await completeH;

        const phase2 = requests.slice(requestsBeforeRestore);
        c.check(phase2.includes("/robots.txt"),
            "robots.txt re-fetched under the new process's policy");
        c.check(phase2.includes("/x") && phase2.includes("/y") && phase2.includes("/z"),
            "queued and subsequently discovered pages fetched after resume");
        c.check(!phase2.includes("/index"),
            "completed initial page not re-fetched after resume");
        c.check(requests.filter(url => url === "/index").length === 1,
            "initial page fetched exactly once across both runs");
        const indexItems = H.queue.filter(item => item.path === "/index");
        c.check(indexItems.length === 1, "initial page not duplicated on restart");
        c.check(indexItems[0].stateData && indexItems[0].stateData.code === 200,
            "terminal state of completed page preserved across the checkpoint");
        c.check(H.queue.length === 4,
            "rediscovered initial URL deduplicated against the restored index");
        c.check(H.queue.every(item => item.fetched),
            "final queue fully fetched after resume");
    } finally {
        await shutdown(crawlers, server);
    }
});
