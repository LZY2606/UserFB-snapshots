# Wire-text acquisition matrix

Regression contract for the chain an HTTP response travels before user code
sees it: got decompresses the entity, the crawler picks a charset, iconv-lite
turns bytes into text, and only then does cheerio (DOM) or `JSON.parse`
materialize the result. The tests live in `test/wire-text.test.mjs` (outer
test name `wire acquisition contract`, Node's built-in runner) and exercise
the real built `Crawler` through its public `send` and `add` entries only.

## Where the bytes come from

Every sample body is written out in the test file as a **literal byte
array**, assembled with ASCII skeletons via `Buffer.concat`. No sample byte
was produced by running the charset converter under test (iconv-lite); the
arrays were cross-checked during development against Node's built-in WHATWG
`TextDecoder`, which is an independent implementation:

| Sample | Literal bytes | Expected Unicode text |
| --- | --- | --- |
| UTF-8 | `E6 B1 89 E5 AD 97 65 CC 81` | `汉字` + `e` + combining acute U+0301 |
| Latin-1 | `4A F6 72 67` / `63 61 66 E9` | `Jörg` / `café` (1 byte = 1 code point) |
| Shift-JIS | `93 FA 96 7B 8C EA` | `日本語` (日=93FA 本=967B 語=8CEA) |
| JSON | ASCII skeleton + the UTF-8 bytes above | `{"name":"汉字","nested":{"label":"汉字é","count":3,"ok":true}}` |

The Latin-1 bytes `F6`/`E9` are deliberately outside ASCII: a decode-as-UTF-8
then re-encode round trip would not reproduce them, so byte equality in the
`encoding:null` checks proves no hidden text stage ran.

gzip variants are produced at serve time with `zlib.gzipSync` from those same
bytes; the gzip header (first 10 bytes) and the remaining body are written in
several **unequal** `res.write` chunks. All comparisons happen against the
decompressed entity, never against the compressed archive.

## How declarations and options determine the expectations

Selection rules under test (in priority order):

1. Explicit `encoding` option wins over any detection.
2. `forceUTF8` / `isJson` force UTF-8 decoding regardless of declarations,
   but `response.charset` still reports the *detected* declaration.
3. Otherwise the HTTP `Content-Type` charset is used; an HTML `<meta>`
   charset is only consulted when the HTTP header carries no charset.
4. With no declaration at all, decoding falls back to UTF-8.
5. `encoding: null` skips text decoding entirely and returns the raw entity
   bytes as a `Buffer`.

| Route | Wire bytes | Response headers | Request options | Expected result |
| --- | --- | --- | --- | --- |
| `/utf8`, `/utf8.gz` | UTF-8 page, `<meta charset="utf-8">` | `text/html;charset=utf-8` | — | `charset='utf-8'`; `data-name` and `<p>` text = `汉字é`; body is string |
| `/latin1`, `/latin1.gz` | Latin-1 page, no `<meta>` | `text/html;charset=iso-8859-1` | — | `charset='iso-8859-1'`; `data-name` = `Jörg`, text = `Jörg café` (header-only declaration) |
| `/shiftjis`, `/shiftjis.gz` | Shift-JIS page, no `<meta>` | `text/html;charset=shift_jis` | — | `charset='shift_jis'`; attribute and text = `日本語` |
| `/json`, `/json.gz` | UTF-8 JSON | `application/json;charset=utf-8` | `isJson: true` | parsed object: `name='汉字'`, `nested.label='汉字é'`, `nested.count=3`, `nested.ok=true`; no `$` handle |
| `/latin1-meta` | Latin-1 page with `<meta charset="iso-8859-1">` | `text/html` (no charset) | — | meta fallback: `charset='iso-8859-1'`, `Jörg` decoded correctly |
| `/conflict` | UTF-8 bytes, `<meta charset="iso-8859-1">` | `text/html;charset=utf-8` | — | HTTP beats meta: `charset='utf-8'`, text = `汉字é` |
| `/mislabeled` | Latin-1 bytes, no `<meta>` | `text/html;charset=utf-8` (lies) | `encoding: 'iso-8859-1'` | explicit option beats detection: text = `Jörg café`; `charset` still reports `'utf-8'` |
| `/shiftjis-plain` | Shift-JIS bytes, no `<meta>` | `text/html` | `encoding: 'shift_jis'` | explicit option with zero declarations: text = `日本語`; `charset=null` |
| `/utf8-mislabeled` | UTF-8 bytes, no `<meta>` | `text/html;charset=iso-8859-1` (lies) | `forceUTF8: true` | forced UTF-8 decode: text = `汉字é`; `charset` still reports `'iso-8859-1'` |
| `/json-latin1-header` | UTF-8 JSON | `application/json;charset=iso-8859-1` (lies) | `isJson: true` | forced UTF-8 decode, parsed JSON correct; `charset` still `'iso-8859-1'`; no `$` |
| `/latin1`, `/latin1.gz` | Latin-1 page | `text/html;charset=iso-8859-1` | `encoding: null`, `jQuery: false` | `body` is a `Buffer` byte-for-byte equal to the served entity; no `$` |
| `/gated/*` | per-suffix variants of the above | matching headers | one with `encoding: 'shift_jis'`, two without | server holds all three responses, answers in reverse arrival order; each request still gets its own decoding |
| `/gated/latin1` (4th request) | Latin-1 page | `text/html;charset=iso-8859-1` | none, sent after the gated batch | no state leaks from the earlier explicit `shift_jis` choice: `Jörg café`, `charset='iso-8859-1'` |
| `/latin1-raw` | Latin-1 bytes, no `<meta>` | `text/html` | instance default `encoding: 'iso-8859-1'` | instance A decodes `Jörg café`; instance B (`forceUTF8`) cannot |
| `/utf8-mislabeled` | UTF-8 bytes | `text/html;charset=iso-8859-1` | instance default `forceUTF8: true` | instance B decodes `汉字é`; instance A cannot — defaults stay per-instance |

Every route is fetched through **both** public entries (`send` and `add`)
unless the table says otherwise; `add` callbacks release their task and the
crawler's `drain` event is awaited, so no request is left hanging. The server
records the path of every hit, and the final subtest compares the full
per-path hit counts against the expected plan.

## Degradation harness (`test/run-wire-text.mjs`)

The runner first executes the regression against the unmodified tree
(`WIRE_TEXT_BASE_OK`), then rebuilds three private source copies, each with
exactly one degradation, runs an ASCII HTTP smoke test on the degraded build,
and finally runs the same regression file against it:

| Degradation | Patched location | Expected failing expectations | Marker |
| --- | --- | --- | --- |
| HTTP charset no longer participates | `getCharset` in `src/options.ts` | header-only charsets (`/latin1`, `/shiftjis`) mis-decode; `charset` field loses the declared value | `WIRE_CHARSET_CAUGHT` |
| got reads the response as text | `responseType` in `alignOptions` (`src/options.ts`) | non-UTF-8 bodies arrive pre-decoded; `encoding:null` no longer yields a `Buffer` | `WIRE_BYTES_CAUGHT` |
| `encoding:null` enters the text branch | decode guard in `src/crawler.ts` | binary passthrough returns a string instead of the exact bytes | `WIRE_BINARY_CAUGHT` |

A degradation only counts as caught when named test cases fail with assertion
mismatches; build errors, import errors, timeouts, or a zero-test run are
reported as infrastructure failures and abort the run instead.
