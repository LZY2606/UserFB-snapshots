/**
 * Tests for the optional throughput isolation by service location.
 *
 * Measurements are injected through the regular METRIC_ADDED event of the HTTP metrics chain, so the
 * production path (MetricsModel event -> ThroughputController -> ThroughputModel) is exercised.
 * The ABR test uses the real ThroughputRule with a rule context that delegates to the real
 * ThroughputController and a fixed representation ladder. No network requests are performed.
 */
import test from 'node:test';
import assert from 'node:assert/strict';

import Settings from '../../src/core/Settings.js';
import EventBus from '../../src/core/EventBus.js';
import ThroughputController from '../../src/streaming/controllers/ThroughputController.js';
import ThroughputRule from '../../src/streaming/rules/abr/ThroughputRule.js';
import MediaPlayerEvents from '../../src/streaming/MediaPlayerEvents.js';
import MetricsConstants from '../../src/streaming/constants/MetricsConstants.js';
import Constants from '../../src/streaming/constants/Constants.js';
import {HTTPRequest} from '../../src/streaming/vo/metrics/HTTPRequest.js';

const MEDIA_SEGMENT = HTTPRequest.MEDIA_SEGMENT_TYPE;

/**
 * Builds a real ThroughputController connected to the EventBus. All settings that influence the
 * calculations are set explicitly, so expected values can be derived independently.
 */
function createController(options = {}) {
    const context = {};
    const settings = Settings(context).getInstance();
    settings.update({
        streaming: {
            abr: {
                throughput: {
                    isolateByServiceLocation: options.isolate === true,
                    averageCalculationMode: Constants.THROUGHPUT_CALCULATION_MODES.ARITHMETIC_MEAN,
                    useResourceTimingApi: false,
                    useNetworkInformationApi: {fetch: false, xhr: false},
                    useDeadTimeLatency: true,
                    bandwidthSafetyFactor: options.safetyFactor ?? 0.9,
                    sampleSettings: {
                        live: options.live ?? 3,
                        vod: options.vod ?? 4,
                        enableSampleSizeAdjustment: false,
                        maxMeasurementsToKeep: options.maxMeasurementsToKeep ?? 4,
                        averageLatencySampleAmount: options.averageLatencySampleAmount ?? 4
                    },
                    ewma: {
                        throughputSlowHalfLifeSeconds: options.slowHalfLife ?? 8,
                        throughputFastHalfLifeSeconds: options.fastHalfLife ?? 3,
                        latencySlowHalfLifeCount: options.latencySlowHalfLife ?? 2,
                        latencyFastHalfLifeCount: options.latencyFastHalfLife ?? 1,
                        weightDownloadTimeMultiplicationFactor: 0.0015
                    }
                }
            }
        }
    });

    const controller = ThroughputController(context).getInstance();
    controller.setConfig({
        settings,
        playbackController: {getIsDynamic: () => false}
    });
    controller.initialize();

    const eventBus = EventBus(context).getInstance();

    return {
        context,
        settings,
        controller,
        eventBus,
        add(metric, mediaType = Constants.VIDEO) {
            eventBus.trigger(MediaPlayerEvents.METRIC_ADDED, {
                metric: MetricsConstants.HTTP_REQUEST,
                mediaType,
                value: metric
            });
        },
        addAudio(metric) {
            this.add(metric, Constants.AUDIO);
        },
        reset() {
            controller.reset();
            settings.reset();
        }
    };
}

/**
 * Creates a MediaSegment HTTP metric. Throughput is derived from the fetch trace:
 *   throughput = 8 * downloadedBytes / downloadTimeInMs
 * We provide `kbit * downloadTimeInMs / 8` bytes, so the resulting value is exactly `kbit`
 * regardless of the download time. `latency` is the time between trequest and tresponse.
 */
