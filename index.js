import {Transform as TransformStream, PassThrough as PassThroughStream} from 'node:stream';
import zlib from 'node:zlib';
import mimicResponse from 'mimic-response';

// Detect zstd support (available in Node.js >= 22.15.0)
const supportsZstd = typeof zlib.createZstdDecompress === 'function';

const supportedEncodings = new Set(['gzip', 'deflate', 'br', 'identity']);

if (supportsZstd) {
	supportedEncodings.add('zstd');
}

// Parse a `content-encoding` header into the list of applied codings.
// Returns undefined when the header is missing, malformed, or names a
// coding this runtime cannot decode.
const parseEncodingChain = header => {
	if (typeof header !== 'string' || header.length === 0) {
		return undefined;
	}

	const chain = header
		.split(',')
		.map(part => part.replaceAll(/^[ \t]+|[ \t]+$/g, '').toLowerCase());

	if (chain.some(encoding => !supportedEncodings.has(encoding))) {
		return undefined;
	}

	return chain;
};

export default function decompressResponse(response) {
	const chain = parseEncodingChain(response.headers['content-encoding']);

	// Unknown, malformed, or unsupported chains are passed through untouched:
	// no bytes are consumed and no decoding listeners are attached.
	if (!chain) {
		return response;
	}

	// Codings are listed in the order they were applied, so they are decoded
	// in reverse order. `identity` layers are no-ops.
	const decoders = chain.filter(encoding => encoding !== 'identity').reverse();

	if (decoders.length === 0) {
		return response;
	}

	let isEmpty = true;
	let settled = false;
	let tornDown = false;

	// Clone headers to avoid modifying the original response headers
	const headers = {...response.headers};
	delete headers['content-encoding'];
	delete headers['content-length'];

	const finalStream = new PassThroughStream({
		autoDestroy: false,
	});
	finalStream.headers = headers;

	const stages = [];

	const teardown = () => {
		if (tornDown) {
			return;
		}

		tornDown = true;

		response.unpipe(checker);
		checker.unpipe();
		checker.destroy();

		for (const stage of stages) {
			stage.unpipe();
			stage.destroy();
		}
	};

	// Report a single terminal error on the returned stream, destroy the
	// source response, and end every decoding layer.
	const fail = error => {
		if (settled) {
			return;
		}

		settled = true;
		teardown();
		finalStream.destroy(error);
	};

	const onStageError = error => {
		if (settled) {
			return;
		}

		if (isEmpty && !response.readable) {
			settled = true;
			finalStream.end();
			return;
		}

		fail(error);
	};

	// Only destroy response on error, not on normal completion
	finalStream.once('error', () => {
		response.destroy();
	});

	// Stop the decoding chain when the returned stream goes away. The
	// underlying response is left alone unless an error was reported.
	finalStream.once('close', () => {
		teardown();
	});

	response.once('error', fail);

	const createDeflateStage = downstream => {
		// Both zlib-wrapped and raw deflate inputs are supported. The choice
		// must be based on the first bytes this layer actually receives, which
		// for inner layers differ from the first bytes on the wire.
		let inflate;

		const sniffer = new TransformStream({
			transform(data, _encoding, callback) {
				if (!inflate) {
					inflate = (data.length > 0 && (data[0] & 0x08) === 0) // eslint-disable-line no-bitwise
						? zlib.createInflateRaw()
						: zlib.createInflate();

					inflate.on('error', onStageError);
					inflate.pipe(downstream);
					stages.push(inflate);

					sniffer.unpipe(downstream);
					sniffer.pipe(inflate);
				}

				callback(null, data);
			},
		});

		sniffer.on('error', onStageError);
		sniffer.pipe(downstream);
		stages.push(sniffer);

		return sniffer;
	};

	const createStage = (encoding, downstream) => {
		if (encoding === 'deflate') {
			return createDeflateStage(downstream);
		}

		let stream;

		if (encoding === 'gzip') {
			stream = zlib.createUnzip();
		} else if (encoding === 'br') {
			stream = zlib.createBrotliDecompress();
		} else {
			stream = zlib.createZstdDecompress();
		}

		stream.on('error', onStageError);
		stream.pipe(downstream);
		stages.push(stream);

		return stream;
	};

	// The chain is only built once the first bytes arrive, so that empty
	// responses complete successfully without touching any decoder.
	const buildChain = () => {
		let entry = finalStream;

		for (let index = decoders.length - 1; index >= 0; index--) {
			entry = createStage(decoders[index], entry);
		}

		checker.pipe(entry);
	};

	const checker = new TransformStream({
		transform(data, _encoding, callback) {
			if (isEmpty) {
				isEmpty = false;
				buildChain();
			}

			callback(null, data);
		},

		flush(callback) {
			if (isEmpty && !settled) {
				settled = true;
				finalStream.end();
			}

			callback();
		},
	});

	checker.on('error', onStageError);

	mimicResponse(response, finalStream);

	response.pipe(checker);

	return finalStream;
}
