'use strict';

const assert = require('node:assert');
const {EventEmitter, once} = require('node:events');
const Module = require('node:module');
const {test} = require('node:test');
const WebSocket = require('ws');

// minimal protocol, enough to expose the generated command and event helpers
const PROTOCOL = {
    domains: [
        {
            domain: 'Runtime',
            commands: [{name: 'evaluate'}],
            events: [{name: 'consoleAPICalled'}]
        },
        {
            domain: 'Target',
            commands: [{name: 'attachToTarget'}, {name: 'detachFromTarget'}],
            events: [{name: 'detachedFromTarget'}]
        }
    ]
};

// ---------------------------------------------------------------------------
// WebSocket transport
//
// Tests normally run against a real local ws server. In sandboxes where
// binding a local port is forbidden (EPERM), an in-process WebSocket
// implementation is transparently substituted through a Module._load hook
// installed before the library is loaded. It mirrors the small subset of the
// ws API the client uses (open/message/close/error events, readyState,
// send with callback, close) and keeps frame ordering with nextTick queues.
// ---------------------------------------------------------------------------

const inProcessEndpoints = new Map();
let transportHooked = false;

class FakeEndpoint extends EventEmitter {
    constructor(peer) {
        super();
        this.readyState = WebSocket.OPEN;
        this._peer = peer;
    }

    send(data, callback) {
        // mirror ws: writing after close fails the send callback instead of
        // delivering the frame
        if (this.readyState !== WebSocket.OPEN) {
            if (typeof callback === 'function') {
                callback(new Error(`WebSocket is not open: readyState ${this.readyState}`));
            }
            return;
        }
        const peer = this._peer;
        process.nextTick(() => {
            if (peer.readyState === WebSocket.OPEN) {
                peer.emit('message', data);
            }
            if (typeof callback === 'function') {
                callback();
            }
        });
    }

    close() {
        if (this.readyState === WebSocket.CLOSED) {
            return;
        }
        const peer = this._peer;
        this.readyState = WebSocket.CLOSED;
        peer.readyState = WebSocket.CLOSED;
        process.nextTick(() => {
            this.emit('close', 1006);
            peer.emit('close', 1006);
        });
    }

    terminate() {
        this.close();
    }
}

function createEndpointPair() {
    const server = new FakeEndpoint(null);
    const client = new FakeEndpoint(server);
    server._peer = client;
    return {server, client};
}

function installTransportHook() {
    if (transportHooked) {
        return;
    }
    transportHooked = true;
    const originalLoad = Module._load;
    Module._load = function (request, parent, isMain) {
        const realModule = originalLoad.call(this, request, parent, isMain);
        if (request !== 'ws') {
            return realModule;
        }
        function InProcessWebSocket(url, protocols, options) {
            const entry = inProcessEndpoints.get(url);
            if (!entry) {
                return new realModule(url, protocols, options);
            }
            const pair = createEndpointPair();
            process.nextTick(() => {
                pair.client.emit('open');
                entry.handleConnection(pair.server);
            });
            return pair.client;
        }
        Object.assign(InProcessWebSocket, realModule);
        InProcessWebSocket.Server = realModule.Server;
        return InProcessWebSocket;
    };
}

function deferred() {
    let resolve;
    let reject;
    const promise = new Promise((fulfill, fail) => {
        resolve = fulfill;
        reject = fail;
    });
    return {promise, resolve, reject};
}

// local WebSocket endpoint acting as the CDP server
class FakeServer {
    constructor() {
        this.sockets = [];
        this.messages = [];
        this.waiters = [];
        this.connected = new Promise((resolve) => {
            this.resolveConnected = resolve;
        });
    }

    static async start() {
        // prefer a real local WebSocket server
        try {
            const server = new WebSocket.Server({host: '127.0.0.1', port: 0});
            await once(server, 'listening');
            const fake = new FakeServer();
            fake.server = server;
            fake.url = `ws://127.0.0.1:${server.address().port}`;
            server.on('connection', (socket) => fake._handleConnection(socket));
            return fake;
        } catch (err) {
            if (!err || err.code !== 'EPERM') {
                throw err;
            }
            // local port binding is forbidden, use the in-process transport
            installTransportHook();
            const fake = new FakeServer();
            fake.url = `ws://inprocess/fake-${FakeServer._counter++}`;
            inProcessEndpoints.set(fake.url, {
                handleConnection: (socket) => fake._handleConnection(socket)
            });
            return fake;
        }
    }

