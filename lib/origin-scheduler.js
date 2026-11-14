/**
 * @file Per-origin fair scheduling for the crawler. When enabled through
 * {@link Crawler#originScheduling}, this scheduler hands out send slots in a
 * round-robin fashion between origins, while respecting both the global
 * {@link Crawler#maxConcurrency} cap and a per-origin concurrency cap.
 *
 * The scheduler never reaches into a queue implementation directly beyond the
 * documented `getLength`/`get` interface, and it never indexes queue items in
 * memory. Reads may be delayed or return copies, so all eligibility decisions
 * are made through callbacks and reservations are tracked by item ID.
 */

var EventEmitter = require("events").EventEmitter,
    util = require("util");

var DEFAULT_PORTS = {
    http: 80,
    https: 443
};

/**
 * Builds the identity key of an origin from a queue item. Origins are
 * distinguished by the lowercased scheme and hostname, together with the
 * effective port. An omitted port and the scheme's default port are treated as
 * the same origin.
 * @param  {QueueItem} queueItem Queue item to derive the origin from
 * @return {String}              A stable, lowercase origin key
 */
function originKeyOf(queueItem) {
    var protocol = String(queueItem.protocol || "http").toLowerCase(),
        hostname = String(queueItem.host || "").toLowerCase(),
        port = queueItem.port;

    if (port === "" || port === undefined || port === null) {
        port = DEFAULT_PORTS[protocol] || DEFAULT_PORTS.http;
    } else {
        port = Number(port);
        if (!Number.isFinite(port)) {
            port = DEFAULT_PORTS[protocol] || DEFAULT_PORTS.http;
        }
    }

    return protocol + "://" + hostname + ":" + port;
}

/**
 * Creates a new origin scheduler.
 * @class
 * @param {Object} options                      Scheduler options
 * @param {FetchQueue} options.queue            Queue used to locate candidates
 * @param {Number} options.maxConcurrency       Global concurrency cap shared by all origins
 * @param {Number} options.maxConcurrencyPerOrigin Concurrency cap applied to each individual origin
 * @param {Function} options.execute           Called with every scheduling decision
 */
var OriginScheduler = function(options) {
    EventEmitter.call(this);

    var scheduler = this;

    this.queue = options.queue;
    this.maxConcurrency = options.maxConcurrency;
    this.maxConcurrencyPerOrigin = options.maxConcurrencyPerOrigin;
    this.respectRobotsTxt = options.respectRobotsTxt !== false;
    this.execute = options.execute;

    /**
     * Origins in the order their first queue item appeared.
     * @type {String[]}
     */
    this._order = [];

    /**
     * Per-origin state: active slot count and robots.txt loading state.
     * @type {Object}
     */
    this._states = Object.create(null);

    /**
     * All outstanding reservations, keyed by token. A reservation covers
     * everything from the moment a slot is granted until the work (robots.txt
     * fetch, page fetch or rejection) has settled, including waits for
     * asynchronous queue writes.
     * @type {Map}
     */
    this._slots = new Map();

    /**
     * Queue item IDs that currently hold a reservation. The queue itself may
     * return stale copies still marked `queued`, so this set is the authority
     * preventing an item from being scheduled twice.
     * @type {Set}
     */
    this._reservedIds = new Set();

    /**
     * Round-robin cursor: index into {@link OriginScheduler#_order} of the
     * next origin to consider.
     * @type {Number}
     */
    this._turn = 0;

    this._tokenSequence = 0;
    this._pumpRunning = false;
    this._pumpQueued = false;
    this._stopped = false;

    this.schedulePump = function() {
        if (scheduler._stopped) {
            return;
        }

        if (scheduler._pumpRunning) {
            scheduler._pumpQueued = true;
            return;
        }

        scheduler._pumpRunning = true;
        process.nextTick(function() {
            scheduler._pump(function(granted) {
                scheduler._pumpRunning = false;

                if (scheduler._stopped) {
                    scheduler._pumpQueued = false;
                    return;
                }

                if (scheduler._slots.size === 0) {
                    // No outstanding work. Periodic interval ticks re-arm the
                    // queued flag, but re-running an empty pump forever would
                    // livelock; settle instead. Any real state change (a queue
                    // add or a released reservation) triggers a fresh pump.
                    scheduler._pumpQueued = false;
                    scheduler.emit("idle");
                    return;
                }

                // Keep the chain going only when this pump actually handed out
                // slots and new work accumulated while it was running. An
                // empty pump ends here and waits for the next event or tick.
                if (granted && scheduler._pumpQueued) {
                    scheduler._pumpQueued = false;
                    scheduler.schedulePump();
                } else {
                    scheduler._pumpQueued = false;
                }
            });
        });
    };
};

