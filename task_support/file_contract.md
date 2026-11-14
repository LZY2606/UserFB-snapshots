File capture contract

`request.NewRequest(..., "file", ..., "GET", "", ...)` plus
`SetFileTarget(path string, chunkSize int64, workers int) *Request` selects file capture.
The setter records configuration and returns the same Request. `Page.GetFilePath() string`
is nonempty only after successful publication; use the cleaned configured path. In file
mode BodyStr stays empty and no HTML/JSON parsing or character conversion occurs.
Other response types retain their existing behavior. Use the existing request headers,
cookies, proxy and redirect callback without modifying the caller's objects. File requests
must be bodyless GETs, with a configured nonempty path, existing parent directory and
positive chunkSize/workers; caller-supplied Range or If-Range is rejected before I/O.
One caller owns a target path at a time; independent paths do not share state.

Force Accept-Encoding: identity. Probe with Range: bytes=0-0. A resumable nonempty
representation requires 206, one exact Content-Range bytes 0-0/N, one body byte, a strong
ETag and no non-identity Content-Encoding. Record the final response URL (fall back to
the request URL if unavailable). A 200 probe is the full sequential response: consume it
directly into staging. A 206 probe without a strong ETag uses a fresh unconditional full
GET instead. These fallbacks publish only after EOF and length checks, and do not claim
resumability. Empty 200 is a valid empty file. Other probe status or malformed range
metadata fails; a non-identity encoding always fails.

Fixed block i is [i*chunkSize, min((i+1)*chunkSize, N)). Send one single byte Range per
missing block with If-Range equal to the probed strong ETag. At most workers block
responses are active; at least two can be active when workers>=2 and there are two
missing blocks. Probe is not counted as a completed block. Each 206 must have the same
strong ETag, total length, effective response URL, and exact assigned interval. Require
actual body length (including reaching EOF) to equal its span; validate Content-Length
when present. 200 during a block fetch is a representation conflict, never a block.
Close every response. Any protocol/read/write failure makes that Download unsuccessful,
with a useful Errormsg and empty GetFilePath. Stop dispatching new blocks and join started
workers before returning; completed validated blocks may remain for the next invocation.

Staging is `<path>.part`; receipt is `<path>.ranges.json`, UTF-8 JSON with version=1,
url (original URL), effective_url, etag, size, chunk_size, and chunks (array of objects
with index and lowercase hex sha256). Only completed blocks appear; indices are unique
and within the fixed geometry. Persist block bytes before publishing their completion
record, and replace receipts atomically so process interruption cannot leave a half JSON
file. Publication records may never get ahead of durable block data. This is process
restart support, without a power-failure filesystem guarantee. Schema corruption, unknown
version or another original URL is an error before HTTP or modification of those files.

On each new Download, reprobe the source. The same URL with changed ETag, effective URL,
size or requested block size starts new staging without combining representations. If
identity and geometry match, hash each recorded block from .part before trusting it;
missing, short or changed local blocks become missing work, while good blocks are not
redownloaded. .part length alone proves nothing. State without a receipt contains no
trusted completed blocks. A successful fresh process must reuse good records, even if
uncompleted blocks already contain partial bytes from a prior failure.

Only when all blocks have been validated and persisted, close staging and atomically
replace path, then remove receipts. Keep an existing destination unchanged on all earlier
failures. If a process stops after replacement but before receipt cleanup, a later call
may redownload, but must never publish incomplete bytes. Once successful, PageProcesser
and Pipeline see the published file through the normal Spider path. Existing Spider
retry count is unchanged; tests that observe one failed attempt call Download directly.
