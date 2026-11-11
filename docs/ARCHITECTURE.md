# OpenSERP Architecture

## Overview

OpenSERP is a Go API + CLI for search result extraction from Google, Yandex, Baidu, Bing, DuckDuckGo, and Ecosia.

Execution modes:

- **Browser mode**: default path, headless Chromium via `go-rod`, supported by all engines.
- **Raw HTTP mode**: direct HTTP + `goquery`, currently supported by Google, Yandex, Baidu, and Ecosia.

Browser mode is the primary compatibility path.

## Project Layout

```text
openserp/
├── main.go
├── README.md
├── config.yaml
├── docs/
│   ├── ARCHITECTURE.md
│   ├── CONTRIBUTING.md
│   ├── openapi.yaml
│   └── embed.go
├── cmd/
│   ├── root.go
│   ├── serve.go
│   ├── search.go
│   └── proxy_policy.go
├── core/
│   ├── common.go
│   ├── server.go
│   ├── response.go
│   ├── result.go
│   ├── response_builder.go
│   ├── clusters.go
│   ├── format_markdown.go
│   ├── format_text.go
│   ├── enrichment_domain.go
│   ├── enrichment_domains.yaml
│   ├── middleware.go
│   ├── browser.go
│   ├── http_client.go
│   ├── resilient.go
│   ├── retry.go
│   ├── circuit_breaker.go
│   ├── cache.go
│   ├── proxy.go
│   ├── logger.go
│   └── captcha.go
├── google/
├── yandex/
├── baidu/
├── bing/
├── duckduckgo/
├── ecosia/
└── testutil/
```

## Core Interfaces

### `core.SearchEngine`

All engines implement:

- `Search(context.Context, Query) ([]SearchResult, error)`
- `SearchImage(context.Context, Query) ([]SearchResult, error)`
- `IsInitialized() bool`
- `Name() string`
- `GetRateLimiter() *rate.Limiter`

### `core.Query`

Parsed from query parameters (`text`, `lang`, `region`, `date`, `file`, `site`, `limit`, `start`, `filter`, `features`) and the `X-Use-Proxy` request header. At least one of `text`, `site`, or `file` must be non-empty.

### Internal `core.SearchResult`

Engine parsers return the older internal shape:

- `Rank`
- `URL`
- `Title`
- `Description`
- `Ad`

HTTP handlers convert this into the public v2 response through `core/response_builder.go`.

## HTTP Request Flow

```text
HTTP request
  -> Fiber middleware
     -> RequestContextMiddleware
     -> CORS
     -> RequestLoggerMiddleware
  -> handleDedicatedEndpoint / handleMegaEndpoint
  -> Query.InitFromContext
  -> resolveFormat
  -> cache lookup for JSON responses only
  -> ResilientSearcher
     -> circuit breaker
     -> rate limiter
     -> proxy policy resolution
     -> retry loop
     -> engine.Search / engine.SearchImage
        -> browser path: Browser.Navigate -> DOM parse -> []SearchResult
        -> raw path: HTTP client -> goquery parse -> []SearchResult
  -> response enrichment
     -> stable IDs
     -> normalized URL/display URL
     -> pagination position
     -> domain_info/classification
     -> image metadata extraction
  -> mega-only normalized URL dedupe + clusters
  -> cache write for eligible JSON responses
  -> output serializer: JSON, Markdown, text, or NDJSON
```

## Public API Response

JSON endpoints return a v2 envelope.

Top-level fields:

- `query`: request echo, including `engines_requested`
- `meta`: `request_id`, `requested_at`, `took_ms`, `engines_failed`, `version`
- `results`: normalized web or image results
- `pagination`: `page`, `has_more`, `next_start`
- `clusters`: only on `/mega/search`

Stable ID prefixes:

- `s_`: web search result
- `i_`: image result
- `c_`: mega search URL cluster

`meta.engines_failed` is the only engine status list in the body. Clients can derive responded engines as:

```text
query.engines_requested - meta.engines_failed
```

Dedicated endpoint fallback is represented by:

- `X-Fallback-Engine`
- `results[].engine`
- `meta.engines_failed` containing the primary engine

## Mega Search

`/mega/search` and `/mega/image` run selected engines in parallel.

`/mega/search` behavior:

