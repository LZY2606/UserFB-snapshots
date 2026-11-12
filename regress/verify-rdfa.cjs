#!/usr/bin/env node
/**
 * Verification harness for the three local RDFa regression mutations.
 *
 * For every patch this script, working on a throwaway copy of the source:
 *   1. applies the patch,
 *   2. compiles the mutated parser (a compile failure abuts the run),
 *   3. runs test/collection-contract.mjs against that build,
 *   4. requires the designated top-level test to fail on a graph assertion,
 *      reporting the first failing edge or list position,
 *   5. prints a *_DETECTED marker.
 *
 * Any missing file, empty test run, skipped test, compile failure or abnormal
 * process exit makes the script fail; none of these count as a detection.
 * The working tree is never modified: every mutation lives in a temp copy.
 */

'use strict';

// This is a standalone, short-lived CLI harness whose whole control flow is a
// sequential prepare -> patch -> compile -> test pipeline. Synchronous file system
// calls keep that ordering explicit and make the failure-exit semantics immediate,
// so the async/no-sync rule is disabled for this script on purpose.
/* eslint-disable no-sync */

const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const repoRoot = path.resolve(__dirname, '..');
const tscBin = path.join(repoRoot, 'node_modules', '.bin', 'tsc');
const nodeBin = process.execPath;
const contractTest = path.join(repoRoot, 'test', 'collection-contract.mjs');
const parserSource = path.join(repoRoot, 'lib', 'RdfaParser.ts');

const CASES = [
  {
    marker: 'RDFA_DIRECTION_DETECTED',
    topLevel: 'RDFA_DIRECTION',
    patch: path.join(__dirname, 'patches', '01-rev-as-forward.patch'),
    mutationTag: 'MUTATION: treat a pending rev relation as a forward relation',
  },
  {
    marker: 'RDFA_SCOPE_DETECTED',
    topLevel: 'RDFA_LIST_SCOPE',
    patch: path.join(__dirname, 'patches', '02-merge-explicit-lists.patch'),
    mutationTag: 'MUTATION: independent explicit-about lists are merged into the parent list',
  },
  {
    marker: 'RDFA_MULTIPLICITY_DETECTED',
    topLevel: 'RDFA_LIST_MULTIPLICITY',
    patch: path.join(__dirname, 'patches', '03-dedup-list-values.patch'),
    mutationTag: 'MUTATION: drop repeated values instead of keeping every list position',
  },
];

function fail(message) {
  process.stderr.write(`VERIFY_RDFA_FAILURE: ${message}\n`);
  process.exit(1);
}

function requireFile(file, label) {
  if (!fs.existsSync(file) || !fs.statSync(file).isFile() || fs.statSync(file).size === 0) {
    fail(`missing or empty ${label}: ${file}`);
  }
}

function run(command, args, options) {
  const result = spawnSync(command, args, { encoding: 'utf8', ...options });
  if (result.error) {
    fail(`failed to launch ${command}: ${result.error.message}`);
  }
  return result;
}

function makeTempCopy() {
  const root = fs.mkdtempSync(path.join(process.env.TMPDIR || os.tmpdir(), 'verify-rdfa-'));
  fs.mkdirSync(path.join(root, 'lib'));
  fs.copyFileSync(path.join(repoRoot, 'index.ts'), path.join(root, 'index.ts'));
  fs.copyFileSync(path.join(repoRoot, 'tsconfig.json'), path.join(root, 'tsconfig.json'));
  for (const entry of fs.readdirSync(path.join(repoRoot, 'lib'))) {
    fs.copyFileSync(
      path.join(repoRoot, 'lib', entry),
      path.join(root, 'lib', entry),
    );
  }
  // Reuse the installed toolchain without copying or mutating node_modules.
  fs.symlinkSync(path.join(repoRoot, 'node_modules'), path.join(root, 'node_modules'));
  return root;
}

function compileBuild(root) {
  const buildDir = path.join(root, 'build');
  const result = run(tscBin, [ '--ignoreDeprecations', '6.0', '--outDir', buildDir ], { cwd: root });
  if (result.status !== 0) {
    process.stderr.write(result.stdout);
    process.stderr.write(result.stderr);
    fail(`TypeScript compilation failed in ${root}`);
  }
  requireFile(path.join(buildDir, 'index.js'), 'compiled entry point');
  return buildDir;
}

