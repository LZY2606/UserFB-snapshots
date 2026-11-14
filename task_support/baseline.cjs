'use strict';
const assert = require('node:assert/strict');
const {fork} = require('node:child_process');
const {once} = require('node:events');
const Crawler = require('../lib');
(async () => {
  const child = fork(require.resolve('./origin-server.cjs'), [], {stdio:['ignore','inherit','inherit','ipc']});
  const requests = [];
  const ready = new Promise(resolve => child.on('message',m => {if(m.type==='ready')resolve(m.urls);}));
  const firstA = new Promise(resolve => child.on('message',m => {
    if(m.type==='request') {requests.push(m);if(m.origin==='A' && m.path==='/robots.txt')resolve();}
  }));
  const timer = setTimeout(() => {console.error('baseline timeout');process.exitCode=1;child.kill();},10000);
  let crawler, interval;
  try {
    const [a,b] = await ready;
    crawler = new Crawler(a+'/root');
    crawler.interval=100000; crawler.maxConcurrency=2;
    crawler.on('crawlstart',()=>crawler.queueURL(b+'/root'));
    const delivered=[];crawler.on('fetchcomplete',item=>delivered.push(item.url));
    const done=once(crawler,'complete');
    crawler.start();await firstA;
    for(let n=0;n<8;n++)crawler.crawl();
    assert.equal(crawler.fetchingRobotsTxt,true);
    assert.equal(requests.filter(r=>r.origin==='B').length,0);
    console.log('OBSERVED global robots barrier: A held; 8 crawl ticks; B requests=0');
    child.send({type:'release',origin:'A'});
    interval=setInterval(()=>crawler.crawl(),5);
    await done;
    assert.deepEqual(new Set(delivered),new Set([a+'/root',a+'/child',b+'/root',b+'/child']));
    assert.equal(requests.filter(r=>r.path==='/robots.txt').length,2);
    console.log('BASELINE production HTTP crawl delivered=4 robots=2 normal completion');
  } finally {
    clearTimeout(timer);clearInterval(interval);
    if(crawler) {crawler.stop(true);crawler.httpAgent.destroy();crawler.httpsAgent.destroy();}
    if(child.connected)child.send({type:'close'});
    if(child.exitCode===null)await once(child,'exit');
  }
})().catch(e=>{console.error(e);process.exitCode=1;});
