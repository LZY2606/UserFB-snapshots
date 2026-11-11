// Regression contract for the wire -> text acquisition chain:
// gzip decompression (got) -> charset selection -> byte decoding (iconv-lite)
// -> DOM / JSON materialization. Samples are served by a loopback HTTP
// server; raw bytes are written out literally below and cross-checked
// against Node's WHATWG TextDecoder during development -- never produced
// by the charset converter under test. See test/wire-text-matrix.md.

import test from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import { gzipSync } from 'node:zlib';

import Crawler from '../dist/index.js';

// ---------------------------------------------------------------------------
// Sample bytes (literal) and the Unicode text they must decode to.
// ---------------------------------------------------------------------------

// UTF-8: 汉 (U+6C49) 字 (U+5B57) "e" + combining acute (U+0301).
const UTF8_BYTES = [0xe6, 0xb1, 0x89, 0xe5, 0xad, 0x97, 0x65, 0xcc, 0x81];
const UTF8_TEXT = '汉字é';

// Latin-1 (ISO-8859-1): "Jörg" and "café" (1 byte == 1 code point here).
const LATIN1_NAME_BYTES = [0x4a, 0xf6, 0x72, 0x67];
const LATIN1_WORD_BYTES = [0x63, 0x61, 0x66, 0xe9];
const LATIN1_NAME = 'Jörg';
const LATIN1_WORD = 'café';

// Shift-JIS: 日本語 (日=93FA 本=967B 語=8CEA).
const SJIS_BYTES = [0x93, 0xfa, 0x96, 0x7b, 0x8c, 0xea];
const SJIS_TEXT = '日本語';

const ascii = s => Buffer.from(s, 'ascii');

// HTML pages, assembled from ASCII skeletons plus the literal sample bytes.
const UTF8_PAGE = Buffer.concat([
    ascii('<!doctype html><html><head><meta charset="utf-8"></head><body><p data-name="'),
    Buffer.from(UTF8_BYTES),
    ascii('">'),
    Buffer.from(UTF8_BYTES),
    ascii('</p></body></html>'),
]);
// Header-only charset declaration, no <meta> at all.
const LATIN1_PAGE = Buffer.concat([
    ascii('<!doctype html><html><body><p data-name="'),
    Buffer.from(LATIN1_NAME_BYTES),
    ascii('">'),
    Buffer.from(LATIN1_NAME_BYTES),
    ascii(' '),
    Buffer.from(LATIN1_WORD_BYTES),
    ascii('</p></body></html>'),
]);
// Same Latin-1 bytes but the declaration only lives in a <meta> tag.
const LATIN1_META_PAGE = Buffer.concat([
    ascii('<!doctype html><html><head><meta charset="iso-8859-1"></head><body><p data-name="'),
    Buffer.from(LATIN1_NAME_BYTES),
    ascii('">'),
    Buffer.from(LATIN1_NAME_BYTES),
    ascii('</p></body></html>'),
]);
const SJIS_PAGE = Buffer.concat([
    ascii('<!doctype html><html><body><p data-name="'),
    Buffer.from(SJIS_BYTES),
    ascii('">'),
    Buffer.from(SJIS_BYTES),
    ascii('</p></body></html>'),
]);
// UTF-8 bytes whose <meta> claims iso-8859-1 (used with a conflicting header).
const CONFLICT_PAGE = Buffer.concat([
    ascii('<!doctype html><html><head><meta charset="iso-8859-1"></head><body><p data-name="'),
    Buffer.from(UTF8_BYTES),
    ascii('">'),
    Buffer.from(UTF8_BYTES),
    ascii('</p></body></html>'),
]);
// UTF-8 bytes with no <meta> declaration at all.
const UTF8_BARE_PAGE = Buffer.concat([
    ascii('<!doctype html><html><body><p data-name="'),
    Buffer.from(UTF8_BYTES),
    ascii('">'),
    Buffer.from(UTF8_BYTES),
    ascii('</p></body></html>'),
]);

