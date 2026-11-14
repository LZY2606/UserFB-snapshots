'use strict';
const http = require('node:http'), assert = require('node:assert/strict');
const { once } = require('node:events');
const needle = require('..');
function get(url, options) { return new Promise((resolve, reject) => needle.get(url, options, (err, resp) => err ? reject(err) : resolve(resp))); }
(async () => {
 let forwarded, connects = 0;
 const server = http.createServer((req, res) => {
  if (req.url === '/normal') { res.setHeader('Content-Type', 'application/json'); return res.end('{"normal":"needle"}'); }
  forwarded = { method: req.method, path: req.url, host: req.headers.host, auth: req.headers['proxy-authorization'] };
  res.setHeader('Content-Type', 'text/plain'); res.end('FORWARD_ONLY');
 });
 server.on('connect', (req, socket) => { connects++; socket.end('HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n'); });
 server.listen(0, '127.0.0.1'); await once(server, 'listening');
 const address = 'http://127.0.0.1:' + server.address().port;
 try {
  const normal = await get(address + '/normal', { use_proxy_from_env_var: false });
  assert.deepEqual(normal.body, { normal: 'needle' }); console.log('BASELINE_NEEDLE_JSON_OK');
  if (process.argv[2] !== 'normal') {
   const result = await get('https://source.invalid/api?q=7', { proxy: address, proxy_tunnel: true, use_proxy_from_env_var: false, proxy_user: 'demo', proxy_pass: 'example', open_timeout: 2000 });
   assert.equal(result.body, 'FORWARD_ONLY'); assert.equal(connects, 0);
   assert.equal(forwarded.method, 'GET'); assert.equal(forwarded.path, 'https://source.invalid/api?q=7');
   assert.equal(forwarded.host, 'source.invalid'); assert.equal(forwarded.auth, 'Basic ZGVtbzpleGFtcGxl');
   console.log('CURRENT_HTTPS_PROXY_IS_FORWARD_ONLY connects=' + connects + ' path=' + forwarded.path);
  }
 } finally { await new Promise(resolve => server.close(resolve)); }
})().catch(e => { console.error(e); process.exitCode = 1; });
