import test from 'node:test';
import assert from 'node:assert/strict';
import {Buffer} from 'node:buffer';
import {once} from 'node:events';
import http from 'node:http';
import {PassThrough} from 'node:stream';
import zlib from 'node:zlib';
import decompressResponse from '../index.js';

const plain = Buffer.from('{"items":[1,2,3],"message":"Compressible response content."}\n');
const supportsZstd = typeof zlib.zstdCompressSync === 'function';

const tick = () => new Promise(resolve => {
	setImmediate(resolve);
});

const settle = () => new Promise(resolve => {
	setTimeout(resolve, 10);
});

const titleCase = name => name.replaceAll(/(?:^|-)\w/g, character => character.toUpperCase());

// A response stand-in that records its own lifecycle.
class RecordedResponse extends PassThrough {
	constructor(headers = {}) {
		super({autoDestroy: false});

		this.headers = {...headers};
		this.rawHeaders = Object.entries(headers).flatMap(([name, value]) => [titleCase(name), value]);
		this.statusCode = 200;
		this.statusMessage = 'OK';
		this.trailers = {};
		this.rawTrailers = [];
		this.socket = {isFakeSocket: true};
		this.lifecycle = {destroy: 0, error: 0, end: 0};

		const destroy = this.destroy.bind(this);
		this.destroy = error => {
			this.lifecycle.destroy++;
			return destroy(error);
		};

		this.on('error', () => {
			this.lifecycle.error++;
		});

		this.on('end', () => {
			this.lifecycle.end++;
		});
	}
}

// Observes the returned stream: body, ordered end/error events, destroy calls.
const collect = stream => {
	const chunks = [];
	const events = [];
	let destroyCalls = 0;

	const destroy = stream.destroy.bind(stream);
	stream.destroy = error => {
		destroyCalls++;
		return destroy(error);
	};

	stream.on('data', chunk => {
		chunks.push(chunk);
	});

	stream.on('end', () => {
		events.push('end');
	});

	stream.on('error', error => {
		events.push(`error:${error.code ?? error.message}`);
	});

	const terminal = new Promise(resolve => {
		stream.once('end', resolve);
		stream.once('error', resolve);
	});

	return {
		events,
		get destroyCalls() {
			return destroyCalls;
		},
		async settled() {
			await terminal;
			// Let any trailing events surface before asserting.
			await settle();

			return {body: Buffer.concat(chunks), events, destroyCalls};
		},
	};
};

const runChain = async (encoding, encoded, {chunkSize} = {}) => {
	const response = new RecordedResponse({
		'content-encoding': encoding,
		'content-length': String(encoded.length),
	});
	const stream = decompressResponse(response);
	const observer = collect(stream);

	if (chunkSize) {
		for (let offset = 0; offset < encoded.length; offset += chunkSize) {
			response.write(encoded.subarray(offset, offset + chunkSize));
		}

		response.end();
	} else {
		response.end(encoded);
	}

	const result = await observer.settled();
	return {response, stream, ...result};
};

const listenerCounts = stream => ['error', 'close', 'aborted', 'data', 'end', 'readable'].map(event => stream.listenerCount(event));

const errorEvents = events => events.filter(event => event.startsWith('error:'));

test('decodes gzip, br chain in reverse application order', async () => {
	const encoded = zlib.brotliCompressSync(zlib.gzipSync(plain));
	const {response, stream, body, events} = await runChain('gzip, br', encoded);

	assert.deepEqual(body, plain);
	assert.deepEqual(events, ['end']);
	assert.equal(response.lifecycle.destroy, 0);
	assert.equal(response.lifecycle.end, 1);

	// The returned stream's headers are cleaned; the original response is untouched.
	assert.equal(stream.headers['content-encoding'], undefined);
	assert.equal(stream.headers['content-length'], undefined);
	assert.notEqual(stream.headers, response.headers);
	assert.equal(response.headers['content-encoding'], 'gzip, br');
	assert.equal(response.headers['content-length'], String(encoded.length));
	assert.deepEqual(response.rawHeaders, ['Content-Encoding', 'gzip, br', 'Content-Length', String(encoded.length)]);
});