util.inherits(OriginScheduler, EventEmitter);

/**
 * Registers the origin of a queue item. The rotation order is established by
 * the first time an origin ever appears.
 * @param  {QueueItem} queueItem Item that was added to the queue
 * @return {String}              The origin key of the item
 */
OriginScheduler.prototype.noteItem = function(queueItem) {
    var key = originKeyOf(queueItem);

    if (!this._states[key]) {
        this._states[key] = {
            active: 0,
            // Without robots.txt handling, origins are immediately ready to
            // serve pages; otherwise the first slot fetches robots.txt.
            robots: this.respectRobotsTxt ? "idle" : "ready"
        };
        this._order.push(key);
    }

    return key;
};

/**
 * Marks an origin's robots.txt fetch as finished, whether it succeeded or
 * failed. A failed fetch keeps the "ready" state, mirroring the crawler's
 * existing policy of continuing after a `robotstxterror`.
 * @param {String} key Origin key
 */
OriginScheduler.prototype.markRobotsReady = function(key) {
    if (this._states[key]) {
        this._states[key].robots = "ready";
    }
};

/**
 * Returns an origin to its pre-robots state. Used when a scheduled robots.txt
 * decision was handed out but never executed because the crawler was stopped,
 * so a later restart retries the fetch.
 * @param {String} key Origin key
 */
OriginScheduler.prototype.markRobotsIdle = function(key) {
    if (this._states[key] && this.respectRobotsTxt) {
        this._states[key].robots = "idle";
    }
};

/**
 * Stops scheduling new work. Reservations already handed out are left to settle
 * or abort on their own.
 */
OriginScheduler.prototype.stop = function() {
    this._stopped = true;
    this._pumpQueued = false;
};

/**
 * Resumes scheduling after a {@link OriginScheduler#stop}, eg. when the crawler
 * is started again.
 */
OriginScheduler.prototype.restart = function() {
    this._stopped = false;
};

/**
 * Reports whether every reservation, pump and queue scan has settled.
 * @return {Boolean} True when no scheduling work is outstanding
 */
OriginScheduler.prototype.isIdle = function() {
    return this._slots.size === 0 &&
        !this._pumpRunning &&
        !this._pumpQueued;
};

/**
 * Reserves a send slot against both the global and the per-origin caps.
 * @private
 * @param  {String} key       Origin key
 * @param  {QueueItem} queueItem Item the slot is reserved for
 * @param  {String} kind      Either "robots" or "page"
 * @return {Number}           Reservation token
 */
OriginScheduler.prototype._reserve = function(key, queueItem, kind) {
    var token = ++this._tokenSequence;

    this._slots.set(token, {
        key: key,
        id: queueItem.id,
        kind: kind
    });
    this._reservedIds.add(queueItem.id);
    this._states[key].active++;

    return token;
};

/**
 * Releases a reservation. Releasing is idempotent: a slot can only be released
 * once, no matter how many terminal paths (queue write failure, HTTP error,
 * redirect, timeout) fire for the same item.
 * @param  {Number} token Reservation token returned when the slot was granted
 */
OriginScheduler.prototype.release = function(token) {
    var slot = this._slots.get(token);

    if (!slot) {
        return;
    }

    this._slots.delete(token);
    this._reservedIds.delete(slot.id);

    if (this._states[slot.key]) {
        this._states[slot.key].active--;
    }
};

