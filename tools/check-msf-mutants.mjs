#!/usr/bin/env node
// Mutation checker for the structured MSF fuzz regressions (tests/msf_model.rs).
//
// It first proves the pristine tree passes, then applies three deliberately
// degenerate edits to the *library* source, each inside its own throwaway
// source copy. Every copy must still compile; a regression counts as caught
// only when the new tests execute and fail on an assertion that inspects real
// bytes, Source slices, or error results (the msf-* markers embedded in
// tests/msf_model.rs). Compilation failures, missing tests, zero executed
// cases, and timeouts are reported as failures of this checker.
//
// The original working tree is never modified: all edits happen in temporary
// copies, and the src/ tree is hashed before and after to prove it.

'use strict';

import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const REPO_ROOT = path.resolve(__dirname, '..');
const TEST_NAME = 'msf_model';
const TEST_TIMEOUT_MS = 10 * 60 * 1000;

// Assertion markers that identify failures over real bytes, request slices,
// or error results. Keep in sync with tests/msf_model.rs.
const CONTENT_MARKERS = [
  'msf-byte-mismatch',
  'msf-slice-mismatch',
  'msf-unexpected-ok',
  'msf-unexpected-err',
  'msf-view-mismatch',
];

const TEST_FUNCTIONS = ['model_layout', 'relocation_law', 'lazy_errors', 'source_views'];

const MUTANTS = [
  {
    id: 'absent-stream-occupies-page-slot',
    description: '不存在的前置流错误地占用了页号位置',
    file: 'src/msf/mod.rs',
    find: `                    if bytes == u32::max_value() {
                        // stream is not present, ergo nothing to skip
                    } else {`,
    replace: `                    if bytes == u32::max_value() {
                        // MUTANT: an absent stream wrongly consumes one page slot
                        page_numbers_to_skip += 1;
                    } else {`,
  },
  {
    id: 'noncontiguous-pages-coalesced',
    description: '非连续页被错误地合并成连续读取',
    file: 'src/msf/page_list.rs',
    find: `        let is_continuous = match self.last_page {
            Some(n) => n.checked_add(1) == Some(page),
            None => false,
        };`,
    replace: `        let is_continuous = match self.last_page {
            Some(_n) => true, // MUTANT: every pushed page is treated as contiguous
            None => false,
        };`,
  },
  {
    id: 'last-page-not-truncated',
    description: '最后一页不按流长截断',
    file: 'src/msf/mod.rs',
    find: `                // truncate to the size of the stream
                list.truncate(bytes_in_stream as usize);`,
    replace: `                // MUTANT: the stream is deliberately not truncated to its byte length`,
  },
];

function log(message) {
  process.stdout.write(`${message}\n`);
}

function hashDirectory(root) {
  const hash = crypto.createHash('sha256');
  const walk = (directory) => {
    for (const entry of fs.readdirSync(directory, { withFileTypes: true }).sort((a, b) =>
      a.name.localeCompare(b.name)
    )) {
      const full = path.join(directory, entry.name);
      if (entry.isDirectory()) {
        walk(full);
      } else {
        hash.update(path.relative(root, full));
        hash.update(fs.readFileSync(full));
      }
    }
  };
  walk(root);
  return hash.digest('hex');
}

function makeTempCopy(label) {
  const destination = fs.mkdtempSync(path.join(os.tmpdir(), `msf-mutant-${label}-`));
  fs.cpSync(REPO_ROOT, destination, {
    recursive: true,
    filter: (source) => {
      const relative = path.relative(REPO_ROOT, source);
      const first = relative.split(path.sep)[0];
      // Drop regenerable, machine-local, or VCS state. Each copy gets its own
      // target directory anyway.
      return !['target', '.cargo-home', '.git', 'node_modules'].includes(first);
    },
  });
  return destination;
}

function runTests(copyRoot, label) {
  const targetDir = fs.mkdtempSync(path.join(os.tmpdir(), `msf-mutant-target-${label}-`));
  const result = spawnSync(
    'cargo',
    ['test', '--locked', '--offline', '--test', TEST_NAME, '--', '--nocapture', '--test-threads=1'],
    {
      cwd: copyRoot,
      encoding: 'utf8',
      timeout: TEST_TIMEOUT_MS,
      env: {
        ...process.env,
        CARGO_HOME: path.join(REPO_ROOT, '.cargo-home'),
        CARGO_TARGET_DIR: targetDir,
        CARGO_NET_OFFLINE: 'true',
      },
    }
  );

  let output = '';
  if (result.error) {
    output = `spawn failed: ${result.error.message}\n`;
  } else {
    output = `${result.stdout || ''}${result.stderr || ''}`;
  }
  const timedOut = result.error && result.error.code === 'ETIMEDOUT';
  return {
    status: typeof result.status === 'number' ? result.status : -1,
    timedOut,
    output,
    targetDir,
  };
}

// Split sequential (--test-threads=1) test output into panic blocks.
function extractPanicBlocks(output) {
  const lines = output.split('\n');
  const blocks = [];
  let current = null;
  for (const line of lines) {
    if (/^thread '.*'(?: \(\d+\))? panicked at /.test(line)) {
      if (current) blocks.push(current);
      current = [line];
    } else if (current) {
      if (/^(test |test result:|running \d+ tests)/.test(line)) {
        blocks.push(current);
        current = null;
      } else {
        current.push(line);
      }
    }
  }
  if (current) blocks.push(current);
  return blocks.map((block) => block.join('\n'));
}

