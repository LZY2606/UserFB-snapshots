import assert from 'node:assert/strict';
import {setup,sample,Constants} from './fixtures.mjs';
export function normal(){const h=setup();h.add(sample('cdn-a',6400));h.add(sample('cdn-b',1600));assert.equal(h.controller.getAverageThroughput('video',Constants.THROUGHPUT_CALCULATION_MODES.ARITHMETIC_MEAN,2),4000);assert.equal(h.controller.getRawThroughputData('video').length,2);h.controller.reset();console.log('global_http_metrics=ok');}