    _handleConnection(socket) {
        this.sockets.push(socket);
        this.socket = socket;
        socket.on('error', () => {});
        socket.on('message', (data) => {
            const message = JSON.parse(data.toString());
            this.messages.push(message);
            for (const waiter of this.waiters.slice()) {
                if (waiter.predicate(message)) {
                    this.waiters.splice(this.waiters.indexOf(waiter), 1);
                    waiter.resolve(message);
                }
            }
        });
        this.resolveConnected(socket);
    }

    waitForMessage(predicate) {
        const found = this.messages.find(predicate);
        if (found) {
            return Promise.resolve(found);
        }
        return new Promise((resolve) => {
            this.waiters.push({predicate, resolve});
        });
    }

    send(message) {
        this.socket.send(JSON.stringify(message));
        return message;
    }

    respond(message, result) {
        return this.send({id: message.id, result: result || {}});
    }

    respondError(message, error) {
        return this.send({id: message.id, error});
    }

    async close() {
        if (this.server) {
            for (const socket of this.sockets) {
                socket.removeAllListeners();
                socket.on('error', () => {});
                socket.terminate();
            }
            await new Promise((resolve) => this.server.close(resolve));
        } else {
            inProcessEndpoints.delete(this.url);
            for (const socket of this.sockets) {
                socket.removeAllListeners();
                socket.on('error', () => {});
                socket.terminate();
            }
        }
    }
}
FakeServer._counter = 0;

// load the library lazily: FakeServer.start() decides whether the in-process
// transport hook must be installed before chrome.js captures the 'ws'
// constructor at require time
let Chrome;
function loadChrome() {
    if (!Chrome) {
        Chrome = require('./index.js');
    }
    return Chrome;
}

async function connect(fake) {
    const CDP = loadChrome();
    const chrome = await CDP({target: fake.url, protocol: PROTOCOL});
    await fake.connected;
    return chrome;
}

// resolve once the client has processed the given emitted event
function waitForClientEvent(chrome, method, predicate) {
    return new Promise((resolve) => {
        const handler = (params, sessionId) => {
            if (predicate && !predicate(params, sessionId)) {
                return;
            }
            chrome.removeListener(method, handler);
            resolve({params, sessionId});
        };
        chrome.on(method, handler);
    });
}

// a server->client event used as a barrier (events are ordered with the
// messages sent before them on the same WebSocket)
async function serverBarrier(fake, chrome, marker) {
    const done = waitForClientEvent(chrome, 'Runtime.consoleAPICalled', (params) => {
        return params.marker === marker;
    });
    fake.send({method: 'Runtime.consoleAPICalled', params: {marker}});
    await done;
}

// wait for a command frame to reach the server; this also acts as a
// client->server barrier (WebSocket frames preserve ordering)
async function waitForCommand(fake, expression) {
    return fake.waitForMessage((message) => {
        return message.params && message.params.expression === expression;
    });
}

// wrap a promise so rejections can be asserted without unhandled rejection
function outcome(promise) {
    return promise.then((value) => ({value}), (error) => ({error}));
}

// start a fake server and client, registering a deterministic teardown
async function setup(t) {
    const fake = await FakeServer.start();
    const chrome = await connect(fake).catch(async (err) => {
        await fake.close();
        throw err;
    });
    t.after(async () => {
        await chrome.close().catch(() => {});
        await fake.close();
    });
    return {fake, chrome};
}

test('a root command without sessionId succeeds', {timeout: 5000}, async (t) => {
    const {fake, chrome} = await setup(t);

    const messagePromise = waitForCommand(fake, 'baseline');
    const resultPromise = chrome.send('Runtime.evaluate', {
        expression: 'baseline'
    });
    const message = await messagePromise;
    assert.strictEqual(message.sessionId, undefined);
    fake.respond(message, {value: 1});
    const result = await resultPromise;
    assert.deepStrictEqual(result, {value: 1});
    assert.deepStrictEqual(chrome._callbacks, {});
});

