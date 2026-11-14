'use strict';

const WebSocket = require('ws');

const DEFAULT_CAPTURE_TIMEOUT_MS = 1000;

function isFiniteNumber(value) {
    return typeof value === 'number' && Number.isFinite(value);
}

function readPoint(point, name) {
    const valid = point && typeof point === 'object' &&
        isFiniteNumber(point.x) && isFiniteNumber(point.y);
    if (!valid) {
        throw new TypeError(`\`options.${name}\` must be a point with finite \`x\` and \`y\` coordinates`);
    }
    return {x: point.x, y: point.y};
}

// take an independent snapshot of a protocol payload
function snapshot(value) {
    return value === undefined ? value : JSON.parse(JSON.stringify(value));
}

// validate all the options upfront, before any command is sent
function validateOptions(options) {
    if (!options || typeof options !== 'object' || Array.isArray(options)) {
        throw new TypeError('`options` must be an object');
    }
    const source = readPoint(options.source, 'source');
    const capture = readPoint(options.capture, 'capture');
    const target = readPoint(options.target, 'target');
    if (source.x === capture.x && source.y === capture.y) {
        throw new TypeError('`options.capture` must differ from `options.source`');
    }
    let via = [];
    if (options.via !== undefined) {
        if (!Array.isArray(options.via)) {
            throw new TypeError('`options.via` must be an array of points');
        }
        via = options.via.map((point, index) => readPoint(point, `via[${index}]`));
    }
    let captureTimeoutMs = DEFAULT_CAPTURE_TIMEOUT_MS;
    if (options.captureTimeoutMs !== undefined) {
        if (!Number.isInteger(options.captureTimeoutMs) || options.captureTimeoutMs <= 0) {
            throw new TypeError('`options.captureTimeoutMs` must be a positive integer');
        }
        captureTimeoutMs = options.captureTimeoutMs;
    }
    let sessionId;
    if (options.sessionId !== undefined) {
        if (typeof options.sessionId !== 'string' || options.sessionId.length === 0) {
            throw new TypeError('`options.sessionId` must be a non-empty string');
        }
        sessionId = options.sessionId;
    }
    return {source, capture, target, via, captureTimeoutMs, sessionId};
}

