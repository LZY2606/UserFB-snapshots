'use strict';

const EventEmitter = require('events');
const util = require('util');
const formatUrl = require('url').format;
const parseUrl = require('url').parse;

const WebSocket = require('ws');

const api = require('./api.js');
const defaults = require('./defaults.js');
const devtools = require('./devtools.js');
const errors = require('./errors.js');

function sessionDetachedError(sessionId) {
    const error = new Error(`CDP session "${sessionId}" detached`);
    error.code = 'ERR_CDP_SESSION_DETACHED';
    error.sessionId = sessionId;
    return error;
}

class Chrome extends EventEmitter {
    constructor(options, notifier) {
        super();
        // options
        const defaultTarget = (targets) => {
            // prefer type = 'page' inspectable targets as they represents
            // browser tabs (fall back to the first inspectable target
            // otherwise)
            let backup;
            let target = targets.find((target) => {
                if (target.webSocketDebuggerUrl) {
                    backup = backup || target;
                    return target.type === 'page';
                } else {
                    return false;
                }
            });
            target = target || backup;
            if (target) {
                return target;
            } else {
                throw new Error('No inspectable targets');
            }
        };
        options = options || {};
        this.host = options.host || defaults.HOST;
        this.port = options.port || defaults.PORT;
        this.secure = !!(options.secure);
        this.useHostName = !!(options.useHostName);
        this.alterPath = options.alterPath || ((path) => path);
        this.protocol = options.protocol;
        this.local = !!(options.local);
        this.target = options.target || defaultTarget;
        // locals
        this._notifier = notifier;
        this._callbacks = {};
        this._nextCommandId = 1;
        // pending commands grouped by CDP session (sessionId is never reused in
        // a connection, so detached sessions stay invalid until the connection
        // is closed)
        this._commandSessions = new Map();
        this._sessionCommands = new Map();
        this._invalidSessions = new Set();
        // properties
        this.webSocketUrl = undefined;
        // operations
        this._start();
    }

    // avoid misinterpreting protocol's members as custom util.inspect functions
    inspect(depth, options) {
        options.customInspect = false;
        return util.inspect(this, options);
    }

    send(method, params, sessionId, callback) {
        // handle optional arguments
        const optionals = Array.from(arguments).slice(1);
        params = optionals.find(x => typeof x === 'object');
        sessionId = optionals.find(x => typeof x === 'string');
        callback = optionals.find(x => typeof x === 'function');
        // return a promise when a callback is not provided
        if (typeof callback === 'function') {
            this._enqueueCommand(method, params, sessionId, callback);
            return undefined;
        } else {
            return new Promise((fulfill, reject) => {
                this._enqueueCommand(method, params, sessionId, (error, response) => {
                    if (error) {
                        const request = {method, params, sessionId};
                        reject(
                            error instanceof Error
                                ? error // low-level WebSocket error
                                : new errors.ProtocolError(request, response)
                        );
                    } else {
                        fulfill(response);
                    }
                });
            });
        }
    }

    close(callback) {
        const closeWebSocket = (callback) => {
            // don't close if it's already closed
            if (this._ws.readyState === 3) {
                callback();
            } else {
                // don't notify on user-initiated shutdown ('disconnect' event)
                this._ws.removeAllListeners('close');
                this._ws.once('close', () => {
                    this._ws.removeAllListeners();
                    this._handleConnectionClose();
                    callback();
                });
                this._ws.close();
            }
        };
        if (typeof callback === 'function') {
            closeWebSocket(callback);
            return undefined;
        } else {
            return new Promise((fulfill, reject) => {
                closeWebSocket(fulfill);
            });
        }
    }

