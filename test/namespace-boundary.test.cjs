'use strict';

// Namespace boundary tests for custom `lockfilePath` lock spaces.
//
// The interleaving tests use an in-memory fs with manually delivered callbacks
// and node:test mock timers, so no real sleeps or timeout tuning are involved.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const childProcess = require('child_process');

const lockfile = require('..');
const { getLocks } = require('../lib/lockfile');

// ---------------------------------------------------------------------------
// Virtual fs: every operation is queued and only runs when the test says so.
// ---------------------------------------------------------------------------

function createVirtualFs() {
    const entries = new Map();
    const queue = [];
    const calls = {
        mkdir: [],
        rmdir: [],
        stat: [],
        utimes: [],
        realpath: [],
    };

    function enqueue(run) {
        queue.push({ run });
    }

    const vfs = {
        calls,
        runAll() {
            const pending = queue.splice(0);

            pending.forEach((op) => op.run());
        },
        pendingCount() {
            return queue.length;
        },
        exists(p) {
            return entries.has(p);
        },
        entryMtime(p) {
            const entry = entries.get(p);

            return entry && entry.mtime;
        },
        setEntryMtime(p, mtime) {
            entries.get(p).mtime = new Date(mtime);
        },
        removeEntry(p) {
            entries.delete(p);
        },
        realpath(p, cb) {
            calls.realpath.push(p);
            enqueue(() => cb(null, path.resolve(p)));
        },
        mkdir(p, cb) {
            calls.mkdir.push(p);
            enqueue(() => {
                if (entries.has(p)) {
                    return cb(Object.assign(new Error(`EEXIST: ${p}`), { code: 'EEXIST' }));
                }

                entries.set(p, { type: 'dir', mtime: new Date(Date.now()) });
                cb(null);
            });
        },
        rmdir(p, cb) {
            calls.rmdir.push(p);
            enqueue(() => {
                if (!entries.has(p)) {
                    return cb(Object.assign(new Error(`ENOENT: ${p}`), { code: 'ENOENT' }));
                }

                entries.delete(p);
                cb(null);
            });
        },
        stat(p, cb) {
            calls.stat.push(p);
            enqueue(() => {
                const entry = entries.get(p);

                if (!entry) {
                    return cb(Object.assign(new Error(`ENOENT: ${p}`), { code: 'ENOENT' }));
                }

                cb(null, {
                    mtime: new Date(entry.mtime.getTime()),
                    isDirectory: () => entry.type === 'dir',
                });
            });
        },
        utimes(p, atime, mtime, cb) {
            calls.utimes.push(p);
            enqueue(() => {
                const entry = entries.get(p);

                if (!entry) {
                    return cb(Object.assign(new Error(`ENOENT: ${p}`), { code: 'ENOENT' }));
                }

                entry.mtime = new Date(mtime.getTime());
                cb(null);
            });
        },
    };

    return vfs;
}

// A harness wraps the virtual fs and allows individual callback deliveries to
// be held back, so in-flight heartbeat operations can be interleaved precisely.
function createHarness() {
    const vfs = createVirtualFs();
    const held = [];
    const hang = { stat: 0, utimes: 0 };
    const cfs = {
        realpath: (...args) => vfs.realpath(...args),
        mkdir: (...args) => vfs.mkdir(...args),
        rmdir: (...args) => vfs.rmdir(...args),
        stat(p, cb) {
            if (hang.stat > 0) {
                hang.stat -= 1;
                vfs.calls.stat.push(p);

                return held.push({ run: () => vfs.stat(p, cb) });
            }

            vfs.stat(p, cb);
        },
        utimes(p, atime, mtime, cb) {
            if (hang.utimes > 0) {
                hang.utimes -= 1;
                vfs.calls.utimes.push(p);

                return held.push({ run: () => vfs.utimes(p, atime, mtime, cb) });
            }

            vfs.utimes(p, atime, mtime, cb);
        },
    };

    return {
        fs: cfs,
        vfs,
        hangNext(method) {
            hang[method] += 1;
        },
        heldCount() {
            return held.length;
        },
        deliverHeld(index = 0) {
            held.splice(index, 1)[0].run();
        },
    };
}