async function performDragAndDrop(chrome, config) {
    const {source, capture, target, via, captureTimeoutMs, sessionId} = config;
    // confirmation state owned by this very call
    const state = {
        interceptEnabled: false, // `Input.setInterceptDrags({enabled: true})` succeeded
        pressed: false,          // `mousePressed` succeeded
        captureSent: false,      // the capture `mouseMoved` send has been initiated
        lastPosition: null,      // last successfully confirmed mouse move
        closed: false            // the underlying connection is gone
    };
    // per-call event reception; signals wake up the capture wait loop below
    const signals = new Set();
    const signal = () => {
        signals.forEach((resolve) => resolve());
        signals.clear();
    };
    const waitSignal = () => new Promise((resolve) => signals.add(resolve));
    let captureWindowOpen = false;
    let captureReceived = false;
    let capturedData;
    const onDragIntercepted = (params, eventSessionId) => {
        // only the top-level sessionId of this exact scope is acceptable, in
        // particular the root session must not collect child session events
        if (eventSessionId !== sessionId) {
            return;
        }
        // ignore events outside the capture window and only keep the first one
        if (!captureWindowOpen || captureReceived) {
            return;
        }
        captureReceived = true;
        // store an independent snapshot of the browser-provided DragData
        capturedData = snapshot(params && params.data);
        signal();
    };
    const onConnectionClosed = () => {
        state.closed = true;
        signal();
    };
    chrome.on('Input.dragIntercepted', onDragIntercepted);
    chrome.once('_connectionClosed', onConnectionClosed);
    let captureTimer = null;
    const cleanupErrors = [];
    let businessError;
    const send = (method, params) => chrome.send(method, params, sessionId);
    try {
        await send('Input.setInterceptDrags', {enabled: true});
        state.interceptEnabled = true;
        await send('Input.dispatchMouseEvent', {
            type: 'mouseMoved', x: source.x, y: source.y, buttons: 0
        });
        state.lastPosition = source;
        await send('Input.dispatchMouseEvent', {
            type: 'mousePressed', x: source.x, y: source.y,
            button: 'left', buttons: 1, clickCount: 1
        });
        state.pressed = true;
        // open the capture window right before issuing the capture move and
        // start the capture timeout as soon as the move is sent
        captureWindowOpen = true;
        state.captureSent = true;
        const captureMove = send('Input.dispatchMouseEvent', {
            type: 'mouseMoved', x: capture.x, y: capture.y, buttons: 1
        });
        let moveOutcome; // undefined | true | Error
        const captureMoveSettled = captureMove.then(() => {
            moveOutcome = true;
            state.lastPosition = capture;
            signal();
        }, (error) => {
            moveOutcome = error;
            signal();
        });
        let timedOut = false;
        captureTimer = setTimeout(() => {
            timedOut = true;
            signal();
        }, captureTimeoutMs);
        // both the successful reply and the intercepted data are required, in
        // any order, before the capture timeout expires
        while (true) {
            if (state.closed) {
                throw new Error('WebSocket connection closed');
            }
            if (timedOut) {
                // let the in-flight capture move settle before cleaning up
                // according to the confirmed state; a late success must not
                // trigger the route
                await captureMoveSettled;
                const error = new Error(`drag data not captured within ${captureTimeoutMs} ms`);
                error.code = 'ERR_CDP_DRAG_CAPTURE_TIMEOUT';
                throw error;
            }
            if (moveOutcome === true && captureReceived) {
                break;
            }
            if (moveOutcome instanceof Error) {
                throw moveOutcome;
            }
            await waitSignal();
        }
        // walk the route: dragEnter on the first point, dragOver on every
        // point (consecutive duplicates are not merged) and drop on the
        // target; each command gets an independent copy of the captured data
        const route = via.concat([target]);
        for (let index = 0; index < route.length; index++) {
            const point = route[index];
            if (index === 0) {
                await send('Input.dispatchDragEvent', {
                    type: 'dragEnter', x: point.x, y: point.y, data: snapshot(capturedData)
                });
            }
            await send('Input.dispatchDragEvent', {
                type: 'dragOver', x: point.x, y: point.y, data: snapshot(capturedData)
            });
        }
        await send('Input.dispatchDragEvent', {
            type: 'drop', x: target.x, y: target.y, data: snapshot(capturedData)
        });
    } catch (error) {
        businessError = error;
    }
    // cleanup: never send compensation commands that cannot be executed
    const canSend = () => {
        return !state.closed && chrome._ws && chrome._ws.readyState === WebSocket.OPEN;
    };
    const attempt = async (method, params) => {
        if (!canSend()) {
            return;
        }
        try {
            await chrome.send(method, params, sessionId);
        } catch (error) {
            // a failing cleanup item does not prevent the following ones
            cleanupErrors.push(error);
        }
    };
    const releaseMouse = () => attempt('Input.dispatchMouseEvent', {
        type: 'mouseReleased',
        x: state.lastPosition.x, y: state.lastPosition.y,
        button: 'left', buttons: 0, clickCount: 1
    });
    const disableInterception = () => attempt('Input.setInterceptDrags', {enabled: false});
    if (businessError) {
        // failure path: no more route actions, compensate in order, each item
        // is attempted at most once and only if its state was confirmed
        if (state.captureSent) {
            await attempt('Input.cancelDragging', {});
        }
        if (state.pressed) {
            await releaseMouse();
        }
        if (state.interceptEnabled) {
            await disableInterception();
        }
    } else {
        // normal path: release the mouse at the last confirmed position then
        // disable the interception; failures here do not re-enter the
        // failure branch above
        await releaseMouse();
        await disableInterception();
    }
    // teardown: remove the timer and the listeners installed by this call
    if (captureTimer !== null) {
        clearTimeout(captureTimer);
    }
    chrome.removeListener('Input.dragIntercepted', onDragIntercepted);
    chrome.removeListener('_connectionClosed', onConnectionClosed);
    // settle
    if (businessError) {
        // keep the identity of the first business error and attach the
        // cleanup errors in order of occurrence
        if (cleanupErrors.length) {
            businessError.cleanupErrors = cleanupErrors;
        }
        throw businessError;
    }
    if (cleanupErrors.length) {
        const error = cleanupErrors[0];
        if (cleanupErrors.length > 1) {
            error.cleanupErrors = cleanupErrors.slice(1);
        }
        throw error;
    }
    // resolve with an independent copy of the captured DragData
    return snapshot(capturedData);
}

async function dragAndDrop(chrome, options) {
    // full preflight: invalid input rejects before any command is sent
    const config = validateOptions(options);
    // only one call at a time per client and session, reentrant calls are
    // rejected before any command is sent; different sessions may run in
    // parallel
    const lockKey = config.sessionId === undefined ? null : config.sessionId;
    if (chrome._dragAndDropSessions.has(lockKey)) {
        throw new Error('another `dragAndDrop` call is already in progress for this session');
    }
    chrome._dragAndDropSessions.add(lockKey);
    try {
        // the exclusivity is held until the cleanup is complete
        return await performDragAndDrop(chrome, config);
    } finally {
        chrome._dragAndDropSessions.delete(lockKey);
    }
}

module.exports = dragAndDrop;
