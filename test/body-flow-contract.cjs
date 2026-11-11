'use strict';

// Contract tests for BODY literal flow: backpressure between the source stream
// and the body Readable, abandoning the current section with destroy(), and the
// public fetch()/seq.fetch() event contract on top of a local TCP responder.

const test = require('node:test');
const assert = require('node:assert/strict');
const net = require('node:net');
const { Readable } = require('node:stream');
const { Parser } = require('../lib/Parser');
const Imap = require('../lib/Connection');

const CRLF = '\r\n';

function payload(byte, len) {
  return Buffer.alloc(len, byte);
}

// A real Readable with an externally visible read() counter. `chunks` is a
// queue of Buffers pushed one per read(); byteAtATime additionally splits every
// chunk into single-byte pushes.
function makeCountingSource(chunks, options) {
  options = options || {};
  const queue = chunks.slice();
  let reads = 0;
  const src = new Readable({
    highWaterMark: options.highWaterMark === undefined ? 1024
                                                      : options.highWaterMark,
    read() {
      ++reads;
      if (!queue.length) {
        this.push(null);
        return;
      }
      let next = queue.shift();
      if (options.byteAtATime && next.length > 1) {
        queue.unshift(next.subarray(1));
        next = next.subarray(0, 1);
      }
      this.push(next);
    }
  });
  return {
    src,
    reads() { return reads; }
  };
}

function bodyChunks(headerLine, body, tail) {
  const chunks = [Buffer.from(headerLine)];
  for (let i = 0; i < body.length; i += 1024)
    chunks.push(body.subarray(i, i + 1024));
  chunks.push(Buffer.from(tail));
  return chunks;
}

function collect(stream) {
  const chunks = [];
  return new Promise((resolve, reject) => {
    stream.on('data', (c) => chunks.push(c));
    stream.on('error', reject);
    stream.on('end', () => resolve(Buffer.concat(chunks)));
  });
}

function nextTagged(parser) {
  return new Promise((resolve) => parser.once('tagged', resolve));
}

function tick(n) {
  n = n || 1;
  let p = Promise.resolve();
  while (n-- > 0)
    p = p.then(() => new Promise((r) => setImmediate(r)));
  return p;
}

// explicit gate: resolve as soon as `predicate` holds; the timeout is only a
// failure backstop
function waitFor(predicate, timeout) {
  return new Promise((resolve, reject) => {
    const iv = setInterval(() => {
      if (predicate()) {
        clearInterval(iv);
        clearTimeout(timer);
        resolve();
      }
    }, 5);
    const timer = setTimeout(() => {
      clearInterval(iv);
      reject(new Error('waitFor timed out'));
    }, timeout || 3000);
  });
}

// Section whose content must never be interpreted as protocol data: contains
// CRLF, a closing paren and a fake tagged OK line.
function trickyPayload(len) {
  const base = payload(0x61, len);
  const tricky = Buffer.from('aaaa\r\n)\r\nA5 OK definitely just body text\r\n');
  return Buffer.concat([base.subarray(0, 64), tricky, base.subarray(64)]);
}

