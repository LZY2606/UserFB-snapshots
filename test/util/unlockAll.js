'use strict';

const { getLocks } = require('../../lib/lockfile');
const { unlock } = require('../..');

function unlockAll() {
    const locks = getLocks();
    const promises = Object.values(locks).map((lock) => unlock(lock.file, {
        realpath: false,
        lockfilePath: lock.lockfilePath,
        fs: lock.options.fs,
    }));

    return Promise.all(promises);
}

module.exports = unlockAll;
