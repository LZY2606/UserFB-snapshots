// Drives the wire-text regression (test/wire-text.test.mjs) against:
//   1. the real build in this working tree                -> WIRE_TEXT_BASE_OK
//   2. three single-purpose degradations, each applied to a private copy of
//      the sources, rebuilt, smoke-tested over ASCII HTTP, then checked with
//      the same regression file:
//        - HTTP charset no longer participates in decoding  -> WIRE_CHARSET_CAUGHT
//        - got reads the response as text, not bytes        -> WIRE_BYTES_CAUGHT
//        - encoding:null is decoded as text too             -> WIRE_BINARY_CAUGHT
// A degradation only counts as "caught" when the regression fails through
// real assertion mismatches in named test cases -- build, import, timeout
// or zero-test failures are reported as infrastructure problems instead.

import { spawnSync } from 'node:child_process';
import { cpSync, mkdirSync, mkdtempSync, readFileSync, rmSync, symlinkSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const TEST_FILE = path.join('test', 'wire-text.test.mjs');
const TEST_TIMEOUT_MS = 120000;
const BUILD_TIMEOUT_MS = 180000;

function fail(message) {
    console.error(`run-wire-text: ${message}`);
    process.exit(1);
}

function runRegression(cwd) {
    return spawnSync(process.execPath, ['--test', '--test-reporter=tap', TEST_FILE], {
        cwd,
        encoding: 'utf8',
        timeout: TEST_TIMEOUT_MS,
    });
}

// ---------------------------------------------------------------------------
// 1. Baseline: the unmodified working tree must pass.
// ---------------------------------------------------------------------------

const base = runRegression(repo);
process.stdout.write(base.stdout);
process.stderr.write(base.stderr);
if (base.error) fail(`baseline run did not complete: ${base.error.message}`);
if (base.status !== 0) fail('baseline regression failed on the unmodified tree');
console.log('WIRE_TEXT_BASE_OK');

// ---------------------------------------------------------------------------
// 2. Degradations.
// ---------------------------------------------------------------------------

const degradations = [
    {
        name: 'http-charset-ignored',
        marker: 'WIRE_CHARSET_CAUGHT',
        file: 'src/options.ts',
        find: 'const contentType = headers["content-type"] as string;',
        replace: 'const contentType = undefined as unknown as string; // DEGRADED: HTTP charset no longer participates in selection',
    },
    {
        name: 'got-reads-text',
        marker: 'WIRE_BYTES_CAUGHT',
        file: 'src/options.ts',
        find: 'gotOptions.responseType = "buffer";',
        replace: 'gotOptions.responseType = "text"; // DEGRADED: got reads the response as text instead of preserving bytes',
    },
    {
        name: 'binary-still-decoded',
        marker: 'WIRE_BINARY_CAUGHT',
        file: 'src/crawler.ts',
        find: 'if (options.encoding !== null) {',
        replace: 'if (true) { // DEGRADED: encoding:null is forced through the text decoding branch',
    },
];

const SMOKE_SOURCE = `import http from 'node:http';
import Crawler from './dist/index.js';
const server = http.createServer((req, res) => {
    res.writeHead(200, { 'content-type': 'text/html;charset=utf-8' });
    res.end('<!doctype html><p>Hello ASCII smoke</p>');
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
try {
    const crawler = new Crawler({ retries: 0, silence: true });
    const res = await crawler.send({ url: 'http://127.0.0.1:' + server.address().port + '/smoke' });
    if (typeof res.body !== 'string' || !res.body.includes('Hello ASCII smoke')) {
        console.error('ASCII smoke mismatch: ' + JSON.stringify(String(res.body).slice(0, 80)));
        process.exit(1);
    }
    console.log('ascii smoke ok');
} finally {
    server.close();
}
`;

function reportFailures(stdout) {
    const lines = stdout.split('\n');
    const cases = [];
    for (let i = 0; i < lines.length; i++) {
        if (!/^\s*not ok \d+ - /.test(lines[i])) continue;
        const block = [lines[i]];
        // Collect the YAML diagnostic block that follows the "not ok" line,
        // keeping the assertion diff but dropping noise and stack traces.
        for (let j = i + 1; j < lines.length; j++) {
            const l = lines[j];
            if (/^\s*(not )?ok \d+ - /.test(l) || /^\s*\.\.\.$/.test(l) || /^\s*stack:/.test(l)) break;
            if (/^\s*(duration_ms|type|location|failureType):/.test(l) || l.trim() === '---') continue;
            block.push(l);
        }
        cases.push(block.join('\n'));
    }
    return cases;
}

for (const d of degradations) {
    const work = mkdtempSync(path.join(tmpdir(), `wire-text-${d.name}-`));
    try {
        cpSync(path.join(repo, 'src'), path.join(work, 'src'), { recursive: true });
        cpSync(path.join(repo, 'package.json'), path.join(work, 'package.json'));
        cpSync(path.join(repo, 'tsconfig.json'), path.join(work, 'tsconfig.json'));
        mkdirSync(path.join(work, 'test'));
        cpSync(path.join(repo, TEST_FILE), path.join(work, TEST_FILE));
        symlinkSync(path.join(repo, 'node_modules'), path.join(work, 'node_modules'), 'dir');

        const target = path.join(work, d.file);
        const source = readFileSync(target, 'utf8');
        if (source.split(d.find).length !== 2) {
            fail(`[${d.name}] degradation anchor not found exactly once in ${d.file}: ${JSON.stringify(d.find)}`);
        }
        writeFileSync(target, source.replace(d.find, d.replace));

        // Same build the package "build" script defines (tsup), invoked
        // through node directly so no npm shim needs to be spawnable.
        const build = spawnSync(
            process.execPath,
            [path.join('node_modules', 'tsup', 'dist', 'cli-default.js'), 'src/index.ts', '--format', 'cjs,esm', '--clean'],
            { cwd: work, encoding: 'utf8', timeout: BUILD_TIMEOUT_MS },
        );
        if (build.error || build.status !== 0) {
            process.stdout.write(build.stdout ?? '');
            process.stderr.write(build.stderr ?? '');
            fail(`[${d.name}] degraded copy failed to build -- this is an infrastructure problem, not a caught regression`);
        }

        writeFileSync(path.join(work, 'smoke.mjs'), SMOKE_SOURCE);
        const smoke = spawnSync(process.execPath, ['smoke.mjs'], { cwd: work, encoding: 'utf8', timeout: TEST_TIMEOUT_MS });
        process.stdout.write(smoke.stdout ?? '');
        if (smoke.error || smoke.status !== 0) {
            process.stderr.write(smoke.stderr ?? '');
            fail(`[${d.name}] ASCII HTTP smoke failed on the degraded copy -- cannot attribute later failures to the regression`);
        }

        const run = runRegression(work);
        const out = (run.stdout ?? '') + (run.stderr ?? '');
        if (run.error) fail(`[${d.name}] regression run did not complete (${run.error.message}) -- timeouts do not count as caught`);
        if (run.status === 0) fail(`[${d.name}] regression unexpectedly PASSED on the degraded copy`);
        if (/Cannot find module|ERR_MODULE_NOT_FOUND/.test(out)) {
            process.stdout.write(out);
            fail(`[${d.name}] degraded copy failed to import -- not a caught regression`);
        }
        const tests = out.match(/^# tests (\d+)$/m);
        const fails = out.match(/^# fail (\d+)$/m);
        if (!tests || Number(tests[1]) === 0) {
            process.stdout.write(out);
            fail(`[${d.name}] zero tests ran on the degraded copy -- not a caught regression`);
        }
        if (!fails || Number(fails[1]) === 0 || !/ERR_ASSERTION|AssertionError/.test(out)) {
            process.stdout.write(out);
            fail(`[${d.name}] regression failed without assertion mismatches -- not a caught regression`);
        }

        const cases = reportFailures(run.stdout ?? '');
        console.log(`[${d.name}] regression caught the degradation; failing cases:`);
        for (const c of cases) console.log(c);
        console.log(d.marker);
    } finally {
        rmSync(work, { recursive: true, force: true });
    }
}
