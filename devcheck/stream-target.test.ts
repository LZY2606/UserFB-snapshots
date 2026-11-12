import test from 'node:test';
import assert from 'node:assert/strict';
import PostalMime from '../src/postal-mime.js';
import { check } from './check.mjs';
import type { Email } from '../src/postal-mime.js';

type LineCall = { pull: number; isFinal: boolean };

// Wraps processLine and finalize to record, in order, when each physical line reaches the
// parser relative to the pull that delivered its bytes, and that the tree is finalized once.
function observeParser(parser: PostalMime, pulls: () => number) {
    const processLine = parser.processLine.bind(parser);
    const finalize = parser.finalize.bind(parser);
    const calls: LineCall[] = [];
    let finalizeCount = 0;
    parser.processLine = async (
        line: Uint8Array<ArrayBuffer>,
        lineBreak: Uint8Array<ArrayBuffer>,
        isFinal: boolean
    ) => {
        calls.push({ pull: pulls(), isFinal });
        return processLine(line, lineBreak, isFinal);
    };
    parser.finalize = async () => {
        finalizeCount++;
        return finalize();
    };
    return {
        calls,
        get finalizeCount() {
            return finalizeCount;
        }
    };
}

// A deterministic message exercising every boundary the stream framer has to keep apart
// from a chunk boundary: nested multipart, text and html, binary 7/8bit bytes, base64 with
// groups split across chunks, quoted-printable soft breaks, an inline message/rfc822, mixed
// CRLF/LF and a CR-CR-LF run, and an epilogue with no final line break.
function buildMessage(): Uint8Array {
    const pieces: (string | Uint8Array)[] = [
        'From: sender@example.com\r\n',
        'To: rcpt@example.com\r\n',
        'Subject: boundary and encoding mix\r\n',
        'Content-Type: multipart/mixed; boundary="OUT"\r\n',
        '\r\n',
        'preamble is ignored\r\n',
        '--OUT\r\n',
        'Content-Type: multipart/alternative; boundary="IN"\r\n',
        '\r\n',
        '--IN\r\n',
        'Content-Type: text/plain; charset=utf-8\r\n',
        '\r\n',
        'plain line one\r\nplain line two\nlf-only line\r\n',
        '--IN\r\n',
        'Content-Type: text/html; charset=utf-8\r\n',
        '\r\n',
        '<p>hi &amp; bye</p>\r\n',
        '--IN--\r\n',
        '--OUT\r\n',
        'Content-Type: application/octet-stream\r\n',
        'Content-Transfer-Encoding: binary\r\n',
        'Content-Disposition: attachment; filename="bin.dat"\r\n',
        '\r\n'
    ];
    // NUL, bare CR, CRLF, a CR-CR-LF run (the first CR is content, CRLF is the break) and
    // high bytes that must survive without any text normalization.
    pieces.push(new Uint8Array([0x00, 0x01, 0x0d, 0x0a, 0x0d, 0x0d, 0x0a, 0xff, 0xfe, 0x41, 0x0d, 0x42]));
    pieces.push(
        '\r\n',
        '--OUT\r\n',
        'Content-Type: application/octet-stream\r\n',
        'Content-Transfer-Encoding: base64\r\n',
        'Content-Disposition: attachment; filename="b64.dat"\r\n',
        '\r\n',
        'SGVsbG8s\r\nIHdvcmxk\r\nIQ==\r\n',
        '--OUT\r\n',
        'Content-Type: application/octet-stream\r\n',
        'Content-Transfer-Encoding: quoted-printable\r\n',
        'Content-Disposition: attachment; filename="qp.dat"\r\n',
        '\r\n',
        'line one=\r\n',
        'continues=3Dand trailing=\r\n',
        'end\r\n',
        '--OUT\r\n',
        'Content-Type: message/rfc822\r\n',
        '\r\n',
        'Subject: inner\r\nFrom: inner@example.com\r\n\r\ninner body line\r\ninner second\r\n',
        '--OUT--\r\n',
        'epilogue without final line break'
    );
    return Buffer.concat(
        pieces.map(p => (p instanceof Uint8Array ? Buffer.from(p) : Buffer.from(p, 'binary')))
    );
}