// JSON: nested string, number and boolean. Non-ASCII strings are UTF-8 bytes.
const JSON_RAW = Buffer.concat([
    ascii('{"name":"'),
    Buffer.from(UTF8_BYTES.slice(0, 6)), // 汉字
    ascii('","nested":{"label":"'),
    Buffer.from(UTF8_BYTES), // 汉字 + e + combining acute
    ascii('","count":3,"ok":true}}'),
]);
const JSON_NAME = '汉字';

// ---------------------------------------------------------------------------
// Loopback server. Every hit is recorded; gated routes are held back until
// three requests are pending and then answered in reverse arrival order, so
// completion order is server-controlled rather than sleep-based.
// ---------------------------------------------------------------------------

const hits = new Map();
const arrivals = [];

const routes = {
    '/utf8': { headers: { 'content-type': 'text/html;charset=utf-8' }, body: UTF8_PAGE },
    '/utf8.gz': { headers: { 'content-type': 'text/html;charset=utf-8' }, body: UTF8_PAGE, gzip: true },
    '/latin1': { headers: { 'content-type': 'text/html;charset=iso-8859-1' }, body: LATIN1_PAGE },
    '/latin1.gz': { headers: { 'content-type': 'text/html;charset=iso-8859-1' }, body: LATIN1_PAGE, gzip: true },
    '/shiftjis': { headers: { 'content-type': 'text/html;charset=shift_jis' }, body: SJIS_PAGE },
    '/shiftjis.gz': { headers: { 'content-type': 'text/html;charset=shift_jis' }, body: SJIS_PAGE, gzip: true },
    '/json': { headers: { 'content-type': 'application/json;charset=utf-8' }, body: JSON_RAW },
    '/json.gz': { headers: { 'content-type': 'application/json;charset=utf-8' }, body: JSON_RAW, gzip: true },
    '/latin1-meta': { headers: { 'content-type': 'text/html' }, body: LATIN1_META_PAGE },
    '/conflict': { headers: { 'content-type': 'text/html;charset=utf-8' }, body: CONFLICT_PAGE },
    '/mislabeled': { headers: { 'content-type': 'text/html;charset=utf-8' }, body: LATIN1_PAGE },
    '/utf8-mislabeled': { headers: { 'content-type': 'text/html;charset=iso-8859-1' }, body: UTF8_BARE_PAGE },
    '/json-latin1-header': { headers: { 'content-type': 'application/json;charset=iso-8859-1' }, body: JSON_RAW },
    '/shiftjis-plain': { headers: { 'content-type': 'text/html' }, body: SJIS_PAGE },
    '/latin1-raw': { headers: { 'content-type': 'text/html' }, body: LATIN1_PAGE },
    '/gated/utf8': { headers: { 'content-type': 'text/html;charset=utf-8' }, body: UTF8_PAGE, gated: true },
    '/gated/latin1': { headers: { 'content-type': 'text/html;charset=iso-8859-1' }, body: LATIN1_PAGE, gated: true },
    '/gated/shiftjis': { headers: { 'content-type': 'text/html' }, body: SJIS_PAGE, gated: true },
};

function respond(res, route) {
    if (route.gzip) {
        // gzip header and body are split into several unequal writes; the
        // comparison boundary is the decompressed entity, not the archive.
        const packed = gzipSync(route.body);
        res.writeHead(200, { ...route.headers, 'content-encoding': 'gzip' });
        res.write(packed.subarray(0, 10));
        const rest = packed.subarray(10);
        const cut = Math.max(1, Math.floor(rest.length / 3));
        res.write(rest.subarray(0, cut));
        res.end(rest.subarray(cut));
    } else {
        res.writeHead(200, route.headers);
        res.end(route.body);
    }
}

const gate = { armed: true, pending: [] };

