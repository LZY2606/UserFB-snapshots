//////////////////////////////////////////
// Needle -- HTTP CONNECT tunneling
// Drives an HTTP proxy that only accepts CONNECT for HTTPS targets.
// MIT Licensed
//////////////////////////////////////////

var net       = require('net'),
    tls       = require('tls'),
    http      = require('http'),
    https     = require('https'),
    url       = require('url'),
    inherits  = require('util').inherits;

// TLS options that, when given to Needle, are forwarded to the target
// handshake. Verification stays on by default: we only pass through what
// the caller provided and never force rejectUnauthorized off.
var tls_option_keys = 'pfx key passphrase cert ca ciphers rejectUnauthorized secureProtocol checkServerIdentity'.split(' ');

function connect_error(statusCode) {
  var err = new Error('Tunnel CONNECT request failed with status ' + statusCode);
  err.code = 'ERR_NEEDLE_TUNNEL_CONNECT';
  err.proxyStatusCode = statusCode;
  return err;
}

//////////////////////////////////////////
// TunnelAgent: a keep-alive agent whose sockets are TLS connections
// established through an HTTP proxy via CONNECT. The agent itself keeps
// sockets keyed by target host:port, so a redirect chain reuses the
// tunnel to a previously visited origin instead of reconnecting.

function TunnelAgent(pool) {
  https.Agent.call(this, { keepAlive: true, maxFreeSockets: 256 });
  this.pool = pool;
}

inherits(TunnelAgent, https.Agent);

TunnelAgent.prototype.createConnection = function(options, callback) {
  var target = {
    hostname : options.hostname || options.host,
    port     : options.port || 443
  };

  this.pool.establish(target).then(function(tls_socket) {
    if (this.pool.destroyed) { // completed after an abort/timeout: do not leak it
      tls_socket.destroy();
      callback(new Error('Tunnel pool has been destroyed.'));
      return;
    }

    callback(null, tls_socket);
  }.bind(this), function(err) {
    callback(err);
  });
}

//////////////////////////////////////////
// TunnelPool: owns the agent and every in-flight CONNECT/TLS attempt of
// a single Needle call. One instance is shared by the whole redirect
// chain and torn down when the call reaches its terminal state.

function TunnelPool(config) {
  this.config    = config;
  this.destroyed = false;
  this.pending   = {}; // CONNECT/TLS attempts still in flight, keyed by host:port
  this.agent     = new TunnelAgent(this);
}

TunnelPool.prototype.key_for = function(target) {
  var port = target.port || 443;
  return net.isIP(target.hostname) === 6
    ? '[' + target.hostname + ']:' + port
    : target.hostname + ':' + port;
}

// Opens the raw proxy socket, sends CONNECT and upgrades to target TLS.
// Returns a promise for the ready TLS socket.
TunnelPool.prototype.establish = function(target) {
  var config    = this.config,
      proxy     = url.parse(config.proxy),
      port      = target.port || 443,
      authority = net.isIP(target.hostname) === 6
                    ? '[' + target.hostname + ']:' + port
                    : target.hostname + ':' + port,
      headers   = { host: authority };

  // CONNECT carries its own Host and, at most, the proxy credentials.
  // Target cookies/authorization/custom headers never reach this request.
  if (config.proxy_authorization)
    headers['proxy-authorization'] = config.proxy_authorization;

  var connect_options = {
    host   : proxy.hostname,
    port   : proxy.port || 80,
    method : 'CONNECT',
    path   : authority,
    headers: headers,
    agent  : false
  };

  ['localAddress', 'family', 'lookup'].forEach(function(key) {
    if (typeof config.http_opts[key] != 'undefined')
      connect_options[key] = config.http_opts[key];
  });

  var key    = this.key_for(target),
      handle = { target: target, request: null, rawSocket: null, tlsSocket: null, fail: null };
  this.pending[key] = handle;

  var self = this;

  return new Promise(function(resolve, reject) {
    var settled = false,
        request = http.request(connect_options);

    handle.request = request;

    function cleanup() { delete self.pending[key]; }

    function fail(err) {
      if (settled) return;
      settled = true;
      cleanup();
      reject(err);
    }
    handle.fail = fail;

    // teardown (abort/timeout/final release) destroys the underlying handles,
    // which may surface only as 'close': settle then so an in-flight handshake
    // cannot outlive its socket. Explicit errors take precedence via fail().
    function on_close() {
      fail(new Error('Tunnel connection closed before it was established.'));
    }

    // 2xx: the raw socket is now a tunnel, upgrade it with target TLS.
    request.on('connect', function(resp, raw_socket, head) {
      handle.request   = null;
      handle.rawSocket = raw_socket;

      // a proxy-side reset during the handshake must settle the attempt.
      raw_socket.on('error', function(err) {
        fail(err || new Error('Proxy socket error during the TLS handshake.'));
      });

      var tls_options = { socket: raw_socket, host: target.hostname };

      // SNI belongs to the target, and is omitted for IP literals.
      if (!net.isIP(target.hostname))
        tls_options.servername = target.hostname;

      tls_option_keys.forEach(function(option_key) {
        if (typeof config.http_opts[option_key] != 'undefined')
          tls_options[option_key] = config.http_opts[option_key];
      });

      var tls_socket = tls.connect(tls_options);
      handle.tlsSocket = tls_socket;

      tls_socket.once('secureConnect', function() {
        if (settled) return;
        settled = true;
        cleanup();
        resolve(tls_socket);
      });

      tls_socket.on('error', function(err) {
        tls_socket.destroy();
        raw_socket.destroy();
        fail(err); // preserve the original TLS error
      });

      tls_socket.once('close', on_close);
    });

    // anything but 2xx is delivered to the CONNECT request as a response.
    request.on('response', function(resp) {
      resp.on('error', fail);
      resp.resume();
      resp.on('end', function() {
        request.destroy();
        fail(connect_error(resp.statusCode));
      });
    });

    request.on('error', fail);

    request.end();
  });
}

// Releases every tunnel owned by this call, whether idle, busy or still
// being established. Destroying the agent closes its pooled TLS sockets,
// which in turn tears down the underlying raw proxy sockets.
TunnelPool.prototype.destroy = function() {
  if (this.destroyed) return;
  this.destroyed = true;

  var teardown = new Error('Tunnel pool was torn down before the connection was established.');

  // settle every attempt still in flight so its promise cannot hang, and
  // close whatever underlying handle it currently holds.
  Object.keys(this.pending).forEach(function(key) {
    var handle = this.pending[key];
    if (handle.tlsSocket) handle.tlsSocket.destroy();
    if (handle.rawSocket) handle.rawSocket.destroy();
    if (handle.request)   handle.request.destroy();
    if (handle.fail)      handle.fail(teardown);
  }, this);
  this.pending = {};

  this.agent.destroy();
}

module.exports = TunnelPool;