test('protocol errors keep their original callback and promise shapes',
    {timeout: 5000}, async (t) => {
        const {fake, chrome} = await setup(t);

        // callback API passes the protocol error in the response position
        const callbackDone = deferred();
        const callbackMessagePromise = waitForCommand(fake, 'callback-error');
        chrome.send('Runtime.evaluate', {expression: 'callback-error'}, (error, response) => {
            assert.strictEqual(error, true);
            assert.strictEqual(response.code, -32000);
            assert.strictEqual(response.message, 'boom');
            callbackDone.resolve();
        });
        const callbackMessage = await callbackMessagePromise;
        fake.respondError(callbackMessage, {code: -32000, message: 'boom'});
        await callbackDone.promise;

        // promise API wraps it in ProtocolError
        const promiseMessagePromise = waitForCommand(fake, 'promise-error');
        const result = outcome(chrome.send('Runtime.evaluate', {
            expression: 'promise-error'
        }));
        const promiseMessage = await promiseMessagePromise;
        fake.respondError(promiseMessage, {code: -32000, message: 'boom'});
        const {error} = await result;
        assert(error instanceof Chrome.ProtocolError);
        assert.strictEqual(error.response.code, -32000);
        assert.strictEqual(error.request.method, 'Runtime.evaluate');
        assert.deepStrictEqual(chrome._callbacks, {});
    });

