'use strict';

// End-to-end checks for the proxy_tunnel (HTTP CONNECT) transport.
//
// A loopback HTTP proxy and two loopback HTTPS services are stood up for
// each group. All assertions (c.check) are taken at real observation
// points: what the proxy received on CONNECT, what reached the targets
// over TLS, which sockets were reused, and which were torn down.

const http   = require('node:http'),
      https  = require('node:https'),
      net    = require('node:net'),
      needle = require('..'),
      tlsFixture = require('../task_support/tls-fixture.cjs');

const PROXY_USER = 'demo',
      PROXY_PASS = 'example',
      PROXY_AUTH = 'Basic ' + Buffer.from(PROXY_USER + ':' + PROXY_PASS).toString('base64');

function deferred() {
  let resolve;
  const promise = new Promise(function(r) { resolve = r; });
  return { promise: promise, resolve: resolve };
}

function wait_for(test, timeout) {
  const deadline = Date.now() + (timeout || 3000);
  return new Promise(function(resolve, reject) {
    (function poll() {
      let value;
      try { value = test(); } catch (err) { return reject(err); }
      if (value) return resolve(value);
      if (Date.now() > deadline) return reject(new Error('wait_for timed out'));
      setTimeout(poll, 15);
    })();
  });
}

// Starts the proxy, two HTTPS targets and a raw TCP holder (used to stall
// a TLS handshake). Returns handles plus observation logs and barriers.
async function start_fixture() {
  const fixture = tlsFixture();
  const tls_options = { key: fixture.key, cert: fixture.cert };

  const logs = {
    connects   : [], // every CONNECT seen by the proxy
    forwarded  : [], // plain-HTTP forward requests seen by the proxy
    targetReqs : [], // requests reaching the HTTPS targets
    sni        : []  // servername observed on each target TLS connection
  };

  const state = {
    hang       : new Set(),            // authorities whose CONNECT is held
    hangHandles: {},                   // authority -> deferred release handle
    reject     : new Set(),            // authorities whose CONNECT is rejected
    connSeq    : 0
  };

  function make_target() {
    const server = https.createServer(tls_options, function(req, res) {
      req.socket.__cid = req.socket.__cid || ++state.connSeq;
      logs.targetReqs.push({
        authority: 'localhost:' + server.address().port,
        method   : req.method,
        url      : req.url,
        connId   : req.socket.__cid,
        closed   : false,
        headers  : req.headers,
        socket   : req.socket
      });
      req.socket.once('close', function() {
        logs.targetReqs.forEach(function(entry) {
          if (entry.socket === req.socket) entry.closed = true;
        });
      });
      server.handler(req, res);
    });
    server.on('secureConnection', function(sock) {
      logs.sni.push(sock.servername);
    });
    server.handler = function() {};
    return server;
  }

  const target_a = make_target(),
        target_b = make_target();

  // raw TCP service that accepts and holds, so a TLS handshake stalls.
  const holder_sockets = [];
  const holder = net.createServer(function(socket) {
    socket._closed = false;
    socket.on('close', function() { socket._closed = true; });
    socket.on('error', function() {});
    holder_sockets.push(socket);
  });

  const proxy = http.createServer(function(req, res) {
    // plain-HTTP targets keep the original forward-proxy behaviour.
    logs.forwarded.push({
      method : req.method,
      url    : req.url,
      host   : req.headers.host,
      auth   : req.headers['proxy-authorization']
    });
    res.setHeader('Content-Type', 'text/plain');
    res.end('FORWARDED_BODY');
  });

  proxy.on('connect', function(req, clientSocket, head) {
    const authority = req.url;
    const record = {
      authority : authority,
      headers   : req.headers,
      socket    : clientSocket,
      closed    : false
    };
    clientSocket.on('close', function() { record.closed = true; });
    clientSocket.on('error', function() {});
    logs.connects.push(record);

    if (state.reject.has(authority)) {
      clientSocket.write('HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 0\r\n\r\n');
      clientSocket.end();
      return;
    }

    if (state.hang.has(authority)) {
      state.hangHandles[authority] = {
        release: function() { open_tunnel(authority, clientSocket, head); }
      };
      return; // hold the CONNECT response until the barrier is released
    }

    open_tunnel(authority, clientSocket, head);
  });

  function open_tunnel(authority, clientSocket, head) {
    const parts = authority.split(':'),
          host  = parts[0],
          port  = Number(parts[1]),
          upstream = net.connect(port, host);

    upstream.on('connect', function() {
      clientSocket.write('HTTP/1.1 200 Connection Established\r\n\r\n');
      if (head && head.length) upstream.write(head);
      upstream.pipe(clientSocket);
      clientSocket.pipe(upstream);
    });
    upstream.on('error', function() { clientSocket.destroy(); });
    clientSocket.on('error', function() { upstream.destroy(); });
    clientSocket.on('close', function() { upstream.destroy(); });
  }

  await new Promise(function(resolve) { proxy.listen(0, '127.0.0.1', resolve); });
  await new Promise(function(resolve) { target_a.listen(0, '127.0.0.1', resolve); });
  await new Promise(function(resolve) { target_b.listen(0, '127.0.0.1', resolve); });
  await new Promise(function(resolve) { holder.listen(0, '127.0.0.1', resolve); });

  const ports = {
    proxy   : proxy.address().port,
    a       : target_a.address().port,
    b       : target_b.address().port,
    holder  : holder.address().port
  };

  function stop() {
    return Promise.all([
      new Promise(function(r) { proxy.close(r); }),
      new Promise(function(r) { target_a.close(r); }),
      new Promise(function(r) { target_b.close(r); }),
      new Promise(function(r) { holder.close(r); })
    ]);
  }

  return { fixture: fixture, ports: ports, logs: logs, state: state,
           target_a: target_a, target_b: target_b, holder_sockets: holder_sockets,
           stop: stop };
}