// Every single cut point, one-byte chunks, and fixed-stride multi cuts for one message.
function cutPlans(len: number): number[][] {
    const plans: number[][] = [];
    for (let cut = 1; cut < len; cut++) {
        plans.push([cut]);
    }
    const oneByte: number[] = [];
    for (let i = 1; i <= len; i++) {
        oneByte.push(i);
    }
    plans.push(oneByte);
    for (const stride of [2, 3, 4, 7, 16]) {
        const plan: number[] = [];
        for (let i = stride; i < len; i += stride) {
            plan.push(i);
        }
        if (plan.length) {
            plans.push(plan);
        }
    }
    return plans;
}

function segments(bytes: Uint8Array, cuts: number[]): [number, number][] {
    const out: [number, number][] = [];
    let prev = 0;
    for (const cut of [...cuts, bytes.length]) {
        out.push([prev, cut]);
        prev = cut;
    }
    return out;
}

interface SourceOptions {
    // one Uint8Array workspace is refilled on every pull instead of fresh views
    reuseWorkspace?: boolean;
    // the workspace is overwritten with sentinel garbage before each segment is written,
    // so a view retained across pulls would read the wrong bytes
    poisonOnRefill?: boolean;
    // zero-length chunks are enqueued between the real segments
    includeEmptyChunks?: boolean;
    highWaterMark?: number;
}

// Builds a pull based stream over fixed segments and instruments how the parser drives it.
function makeSource(bytes: Uint8Array, cuts: number[], options: SourceOptions = {}) {
    const segs = segments(bytes, cuts);
    const maxSize = Math.max(...segs.map(([s, e]) => e - s));
    const workspace = options.reuseWorkspace ? new Uint8Array(maxSize) : null;
    const poison = new Uint8Array(maxSize).fill(0x5a);

    let pulls = 0;
    let activePulls = 0;
    let maxActivePulls = 0;
    let cancels = 0;
    let index = 0;

    const stream = new ReadableStream<Uint8Array>(
        {
            pull(controller) {
                pulls++;
                activePulls++;
                maxActivePulls = Math.max(maxActivePulls, activePulls);
                try {
                    if (index >= segs.length) {
                        controller.close();
                        return;
                    }
                    const [start, end] = segs[index++];
                    const size = end - start;
                    let view: Uint8Array;
                    if (workspace) {
                        if (options.poisonOnRefill) {
                            workspace.set(poison.subarray(0, Math.max(size, workspace.length)));
                        }
                        view = workspace.subarray(0, size);
                        view.set(bytes.subarray(start, end));
                    } else {
                        view = bytes.subarray(start, end);
                    }
                    if (options.includeEmptyChunks) {
                        controller.enqueue(new Uint8Array(0));
                    }
                    controller.enqueue(view);
                } finally {
                    activePulls--;
                }
            },
            cancel() {
                cancels++;
            }
        },
        { highWaterMark: options.highWaterMark ?? 0 }
    );

    const stats = {
        get pulls() {
            return pulls;
        },
        get maxActivePulls() {
            return maxActivePulls;
        },
        get cancels() {
            return cancels;
        },
        get locked() {
            return stream.locked;
        }
    };

    return { stream, stats };
}