const server = http.createServer((req, res) => {
    const path = new URL(req.url, 'http://127.0.0.1').pathname;
    hits.set(path, (hits.get(path) ?? 0) + 1);
    arrivals.push(path);
    const route = routes[path];
    if (!route) {
        res.writeHead(404);
        res.end('not found');
        return;
    }
    if (route.gated && gate.armed) {
        gate.pending.push({ res, route });
        if (gate.pending.length === 3) {
            gate.armed = false;
            // Answer in reverse arrival order: last request completes first.
            for (const job of gate.pending.splice(0).reverse()) respond(job.res, job.route);
        }
        return;
    }
    respond(res, route);
});

const hitCount = path => hits.get(path) ?? 0;

// ---------------------------------------------------------------------------
// Crawler helpers. `addAndWait` releases the task inside the callback and
// also waits for the crawler's own `drain` event, so nothing is left hanging
// when a subtest finishes.
// ---------------------------------------------------------------------------

function addAndWait(crawler, options) {
    const drained = new Promise(resolve => crawler.once('drain', resolve));
    const finished = new Promise((resolve, reject) => {
        crawler.add({
            ...options,
            callback: (error, res, release) => {
                release();
                if (error) reject(error);
                else resolve(res);
            },
        });
    });
    return Promise.all([finished, drained]).then(([res]) => res);
}

function assertHtml(res, { charset, name, text }) {
    assert.equal(typeof res.body, 'string');
    assert.ok(res.body.includes(text), `body should contain ${JSON.stringify(text)}`);
    assert.equal(res.charset, charset);
    assert.equal(res.$('p').attr('data-name'), name);
    assert.equal(res.$('p').text(), text);
}

function assertJson(res, { charset }) {
    assert.equal(typeof res.body, 'object');
    assert.equal(res.body.name, JSON_NAME);
    assert.equal(res.body.nested.label, UTF8_TEXT);
    assert.equal(res.body.nested.count, 3);
    assert.equal(res.body.nested.ok, true);
    assert.equal(res.charset, charset);
    assert.equal(res.$, undefined);
}

// ---------------------------------------------------------------------------