- Uses `engines` query parameter if provided; otherwise uses all configured engines.
- Skips duplicate engine names.
- Allows partial success; failed engines are listed in `meta.engines_failed`.
- Deduplicates flat results by normalized URL.
- Builds `clusters` from all enriched results before flat dedupe.
- Sorts clusters by score descending, then best rank ascending.

Cluster score:

```text
sum(1 / rank for each occurrence) / engines_queried
```

The score is capped at `1.0` and rounded to two decimals.

## Response Formatting

`resolveFormat` supports:

- `json` (default)
- `markdown`
- `text`
- `ndjson`

The format can be selected with `?format=` or by `Accept` header:

- `text/markdown`
- `text/plain`
- `application/x-ndjson`

Only JSON responses use the response cache. Cached JSON refreshes request-scoped metadata before sending:

- `meta.request_id`
- `meta.requested_at`
- `meta.took_ms`

## Domain Enrichment

`core/enrichment_domain.go` derives:

- `domain_info`: public suffix, SLD, and collapsed category
- `classification`: content type and known source hint

Public suffix parsing uses `golang.org/x/net/publicsuffix`.

Mutable domain category data lives in:

```text
core/enrichment_domains.yaml
```

It can be replaced at runtime:

```bash
OPENSERP_ENRICHMENT_DOMAINS_FILE=/path/to/enrichment_domains.yaml ./openserp serve
```

## Resilience Stack

Request protection sequence:

1. Engine rate limiter
2. Retry with backoff
3. Circuit breaker
4. Proxy policy and proxy health
5. Response cache

Important behaviors:

- `ErrCaptcha` is non-retryable.
- Proxy health is degraded only for proxy/network failures, not parser or captcha errors.
- Dedicated endpoints are engine-pure by default.
- Dedicated fallback is opt-in via `resilience.allow_endpoint_fallback`.
- Fallback responses are not cached on dedicated endpoints.

## Proxy Model

Proxy policy can come from:

- global config
- per-engine config
- per-request `X-Use-Proxy`

Supported request override values:

- `X-Use-Proxy: direct`
- `X-Use-Proxy: <tag>`

Response headers:

- `X-Proxy-Mode`: `off` or `tag_pool`
- `X-Proxy-Tag`
- `X-Proxy-Used`

### Sticky lane cookie persistence

When proxy lanes are enabled, every collection runs in its own isolated
browser context while sharing one logical session per
`tenant + engine + proxy session id`. The persistence timing and merge rules
are:

1. When a page starts on a lane (`Browser.Navigate`), it receives its own
   deep copy of the lane's cookie set, which is restored into the new
   isolated context. The page does not publish any cookies while running.
2. Cookies are only saved when the page finishes using the tab:
   `DeferClosePage`, `Browser.ClosePage` and `ClosePageWithTimeout` read the
   **complete** browser-context cookie set (no URL filter, so redirect,
   other-path and other-domain cookies are never mistaken for deletions)
   before the context is destroyed. Saving at close time instead of right
   after navigation means cookies produced by in-tab work, such as Bing's
   search-box submission and pagination, still reach the next collection.
3. Only that page's own delta is merged: entries it added, entries whose value
   or restorable attributes changed, and entries it deleted. Cookies are
   distinguished by name, domain, path and partition key. Read-only CDP
   statistics such as `size` are not treated as page edits. Unchanged
   inherited values therefore never overwrite another page's newer value or
   revive a cookie another page deleted. When two pages edit the same cookie,
   the last successful commit wins.
4. A `DropCookies` (captcha challenge) or a full `SaveCookies` replace bumps
   the lane generation, so pages already in flight lose their write-back
   qualification even if their cookie read returns late; an LRU eviction
   followed by recreation of the same key likewise rejects old instances
   without keeping permanent tombstones. A late reply never creates a lane,
   updates a new instance, or displaces another lane.
5. `LeavePageOpen` keeps the tab for inspection but still settles the session
   snapshot at `DeferClosePage`; later manual operations are no longer part of
   the finished snapshot. A canceled request, a failed cookie read, a failed
   page initialization, or browser shutdown discards the page's edits and
   releases its registration while the bounded page/context cleanup still
   runs.

## Config Reference

Config priority: `CLI flags > OPENSERP_* env vars > config.yaml > defaults` (via Viper).

See [config.yaml](../config.yaml) for all available sections and defaults.
