'use strict';

const assert = require('node:assert');

const {check} = require('../task_support/check');
const {openPeer} = require('../task_support/peer');

function same(actual, expected) {
    try {
        assert.deepStrictEqual(actual, expected);
        return true;
    } catch (error) {
        return false;
    }
}

function sleep(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

// pull every incoming message into a local queue so that the tests can both
// await the next command and assert that no command has been sent
function collect(p) {
    const messages = [];
    const waiters = [];
    let stopped = false;
    (async () => {
        while (!stopped) {
            const message = await p.next();
            if (waiters.length) {
                waiters.shift()(message);
            } else {
                messages.push(message);
            }
        }
    })();
    return {
        next() {
            return messages.length
                ? Promise.resolve(messages.shift())
                : new Promise((resolve) => waiters.push(resolve));
        },
        size() {
            return messages.length;
        },
        stop() {
            stopped = true;
        }
    };
}

async function expectCommand(c, method, params, sessionId) {
    const m = await c.next();
    check(m && m.method === method,
        `expected \`${method}\`, got \`${m && m.method}\` (${JSON.stringify(m)})`);
    if (params !== undefined) {
        check(same(m.params, params),
            `unexpected params for \`${method}\`: ${JSON.stringify(m.params)}`);
    }
    check(m.sessionId === sessionId,
        `unexpected sessionId for \`${method}\`: ${JSON.stringify(m.sessionId)}`);
    return m;
}

function rejectionOf(promise) {
    return promise.then(
        () => null,
        (error) => error
    );
}

const PRESS = (x, y) => ({type: 'mousePressed', x, y, button: 'left', buttons: 1, clickCount: 1});
const RELEASE = (x, y) => ({type: 'mouseReleased', x, y, button: 'left', buttons: 0, clickCount: 1});
const MOVE = (x, y, buttons) => ({type: 'mouseMoved', x, y, buttons});

// the event may arrive before the capture move reply, duplicate and foreign
// session events must not overwrite the first in-window payload
async function captureEventBeforeReply() {
    const p = await openPeer();
    const c = collect(p);
    const pending = [];
    try {
        const data = {items: [{mimeType: 'text/plain', data: 'order-17'}], files: [], dragOperationsMask: 17};
        const expected = JSON.parse(JSON.stringify(data));
        const promise = p.client.dragAndDrop({
            source: {x: 10, y: 20},
            capture: {x: 40, y: 50},
            via: [{x: 60, y: 70}, {x: 60, y: 70}],
            target: {x: 90, y: 100}
        });
        pending.push(promise);
        let m = await expectCommand(c, 'Input.setInterceptDrags', {enabled: true}, undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', MOVE(10, 20, 0), undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', PRESS(10, 20), undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', MOVE(40, 50, 1), undefined);
        // the event arrives before the reply of the capture move
        p.event('Input.dragIntercepted', {data});
        // later duplicates and child session events must be ignored
        p.event('Input.dragIntercepted', {data: {items: [], files: [], dragOperationsMask: 0}});
        p.event('Input.dragIntercepted', {data: {forged: true}}, 'child-session');
        // mutating the payload after the fact must not affect the snapshot
        data.items.push({mimeType: 'text/plain', data: 'forged'});
        data.dragOperationsMask = 0;
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchDragEvent', {type: 'dragEnter', x: 60, y: 70, data: expected}, undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchDragEvent', {type: 'dragOver', x: 60, y: 70, data: expected}, undefined);
        p.reply(m);
        // consecutive identical points are not merged
        m = await expectCommand(c, 'Input.dispatchDragEvent', {type: 'dragOver', x: 60, y: 70, data: expected}, undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchDragEvent', {type: 'dragOver', x: 90, y: 100, data: expected}, undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchDragEvent', {type: 'drop', x: 90, y: 100, data: expected}, undefined);
        p.reply(m);
        // the mouse is released at the last confirmed position (capture)
        m = await expectCommand(c, 'Input.dispatchMouseEvent', RELEASE(40, 50), undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.setInterceptDrags', {enabled: false}, undefined);
        p.reply(m);
        const result = await promise;
        check(same(result, expected), 'the result must equal the captured DragData');
        check(result !== data && result.items !== data.items && result.items.length === 1,
            'the result must be an independent copy of the captured DragData');
    } finally {
        pending.forEach((promise) => promise.catch(() => {}));
        c.stop();
        await p.close();
    }
}

// the event may also arrive after the capture move reply; a child session
// call must not collect root session events
async function captureEventAfterReply() {
    const p = await openPeer();
    const c = collect(p);
    const pending = [];
    try {
        const data = {items: [{mimeType: 'text/plain', data: 'order-18'}], files: [], dragOperationsMask: 5};
        const promise = p.client.dragAndDrop({
            source: {x: 1, y: 2},
            capture: {x: 3, y: 4},
            target: {x: 5, y: 6},
            sessionId: 'session-1'
        });
        pending.push(promise);
        let m = await expectCommand(c, 'Input.setInterceptDrags', {enabled: true}, 'session-1');
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', MOVE(1, 2, 0), 'session-1');
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', PRESS(1, 2), 'session-1');
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', MOVE(3, 4, 1), 'session-1');
        // the reply arrives before the event
        p.reply(m);
        // a root session event must not satisfy a child session call
        p.event('Input.dragIntercepted', {data: {forged: true}});
        await sleep(30);
        check(c.size() === 0, 'a root session event must not satisfy a child session call');
        p.event('Input.dragIntercepted', {data}, 'session-1');
        // without `via` points the route is just the target
        m = await expectCommand(c, 'Input.dispatchDragEvent', {type: 'dragEnter', x: 5, y: 6, data}, 'session-1');
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchDragEvent', {type: 'dragOver', x: 5, y: 6, data}, 'session-1');
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchDragEvent', {type: 'drop', x: 5, y: 6, data}, 'session-1');
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', RELEASE(3, 4), 'session-1');
        p.reply(m);
        m = await expectCommand(c, 'Input.setInterceptDrags', {enabled: false}, 'session-1');
        p.reply(m);
        check(same(await promise, data), 'the result must equal the captured DragData');
    } finally {
        pending.forEach((promise) => promise.catch(() => {}));
        c.stop();
        await p.close();
    }
}

// the capture timeout waits for the in-flight capture move to settle, a late
// success must not trigger the route and the cleanup follows the confirmed
// state
async function captureTimeout() {
    const p = await openPeer();
    const c = collect(p);
    const pending = [];
    try {
        const promise = p.client.dragAndDrop({
            source: {x: 10, y: 20},
            capture: {x: 40, y: 50},
            target: {x: 90, y: 100},
            captureTimeoutMs: 60
        });
        pending.push(promise);
        let m = await expectCommand(c, 'Input.setInterceptDrags', {enabled: true}, undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', MOVE(10, 20, 0), undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', PRESS(10, 20), undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', MOVE(40, 50, 1), undefined);
        // the timeout expires while the capture move is still in flight: no
        // cleanup command may be sent before that command settles
        await sleep(150);
        check(c.size() === 0, 'the cleanup must wait for the in-flight capture move to settle');
        // the late success must not trigger the route
        p.reply(m);
        m = await expectCommand(c, 'Input.cancelDragging', {}, undefined);
        p.reply(m);
        // the late success confirmed the capture position
        m = await expectCommand(c, 'Input.dispatchMouseEvent', RELEASE(40, 50), undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.setInterceptDrags', {enabled: false}, undefined);
        p.reply(m);
        const error = await rejectionOf(promise);
        check(error instanceof Error, 'the timeout must reject with an Error');
        check(error && error.code === 'ERR_CDP_DRAG_CAPTURE_TIMEOUT',
            `the timeout error code must be ERR_CDP_DRAG_CAPTURE_TIMEOUT, got ${error && error.code}`);
        check(error.cleanupErrors === undefined, 'no cleanup errors are expected here');
    } finally {
        pending.forEach((promise) => promise.catch(() => {}));
        c.stop();
        await p.close();
    }
}

async function capture_route() {
    await captureEventBeforeReply();
    await captureEventAfterReply();
    await captureTimeout();
}

// failing to enable the interception produces no cleanup command at all
async function cleanupNothingConfirmed() {
    const p = await openPeer();
    const c = collect(p);
    const pending = [];
    try {
        const promise = p.client.dragAndDrop({
            source: {x: 1, y: 2},
            capture: {x: 3, y: 4},
            target: {x: 5, y: 6}
        });
        pending.push(promise);
        const m = await expectCommand(c, 'Input.setInterceptDrags', {enabled: true}, undefined);
        p.reject(m, 'intercept denied');
        const error = await rejectionOf(promise);
        check(error instanceof Error && /intercept denied/.test(error.message),
            'the business error must keep its identity');
        check(error.request && error.request.method === 'Input.setInterceptDrags',
            'the business error must be the original protocol error');
        check(error.cleanupErrors === undefined, 'no cleanup error is expected');
        await sleep(30);
        check(c.size() === 0, 'no cleanup command when nothing was confirmed');
    } finally {
        pending.forEach((promise) => promise.catch(() => {}));
        c.stop();
        await p.close();
    }
}

// a failing press only requires disabling the interception
async function cleanupInterceptionOnly() {
    const p = await openPeer();
    const c = collect(p);
    const pending = [];
    try {
        const promise = p.client.dragAndDrop({
            source: {x: 1, y: 2},
            capture: {x: 3, y: 4},
            target: {x: 5, y: 6}
        });
        pending.push(promise);
        let m = await expectCommand(c, 'Input.setInterceptDrags', {enabled: true}, undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', MOVE(1, 2, 0), undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', PRESS(1, 2), undefined);
        p.reject(m, 'press denied');
        m = await expectCommand(c, 'Input.setInterceptDrags', {enabled: false}, undefined);
        p.reply(m);
        const error = await rejectionOf(promise);
        check(error instanceof Error && /press denied/.test(error.message),
            'the business error must keep its identity');
        check(error.cleanupErrors === undefined, 'no cleanup error is expected');
        await sleep(30);
        check(c.size() === 0, 'no cancelDragging or mouseReleased before the press is confirmed');
    } finally {
        pending.forEach((promise) => promise.catch(() => {}));
        c.stop();
        await p.close();
    }
}

// a failing capture move requires the full compensation chain in order, and
// cleanup failures are collected without stopping the chain
async function cleanupFullChainWithFailures() {
    const p = await openPeer();
    const c = collect(p);
    const pending = [];
    try {
        const promise = p.client.dragAndDrop({
            source: {x: 10, y: 20},
            capture: {x: 40, y: 50},
            target: {x: 90, y: 100}
        });
        pending.push(promise);
        let m = await expectCommand(c, 'Input.setInterceptDrags', {enabled: true}, undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', MOVE(10, 20, 0), undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', PRESS(10, 20), undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', MOVE(40, 50, 1), undefined);
        p.reject(m, 'capture move denied');
        // compensation order: cancelDragging, mouseReleased, disable
        m = await expectCommand(c, 'Input.cancelDragging', {}, undefined);
        p.reject(m, 'cancel failed');
        // the capture move was not confirmed: release at the source position
        m = await expectCommand(c, 'Input.dispatchMouseEvent', RELEASE(10, 20), undefined);
        p.reject(m, 'release failed');
        m = await expectCommand(c, 'Input.setInterceptDrags', {enabled: false}, undefined);
        p.reply(m);
        const error = await rejectionOf(promise);
        check(error instanceof Error && /capture move denied/.test(error.message),
            'the business error must keep its identity');
        check(Array.isArray(error.cleanupErrors) && error.cleanupErrors.length === 2,
            'cleanup errors must be collected in `cleanupErrors`');
        check(error.cleanupErrors && /cancel failed/.test(error.cleanupErrors[0].message) &&
            /release failed/.test(error.cleanupErrors[1].message),
            'cleanup errors must be reported in order of occurrence');
    } finally {
        pending.forEach((promise) => promise.catch(() => {}));
        c.stop();
        await p.close();
    }
}

// cleanup failures after a successful route reject the promise with the
// first cleanup error and do not re-enter the failure branch
async function cleanupAfterSuccessfulRoute() {
    const p = await openPeer();
    const c = collect(p);
    const pending = [];
    try {
        const data = {items: [{mimeType: 'text/plain', data: 'order-19'}], files: [], dragOperationsMask: 1};
        const promise = p.client.dragAndDrop({
            source: {x: 10, y: 20},
            capture: {x: 40, y: 50},
            target: {x: 90, y: 100}
        });
        pending.push(promise);
        let m = await expectCommand(c, 'Input.setInterceptDrags', {enabled: true}, undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', MOVE(10, 20, 0), undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', PRESS(10, 20), undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', MOVE(40, 50, 1), undefined);
        p.event('Input.dragIntercepted', {data});
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchDragEvent', {type: 'dragEnter', x: 90, y: 100, data}, undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchDragEvent', {type: 'dragOver', x: 90, y: 100, data}, undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchDragEvent', {type: 'drop', x: 90, y: 100, data}, undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', RELEASE(40, 50), undefined);
        p.reject(m, 'release failed');
        // a failing cleanup item does not prevent the following ones
        m = await expectCommand(c, 'Input.setInterceptDrags', {enabled: false}, undefined);
        p.reject(m, 'disable failed');
        const error = await rejectionOf(promise);
        check(error instanceof Error && /release failed/.test(error.message),
            'the first cleanup error must reject the promise');
        check(Array.isArray(error.cleanupErrors) && error.cleanupErrors.length === 1 &&
            /disable failed/.test(error.cleanupErrors[0].message),
            'the remaining cleanup errors must be collected in `cleanupErrors`');
        await sleep(30);
        check(c.size() === 0, 'cleanup failures must not re-enter the failure branch');
    } finally {
        pending.forEach((promise) => promise.catch(() => {}));
        c.stop();
        await p.close();
    }
}

// closing the connection while waiting for the intercepted event ends the
// wait and no compensation command is sent
async function cleanupOnCloseWhileWaitingForEvent() {
    const p = await openPeer();
    const c = collect(p);
    const pending = [];
    try {
        const promise = p.client.dragAndDrop({
            source: {x: 10, y: 20},
            capture: {x: 40, y: 50},
            target: {x: 90, y: 100},
            captureTimeoutMs: 10000
        });
        pending.push(promise);
        let m = await expectCommand(c, 'Input.setInterceptDrags', {enabled: true}, undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', MOVE(10, 20, 0), undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', PRESS(10, 20), undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', MOVE(40, 50, 1), undefined);
        p.reply(m);
        await p.client.close();
        const error = await rejectionOf(promise);
        check(error instanceof Error, 'closing the connection must end the capture wait');
        check(p.client.listenerCount('Input.dragIntercepted') === 0,
            'the event listener must be removed');
        check(p.client.listenerCount('_connectionClosed') === 0,
            'the connection listener must be removed');
    } finally {
        pending.forEach((promise) => promise.catch(() => {}));
        c.stop();
        await p.close();
    }
}

// closing the connection with the capture move still in flight settles the
// call without sending compensation commands
async function cleanupOnCloseWithCommandInFlight() {
    const p = await openPeer();
    const c = collect(p);
    const pending = [];
    try {
        const promise = p.client.dragAndDrop({
            source: {x: 10, y: 20},
            capture: {x: 40, y: 50},
            target: {x: 90, y: 100}
        });
        pending.push(promise);
        let m = await expectCommand(c, 'Input.setInterceptDrags', {enabled: true}, undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', MOVE(10, 20, 0), undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', PRESS(10, 20), undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', MOVE(40, 50, 1), undefined);
        await p.client.close();
        const error = await rejectionOf(promise);
        check(error instanceof Error, 'closing the connection must settle the call');
        check(p.client.listenerCount('Input.dragIntercepted') === 0,
            'the event listener must be removed');
    } finally {
        pending.forEach((promise) => promise.catch(() => {}));
        c.stop();
        await p.close();
    }
}

async function cleanup_state() {
    await cleanupNothingConfirmed();
    await cleanupInterceptionOnly();
    await cleanupFullChainWithFailures();
    await cleanupAfterSuccessfulRoute();
    await cleanupOnCloseWhileWaitingForEvent();
    await cleanupOnCloseWithCommandInFlight();
}

// one call per client and session; different sessions run in parallel
async function exclusivityAndParallelSessions() {
    const p = await openPeer();
    const c = collect(p);
    const pending = [];
    try {
        check(typeof p.client.send === 'function', '`send` must be preserved');
        check(typeof p.client.Input.dispatchMouseEvent === 'function',
            'the generated API must be preserved');
        check(typeof p.client.dragAndDrop === 'function', '`dragAndDrop` must be available');
        const dataA = {items: [{mimeType: 'text/plain', data: 'root'}], files: [], dragOperationsMask: 1};
        const dataB = {items: [{mimeType: 'text/plain', data: 'child'}], files: [], dragOperationsMask: 2};
        const a = p.client.dragAndDrop({
            source: {x: 1, y: 2},
            capture: {x: 3, y: 4},
            target: {x: 5, y: 6}
        });
        pending.push(a);
        let m = await expectCommand(c, 'Input.setInterceptDrags', {enabled: true}, undefined);
        // a reentrant call on the same session is rejected before any packet
        const busy = await rejectionOf(p.client.dragAndDrop({
            source: {x: 1, y: 2},
            capture: {x: 3, y: 4},
            target: {x: 5, y: 6}
        }));
        check(busy instanceof Error && /already in progress/.test(busy.message),
            'a reentrant call on the same session must reject');
        check(c.size() === 0, 'a reentrant call must not send commands');
        // a call on a different session may proceed in parallel
        const b = p.client.dragAndDrop({
            source: {x: 11, y: 12},
            capture: {x: 13, y: 14},
            target: {x: 15, y: 16},
            sessionId: 'session-b'
        });
        pending.push(b);
        const mb = await expectCommand(c, 'Input.setInterceptDrags', {enabled: true}, 'session-b');
        // complete the root call first
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', MOVE(1, 2, 0), undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', PRESS(1, 2), undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', MOVE(3, 4, 1), undefined);
        p.event('Input.dragIntercepted', {data: dataA});
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchDragEvent', {type: 'dragEnter', x: 5, y: 6, data: dataA}, undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchDragEvent', {type: 'dragOver', x: 5, y: 6, data: dataA}, undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchDragEvent', {type: 'drop', x: 5, y: 6, data: dataA}, undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', RELEASE(3, 4), undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.setInterceptDrags', {enabled: false}, undefined);
        p.reply(m);
        check(same(await a, dataA), 'the root call must resolve with its own DragData');
        // then complete the child session call
        p.reply(mb);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', MOVE(11, 12, 0), 'session-b');
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', PRESS(11, 12), 'session-b');
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', MOVE(13, 14, 1), 'session-b');
        p.event('Input.dragIntercepted', {data: dataB}, 'session-b');
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchDragEvent', {type: 'dragEnter', x: 15, y: 16, data: dataB}, 'session-b');
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchDragEvent', {type: 'dragOver', x: 15, y: 16, data: dataB}, 'session-b');
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchDragEvent', {type: 'drop', x: 15, y: 16, data: dataB}, 'session-b');
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', RELEASE(13, 14), 'session-b');
        p.reply(m);
        m = await expectCommand(c, 'Input.setInterceptDrags', {enabled: false}, 'session-b');
        p.reply(m);
        check(same(await b, dataB), 'the child session call must resolve with its own DragData');
    } finally {
        pending.forEach((promise) => promise.catch(() => {}));
        c.stop();
        await p.close();
    }
}

// after a failed call the session is usable again, stale events do not leak
// into the next call and every call removes its own listeners
async function reuseAfterFailure() {
    const p = await openPeer();
    const c = collect(p);
    const pending = [];
    try {
        check(p.client.listenerCount('Input.dragIntercepted') === 0,
            'no event listener must be installed before the calls');
        const failing = p.client.dragAndDrop({
            source: {x: 1, y: 2},
            capture: {x: 3, y: 4},
            target: {x: 5, y: 6}
        });
        pending.push(failing);
        let m = await expectCommand(c, 'Input.setInterceptDrags', {enabled: true}, undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', MOVE(1, 2, 0), undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', PRESS(1, 2), undefined);
        p.reject(m, 'press denied');
        m = await expectCommand(c, 'Input.setInterceptDrags', {enabled: false}, undefined);
        p.reply(m);
        const error = await rejectionOf(failing);
        check(error instanceof Error && /press denied/.test(error.message),
            'the first call must fail with the business error');
        check(p.client.listenerCount('Input.dragIntercepted') === 0,
            'the failed call must remove its event listener');
        check(p.client.listenerCount('_connectionClosed') === 0,
            'the failed call must remove its connection listener');
        // stale events from the failed attempt must not pollute the next call
        p.event('Input.dragIntercepted', {data: {stale: true}});
        const data = {items: [{mimeType: 'text/plain', data: 'fresh'}], files: [], dragOperationsMask: 3};
        const ok = p.client.dragAndDrop({
            source: {x: 1, y: 2},
            capture: {x: 3, y: 4},
            target: {x: 5, y: 6}
        });
        pending.push(ok);
        m = await expectCommand(c, 'Input.setInterceptDrags', {enabled: true}, undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', MOVE(1, 2, 0), undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', PRESS(1, 2), undefined);
        // an event arriving before the capture window opens must be ignored
        p.event('Input.dragIntercepted', {data: {stale: true}});
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', MOVE(3, 4, 1), undefined);
        p.event('Input.dragIntercepted', {data});
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchDragEvent', {type: 'dragEnter', x: 5, y: 6, data}, undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchDragEvent', {type: 'dragOver', x: 5, y: 6, data}, undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchDragEvent', {type: 'drop', x: 5, y: 6, data}, undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.dispatchMouseEvent', RELEASE(3, 4), undefined);
        p.reply(m);
        m = await expectCommand(c, 'Input.setInterceptDrags', {enabled: false}, undefined);
        p.reply(m);
        check(same(await ok, data), 'the second call must capture the fresh DragData');
        check(p.client.listenerCount('Input.dragIntercepted') === 0,
            'the successful call must remove its event listener');
        check(p.client.listenerCount('_connectionClosed') === 0,
            'the successful call must remove its connection listener');
    } finally {
        pending.forEach((promise) => promise.catch(() => {}));
        c.stop();
        await p.close();
    }
}

// invalid options reject before any command is sent
async function validationPreflight() {
    const p = await openPeer();
    const c = collect(p);
    try {
        const point = {x: 1, y: 2};
        const invalidOptions = [
            undefined,
            null,
            {},
            {source: point},
            {source: point, capture: {x: 3, y: 4}},
            {source: {x: NaN, y: 2}, capture: {x: 3, y: 4}, target: {x: 5, y: 6}},
            {source: {x: Infinity, y: 2}, capture: {x: 3, y: 4}, target: {x: 5, y: 6}},
            {source: {x: 1}, capture: {x: 3, y: 4}, target: {x: 5, y: 6}},
            {source: point, capture: point, target: {x: 5, y: 6}},
            {source: point, capture: {x: 3, y: 4}, target: {x: 5, y: 6}, via: {}},
            {source: point, capture: {x: 3, y: 4}, target: {x: 5, y: 6}, via: [{x: 1}]},
            {source: point, capture: {x: 3, y: 4}, target: {x: 5, y: 6}, captureTimeoutMs: 0},
            {source: point, capture: {x: 3, y: 4}, target: {x: 5, y: 6}, captureTimeoutMs: -1},
            {source: point, capture: {x: 3, y: 4}, target: {x: 5, y: 6}, captureTimeoutMs: 1.5},
            {source: point, capture: {x: 3, y: 4}, target: {x: 5, y: 6}, captureTimeoutMs: '100'},
            {source: point, capture: {x: 3, y: 4}, target: {x: 5, y: 6}, sessionId: ''},
            {source: point, capture: {x: 3, y: 4}, target: {x: 5, y: 6}, sessionId: 42}
        ];
        for (const options of invalidOptions) {
            const error = await rejectionOf(p.client.dragAndDrop(options));
            check(error instanceof TypeError,
                `invalid options must reject with a TypeError: ${JSON.stringify(options)}`);
        }
        await sleep(30);
        check(c.size() === 0, 'preflight failures must not send commands');
    } finally {
        c.stop();
        await p.close();
    }
}

async function isolation_reuse() {
    await exclusivityAndParallelSessions();
    await reuseAfterFailure();
    await validationPreflight();
}

module.exports = {capture_route, cleanup_state, isolation_reuse};