test('decodes br, gzip (opposite order)', async () => {
	const encoded = zlib.gzipSync(zlib.brotliCompressSync(plain));
	const {body, events} = await runChain('br, gzip', encoded);

	assert.deepEqual(body, plain);
	assert.deepEqual(events, ['end']);
});

test('decodes repeated gzip codings', async () => {
	const encoded = zlib.gzipSync(zlib.gzipSync(plain));
	const {body, events} = await runChain('gzip, gzip', encoded);

	assert.deepEqual(body, plain);
	assert.deepEqual(events, ['end']);
});

test('coding names are case-insensitive and allow space/tab padding', async () => {
	const encoded = zlib.brotliCompressSync(zlib.gzipSync(plain));
	const {body, events} = await runChain('  GzIp ,\tBR\t', encoded);

	assert.deepEqual(body, plain);
	assert.deepEqual(events, ['end']);
});

test('inner deflate layer sniffs its own first bytes', async () => {
	// The payload starts with 'C' so that its raw-deflate stream has the
	// 0x08 bit clear in the first byte, which is what the raw/zlib-wrapped
	// sniffing (shared with single-layer deflate) keys on.
	const deflatePlain = Buffer.from('Compressible response content.\n');

	// Raw deflate on the inside: the wire starts with the gzip magic (0x1f),
	// so reusing the wire's first byte for the inner layer would misdetect.
	const rawInner = zlib.gzipSync(zlib.deflateRawSync(deflatePlain));
	const rawResult = await runChain('deflate, gzip', rawInner);
	assert.deepEqual(rawResult.body, deflatePlain);
	assert.deepEqual(rawResult.events, ['end']);

	// Zlib-wrapped deflate on the inside keeps working too.
	const wrappedInner = zlib.gzipSync(zlib.deflateSync(deflatePlain));
	const wrappedResult = await runChain('deflate, gzip', wrappedInner);
	assert.deepEqual(wrappedResult.body, deflatePlain);
	assert.deepEqual(wrappedResult.events, ['end']);
});

test('decodes a chain fed one byte at a time', async () => {
	const encoded = zlib.brotliCompressSync(zlib.gzipSync(plain));
	const {body, events} = await runChain('gzip, br', encoded, {chunkSize: 1});

	assert.deepEqual(body, plain);
	assert.deepEqual(events, ['end']);
});

if (supportsZstd) {
	test('decodes zstd as part of a chain', async () => {
		const encoded = zlib.gzipSync(zlib.zstdCompressSync(plain));
		const {body, events} = await runChain('zstd, gzip', encoded);

		assert.deepEqual(body, plain);
		assert.deepEqual(events, ['end']);
	});
}

test('identity-only chain returns the response untouched', () => {
	const response = new RecordedResponse({'content-encoding': 'identity'});
	const before = listenerCounts(response);

	const stream = decompressResponse(response);

	assert.equal(stream, response);
	assert.deepEqual(listenerCounts(response), before);
	assert.equal(response.readableFlowing, null);
	assert.equal(response.lifecycle.destroy, 0);
});

test('identity layers inside a chain are no-ops', async () => {
	const encoded = zlib.gzipSync(plain);
	const {body, events} = await runChain('gzip, identity', encoded);

	assert.deepEqual(body, plain);
	assert.deepEqual(events, ['end']);
});

const assertPassthrough = async (encoding, encoded) => {
	const response = new RecordedResponse({'content-encoding': encoding});
	const before = listenerCounts(response);

	const stream = decompressResponse(response);

	// Same object, no decoding listeners, no bytes consumed.
	assert.equal(stream, response, encoding);
	assert.deepEqual(listenerCounts(response), before, encoding);
	assert.equal(response.readableFlowing, null, encoding);

	const chunks = [];
	stream.on('data', chunk => {
		chunks.push(chunk);
	});
	response.end(encoded);
	await once(stream, 'end');

	// The compressed bytes pass through as-is.
	assert.deepEqual(Buffer.concat(chunks), encoded, encoding);
	assert.equal(stream.headers['content-encoding'], encoding, encoding);
	assert.equal(response.lifecycle.destroy, 0, encoding);
};