test('BODY flow contract', async (t) => {
  // ---------------------------------------------------------------- pressure
  await t.test('backpressure stops source reads and resumes losslessly', async () => {
    const bytes = payload(0x78, 128 * 1024);
    const chunks = bodyChunks(
      `* 1 FETCH (BODY[TEXT] {${bytes.length}}${CRLF}`,
      bytes,
      ` UID 7)${CRLF}A5 OK done${CRLF}`
    );
    const { src, reads } = makeCountingSource(chunks);
    const parser = new Parser(src);

    let bodyCount = 0;
    const bodyPromise = new Promise((resolve) => {
      parser.on('body', (stream) => {
        ++bodyCount;
        resolve(stream);
      });
    });
    const tagged = nextTagged(parser);
    const stream = await bodyPromise;

    // consumer is busy: leave the stream paused and let the parser settle
    await tick(4);
    const hwm = stream.readableHighWaterMark;
    assert.equal(stream.readableLength, hwm,
                 'body buffering caps at its highWaterMark');
    const readsWhenFull = reads();

    // more source readable notifications, of any origin, must not bypass the
    // paused state
    src.emit('readable');
    await tick(2);
    src.emit('readable');
    await tick(2);
    assert.equal(reads(), readsWhenFull,
                 'no extra source reads while the body is full');
    assert.equal(stream.readableLength, hwm);

    // now the consumer starts reading; the literal must continue exactly where
    // it stopped, without duplicated or missing bytes
    const got = collect(stream);
    const [content] = await Promise.all([got, tagged]);
    assert.equal(Buffer.compare(content, bytes), 0, 'full body content');
    assert.equal(bodyCount, 1, 'a single body event for the section');
    assert.ok(reads() > readsWhenFull, 'source reads resumed on demand');
  });

  await t.test('literal boundary and the next response arrive in one chunk', async () => {
    const bytes = payload(0x62, 4000);
    const chunks = [
      Buffer.from(`* 1 FETCH (BODY[TEXT] {${bytes.length}}${CRLF}`),
      bytes,
      Buffer.from(` UID 9)${CRLF}A5 OK done${CRLF}`)
    ];
    const { src } = makeCountingSource(chunks, { highWaterMark: 64 * 1024 });
    const parser = new Parser(src);
    const tagged = nextTagged(parser);
    const body = await new Promise((r) => parser.on('body', r));
    const [content] = await Promise.all([collect(body), tagged]);
    assert.equal(Buffer.compare(content, bytes), 0);
  });

  await t.test('byte-at-a-time source input', async () => {
    const bytes = trickyPayload(3000);
    const chunks = bodyChunks(
      `* 1 FETCH (BODY[TEXT] {${bytes.length}}${CRLF}`,
      bytes,
      `)${CRLF}A5 OK done${CRLF}`
    );
    const { src } = makeCountingSource(chunks, { byteAtATime: true });
    const parser = new Parser(src);
    const tagged = nextTagged(parser);
    const body = await new Promise((r) => parser.on('body', r));
    const [content] = await Promise.all([collect(body), tagged]);
    assert.equal(Buffer.compare(content, bytes), 0,
                 'every byte delivered once, in order');
  });

  await t.test('repeated pause and resume from the consumer', async () => {
    const bytes = payload(0x63, 256 * 1024);
    const chunks = bodyChunks(
      `* 1 FETCH (BODY[TEXT] {${bytes.length}}${CRLF}`,
      bytes,
      `)${CRLF}A5 OK done${CRLF}`
    );
    const { src } = makeCountingSource(chunks);
    const parser = new Parser(src);
    const tagged = nextTagged(parser);
    const body = await new Promise((r) => parser.on('body', r));

    const got = [];
    let total = 0;
    while (total < bytes.length) {
      await tick(1); // stay paused for a turn between every burst
      const part = body.read(4096);
      if (part === null)
        await new Promise((r) => body.once('readable', r));
      else {
        got.push(part);
        total += part.length;
      }
    }
    await tagged;
    assert.equal(Buffer.compare(Buffer.concat(got), bytes), 0,
                 'pause/resume cycles keep the byte sequence intact');
    body.resume();
    await new Promise((r) => body.once('end', r));
  });

  await t.test('empty body ends naturally and parsing continues', async () => {
    const chunks = [Buffer.from(
      `* 1 FETCH (BODY[TEXT] {0}${CRLF} UID 3)${CRLF}A5 OK done${CRLF}`
    )];
    const { src } = makeCountingSource(chunks);
    const parser = new Parser(src);
    let dataEvents = 0, endEvents = 0, closeEvents = 0;
    parser.on('body', (stream) => {
      stream.on('data', () => ++dataEvents);
      stream.on('end', () => ++endEvents);
      stream.on('close', () => ++closeEvents);
    });
    await nextTagged(parser);
    await tick(2);
    assert.equal(dataEvents, 0);
    assert.equal(endEvents, 1);
    assert.equal(closeEvents, 1);
  });

  console.log('IMAP_PRESSURE_OK');

  // ---------------------------------------------------------------- discard
  // Runs one destroy() scenario. The first section holds protocol-looking
  // content and is abandoned; the second section must survive untouched.
  async function discardScenario(timing) {
    const first = trickyPayload(96 * 1024);
    const second = Buffer.from('second section keeps every byte');
    const chunks = [
      Buffer.from(`* 1 FETCH (BODY[1] {${first.length}}${CRLF}`),
    ];
    for (let i = 0; i < first.length; i += 1024)
      chunks.push(first.subarray(i, i + 1024));
    chunks.push(Buffer.from(` BODY[2] {${second.length}}${CRLF}`));
    chunks.push(second);
    chunks.push(Buffer.from(
      ` UID 7 FLAGS (\\Seen) INTERNALDATE "05-Sep-2004 00:38:03 +0000")${CRLF}`
      + `A5 OK done${CRLF}`
    ));
    const { src } = makeCountingSource(chunks);
    const parser = new Parser(src);

    const seen = [];
    let endOnDestroyed = 0, closeOnDestroyed = 0, dataAfterDestroy = 0;
    let abandoned = false, bufferedAtDestroy = 0;
    let destroyedStream;
    parser.on('body', (stream, info) => {
      seen.push(info.which);
      if (info.which === '1') {
        destroyedStream = stream;
        stream.on('end', () => ++endOnDestroyed);
        stream.on('close', () => ++closeOnDestroyed);
        if (timing === 'immediate') {
          abandoned = true;
          stream.destroy();
        } else if (timing === 'full') {
          // stay paused so the buffer fills against backpressure
          setTimeout(() => {
            bufferedAtDestroy = stream.readableLength;
            abandoned = true;
            stream.destroy();
          }, 60);
        } else {
          let consumed = 0;
          stream.on('data', (d) => {
            if (abandoned) {
              ++dataAfterDestroy;
              return;
            }
            consumed += d.length;
            if (consumed >= 8192) {
              stream.pause();
              setTimeout(() => {
                abandoned = true;
                stream.destroy();
              }, 20);
            }
          });
        }
      } else {
        stream.on('data', () => {});
        stream.on('end', () => {});
      }
    });

    const tagged = await Promise.race([
      nextTagged(parser),
      new Promise((_, reject) => setTimeout(
        () => reject(new Error('tagged OK never reached after ' + timing)), 3000))
    ]);
    assert.equal(tagged.tagnum, 5);
    assert.deepEqual(seen, ['1', '2'], 'each section emits exactly one body');
    assert.equal(endOnDestroyed, 0, 'destroyed stream emits no end');
    assert.equal(closeOnDestroyed, 1, 'destroyed stream closes once');
    assert.equal(dataAfterDestroy, 0, 'no synthetic data after destroy');
    assert.equal(destroyedStream.readable, false);
    if (timing === 'full')
      assert.equal(bufferedAtDestroy, destroyedStream.readableHighWaterMark,
                   'full timing really hit the high water mark');
  }

  await t.test('destroy() immediately after receiving the body', async () => {
    await discardScenario('immediate');
  });

  await t.test('destroy() after the body buffer filled up', async () => {
    await discardScenario('full');
  });

  await t.test('destroy() after part of the body was consumed', async () => {
    await discardScenario('partial');
  });

  await t.test('a following section is fully delivered after discard', async () => {
    const first = trickyPayload(70 * 1024);
    const second = Buffer.from('surviving section \r\nA5 OK fake\r\n end');
    const chunks = [
      Buffer.from(`* 1 FETCH (BODY[1] {${first.length}}${CRLF}`),
      first,
      Buffer.from(` BODY[2] {${second.length}}${CRLF}`),
      second,
      Buffer.from(` UID 8)${CRLF}A5 OK done${CRLF}`)
    ];
    const { src } = makeCountingSource(chunks, { highWaterMark: 128 * 1024 });
    const parser = new Parser(src);
    let secondContent;
    parser.on('body', (stream, info) => {
      if (info.which === '1')
        setImmediate(() => stream.destroy());
      else
        collect(stream).then((buf) => { secondContent = buf; });
    });
    await nextTagged(parser);
    await tick(3);
    assert.ok(secondContent && secondContent.equals(second),
              'the next section is not discarded with the first');
  });

  console.log('IMAP_DISCARD_OK');

  // ------------------------------------------------------------- fetch / TCP
  await t.test('public fetch() and seq.fetch() survive a discarded section', async (t2) => {
    const body1 = trickyPayload(96 * 1024);
    const body2 = Buffer.from('section two payload');
    const commands = [];

    const server = net.createServer((sock) => {
      sock.write(`* OK ready${CRLF}`);
      let buf = '';
      const responses = {
        CAPABILITY: `* CAPABILITY IMAP4rev1 UNSELECT IDLE NAMESPACE QUOTA CHILDREN${CRLF}`
                    + `A0 OK caps${CRLF}`,
        LOGIN: `* CAPABILITY IMAP4rev1 UNSELECT IDLE NAMESPACE QUOTA CHILDREN${CRLF}`
               + `A1 OK authenticated${CRLF}`,
        NAMESPACE: `* NAMESPACE (("" "/")) NIL NIL${CRLF}A2 OK Success${CRLF}`,
        LIST: `* LIST (\\NoSelect) "/" "/"${CRLF}A3 OK Success${CRLF}`,
        EXAMINE: [
          '* FLAGS (\\Answered \\Flagged \\Draft \\Deleted \\Seen)',
          '* OK [PERMANENTFLAGS ()] Flags permitted.',
          '* OK [UIDVALIDITY 2] UIDs valid.',
          '* 2 EXISTS',
          '* 0 RECENT',
          '* OK [UIDNEXT 4422] Predicted next UID.',
          'A4 OK [READ-ONLY] INBOX selected. (Success)',
          ''
        ].join(CRLF)
      };
      function fetchResponse(seqno, uid) {
        return `* ${seqno} FETCH (BODY[1] {${body1.length}}${CRLF}`
               + body1.toString('latin1')
               + ` BODY[2] {${body2.length}}${CRLF}`
               + body2.toString('latin1')
               + ` UID ${uid} FLAGS (\\Seen)`
               + ` INTERNALDATE "05-Sep-2004 00:38:03 +0000")${CRLF}`;
      }
      sock.on('data', (data) => {
        buf += data.toString('latin1');
        const lines = buf.split(CRLF);
        buf = lines.pop();
        for (const line of lines) {
          commands.push(line);
          if (/^A5 UID FETCH /.test(line)) {
            sock.write(fetchResponse(1, 101));
            sock.write(fetchResponse(2, 102));
            sock.write(`A5 OK Success${CRLF}`);
          } else if (/^A6 FETCH /.test(line)) {
            sock.write(fetchResponse(1, 101));
            sock.write(fetchResponse(2, 102));
            sock.write(`A6 OK Success${CRLF}`);
          } else if (/^A7 LOGOUT/.test(line)) {
            sock.write(`* BYE LOGOUT Requested${CRLF}A7 OK good day${CRLF}`);
            sock.end();
          } else {
            for (const key of Object.keys(responses)) {
              if (line.indexOf(' ' + key) !== -1
                  || line.indexOf(key) === 3) {
                sock.write(responses[key]);
                break;
              }
            }
          }
        }
      });
    });

    await new Promise((resolve, reject) => {
      server.once('error', reject);
      server.listen(0, '127.0.0.1', resolve);
    });
    t2.after(() => server.close());
    const port = server.address().port;

    const imap = new Imap({
      user: 'foo',
      password: 'bar',
      host: '127.0.0.1',
      port,
      keepalive: false
    });
    t2.after(() => { try { imap.destroy(); } catch (_) {} });

    await new Promise((resolve, reject) => {
      imap.once('ready', resolve);
      imap.once('error', reject);
      imap.connect();
    });
    await new Promise((resolve, reject) => {
      imap.openBox('INBOX', true, (err) => err ? reject(err) : resolve());
    });

    async function runFetch(fetchFactory) {
      const f = fetchFactory();
      const messages = [];
      let fetchEnds = 0;
      const done = new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error('fetch stalled')), 5000);
        f.once('error', reject);
        f.on('end', () => { ++fetchEnds; clearTimeout(timer); resolve(); });
      });

      f.on('message', (msg, seqno) => {
        const record = {
          seqno,
          bodies: new Map(),
          bodyEvents: 0,
          attributes: null,
          ends: 0
        };
        messages.push(record);
        msg.on('body', (stream, info) => {
          ++record.bodyEvents;
          if (seqno === 1 && info.which === '1') {
            // read the beginning, decide the section is uninteresting, then
            // abandon only this section
            stream.once('readable', function onReadable() {
              const head = stream.read(16);
              assert.ok(head && head.length === 16);
              stream.destroy();
            });
            record.bodies.set(info.which, null);
          } else {
            collect(stream).then((buf) => record.bodies.set(info.which, buf));
          }
        });
        msg.once('attributes', (attrs) => { record.attributes = attrs; });
        msg.on('end', () => { ++record.ends; });
      });

      await done;
      assert.equal(fetchEnds, 1, "fetch 'end' fires exactly once");
      assert.equal(messages.length, 2, 'both messages delivered');
      for (const msg of messages) {
        assert.equal(msg.bodyEvents, 2, 'both sections announced');
        assert.equal(msg.ends, 1, "message 'end' fires once");
        assert.ok(msg.attributes, 'attributes collected');
        assert.equal(msg.attributes.flags.length, 1);
        assert.ok(msg.attributes.date instanceof Date);
        assert.ok(typeof msg.attributes.uid === 'number');
        assert.ok(msg.bodies.get('2').equals(body2),
                  'section 2 content intact for seqno ' + msg.seqno);
      }
      assert.equal(messages[0].attributes.uid, 101);
      assert.equal(messages[1].attributes.uid, 102);
      assert.equal(messages[0].bodies.get('1'), null, 'section 1 was discarded');
    }

    await runFetch(() => imap.fetch([101, 102], { bodies: ['1', '2'] }));
    await runFetch(() => imap.seq.fetch([1, 2], { bodies: ['1', '2'] }));

    assert.ok(commands.some((l) =>
      l === 'A5 UID FETCH 101,102 (UID FLAGS INTERNALDATE BODY.PEEK[1] BODY.PEEK[2])'),
      'UID fetch command shape');
    assert.ok(commands.some((l) =>
      l === 'A6 FETCH 1,2 (UID FLAGS INTERNALDATE BODY.PEEK[1] BODY.PEEK[2])'),
      'seqno fetch command shape');

    imap.end();
    await waitFor(() => commands.some((l) => /^A7 LOGOUT/.test(l)), 3000);
  });

  await t.test('unrequested BODY sections are drained without stalling fetch', async (t2) => {
    const extra = payload(0x5a, 256 * 1024);
    const commands = [];
    const server = net.createServer((sock) => {
      sock.write(`* OK ready${CRLF}`);
      let buf = '';
      const canned = {
        CAPABILITY: `* CAPABILITY IMAP4rev1 NAMESPACE${CRLF}A0 OK caps${CRLF}`,
        LOGIN: `* CAPABILITY IMAP4rev1 NAMESPACE${CRLF}A1 OK auth${CRLF}`,
        NAMESPACE: `* NAMESPACE (("" "/")) NIL NIL${CRLF}A2 OK Success${CRLF}`,
        LIST: `* LIST (\\NoSelect) "/" "/"${CRLF}A3 OK Success${CRLF}`,
        EXAMINE: [
          '* FLAGS (\\Seen)',
          '* OK [UIDVALIDITY 2] ok',
          '* 1 EXISTS',
          'A4 OK [READ-ONLY] INBOX selected. (Success)',
          ''
        ].join(CRLF)
      };
      sock.on('data', (data) => {
        buf += data.toString('latin1');
        const lines = buf.split(CRLF);
        buf = lines.pop();
        for (const line of lines) {
          commands.push(line);
          if (/^A5 UID FETCH /.test(line)) {
            sock.write(`* 1 FETCH (UID 55 FLAGS (\\Seen)`
                       + ` INTERNALDATE "05-Sep-2004 00:38:03 +0000"`
                       + ` BODY[TEXT] {${extra.length}}${CRLF}`
                       + extra.toString('latin1')
                       + `)${CRLF}A5 OK Success${CRLF}`);
          } else if (/^A6 LOGOUT/.test(line)) {
            sock.write(`* BYE${CRLF}A6 OK bye${CRLF}`);
            sock.end();
          } else {
            for (const key of Object.keys(canned)) {
              if (line.indexOf(' ' + key) !== -1 || line.indexOf(key) === 3) {
                sock.write(canned[key]);
                break;
              }
            }
          }
        }
      });
    });
    await new Promise((resolve, reject) => {
      server.once('error', reject);
      server.listen(0, '127.0.0.1', resolve);
    });
    t2.after(() => server.close());

    const imap = new Imap({
      user: 'foo', password: 'bar',
      host: '127.0.0.1', port: server.address().port,
      keepalive: false
    });
    t2.after(() => { try { imap.destroy(); } catch (_) {} });

    await new Promise((resolve, reject) => {
      imap.once('ready', resolve);
      imap.once('error', reject);
      imap.connect();
    });
    await new Promise((resolve, reject) => {
      imap.openBox('INBOX', true, (err) => err ? reject(err) : resolve());
    });

    const f = imap.fetch([55]); // no bodies requested
    let messages = 0, fetchEnded = false, attrs = null;
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('auto-drain stalled')), 5000);
      f.on('message', (msg) => {
        ++messages;
        msg.once('attributes', (a) => { attrs = a; });
      });
      f.once('error', reject);
      f.on('end', () => {
        fetchEnded = true;
        clearTimeout(timer);
        resolve();
      });
    });
    assert.equal(messages, 1);
    assert.equal(fetchEnded, true);
    assert.equal(attrs.uid, 55);
    imap.end();
    await waitFor(() => commands.some((l) => /^A6 LOGOUT/.test(l)), 3000);
  });

  console.log('IMAP_FETCH_OK');
});