test('Target.detachedFromTarget settles the detached session only',
    {timeout: 10000}, async (t) => {
        const {fake, chrome} = await setup(t);

        const readyEvents = [];
        chrome.on('ready', () => readyEvents.push(true));

        // all three event styles must receive the notification
        const notifications = {raw: [], named: [], session: []};
        chrome.on('event', (message) => notifications.raw.push(message));
        chrome.on('Target.detachedFromTarget', (params, sessionId) => {
            notifications.named.push({params, sessionId});
        });
        let eventCalls = 0;
        const eventDone = deferred();
        const eventSendErrors = [];
        chrome.Target.detachedFromTarget('parent', (params, sessionId) => {
            notifications.session.push({params, sessionId});
            eventCalls++;
            // the session is already invalid while the event is delivered, so
            // both public entry points must fail locally
            chrome.send('Runtime.evaluate', {expression: 'child-in-event-send'},
                'child', (error) => eventSendErrors.push(error));
            chrome.Runtime.evaluate({expression: 'child-in-event-shorthand'},
                'child', (error) => eventSendErrors.push(error));
            eventDone.resolve();
        });

        let childCallbackCalls = 0;
        const childCallbackDone = deferred();
        let rootFromCallback;
        let siblingFromCallback;
        chrome.send('Runtime.evaluate', {expression: 'child-callback'},
            'child', (error, response) => {
                childCallbackCalls++;
                assert(error instanceof Error);
                assert.strictEqual(error.code, 'ERR_CDP_SESSION_DETACHED');
                assert.strictEqual(error.sessionId, 'child');
                assert.strictEqual(response, undefined);
                // send unrelated commands from within the settlement callback;
                // the batch cleanup must not remove these
                rootFromCallback = outcome(chrome.send('Runtime.evaluate', {
                    expression: 'root-from-callback'
                }));
                siblingFromCallback = outcome(chrome.send('Runtime.evaluate', {
                    expression: 'sibling-from-callback'
                }, 'sibling'));
                childCallbackDone.resolve();
            });

        const childPromise = outcome(chrome.Runtime.evaluate({
            expression: 'child-promise'
        }, 'child'));
        const siblingPromise = outcome(chrome.send('Runtime.evaluate', {
            expression: 'sibling'
        }, 'sibling'));
        let parentCallbackCalls = 0;
        const parentDone = deferred();
        chrome.send('Runtime.evaluate', {expression: 'parent'}, 'parent', (error, response) => {
            parentCallbackCalls++;
            assert.strictEqual(error, false);
            assert.deepStrictEqual(response, {value: 'parent'});
            parentDone.resolve();
        });
        const rootPromise = outcome(chrome.send('Runtime.evaluate', {
            expression: 'root'
        }));

        const expressions = [
            'child-callback', 'child-promise', 'sibling', 'parent', 'root'
        ];
        const messages = await Promise.all(expressions.map((expression) => {
            return waitForCommand(fake, expression);
        }));
        const byExpression = {};
        for (const message of messages) {
            byExpression[message.params.expression] = message;
        }
        // commands are tracked from the moment they are accepted, even before
        // the WebSocket send callback runs
        for (const message of messages) {
            assert.strictEqual(typeof chrome._callbacks[message.id], 'function');
        }

        fake.send({
            method: 'Target.detachedFromTarget',
            sessionId: 'parent',
            params: {sessionId: 'child'}
        });

        await eventDone.promise;
        await childCallbackDone.promise;

        const childResult = await childPromise;
        assert(childResult.error instanceof Error);
        assert.strictEqual(childResult.error.code, 'ERR_CDP_SESSION_DETACHED');
        assert.strictEqual(childResult.error.sessionId, 'child');
        assert.strictEqual(childResult.error.request, undefined);
        assert.strictEqual(childResult.error.response, undefined);
        assert.strictEqual(childCallbackCalls, 1);

        // params.sessionId is the detached session
        assert.strictEqual(notifications.raw.length, 1);
        assert.strictEqual(notifications.named.length, 1);
        assert.strictEqual(notifications.session.length, 1);
        assert.strictEqual(notifications.raw[0].method, 'Target.detachedFromTarget');
        assert.strictEqual(notifications.raw[0].sessionId, 'parent');
        assert.deepStrictEqual(notifications.raw[0].params, {sessionId: 'child'});
        assert.deepStrictEqual(notifications.named[0].params, {sessionId: 'child'});
        assert.strictEqual(notifications.named[0].sessionId, 'parent');
        assert.deepStrictEqual(notifications.session[0].params, {sessionId: 'child'});
        assert.strictEqual(notifications.session[0].sessionId, 'parent');

        assert.strictEqual(eventSendErrors.length, 2);
        for (const error of eventSendErrors) {
            assert(error instanceof Error);
            assert.strictEqual(error.code, 'ERR_CDP_SESSION_DETACHED');
            assert.strictEqual(error.sessionId, 'child');
        }
        assert.strictEqual(chrome._invalidSessions.has('child'), true);
        assert.strictEqual(chrome._sessionCommands.has('child'), false);

        // the commands enqueued inside the callback are still pending
        const rootFromCallbackMessage = await waitForCommand(fake, 'root-from-callback');
        const siblingFromCallbackMessage = await waitForCommand(fake, 'sibling-from-callback');
        assert.strictEqual(typeof chrome._callbacks[rootFromCallbackMessage.id], 'function');
        assert.strictEqual(typeof chrome._callbacks[siblingFromCallbackMessage.id], 'function');

        // client->server barrier: anything written to the invalidated session
        // would have reached the server before this root command
        const barrierResult = outcome(chrome.send('Runtime.evaluate', {
            expression: 'after-detach-barrier'
        }));
        const barrierMessage = await waitForCommand(fake, 'after-detach-barrier');
        assert.strictEqual(fake.messages.filter((message) => {
            return message.params && (
                message.params.expression === 'child-in-event-send' ||
                message.params.expression === 'child-in-event-shorthand'
            );
        }).length, 0);
        fake.respond(barrierMessage, {value: 'barrier'});
        const barrier = await barrierResult;
        assert.ifError(barrier.error);
        assert.deepStrictEqual(barrier.value, {value: 'barrier'});

        // a repeated detach must not settle anything again
        fake.send({
            method: 'Target.detachedFromTarget',
            sessionId: 'parent',
            params: {sessionId: 'child'}
        });
        await serverBarrier(fake, chrome, 'duplicate-detach');
        assert.strictEqual(childCallbackCalls, 1);
        assert.strictEqual((await childPromise).error.code,
            'ERR_CDP_SESSION_DETACHED');

        // unrelated sessions and the root still work
        fake.respond(byExpression.sibling, {value: 'sibling'});
        fake.respond(byExpression.root, {value: 'root'});
        fake.respond(siblingFromCallbackMessage, {value: 'sibling-cb'});
        fake.respond(rootFromCallbackMessage, {value: 'root-cb'});

        const siblingResult = await siblingPromise;
        assert.ifError(siblingResult.error);
        assert.deepStrictEqual(siblingResult.value, {value: 'sibling'});
        const rootResult = await rootPromise;
        assert.ifError(rootResult.error);
        assert.deepStrictEqual(rootResult.value, {value: 'root'});
        const siblingFromCallbackResult = await siblingFromCallback;
        assert.ifError(siblingFromCallbackResult.error);
        assert.deepStrictEqual(siblingFromCallbackResult.value, {value: 'sibling-cb'});
        const rootFromCallbackResult = await rootFromCallback;
        assert.ifError(rootFromCallbackResult.error);
        assert.deepStrictEqual(rootFromCallbackResult.value, {value: 'root-cb'});

        // a root command still works after the duplicate detach
        const afterDuplicateResult = outcome(chrome.send('Runtime.evaluate', {
            expression: 'after-duplicate-detach'
        }));
        const afterDuplicateMessage = await waitForCommand(fake, 'after-duplicate-detach');
        fake.respond(afterDuplicateMessage, {value: 'after-duplicate'});
        const afterDuplicate = await afterDuplicateResult;
        assert.ifError(afterDuplicate.error);
        assert.deepStrictEqual(afterDuplicate.value, {value: 'after-duplicate'});

        // late replies for the detached session are ignored
        fake.send({
            id: byExpression['child-promise'].id,
            result: {value: 'late-child-success'}
        });
        fake.send({
            id: byExpression['child-callback'].id,
            error: {code: -32000, message: 'late-child-error'}
        });
        await serverBarrier(fake, chrome, 'late-child-replies');
        assert.strictEqual(childCallbackCalls, 1);
        assert.strictEqual((await childPromise).error.code,
            'ERR_CDP_SESSION_DETACHED');

        // parent was kept pending until last so this is a single drain
        fake.respond(byExpression.parent, {value: 'parent'});
        await parentDone.promise;
        assert.strictEqual(parentCallbackCalls, 1);
        assert.deepStrictEqual(chrome._callbacks, {});
        assert.strictEqual(chrome._commandSessions.size, 0);
        assert.strictEqual(chrome._sessionCommands.size, 0);
        assert.strictEqual(readyEvents.length, 1);
        assert.strictEqual(eventCalls, 2); // notifications are not suppressed
    });

