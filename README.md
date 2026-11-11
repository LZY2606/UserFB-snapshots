httpcache
=========

[![Build Status](https://travis-ci.org/gregjones/httpcache.svg?branch=master)](https://travis-ci.org/gregjones/httpcache) [![GoDoc](https://godoc.org/github.com/gregjones/httpcache?status.svg)](https://godoc.org/github.com/gregjones/httpcache)

Package httpcache provides a http.RoundTripper implementation that works as a mostly [RFC 7234](https://tools.ietf.org/html/rfc7234) compliant cache for http responses.

It is only suitable for use as a 'private' cache (i.e. for a web-browser or an API-client and not for a shared proxy).

This project isn't actively maintained; it works for what I, and seemingly others, want to do with it, and I consider it "done". That said, if you find any issues, please open a Pull Request and I will try to review it. Any changes now that change the public API won't be considered.

Cache Backends
--------------

- The built-in 'memory' cache stores responses in an in-memory map.
- [`github.com/gregjones/httpcache/diskcache`](https://github.com/gregjones/httpcache/tree/master/diskcache) provides a filesystem-backed cache using the [diskv](https://github.com/peterbourgon/diskv) library.
- [`github.com/gregjones/httpcache/memcache`](https://github.com/gregjones/httpcache/tree/master/memcache) provides memcache implementations, for both App Engine and 'normal' memcache servers.
- [`sourcegraph.com/sourcegraph/s3cache`](https://sourcegraph.com/github.com/sourcegraph/s3cache) uses Amazon S3 for storage.
- [`github.com/gregjones/httpcache/leveldbcache`](https://github.com/gregjones/httpcache/tree/master/leveldbcache) provides a filesystem-backed cache using [leveldb](https://github.com/syndtr/goleveldb/leveldb).
- [`github.com/die-net/lrucache`](https://github.com/die-net/lrucache) provides an in-memory cache that will evict least-recently used entries.
- [`github.com/die-net/lrucache/twotier`](https://github.com/die-net/lrucache/tree/master/twotier) allows caches to be combined, for example to use lrucache above with a persistent disk-cache.
- [`github.com/birkelund/boltdbcache`](https://github.com/birkelund/boltdbcache) provides a BoltDB implementation (based on the [bbolt](https://github.com/coreos/bbolt) fork).

If you implement any other backend and wish it to be linked here, please send a PR editing this file.

Caller-supplied conditional requests
------------------------------------

The `Transport` distinguishes two kinds of conditional GET/HEAD requests:

1. **Caller-owned conditional requests.** If the request handed to `RoundTrip`
   already contains an `If-None-Match` or `If-Modified-Since` header, the
   condition belongs to the caller. The request is forwarded downstream
   untouched: no cached response is returned directly (whether fresh or
   stale), no validators from the cache are added or substituted, and the
   original `Request` and its header slices are not modified. A `304 Not
   Modified` response is passed back to the caller exactly as received, with
   an empty body and no `X-From-Cache` header. **Interpreting that 304 is the
   caller's responsibility** — the cache does not know whether the caller's
   validator matches the stored entity, so it cannot merge the cached body
   into the answer. The existing cache entry is left unchanged and the 304 is
   not stored on its own. A new `200` response still follows the normal store
   rules (it replaces the old entry once its body is fully consumed, and
   `no-store` remains honored), while transport errors and `5xx` responses are
   returned as-is even when `stale-if-error` would otherwise apply.

2. **Transport-generated revalidation.** Requests without conditional headers
   keep the automatic behavior: when a stale cached entry exists, the
   `Transport` attaches the cached validators itself. If the origin answers
   `304`, the `Transport` merges it with the cached entity and returns a
   `200` (marked with `X-From-Cache` when enabled), refreshing the
   end-to-end response headers.

License
-------

-	[MIT License](LICENSE.txt)
