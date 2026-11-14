'use strict';
const http = require('node:http');
const held = new Map();
const servers = [];
let closing = false;
for (const origin of ['A', 'B']) {
  const server = http.createServer((req, res) => {
    process.send({type:'request', origin, path:req.url});
    const answer = () => {
      if (req.url === '/robots.txt') {
        res.writeHead(200, {'Content-Type':'text/plain'});
        res.end('User-agent: *\nDisallow: /forbidden\n');
      } else {
        res.writeHead(200, {'Content-Type':'text/html'});
        res.end(req.url === '/root' ? '<a href="/child">child</a>' : 'leaf');
      }
    };
    if (origin === 'A' && req.url === '/robots.txt') held.set(origin, answer);
    else answer();
  });
  servers.push(server);
}
process.on('message', m => {
  if (m.type === 'release') {
    const answer = held.get(m.origin); held.delete(m.origin);
    if (answer) answer();
  }
  if (m.type === 'close' && !closing) {
    closing = true;
    held.forEach(answer => answer()); held.clear();
    for (const server of servers) server.closeAllConnections();
    Promise.all(servers.map(s => new Promise(r => s.close(r)))).then(() => process.exit(0));
  }
});
process.on('disconnect', () => process.exit(0));
Promise.all(servers.map(s => new Promise(r => s.listen(0,'127.0.0.1',r))))
  .then(() => process.send({type:'ready', urls:servers.map(s => `http://127.0.0.1:${s.address().port}`)}));