function send(uri, options) {
  let resolve;
  const done = new Promise(function(r) { resolve = r; });
  const stream = needle.get(uri, options, function(err, resp, body) {
    resolve({ err: err, resp: resp, body: body });
  });
  return { stream: stream, done: done };
}

function base_options(fixture, extra) {
  return Object.assign({
    proxy                  : 'http://' + PROXY_USER + ':' + PROXY_PASS + '@127.0.0.1:' + fixture.ports.proxy,
    proxy_tunnel           : true,
    use_proxy_from_env_var : false,
    ca                     : fixture.fixture.cert,
    headers                : { 'X-Custom': 'custom-value' },
    cookies                : { sid: 'secret-cookie' }
  }, extra || {});
}

//////////////////////////////////////////
// Group 1: transport mechanics and two-layer header identity

async function TunnelTransport(c) {
  const fixture = await start_fixture();
  try {
    const ports = fixture.ports, logs = fixture.logs;
    const target_url = 'https://localhost:' + ports.a + '/api?query=hello';

    fixture.target_a.handler = function(req, res) {
      res.setHeader('Content-Type', 'application/json');
      res.end(JSON.stringify({ ok: true, path: req.url }));
    };

    const call = send(target_url, base_options(fixture, {
      headers: { 'X-Custom': 'custom-value', Authorization: 'Bearer target-token' }
    }));
    const result = await call.done;

    // the target response is delivered through the normal Needle entry point.
    c.check(!result.err, 'tunneled request should succeed');
    c.check(result.resp && result.resp.statusCode === 200, 'target status is 200');
    c.check(result.body && result.body.ok === true, 'JSON body is parsed from the target');

    await wait_for(function() { return logs.connects.length === 1; });
    const connect = logs.connects[0];

    // CONNECT line targets host:port, with only its own Host and proxy auth.
    c.check(connect.authority === 'localhost:' + ports.a, 'CONNECT authority is target host:port');
    c.check(connect.headers.host === 'localhost:' + ports.a, 'CONNECT Host is the target authority');
    c.check(connect.headers['proxy-authorization'] === PROXY_AUTH, 'CONNECT carries proxy credentials');
    c.check(!('cookie' in connect.headers), 'target Cookie is absent from CONNECT');
    c.check(!('authorization' in connect.headers), 'target Authorization is absent from CONNECT');
    c.check(!('x-custom' in connect.headers), 'custom target header is absent from CONNECT');

    await wait_for(function() { return logs.targetReqs.length === 1; });
    const inner = logs.targetReqs[0];

    // inside the tunnel: origin-form path and target identity headers.
    c.check(inner.url === '/api?query=hello', 'tunneled request uses path and query only');
    c.check(inner.headers.host === 'localhost:' + ports.a, 'inner Host is the target');
    c.check(inner.headers.authorization === 'Bearer target-token', 'target Authorization survives inside the tunnel');
    c.check(inner.headers.cookie === 'sid=secret-cookie', 'target Cookie survives inside the tunnel');
    c.check(inner.headers['x-custom'] === 'custom-value', 'custom header survives inside the tunnel');
    c.check(!('proxy-authorization' in inner.headers), 'proxy credentials never reach the target request');
    c.check(logs.sni[0] === 'localhost', 'SNI belongs to the target, not the proxy');

    // certificate verification stays on by default and honors ca / rejectUnauthorized.
    const insecure = send(target_url, base_options(fixture, { ca: undefined }));
    const insecure_result = await insecure.done;
    c.check(!!insecure_result.err, 'request without the CA is rejected');
    c.check(/CERT|SELF_SIGNED|UNABLE_TO_VERIFY/.test(insecure_result.err.code || ''),
            'TLS failure preserves the original certificate error, got: ' + (insecure_result.err && insecure_result.err.code));

    const permissive = send(target_url, base_options(fixture, { ca: undefined, rejectUnauthorized: false }));
    const permissive_result = await permissive.done;
    c.check(!permissive_result.err, 'rejectUnauthorized:false keeps its original meaning');

    // plain HTTP targets still use the forward-proxy path, never CONNECT.
    const connects_before = logs.connects.length;
    const forward = send('http://127.0.0.1:' + ports.proxy + '/plain', base_options(fixture));
    const forward_result = await forward.done;
    c.check(!forward_result.err && forward_result.body === 'FORWARDED_BODY', 'HTTP target is forwarded');
    c.check(logs.forwarded.length === 1, 'proxy received one forward request');
    c.check(logs.forwarded[0].url === 'http://127.0.0.1:' + ports.proxy + '/plain',
            'forward request keeps the absolute URL');
    c.check(logs.connects.length === connects_before, 'HTTP target issues no CONNECT');
  } finally {
    await fixture.stop();
  }
}