function mediaSegment({serviceLocation, kbit, latency = 50, downloadTimeInMs = 1000}) {
    return {
        type: MEDIA_SEGMENT,
        url: 'https://media.invalid/segment.m4s',
        _serviceLocation: serviceLocation,
        trequest: new Date(0),
        tresponse: new Date(latency),
        _tfinish: new Date(latency + downloadTimeInMs),
        _fileLoaderType: Constants.FILE_LOADER_TYPES.FETCH,
        trace: [{s: new Date(latency), d: downloadTimeInMs, b: [kbit * downloadTimeInMs / 8]}]
    };
}

/** MPD measurement, never part of a media service location. */
function mpdRequest(serviceLocation) {
    const request = mediaSegment({serviceLocation, kbit: 9000});
    request.type = HTTPRequest.MPD_TYPE;
    return request;
}

/** Reference time of 0 ms is below cacheLoadThresholds.video, therefore classified as cached. */
function cachedSegment(serviceLocation) {
    const request = mediaSegment({serviceLocation, kbit: 9000, latency: 0});
    request._tfinish = new Date(0);
    return request;
}

/** Zero download time results in a NaN throughput and must be ignored. */
function invalidSegment(serviceLocation) {
    const request = mediaSegment({serviceLocation, kbit: 9000});
    request.trace = [{s: new Date(200), d: 0, b: [0]}];
    return request;
}

function valuesOf(dict) {
    return dict.map((entry) => entry.value);
}

/**
 * Independently computes the startup corrected EWMA from the documented formula:
 *   alpha = 0.5^(weight / halfLife)
 *   estimate = (1 - alpha) * value + alpha * estimate   (starting at 0)
 *   corrected = estimate / (1 - 0.5^(totalWeight / halfLife))
 */
function ewmaEstimate(values, weights, halfLife) {
    let estimate = 0;
    let totalWeight = 0;
    values.forEach((value, index) => {
        const alpha = Math.pow(0.5, weights[index] / halfLife);
        estimate = (1 - alpha) * value + alpha * estimate;
        totalWeight += weights[index];
    });
    return estimate / (1 - Math.pow(0.5, totalWeight / halfLife));
}

