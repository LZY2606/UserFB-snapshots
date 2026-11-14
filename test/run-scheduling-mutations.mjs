/*
 * Scheduling mutation runner.
 *
 * Flow:
 *   1. Normal source: ASCII in-process HTTP smoke, then the full scheduling
 *      regression  -> SCHEDULING_BASE_OK
 *   2. Three independent temporary source copies, each rebuilt on its own and
 *      each carrying exactly one deliberate degeneration:
 *        - fifo:     same-priority dequeue becomes LIFO
 *        - retry:    the slot is returned before the retry is re-queued
 *        - calendar: setLimiter("rateLimit", ...) resets nextRequestTime
 *      Each copy passes the smoke first and then fails the SAME new regression
 *      through a content assertion (expected vs actual sequences).
 *
 * A build/import/timeout/no-execution failure is reported as INFRA and never
 * counts as a detection: a mutant only prints its CAUGHT marker when the
 * regression actually ran and an assertion message names the mismatch.
 *
 * No TCP ports are used anywhere (the sandbox forbids binding); the smoke and
 * the regression talk to a real node:http.Server over an in-process transport.
 */
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";

const REPO_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const WORK_ROOT = path.join(REPO_ROOT, ".task-build", "mutants");
const REGRESSION_REL = path.join("task_support", "execute.cjs");
const SMOKE_REL = path.join(".task-build", "sched-smoke.mjs");
const RUN_TIMEOUT_MS = 120000;

const SMOKE_SOURCE = `
import http from "node:http";
import { Duplex } from "node:stream";
import Crawler from "../dist/index.js";
function pair() {
  let peer;
  const a = new Duplex({ read() {}, write(c, _e, cb) { peer.push(c); cb(); }, final(cb) { peer.push(null); cb(); } });
  const b = new Duplex({ read() {}, write(c, _e, cb) { a.push(c); cb(); }, final(cb) { a.push(null); cb(); } });
  peer = b;
  return [a, b];
}
const server = http.createServer((req, res) => {
  res.writeHead(200, { "content-type": "text/plain; charset=ascii" });
  res.end("ascii:smoke");
});
const agent = { http: { addRequest(req) { const [c, s] = pair(); server.emit("connection", s); req.onSocket(c); } } };
const crawler = new Crawler({ jQuery: false, retries: 0, silence: true });
const result = await crawler.send({ url: "http://sched.local/smoke", agent });
if (result.body !== "ascii:smoke") throw new Error("smoke body mismatch: " + JSON.stringify(result.body));
console.log("SCHED_SMOKE_OK");
server.closeAllConnections();
server.close();
`;

const MUTANTS = [
    {
        name: "fifo",
        marker: "SCHEDULING_FIFO_CAUGHT",
        group: "Pool dispatch",
        file: path.join("src", "lib", "queue.ts"),
        find: `        const node = this._dummyHead.next as Node<T>;
        const newFirst = node.next as Node<T> | DummyTailNode;
        this._dummyHead.next = newFirst;
        newFirst.prev = this._dummyHead;
        node.next = null;
        this._length--;
        return node.value;`,
        replace: `        // MUTATION: pop the tail instead of the head (same-priority LIFO)
        const node = this._dummyTail.prev as Node<T>;
        const newLast = node.prev as Node<T> | DummyHeadNode;
        this._dummyTail.prev = newLast;
        newLast.next = this._dummyTail;
        node.prev = null;
        this._length--;
        return node.value;`,
    },
    {
        name: "retry",
        marker: "SCHEDULING_RETRY_CAUGHT",
        group: "Retry occupancy",
        file: path.join("src", "crawler.ts"),
        find: `                log.warn(\`\${error} occurred on \${options.url}. \${options.retries ? \`(\${options.retries} retries left)\` : ""}\`);
                setTimeout(() => {
                    options.retries!--;
                    this._execute(options as CrawlerOptions);
                }, options.retryInterval);
                return;`,
        replace: `                log.warn(\`\${error} occurred on \${options.url}. \${options.retries ? \`(\${options.retries} retries left)\` : ""}\`);
                // MUTATION: return the pool slot first, then re-queue the retry
                options.release?.();
                setTimeout(() => {
                    options.retries!--;
                    this._schedule(options as CrawlerOptions);
                }, options.retryInterval);
                return;`,
    },
    {
        name: "calendar",
        marker: "SCHEDULING_CALENDAR_CAUGHT",
        group: "Rate calendar",
        file: path.join("src", "rateLimiter", "rateLimiter.ts"),
        find: `        this.rateLimit = rateLimit;
        if (this.rateLimit > 0) this.maxConnections = 1;`,
        replace: `        this.rateLimit = rateLimit;
        // MUTATION: wipe the existing send calendar when rateLimit changes
        this.nextRequestTime = Date.now();
        if (this.rateLimit > 0) this.maxConnections = 1;`,
    },
];