test('wire acquisition contract', async t => {
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    t.after(() => new Promise(resolve => {
        server.close(resolve);
        server.closeIdleConnections();
    }));

    const base = `http://127.0.0.1:${server.address().port}`;
    const crawler = new Crawler({ retries: 0, silence: true });

    await t.test('utf-8 html, plain and gzip, via send and add', async () => {
        for (const path of ['/utf8', '/utf8.gz']) {
            const url = base + path;
            const expected = { charset: 'utf-8', name: UTF8_TEXT, text: UTF8_TEXT };
            assertHtml(await crawler.send({ url }), expected);
            assertHtml(await addAndWait(crawler, { url }), expected);
            assert.equal(hitCount(path), 2);
        }
    });

    await t.test('latin-1 html, plain and gzip, via send and add', async () => {
        for (const path of ['/latin1', '/latin1.gz']) {
            const url = base + path;
            const expected = { charset: 'iso-8859-1', name: LATIN1_NAME, text: `${LATIN1_NAME} ${LATIN1_WORD}` };
            assertHtml(await crawler.send({ url }), expected);
            assertHtml(await addAndWait(crawler, { url }), expected);
            assert.equal(hitCount(path), 2);
        }
    });

    await t.test('shift_jis html, plain and gzip, via send and add', async () => {
        for (const path of ['/shiftjis', '/shiftjis.gz']) {
            const url = base + path;
            const expected = { charset: 'shift_jis', name: SJIS_TEXT, text: SJIS_TEXT };
            assertHtml(await crawler.send({ url }), expected);
            assertHtml(await addAndWait(crawler, { url }), expected);
            assert.equal(hitCount(path), 2);
        }
    });

    await t.test('json, plain and gzip, via send and add', async () => {
        for (const path of ['/json', '/json.gz']) {
            const url = base + path;
            assertJson(await crawler.send({ url, isJson: true }), { charset: 'utf-8' });
            assertJson(await addAndWait(crawler, { url, isJson: true }), { charset: 'utf-8' });
            assert.equal(hitCount(path), 2);
        }
    });

    await t.test('charset source precedence', async () => {
        // No HTTP charset -> fall back to the HTML <meta> declaration.
        const meta = { charset: 'iso-8859-1', name: LATIN1_NAME, text: LATIN1_NAME };
        assertHtml(await crawler.send({ url: base + '/latin1-meta' }), meta);
        assertHtml(await addAndWait(crawler, { url: base + '/latin1-meta' }), meta);

        // HTTP charset conflicts with <meta> -> the HTTP declaration wins.
        const conflict = { charset: 'utf-8', name: UTF8_TEXT, text: UTF8_TEXT };
        assertHtml(await crawler.send({ url: base + '/conflict' }), conflict);
        assertHtml(await addAndWait(crawler, { url: base + '/conflict' }), conflict);

        // Explicit `encoding` beats the detected (here: lying) HTTP charset;
        // the charset field still reports the detected declaration.
        const mislabeled = { charset: 'utf-8', name: LATIN1_NAME, text: `${LATIN1_NAME} ${LATIN1_WORD}` };
        assertHtml(await crawler.send({ url: base + '/mislabeled', encoding: 'iso-8859-1' }), mislabeled);
        assertHtml(await addAndWait(crawler, { url: base + '/mislabeled', encoding: 'iso-8859-1' }), mislabeled);

        // Explicit `encoding` also works when nothing is declared at all.
        const plain = { charset: null, name: SJIS_TEXT, text: SJIS_TEXT };
        assertHtml(await crawler.send({ url: base + '/shiftjis-plain', encoding: 'shift_jis' }), plain);
        assertHtml(await addAndWait(crawler, { url: base + '/shiftjis-plain', encoding: 'shift_jis' }), plain);

        // forceUTF8 decodes as UTF-8 even though the header says latin-1;
        // the charset field keeps reporting the detected declaration.
        const forced = { charset: 'iso-8859-1', name: UTF8_TEXT, text: UTF8_TEXT };
        assertHtml(await crawler.send({ url: base + '/utf8-mislabeled', forceUTF8: true }), forced);
        assertHtml(await addAndWait(crawler, { url: base + '/utf8-mislabeled', forceUTF8: true }), forced);

        // isJson also forces UTF-8 decoding against a latin-1 declaration,
        // and JSON responses never get a cheerio handle.
        assertJson(await crawler.send({ url: base + '/json-latin1-header', isJson: true }), { charset: 'iso-8859-1' });
        assertJson(await addAndWait(crawler, { url: base + '/json-latin1-header', isJson: true }), { charset: 'iso-8859-1' });

        for (const path of ['/latin1-meta', '/conflict', '/mislabeled', '/shiftjis-plain', '/utf8-mislabeled', '/json-latin1-header']) {
            assert.equal(hitCount(path), 2, `${path} should be hit exactly twice`);
        }
    });

    await t.test('encoding null keeps the exact entity bytes', async () => {
        // 0xF6 / 0xE9 would not survive a decode-as-utf8 -> re-encode round
        // trip, so byte equality proves no text stage ran in between.
        for (const path of ['/latin1', '/latin1.gz']) {
            const url = base + path;
            for (const res of [
                await crawler.send({ url, encoding: null, jQuery: false }),
                await addAndWait(crawler, { url, encoding: null, jQuery: false }),
            ]) {
                assert.ok(Buffer.isBuffer(res.body));
                assert.deepEqual(res.body, LATIN1_PAGE);
                assert.equal(res.charset, 'iso-8859-1');
                assert.equal(res.$, undefined);
            }
        }
        // 4 hits in this subtest plus 2 each from the latin-1 subtest.
        assert.equal(hitCount('/latin1'), 4);
        assert.equal(hitCount('/latin1.gz'), 4);
    });

    await t.test('interleaved encodings do not leak into later requests', async () => {
        const results = new Map();
        const drained = new Promise(resolve => crawler.once('drain', resolve));
        let remaining = 3;
        const allDone = new Promise((resolve, reject) => {
            const callback = key => (error, res, release) => {
                release();
                if (error) reject(error);
                else {
                    results.set(key, res);
                    if (--remaining === 0) resolve();
                }
            };
            crawler.add({ url: `${base}/gated/shiftjis`, encoding: 'shift_jis', callback: callback('shiftjis') });
            crawler.add({ url: `${base}/gated/utf8`, callback: callback('utf8') });
            crawler.add({ url: `${base}/gated/latin1`, callback: callback('latin1') });
        });
        await Promise.all([allDone, drained]);

        // The server answered in reverse arrival order; every request still
        // got the decoding its own response declared.
        assert.deepEqual(arrivals.slice(-3), ['/gated/shiftjis', '/gated/utf8', '/gated/latin1']);
        assert.equal(results.get('shiftjis').$('p').text(), SJIS_TEXT);
        assert.equal(results.get('utf8').$('p').text(), UTF8_TEXT);
        assert.equal(results.get('latin1').$('p').text(), `${LATIN1_NAME} ${LATIN1_WORD}`);

        // A later request without an explicit encoding must not inherit the
        // shift_jis choice made for an earlier request on the same crawler.
        const after = await crawler.send({ url: `${base}/gated/latin1` });
        assertHtml(after, { charset: 'iso-8859-1', name: LATIN1_NAME, text: `${LATIN1_NAME} ${LATIN1_WORD}` });

        assert.equal(hitCount('/gated/shiftjis'), 1);
        assert.equal(hitCount('/gated/utf8'), 1);
        assert.equal(hitCount('/gated/latin1'), 2);
    });

    await t.test('two crawler instances keep their own defaults', async () => {
        const latinDefault = new Crawler({ retries: 0, silence: true, encoding: 'iso-8859-1' });
        const utf8Default = new Crawler({ retries: 0, silence: true, forceUTF8: true });

        // Undeclared latin-1 bytes: only the instance defaulting to
        // iso-8859-1 reads them correctly.
        const latin = await latinDefault.send({ url: base + '/latin1-raw' });
        assertHtml(latin, { charset: null, name: LATIN1_NAME, text: `${LATIN1_NAME} ${LATIN1_WORD}` });
        const latinMojibake = await utf8Default.send({ url: base + '/latin1-raw' });
        assert.notEqual(latinMojibake.body.includes(LATIN1_NAME), true);

        // UTF-8 bytes mislabeled as latin-1: only the forceUTF8 instance
        // reads them correctly.
        const utf8 = await utf8Default.send({ url: base + '/utf8-mislabeled' });
        assertHtml(utf8, { charset: 'iso-8859-1', name: UTF8_TEXT, text: UTF8_TEXT });
        const utf8Mojibake = await latinDefault.send({ url: base + '/utf8-mislabeled' });
        assert.notEqual(utf8Mojibake.body.includes(UTF8_TEXT), true);

        // Re-check the first instance after the second one ran: defaults did
        // not drift across instances.
        const latinAgain = await latinDefault.send({ url: base + '/latin1-raw' });
        assertHtml(latinAgain, { charset: null, name: LATIN1_NAME, text: `${LATIN1_NAME} ${LATIN1_WORD}` });

        assert.equal(hitCount('/latin1-raw'), 3);
        assert.equal(hitCount('/utf8-mislabeled'), 2 + 2); // 2 from precedence subtest
    });

    await t.test('server saw every request exactly as planned', () => {
        const expected = {
            '/utf8': 2, '/utf8.gz': 2,
            '/latin1': 4, '/latin1.gz': 4,
            '/shiftjis': 2, '/shiftjis.gz': 2,
            '/json': 2, '/json.gz': 2,
            '/latin1-meta': 2, '/conflict': 2, '/mislabeled': 2,
            '/shiftjis-plain': 2, '/utf8-mislabeled': 4,
            '/json-latin1-header': 2, '/latin1-raw': 3,
            '/gated/shiftjis': 1, '/gated/utf8': 1, '/gated/latin1': 2,
        };
        const actual = Object.fromEntries(hits);
        assert.deepEqual(actual, expected);
        const total = Object.values(expected).reduce((a, b) => a + b, 0);
        assert.equal(arrivals.length, total);
    });
});