test('ready is emitted once when a detach drains all pending commands',
    {timeout: 5000}, async (t) => {
        const {fake, chrome} = await setup(t);

        const readyEvents = [];
        chrome.on('ready', () => readyEvents.push(true));

        let childCallbackCalls = 0;
        const childCallbackDone = deferred();
        chrome.send('Runtime.evaluate', {expression: 'ready-child-callback'},
            'child', (error) => {
                childCallbackCalls++;
                assert.strictEqual(error.code, 'ERR_CDP_SESSION_DETACHED');
                childCallbackDone.resolve();
            });
        const childPromise = outcome(chrome.Runtime.evaluate({
            expression: 'ready-child-promise'
        }, 'child'));

        const messages = await Promise.all([
            waitForCommand(fake, 'ready-child-callback'),
            waitForCommand(fake, 'ready-child-promise')
        ]);
        assert.strictEqual(typeof chrome._callbacks[messages[0].id], 'function');
        assert.strictEqual(typeof chrome._callbacks[messages[1].id], 'function');

        fake.send({
            method: 'Target.detachedFromTarget',
            sessionId: 'parent',
            params: {sessionId: 'child'}
        });
        await serverBarrier(fake, chrome, 'ready-drained');

        await childCallbackDone.promise;
        const childResult = await childPromise;
        assert.strictEqual(childCallbackCalls, 1);
        assert.strictEqual(childResult.error.code, 'ERR_CDP_SESSION_DETACHED');
        assert.deepStrictEqual(chrome._callbacks, {});
        assert.strictEqual(readyEvents.length, 1);

        // duplicate detach plus late replies must not settle or emit again
        fake.send({
            method: 'Target.detachedFromTarget',
            sessionId: 'parent',
            params: {sessionId: 'child'}
        });
        fake.send({
            id: messages[1].id,
            result: {value: 'late-success'}
        });
        fake.send({
            id: messages[0].id,
            error: {code: -32000, message: 'late-error'}
        });
        await serverBarrier(fake, chrome, 'ready-duplicate');
        assert.strictEqual(childCallbackCalls, 1);
        assert.strictEqual(childResult.error.code, 'ERR_CDP_SESSION_DETACHED');
        assert.strictEqual(readyEvents.length, 1);

        // the invalidated session is rejected locally without writing a frame
        const localErrors = [];
        chrome.send('Runtime.evaluate', {expression: 'ready-late-send'},
            'child', (error) => localErrors.push(error));
        chrome.Runtime.evaluate({expression: 'ready-late-shorthand'},
            'child', (error) => localErrors.push(error));
        assert.strictEqual(localErrors.length, 2);
        for (const error of localErrors) {
            assert.strictEqual(error.code, 'ERR_CDP_SESSION_DETACHED');
            assert.strictEqual(error.sessionId, 'child');
        }
        const barrierResult = outcome(chrome.send('Runtime.evaluate', {
            expression: 'ready-no-write-barrier'
        }));
        const barrierMessage = await waitForCommand(fake, 'ready-no-write-barrier');
        assert.strictEqual(fake.messages.filter((message) => {
            return message.params && (
                message.params.expression === 'ready-late-send' ||
                message.params.expression === 'ready-late-shorthand'
            );
        }).length, 0);
        fake.respond(barrierMessage, {});
        await barrierResult;
    });

