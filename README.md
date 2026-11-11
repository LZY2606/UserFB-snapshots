httpcache
=========

[![Build Status](https://travis-ci.org/gregjones/httpcache.svg?branch=master)](https://travis-ci.org/gregjones/httpcache) [![GoDoc](https://godoc.org/github.com/gregjones/httpcache?status.svg)](https://godoc.org/github.com/gregjones/httpcache)

Package httpcache provides a http.RoundTripper implementation that works as a mostly [RFC 7234](https://tools.ietf.org/html/rfc7234) compliant cache for http responses.

It is only suitable for use as a 'private' cache (i.e. for a web-browser or an API-client and not for a shared proxy).

This project isn't actively maintained; it works for what I, and seemingly others, want to do with it, and I consider it "done". That said, if you find any issues, please open a Pull Request and I will try to review it. Any changes now that change the public API won't be considered.

Caller-supplied conditional requests
-------------------------------------

If a request already carries an `If-None-Match` or `If-Modified-Since` header (header
names are matched case-insensitively, and a present field with an empty value still
counts as supplied), that condition belongs to the caller, not to the cache:

- The header is forwarded to the origin exactly as given. Weak ETags, multiple tags,
  repeated header lines and both validators present at once are all passed through
  byte-for-byte; the request and its header maps are never modified.
- A fresh cached entry is never served in place of such a request, and the cache never
  adds, replaces or fills in validators.
- An origin `304 Not Modified` is returned to the caller verbatim: same 304 status,
  empty body, no `X-From-Cache` header, and nothing is written, deleted or refreshed in
  the cache. The cache does not interpret a caller-conditional 304 — the caller does.
- Network errors and 5xx responses are likewise handed back as-is and never trigger a
  `stale-if-error` fallback.
- A full `200` response to a conditional request is still cached, subject to the
  commit rules below.

The cache's own automatic revalidation is separate. When the Transport itself attaches
validators for a stale entry, an origin 304 is merged with the exact cached snapshot
that generated those conditional headers to produce a 200; a 304 produced while the
request carried caller conditions, mismatched `Vary`, no validator, or a `no-cache`
request is never merged with a local entity.

Concurrent updates, cache state and response bodies
----------------------------------------------------

Within a single `Transport` instance, every GET/HEAD is treated as an update attempt
based on the cache state observed immediately before the request was sent. Each cache
key has a generation counter, and every successful `Set` or `Delete` advances it — even
when the new bytes are identical to the old ones (a delete followed by re-writing the
same content is still a new state).

- A `200` (or an automatically revalidated 304) candidate only reaches the cache after
  its response body is read to EOF, and only while the generation seen at departure is
  still current. When several requests start from the same state, the first valid commit
  wins; the others still deliver their complete responses to their callers but can no
  longer write or delete the key.
- Closing a body before EOF, reading a non-EOF error, or having the request context
  cancelled forfeits the commit, so an earlier, slower reader can never restore an entry
  that has since been updated or deleted. Repeated EOF reads and repeated `Close` calls
  never commit twice.
- `no-store` on the request or response deletes the key as soon as the response headers
  arrive, under the same state rule; deleting a missing key still creates a new state.
- `stale-if-error` keeps working for ordinary requests, but the fallback never writes
  the cache, and a snapshot that has already been replaced or invalidated is not reused.

This coordination is scoped to one `Transport` instance and covers only the GET/HEAD
updates that instance initiates. It does not coordinate direct external calls to the
`Cache` or two different `Transport` instances sharing a backend. Backends keep the
plain `Get`/`Set`/`Delete` interface — no CAS or transaction support is required.

The per-key coordination entry is created on demand when a request begins and removed as
soon as the last in-flight request for that key, including any still-open response
`Body`, has finished. No locks, generations or tombstones are retained for URLs that are
no longer in flight.

Response bodies are streamed to the caller: the Transport neither reads ahead nor spawns
a background reader to drain a body. The caller is responsible for reading to EOF and/or
calling `Close`; doing so releases the coordination state for that request. Another
request for the same key can still reach the origin meanwhile, and an open body for one
key never blocks traffic on other keys.

Note the distinction this creates: a request already in flight may legitimately receive
an older snapshot while a newer state ends up committed in the cache. The response a
caller already holds is not retroactively swapped, while the cache converges to the most
recent valid state.

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

License
-------

-	[MIT License](LICENSE.txt)