test('stream_early_parse', async t => {
    const message = buildMessage();
    const expected = await new PostalMime().parse(message);

    // One-byte chunks over a pull stream: the first line must reach processLine on the
    // first pull, long before the whole message has been read.
    const cuts: number[] = [];
    for (let i = 1; i <= message.length; i++) {
        cuts.push(i);
    }
    const { stream, stats } = makeSource(message, cuts);
    const parser = new PostalMime();
    const observation = observeParser(parser, () => stats.pulls);

    const email = await parser.parse(stream);
    check(t, email, expected);

    // The first line is handed to processLine exactly when its LF byte is pulled, not after
    // the whole message is buffered.
    const firstLineFeedPull = message.indexOf(0x0a) + 1;
    assert.ok(observation.calls.length > 1, 'the message is processed line by line, not in one call');
    assert.equal(
        observation.calls[0].pull,
        firstLineFeedPull,
        'the first physical line is processed as soon as its line break arrives'
    );
    assert.ok(firstLineFeedPull > 1, 'the first line spans more than the first byte, so processing waits for it');
    assert.ok(stats.pulls > observation.calls[0].pull, 'parsing starts before the source is read to the end');
    assert.equal(stats.maxActivePulls, 1, 'at most one reader.read is in flight at a time');
    assert.equal(stats.cancels, 0, 'a successful parse does not cancel the source');
    assert.equal(stats.locked, false, 'the reader lock is released after a normal end');

    // An over-large header is a parse error that surfaces while most of the source is still
    // unread: the parser stops pulling and cancels once instead of draining to EOF first.
    const bigHeader = Buffer.from(`Subject: ${'x'.repeat(4096)}\r\n\r\nbody\r\n`);
    const headerCuts: number[] = [];
    for (let i = 1; i <= bigHeader.length; i++) {
        headerCuts.push(i);
    }
    const failing = makeSource(bigHeader, headerCuts);
    await assert.rejects(
        () => new PostalMime({ maxHeadersSize: 64 }).parse(failing.stream),
        err => err instanceof Error && (err as { code?: string }).code === 'EMAXHEADERSIZE'
    );
    assert.ok(failing.stats.pulls < bigHeader.length, 'the source is not read to its end before the parse error');
    assert.equal(failing.stats.cancels, 1, 'a parse error cancels the source exactly once');
    assert.equal(failing.stats.locked, false, 'the reader lock is released after a parse error');
});

test('octet_boundaries', async t => {
    const message = buildMessage();
    const expected = await new PostalMime().parse(message);

    let plans = 0;
    for (const cuts of cutPlans(message.length)) {
        const { stream, stats } = makeSource(message, cuts);
        const parser = new PostalMime();
        const observation = observeParser(parser, () => stats.pulls);
        const email = await parser.parse(stream);

        // full headers, text and html bodies and every attachment byte, not a summary
        check(t, email, expected);
        assert.equal(observation.finalizeCount, 1, 'a message ending in a boundary is finalized once');
        assert.equal(stats.locked, false);
        plans++;
    }

    // A last boundary landing exactly on a chunk end, and an EOF stopping on a body line
    // with no line break, each complete a single time.
    const boundaryMail = Buffer.from(
        'Content-Type: multipart/mixed; boundary=B\r\n\r\n--B\r\nContent-Type: text/plain\r\n\r\npart\r\n--B--\r\n'
    );
    const boundaryCuts = [boundaryMail.lastIndexOf('--B--') + '--B--\r\n'.length];
    const boundaryExpected = await new PostalMime().parse(boundaryMail);
    const boundarySource = makeSource(boundaryMail, boundaryCuts);
    const boundaryParser = new PostalMime();
    const boundaryObservation = observeParser(boundaryParser, () => boundarySource.stats.pulls);
    const boundaryEmail = await boundaryParser.parse(boundarySource.stream);
    check(t, boundaryEmail, boundaryExpected);
    assert.equal(boundaryObservation.finalizeCount, 1);

    const noTrailingBreak = Buffer.from('Subject: no-break\r\n\r\nlast line without a break');
    const noBreakExpected = await new PostalMime().parse(noTrailingBreak);
    const noBreakSource = makeSource(noTrailingBreak, [noTrailingBreak.length - 1]);
    const noBreakParser = new PostalMime();
    const noBreakObservation = observeParser(noBreakParser, () => noBreakSource.stats.pulls);
    const noBreakEmail = await noBreakParser.parse(noBreakSource.stream);
    check(t, noBreakEmail, noBreakExpected);
    assert.equal(noBreakObservation.finalizeCount, 1, 'the tree is finalized once at EOF');

    t.diagnostic(`compared ${plans} chunk plans byte for byte`);
});

