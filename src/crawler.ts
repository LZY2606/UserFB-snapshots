import { EventEmitter } from "events";
import { Cluster } from "./rateLimiter/index.js";
import { isBoolean, isFunction, setDefaults, flattenDeep, lowerObjectKeys, isNumber } from "./lib/utils.js";
import { getValidOptions, alignOptions, getCharset, renameOptionParams } from "./options.js";
import { getLogger } from "./logger.js";
import type { CrawlerOptions, RequestOptions, RequestConfig, CrawlerResponse, RevisionRequestOptions } from "./types/crawler.js";
import { load } from "cheerio";
import seenreq from "seenreq";
import iconv from "iconv-lite";
import type { Got } from "got";
// @todo: remove seenreq dependency
const log = getLogger();

type RevisionPhase = "idle" | "queued" | "downloading" | "retry" | "callback";

/**
 * Bookkeeping for one business key managed by `addRevision`. At most one
 * attempt (`current`) owns a request-pool slot at any time; `pending` holds
 * the newest config that has not been dispatched yet.
 */
interface RevisionState {
    key: string;
    phase: RevisionPhase;
    pending: CrawlerOptions | null;
    current: CrawlerOptions | null;
    task: ((done: () => void) => void) | null;
    limiterDone: (() => void) | null;
    retryTimer: NodeJS.Timeout | null;
    retryGeneration: number;
    callbackDone: boolean;
}

// 缓存 got 实例
let gotInstance: Got | null = null;

async function loadGot() {
  if (!gotInstance) {
    gotInstance = (await import("got")).default;
  }
  return gotInstance;
}

class Crawler extends EventEmitter {
    private _limiters: Cluster;
    private _UAIndex = 0;
    private _proxyIndex = 0;

    public options: CrawlerOptions;
    public seen: any;

    /** Per-key revision bookkeeping, keyed by the business key of addRevision. */
    private _revisionStates: Map<string, RevisionState> = new Map();
    /** Reverse lookup from a live attempt's options back to its revision state. */
    private _revisionContexts: Map<CrawlerOptions, RevisionState> = new Map();

    constructor(options?: CrawlerOptions) {
        super();
        const defaultOptions: CrawlerOptions = {
            maxConnections: 10,
            rateLimit: 0,
            priorityLevels: 10,
            skipDuplicates: false,
            homogeneous: false,
            method: "GET",
            forceUTF8: false,
            jQuery: true,
            priority: 5,
            retries: 2,
            retryInterval: 3000,
            timeout: 20000,
            isJson: false,
            silence: false,
            rejectUnauthorized: false, // set to "true" in production environment.
            userAgents: "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/135.0.0.0 Safari/537.36"
        };
        options = renameOptionParams(options);
        this.options = { ...defaultOptions, ...options };
        if (this.options.rateLimit! > 0) {
            this.options.maxConnections = 1;
        }
        if (this.options.silence) {
            log.settings.minLevel = 7;
        }

        this._limiters = new Cluster({
            maxConnections: this.options.maxConnection !== undefined ? this.options.maxConnection : this.options.maxConnections!,
            rateLimit: this.options.limiter !== undefined ? this.options.limiter : this.options.rateLimit!,
            priorityLevels: this.options.priorityLevels!,
            defaultPriority: this.options.priority!,
            homogeneous: this.options.homogeneous,
        });

        this.seen = new seenreq(this.options.seenreq);
        this.seen
            .initialize()
            .then(() => {
                log.debug("seenreq initialized");
            })
            .catch((error: unknown) => {
                log.error(error);
            });
        this.on("_release", () => {
            log.debug(`Queue size: ${this.queueSize}`);
            if (this._limiters.empty && this._revisionStates.size === 0) this.emit("drain");
        });
    }

    private _detectHtmlOnHeaders = (headers: Record<string, unknown>): boolean => {
        const contentType = headers["content-type"] as string;
        if (/xml|html/i.test(contentType)) return true;
        return false;
    };

    private _schedule = (options: CrawlerOptions): void => {
        this.emit("schedule", options);
        this._limiters
            .getRateLimiter(options.rateLimiterId)
            .submit(options.priority as number, (done, rateLimiterId) => {
                options.release = () => {
                    done();
                    this.emit("_release");
                };
                options.callback = options.callback || options.release;

                if (rateLimiterId) {
                    this.emit("limiterChange", options, rateLimiterId);
                }

                if (options.html) {
                    options.url = options.url ?? "";
                    this._handler(null, options, { body: options.html, headers: { "content-type": "text/html" } });
                } else {
                    options.url = options.url ?? options.uri;
                    if (typeof options.url === "function") {
                        options.url((url: string) => {
                            options.url = url;
                            this._execute(options);
                        });
                    } else {
                        delete options.uri;
                        this._execute(options);
                    }
                }
            });
    };