async function flush(h, times = 8) {
    for (let i = 0; i < times; i += 1) {
        h.vfs.runAll();
        // eslint-disable-next-line no-await-in-loop
        await Promise.resolve();
    }
}

async function whenSettled(h, promise) {
    let settled = false;
    const marker = promise.then(() => {
        settled = true;
    }, () => {
        settled = true;
    });

    for (let i = 0; i < 30; i += 1) {
        if (settled) {
            break;
        }

        h.vfs.runAll();
        // eslint-disable-next-line no-await-in-loop
        await Promise.resolve();
    }

    await marker;

    return promise;
}

function count(calls, method, p) {
    return calls[method].filter((entry) => entry === p).length;
}

function makeTempDir() {
    return fs.mkdtempSync(path.join(os.tmpdir(), 'proper-lockfile-namespace-'));
}

const TARGET = '/work/target';
const LOCK_A = '/work/a.lock';
const LOCK_B = '/work/b.lock';
const SHARED = '/work/shared.lock';

function baseOptions(h, extra) {
    return {
        fs: h.fs,
        realpath: false,
        stale: 4000,
        update: 1000,
        onCompromised(err) {
            throw err;
        },
        ...extra,
    };
}

// ---------------------------------------------------------------------------
// 1. The regression: two custom namespaces on the same target
// ---------------------------------------------------------------------------

test('two custom namespaces on the same target are independently held, checked and released', async (t) => {
    t.mock.timers.enable({ apis: ['setTimeout', 'Date'], now: 1000000 });

    const h = createHarness();
    const compromised = [];
    const options = baseOptions(h, { onCompromised: (err) => compromised.push(err) });

    const releaseA = await whenSettled(h, lockfile.lock(TARGET, { ...options, lockfilePath: LOCK_A }));
    const releaseB = await whenSettled(h, lockfile.lock(TARGET, { ...options, lockfilePath: LOCK_B }));

    assert.equal(h.vfs.exists(LOCK_A), true);
    assert.equal(h.vfs.exists(LOCK_B), true);
    assert.equal(Object.keys(getLocks()).length, 2);

    // Both namespaces renew their own lease on the same cadence
    t.mock.timers.tick(1000);
    await flush(h);

    const mtimeA1 = h.vfs.entryMtime(LOCK_A).getTime();
    const mtimeB1 = h.vfs.entryMtime(LOCK_B).getTime();

    assert.ok(count(h.vfs.calls, 'utimes', LOCK_A) >= 1);
    assert.ok(count(h.vfs.calls, 'utimes', LOCK_B) >= 1);

    t.mock.timers.tick(1000);
    await flush(h);

    assert.ok(h.vfs.entryMtime(LOCK_A).getTime() > mtimeA1);
    assert.ok(h.vfs.entryMtime(LOCK_B).getTime() > mtimeB1);

    // Releasing A must not touch B's heartbeat nor its released state
    await whenSettled(h, releaseA());
    await flush(h);

    assert.equal(h.vfs.exists(LOCK_A), false);
    assert.equal(h.vfs.exists(LOCK_B), true);
    assert.equal(Object.keys(getLocks()).length, 1);

    const utimesOnBAfterRelease = count(h.vfs.calls, 'utimes', LOCK_B);
    const utimesOnAAfterRelease = count(h.vfs.calls, 'utimes', LOCK_A);
    const mtimeB2 = h.vfs.entryMtime(LOCK_B).getTime();

    t.mock.timers.tick(1000);
    await flush(h);

    assert.ok(h.vfs.entryMtime(LOCK_B).getTime() > mtimeB2);
    assert.ok(count(h.vfs.calls, 'utimes', LOCK_B) > utimesOnBAfterRelease);
    assert.equal(count(h.vfs.calls, 'utimes', LOCK_A), utimesOnAAfterRelease);
    assert.equal(compromised.length, 0);

    // The old A closure cannot release twice and cannot release B
    await assert.rejects(releaseA(), (err) => err.code === 'ERELEASED');
    assert.equal(h.vfs.exists(LOCK_B), true);

    // A third applicant cannot steal B while B keeps renewing
    await assert.rejects(
        whenSettled(h, lockfile.lock(TARGET, { ...options, lockfilePath: LOCK_B, stale: 2000 })),
        (err) => err.code === 'ELOCKED');
    assert.equal(
        await whenSettled(h, lockfile.check(TARGET, { ...options, lockfilePath: LOCK_B })),
        true);

    // B's own closure still releases exactly B
    await whenSettled(h, releaseB());
    await flush(h);

    assert.equal(h.vfs.exists(LOCK_B), false);
    assert.equal(Object.keys(getLocks()).length, 0);
    assert.equal(compromised.length, 0);
});