test('a late ws.send callback cannot resurrect a detached command',
    {timeout: 5000}, async (t) => {
        const {fake, chrome} = await setup(t);

        // send the frame but keep the ws.send callback pending
        const originalSend = chrome._ws.send.bind(chrome._ws);
        const sendCallbacks = [];
        chrome._ws.send = (data, callback) => {
            sendCallbacks.push(callback);
            originalSend(data, () => {});
        };

        let childCallbackCalls = 0;
        const childCallbackDone = deferred();
        chrome.send('Runtime.evaluate', {expression: 'late-send-child-callback'},
            'child', (error) => {
                childCallbackCalls++;
                assert.strictEqual(error.code, 'ERR_CDP_SESSION_DETACHED');
                assert.strictEqual(error.sessionId, 'child');
                childCallbackDone.resolve();
            });
        const childPromise = outcome(chrome.Runtime.evaluate({
            expression: 'late-send-child-promise'
        }, 'child'));

        const messages = await Promise.all([
            waitForCommand(fake, 'late-send-child-callback'),
            waitForCommand(fake, 'late-send-child-promise')
        ]);
        // registered before the send callback has run
        assert.strictEqual(typeof chrome._callbacks[messages[0].id], 'function');
        assert.strictEqual(typeof chrome._callbacks[messages[1].id], 'function');

        fake.send({
            method: 'Target.detachedFromTarget',
            sessionId: 'parent',
            params: {sessionId: 'child'}
        });
        await serverBarrier(fake, chrome, 'late-send-detached');
        await childCallbackDone.promise;
        const childResult = await childPromise;
        assert.strictEqual(childCallbackCalls, 1);
        assert.strictEqual(childResult.error.code, 'ERR_CDP_SESSION_DETACHED');

        const localErrors = [];
        chrome.send('Runtime.evaluate', {expression: 'late-send-local'},
            'child', (error) => localErrors.push(error));
        chrome.Runtime.evaluate({expression: 'late-send-local-shorthand'},
            'child', (error) => localErrors.push(error));
        assert.strictEqual(localErrors.length, 2);
        for (const error of localErrors) {
            assert.strictEqual(error.code, 'ERR_CDP_SESSION_DETACHED');
        }

        // client->server barrier: no frame was written to the invalid session
        const barrierResult = outcome(chrome.send('Runtime.evaluate', {
            expression: 'late-send-barrier'
        }));
        const barrierMessage = await waitForCommand(fake, 'late-send-barrier');
        assert.strictEqual(fake.messages.filter((message) => {
            return message.params && (
                message.params.expression === 'late-send-local' ||
                message.params.expression === 'late-send-local-shorthand'
            );
        }).length, 0);

        // release the delayed send callbacks, one success and one failure; they
        // must not settle the commands a second time or put them back
        sendCallbacks[0]();
        sendCallbacks[1](new Error('late send failure'));

        fake.respond(barrierMessage, {});
        await barrierResult;
        sendCallbacks[2]();

        assert.strictEqual(childCallbackCalls, 1);
        assert.strictEqual(childResult.error.code, 'ERR_CDP_SESSION_DETACHED');
        assert.deepStrictEqual(chrome._callbacks, {});
        assert.strictEqual(chrome._commandSessions.size, 0);
        assert.strictEqual(chrome._sessionCommands.size, 0);
    });

