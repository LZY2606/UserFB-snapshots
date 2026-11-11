import assert from 'node:assert/strict';
import {Buffer} from 'node:buffer';
import {PassThrough} from 'node:stream';
import zlib from 'node:zlib';
import decompressResponse from '../index.js';

const fixture = 'Compressible response content.\n';
const supportsZstd = typeof zlib.zstdCompressSync === 'function';

class FakeResponse extends PassThrough {
	constructor(headers = {}) {
		super({autoDestroy: false});
		this.headers = {...headers};
		this.rawHeaders = [];
		this.statusCode = 200;
		this.statusMessage = 'OK';
		this.trailers = {};
		this.rawTrailers = [];
		this.socket = {};
		this.destroyCount = 0;
		const originalDestroy = this.destroy.bind(this);
		this.destroy = error => {
			this.destroyCount++;
			return originalDestroy(error);
		};
	}
}

const drain = stream => new Promise((resolve, reject) => {
	const chunks = [];
	stream.on('data', chunk => {
		chunks.push(chunk);
	});
	stream.on('error', reject);
	stream.on('end', () => resolve(Buffer.concat(chunks)));
});

const check = async (name, encoding, compressed, expected = Buffer.from(fixture)) => {
	const response = new FakeResponse({'content-encoding': encoding, 'content-length': String(compressed.length)});
	const stream = decompressResponse(response);
	response.end(compressed);
	assert.notEqual(stream, response, name);
	assert.equal(stream.statusCode, 200, name);
	assert.equal(stream.socket, response.socket, name);
	assert.equal(stream.headers['content-encoding'], undefined, name);
	assert.equal(stream.headers['content-length'], undefined, name);
	assert.equal(response.headers['content-encoding'], encoding, name);
	const body = await drain(stream);
	assert.deepEqual(body, expected, name);
	assert.equal(response.destroyCount, 0, name);
	console.log('ok -', name);
};

await check('gzip', 'gzip', zlib.gzipSync(fixture));
await check('deflate', 'deflate', zlib.deflateSync(fixture));
await check('deflate raw', 'deflate', zlib.deflateRawSync(fixture));
await check('brotli', 'br', zlib.brotliCompressSync(fixture));
if (supportsZstd) {
	await check('zstd', 'zstd', zlib.zstdCompressSync(fixture));
}

// Missing data: non-empty truncated gzip must error, not succeed as empty.
{
	const truncated = zlib.gzipSync(fixture).subarray(0, -1);
	const response = new FakeResponse({'content-encoding': 'gzip'});
	const stream = decompressResponse(response);
	response.end(truncated);
	await assert.rejects(drain(stream), {code: 'Z_BUF_ERROR'});
	assert.equal(response.destroyCount, 1);
	console.log('ok - missing data errors and destroys response');
}

// Unknown encoding: fully untouched.
{
	const response = new FakeResponse({'content-encoding': 'unicorn'});
	const listeners = ['error', 'data', 'end'].map(event => response.listenerCount(event));
	const stream = decompressResponse(response);
	assert.equal(stream, response);
	assert.deepEqual(['error', 'data', 'end'].map(event => response.listenerCount(event)), listeners);
	response.end(Buffer.from(fixture));
	assert.deepEqual(await drain(stream), Buffer.from(fixture));
	assert.equal(stream.headers['content-encoding'], 'unicorn');
	console.log('ok - unknown encoding passthrough');
}

// Empty compressed response: legacy success behavior.
{
	const response = new FakeResponse({'content-encoding': 'gzip'});
	const stream = decompressResponse(response);
	response.end();
	assert.deepEqual(await drain(stream), Buffer.alloc(0));
	assert.equal(response.destroyCount, 0);
	console.log('ok - empty response succeeds');
}

// Manual destroy without error does not destroy the underlying response.
{
	const response = new FakeResponse({'content-encoding': 'gzip'});
	const stream = decompressResponse(response);
	response.write(zlib.gzipSync(fixture).subarray(0, 4));
	stream.destroy();
	assert.equal(stream.destroyed, true);
	assert.equal(response.destroyCount, 0);
	response.destroy();
	console.log('ok - manual destroy leaves response alone');
}

console.log('all single-layer regression checks passed');