test('a live lease in another namespace is not given away to a stale applicant', async (t) => {
    t.mock.timers.enable({ apis: ['setTimeout', 'Date'], now: 2000000 });

    const h = createHarness();
    const options = baseOptions(h, { onCompromised: () => {} });

    const releaseA = await whenSettled(h, lockfile.lock(TARGET, { ...options, lockfilePath: LOCK_A }));
    const releaseB = await whenSettled(h, lockfile.lock(TARGET, { ...options, lockfilePath: LOCK_B }));

    // Release A; B keeps getting renewed
    await whenSettled(h, releaseA());
    await flush(h);

    assert.equal(h.vfs.exists(LOCK_A), false);
    assert.equal(h.vfs.exists(LOCK_B), true);

    t.mock.timers.tick(1000);
    await flush(h);
    t.mock.timers.tick(1000);
    await flush(h);

    // B is fresh: stale takeover must fail even with the smallest stale window
    await assert.rejects(
        whenSettled(h, lockfile.lock(TARGET, { ...options, lockfilePath: LOCK_B, stale: 2000 })),
        (err) => err.code === 'ELOCKED');

    // Once B is actually stale (no more renewals, clock past the threshold),
    // the ordinary stale takeover still works in a custom namespace
    h.vfs.setEntryMtime(LOCK_B, new Date(0));

    const releaseC = await whenSettled(
        h,
        lockfile.lock(TARGET, { ...options, lockfilePath: LOCK_B, stale: 2000 }));

    await flush(h);

    assert.equal(h.vfs.exists(LOCK_B), true);
    assert.equal(Object.keys(getLocks()).length, 1);

    await whenSettled(h, releaseC());
    await flush(h);
    await releaseB().catch(() => {}); // B was superseded; its closure must be inert
});

// ---------------------------------------------------------------------------
// 2. Same namespace contention
// ---------------------------------------------------------------------------

test('two acquisitions in the same namespace arbitrate through one physical mkdir', async (t) => {
    t.mock.timers.enable({ apis: ['setTimeout', 'Date'], now: 3000000 });

    const h = createHarness();
    const options = baseOptions(h, { lockfilePath: LOCK_A });

    await whenSettled(h, lockfile.lock(TARGET, options));

    await assert.rejects(
        whenSettled(h, lockfile.lock(TARGET, { ...options })),
        (err) => err.code === 'ELOCKED');

    // Only one physical mkdir ever succeeded for this lock space
    assert.equal(count(h.vfs.calls, 'mkdir', LOCK_A), 2);
    assert.equal(
        await whenSettled(h, lockfile.check(TARGET, { ...options })),
        true);

    await whenSettled(h, lockfile.unlock(TARGET, { ...options }));
    await flush(h);

    assert.equal(h.vfs.exists(LOCK_A), false);
    assert.equal(
        await whenSettled(h, lockfile.check(TARGET, { ...options })),
        false);

    const release2 = await whenSettled(h, lockfile.lock(TARGET, options));

    assert.equal(h.vfs.exists(LOCK_A), true);

    await whenSettled(h, release2());
    await flush(h);

    assert.equal(h.vfs.exists(LOCK_A), false);
});

// ---------------------------------------------------------------------------
// 3. Two targets pointing at the same effective lockfilePath share one lock
// ---------------------------------------------------------------------------

test('different targets sharing one lockfilePath still contend on the physical lock', async (t) => {
    t.mock.timers.enable({ apis: ['setTimeout', 'Date'], now: 4000000 });

    const h = createHarness();
    const options = baseOptions(h, { lockfilePath: SHARED });

    const release = await whenSettled(h, lockfile.lock('/work/target-one', options));

    await assert.rejects(
        whenSettled(h, lockfile.lock('/work/target-two', { ...options })),
        (err) => err.code === 'ELOCKED');

    assert.equal(h.vfs.exists(SHARED), true);
    assert.equal(Object.keys(getLocks()).length, 1);

    await whenSettled(h, release());
    await flush(h);

    const release2 = await whenSettled(h, lockfile.lock('/work/target-two', { ...options }));

    assert.equal(h.vfs.exists(SHARED), true);

    await whenSettled(h, release2());
    await flush(h);

    assert.equal(h.vfs.exists(SHARED), false);
});