//////////////////////////////////////////
// Group 2: A -> B -> A redirect identity and tunnel reuse

async function RedirectIdentity(c) {
  const fixture = await start_fixture();
  try {
    const ports = fixture.ports, logs = fixture.logs;
    const a_start = 'https://localhost:' + ports.a + '/start',
          b_step  = 'https://localhost:' + ports.b + '/step',
          a_final = 'https://localhost:' + ports.a + '/final';

    // server-side barrier: B must not answer until A has been observed.
    const b_gate = deferred();

    fixture.target_a.handler = function(req, res) {
      if (req.url === '/start') {
        b_gate.resolve();
        res.statusCode = 302;
        res.setHeader('Location', b_step);
        res.end('redirecting-to-b'); // body must be drained before handoff
      } else {
        res.setHeader('Content-Type', 'text/plain');
        res.end('FINAL_BODY');
      }
    };

    fixture.target_b.handler = async function(req, res) {
      await b_gate.promise; // honor the ordering barrier
      res.statusCode = 302;
      res.setHeader('Location', a_final);
      res.end('redirecting-to-a');
    };

    const call = send(a_start, base_options(fixture, {
      follow_max: 3,
      headers   : { 'X-Custom': 'custom-value', Authorization: 'Bearer target-token' }
    }));
    const result = await call.done;

    c.check(!result.err, 'redirect chain should complete');
    c.check(result.resp.statusCode === 200, 'final response is 200');
    c.check(result.body === 'FINAL_BODY', 'final body is delivered once');

    await wait_for(function() { return logs.connects.length >= 2; });
    const a_connects = logs.connects.filter(function(x) { return x.authority === 'localhost:' + ports.a; }),
          b_connects = logs.connects.filter(function(x) { return x.authority === 'localhost:' + ports.b; });

    // one tunnel per origin: returning to A reuses its connection instead of reconnecting.
    c.check(a_connects.length === 1, 'A tunnel is CONNECTed exactly once across A->B->A');
    c.check(b_connects.length === 1, 'B tunnel is CONNECTed exactly once');
    c.check(a_connects[0] !== b_connects[0], 'A tunnel is never used to request B');

    await wait_for(function() { return logs.targetReqs.length === 3; });
    const start_req = logs.targetReqs.find(function(x) { return x.url === '/start'; }),
          step_req  = logs.targetReqs.find(function(x) { return x.url === '/step'; }),
          final_req = logs.targetReqs.find(function(x) { return x.url === '/final'; });

    c.check(step_req.authority === 'localhost:' + ports.b, 'B request lands on B');
    c.check(start_req.connId === final_req.connId, 'A connection is reused on the return hop');
    c.check(step_req.connId !== start_req.connId, 'B uses its own isolated connection');

    // cross-origin credential cleanup is preserved for target headers...
    c.check(!('authorization' in step_req.headers), 'B never receives the A Authorization');
    c.check(!('cookie' in step_req.headers), 'B never receives the A Cookie');
    c.check(!('authorization' in final_req.headers), 'stripped Authorization is not restored on return to A');
    c.check(!('cookie' in final_req.headers), 'stripped Cookie is not restored on return to A');

    // ...while proxy authentication stays independent and authenticates every tunnel.
    c.check(a_connects[0].headers['proxy-authorization'] === PROXY_AUTH, 'A CONNECT keeps proxy auth');
    c.check(b_connects[0].headers['proxy-authorization'] === PROXY_AUTH, 'new B tunnel still authenticates to the same proxy');
    c.check(!('proxy-authorization' in step_req.headers), 'proxy auth stays out of the target request');
  } finally {
    await fixture.stop();
  }
}