test('transfer_decoders', async t => {
    const message = buildMessage();
    const expected = await new PostalMime().parse(message);

    // Split inside the base64 groups, the quoted-printable soft breaks and the binary
    // part's CR/CRLF runs. The decoded bytes must be identical for every chunking.
    for (const cuts of cutPlans(message.length)) {
        const { stream } = makeSource(message, cuts);
        const email = await new PostalMime().parse(stream);
        check(t, email.attachments, expected.attachments);
    }

    const byName = (email: Email, filename: string) =>
        new Uint8Array(email.attachments.find(a => a.filename === filename)!.content as ArrayBuffer);

    // The binary attachment keeps every original byte, the embedded CR/CRLF and the high
    // bytes included. The CRLF in front of its boundary is the boundary's and is not part
    // of the part, so it is not in the content.
    assert.deepEqual(Array.from(byName(expected, 'bin.dat')), [
        0x00, 0x01, 0x0d, 0x0a, 0x0d, 0x0d, 0x0a, 0xff, 0xfe, 0x41, 0x0d, 0x42
    ]);
    assert.deepEqual(Array.from(byName(expected, 'b64.dat')), Array.from(Buffer.from('Hello, world!')));
    assert.deepEqual(
        Array.from(byName(expected, 'qp.dat')),
        Array.from(Buffer.from('line onecontinues=and trailingend'))
    );

    // A trailing line break at the physical EOF is content; the break before a boundary is
    // the boundary's. The same body parsed at EOF keeps its final CRLF.
    const eofBody = Buffer.from(
        'Content-Type: application/octet-stream\r\nContent-Transfer-Encoding: binary\r\n\r\nabc\r\n'
    );
    const eofExpected = await new PostalMime().parse(eofBody);
    const eofBytes = new Uint8Array(eofExpected.attachments[0].content as ArrayBuffer);
    assert.deepEqual(Array.from(eofBytes), [0x61, 0x62, 0x63, 0x0d, 0x0a]);
    for (const cuts of cutPlans(eofBody.length)) {
        const email = await new PostalMime().parse(makeSource(eofBody, cuts).stream);
        check(t, new Uint8Array(email.attachments[0].content as ArrayBuffer), eofBytes);
    }
});

test('source_ownership', async t => {
    const message = buildMessage();
    const expected = await new PostalMime().parse(message);

    // A reused workspace, refilled with garbage on every pull, can not corrupt a line the
    // parser still needs: every retained byte is an independent copy.
    for (const cuts of cutPlans(message.length)) {
        const reused = makeSource(message, cuts, { reuseWorkspace: true, poisonOnRefill: true });
        const email = await new PostalMime().parse(reused.stream);
        check(t, email, expected);
    }

    // Views at a non-zero offset of a larger buffer: sentinel LF/CR bytes just outside the
    // view must never be read, or they would split lines and change the result.
    const backing = new Uint8Array(message.length + 16);
    backing.fill(0x0a);
    backing.set(message, 8);
    backing[7] = 0x0d;
    const view = backing.subarray(8, 8 + message.length);
    const offsetSource = new ReadableStream<Uint8Array>({
        pull(controller) {
            controller.enqueue(view);
            controller.close();
        }
    });
    const offsetEmail = await new PostalMime().parse(offsetSource);
    check(t, offsetEmail, expected);

    // Empty chunks interspersed with real ones change nothing and produce no empty lines.
    const withEmpties = makeSource(message, [10, 20, 40, 80, 200], { includeEmptyChunks: true });
    const emptiesEmail = await new PostalMime().parse(withEmpties.stream);
    check(t, emptiesEmail, expected);

    // An ordinary, non-reusing stream keeps its own prefetch strategy (a positive
    // highWaterMark) and parses identically.
    const prefetched = makeSource(message, [64, 128, 256, 512], { highWaterMark: 16 });
    const prefetchedEmail = await new PostalMime().parse(prefetched.stream);
    check(t, prefetchedEmail, expected);
});