function runContract(buildDir) {
  const result = run(nodeBin, [ '--test', '--test-reporter=tap', contractTest ], {
    cwd: repoRoot,
    env: {
      ...process.env,
      CHECK_BUILD_DIR: buildDir,
      NODE_PATH: path.join(repoRoot, 'node_modules'),
    },
  });
  if (result.signal) {
    fail(`contract runner exited abnormally via signal ${result.signal}`);
  }
  return { status: result.status, output: `${result.stdout || ''}${result.stderr || ''}` };
}

const POSITION_PATTERN = /(edge #\d+[^\n]*|list position [^\n]*|[A-Za-z ]*list cell #\d+[^\n]*)/u;

function evaluateTap(output, expectedTopLevel) {
  const testsMatch = output.match(/^# tests (\d+)$/mu);
  if (!testsMatch) {
    fail('contract run produced no TAP summary (crash or zero executed tests)');
  }
  const testCount = Number(testsMatch[1]);
  if (testCount < 4) {
    fail(`expected at least the four top-level tests to execute, got ${testCount}`);
  }

  const skipped = output.match(/^# skipped (\d+)$/mu);
  const cancelled = output.match(/^# cancelled (\d+)$/mu);
  const todo = output.match(/^# todo (\d+)$/mu);
  if (!skipped || skipped[1] !== '0') {
    fail('contract run contains skipped tests');
  }
  if (!cancelled || cancelled[1] !== '0') {
    fail('contract run contains cancelled tests');
  }
  if (todo && todo[1] !== '0') {
    fail('contract run contains todo tests');
  }

  const failCount = Number((output.match(/^# fail (\d+)$/mu) || [])[1] || 0);
  if (failCount === 0) {
    fail(`mutation was not detected: ${expectedTopLevel} still passes`);
  }

  const topLevelFailure = new RegExp(`^not ok \\d+ - ${expectedTopLevel}$`, 'mu');
  if (!topLevelFailure.test(output)) {
    fail(`graph assertion failure was not attributed to ${expectedTopLevel}`);
  }

  // The first failing edge or list position comes from the graph assertions only;
  // a parser crash would surface without any of these position markers.
  const position = output.match(POSITION_PATTERN);
  if (!position) {
    fail(`${expectedTopLevel} failed, but not at a named edge or list position`);
  }
  return { failCount, position: position[0].trim().replace(/'+$/u, '') };
}

// Pre-flight: every required input must exist before any temp work starts.
requireFile(contractTest, 'contract test');
requireFile(parserSource, 'parser source');
requireFile(tscBin, 'TypeScript compiler');
for (const testCase of CASES) {
  requireFile(testCase.patch, 'mutation patch');
}

// Sanity run: the unpatched parser in a clean copy must pass the contract first,
// otherwise a later failure cannot be attributed to the applied mutation.
{
  const cleanRoot = makeTempCopy();
  try {
    const buildDir = compileBuild(cleanRoot);
    const cleanRun = runContract(buildDir);
    if (cleanRun.status !== 0) {
      process.stderr.write(cleanRun.output);
      fail('unpatched parser fails the contract in a clean temp copy; refusing to score mutations');
    }
    if (!/^# fail 0$/mu.test(cleanRun.output)) {
      fail('unpatched parser contract did not report zero failures');
    }
  } finally {
    fs.rmSync(cleanRoot, { recursive: true, force: true });
  }
}

for (const testCase of CASES) {
  const root = makeTempCopy();
  try {
    const targetSource = path.join(root, 'lib', 'RdfaParser.ts');
    const patchResult = run('patch', [ '-p1', '--batch', '--forward', '--directory', root ], {
      input: fs.readFileSync(testCase.patch),
    });
    if (patchResult.status !== 0) {
      process.stderr.write(patchResult.stdout);
      process.stderr.write(patchResult.stderr);
      fail(`patch did not apply cleanly: ${testCase.patch}`);
    }

    const mutated = fs.readFileSync(targetSource, 'utf8');
    const original = fs.readFileSync(parserSource, 'utf8');
    if (mutated === original || !mutated.includes(testCase.mutationTag)) {
      fail(`patch content not present after application: ${testCase.patch}`);
    }

    const buildDir = compileBuild(root);
    const contractRun = runContract(buildDir);
    if (contractRun.status === 0) {
      fail(`mutation compiled but the contract still passes: ${testCase.patch}`);
    }
    const { failCount, position } = evaluateTap(contractRun.output, testCase.topLevel);
    process.stdout.write(`${testCase.marker} failures=${failCount} first=${position}\n`);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
}
