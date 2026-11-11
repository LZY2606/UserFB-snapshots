'use strict';

const fs = require('fs');
const lockfile = require('../../');

const [target, lockA, lockB, foreign] = process.argv.slice(2);

fs.writeFileSync(target, '');

(async () => {
    // Two independent lock namespaces on the same target
    const releaseA = await lockfile.lock(target, { lockfilePath: lockA });

    await lockfile.lock(target, { lockfilePath: lockB });

    // A directory that is not owned by any lease of this process
    fs.mkdirSync(foreign);

    // Explicitly release one namespace; the other stays held until exit
    await releaseA();

    // Natural exit: onExit must remove only the lock still held by this process
})().catch((err) => {
    console.error(err);
    process.exitCode = 1;
});
