'use strict';
let current;
function check(condition, message) {
    if (!current) throw new Error('check outside group');
    current.count++;
    if (!condition) {
        current.failed = true;
        throw new Error(message || 'content assertion failed');
    }
}
async function runGroup(name, body) {
    current = {count:0, failed:false};
    const state = current;
    const result = await body();
    if (result && (result.skip || result.skipped)) throw new Error(name + ': skipped group');
    if (!state.count || state.failed) throw new Error(name + ': no assertions or persistent assertion failure');
    return state.count;
}
module.exports = {check, runGroup};