// ---------------------------------------------------------------------------
// testServiceLocationHistory
// ---------------------------------------------------------------------------
test('testServiceLocationHistory', () => {
    const h = createController({isolate: true, maxMeasurementsToKeep: 3, vod: 4});

    // --- A/B/A -------------------------------------------------------------
    h.add(mediaSegment({serviceLocation: 'cdn-a', kbit: 6400}));
    assert.equal(h.controller.getCurrentServiceLocation(Constants.VIDEO), 'cdn-a');
    assert.equal(h.controller.getAverageThroughput(Constants.VIDEO), 6400);
    assert.deepEqual(valuesOf(h.controller.getRawThroughputData(Constants.VIDEO)), [6400]);

    h.add(mediaSegment({serviceLocation: 'cdn-b', kbit: 1600}));
    // The slow location must not be dragged up by the fast location's history.
    assert.equal(h.controller.getCurrentServiceLocation(Constants.VIDEO), 'cdn-b');
    assert.equal(h.controller.getAverageThroughput(Constants.VIDEO), 1600);
    assert.deepEqual(valuesOf(h.controller.getRawThroughputData(Constants.VIDEO)), [1600]);

    // Switching back restores A's samples; nothing is cleared or copied.
    h.add(mediaSegment({serviceLocation: 'cdn-a', kbit: 3200}));
    assert.equal(h.controller.getCurrentServiceLocation(Constants.VIDEO), 'cdn-a');
    assert.equal(h.controller.getAverageThroughput(Constants.VIDEO), 4800);
    assert.deepEqual(valuesOf(h.controller.getRawThroughputData(Constants.VIDEO)), [6400, 3200]);

    // An explicit calculation mode and sample size also stays within the current location.
    assert.equal(
        h.controller.getAverageThroughput(Constants.VIDEO, Constants.THROUGHPUT_CALCULATION_MODES.ARITHMETIC_MEAN, 1),
        3200
    );
    assert.equal(
        h.controller.getAverageThroughput(Constants.VIDEO, Constants.THROUGHPUT_CALCULATION_MODES.ARITHMETIC_MEAN, 4),
        4800
    );
    // getSafeAverageThroughput applies the safety factor exactly once: 4800 * 0.9.
    assert.equal(h.controller.getSafeAverageThroughput(Constants.VIDEO), 4320);
    assert.equal(
        h.controller.getSafeAverageThroughput(Constants.VIDEO, Constants.THROUGHPUT_CALCULATION_MODES.ARITHMETIC_MEAN, 1),
        Math.round(3200 * 0.9)
    );

    // --- Several samples, per location window ------------------------------
    h.add(mediaSegment({serviceLocation: 'cdn-b', kbit: 4800}));
    assert.deepEqual(valuesOf(h.controller.getRawThroughputData(Constants.VIDEO)), [1600, 4800]);

    h.add(mediaSegment({serviceLocation: 'cdn-a', kbit: 1000}));
    assert.deepEqual(valuesOf(h.controller.getRawThroughputData(Constants.VIDEO)), [6400, 3200, 1000]);

    h.add(mediaSegment({serviceLocation: 'cdn-b', kbit: 2000}));
    assert.deepEqual(valuesOf(h.controller.getRawThroughputData(Constants.VIDEO)), [1600, 4800, 2000]);

    // cdn-a already holds maxMeasurementsToKeep (3) samples; adding one only shifts A's own window.
    h.add(mediaSegment({serviceLocation: 'cdn-a', kbit: 5000}));
    assert.deepEqual(valuesOf(h.controller.getRawThroughputData(Constants.VIDEO)), [3200, 1000, 5000]);

    h.add(mediaSegment({serviceLocation: 'cdn-b', kbit: 3000}));
    assert.deepEqual(valuesOf(h.controller.getRawThroughputData(Constants.VIDEO)), [4800, 2000, 3000]);

    h.add(mediaSegment({serviceLocation: 'cdn-a', kbit: 7000}));
    assert.deepEqual(valuesOf(h.controller.getRawThroughputData(Constants.VIDEO)), [1000, 5000, 7000]);
    assert.equal(h.controller.getAverageThroughput(Constants.VIDEO), Math.round(13000 / 3));

    // The two locations kept independent windows, so none of A's samples was pushed out by B.
    h.add(mediaSegment({serviceLocation: 'cdn-b', kbit: 3000}));
    assert.deepEqual(valuesOf(h.controller.getRawThroughputData(Constants.VIDEO)), [2000, 3000, 3000]);
    assert.equal(h.controller.getCurrentServiceLocation(Constants.VIDEO), 'cdn-b');

    // --- Unnamed scope and a location named "__proto__" --------------------
    // Missing, null and empty string all describe the same unnamed scope.
    h.add(mediaSegment({serviceLocation: undefined, kbit: 1000}));
    assert.equal(h.controller.getCurrentServiceLocation(Constants.VIDEO), '');
    h.add(mediaSegment({serviceLocation: null, kbit: 2000}));
    assert.equal(h.controller.getCurrentServiceLocation(Constants.VIDEO), '');
    assert.deepEqual(valuesOf(h.controller.getRawThroughputData(Constants.VIDEO)), [1000, 2000]);
    h.add(mediaSegment({serviceLocation: '', kbit: 3000}));
    assert.deepEqual(valuesOf(h.controller.getRawThroughputData(Constants.VIDEO)), [1000, 2000, 3000]);
    assert.equal(h.controller.getAverageThroughput(Constants.VIDEO), 2000);

    // A location literally called "__proto__" is an ordinary separate scope.
    h.add(mediaSegment({serviceLocation: '__proto__', kbit: 4000}));
    assert.equal(h.controller.getCurrentServiceLocation(Constants.VIDEO), '__proto__');
    assert.deepEqual(valuesOf(h.controller.getRawThroughputData(Constants.VIDEO)), [4000]);
    h.add(mediaSegment({serviceLocation: '__proto__', kbit: 6000}));
    assert.deepEqual(valuesOf(h.controller.getRawThroughputData(Constants.VIDEO)), [4000, 6000]);

    // Going back to the unnamed scope restores its samples and never exposes prototype members.
    h.add(mediaSegment({serviceLocation: '', kbit: 500}));
    assert.equal(h.controller.getCurrentServiceLocation(Constants.VIDEO), '');
    assert.deepEqual(valuesOf(h.controller.getRawThroughputData(Constants.VIDEO)), [2000, 3000, 500]);
    assert.equal(h.controller.getAverageThroughput(Constants.VIDEO), Math.round(5500 / 3));
    assert.deepEqual(valuesOf(h.controller.getRawThroughputData(Constants.VIDEO)).filter((v) => v === 4000), []);

    // --- Cache, MPD and invalid requests must not switch the scope ---------
    h.add(cachedSegment('cdn-a'));
    assert.equal(h.controller.getCurrentServiceLocation(Constants.VIDEO), '');
    assert.deepEqual(valuesOf(h.controller.getRawThroughputData(Constants.VIDEO)), [2000, 3000, 500]);

    h.add(invalidSegment('cdn-a'));
    assert.equal(h.controller.getCurrentServiceLocation(Constants.VIDEO), '');
    assert.deepEqual(valuesOf(h.controller.getRawThroughputData(Constants.VIDEO)), [2000, 3000, 500]);

    // MPD is a different media type ("stream") and must not enter any media location.
    h.add(mpdRequest('cdn-mpd'), Constants.STREAM);
    assert.equal(h.controller.getCurrentServiceLocation(Constants.VIDEO), '');
    assert.deepEqual(valuesOf(h.controller.getRawThroughputData(Constants.VIDEO)), [2000, 3000, 500]);
    // In isolated mode the MPD measurement is not part of the global mixed history either.
    assert.deepEqual(h.controller.getRawThroughputData(Constants.STREAM), []);

    // A valid media segment on a new location does switch the scope.
    h.add(mediaSegment({serviceLocation: 'cdn-c', kbit: 200}));
    assert.equal(h.controller.getCurrentServiceLocation(Constants.VIDEO), 'cdn-c');
    assert.deepEqual(valuesOf(h.controller.getRawThroughputData(Constants.VIDEO)), [200]);

    // --- Audio and video are interleaved but independent -------------------
    h.addAudio(mediaSegment({serviceLocation: 'audio-a', kbit: 128}));
    assert.equal(h.controller.getCurrentServiceLocation(Constants.AUDIO), 'audio-a');
    // Video stayed on cdn-c.
    assert.equal(h.controller.getCurrentServiceLocation(Constants.VIDEO), 'cdn-c');

    h.addAudio(mediaSegment({serviceLocation: 'audio-b', kbit: 64}));
    assert.equal(h.controller.getCurrentServiceLocation(Constants.AUDIO), 'audio-b');
    assert.deepEqual(valuesOf(h.controller.getRawThroughputData(Constants.AUDIO)), [64]);

    // Switching video back to cdn-a restores its full independent history; audio does not move.
    h.add(mediaSegment({serviceLocation: 'cdn-a', kbit: 3200}));
    assert.equal(h.controller.getCurrentServiceLocation(Constants.VIDEO), 'cdn-a');
    assert.equal(h.controller.getCurrentServiceLocation(Constants.AUDIO), 'audio-b');
    // cdn-a window of 3: the oldest sample (1000) is shifted out, 7000 and the new 3200 remain.
    assert.deepEqual(valuesOf(h.controller.getRawThroughputData(Constants.VIDEO)), [5000, 7000, 3200]);
    assert.equal(h.controller.getAverageThroughput(Constants.VIDEO), Math.round(15200 / 3));
    assert.deepEqual(valuesOf(h.controller.getRawThroughputData(Constants.AUDIO)), [64]);

    // Switching audio back to audio-a restores its sample; video does not move.
    h.addAudio(mediaSegment({serviceLocation: 'audio-a', kbit: 256}));
    assert.equal(h.controller.getCurrentServiceLocation(Constants.AUDIO), 'audio-a');
    assert.equal(h.controller.getCurrentServiceLocation(Constants.VIDEO), 'cdn-a');
    assert.deepEqual(valuesOf(h.controller.getRawThroughputData(Constants.AUDIO)), [128, 256]);

    // --- Reset and re-initialize -------------------------------------------
    h.controller.reset();
    assert.equal(h.controller.getCurrentServiceLocation(Constants.VIDEO), null);
    assert.equal(h.controller.getCurrentServiceLocation(Constants.AUDIO), null);
    assert.deepEqual(h.controller.getRawThroughputData(Constants.VIDEO), []);
    assert.deepEqual(h.controller.getRawThroughputData(Constants.AUDIO), []);

    h.controller.initialize();
    h.add(mediaSegment({serviceLocation: 'cdn-a', kbit: 1000}));
    // Events must be handled exactly once after re-initialize.
    assert.deepEqual(valuesOf(h.controller.getRawThroughputData(Constants.VIDEO)), [1000]);
    h.add(mediaSegment({serviceLocation: 'cdn-a', kbit: 3000}));
    assert.deepEqual(valuesOf(h.controller.getRawThroughputData(Constants.VIDEO)), [1000, 3000]);

    h.reset();

    // --- Disabled option keeps the historic global mixed behaviour ---------
    const global = createController({isolate: false, maxMeasurementsToKeep: 3});
    global.add(mediaSegment({serviceLocation: 'cdn-a', kbit: 6400}));
    global.add(mediaSegment({serviceLocation: 'cdn-b', kbit: 1600}));

    // Default behaviour: both service locations share one history.
    assert.equal(global.controller.getAverageThroughput(Constants.VIDEO), 4000);
    assert.deepEqual(valuesOf(global.controller.getRawThroughputData(Constants.VIDEO)), [6400, 1600]);
    // The new scope API stays neutral when the option is disabled.
    assert.equal(global.controller.getCurrentServiceLocation(Constants.VIDEO), null);

    // MPD measurements keep their historic global handling.
    global.add(mpdRequest('cdn-mpd'), Constants.STREAM);
    assert.deepEqual(valuesOf(global.controller.getRawThroughputData(Constants.STREAM)), [9000]);

    // Explicit mode and sample size still refer to the global history.
    assert.equal(
        global.controller.getAverageThroughput(Constants.VIDEO, Constants.THROUGHPUT_CALCULATION_MODES.ARITHMETIC_MEAN, 1),
        1600
    );
    assert.equal(global.controller.getSafeAverageThroughput(Constants.VIDEO), Math.round(4000 * 0.9));

    global.reset();
});

