import {type IncomingMessage, type IncomingHttpHeaders} from 'node:http';

export type UncompressedIncomingHttpHeaders = {
	'content-encoding'?: never;
} & IncomingHttpHeaders;

export type UncompressedIncomingMessage = {
	headers: UncompressedIncomingHttpHeaders;
} & IncomingMessage;

/**
Decompress a HTTP response if needed.

Supports `content-encoding` chains (for example, `gzip, br`). Codings are decoded in reverse order of how they were applied. If the chain contains an unknown, malformed, or unsupported coding, the original response is returned as-is, still compressed.

@param response - The HTTP incoming stream with compressed data.
@returns The decompressed HTTP response stream, or the original response if it cannot be decompressed.

@example
```
import http from 'node:http';
import decompressResponse from 'decompress-response';

http.get('https://sindresorhus.com', response => {
	response = decompressResponse(response);
});
```
*/
export default function decompressResponse(response: IncomingMessage): UncompressedIncomingMessage | IncomingMessage;
