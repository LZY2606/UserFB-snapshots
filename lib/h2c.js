/**
 * @file simplecrawler's cleartext HTTP/2 (h2c) transport. Requests to origins
 * listed in {@link Crawler#h2cOrigins} are made over prior-knowledge HTTP/2
 * connections (using node's built-in http2 module) instead of HTTP/1, while
 * the queue, robots.txt handling, discovery and event machinery of the
 * crawler are reused unchanged.
 */

var http2         = require("http2"),
    EventEmitter  = require("events").EventEmitter,
    util          = require("util");

// Connection-specific (hop-by-hop) header fields that must never be
// forwarded onto an HTTP/2 connection (RFC 7540, section 8.1.2.2). The
// Host header is handled separately, since it maps to :authority.
var HOP_BY_HOP_HEADERS = [
    "connection",
    "keep-alive",
    "proxy-connection",
    "proxy-authenticate",
    "proxy-authorization",
    "transfer-encoding",
    "upgrade",
    "http2-settings"
];

function sessionClosedError() {
    var error = new Error("The h2c session closed before the request could complete");
    error.code = "H2_SESSION_CLOSED";
    return error;
}

function streamAbortedError() {
    var error = new Error("The h2c stream was aborted before the response completed");
    error.code = "H2_STREAM_ABORTED";
    return error;
}

function goawayExcludedError() {
    var error = new Error("The h2c request was excluded by a GOAWAY frame twice and won't be resent again");
    error.code = "H2_GOAWAY_EXCLUDED";
    return error;
}

/**
 * Validates and normalizes a list of h2c origin strings. Only cleartext
 * http:// origins without a path, query, fragment or credentials are
 * accepted. The default port and an explicit port of 80 normalize to the
 * same origin (URL's `origin` serialization drops a default port).
 * @param  {Array} origins Array of origin strings, eg. ["http://example.com:8080"]
 * @return {Object}         Map of normalized origin strings
 */
function normalizeOrigins(origins) {
    if (!Array.isArray(origins)) {
        throw new Error("h2cOrigins must be an array of http:// origin strings");
    }

    var normalized = {};

    origins.forEach(function(entry) {
        var parsed;

        try {
            parsed = new URL(String(entry));
        } catch (error) {
            throw new Error("Invalid h2cOrigins entry (not a valid URL): " + entry);
        }

        if (parsed.protocol !== "http:") {
            throw new Error("Invalid h2cOrigins entry (only cleartext http:// origins are supported): " + entry);
        }

        if (parsed.username || parsed.password) {
            throw new Error("Invalid h2cOrigins entry (must not contain credentials): " + entry);
        }

        if (parsed.pathname !== "/" || parsed.search || parsed.hash) {
            throw new Error("Invalid h2cOrigins entry (must not contain a path, query string or fragment): " + entry);
        }

        if (!parsed.hostname) {
            throw new Error("Invalid h2cOrigins entry (missing hostname): " + entry);
        }

        normalized[parsed.origin] = true;
    });

    return normalized;
}

/**
 * Manages a pool of h2c sessions, one per origin. Requests are handed a
 * stream once the session's first remote SETTINGS frame has arrived and the
 * peer's maxConcurrentStreams limit admits a new stream.
 * @param  {Crawler} crawler The crawler this client belongs to
 * @class
 */
var H2CClient = function(crawler) {
    this.crawler = crawler;

    // Dispatch table of usable sessions, keyed by origin. Sessions that
    // received a GOAWAY (or closed) are removed from this table, so new
    // requests get a fresh session generation.
    this.sessions = {};

    // All live session entries, including GOAWAY'd ones that are still
    // draining their remaining streams
    this.entries = [];

    this.destroyed = false;
};

/**
 * Creates a new logical request. The returned object mimics the parts of
 * the http.ClientRequest interface that the crawler relies upon.
 * @param  {String} origin         The normalized origin to request from
 * @param  {Object} requestOptions Request options as produced by {@link Crawler#getRequestOptions}
 * @param  {Function} callback     Called with the response object once response headers arrive
 * @return {H2CRequest}
 */
