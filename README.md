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
- [`sourcegraph.com/sourcegraph/s3cache`](https://github.com/sourcegraph/s3cache) uses Amazon S3 for storage.
- [`github.com/gregjones/httpcache/leveldbcache`](https://github.com/gregjones/httpcache/tree/master/leveldbcache) provides a filesystem-backed cache using [leveldb](https://github.com/syndtr/goleveldb/leveldb).
- [`github.com/die-net/lrucache`](https://github.com/die-net/lrucache) provides an in-memory cache that will evict least-recently used entries.
- [`github.com/die-net/lrucache/twotier`](https://github.com/die-net/lrucache/tree/master/twotier) allows caches to be combined, for example to use lrucache above with a persistent disk-cache.
- [`github.com/birkelund/boltdbcache`](https://github.com/birkelund/boltdbcache) provides a BoltDB implementation (based on the [bbolt](https://github.com/coreos/bbolt) fork).

If you implement any other backend and wish it to be linked here, please send me a PR editing this file.

Multi-representation Vary cache (`VaryVariants`)
------------------------------------------------

By default a URL keeps a single cached response, so alternating requests that
carry different `Vary` selection headers (for example `Accept-Language: en`
followed by `Accept-Language: fr`) keep evicting each other. Setting the
exported `Transport.VaryVariants` boolean to `true` switches that URL's cache
slot to a catalog that stores one representation per distinct request
condition instead of replacing the previous response.

Scope of the mode:

- It only applies to plain `GET` requests without `Range`, `If-None-Match` or
  `If-Modified-Since` headers. Every other request (including `HEAD`, `POST`
  and conditional or range requests) follows the unchanged single-response
  path.
- `Vary` field names are matched case-insensitively, deduplicated, with empty
  tokens ignored and multiple `Vary` header lines interpreted together.
- Matching keeps every actual request header value and its order; only
  surrounding spaces and tabs are trimmed per value. Values are
  case-sensitive, and a missing field is distinct from a field present with an
  explicit empty value.
- A `Vary: *` response is still returned to its caller, but it is never stored
  for reuse by a later request.
- Each representation carries its own ETag, date and `stale-if-error` state.
  A request without a matching representation never borrows another
  representation's body on 5xx errors and never sends its validators.
- A body is published only after it has been fully read to EOF; an early
  `Close` or a read error stores no partial representation.
- A URL keeps a single `Vary` field set at a time. A 200 or validated 304 that
  publishes a different set withdraws every representation of the previous
  set; publications with the same set only replace the representation with the
  same request condition.

Cache sharing boundaries:

- The catalog is persisted through the same `Cache` interface as ordinary
  entries, under the same per-URL key, in a self-describing binary format with
  its own magic prefix. Raw cache bytes can be copied into a brand new `Cache`
  and `Transport` (including another process backed by diskcache) and both
  representations remain selectable.
- The default (`VaryVariants == false`) storage format is unchanged. The new
  mode reads legacy single-response entries and validates their `Vary`
  conditions without migrating the backend; anything the legacy format cannot
  express (notably missing versus empty header values, or multi-valued
  headers) is conservatively treated as a miss and re-fetched.
- `CachedResponse` selects the matching representation for catalog entries and
  returns `nil` when no representation matches, while legacy entries are
  returned exactly as before.

License
-------

-	[MIT License](LICENSE.txt)