// ---------------------------------------------------------------------------
// testServiceLocationEwma
// ---------------------------------------------------------------------------
test('testServiceLocationEwma', () => {
    // maxMeasurementsToKeep=2 to also verify the per location sample window.
    const h = createController({isolate: true, maxMeasurementsToKeep: 2});

    const throughputWeights = (downloadTime) => 0.0015 * downloadTime;
    const throughputHalfLives = {fast: 3, slow: 8};
    const latencyHalfLives = {fast: 1, slow: 2};

    // --- Non equal sample durations on cdn-a -------------------------------
    // Weights: 0.0015 * 1000 = 1.5 and 0.0015 * 3000 = 4.5.
    h.add(mediaSegment({serviceLocation: 'cdn-a', kbit: 1000, downloadTimeInMs: 1000, latency: 10}));
    h.add(mediaSegment({serviceLocation: 'cdn-a', kbit: 3000, downloadTimeInMs: 3000, latency: 20}));

    const throughputEwmaA = h.controller.getAverageThroughput(
        Constants.VIDEO, Constants.THROUGHPUT_CALCULATION_MODES.EWMA
    );
    const estimatedThroughputA = Math.min(
        ewmaEstimate([1000, 3000], [throughputWeights(1000), throughputWeights(3000)], throughputHalfLives.fast),
        ewmaEstimate([1000, 3000], [throughputWeights(1000), throughputWeights(3000)], throughputHalfLives.slow)
    );
    assert.ok(Math.abs(throughputEwmaA - estimatedThroughputA) <= 0.5, `throughput EWMA ${throughputEwmaA} vs ${estimatedThroughputA}`);

    const latencyEwmaA = h.controller.getAverageLatency(
        Constants.VIDEO, Constants.THROUGHPUT_CALCULATION_MODES.EWMA
    );
    const estimatedLatencyA = Math.max(
        ewmaEstimate([10, 20], [1, 1], latencyHalfLives.fast),
        ewmaEstimate([10, 20], [1, 1], latencyHalfLives.slow)
    );
    assert.ok(Math.abs(latencyEwmaA - estimatedLatencyA) <= 0.5, `latency EWMA ${latencyEwmaA} vs ${estimatedLatencyA}`);
    // Arithmetic latency of the location, checked independently as well.
    assert.equal(h.controller.getAverageLatency(Constants.VIDEO), 15);

    // The arithmetic window of the location contains both samples.
    assert.deepEqual(valuesOf(h.controller.getRawThroughputData(Constants.VIDEO)), [1000, 3000]);
    assert.equal(h.controller.getAverageThroughput(Constants.VIDEO), 2000);
    // sampleSize > available is clamped to the location's own samples.
    assert.equal(
        h.controller.getAverageThroughput(Constants.VIDEO, Constants.THROUGHPUT_CALCULATION_MODES.ARITHMETIC_MEAN, 4),
        2000
    );

    // --- Slow location starts from its own EWMA state ----------------------
    h.add(mediaSegment({serviceLocation: 'cdn-b', kbit: 200, downloadTimeInMs: 1000, latency: 400}));
    const throughputEwmaB = h.controller.getAverageThroughput(
        Constants.VIDEO, Constants.THROUGHPUT_CALCULATION_MODES.EWMA
    );
    // A single sample is corrected back to its exact value, independent of the other location.
    assert.equal(throughputEwmaB, 200);
    assert.ok(Math.abs(throughputEwmaB - Math.min(
        ewmaEstimate([200], [throughputWeights(1000)], throughputHalfLives.fast),
        ewmaEstimate([200], [throughputWeights(1000)], throughputHalfLives.slow)
    )) <= 0.5);
    assert.equal(h.controller.getAverageLatency(Constants.VIDEO, Constants.THROUGHPUT_CALCULATION_MODES.EWMA), 400);
    assert.equal(h.controller.getAverageLatency(Constants.VIDEO), 400);

    // cdn-b has a single sample; its arithmetic window is independent of cdn-a.
    assert.deepEqual(valuesOf(h.controller.getRawThroughputData(Constants.VIDEO)), [200]);

    // --- Back to cdn-a: original EWMA continues, window is per location ----
    h.add(mediaSegment({serviceLocation: 'cdn-a', kbit: 1000, downloadTimeInMs: 1000, latency: 10}));

    // The raw window of cdn-a is limited to 2 samples, so the first sample (1000, 1s) has been shifted.
    assert.deepEqual(valuesOf(h.controller.getRawThroughputData(Constants.VIDEO)), [3000, 1000]);
    // ...but the EWMA estimate continues from cdn-a's own previous state, not from cdn-b's.
    const throughputEwmaA2 = h.controller.getAverageThroughput(
        Constants.VIDEO, Constants.THROUGHPUT_CALCULATION_MODES.EWMA
    );
    const estimatedThroughputA2 = Math.min(
        ewmaEstimate([1000, 3000, 1000],
            [throughputWeights(1000), throughputWeights(3000), throughputWeights(1000)],
            throughputHalfLives.fast),
        ewmaEstimate([1000, 3000, 1000],
            [throughputWeights(1000), throughputWeights(3000), throughputWeights(1000)],
            throughputHalfLives.slow)
    );
    assert.ok(Math.abs(throughputEwmaA2 - estimatedThroughputA2) <= 0.5, `throughput EWMA ${throughputEwmaA2} vs ${estimatedThroughputA2}`);

    const latencyEwmaA2 = h.controller.getAverageLatency(
        Constants.VIDEO, Constants.THROUGHPUT_CALCULATION_MODES.EWMA
    );
    const estimatedLatencyA2 = Math.max(
        ewmaEstimate([10, 20, 10], [1, 1, 1], latencyHalfLives.fast),
        ewmaEstimate([10, 20, 10], [1, 1, 1], latencyHalfLives.slow)
    );
    assert.ok(Math.abs(latencyEwmaA2 - estimatedLatencyA2) <= 0.5);
    // The arithmetic latency window is limited to 2 samples: [20, 10] -> 15.
    assert.equal(h.controller.getAverageLatency(Constants.VIDEO), 15);

    // Switching to cdn-b again restores its own EWMA history (still just one sample).
    h.add(mediaSegment({serviceLocation: 'cdn-b', kbit: 200, downloadTimeInMs: 1000, latency: 400}));
    assert.equal(h.controller.getCurrentServiceLocation(Constants.VIDEO), 'cdn-b');
    assert.deepEqual(valuesOf(h.controller.getRawThroughputData(Constants.VIDEO)), [200, 200]);
    const throughputEwmaB2 = h.controller.getAverageThroughput(
        Constants.VIDEO, Constants.THROUGHPUT_CALCULATION_MODES.EWMA
    );
    assert.ok(Math.abs(throughputEwmaB2 - Math.min(
        ewmaEstimate([200, 200], [throughputWeights(1000), throughputWeights(1000)], throughputHalfLives.fast),
        ewmaEstimate([200, 200], [throughputWeights(1000), throughputWeights(1000)], throughputHalfLives.slow)
    )) <= 0.5);

    h.reset();

    // The option is read when initialize() is called: a fresh instance with the option disabled
    // keeps the global mixed EWMA behaviour.
    const global = createController({isolate: false, maxMeasurementsToKeep: 2});
    global.add(mediaSegment({serviceLocation: 'cdn-a', kbit: 1000, downloadTimeInMs: 1000}));
    global.add(mediaSegment({serviceLocation: 'cdn-b', kbit: 3000, downloadTimeInMs: 3000}));
    const mixed = global.controller.getAverageThroughput(Constants.VIDEO, Constants.THROUGHPUT_CALCULATION_MODES.EWMA);
    const expectedMixed = Math.min(
        ewmaEstimate([1000, 3000], [throughputWeights(1000), throughputWeights(3000)], throughputHalfLives.fast),
        ewmaEstimate([1000, 3000], [throughputWeights(1000), throughputWeights(3000)], throughputHalfLives.slow)
    );
    assert.ok(Math.abs(mixed - expectedMixed) <= 0.5);
    assert.equal(global.controller.getCurrentServiceLocation(Constants.VIDEO), null);
    global.reset();
});