    // initiate the connection process
    async _start() {
        const options = {
            host: this.host,
            port: this.port,
            secure: this.secure,
            useHostName: this.useHostName,
            alterPath: this.alterPath
        };
        try {
            // fetch the WebSocket debugger URL
            const url = await this._fetchDebuggerURL(options);
            // allow the user to alter the URL
            const urlObject = parseUrl(url);
            urlObject.pathname = options.alterPath(urlObject.pathname);
            this.webSocketUrl = formatUrl(urlObject);
            // update the connection parameters using the debugging URL
            options.host = urlObject.hostname;
            options.port = urlObject.port || options.port;
            // fetch the protocol and prepare the API
            const protocol = await this._fetchProtocol(options);
            api.prepare(this, protocol);
            // finally connect to the WebSocket
            await this._connectToWebSocket();
            // since the handler is executed synchronously, the emit() must be
            // performed in the next tick so that uncaught errors in the client code
            // are not intercepted by the Promise mechanism and therefore reported
            // via the 'error' event
            process.nextTick(() => {
                this._notifier.emit('connect', this);
            });
        } catch (err) {
            this._notifier.emit('error', err);
        }
    }

    // fetch the WebSocket URL according to 'target'
    async _fetchDebuggerURL(options) {
        const userTarget = this.target;
        switch (typeof userTarget) {
        case 'string': {
            let idOrUrl = userTarget;
            // use default host and port if omitted (and a relative URL is specified)
            if (idOrUrl.startsWith('/')) {
                idOrUrl = `ws://${this.host}:${this.port}${idOrUrl}`;
            }
            // a WebSocket URL is specified by the user (e.g., node-inspector)
            if (idOrUrl.match(/^wss?:/i)) {
                return idOrUrl; // done!
            }
            // a target id is specified by the user
            else {
                const targets = await devtools.List(options);
                const object = targets.find((target) => target.id === idOrUrl);
                return object.webSocketDebuggerUrl;
            }
        }
        case 'object': {
            const object = userTarget;
            return object.webSocketDebuggerUrl;
        }
        case 'function': {
            const func = userTarget;
            const targets = await devtools.List(options);
            const result = func(targets);
            const object = typeof result === 'number' ? targets[result] : result;
            return object.webSocketDebuggerUrl;
        }
        default:
            throw new Error(`Invalid target argument "${this.target}"`);
        }
    }

    // fetch the protocol according to 'protocol' and 'local'
    async _fetchProtocol(options) {
        // if a protocol has been provided then use it
        if (this.protocol) {
            return this.protocol;
        }
        // otherwise user either the local or the remote version
        else {
            options.local = this.local;
            return await devtools.Protocol(options);
        }
    }

    // establish the WebSocket connection and start processing user commands
    _connectToWebSocket() {
        return new Promise((fulfill, reject) => {
            // create the WebSocket
            try {
                if (this.secure) {
                    this.webSocketUrl = this.webSocketUrl.replace(/^ws:/i, 'wss:');
                }
                this._ws = new WebSocket(this.webSocketUrl, [], {
                    maxPayload: 256 * 1024 * 1024,
                    perMessageDeflate: false,
                    followRedirects: true,
                });
            } catch (err) {
                // handles bad URLs
                reject(err);
                return;
            }
            // set up event handlers
            this._ws.on('open', () => {
                fulfill();
            });
            this._ws.on('message', (data) => {
                const message = JSON.parse(data);
                this._handleMessage(message);
            });
            this._ws.on('close', (code) => {
                this._handleConnectionClose();
                this.emit('disconnect');
            });
            this._ws.on('error', (err) => {
                reject(err);
            });
        });
    }

    _handleConnectionClose() {
        // make sure to complete all the unresolved callbacks
        const err = new Error('WebSocket connection closed');
        this._settleCommands(Object.keys(this._callbacks), err);
    }

    // remove a pending command from the internal registries, returning its
    // callback (if it has not already been settled)
    _takeCommand(id) {
        // all registries are keyed by the string command id (object keys are
        // strings anyway)
        id = String(id);
        const callback = this._callbacks[id];
        if (!callback) {
            return undefined;
        }
        delete this._callbacks[id];
        const sessionId = this._commandSessions.get(id);
        if (sessionId !== undefined) {
            this._commandSessions.delete(id);
            const ids = this._sessionCommands.get(sessionId);
            if (ids) {
                ids.delete(id);
                if (ids.size === 0) {
                    this._sessionCommands.delete(sessionId);
                }
            }
        }
        return callback;
    }