H2CClient.prototype.request = function(origin, requestOptions, callback) {
    return new H2CRequest(this, origin, requestOptions, callback);
};

/**
 * Tears down every session managed by this client. Requests still waiting
 * for a stream are failed; in-flight requests are expected to have been
 * aborted by the crawler already (stop(true)) or to have completed
 * (normal completion).
 * @return {H2CClient} Returns the client instance to enable chained calls
 */
H2CClient.prototype.destroy = function() {
    if (this.destroyed) {
        return this;
    }

    this.destroyed = true;

    this.entries.forEach(function(entry) {
        entry.pending.splice(0).forEach(function(request) {
            request._fail(sessionClosedError());
        });

        if (!entry.session.destroyed) {
            entry.session.destroy();
        }
    });

    this.entries = [];
    this.sessions = {};

    return this;
};

/**
 * Finds or creates the dispatchable session for an origin and queues the
 * request for a stream on it.
 * @private
 * @param  {H2CRequest} request The request to dispatch
 */
H2CClient.prototype._dispatch = function(request) {
    if (request.terminated || request.aborted) {
        return;
    }

    if (this.destroyed) {
        return request._fail(sessionClosedError());
    }

    var entry = this.sessions[request.origin];

    if (!entry) {
        entry = this._createSession(request.origin);
    }

    request.entry = entry;
    entry.pending.push(request);
    this._pump(entry);
};

/**
 * Creates a new prior-knowledge h2c session to an origin. No stream is
 * created until the first remote SETTINGS frame has been received.
 * @private
 * @param  {String} origin The normalized origin to connect to
 * @return {Object}        The new session entry
 */
H2CClient.prototype._createSession = function(origin) {
    var client = this;

    var session = http2.connect(origin);

    var entry = {
        origin: origin,
        session: session,
        ready: false,
        limit: 0,
        active: 0,
        pending: [],
        streaming: new Set(),
        goaway: false,
        error: null
    };

    session.on("remoteSettings", function(settings) {
        entry.ready = true;
        entry.limit = settings.maxConcurrentStreams === undefined ?
            Infinity : settings.maxConcurrentStreams;

        // The limit may have been raised or lowered. Existing streams are
        // never touched - only the admission of new streams is governed.
        client._pump(entry);
    });

    session.on("goaway", function(errorCode, lastStreamID) {
        client._onGoaway(entry, lastStreamID);
    });

    session.on("error", function(error) {
        entry.error = error;
        client._failEntry(entry, error);
    });

    session.on("close", function() {
        client._onSessionClose(entry);
    });

    this.sessions[origin] = entry;
    this.entries.push(entry);

    return entry;
};

/**
 * Admits pending requests onto a session while the peer's
 * maxConcurrentStreams limit allows it.
 * @private
 * @param  {Object} entry The session entry to pump
 */
H2CClient.prototype._pump = function(entry) {
    if (!entry.ready || entry.goaway ||
            entry.session.destroyed || entry.session.closed) {
        return;
    }

    while (entry.pending.length && entry.active < entry.limit) {
        var request = entry.pending.shift();
        this._startStream(entry, request);
    }
};

/**
 * Creates the actual HTTP/2 stream for a request, translating the HTTP/1
 * style request options into HTTP/2 headers.
 * @private
 * @param  {Object} entry     The session entry to create the stream on
 * @param  {H2CRequest} request The request to start
 */