//////////////////////////////////////////
// Group 3: rejection, handshake abort and lifecycle cleanup

async function TunnelLifecycle(c) {
  const fixture = await start_fixture();
  try {
    const ports = fixture.ports, logs = fixture.logs, state = fixture.state;

    // -- 1. CONNECT rejection ------------------------------------------------
    const reject_authority = 'localhost:' + ports.a;
    state.reject.add(reject_authority);

    const rejected = send('https://' + reject_authority + '/denied', base_options(fixture));
    let err_events = 0;
    rejected.stream.on('err', function() { err_events++; });
    const rejected_result = await rejected.done;

    c.check(!!rejected_result.err, 'rejected CONNECT ends in an error');
    c.check(rejected_result.err.code === 'ERR_NEEDLE_TUNNEL_CONNECT', 'tunnel error code is set');
    c.check(rejected_result.err.proxyStatusCode === 407, 'proxy status code is reported');
    c.check(err_events === 1, 'terminal error is delivered exactly once');
    c.check(logs.targetReqs.filter(function(x) { return x.authority === reject_authority; }).length === 0,
            'rejected CONNECT never reaches the target');
    state.reject.delete(reject_authority);

    // -- 2. open_timeout covers a stalled TLS handshake ----------------------
    const stall_authority = 'localhost:' + fixture.ports.holder;
    const stalled = send('https://' + stall_authority + '/stall', base_options(fixture, {
      open_timeout: 120, response_timeout: 60000
    }));
    let timeout_type = null;
    stalled.stream.on('timeout', function(type) { timeout_type = type; });
    const stalled_result = await stalled.done;

    c.check(!!stalled_result.err, 'stalled handshake fails the call');
    c.check(timeout_type === 'open', 'open timeout covers CONNECT and the TLS handshake');
    await wait_for(function() { return fixture.holder_sockets.length > 0; });
    await wait_for(function() { return fixture.holder_sockets.every(function(s) { return s._closed; }); });
    const holder_all_closed = fixture.holder_sockets.length > 0
      && fixture.holder_sockets.every(function(s) { return s._closed; });
    c.check(holder_all_closed, 'late handshake leaves no held connection behind');
    c.check(logs.targetReqs.length === 0, 'no target request is produced by a stalled handshake');

    // -- 3. out.abort() while CONNECT is held --------------------------------
    const hang_authority = 'localhost:' + ports.b;
    state.hang.add(hang_authority);
    const aborted = send('https://' + hang_authority + '/hang', base_options(fixture));
    let abort_err_events = 0;
    aborted.stream.on('err', function() { abort_err_events++; });

    await wait_for(function() {
      return logs.connects.some(function(x) { return x.authority === hang_authority; });
    });
    const hang_record = logs.connects.find(function(x) { return x.authority === hang_authority; });

    aborted.stream.abort();
    const aborted_result = await aborted.done;
    c.check(!!aborted_result.err, 'abort terminates the pending call');
    c.check(abort_err_events === 1, 'abort terminal state is delivered once');
    await wait_for(function() { return hang_record.closed; });
    c.check(hang_record.closed, 'abort closes the in-flight proxy connection');

    // release the barrier after abort: the late connection must never carry a request.
    state.hangHandles[hang_authority].release();
    await new Promise(function(r) { setTimeout(r, 150); });
    c.check(logs.targetReqs.filter(function(x) { return x.authority === hang_authority; }).length === 0,
            'a late handshake after abort carries no target request');
    state.hang.delete(hang_authority);

    // -- 4. consuming the final body releases every idle tunnel --------------
    fixture.target_a.handler = function(req, res) {
      res.setHeader('Content-Type', 'text/plain');
      res.end('DONE');
    };
    const ok = send('https://localhost:' + ports.a + '/finish', base_options(fixture));
    const ok_result = await ok.done;
    c.check(!ok_result.err && ok_result.body === 'DONE', 'final response is fully consumed');

    const released = await wait_for(function() {
      return logs.targetReqs.find(function(x) { return x.url === '/finish' && x.closed; });
    });
    c.check(!!released, 'tunnel socket is released when the call reaches its terminal state');
  } finally {
    await fixture.stop();
  }
}

module.exports = {
  TunnelTransport: TunnelTransport,
  RedirectIdentity: RedirectIdentity,
  TunnelLifecycle: TunnelLifecycle
};
