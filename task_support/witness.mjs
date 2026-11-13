import assert from 'node:assert/strict';
import {normal} from './normal.mjs';
import {setup,sample} from './fixtures.mjs';
normal();const h=setup();h.add(sample('cdn-a',6400));h.add(sample('cdn-b',1600));const n=h.controller.getAverageThroughput('video');console.log(`last_location=cdn-b mixed_throughput=${n}`);assert.equal(n,4000);console.log(`location_scope_api=${typeof h.controller.getCurrentServiceLocation}`);h.controller.reset();