test('chains with unknown or empty members pass through completely', async () => {
	const encoded = zlib.brotliCompressSync(zlib.gzipSync(plain));

	await Promise.all([
		'gzip, unicorn',
		'gzip,,br',
		'br,',
		'gzip, br, identity, x-custom',
	].map(encoding => assertPassthrough(encoding, encoded)));
});

test('corrupt outer layer reports a single error and destroys the response', async () => {
	const encoded = zlib.gzipSync(zlib.gzipSync(plain));
	const corrupt = encoded.subarray(0, -1); // Truncate the outer gzip stream

	const {response, events} = await runChain('gzip, gzip', corrupt);

	assert.equal(errorEvents(events).length, 1, JSON.stringify(events));
	assert.ok(!events.includes('end'));
	assert.match(errorEvents(events)[0], /^error:Z_(BUF|DATA)_ERROR$/);
	assert.equal(response.lifecycle.destroy, 1);
});

test('corrupt inner layer reports a single error and destroys the response', async () => {
	const inner = zlib.gzipSync(plain).subarray(0, -4); // Truncated inner gzip stream
	const encoded = zlib.gzipSync(inner); // Outer layer is valid

	const {response, events} = await runChain('gzip, gzip', encoded);

	assert.equal(errorEvents(events).length, 1, JSON.stringify(events));
	assert.ok(!events.includes('end'));
	assert.match(errorEvents(events)[0], /^error:Z_(BUF|DATA)_ERROR$/);
	assert.equal(response.lifecycle.destroy, 1);
});

test('source data arriving after an inner layer error causes no further errors', async () => {
	const bigPlain = Buffer.from('Compressible response content.\n'.repeat(200));
	const inner = zlib.gzipSync(bigPlain);
	// Flip one bit to corrupt the inner gzip body.
	inner[Math.floor(inner.length / 2)] = inner[Math.floor(inner.length / 2)] ^ 0xFF; // eslint-disable-line no-bitwise
	const encoded = zlib.gzipSync(inner);

	const response = new RecordedResponse({'content-encoding': 'gzip, gzip'});
	const stream = decompressResponse(response);
	const observer = collect(stream);

	const chunkSize = 32;
	let offset = 0;

	const feedUntilError = async () => {
		if (offset >= encoded.length) {
			return;
		}

		response.write(encoded.subarray(offset, offset + chunkSize));
		offset += chunkSize;

		if (errorEvents(observer.events).length > 0) {
			return;
		}

		await tick();
		await feedUntilError();
	};

	// The inner layer fails partway through; the source keeps sending data.
	const feedRest = async () => {
		if (offset >= encoded.length) {
			return;
		}

		response.write(encoded.subarray(offset, offset + chunkSize));
		offset += chunkSize;
		await tick();
		await feedRest();
	};

	await feedUntilError();
	await feedRest();

	response.end();

	const {events} = await observer.settled();
	assert.equal(errorEvents(events).length, 1, JSON.stringify(events));
	assert.ok(!events.includes('end'));
	assert.equal(response.lifecycle.destroy, 1);
});

test('source response error is reported once and destroys the response', async () => {
	const encoded = zlib.brotliCompressSync(zlib.gzipSync(plain));
	const response = new RecordedResponse({'content-encoding': 'gzip, br'});
	const stream = decompressResponse(response);
	const observer = collect(stream);

	response.write(encoded.subarray(0, 10));
	await tick();
	response.emit('error', new Error('source boom'));

	const {events} = await observer.settled();
	assert.deepEqual(events, ['error:source boom']);
	assert.equal(response.lifecycle.destroy, 1);
});

test('empty chained response completes successfully', async () => {
	const response = new RecordedResponse({'content-encoding': 'gzip, br', 'content-length': '0'});
	const stream = decompressResponse(response);
	const observer = collect(stream);

	response.end();

	const {body, events} = await observer.settled();
	assert.equal(body.length, 0);
	assert.deepEqual(events, ['end']);
	assert.equal(response.lifecycle.destroy, 0);
});