    private _execute = async (options: CrawlerOptions): Promise<CrawlerResponse> => {
        if (options.proxy) log.debug(`Using proxy: ${options.proxy}`);
        else if (options.proxies) log.debug(`Using proxies: ${options.proxies}`);

        options.headers = options.headers ?? {};
        options.headers = lowerObjectKeys(options.headers);

        if (options.forceUTF8 || options.isJson) options.encoding = "utf8";

        if (Array.isArray(options.userAgents)) {
            this._UAIndex = this._UAIndex % options.userAgents.length;
            options.headers["user-agent"] = options.userAgents[this._UAIndex];
            this._UAIndex++;
        } else {
            options.headers["user-agent"] = options.headers["user-agent"] ?? options.userAgents;
        }

        if (!options.proxy && Array.isArray(options.proxies)) {
            this._proxyIndex = this._proxyIndex % options.proxies.length;
            options.proxy = options.proxies[this._proxyIndex];
            this._proxyIndex++;
        }

        const request = async () => {
            if (options.skipEventRequest !== true) {
                this.emit("request", options);
            }
            let response: CrawlerResponse;
            try {
                const got = await loadGot(); // 动态加载 got
                // got v15 no longer accepts `url` inside the options object; it
                // must be passed as the first argument.
                const { url, ...gotOptions } = alignOptions(options);
                response = await got(url as string | URL, gotOptions);
            } catch (error) {
                log.debug(error);
                return this._handler(error, options);
            }
            return this._handler(null, options, response);
        };

        if (isFunction(options.preRequest)) {
            try {
                options.preRequest!(options, async (err?: Error | null) => {
                    if (err) {
                        log.debug(err);
                        return this._handler(err, options);
                    }
                    return await request();
                });
            } catch (err) {
                log.error(err);
                throw err;
            }
        } else {
            return await request();
        }
    };

    private _handler = (error: unknown, options: RequestOptions, response?: CrawlerResponse): CrawlerResponse => {
        const revisionContext = this._revisionContexts.get(options as CrawlerOptions);
        if (revisionContext) {
            return this._revisionHandler(revisionContext, error, options, response);
        }
        if (error) {
            if (options.retries && options.retries > 0) {
                log.warn(`${error} occurred on ${options.url}. ${options.retries ? `(${options.retries} retries left)` : ""}`);
                setTimeout(() => {
                    options.retries!--;
                    this._execute(options as CrawlerOptions);
                }, options.retryInterval);
                return;
            } else {
                log.error(`${error} occurred on ${options.url}. Request failed.`);
                if (options.callback && typeof options.callback === "function") {
                    return options.callback(error, { options }, options.release);
                } else {
                    throw error;
                }
            }
        }
        response = this._processResponse(options, response as CrawlerResponse);
        if (options.callback && typeof options.callback === "function") {
            return options.callback(null, response, options.release);
        }
        return response;
    };