// ---------------------------------------------------------------------------
// 4. Absolute/relative equivalence and cwd independence after acquisition
// ---------------------------------------------------------------------------

test('relative lockfilePath is absolutized at acquisition time and survives a cwd change', (t, done) => {
    const cwd = process.cwd();
    const dir = makeTempDir();
    const target = path.join(dir, 'target');
    const absoluteLock = path.join(dir, 'space.lock');

    fs.writeFileSync(target, '');

    try {
        process.chdir(dir);

        // A relative lockfile path is fixed against the acquisition-time cwd
        const release = lockfile.lockSync(target, { realpath: false, lockfilePath: 'space.lock' });

        assert.equal(fs.existsSync(absoluteLock), true);

        // The absolute lexical form identifies the same effective namespace
        assert.equal(lockfile.checkSync(target, { realpath: false, lockfilePath: absoluteLock }), true);

        // Textual variants of the same effective path contend on the same mkdir
        assert.throws(
            () => lockfile.lockSync(target, { realpath: false, lockfilePath: absoluteLock }),
            /already being held/);

        // Change cwd before releasing: the stored absolute path must still be used
        process.chdir(os.tmpdir());

        // A relative path now resolves to a different, empty namespace: it must
        // report ENOTACQUIRED and never remove the acquired directory, nor a
        // directory in the new cwd
        assert.throws(
            () => lockfile.unlockSync(target, { realpath: false, lockfilePath: 'space.lock' }),
            /not acquired\/owned by you/);
        assert.equal(fs.existsSync(absoluteLock), true);
        assert.equal(fs.existsSync(path.join(os.tmpdir(), 'space.lock')), false);

        // The release closure itself carries the absolute path fixed at acquisition
        release();

        assert.equal(fs.existsSync(absoluteLock), false);
        assert.equal(fs.existsSync(path.join(os.tmpdir(), 'space.lock')), false);
        done();
    } catch (err) {
        done(err);
    } finally {
        process.chdir(cwd);
        fs.rmSync(dir, { recursive: true, force: true });
    }
});

// ---------------------------------------------------------------------------
// 5. Default and custom namespaces coexisting on one target
// ---------------------------------------------------------------------------

test('default and custom namespaces coexist and unlock() only touches the named space', async (t) => {
    const dir = makeTempDir();
    const target = path.join(dir, 'target');
    const customLock = path.join(dir, 'custom.lock');
    const defaultLock = `${target}.lock`;

    fs.writeFileSync(target, '');

    t.after(() => fs.rmSync(dir, { recursive: true, force: true }));

    const releaseDefault = await lockfile.lock(target);
    const releaseCustom = await lockfile.lock(target, { lockfilePath: customLock });

    assert.equal(fs.existsSync(defaultLock), true);
    assert.equal(fs.existsSync(customLock), true);
    assert.equal(Object.keys(getLocks()).length, 2);

    assert.equal(await lockfile.check(target), true);
    assert.equal(await lockfile.check(target, { lockfilePath: customLock }), true);

    // Unlock without a lockfilePath only addresses the default space
    await lockfile.unlock(target);

    assert.equal(fs.existsSync(defaultLock), false);
    assert.equal(fs.existsSync(customLock), true);
    await assert.rejects(lockfile.unlock(target), (err) => err.code === 'ENOTACQUIRED');
    assert.equal(fs.existsSync(customLock), true);

    // The custom lease is still alive and can be renewed/released
    assert.equal(await lockfile.check(target, { lockfilePath: customLock }), true);
    await releaseCustom();

    assert.equal(fs.existsSync(customLock), false);
    assert.equal(Object.keys(getLocks()).length, 0);

    await releaseDefault().catch((err) => assert.equal(err.code, 'ERELEASED'));
});

// ---------------------------------------------------------------------------
// 6. Synchronous API parity
// ---------------------------------------------------------------------------