test('a reply can arrive before the ws.send callback without double settling',
    {timeout: 5000}, async (t) => {
        const {fake, chrome} = await setup(t);

        const originalSend = chrome._ws.send.bind(chrome._ws);
        const sendCallbacks = [];
        chrome._ws.send = (data, callback) => {
            sendCallbacks.push(callback);
            originalSend(data, () => {});
        };

        let calls = 0;
        const done = deferred();
        chrome.send('Runtime.evaluate', {expression: 'reply-first'}, (error, response) => {
            calls++;
            assert.strictEqual(error, false);
            assert.deepStrictEqual(response, {value: 'reply-first'});
            done.resolve();
        });
        const message = await waitForCommand(fake, 'reply-first');
        fake.respond(message, {value: 'reply-first'});
        await done.promise;
        assert.strictEqual(calls, 1);
        assert.deepStrictEqual(chrome._callbacks, {});

        // the late callback has nothing left to do
        sendCallbacks[0]();
        assert.strictEqual(calls, 1);
        assert.deepStrictEqual(chrome._callbacks, {});

        // same window, but the delayed callback reports a send failure
        let failureCalls = 0;
        const failureDone = deferred();
        chrome.send('Runtime.evaluate', {expression: 'reply-first-error'},
            (error, response) => {
                failureCalls++;
                assert.strictEqual(error, false);
                assert.deepStrictEqual(response, {value: 'reply-first-error'});
                failureDone.resolve();
            });
        const failureMessage = await waitForCommand(fake, 'reply-first-error');
        fake.respond(failureMessage, {value: 'reply-first-error'});
        await failureDone.promise;
        assert.strictEqual(failureCalls, 1);

        sendCallbacks[1](new Error('late send failure'));
        assert.strictEqual(failureCalls, 1);
        assert.deepStrictEqual(chrome._callbacks, {});
    });

test('closing the connection settles remaining commands exactly once',
    {timeout: 5000}, async (t) => {
        const {fake, chrome} = await setup(t);

        const readyEvents = [];
        chrome.on('ready', () => readyEvents.push(true));

        const childPromise = outcome(chrome.Runtime.evaluate({
            expression: 'close-child'
        }, 'child'));
        const siblingPromise = outcome(chrome.send('Runtime.evaluate', {
            expression: 'close-sibling'
        }, 'sibling'));
        let callbackCalls = 0;
        const callbackDone = deferred();
        chrome.send('Runtime.evaluate', {expression: 'close-root'}, (error, response) => {
            callbackCalls++;
            assert(error instanceof Error);
            assert(!(error instanceof Chrome.ProtocolError));
            assert.strictEqual(response, undefined);
            callbackDone.resolve();
        });

        await Promise.all([
            waitForCommand(fake, 'close-child'),
            waitForCommand(fake, 'close-sibling'),
            waitForCommand(fake, 'close-root')
        ]);

        await chrome.close();
        const childResult = await childPromise;
        const siblingResult = await siblingPromise;
        await callbackDone.promise;
        assert(childResult.error instanceof Error);
        assert(!(childResult.error instanceof Chrome.ProtocolError));
        assert(siblingResult.error instanceof Error);
        assert(!(siblingResult.error instanceof Chrome.ProtocolError));
        assert.strictEqual(callbackCalls, 1);
        assert.deepStrictEqual(chrome._callbacks, {});
        assert.strictEqual(chrome._commandSessions.size, 0);
        assert.strictEqual(chrome._sessionCommands.size, 0);
        assert.strictEqual(readyEvents.length, 1);

        // commands issued after close fail locally as well
        const afterClose = outcome(chrome.send('Runtime.evaluate', {
            expression: 'after-close'
        }));
        const afterCloseResult = await afterClose;
        assert(afterCloseResult.error instanceof Error);
        assert.strictEqual(fake.messages.filter((message) => {
            return message.params && message.params.expression === 'after-close';
        }).length, 0);
    });