function runNode(cwd, args, { schedEntry = true } = {}) {
    const env = { ...process.env };
    if (schedEntry) env.SCHED_CRAWLER_ENTRY = path.join(cwd, "dist", "index.js");
    return spawnSync(process.execPath, args, {
        cwd,
        encoding: "utf8",
        timeout: RUN_TIMEOUT_MS,
        maxBuffer: 16 * 1024 * 1024,
        env,
    });
}

function fail(reason, detail) {
    throw new Error(`SCHEDULING_RUNNER_ABORT ${reason}${detail ? ` :: ${detail}` : ""}`);
}

function ensureSmoke(cwd) {
    fs.mkdirSync(path.join(cwd, ".task-build"), { recursive: true });
    fs.writeFileSync(path.join(cwd, SMOKE_REL), SMOKE_SOURCE);
    const smoke = runNode(cwd, [SMOKE_REL]);
    if (smoke.status !== 0 || !String(smoke.stdout).includes("SCHED_SMOKE_OK")) {
        fail("smoke failed (build/import/HTTP problem, not a content detection)",
            `status=${smoke.status} signal=${smoke.signal}\n${smoke.stdout}\n${smoke.stderr}`.slice(0, 2000));
    }
}

function runRegression(cwd) {
    fs.mkdirSync(path.join(cwd, ".task-build"), { recursive: true });
    const result = runNode(cwd, [REGRESSION_REL]);
    return result;
}

function copySourceTree(dest) {
    fs.rmSync(dest, { recursive: true, force: true });
    fs.mkdirSync(dest, { recursive: true });
    // Copy only the trees a rebuild + the regression need. Copying the repo
    // root wholesale is impossible here: the workspace lives underneath it.
    for (const entry of ["src", "task_support", "test", "package.json", "tsconfig.json", "eslint.config.js"]) {
        const source = path.join(REPO_ROOT, entry);
        if (fs.existsSync(source)) fs.cpSync(source, path.join(dest, entry), { recursive: true });
    }
    fs.symlinkSync(path.join(REPO_ROOT, "node_modules"), path.join(dest, "node_modules"), "dir");
}

function applyMutation(cwd, mutant) {
    const file = path.join(cwd, mutant.file);
    const source = fs.readFileSync(file, "utf8");
    if (!source.includes(mutant.find)) {
        fail(`mutation anchor missing in ${mutant.file} for ${mutant.name} (source drifted?)`);
    }
    fs.writeFileSync(file, source.replace(mutant.find, mutant.replace));
}

function rebuild(cwd, label) {
    const build = runNode(cwd, [path.join("task_support", "build.cjs")], { schedEntry: false });
    if (build.status !== 0 || !String(build.stdout).includes("BUILD_OK")) {
        fail(`${label} rebuild failed`, `status=${build.status}\n${build.stdout}\n${build.stderr}`.slice(0, 2000));
    }
}

function contentFailure(result, group) {
    const output = `${result.stdout ?? ""}\n${result.stderr ?? ""}`;
    if (result.signal === "SIGTERM") return { ok: false, why: "regression timed out" };
    if (result.status === 0) return { ok: false, why: "mutant regression unexpectedly passed" };
    if (!output.includes(`GROUP_START ${group}`)) {
        return { ok: false, why: `target group "${group}" never executed` };
    }
    // check.cjs AssertionError messages carry both sides of the mismatch.
    const mismatch = output.match(/[^\n]*-- expected [^\n]* actual [^\n]*/);
    if (!mismatch) return { ok: false, why: "failure had no expected/actual content assertion" };
    return { ok: true, message: mismatch[0].trim() };
}

function main() {
    fs.mkdirSync(WORK_ROOT, { recursive: true });
    try {
        // 1. Normal source: smoke then the unchanged regression must pass.
        ensureSmoke(REPO_ROOT);
        const base = runRegression(REPO_ROOT);
        const baseOutput = `${base.stdout ?? ""}\n${base.stderr ?? ""}`;
        if (base.status !== 0) fail("normal-source regression failed", baseOutput.slice(0, 2000));
        for (const group of ["Pool dispatch", "Retry occupancy", "Rate calendar"]) {
            if (!baseOutput.includes(`GROUP_OK ${group}`)) {
                fail(`normal-source run did not execute/pass ${group}`);
            }
        }
        console.log("SCHEDULING_BASE_OK");

        // 2. Independent mutated copies, each rebuilt and checked on its own.
        for (const mutant of MUTANTS) {
            const cwd = path.join(WORK_ROOT, mutant.name);
            copySourceTree(cwd);
            applyMutation(cwd, mutant);
            rebuild(cwd, mutant.name);
            ensureSmoke(cwd);

            const result = runRegression(cwd);
            const verdict = contentFailure(result, mutant.group);
            if (!verdict.ok) {
                fail(`${mutant.name} not detected by content assertions`,
                    `${verdict.why}\n${`${result.stdout ?? ""}\n${result.stderr ?? ""}`.slice(0, 2000)}`);
            }
            // The marker must appear on its own line (run.cjs matches exact lines).
            console.log(mutant.marker);
            console.log(`  detection [${mutant.group}] :: ${verdict.message}`);
        }
    } finally {
        fs.rmSync(WORK_ROOT, { recursive: true, force: true });
    }
}

main();