/**
 * Scans the queue (through its async, copy-returning interface) for the oldest
 * queued item of a given origin that doesn't already hold a reservation.
 * @private
 * @param  {String} key      Origin key
 * @param  {Function} callback Called with (error, queueItem|null)
 */
OriginScheduler.prototype._findCandidate = function(key, callback) {
    var scheduler = this,
        index = 0,
        length;

    function examine() {
        if (index >= length) {
            return callback(null, null);
        }

        scheduler.queue.get(index, function(error, queueItem) {
            if (error) {
                // Stale lengths can push reads past the end of a database
                // backed queue. Treat that as "no more candidates", any other
                // read error likewise just skips this scan round.
                return callback(null, null);
            }

            if (queueItem &&
                queueItem.status === "queued" &&
                !scheduler._reservedIds.has(queueItem.id) &&
                originKeyOf(queueItem) === key) {

                return callback(null, queueItem);
            }

            index++;
            // Break the call chain: in-memory queues call back synchronously,
            // so a plain recursion would overflow on a large queue.
            setImmediate(examine);
        });
    }

    this.queue.getLength(function(error, queueLength) {
        if (error) {
            return callback(error);
        }

        length = queueLength;
        setImmediate(examine);
    });
};

/**
 * Repeatedly grants slots while global capacity remains, walking the origins in
 * rotation. Origins that are capped, loading robots.txt or have no available
 * work are skipped without blocking the origins behind them.
 * @private
 * @param  {Function} done Called when this pump run has settled
 */
OriginScheduler.prototype._pump = function(done) {
    var scheduler = this,
        emptyProbes = 0,
        granted = false;

    function finish() {
        done(granted);
    }

    function step() {
        if (scheduler._stopped || scheduler._slots.size >= scheduler.maxConcurrency) {
            return finish();
        }

        if (!scheduler._order.length) {
            return finish();
        }

        // Locate the next origin with spare per-origin capacity whose robots
        // policy isn't still loading, scanning at most one full rotation.
        var offset = 0,
            key = null,
            state = null;

        for (; offset < scheduler._order.length; offset++) {
            var candidateKey =
                scheduler._order[(scheduler._turn + offset) % scheduler._order.length],
                candidateState = scheduler._states[candidateKey];

            if (candidateState.active < scheduler.maxConcurrencyPerOrigin &&
                candidateState.robots !== "loading") {

                key = candidateKey;
                state = candidateState;
                break;
            }
        }

        if (!key) {
            return finish();
        }

        scheduler._findCandidate(key, function(error, queueItem) {
            if (scheduler._stopped) {
                return finish();
            }

            if (error || !queueItem) {
                // Nothing sendable for this origin right now. Skip it and let
                // the rest of the rotation take its turn. If a whole rotation
                // produced no grants this pump ends; waiting work can only
                // appear through new queue events or released reservations,
                // and those trigger a fresh pump themselves.
                emptyProbes++;
                scheduler._turn =
                    (scheduler._order.indexOf(key) + 1) % scheduler._order.length;

                if (emptyProbes >= scheduler._order.length) {
                    return finish();
                }

                return setImmediate(step);
            }

            // A slot is about to be handed out, so the rotation advances to
            // the next origin regardless of whether this is a robots.txt or
            // a page fetch. Grants reset the empty-rotation counter.
            granted = true;
            emptyProbes = 0;
            scheduler._turn =
                (scheduler._order.indexOf(key) + 1) % scheduler._order.length;

            if (state.robots === "idle") {
                state.robots = "loading";
                var robotsToken = scheduler._reserve(key, queueItem, "robots");

                scheduler.execute({
                    type: "robots",
                    token: robotsToken,
                    origin: key,
                    queueItem: queueItem
                });
            } else {
                var pageToken = scheduler._reserve(key, queueItem, "page");

                scheduler.execute({
                    type: "fetch",
                    token: pageToken,
                    origin: key,
                    queueItem: queueItem
                });
            }

            setImmediate(step);
        });
    }

    step();
};

OriginScheduler.originKeyOf = originKeyOf;
module.exports = OriginScheduler;