// ---------------------------------------------------------------------------
// testServiceLocationAbr
// ---------------------------------------------------------------------------
test('testServiceLocationAbr', () => {
    // Fixed representation ladder (bitrateInKbit) used by the rule context below.
    const LADDER = [
        {bitrateInKbit: 400, bandwidth: 400 * 1000, absoluteIndex: 0, id: 'r0'},
        {bitrateInKbit: 800, bandwidth: 800 * 1000, absoluteIndex: 1, id: 'r1'},
        {bitrateInKbit: 1600, bandwidth: 1600 * 1000, absoluteIndex: 2, id: 'r2'},
        {bitrateInKbit: 3200, bandwidth: 3200 * 1000, absoluteIndex: 3, id: 'r3'},
        {bitrateInKbit: 6400, bandwidth: 6400 * 1000, absoluteIndex: 4, id: 'r4'}
    ];

    const dashMetrics = {
        getCurrentBufferState: () => ({state: MetricsConstants.BUFFER_LOADED})
    };

    const abrController = {
        getAbandonmentStateFor: () => MetricsConstants.ALLOW_LOAD,
        // Same semantics as AbrController.getOptimalRepresentationForBitrate for this ladder.
        getOptimalRepresentationForBitrate: (mediaInfo, bitrateInKbit) => {
            const candidates = LADDER.filter((rep) => rep.bitrateInKbit <= bitrateInKbit);
            if (candidates.length === 0) {
                return LADDER[0];
            }
            return candidates.reduce((max, curr) => curr.absoluteIndex > max.absoluteIndex ? curr : max);
        }
    };

    function buildRulesContext(controller) {
        return {
            getMediaInfo: () => ({id: 'video-media-info'}),
            getMediaType: () => Constants.VIDEO,
            getStreamInfo: () => ({id: 'stream-0', manifestInfo: {isDynamic: false}}),
            getScheduleController: () => ({setTimeToLoadDelay() {}}),
            getAbrController: () => abrController,
            getThroughputController: () => controller
        };
    }

    function selectRepresentation(rulesContext) {
        const context = {};
        const rule = ThroughputRule(context).create({dashMetrics});
        const switchRequest = rule.getSwitchRequest(rulesContext);
        return {
            rule,
            switchRequest,
            representation: switchRequest.representation ? switchRequest.representation.id : null
        };
    }

    const h = createController({isolate: true, maxMeasurementsToKeep: 4, safetyFactor: 0.9});
    const rulesContext = buildRulesContext(h.controller);

    // --- Isolated selection follows the active location --------------------
    // cdn-a 6400 -> safe 5760 -> highest rep <= 5760 is r3 (3200).
    h.add(mediaSegment({serviceLocation: 'cdn-a', kbit: 6400}));
    assert.equal(h.controller.getSafeAverageThroughput(Constants.VIDEO), 5760);
    assert.equal(selectRepresentation(rulesContext).representation, 'r3');

    // cdn-b 1600 -> safe 1440 -> r1 (800); the fast location's history is not used.
    h.add(mediaSegment({serviceLocation: 'cdn-b', kbit: 1600}));
    assert.equal(h.controller.getSafeAverageThroughput(Constants.VIDEO), 1440);
    const selectionB = selectRepresentation(rulesContext);
    assert.equal(selectionB.representation, 'r1');
    assert.equal(selectionB.switchRequest.rule, 'ThroughputRule');
    assert.equal(selectionB.switchRequest.reason.throughput, 1440);
    assert.equal(selectionB.switchRequest.reason.latency, 50);

    // Back on cdn-a with two samples: (6400 + 3200) / 2 = 4800 -> safe 4320 -> r3.
    h.add(mediaSegment({serviceLocation: 'cdn-a', kbit: 3200}));
    assert.equal(h.controller.getAverageThroughput(Constants.VIDEO), 4800);
    assert.equal(h.controller.getSafeAverageThroughput(Constants.VIDEO), 4320);
    assert.equal(selectRepresentation(rulesContext).representation, 'r3');

    // A further slow sample on cdn-b keeps the selection limited by the slow location.
    h.add(mediaSegment({serviceLocation: 'cdn-b', kbit: 1600}));
    // cdn-b now has [1600, 1600], average 1600 -> safe 1440 -> r1.
    assert.equal(h.controller.getAverageThroughput(Constants.VIDEO), 1600);
    assert.equal(selectRepresentation(rulesContext).representation, 'r1');

    // --- Disabled option selects a different representation ----------------
    // With the option disabled the same sample sequence is mixed:
    // cdn-a 6400 and cdn-b 1600 average to 4000 -> safe 3600 -> r3 instead of r1.
    const global = createController({isolate: false, maxMeasurementsToKeep: 4, safetyFactor: 0.9});
    global.add(mediaSegment({serviceLocation: 'cdn-a', kbit: 6400}));
    global.add(mediaSegment({serviceLocation: 'cdn-b', kbit: 1600}));
    assert.equal(global.controller.getAverageThroughput(Constants.VIDEO), 4000);
    assert.equal(global.controller.getSafeAverageThroughput(Constants.VIDEO), 3600);
    const globalRulesContext = buildRulesContext(global.controller);
    assert.equal(selectRepresentation(globalRulesContext).representation, 'r3');

    // Sanity: the isolated controller in the identical B state picks r1, proving the rule really
    // consumed the scoped controller rather than a separate decision path.
    assert.equal(selectRepresentation(rulesContext).representation, 'r1');

    h.reset();
    global.reset();
});
