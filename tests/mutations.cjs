// Mutation driver for the streaming parser test suite.
//
// Copies the library sources and tests/stream.test.cjs into a temporary
// directory, applies one behaviour-degrading edit to lib/vtt.js per copy,
// and runs the unmodified suite against each mutated copy. A mutation is
// "detected" only if the suite still loads and parses, and exactly the
// expected test cases fail. Missing anchors, zero executed tests, load
// errors, SyntaxErrors and timeouts all fail this driver instead.
//
// Prints VTT_MUTATION_OK <name> per detected mutation and VTT_MUTATIONS_OK
// once all of them are confirmed. Temporary copies are removed on exit; the
// working tree is never modified.
'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {spawnSync} = require('node:child_process');

const repoRoot = path.join(__dirname, '..');
const libFiles = [
  'index.js',
  'vtt.js',
  'vttcue.js',
  'vttcue-extended.js',
  'vttregion.js',
  'vttregion-extended.js'
];
const suiteFile = 'stream.test.cjs';
const suiteTests = ['replay-splits', 'line-endings', 'recovery-regions', 'decoder-eof', 'instance-isolation'];

const mutations = [
  {
    name: 'replay',
    description: 'drop reprocessing of the held timing line',
    anchor: 'self.heldLine = line;',
    replacement: '/* mutation: held timing line dropped */',
    expectFail: ['replay-splits', 'recovery-regions']
  },
  {
    name: 'crlf',
    description: 'treat a cross-chunk CRLF as two line breaks',
    anchor: 'if (self.sawCR && self.buffer) {',
    replacement: 'if (false) {',
    expectFail: ['line-endings']
  },
  {
    name: 'decoder-tail',
    description: "drop the final decoder.decode() output at flush",
    anchor: 'self.buffer += self.decoder.decode();',
    replacement: 'self.buffer += "";',
    expectFail: ['decoder-eof']
  }
];

function fail(message) {
  throw new Error('VTT_MUTATION_DRIVER_FAIL ' + message);
}

// Parse the top-level TAP results into {name: true|false}.
function tapResults(tap) {
  const results = new Map();
  for (const line of tap.split('\n')) {
    const m = line.match(/^(not )?ok \d+ - (.+?)\s*$/);
    if (m) {
      results.set(m[2], !m[1]);
    }
  }
  return results;
}

// Extract the "not ok" entries with their diagnostics for reporting.
function tapFailures(tap, names) {
  const lines = tap.split('\n');
  const blocks = [];
  let current = null;
  for (const line of lines) {
    const m = line.match(/^(not )?ok \d+ - (.+?)\s*$/);
    if (m) {
      current = m[1] && names.has(m[2]) ? [line] : null;
      if (current) {
        blocks.push(current);
      }
      continue;
    }
    if (current && (line.startsWith(' ') || line.startsWith('#'))) {
      current.push(line);
    }
  }
  return blocks.map(block => block.join('\n')).join('\n');
}

function runMutation(mutation) {
  const work = fs.mkdtempSync(path.join(os.tmpdir(), 'vtt-mutation-' + mutation.name + '-'));
  try {
    // Copy the library and the unmodified suite.
    fs.mkdirSync(path.join(work, 'lib'));
    fs.mkdirSync(path.join(work, 'tests'));
    for (const file of libFiles) {
      fs.copyFileSync(path.join(repoRoot, 'lib', file), path.join(work, 'lib', file));
    }
    fs.copyFileSync(path.join(repoRoot, 'tests', suiteFile), path.join(work, 'tests', suiteFile));

    // Apply the mutation; the anchor must exist exactly once.
    const vttPath = path.join(work, 'lib', 'vtt.js');
    const source = fs.readFileSync(vttPath, 'utf8');
    const occurrences = source.split(mutation.anchor).length - 1;
    if (occurrences !== 1) {
      fail(mutation.name + ': anchor not found exactly once (' + occurrences + '): ' + mutation.anchor);
    }
    fs.writeFileSync(vttPath, source.replace(mutation.anchor, mutation.replacement));

    // The mutated copy must still load and keep parsing.
    let mutated;
    try {
      mutated = require(path.join(work, 'lib'));
    } catch (e) {
      fail(mutation.name + ': mutated copy does not load: ' + e.message);
    }
    try {
      const p = new mutated.WebVTT.Parser(
        {VTTCue: mutated.VTTCue, VTTRegion: mutated.VTTRegion}, new TextDecoder());
      let seen = 0;
      p.oncue = () => seen++;
      p.parse(Buffer.from('WEBVTT\n\n00:01.000 --> 00:02.000\nhi\n\n'));
      p.flush();
      assert.equal(seen, 1);
    } catch (e) {
      fail(mutation.name + ': mutated copy cannot parse: ' + e.message);
    }

    // Run the normal suite against the mutated copy.
    const child = spawnSync(
      process.execPath,
      ['--test', '--test-reporter=tap', path.join('tests', suiteFile)],
      {cwd: work, encoding: 'utf8', timeout: 60000});
    if (child.error) {
      fail(mutation.name + ': suite run failed: ' + child.error.message);
    }
    if (child.signal) {
      fail(mutation.name + ': suite run killed by ' + child.signal + ' (timeout?)');
    }
    const tap = child.stdout || '';
    const results = tapResults(tap);
    if (results.size === 0) {
      fail(mutation.name + ': zero tests ran\n' + tap + (child.stderr || ''));
    }
    for (const name of suiteTests) {
      if (!results.has(name)) {
        fail(mutation.name + ': test missing from run: ' + name + '\n' + tap);
      }
    }

    const failed = suiteTests.filter(name => !results.get(name));
    const expected = [...mutation.expectFail].sort();
    const actual = failed.sort();
    if (JSON.stringify(actual) !== JSON.stringify(expected)) {
      fail(mutation.name + ': expected failures ' + JSON.stringify(expected) +
        ' but got ' + JSON.stringify(actual) + '\n' + tap + (child.stderr || ''));
    }
    if (child.status === 0) {
      fail(mutation.name + ': suite passed against mutated copy');
    }

    // Keep the failing case names and assertion diagnostics in the output.
    console.log(tapFailures(tap, new Set(expected)));
    console.log('VTT_MUTATION_OK ' + mutation.name);
  } finally {
    fs.rmSync(work, {recursive: true, force: true});
  }
}

for (const mutation of mutations) {
  runMutation(mutation);
}
console.log('VTT_MUTATIONS_OK');