function sampleSizeOf(block) {
  const pageSize = block.match(/page_size=(\d+)/);
  const pagesUsed = block.match(/pages_used=(\d+)/);
  if (!pageSize || !pagesUsed) return Number.POSITIVE_INFINITY;
  return (Number(pagesUsed[1]) + 1) * Number(pageSize[1]);
}

function summarizeRun(output) {
  const ran = /running 4 tests/.test(output);
  const executed = TEST_FUNCTIONS.filter((name) =>
    new RegExp(`test ${name} \\.\\.\\. (ok|FAILED)`).test(output)
  );
  const failed = TEST_FUNCTIONS.filter((name) =>
    new RegExp(`test ${name} \\.\\.\\. FAILED`).test(output)
  );
  const compileError = /^error(?:\[[A-Z0-9]+\])?:/.test(output.trim()) || /could not compile/.test(output);
  const markersSeen = CONTENT_MARKERS.filter((marker) => output.includes(marker));
  return { ran, executed, failed, compileError, markersSeen };
}

function fail(message) {
  log(`FAIL: ${message}`);
  process.exitCode = 1;
}

function main() {
  const srcBefore = hashDirectory(path.join(REPO_ROOT, 'src'));
  log('== baseline: pristine tree must pass ==');

  const baselineCopy = makeTempCopy('baseline');
  const baseline = runTests(baselineCopy, 'baseline');
  if (baseline.timedOut) {
    fail('baseline run timed out');
    return;
  }
  if (baseline.status !== 0) {
    fail(`baseline does not pass (status ${baseline.status})\n${baseline.output}`);
    return;
  }
  const baselineSummary = summarizeRun(baseline.output);
  if (!baselineSummary.ran || baselineSummary.executed.length !== 4) {
    fail(`baseline did not execute all four tests\n${baseline.output}`);
    return;
  }
  log('baseline ok: model_layout, relocation_law, lazy_errors, source_views');

  for (const mutant of MUTANTS) {
    log('');
    log(`== mutant ${mutant.id}: ${mutant.description} ==`);
    const copyRoot = makeTempCopy(mutant.id);
    const targetFile = path.join(copyRoot, mutant.file);
    const original = fs.readFileSync(targetFile, 'utf8');
    if (!original.includes(mutant.find)) {
      fail(`${mutant.id}: could not locate the source pattern to mutate in ${mutant.file}`);
      continue;
    }
    const occurrences = original.split(mutant.find).length - 1;
    if (occurrences !== 1) {
      fail(`${mutant.id}: source pattern must be unique, found ${occurrences}`);
      continue;
    }
    fs.writeFileSync(targetFile, original.replace(mutant.find, mutant.replace));

    const run = runTests(copyRoot, mutant.id);
    if (run.timedOut) {
      fail(`${mutant.id}: test run timed out; timeout does not count as caught`);
      continue;
    }

    const summary = summarizeRun(run.output);
    if (summary.compileError) {
      fail(`${mutant.id}: mutant copy failed to compile; compilation failure does not count`);
      log(run.output.split('\n').slice(0, 25).join('\n'));
      continue;
    }
    if (!summary.ran || summary.executed.length === 0) {
      fail(`${mutant.id}: zero test cases executed; this does not count as caught`);
      log(run.output.split('\n').slice(0, 25).join('\n'));
      continue;
    }
    if (summary.failed.length === 0) {
      fail(`${mutant.id}: tests passed despite the degenerate behavior`);
      continue;
    }
    if (summary.markersSeen.length === 0) {
      fail(
        `${mutant.id}: failed but no byte/slice/error assertion marker was hit; \
library-internal panics do not count`
      );
      log(run.output.split('\n').slice(0, 25).join('\n'));
      continue;
    }

    // The regression is genuinely caught. Report the smallest failing sample.
    const markerBlocks = extractPanicBlocks(run.output).filter((block) =>
      CONTENT_MARKERS.some((marker) => block.includes(marker))
    );
    markerBlocks.sort((a, b) => sampleSizeOf(a) - sampleSizeOf(b));
    const smallest = markerBlocks[0];

    log(`CAUGHT: ${mutant.id} (${mutant.description})`);
    log(`  tests executed: ${summary.executed.join(', ')}`);
    log(`  tests failed:   ${summary.failed.join(', ')}`);
    log(`  markers:        ${summary.markersSeen.join(', ')}`);
    log('  minimal failing sample:');
    log(
      smallest
        .split('\n')
        .map((line) => `    ${line}`)
        .join('\n')
    );
  }

  const srcAfter = hashDirectory(path.join(REPO_ROOT, 'src'));
  log('');
  if (srcBefore !== srcAfter) {
    fail('original src/ tree changed while running mutants');
  } else {
    log('original src/ tree unchanged');
  }

  if (process.exitCode) {
    log('');
    log('MUTANT CHECK FAILED');
  } else {
    log('');
    log('MUTANT CHECK OK');
  }
}

main();
