import {build} from 'esbuild';
import {existsSync, mkdirSync, readFileSync, writeFileSync} from 'node:fs';
import {resolve, join} from 'node:path';
import {spawnSync} from 'node:child_process';
const mode = process.argv[2];
const dir = resolve('.build');
mkdirSync(dir, {recursive: true});
const core = resolve('devtools/node_modules/puppeteer-core/lib/puppeteer/puppeteer-core.js');
if (!existsSync(core)) throw new Error('run npm ci --prefix devtools first');
const versions = Object.fromEntries(['esbuild', 'puppeteer-core'].map(name => [name,
  JSON.parse(readFileSync(`devtools/node_modules/${name}/package.json`)).version]));
if (versions.esbuild !== '0.25.5' || versions['puppeteer-core'] !== '25.15.0') throw new Error('dependency version mismatch');
console.log(`LOCKED_TOOLS esbuild=${versions.esbuild} puppeteer-core=${versions['puppeteer-core']}`);
async function run(entry, {test = false, names = [], pattern} = {}) {
  if (!existsSync(entry) || !readFileSync(entry, 'utf8').trim()) throw new Error(`Required source missing or empty: ${entry}`);
  const out = join(dir, entry.replaceAll('/', '_') + '.mjs');
  await build({entryPoints: [entry], bundle: true, platform: 'node', format: 'esm',
    packages: 'external', outfile: out, logLevel: 'silent', plugins: [{name: 'core-for-public-browser-import',
      setup(b) { b.onResolve({filter: /^(puppeteer|puppeteer-core)$/}, () => ({path: core, external: true})); }}]});
  const args = [...(test ? ['--test', '--test-reporter=tap', ...(pattern ? [`--test-name-pattern=${pattern}`] : [])] : []), out];
  const r = spawnSync(process.execPath, args, {encoding: 'utf8', timeout: 90000,
    env: {...process.env, TMPDIR: dir}});
  process.stdout.write(r.stdout || ''); process.stderr.write(r.stderr || '');
  writeFileSync(join(dir, entry.replaceAll('/', '_') + '.log'), (r.stdout || '') + (r.stderr || ''));
  if (r.error) throw r.error;
  if (r.status !== 0) throw new Error(`${entry} exited ${r.status}`);
  if (test) {
    if (!/^# pass [1-9]\d*$/m.test(r.stdout) || !/^# fail 0$/m.test(r.stdout)) throw new Error(`No passing tests: ${entry}`);
    for (const name of names) {
      const escaped = name.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
      if (!new RegExp(`^ok \\d+ - ${escaped}\\s*$`, 'm').test(r.stdout)) throw new Error(`Required top-level test did not pass: ${name}`);
    }
    if (names.length && (!/^# skipped 0$/m.test(r.stdout) || !/^# todo 0$/m.test(r.stdout) || !/^# cancelled 0$/m.test(r.stdout))) throw new Error('Target tests must not skip, cancel, or be todo');
  }
}
if (mode === 'baseline') await run('devtools/local-baseline.mjs');
else if (mode === 'target') await run('download-contract.test.ts', {test: true,
  names: ['download correlation', 'download settlement', 'download cleanup', 'download generated parity']});
else if (mode === 'chrome-baseline') { await run('devtools/chrome-baseline.mjs'); process.exit(0); }
else if (mode !== 'old') throw new Error('use baseline, target, old, or chrome-baseline');
for (const entry of ['test/InMemoryLineWriter.test.ts', 'test/vlq.test.ts']) await run(entry, {test: true});
// Parsing is directly affected by the proposed schema extension. Other branches
// of this upstream file use a browser and are intentionally not selected here.
await run('test/SchemaUtils.test.ts', {test: true, pattern: 'Schema parser'});
console.log('REPLAY_SELECTED_OLD_TESTS_PASSED');
