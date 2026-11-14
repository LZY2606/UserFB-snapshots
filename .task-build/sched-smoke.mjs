
import http from "node:http";
import { Duplex } from "node:stream";
import Crawler from "../dist/index.js";
function pair() {
  let peer;
  const a = new Duplex({ read() {}, write(c, _e, cb) { peer.push(c); cb(); }, final(cb) { peer.push(null); cb(); } });
  const b = new Duplex({ read() {}, write(c, _e, cb) { a.push(c); cb(); }, final(cb) { a.push(null); cb(); } });
  peer = b;
  return [a, b];
}
const server = http.createServer((req, res) => {
  res.writeHead(200, { "content-type": "text/plain; charset=ascii" });
  res.end("ascii:smoke");
});
const agent = { http: { addRequest(req) { const [c, s] = pair(); server.emit("connection", s); req.onSocket(c); } } };
const crawler = new Crawler({ jQuery: false, retries: 0, silence: true });
const result = await crawler.send({ url: "http://sched.local/smoke", agent });
if (result.body !== "ascii:smoke") throw new Error("smoke body mismatch: " + JSON.stringify(result.body));
console.log("SCHED_SMOKE_OK");
server.closeAllConnections();
server.close();