test('sync API keeps namespaces independent and scopes unlock to the named space', (t, done) => {
    const dir = makeTempDir();
    const target = path.join(dir, 'target');
    const lockA = path.join(dir, 'a.lock');
    const lockB = path.join(dir, 'b.lock');
    const defaultLock = `${target}.lock`;

    fs.writeFileSync(target, '');

    try {
        const releaseA = lockfile.lockSync(target, { lockfilePath: lockA });
        const releaseB = lockfile.lockSync(target, { lockfilePath: lockB });

        assert.equal(fs.existsSync(lockA), true);
        assert.equal(fs.existsSync(lockB), true);

        // Same namespace contention
        assert.throws(() => lockfile.lockSync(target, { lockfilePath: lockA }), /already being held/);

        // checkSync is namespace aware
        assert.equal(lockfile.checkSync(target, { lockfilePath: lockA }), true);
        assert.equal(lockfile.checkSync(target, { lockfilePath: lockB }), true);
        assert.equal(lockfile.checkSync(target), false);

        // unlockSync only removes the named space
        lockfile.unlockSync(target, { lockfilePath: lockA });

        assert.equal(fs.existsSync(lockA), false);
        assert.equal(fs.existsSync(lockB), true);

        // The old closure cannot release twice
        assert.throws(releaseA, (err) => err.code === 'ERELEASED');
        assert.equal(fs.existsSync(lockB), true);

        // Default unlock must not pick a custom space
        assert.throws(() => lockfile.unlockSync(target), /not acquired\/owned by you/);
        assert.equal(fs.existsSync(lockB), true);

        releaseB();

        assert.equal(fs.existsSync(lockB), false);
        assert.equal(fs.existsSync(defaultLock), false);
        done();
    } catch (err) {
        done(err);
    } finally {
        fs.rmSync(dir, { recursive: true, force: true });
    }
});

// ---------------------------------------------------------------------------
// 7. An old release closure cannot release a reacquired lease
// ---------------------------------------------------------------------------

test('a stale release closure reports ERELEASED and cannot release the newer lease', async (t) => {
    t.mock.timers.enable({ apis: ['setTimeout', 'Date'], now: 5000000 });

    const h = createHarness();
    const options = baseOptions(h, { lockfilePath: LOCK_A });

    const releaseA = await whenSettled(h, lockfile.lock(TARGET, options));

    await whenSettled(h, releaseA());
    await flush(h);

    assert.equal(h.vfs.exists(LOCK_A), false);

    const releaseB = await whenSettled(h, lockfile.lock(TARGET, { ...options }));

    assert.equal(h.vfs.exists(LOCK_A), true);
    assert.equal(Object.keys(getLocks()).length, 1);

    // The old closure must not release the new lease
    await assert.rejects(releaseA(), (err) => err.code === 'ERELEASED');
    assert.equal(h.vfs.exists(LOCK_A), true);
    assert.equal(Object.keys(getLocks()).length, 1);

    // The new lease renews and releases normally
    const mtimeBefore = h.vfs.entryMtime(LOCK_A).getTime();

    t.mock.timers.tick(1000);
    await flush(h);

    assert.ok(h.vfs.entryMtime(LOCK_A).getTime() > mtimeBefore);

    await whenSettled(h, releaseB());
    await flush(h);

    assert.equal(h.vfs.exists(LOCK_A), false);

    // unlock() on the empty space reports ENOTACQUIRED, it does not invent a lease
    await assert.rejects(
        whenSettled(h, lockfile.unlock(TARGET, { ...options })),
        (err) => err.code === 'ENOTACQUIRED');
});

// ---------------------------------------------------------------------------
// 8. Each lease keeps its own fs and options
// ---------------------------------------------------------------------------

