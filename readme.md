# decompress-response

> Decompress a HTTP response if needed

Decompresses the [response](https://nodejs.org/api/http.html#http_class_http_incomingmessage) from [`http.request`](https://nodejs.org/api/http.html#http_http_request_options_callback) if it's gzipped, deflated or compressed with Brotli, otherwise just passes it through.

Used by [`got`](https://github.com/sindresorhus/got).

## Install

```sh
npm install decompress-response
```

## Usage

```js
import http from 'node:http';
import decompressResponse from 'decompress-response';

http.get('https://sindresorhus.com', response => {
	response = decompressResponse(response);
});
```

## API

### decompressResponse(response)

Returns the decompressed HTTP response stream, or the original response if it cannot be decompressed.

#### response

Type: [`http.IncomingMessage`](https://nodejs.org/api/http.html#http_class_http_incomingmessage)

The HTTP incoming stream with compressed data.

## Content-Encoding chains

The `content-encoding` header may list multiple codings, for example `gzip, br`. Codings are listed in the order they were applied, so they are decoded in reverse order. Coding names are case-insensitive and may be padded with spaces or tabs.

Supported codings: `gzip`, `deflate` (both zlib-wrapped and raw inputs are detected per layer), `br`, `identity` (a no-op), and `zstd` when the Node.js runtime supports it. Codings may be combined and repeated.

The whole chain is validated up front. If it contains an unknown, empty, or unsupported coding — or consists only of `identity` — the original response is returned as-is: no bytes are consumed and no decoding listeners are attached.

When a chain is decoded, the returned stream's `headers` no longer contain `content-encoding` and `content-length`. The original response's `headers`, `rawHeaders`, and custom properties are left untouched, and accessors like `statusCode`, `socket`, and `trailers` keep working.

If any layer fails to decode, the returned stream emits a single `error`, the source response is destroyed, and all decoding layers are torn down.
