import assert from 'node:assert/strict';
import {PassThrough} from 'node:stream';
import {gzipSync, brotliCompressSync} from 'node:zlib';
import decompressResponse from '../index.js';
const plain = Buffer.from('{"items":[1,2,3]}');
async function probe(encoding, bytes) {
 const source = new PassThrough({autoDestroy: false});
 source.headers = {'content-encoding': encoding, 'content-length': String(bytes.length)};
 source.statusCode = 200;
 source.rawHeaders = ['Content-Encoding', encoding];
 const result = decompressResponse(source);
 source.end(bytes);
 const chunks = [];
 for await (const chunk of result) chunks.push(chunk);
 return {same: result === source, bytes: Buffer.concat(chunks), headers: result.headers};
}
const single = await probe('gzip', gzipSync(plain));
assert.deepEqual(single.bytes, plain);
assert.equal(single.headers['content-encoding'], undefined);
const encoded = brotliCompressSync(gzipSync(plain));
const chain = await probe('gzip, br', encoded);
assert.equal(chain.same, false);
assert.deepEqual(chain.bytes, plain);
assert.equal(chain.headers['content-encoding'], undefined);
console.log('baseline: single gzip decoded; gzip, br chain decoded in reverse order');
