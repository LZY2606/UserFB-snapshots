'use strict';
const fs = require('node:fs');
const path = require('node:path');
const {runGroup} = require('./check');
const target = path.resolve(__dirname, '../test/drag-contract.js');
const names = ['capture_route', 'cleanup_state', 'isolation_reuse'];
(async () => {
    if (!fs.existsSync(target) || !fs.readFileSync(target, 'utf8').trim()) throw new Error('target missing or empty: test/drag-contract.js');
    const groups = require(target);
    for (const name of names) if (typeof groups[name] !== 'function') throw new Error('unregistered group: ' + name);
    for (const name of names) {
        const count = await runGroup(name, groups[name]);
        console.log('TASK_GROUP_OK ' + name + ' assertions=' + count);
    }
    console.log('TASK_TARGET_COMPLETE');
})().catch(error => { console.error(error); process.exitCode=1; });