test('source_errors', async t => {
    // A read() rejection is rethrown with its exact Error identity, no wrapping.
    const sourceError = new Error('source failed');
    const rejecting = new ReadableStream({
        pull() {
            return Promise.reject(sourceError);
        }
    });
    await assert.rejects(() => new PostalMime().parse(rejecting), err => err === sourceError);
    assert.equal(rejecting.locked, false, 'the lock is released after a read rejection');

    // A parse error cancels the source once, and a cancel that rejects never replaces it.
    const bigHeader = Buffer.from(`Subject: ${'y'.repeat(4096)}\r\n\r\nbody`);
    const cancelBoom = new Error('cancel failed');
    const failCancel = new ReadableStream({
        start(controller) {
            controller.enqueue(bigHeader);
            controller.close();
        },
        cancel() {
            return Promise.reject(cancelBoom);
        }
    });
    await assert.rejects(
        () => new PostalMime({ maxHeadersSize: 64 }).parse(failCancel),
        err => err instanceof Error && (err as { code?: string }).code === 'EMAXHEADERSIZE' && err !== cancelBoom
    );
    assert.equal(failCancel.locked, false);

    // A cancel that never settles can not hold back or mask the parse error either.
    const hangingCancel = new ReadableStream({
        start(controller) {
            controller.enqueue(bigHeader);
            controller.close();
        },
        cancel() {
            return new Promise<void>(() => {});
        }
    });
    await assert.rejects(
        () => new PostalMime({ maxHeadersSize: 64 }).parse(hangingCancel),
        err => (err as { code?: string }).code === 'EMAXHEADERSIZE'
    );
    assert.equal(hangingCancel.locked, false);

    // A non-byte chunk is EINVALIDINPUT and cancels once.
    let cancels = 0;
    const badChunk = new ReadableStream({
        start(controller) {
            controller.enqueue('not bytes' as unknown as Uint8Array);
        },
        cancel() {
            cancels++;
        }
    });
    await assert.rejects(
        () => new PostalMime().parse(badChunk),
        err => err instanceof TypeError && (err as { code?: string }).code === 'EINVALIDINPUT'
    );
    assert.equal(cancels, 1);
    assert.equal(badChunk.locked, false);

    // An empty stream and a stream of only empty chunks both parse to an empty message once
    // and release the lock without cancelling.
    const empty = new ReadableStream({ start(controller) { controller.close(); } });
    const emptyEmail = await new PostalMime().parse(empty);
    check(t, emptyEmail, await new PostalMime().parse(new ArrayBuffer(0)));
    assert.equal(empty.locked, false);

    const onlyEmpty = new ReadableStream({
        pull(controller) {
            controller.enqueue(new Uint8Array(0));
            controller.close();
        }
    });
    const onlyEmptyEmail = await new PostalMime().parse(onlyEmpty);
    check(t, onlyEmptyEmail, emptyEmail);
    assert.equal(onlyEmpty.locked, false);

    // A parser instance still parses only once; the stream path does not reset that.
    const reused = new PostalMime();
    const oneShot = new ReadableStream({
        pull(controller) {
            controller.enqueue(Buffer.from('Subject: once\r\n\r\nbody'));
            controller.close();
        }
    });
    await reused.parse(oneShot);
    await assert.rejects(
        () => reused.parse(new ArrayBuffer(0)),
        err => (err as { code?: string }).code === 'EPARSERREUSED'
    );
});