test('each lease stores and uses its own fs/options, never borrowing from another active lease', async (t) => {
    t.mock.timers.enable({ apis: ['setTimeout', 'Date'], now: 6000000 });

    const hA = createHarness();
    const hB = createHarness();
    const releaseA = await whenSettled(hA, lockfile.lock(TARGET, baseOptions(hA, { lockfilePath: LOCK_A })));
    const releaseB = await whenSettled(hB, lockfile.lock(TARGET, baseOptions(hB, { lockfilePath: LOCK_B })));

    assert.equal(Object.keys(getLocks()).length, 2);

    // Both heartbeats operate exclusively through their own fs instance
    t.mock.timers.tick(1000);
    await flush(hA);
    await flush(hB);

    assert.ok(count(hA.vfs.calls, 'utimes', LOCK_A) >= 1);
    assert.equal(count(hA.vfs.calls, 'utimes', LOCK_B), 0);
    assert.ok(count(hB.vfs.calls, 'utimes', LOCK_B) >= 1);
    assert.equal(count(hB.vfs.calls, 'utimes', LOCK_A), 0);

    // Releasing A uses A's fs and options only; B's fs sees nothing at all
    const bCallsBefore = hB.vfs.calls.rmdir.length;

    await whenSettled(hA, releaseA());
    await flush(hA);

    assert.equal(hA.vfs.exists(LOCK_A), false);
    assert.equal(hB.vfs.exists(LOCK_B), true);
    assert.equal(hB.vfs.calls.rmdir.length, bCallsBefore);

    // B keeps renewing through its own fs after A is gone
    const bMtime = hB.vfs.entryMtime(LOCK_B).getTime();

    t.mock.timers.tick(1000);
    await flush(hB);

    assert.ok(hB.vfs.entryMtime(LOCK_B).getTime() > bMtime);

    await whenSettled(hB, releaseB());
    await flush(hB);

    assert.equal(hB.vfs.exists(LOCK_B), false);
    assert.equal(Object.keys(getLocks()).length, 0);
});

// ---------------------------------------------------------------------------
// 9. Delayed heartbeat callbacks from a released lease must not hit the new one
// ---------------------------------------------------------------------------

test('a delayed stat callback from the released lease never touches the reacquired lease', async (t) => {
    t.mock.timers.enable({ apis: ['setTimeout', 'Date'], now: 6000000 });

    const h = createHarness();
    const compromised = [];
    const options = baseOptions(h, { lockfilePath: LOCK_A, onCompromised: (err) => compromised.push(err) });

    const releaseA = await whenSettled(h, lockfile.lock(TARGET, options));

    // Heartbeat stat is in flight when A is released and B takes the same space
    h.hangNext('stat');
    t.mock.timers.tick(1000);

    assert.equal(h.heldCount(), 1);

    await whenSettled(h, releaseA());
    await flush(h);

    assert.equal(h.vfs.exists(LOCK_A), false);

    const releaseB = await whenSettled(h, lockfile.lock(TARGET, { ...options }));

    assert.equal(h.vfs.exists(LOCK_A), true);

    // Deliver the old stat callback: it must die quietly
    h.deliverHeld();
    await flush(h);

    const statsAfterDelivery = count(h.vfs.calls, 'stat', LOCK_A);
    const bMtime = h.vfs.entryMtime(LOCK_A).getTime();

    assert.equal(compromised.length, 0);
    assert.equal(h.vfs.entryMtime(LOCK_A).getTime(), bMtime);
    assert.equal(Object.keys(getLocks()).length, 1);

    // Only B's heartbeat fires on the next tick (one stat, one utimes)
    t.mock.timers.tick(1000);
    await flush(h);

    assert.equal(count(h.vfs.calls, 'stat', LOCK_A), statsAfterDelivery + 1);
    assert.ok(h.vfs.entryMtime(LOCK_A).getTime() > bMtime);
    assert.equal(compromised.length, 0);

    await whenSettled(h, releaseB());
    await flush(h);
});