    // settle a single pending command, at most once
    _settleCommand(id, error, response) {
        const callback = this._takeCommand(id);
        if (!callback) {
            return false;
        }
        callback(error, response);
        // notify when there are no more pending commands
        if (Object.keys(this._callbacks).length === 0) {
            this.emit('ready');
        }
        return true;
    }

    // settle a batch of pending commands, at most once each; internal state is
    // updated before user callbacks are invoked so that commands enqueued by
    // those callbacks are not affected by this cleanup
    _settleCommands(ids, error) {
        const callbacks = [];
        for (const id of ids) {
            const callback = this._takeCommand(id);
            if (callback) {
                callbacks.push(callback);
            }
        }
        if (callbacks.length === 0) {
            return;
        }
        const drained = Object.keys(this._callbacks).length === 0;
        for (const callback of callbacks) {
            callback(error, undefined);
        }
        if (drained && Object.keys(this._callbacks).length === 0) {
            this.emit('ready');
        }
    }

    // invalidate a CDP session after the target it belongs to has been detached
    _invalidateSession(sessionId) {
        // detached session IDs are never reused, ignore repeated events
        if (this._invalidSessions.has(sessionId)) {
            return;
        }
        this._invalidSessions.add(sessionId);
        const ids = this._sessionCommands.get(sessionId);
        if (ids && ids.size > 0) {
            this._settleCommands(Array.from(ids), sessionDetachedError(sessionId));
        }
    }

    // handle the messages read from the WebSocket
    _handleMessage(message) {
        // command response
        if (message.id) {
            if (!this._callbacks[message.id]) {
                return;
            }
            // interpret the lack of both 'error' and 'result' as success
            // (this may happen with node-inspector)
            if (message.error) {
                this._settleCommand(message.id, true, message.error);
            } else {
                this._settleCommand(message.id, false, message.result || {});
            }
        }
        // event
        else if (message.method) {
            const {method, params, sessionId} = message;
            // the session being detached is params.sessionId, not the
            // sessionId of the event envelope
            if (method === 'Target.detachedFromTarget' &&
                params && typeof params.sessionId === 'string') {
                this._invalidateSession(params.sessionId);
            }
            this.emit('event', message);
            this.emit(method, params, sessionId);
            this.emit(`${method}.${sessionId}`, params, sessionId);
        }
    }

    // send a command to the remote endpoint and register a callback for the reply
    _enqueueCommand(method, params, sessionId, callback) {
        // a detached session cannot receive new commands, fail without writing
        // anything to the WebSocket
        if (sessionId !== undefined &&
            this._invalidSessions.has(sessionId)) {
            callback(sessionDetachedError(sessionId));
            return;
        }
        const id = this._nextCommandId++;
        const key = String(id);
        // register the command before sending it: the reply (or a detach event)
        // may be handled before the WebSocket send callback runs
        this._callbacks[key] = callback;
        if (sessionId !== undefined) {
            this._commandSessions.set(key, sessionId);
            let ids = this._sessionCommands.get(sessionId);
            if (!ids) {
                ids = new Set();
                this._sessionCommands.set(sessionId, ids);
            }
            ids.add(key);
        }
        const message = {
            id,
            method,
            sessionId,
            params: params || {}
        };
        try {
            this._ws.send(JSON.stringify(message), (err) => {
                if (err) {
                    // handle low-level WebSocket errors, unless the command has
                    // already been settled by a reply, a detach, or a close
                    this._settleCommand(id, err);
                }
            });
        } catch (err) {
            this._settleCommand(id, err);
        }
    }
}

module.exports = Chrome;
