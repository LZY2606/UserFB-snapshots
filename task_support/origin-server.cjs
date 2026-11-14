'use strict';
const http = require('node:http');

// Per-origin, per-path controllable behaviour. The defaults preserve the
// original baseline service: A's robots.txt is held until an explicit
// {type:'release', origin:'A'} message arrives.
const held = new Map();
const delays = new Map();
const robotsOverrides = new Map();
const robotsFailures = new Set();
const destroyPaths = new Set();
const servers = [];
let closing = false;
let requestSequence = 0;
// The baseline service always parks A's robots.txt until a release message.
// Test scenarios that drive holds explicitly can turn that default off.
let autoHoldRobots = true;

function key(origin, path) {
  return origin + " " + path;
}

function makeBody(reqUrl) {
  if (reqUrl === '/root') {
    return '<a href="/child">child</a>';
  }

  if (reqUrl === '/tree') {
    return ['<a href="/tree/1">1</a>',
      '<a href="/tree/2">2</a>',
      '<a href="/tree/3">3</a>'].join('');
  }

  return 'leaf';
}

for (const origin of ['A', 'B']) {
  const server = http.createServer((req, res) => {
    const id = ++requestSequence;
    process.send({type:'request', id, origin, path:req.url});

    const answer = () => {
      if (closing || res.writableEnded) {
        return;
      }

      if (req.url === '/robots.txt') {
        if (robotsFailures.has(origin)) {
          res.writeHead(500, {'Content-Type':'text/plain'});
          res.end('robots failure');
        } else {
          res.writeHead(200, {'Content-Type':'text/plain'});
          res.end(robotsOverrides.get(origin) ||
            'User-agent: *\nDisallow: /forbidden\n');
        }
      } else if (req.url === '/redir') {
        res.writeHead(302, {'Location':'/leaf'});
        res.end();
      } else {
        res.writeHead(200, {'Content-Type':'text/html'});
        res.end(makeBody(req.url));
      }

      process.send({type:'response-end', id, origin, path:req.url});
    };

    if (destroyPaths.has(key(origin, req.url))) {
      req.socket.destroy();
      process.send({type:'response-end', id, origin, path:req.url});
      return;
    }

    const requestKey = key(origin, req.url);

    // Baseline compatibility: A's robots.txt waits for a release message.
    if (autoHoldRobots && origin === 'A' && req.url === '/robots.txt' && !held.has(requestKey)) {
      held.set(requestKey, answer);
      return;
    }

    if (held.has(requestKey)) {
      // An explicit hold was armed before the request arrived.
      held.set(requestKey, answer);
      return;
    }

    const delay = delays.get(requestKey) || 0;

    if (delay) {
      delays.delete(requestKey);
      setTimeout(answer, delay);
    } else {
      answer();
    }
  });

  servers.push(server);
}

function releaseOrigin(origin) {
  for (const [requestKey, answer] of held) {
    if (requestKey.startsWith(origin + " ")) {
      held.delete(requestKey);
      if (answer) {
        answer();
      }
    }
  }
}

process.on('message', m => {
  if (m.type === 'release') {
    if (m.path) {
      const requestKey = key(m.origin, m.path);
      const answer = held.get(requestKey);
      held.delete(requestKey);
      if (answer) {
        answer();
      }
    } else {
      releaseOrigin(m.origin);
    }
  }

  if (m.type === 'hold') {
    held.set(key(m.origin, m.path), null);
  }

  if (m.type === 'delay') {
    delays.set(key(m.origin, m.path), m.ms);
  }

  if (m.type === 'robots-body') {
    robotsOverrides.set(m.origin, m.body);
  }

  if (m.type === 'robots-fail') {
    robotsFailures.add(m.origin);
  }

  if (m.type === 'destroy') {
    destroyPaths.add(key(m.origin, m.path));
  }

  if (m.type === 'auto-hold') {
    autoHoldRobots = m.enabled !== false;
  }

  if (m.type === 'close' && !closing) {
    closing = true;
    held.forEach(answer => answer && answer());
    held.clear();
    for (const server of servers) {
      server.closeAllConnections();
    }

    Promise.all(servers.map(s => new Promise(r => s.close(r)))).then(() => process.exit(0));
  }
});

process.on('disconnect', () => process.exit(0));
Promise.all(servers.map(s => new Promise(r => s.listen(0,'127.0.0.1',r))))
  .then(() => process.send({type:'ready', urls:servers.map(s => `http://127.0.0.1:${s.address().port}`)}));