test('a delayed utimes callback from the released lease never reschedules nor compromises the new lease', async (t) => {
    t.mock.timers.enable({ apis: ['setTimeout', 'Date'], now: 7000000 });

    const h = createHarness();
    const compromised = [];
    const options = baseOptions(h, { lockfilePath: LOCK_A, onCompromised: (err) => compromised.push(err) });

    const releaseA = await whenSettled(h, lockfile.lock(TARGET, options));

    // Let the stat complete, then hold the utimes callback
    t.mock.timers.tick(1000);
    h.hangNext('utimes');
    await flush(h);

    assert.equal(h.heldCount(), 1);

    await whenSettled(h, releaseA());
    await flush(h);

    const releaseB = await whenSettled(h, lockfile.lock(TARGET, { ...options }));

    const bLease = getLocks()[Object.keys(getLocks())[0]];
    const bTrackedMtime = bLease.mtime.getTime();
    const statsBefore = count(h.vfs.calls, 'stat', LOCK_A);

    h.deliverHeld();
    await flush(h);

    // B's bookkeeping is untouched and A reported no compromise
    assert.equal(bLease.mtime.getTime(), bTrackedMtime);
    assert.equal(compromised.length, 0);
    assert.equal(getLocks()[bLease.key], bLease);

    // A must not have rescheduled itself: exactly one stat (B's) on the next tick
    t.mock.timers.tick(1000);
    await flush(h);

    assert.equal(count(h.vfs.calls, 'stat', LOCK_A), statsBefore + 1);
    assert.equal(compromised.length, 0);

    await whenSettled(h, releaseB());
    await flush(h);
});

test('a lease stolen by stale takeover reports compromised once without harming the new lease', async (t) => {
    t.mock.timers.enable({ apis: ['setTimeout', 'Date'], now: 8000000 });

    const h = createHarness();
    const compromisedA = [];
    const options = baseOptions(h, {
        lockfilePath: LOCK_A,
        onCompromised: (err) => compromisedA.push(err),
    });

    const releaseA = await whenSettled(h, lockfile.lock(TARGET, options));

    // Simulate A going stale and a third party taking over the physical lock
    h.vfs.setEntryMtime(LOCK_A, new Date(0));

    const releaseB = await whenSettled(
        h,
        lockfile.lock(TARGET, { ...options, stale: 2000, onCompromised: () => {} }));

    await flush(h);

    const bLease = getLocks()[Object.keys(getLocks())[0]];
    const statsBefore = count(h.vfs.calls, 'stat', LOCK_A);
    const utimesBefore = count(h.vfs.calls, 'utimes', LOCK_A);

    // A's pending heartbeat now notices it was superseded, while B's own
    // heartbeat renews B normally in the very same tick
    t.mock.timers.tick(1000);
    await flush(h);

    assert.equal(compromisedA.length, 1);
    assert.equal(compromisedA[0].code, 'ECOMPROMISED');
    assert.equal(getLocks()[bLease.key], bLease);
    // A performed no stat and no utimes: only B's heartbeat ran
    assert.equal(count(h.vfs.calls, 'stat', LOCK_A), statsBefore + 1);
    assert.equal(count(h.vfs.calls, 'utimes', LOCK_A), utimesBefore + 1);

    // A's compromise fires exactly once and never comes back
    const statsAfterCompromise = count(h.vfs.calls, 'stat', LOCK_A);

    t.mock.timers.tick(1000);
    await flush(h);

    assert.equal(compromisedA.length, 1);
    assert.equal(count(h.vfs.calls, 'stat', LOCK_A), statsAfterCompromise + 1);

    // A's closure is now inert; B still works
    await assert.rejects(releaseA(), (err) => err.code === 'ERELEASED');
    assert.equal(h.vfs.exists(LOCK_A), true);

    await whenSettled(h, releaseB());
    await flush(h);

    assert.equal(h.vfs.exists(LOCK_A), false);
});

// ---------------------------------------------------------------------------
// 10. Process exit cleanup walks every still-held namespace
// ---------------------------------------------------------------------------

test('normal process exit removes every lock still held and leaves foreign directories alone', () => {
    const dir = makeTempDir();
    const target = path.join(dir, 'target');
    const lockA = path.join(dir, 'a.lock');
    const lockB = path.join(dir, 'b.lock');
    const foreign = path.join(dir, 'foreign.lock');

    try {
        const result = childProcess.spawnSync(
            process.execPath,
            [path.join(__dirname, 'fixtures', 'namespace-exit.cjs'), target, lockA, lockB, foreign],
            { encoding: 'utf8' });

        assert.equal(result.status, 0, result.stderr);
        assert.equal(fs.existsSync(lockA), false, 'released namespace must stay gone');
        assert.equal(fs.existsSync(lockB), false, 'held namespace must be cleaned on exit');
        assert.equal(fs.existsSync(foreign), true, 'directories not owned by a lease must be untouched');
    } finally {
        fs.rmSync(dir, { recursive: true, force: true });
    }
});
