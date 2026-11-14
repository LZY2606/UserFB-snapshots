'use strict';
const {spawnSync} = require('node:child_process');
const path = require('node:path');
const mode = process.argv[2];
const options = {cwd:path.resolve(__dirname,'..'), encoding:'utf8', timeout:12000, maxBuffer:4*1024*1024};
function run(args) {
    const result = spawnSync(process.execPath, ['--unhandled-rejections=strict', ...args], options);
    process.stdout.write(result.stdout || '');
    process.stderr.write(result.stderr || '');
    if (result.error) throw result.error;
    if (result.status !== 0) throw new Error('child failed: ' + String(result.status));
    return result.stdout;
}
try {
    if (mode === 'baseline') {
        run(['--test', 'task_support/baseline.test.cjs']);
        run(['task_support/native-baseline.js']);
        console.log('BASELINE_OK');
    } else if (mode === 'target') {
        const out = run(['task_support/driver.js']);
        for (const name of ['capture_route','cleanup_state','isolation_reuse']) {
            const pattern = new RegExp('^TASK_GROUP_OK ' + name + ' assertions=[1-9][0-9]*$', 'm');
            if (!pattern.test(out)) throw new Error('missing completed group: ' + name);
        }
        if (!/^TASK_TARGET_COMPLETE$/m.test(out)) throw new Error('missing final driver marker');
        console.log('TARGET_OK');
    } else throw new Error('usage: run.sh baseline|target');
} catch (error) { console.error(error); process.exitCode=1; }