H2CClient.prototype._startStream = function(entry, request) {
    var client = this;

    var headers = {
        ":method": request.method,
        ":scheme": "http",
        ":path": request.path
    };

    Object.keys(request.headers).forEach(function(name) {
        var lower = name.toLowerCase(),
            value = request.headers[name];

        // Host maps to :authority
        if (lower === "host") {
            headers[":authority"] = value;
            return;
        }

        // TE is only legal with the value "trailers"
        if (lower === "te" && String(value).toLowerCase() !== "trailers") {
            return;
        }

        // No body is ever sent, and hop-by-hop headers are illegal in HTTP/2
        if (lower === "content-length" || HOP_BY_HOP_HEADERS.indexOf(lower) !== -1) {
            return;
        }

        headers[lower] = value;
    });

    if (!headers[":authority"]) {
        headers[":authority"] = request.origin.replace(/^http:\/\//i, "");
    }

    var stream;

    try {
        stream = entry.session.request(headers);
    } catch (error) {
        return request._fail(error);
    }

    entry.active++;
    entry.streaming.add(request);
    request.stream = stream;

    stream.on("response", function(responseHeaders) {
        request._onResponse(stream, responseHeaders);
    });

    stream.on("error", function(error) {
        request._onStreamFailure(stream, error);
    });

    stream.on("aborted", function() {
        request._onStreamFailure(stream, streamAbortedError());
    });

    stream.on("close", function() {
        entry.active--;
        client._pump(entry);
    });

    // GET requests carry no body - half-close the stream right away
    stream.end();
};

/**
 * Handles a GOAWAY frame: this generation of the session leaves the
 * dispatch table. Streams at or below lastStreamID (and any stream that
 * already delivered response headers) are left to finish on the old
 * session. Refused streams without delivered headers (GET only) and
 * requests that never got a stream move to a new session generation.
 * @private
 * @param  {Object} entry        The session entry the GOAWAY was received on
 * @param  {Number} lastStreamID The last stream ID the server will process
 */
H2CClient.prototype._onGoaway = function(entry, lastStreamID) {
    var client = this;

    entry.goaway = true;

    if (this.sessions[entry.origin] === entry) {
        delete this.sessions[entry.origin];
    }

    // Requests that never got a stream move to a new session generation
    var pending = entry.pending.splice(0);

    // Refused streams that haven't delivered response headers yet are
    // resent on a new session - GET requests only
    var refugees = [];

    entry.streaming.forEach(function(request) {
        if (request.stream &&
                request.stream.id > lastStreamID &&
                !request.headersDelivered &&
                request.method === "GET") {

            entry.streaming.delete(request);

            var stream = request.stream;
            request.stream = null;

            if (!stream.destroyed) {
                stream.destroy();
            }

            refugees.push(request);
        }
    });

    // Requests that were already streaming arrived first, so they're
    // re-queued ahead of merely pending ones
    refugees.concat(pending).forEach(function(request) {
        request.entry = null;
        client._resendOrFail(request);
    });

    // Remaining streams finish on this session, which is then closed
    if (!entry.session.destroyed) {
        entry.session.close();
    }
};

/**
 * Resends a request that was excluded by a GOAWAY frame, or fails it if it
 * has already been resent once. A logical request is never resent more than
 * once, so a second exclusion ends the request instead of reconnecting
 * forever.
 * @private
 * @param  {H2CRequest} request The request to resend or fail
 */
H2CClient.prototype._resendOrFail = function(request) {
    if (request.terminated || request.aborted) {
        return;
    }

    request.goawayResends++;

    if (request.goawayResends > 1) {
        request._fail(goawayExcludedError());
    } else {
        this._dispatch(request);
    }
};

/**
 * Handles a session closing: the entry leaves all tables and any request
 * still attached to it is failed. Ordinary network errors are never
 * retried.
 * @private
 * @param  {Object} entry The session entry that closed
 */
H2CClient.prototype._onSessionClose = function(entry) {
    if (this.sessions[entry.origin] === entry) {
        delete this.sessions[entry.origin];
    }

    var index = this.entries.indexOf(entry);

    if (index !== -1) {
        this.entries.splice(index, 1);
    }

    this._failEntry(entry, entry.error || sessionClosedError());
};

/**
 * Fails every request still attached to a session entry.
 * @private
 * @param  {Object} entry The session entry
 * @param  {Error} error  The error to fail the requests with
 */
H2CClient.prototype._failEntry = function(entry, error) {
    var stranded = entry.pending.splice(0).concat(Array.from(entry.streaming));

    stranded.forEach(function(request) {
        request._fail(error);
    });
};

/**
 * A logical request over h2c. Presents the subset of the
 * http.ClientRequest interface that the crawler uses: end(), abort(),
 * aborted, setTimeout() and the "abort"/"error" events.
 * @param  {H2CClient} client        The client that owns this request
 * @param  {String} origin           The normalized origin to request from
 * @param  {Object} requestOptions   Request options as produced by {@link Crawler#getRequestOptions}
 * @param  {Function} callback       Called with the response object once response headers arrive
 * @class
 */
var H2CRequest = function(client, origin, requestOptions, callback) {
    EventEmitter.call(this);

    this.client = client;
    this.origin = origin;
    this.method = (requestOptions.method || "GET").toUpperCase();
    this.path = requestOptions.path || "/";
    this.headers = requestOptions.headers || {};
    this.callback = callback;

    this.aborted = false;
    this.terminated = false;
    this.headersDelivered = false;
    this.goawayResends = 0;

    this.entry = null;
    this.stream = null;
    this.response = null;

    this._timer = null;
};

util.inherits(H2CRequest, EventEmitter);

/**
 * Starts the request. Dispatch is deferred by a tick so callers can attach
 * event listeners first, mirroring http.ClientRequest.
 * @return {H2CRequest} Returns the request instance to enable chained calls
 */
H2CRequest.prototype.end = function() {
    var request = this;

    process.nextTick(function() {
        request.client._dispatch(request);
    });

    return this;
};

/**
 * Sets a timeout for the request, mirroring
 * http.ClientRequest#setTimeout. The crawler's own timeout logic decides
 * what happens when the timer fires.
 * @param  {Number} msecs      Timeout in milliseconds
 * @param  {Function} callback Called once if the timeout elapses
 * @return {H2CRequest}        Returns the request instance to enable chained calls
 */
H2CRequest.prototype.setTimeout = function(msecs, callback) {
    if (this._timer) {
        clearTimeout(this._timer);
        this._timer = null;
    }

    if (callback instanceof Function) {
        this._timer = setTimeout(callback, msecs);

        if (this._timer.unref) {
            this._timer.unref();
        }
    }

    return this;
};

/**
 * Aborts the request, following the same notification rules as an aborted
 * http.ClientRequest: an "abort" event is emitted and no "error" event
 * follows.
 * @return {H2CRequest} Returns the request instance to enable chained calls
 */
H2CRequest.prototype.abort = function() {
    if (this.aborted) {
        return this;
    }

    this.aborted = true;
    this._clearTimer();
    this._detach(true);
    this.emit("abort");

    return this;
};

/**
 * Clears the timeout timer, if any
 * @private
 */
H2CRequest.prototype._clearTimer = function() {
    if (this._timer) {
        clearTimeout(this._timer);
        this._timer = null;
    }
};

/**
 * Detaches the request from its session entry and (optionally) destroys
 * the underlying stream.
 * @private
 * @param  {Boolean} destroyStream Whether to destroy the underlying stream
 */
H2CRequest.prototype._detach = function(destroyStream) {
    var entry = this.entry;

    if (entry) {
        var index = entry.pending.indexOf(this);

        if (index !== -1) {
            entry.pending.splice(index, 1);
        }

        entry.streaming.delete(this);
        this.entry = null;
    }

    var stream = this.stream;
    this.stream = null;

    if (destroyStream && stream && !stream.destroyed) {
        stream.destroy();
    }
};

/**
 * Fails the request, emitting an "error" event - unless the request was
 * already settled or manually aborted.
 * @private
 * @param  {Error} error The error to fail with
 */
H2CRequest.prototype._fail = function(error) {
    if (this.terminated || this.aborted) {
        return;
    }

    this.terminated = true;
    this._clearTimer();
    this._detach(true);
    this.emit("error", error);
};

/**
 * Handles response headers arriving on the stream: builds the response
 * object and hands it to the crawler's response handling.
 * @private
 * @param  {ClientHttp2Stream} stream The stream the headers arrived on
 * @param  {Object} headers           The response headers (including :status)
 */
H2CRequest.prototype._onResponse = function(stream, headers) {
    if (this.stream !== stream || this.terminated || this.aborted) {
        return;
    }

    this.headersDelivered = true;
    this.response = new H2CResponse(this, stream, headers);
    this.callback(this.response);
};

/**
 * Called once the complete response body has been received. The request is
 * settled - late stream events must not turn into errors.
 * @private
 */
H2CRequest.prototype._onResponseEnd = function() {
    this.terminated = true;
    this._clearTimer();
    this._detach(false);
};

/**
 * Handles stream failures. A response whose body is interrupted is a
 * failure - it is never replayed and never delivered as a partial page.
 * @private
 * @param  {ClientHttp2Stream} stream The stream that failed
 * @param  {Error} error              The failure
 */
H2CRequest.prototype._onStreamFailure = function(stream, error) {
    if (this.stream !== stream) {
        return;
    }

    this._fail(error);
};

/**
 * A response over h2c. Presents the subset of the http.IncomingMessage
 * interface that the crawler uses: statusCode, headers, req, destroy() and
 * the "data"/"end" events. Body data is buffered until the crawler attaches
 * a "data" listener, since the crawler attaches its listeners
 * asynchronously.
 * @param  {H2CRequest} request       The request this response belongs to
 * @param  {ClientHttp2Stream} stream The underlying HTTP/2 stream
 * @param  {Object} headers           The response headers (including :status)
 * @class
 */
var H2CResponse = function(request, stream, headers) {
    EventEmitter.call(this);

    var response = this;

    this.req = request;
    this.statusCode = headers[":status"];
    this.headers = {};

    // Pseudo headers are not exposed to the crawler
    Object.keys(headers).forEach(function(name) {
        if (name.charAt(0) !== ":") {
            response.headers[name] = headers[name];
        }
    });

    this._request = request;
    this._stream = stream;
    this._chunks = [];
    this._ended = false;
    this._endEmitted = false;
    this._destroyed = false;

    stream.on("data", function(chunk) {
        response._chunks.push(chunk);
        response._drain();
    });

    stream.on("end", function() {
        response._ended = true;
        request._onResponseEnd();
        response._drain();
    });

    this.on("newListener", function(event) {
        if (event === "data" || event === "end") {
            process.nextTick(function() {
                response._drain();
            });
        }
    });
};

util.inherits(H2CResponse, EventEmitter);

/**
 * Emits buffered body data (and "end", if the body is complete) once the
 * crawler is listening.
 * @private
 */
H2CResponse.prototype._drain = function() {
    if (this._destroyed || !this.listeners("data").length) {
        return;
    }

    var chunks = this._chunks;
    this._chunks = [];

    chunks.forEach(function(chunk) {
        this.emit("data", chunk);
    }, this);

    if (this._ended && !this._endEmitted) {
        this._endEmitted = true;
        this.emit("end");
    }
};

/**
 * Destroys the response and the underlying stream. Since the crawler
 * destroys responses deliberately (redirects, error statuses, size
 * limits), the stream is detached from the request first so its teardown
 * doesn't surface as a request error.
 */
H2CResponse.prototype.destroy = function() {
    if (this._destroyed) {
        return;
    }

    this._destroyed = true;
    this._chunks = [];

    var stream = this._stream;

    // The request is done as far as the crawler is concerned
    this._request.terminated = true;
    this._request._clearTimer();

    if (this._request.stream === stream) {
        this._request._detach(false);
    }

    if (stream && !stream.destroyed) {
        stream.destroy();
    }
};

module.exports = H2CClient;
module.exports.normalizeOrigins = normalizeOrigins;