    /**
     * Body normalization shared by the legacy queue path and the revision
     * path: charset sniffing, iconv decoding, JSON parsing and Cheerio load.
     */
    private _processResponse = (options: RequestOptions, response: CrawlerResponse): CrawlerResponse => {
        if (!response.body) response.body = "";
        // got v15 returns the body as a Uint8Array; normalize to Buffer so
        // charset sniffing and iconv decoding behave as before.
        else if (!Buffer.isBuffer(response.body)) response.body = Buffer.from(response.body as Uint8Array);
        log.debug("Got " + (options.url || "html") + " (" + response.body.length + " bytes)...");
        response.options = options;

        response.charset = getCharset(response.headers);
        if (!response.charset) {
            const match = response.body.toString().match(/charset=['"]?([\w.-]+)/i);
            response.charset = match ? match[1].trim().toLowerCase() : null;
        }
        log.debug("Charset: " + response.charset);

        if (options.encoding !== null) {
            options.encoding = options.encoding ?? response.charset ?? "utf8";
            try {
                if (!Buffer.isBuffer(response.body)) response.body = Buffer.from(response.body);
                response.body = iconv.decode(response.body, options.encoding as string);
                response.body = response.body.toString();
            } catch (err) {
                log.error(err);
            }
        }

        if (options.isJson) {
            try {
                response.body = JSON.parse(response.body);
            } catch (_err) {
                log.warn("JSON parsing failed, body is not JSON. Set isJson to false to mute this warning.");
            }
        }

        if (options.jQuery === true && !options.isJson) {
            if (response.body === "" || !this._detectHtmlOnHeaders(response.headers)) {
                log.warn("response body is not HTML, skip injecting. Set jQuery to false to mute this warning.");
            } else {
                try {
                    response.$ = load(response.body);
                } catch (_err) {
                    log.warn("HTML detected failed. Set jQuery to false to mute this warning.");
                }
            }
        }
        return response;
    };

    public get queueSize(): number {
        return 0;
    }

    /**
     * @param rateLimiterId
     * @param property
     * @param value
     * @description Set the rate limiter property.
     * @version 2.0.0 Only support `rateLimit` change.
     * @example
     * ```js
     * const crawler = new Crawler();
     * crawler.setLimiter(0, "rateLimit", 1000);
     * ```
     */
    public setLimiter(rateLimiterId: number, property: string, value: unknown): void {
        if (!isNumber(rateLimiterId)) {
            log.error("rateLimiterId must be a number");
            return;
        }
        if (property === "rateLimit") {
            this._limiters.getRateLimiter(rateLimiterId).setRateLimit(value as number);
        }
        // @todo other properties
    }
    /**
     * @param options
     * @returns if there is a "callback" function in the options, return the result of the callback function. \
     * Otherwise, return a promise, which resolves when the request is successful and rejects when the request fails.
     * In the case of the promise, the resolved value will be the response object.
     * @description Send a request directly.
     * @example
     * ```js
     * const crawler = new Crawler();
     * crawler.send({
     *      url: "https://example.com",
     *      callback: (error, response, done) => { done(); }
     * });
     * await crawler.send("https://example.com");
     * ```
     */
    public send = async (options: RequestConfig): Promise<CrawlerResponse> => {
        options = getValidOptions(options);
        options.retries = options.retries ?? 0;
        setDefaults(options, this.options);
        options.skipEventRequest = isBoolean(options.skipEventRequest) ? options.skipEventRequest : true;
        delete options.preRequest;
        return await this._execute(options);
    };
    /**
     * @deprecated
     * @description Old interface version. It is recommended to use `Crawler.send()` instead.
     * @see Crawler.send
     */
    public direct = async (options: RequestConfig): Promise<CrawlerResponse> => {
        return await this.send(options);
    };
    /**
     * @param options
     * @description Add a request to the queue.
     * @example
     * ```js
     * const crawler = new Crawler();
     * crawler.add({
     *     url: "https://example.com",
     *     callback: (error, response, done) => { done(); }
     * });
     * ```
     */
    public add = (options: RequestConfig): void => {
        let optionsArray = Array.isArray(options) ? options : [options];
        optionsArray = flattenDeep(optionsArray);
        optionsArray.forEach(options => {
            try {
                options = getValidOptions(options) as RequestOptions;
            } catch (err) {
                log.warn(err);
                return;
            }
            setDefaults(options, this.options);
            options.headers = { ...this.options.headers, ...options.headers };
            if (!this.options.skipDuplicates) {
                this._schedule(options as CrawlerOptions);
                return;
            }

            this.seen
                .exists(options, options.seenreq)
                .then((rst: any) => {
                    if (!rst) {
                        this._schedule(options as CrawlerOptions);
                    }
                })
                .catch((error: unknown) => log.error(error));
        });
    };
    /**
     * @deprecated
     * @description Old interface version. It is recommended to use `Crawler.add()` instead.
     * @see Crawler.add
     */
    public queue = (options: RequestConfig): void => {
        return this.add(options);
    };

    /**
     * The largest revision already accepted for each business key. Kept for
     * the lifetime of the instance, independently of the active state below,
     * so the high-water mark never moves backwards even after a key drains.
     */
    private _revisionWaterMarks: Map<string, number> = new Map();

    /**
     * Merge repeated notifications for the same business key and only fetch
     * the newest revision. Identity is the `key`, so the seenreq URL
     * de-duplication used by `add()` is bypassed. The request still goes
     * through the shared request pool, fetch, decode and callback pipeline.
     *
     * @param key Business key, e.g. the product id. Must be a non-empty string.
     * @param revision Monotonic revision number. Must be a non-negative safe integer.
     * @param options Plain HTTP GET options; `url` (or `uri`) must be a string.
     * @returns `true` when the revision is newer than the high-water mark and
     * was accepted; `false` when it is equal or older, in which case no
     * request is sent and no callback runs.
     * @throws {TypeError} When any argument is invalid. Existing work is never
     * touched by a validation failure.
     * @example
     * ```js
     * const crawler = new Crawler();
     * // Full scan and incremental notifications may race each other; only the
     * // newest revision of each product is actually downloaded.
     * crawler.addRevision("product-42", 1, {
     *     url: "https://example.com/products/42",
     *     callback: (error, response, done) => { done(); },
     * });
     * crawler.addRevision("product-42", 2, {
     *     url: "https://example.com/products/42",
     *     headers: { "x-revision": "2" },
     *     callback: (error, response, done) => { done(); },
     * });
     * ```
     */
    public addRevision = (key: string, revision: number, options: RevisionRequestOptions): boolean => {
        this._revisionValidate(key, revision, options);
        const acceptedRevision = this._revisionWaterMarks.get(key);
        if (acceptedRevision !== undefined && revision <= acceptedRevision) {
            return false;
        }
        this._revisionWaterMarks.set(key, revision);

        const config = this._revisionBuildOptions(options);
        let state = this._revisionStates.get(key);
        if (!state) {
            state = {
                key,
                phase: "idle",
                pending: null,
                current: null,
                task: null,
                limiterDone: null,
                retryTimer: null,
                retryGeneration: 0,
                callbackDone: false,
            };
            this._revisionStates.set(key, state);
        }

        switch (state.phase) {
            case "idle":
                this._revisionSubmit(state, config);
                break;
            case "queued":
                // Not dispatched to the network yet: swap the waiting config
                // instead of taking another queue position.
                this._limiters.cancel(state.task as (done: () => void) => void);
                this._revisionSubmit(state, config);
                break;
            case "retry":
                // The old attempt is finished but its backoff still holds the
                // slot: invalidate the backoff and advance immediately.
                state.pending = config;
                this._revisionCancelRetry(state);
                this._revisionAdvance(state);
                break;
            case "downloading":
            case "callback":
            default:
                // One download/callback is already bound to the slot; keep
                // only the newest config and dispatch it once the slot frees.
                state.pending = config;
                break;
        }
        return true;
    };

    private _revisionValidate(key: unknown, revision: unknown, options: unknown): asserts options is RevisionRequestOptions {
        if (typeof key !== "string" || key.length === 0) {
            throw new TypeError("addRevision: key must be a non-empty string");
        }
        if (typeof revision !== "number" || !Number.isSafeInteger(revision) || revision < 0) {
            throw new TypeError("addRevision: revision must be a non-negative safe integer");
        }
        if (options === null || typeof options !== "object" || Array.isArray(options)) {
            throw new TypeError("addRevision: options must be a single plain request options object");
        }
        const prototype = Object.getPrototypeOf(options);
        if (prototype !== Object.prototype && prototype !== null) {
            throw new TypeError("addRevision: options must be a single plain request options object");
        }
        const opts = options as Record<string, unknown>;
        if ("html" in opts) {
            throw new TypeError("addRevision: inline html is not supported");
        }
        if ("preRequest" in opts) {
            throw new TypeError("addRevision: preRequest is not supported");
        }
        if (opts.method !== undefined && opts.method !== "GET") {
            throw new TypeError("addRevision: only plain HTTP GET requests are supported");
        }
        if (opts.url !== undefined && typeof opts.url !== "string") {
            throw new TypeError("addRevision: options.url must be a string");
        }
        if (opts.uri !== undefined && typeof opts.uri !== "string") {
            throw new TypeError("addRevision: options.uri must be a string");
        }
        const url = opts.url !== undefined ? opts.url : opts.uri;
        if (typeof url !== "string" || url.length === 0) {
            throw new TypeError("addRevision: a string url or uri is required");
        }
    }

    /**
     * Copies the accepted options at reception time so later mutations of the
     * caller's object cannot change the request that will be sent. Plain
     * nested values (headers, searchParams, ...) are cloned recursively;
     * class instances such as cookie jars keep their reference.
     */
    private _revisionClone = (value: unknown): unknown => {
        if (Array.isArray(value)) {
            return value.map(item => this._revisionClone(item));
        }
        if (value !== null && typeof value === "object") {
            const prototype = Object.getPrototypeOf(value);
            if (prototype === Object.prototype || prototype === null) {
                const clone: Record<string, unknown> = {};
                Object.keys(value as Record<string, unknown>).forEach(k => {
                    clone[k] = this._revisionClone((value as Record<string, unknown>)[k]);
                });
                return clone;
            }
        }
        return value;
    };

    private _revisionBuildOptions = (options: RevisionRequestOptions): CrawlerOptions => {
        const config = this._revisionClone(options) as CrawlerOptions;
        config.method = "GET";
        if (config.url === undefined && config.uri !== undefined) {
            config.url = config.uri;
        }
        delete config.uri;
        setDefaults(config, this.options);
        config.headers = { ...this.options.headers, ...config.headers };
        return config;
    };

    /**
     * Hand a config to the shared request pool. The pool slot is reserved
     * here, so the key counts as busy from submission until user `done`.
     */
    private _revisionSubmit = (state: RevisionState, config: CrawlerOptions): void => {
        if (state.current) {
            this._revisionContexts.delete(state.current);
        }
        state.current = config;
        state.pending = null;
        state.callbackDone = false;
        state.phase = "queued";
        this._revisionContexts.set(config, state);

        const task = (done: () => void): void => {
            state.limiterDone = done;
            state.phase = "downloading";
            config.release = () => this._revisionUserDone(state);
            config.callback = config.callback || config.release;
            delete config.uri;
            this._execute(config);
        };
        state.task = task;
        this._limiters
            .getRateLimiter(config.rateLimiterId)
            .submit(config.priority as number, task);
    };

    /**
     * Route the result of a live revision attempt. A superseded attempt is
     * swallowed entirely: no legacy callback, no retries of the old revision.
     */
    private _revisionHandler = (state: RevisionState, error: unknown, options: RequestOptions, response?: CrawlerResponse): CrawlerResponse => {
        // Zombie retry timer: the attempt was superseded while backing off.
        if (state.current !== options) {
            return;
        }
        if (state.pending) {
            // The attempt was already replaced before it reached the user
            // callback. Drop both success and error, including old retries,
            // and dispatch the newest revision through the normal pool.
            this._revisionAdvance(state);
            return;
        }
        if (error) {
            if (options.retries && options.retries > 0) {
                log.warn(`${error} occurred on ${options.url}. ${options.retries ? `(${options.retries} retries left)` : ""}`);
                state.phase = "retry";
                const generation = ++state.retryGeneration;
                state.retryTimer = setTimeout(() => {
                    state.retryTimer = null;
                    if (generation !== state.retryGeneration || state.current !== options) return;
                    options.retries!--;
                    state.phase = "downloading";
                    this._execute(options as CrawlerOptions);
                }, options.retryInterval);
                return;
            }
            log.error(`${error} occurred on ${options.url}. Request failed.`);
            state.phase = "callback";
            return options.callback!(error, { options }, options.release);
        }
        state.phase = "callback";
        const processed = this._processResponse(options, response as CrawlerResponse);
        return options.callback!(null, processed, options.release);
    };

    private _revisionCancelRetry = (state: RevisionState): void => {
        if (state.retryTimer) {
            clearTimeout(state.retryTimer);
            state.retryTimer = null;
        }
        // Bump the generation so a timer that was already queued by the event
        // loop can never dispatch the stale revision afterwards.
        state.retryGeneration++;
    };

    private _revisionReleaseSlot = (state: RevisionState): void => {
        if (state.limiterDone) {
            const done = state.limiterDone;
            state.limiterDone = null;
            done();
            this.emit("_release");
        }
    };

    /**
     * Send the newest pending config via the pool, reusing the slot the
     * previous attempt has just released. Called when an attempt ends
     * superseded, or when a retry backoff is invalidated by an update.
     */
    private _revisionAdvance = (state: RevisionState): void => {
        const next = state.pending;
        if (!next) {
            this._revisionReleaseSlot(state);
            this._revisionTeardown(state);
            this._revisionMaybeDrain();
            return;
        }
        this._revisionReleaseSlot(state);
        this._revisionSubmit(state, next);
    };

    /**
     * User-side `done`. Settles exactly once: repeated calls neither release
     * the pool slot twice nor dispatch the successor twice. Updates that
     * arrived during the callback are sent afterwards, newest one only.
     */
    private _revisionUserDone = (state: RevisionState): void => {
        if (state.callbackDone || state.phase !== "callback") {
            return;
        }
        state.callbackDone = true;
        if (state.pending) {
            const next = state.pending;
            this._revisionReleaseSlot(state);
            this._revisionSubmit(state, next);
            return;
        }
        this._revisionReleaseSlot(state);
        this._revisionTeardown(state);
        this._revisionMaybeDrain();
    };

    private _revisionTeardown = (state: RevisionState): void => {
        if (state.current) {
            this._revisionContexts.delete(state.current);
        }
        state.current = null;
        state.task = null;
        state.pending = null;
        state.retryTimer = null;
        this._revisionStates.delete(state.key);
    };

    private _revisionMaybeDrain = (): void => {
        if (this._limiters.empty && this._revisionStates.size === 0) {
            this.emit("drain");
        }
    };
}

export default Crawler;