test('manual destroy without error does not destroy the underlying response', async () => {
	const encoded = zlib.brotliCompressSync(zlib.gzipSync(plain));
	const response = new RecordedResponse({'content-encoding': 'gzip, br'});
	const stream = decompressResponse(response);
	const observer = collect(stream);

	response.write(encoded.subarray(0, 8));
	stream.destroy();

	assert.equal(stream.destroyed, true);

	await settle();

	assert.equal(response.lifecycle.destroy, 0);
	assert.equal(errorEvents(observer.events).length, 0);

	response.destroy();
});

test('real http.IncomingMessage metadata survives the default export', async () => {
	const encoded = zlib.brotliCompressSync(zlib.gzipSync(plain));
	const socket = new PassThrough();
	const message = new http.IncomingMessage(socket);
	message.statusCode = 200;
	message.statusMessage = 'OK';
	message.headers = {
		'content-encoding': 'gzip, br',
		'content-length': String(encoded.length),
		'content-type': 'text/plain',
	};
	message.rawHeaders = [
		'Content-Encoding',
		'gzip, br',
		'Content-Length',
		String(encoded.length),
		'Content-Type',
		'text/plain',
	];
	message.customProp = '🦄';

	const stream = decompressResponse(message);
	assert.notEqual(stream, message);

	const observer = collect(stream);
	message.push(encoded);
	// eslint-disable-next-line unicorn/prefer-single-call
	message.push(null);
	message.complete = true; // The full message has been received

	const {body, events} = await observer.settled();
	assert.deepEqual(body, plain);
	assert.deepEqual(events, ['end']);

	// Metadata from the real IncomingMessage is still accessible.
	assert.equal(stream.statusCode, 200);
	assert.equal(stream.statusMessage, 'OK');
	assert.equal(stream.socket, socket);
	assert.equal(typeof stream.setTimeout, 'function');
	assert.deepEqual(stream.trailers, {});
	assert.deepEqual(stream.rawHeaders, message.rawHeaders);
	assert.equal(stream.customProp, '🦄');
	assert.equal(stream.httpVersion, message.httpVersion);

	// The returned headers are cleaned; the original message is untouched.
	assert.equal(stream.headers['content-encoding'], undefined);
	assert.equal(stream.headers['content-length'], undefined);
	assert.equal(stream.headers['content-type'], 'text/plain');
	assert.equal(message.headers['content-encoding'], 'gzip, br');
	assert.equal(message.headers['content-length'], String(encoded.length));
	assert.equal(message.complete, true);
});

// A real loopback HTTP exchange. Some sandboxed environments forbid listen();
// the test is skipped there instead of failing.
const canListen = await new Promise(resolve => {
	const probe = http.createServer();
	probe.once('error', () => {
		resolve(false);
	});
	probe.listen(0, '127.0.0.1', () => {
		probe.close(() => {
			resolve(true);
		});
	});
});

test('loopback HTTP response through the default export', {skip: canListen ? false : 'listen() is not permitted in this environment'}, async () => {
	const encoded = zlib.brotliCompressSync(zlib.gzipSync(plain));
	const server = http.createServer((request, response) => {
		response.statusCode = 200;
		response.setHeader('content-type', 'text/plain');
		response.setHeader('content-encoding', 'gzip, br');
		response.end(encoded);
	});

	await new Promise((resolve, reject) => {
		server.once('error', reject);
		server.listen(0, '127.0.0.1', resolve);
	});

	try {
		const message = await new Promise((resolve, reject) => {
			const request = http.get({host: '127.0.0.1', port: server.address().port}, resolve);
			request.once('error', reject);
		});

		const stream = decompressResponse(message);
		const observer = collect(stream);

		const {body, events} = await observer.settled();
		assert.deepEqual(body, plain);
		assert.deepEqual(events, ['end']);

		// Genuine IncomingMessage metadata is preserved, not masked by a test double.
		assert.equal(stream.statusCode, 200);
		assert.ok(stream.socket);
		assert.equal(typeof stream.setTimeout, 'function');
		assert.deepEqual(stream.trailers, {});
		assert.ok(stream.rawHeaders.includes('Content-Encoding'));
		assert.equal(stream.headers['content-encoding'], undefined);
		assert.equal(stream.headers['content-length'], undefined);
		assert.equal(message.headers['content-encoding'], 'gzip, br');
		assert.equal(message.complete, true);
	} finally {
		server.close();
	}
});
