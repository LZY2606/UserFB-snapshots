import assert from 'node:assert/strict';
import test from 'node:test';

import Settings from '../../src/core/Settings.js';
import EventBus from '../../src/core/EventBus.js';
import ThroughputController from '../../src/streaming/controllers/ThroughputController.js';
import ThroughputRule from '../../src/streaming/rules/abr/ThroughputRule.js';
import MediaPlayerEvents from '../../src/streaming/MediaPlayerEvents.js';
import MetricsConstants from '../../src/streaming/constants/MetricsConstants.js';
import Constants from '../../src/streaming/constants/Constants.js';

const CALCULATION_MODES = Constants.THROUGHPUT_CALCULATION_MODES;

/**
 * Creates a controller wired to a fresh context. The isolation setting is applied before
 * initialize() as required by the feature.
 */
function createHarness({isolateByServiceLocation = false, maxMeasurementsToKeep = 4, averageCalculationMode = CALCULATION_MODES.ARITHMETIC_MEAN} = {}) {
    const context = {};
    const settings = Settings(context).getInstance();
    settings.reset();
    settings.update({
        streaming: {
            abr: {
                throughput: {
                    averageCalculationMode,
                    isolateByServiceLocation,
                    useResourceTimingApi: false,
                    useDeadTimeLatency: true,
                    useNetworkInformationApi: {fetch: false, xhr: false},
                    sampleSettings: {
                        enableSampleSizeAdjustment: false,
                        maxMeasurementsToKeep
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

    function add(metric) {
        eventBus.trigger(MediaPlayerEvents.METRIC_ADDED, metric);
    }

    function addMedia(mediaType, serviceLocation, kbit, {latency = 50, downloadTimeMs = 1000} = {}) {
        add({
            metric: MetricsConstants.HTTP_REQUEST,
            mediaType,
            value: mediaSample(serviceLocation, kbit, {latency, downloadTimeMs})
        });
    }

    return {context, settings, controller, add, addMedia};
}

function mediaSample(serviceLocation, kbit, {latency = 50, downloadTimeMs = 1000} = {}) {
    return {
        type: 'MediaSegment',
        url: `https://media.invalid/${kbit}.m4s`,
        _serviceLocation: serviceLocation,
        trequest: new Date(0),
        tresponse: new Date(latency),
        _tfinish: new Date(latency + downloadTimeMs),
        _fileLoaderType: Constants.FILE_LOADER_TYPES.FETCH,
        trace: [{s: new Date(latency), d: downloadTimeMs, b: [kbit * downloadTimeMs / 8]}]
    };
}

function mpdSample(serviceLocation) {
    return {
        type: 'MPD',
        url: 'https://media.invalid/manifest.mpd',
        _serviceLocation: serviceLocation,
        trequest: new Date(0),
        tresponse: new Date(50),
        _tfinish: new Date(1050),
        _fileLoaderType: Constants.FILE_LOADER_TYPES.FETCH,
        trace: [{s: new Date(50), d: 1000, b: [500000]}]
    };
}

function cachedMediaSample(serviceLocation, kbit) {
    const sample = mediaSample(serviceLocation, kbit);
    // cacheLoadThresholds.video is 10ms by default, so this response is treated as cached
    sample._tfinish = new Date(sample.trequest.getTime() + 1);
    return sample;
}

function invalidDownloadTimeSample(serviceLocation) {
    return {
        type: 'MediaSegment',
        url: 'https://media.invalid/zero-duration.m4s',
        _serviceLocation: serviceLocation,
        trequest: new Date(0),
        tresponse: new Date(50),
        _tfinish: new Date(1050),
        _fileLoaderType: Constants.FILE_LOADER_TYPES.FETCH,
        trace: [{s: new Date(50), d: 0, b: [1000]}]
    };
}

function rawValues(controller, mediaType) {
    return controller.getRawThroughputData(mediaType).map((entry) => entry.value);
}

function rawLocations(controller, mediaType) {
    return controller.getRawThroughputData(mediaType).map((entry) => entry.serviceLocation);
}

/**
 * Independent re-implementation of the EWMA formulas used by ThroughputModel and
 * ThroughputController, so the expected values are not derived from the production code paths.
 */
function calculateEwmaIndependently(samples, {fastHalfLife, slowHalfLife, useMin}) {
    let fastEstimate = 0;
    let slowEstimate = 0;
    let totalWeight = 0;

    for (const {value, weight} of samples) {
        const fastAlpha = Math.pow(0.5, weight / fastHalfLife);
        const slowAlpha = Math.pow(0.5, weight / slowHalfLife);
        fastEstimate = (1 - fastAlpha) * value + fastAlpha * fastEstimate;
        slowEstimate = (1 - slowAlpha) * value + slowAlpha * slowEstimate;
        totalWeight += weight;
    }

    const correctedFast = fastEstimate / (1 - Math.pow(0.5, totalWeight / fastHalfLife));
    const correctedSlow = slowEstimate / (1 - Math.pow(0.5, totalWeight / slowHalfLife));

    return useMin ? Math.min(correctedFast, correctedSlow) : Math.max(correctedFast, correctedSlow);
}

function createRuleContext({controller, mediaType = Constants.VIDEO, ladder}) {
    const mediaInfo = {type: mediaType, streamInfo: {id: 'stream-0', manifestInfo: {isDynamic: false}}};
    const scheduleController = {setTimeToLoadDelay() {}};

    const abrController = {
        getAbandonmentStateFor() {
            return MetricsConstants.ALLOW_LOAD;
        },
        // Uses the fixed bitrate ladder, same contract as the real AbrController
        getOptimalRepresentationForBitrate(_mediaInfo, bitrateInKbit) {
            return ladder.filter((rep) => rep.bitrateInKbit <= bitrateInKbit).pop() || ladder[0];
        }
    };

    const dashMetrics = {
        getCurrentBufferState() {
            return {state: MetricsConstants.BUFFER_LOADED};
        }
    };

    return {
        getMediaInfo: () => mediaInfo,
        getMediaType: () => mediaType,
        getStreamInfo: () => mediaInfo.streamInfo,
        getScheduleController: () => scheduleController,
        getAbrController: () => abrController,
        getThroughputController: () => controller,
        _dashMetrics: dashMetrics
    };
}

function selectRepresentation(rule, ruleContext) {
    const switchRequest = rule.getSwitchRequest(ruleContext);
    return switchRequest;
}

// ---------------------------------------------------------------------------------------------
// Test 1: history scoping
// ---------------------------------------------------------------------------------------------

test('testServiceLocationHistory', () => {
    const {controller, add, addMedia} = createHarness({isolateByServiceLocation: true, maxMeasurementsToKeep: 4});

    // Before any media sample there is no current location
    assert.equal(controller.getCurrentServiceLocation('video'), null);
    assert.equal(controller.getCurrentServiceLocation('audio'), null);

    // A -> B -> A
    addMedia('video', 'cdn-a', 6400);
    assert.equal(controller.getCurrentServiceLocation('video'), 'cdn-a');
    assert.deepEqual(rawValues(controller, 'video'), [6400]);
    assert.equal(controller.getAverageThroughput('video', CALCULATION_MODES.ARITHMETIC_MEAN), 6400);

    addMedia('video', 'cdn-b', 1600);
    assert.equal(controller.getCurrentServiceLocation('video'), 'cdn-b');
    assert.deepEqual(rawValues(controller, 'video'), [1600]);
    assert.deepEqual(rawLocations(controller, 'video'), ['cdn-b']);
    assert.equal(controller.getAverageThroughput('video', CALCULATION_MODES.ARITHMETIC_MEAN), 1600);

    addMedia('video', 'cdn-a', 3200);
    assert.equal(controller.getCurrentServiceLocation('video'), 'cdn-a');
    assert.deepEqual(rawValues(controller, 'video'), [6400, 3200]);
    assert.equal(controller.getAverageThroughput('video', CALCULATION_MODES.ARITHMETIC_MEAN), 4800);

    // B has two samples before A fills its own window
    addMedia('video', 'cdn-b', 2600);
    assert.deepEqual(rawValues(controller, 'video'), [1600, 2600]);

    // Multiple windows worth of samples on A must not evict B's history
    addMedia('video', 'cdn-a', 1000);
    addMedia('video', 'cdn-a', 2000);
    addMedia('video', 'cdn-a', 3000);
    addMedia('video', 'cdn-a', 4000);
    assert.deepEqual(rawValues(controller, 'video'), [1000, 2000, 3000, 4000]);
    assert.equal(controller.getAverageThroughput('video', CALCULATION_MODES.ARITHMETIC_MEAN), 2500);

    addMedia('video', 'cdn-b', 1600);
    assert.deepEqual(rawValues(controller, 'video'), [1600, 2600, 1600]);
    assert.equal(controller.getAverageThroughput('video', CALCULATION_MODES.ARITHMETIC_MEAN), Math.round((1600 + 2600 + 1600) / 3));

    // Cache hits and MPD measurements neither move the current scope nor enter a media scope
    const beforeCache = rawValues(controller, 'video');
    add({metric: MetricsConstants.HTTP_REQUEST, mediaType: 'video', value: cachedMediaSample('cdn-c', 1234)});
    assert.equal(controller.getCurrentServiceLocation('video'), 'cdn-b');
    assert.deepEqual(rawValues(controller, 'video'), beforeCache);

    add({metric: MetricsConstants.HTTP_REQUEST, mediaType: Constants.STREAM, value: mpdSample('cdn-c')});
    add({metric: MetricsConstants.HTTP_REQUEST, mediaType: 'video', value: mpdSample('cdn-c')});
    assert.equal(controller.getCurrentServiceLocation('video'), 'cdn-b');
    assert.deepEqual(rawValues(controller, 'video'), beforeCache);

    // No valid download measurement: rejected and scope/statistics unchanged
    add({metric: MetricsConstants.HTTP_REQUEST, mediaType: 'video', value: invalidDownloadTimeSample('cdn-a')});
    assert.equal(controller.getCurrentServiceLocation('video'), 'cdn-b');
    assert.deepEqual(rawValues(controller, 'video'), beforeCache);

    // Unnamed locations (undefined, null and empty string) share one scope.
    // The loaded object is what the caller passes to the event bus, so strip the location
    // instead of relying on JSON serialization.
    const unsetLocationSample = mediaSample('cdn-c', 5000);
    delete unsetLocationSample._serviceLocation;
    add({metric: MetricsConstants.HTTP_REQUEST, mediaType: 'video', value: unsetLocationSample});
    assert.equal(controller.getCurrentServiceLocation('video'), '');
    const nullLocationSample = mediaSample('cdn-c', 3000);
    nullLocationSample._serviceLocation = null;
    add({metric: MetricsConstants.HTTP_REQUEST, mediaType: 'video', value: nullLocationSample});
    addMedia('video', '', 1000);
    assert.equal(controller.getCurrentServiceLocation('video'), '');
    assert.deepEqual(rawValues(controller, 'video'), [5000, 3000, 1000]);
    assert.equal(controller.getAverageThroughput('video', CALCULATION_MODES.ARITHMETIC_MEAN), 3000);

    // A location named "__proto__" is an ordinary entry, not the object prototype
    addMedia('video', '__proto__', 7000);
    assert.equal(controller.getCurrentServiceLocation('video'), '__proto__');
    assert.deepEqual(rawValues(controller, 'video'), [7000]);
    assert.equal(Object.prototype.polluted, undefined);
    assert.equal(Object.getPrototypeOf(controller.getRawThroughputData('video')), Array.prototype);

    // Audio and video scopes are independent
    addMedia('audio', 'cdn-a', 6400);
    addMedia('audio', 'cdn-b', 1600);
    assert.equal(controller.getCurrentServiceLocation('audio'), 'cdn-b');
    assert.equal(controller.getCurrentServiceLocation('video'), '__proto__');
    assert.deepEqual(rawValues(controller, 'audio'), [1600]);

    addMedia('video', 'cdn-a', 2400);
    assert.equal(controller.getCurrentServiceLocation('video'), 'cdn-a');
    assert.equal(controller.getCurrentServiceLocation('audio'), 'cdn-b');
    assert.deepEqual(rawValues(controller, 'audio'), [1600]);
    // A's history was never copied from or cleared by another location. B still holds the three
    // samples it collected within its own window.
    assert.deepEqual(rawValues(controller, 'video'), [2000, 3000, 4000, 2400]);
    addMedia('video', 'cdn-b', 1600);
    assert.deepEqual(rawValues(controller, 'video'), [1600, 2600, 1600, 1600]);
    assert.equal(controller.getAverageThroughput('video', CALCULATION_MODES.ARITHMETIC_MEAN), 1850);

    // Disabled mode keeps the historical mixed behaviour and the new API is inert
    const mixed = createHarness({isolateByServiceLocation: false, maxMeasurementsToKeep: 4});
    mixed.addMedia('video', 'cdn-a', 6400);
    mixed.addMedia('video', 'cdn-b', 1600);
    assert.equal(mixed.controller.getAverageThroughput('video', CALCULATION_MODES.ARITHMETIC_MEAN), 4000);
    assert.equal(mixed.controller.getAverageThroughput('video', CALCULATION_MODES.ARITHMETIC_MEAN, 2), 4000);
    assert.deepEqual(rawValues(mixed.controller, 'video'), [6400, 1600]);
    assert.equal(mixed.controller.getCurrentServiceLocation('video'), null);

    // reset clears location history and the current scope; re-initializing handles events once
    controller.reset();
    assert.equal(controller.getCurrentServiceLocation('video'), null);
    assert.deepEqual(rawValues(controller, 'video'), []);

    controller.initialize();
    addMedia('video', 'cdn-a', 6400);
    assert.equal(controller.getCurrentServiceLocation('video'), 'cdn-a');
    assert.deepEqual(rawValues(controller, 'video'), [6400]);
    controller.reset();
});

// ---------------------------------------------------------------------------------------------
// Test 2: EWMA and latency per location
// ---------------------------------------------------------------------------------------------

test('testServiceLocationEwma', () => {
    const harness = createHarness({
        isolateByServiceLocation: true,
        averageCalculationMode: CALCULATION_MODES.EWMA
    });
    const {controller, settings, addMedia} = harness;
    const factor = settings.get().streaming.abr.throughput.ewma.weightDownloadTimeMultiplicationFactor;

    const aSamples = [
        {kbit: 4000, downloadTimeMs: 2000, latency: 100},
        {kbit: 2000, downloadTimeMs: 500, latency: 300}
    ];
    const bSamples = [
        {kbit: 8000, downloadTimeMs: 1000, latency: 20}
    ];

    function addSamples(serviceLocation, samples) {
        samples.forEach(({kbit, downloadTimeMs, latency}) => addMedia('video', serviceLocation, kbit, {downloadTimeMs, latency}));
    }

    // A measurements
    addSamples('cdn-a', aSamples);
    const expectedThroughputA = calculateEwmaIndependently(
        aSamples.map(({kbit, downloadTimeMs}) => ({value: kbit, weight: factor * downloadTimeMs})),
        {fastHalfLife: 3, slowHalfLife: 8, useMin: true}
    );
    const expectedLatencyA = calculateEwmaIndependently(
        aSamples.map(({latency}) => ({value: latency, weight: 1})),
        {fastHalfLife: 1, slowHalfLife: 2, useMin: false}
    );

    assert.equal(controller.getAverageThroughput('video', CALCULATION_MODES.EWMA), Math.round(expectedThroughputA));
    assert.equal(controller.getAverageLatency('video', CALCULATION_MODES.EWMA), Math.round(expectedLatencyA));

    // B measurements only describe B, no A history leaks in
    addSamples('cdn-b', bSamples);
    const expectedThroughputB = calculateEwmaIndependently(
        bSamples.map(({kbit, downloadTimeMs}) => ({value: kbit, weight: factor * downloadTimeMs})),
        {fastHalfLife: 3, slowHalfLife: 8, useMin: true}
    );
    const expectedLatencyB = calculateEwmaIndependently(
        bSamples.map(({latency}) => ({value: latency, weight: 1})),
        {fastHalfLife: 1, slowHalfLife: 2, useMin: false}
    );

    assert.equal(controller.getAverageThroughput('video', CALCULATION_MODES.EWMA), Math.round(expectedThroughputB));
    assert.equal(controller.getAverageLatency('video', CALCULATION_MODES.EWMA), Math.round(expectedLatencyB));
    assert.notEqual(Math.round(expectedThroughputA), Math.round(expectedThroughputB));

    // Mixed input would produce a different estimate than the isolated one
    const mixedExpected = calculateEwmaIndependently(
        [...aSamples, ...bSamples].map(({kbit, downloadTimeMs}) => ({value: kbit, weight: factor * downloadTimeMs})),
        {fastHalfLife: 3, slowHalfLife: 8, useMin: true}
    );
    assert.notEqual(Math.round(mixedExpected), Math.round(expectedThroughputB));

    // Back on A the original samples and EWMA state are restored, not cleared or copied
    addMedia('video', 'cdn-a', 0, {downloadTimeMs: 1000, latency: 50});
    const restoredSamples = aSamples.map(({kbit, downloadTimeMs}) => ({value: kbit, weight: factor * downloadTimeMs}));
    const expectedThroughputAWithZero = calculateEwmaIndependently(
        [...restoredSamples, {value: 0, weight: factor * 1000}],
        {fastHalfLife: 3, slowHalfLife: 8, useMin: true}
    );
    assert.equal(controller.getAverageThroughput('video', CALCULATION_MODES.EWMA), Math.round(expectedThroughputAWithZero));

    // Explicit calculation mode and sample size still use the isolated scope only
    assert.equal(
        controller.getAverageThroughput('video', CALCULATION_MODES.ARITHMETIC_MEAN, 2),
        Math.round((2000 + 0) / 2)
    );
    assert.equal(
        controller.getSafeAverageThroughput('video', CALCULATION_MODES.ARITHMETIC_MEAN, 2),
        Math.round(Math.round((2000 + 0) / 2) * settings.get().streaming.abr.throughput.bandwidthSafetyFactor)
    );

    // The legacy global dict remains untouched by scoped media downloads
    assert.equal(controller.getRawThroughputData('video').length, 3);
    controller.reset();
});

// ---------------------------------------------------------------------------------------------
// Test 3: the real ThroughputRule uses the scoped controller for ABR decisions
// ---------------------------------------------------------------------------------------------

test('testServiceLocationAbr', () => {
    const ladder = [
        {bitrateInKbit: 500},
        {bitrateInKbit: 1000},
        {bitrateInKbit: 2500},
        {bitrateInKbit: 5000}
    ];

    function createRule(harness) {
        const ruleContext = createRuleContext({controller: harness.controller, ladder});
        return {
            rule: ThroughputRule(harness.context).create({dashMetrics: ruleContext._dashMetrics}),
            context: ruleContext
        };
    }

    const isolated = createHarness({isolateByServiceLocation: true, maxMeasurementsToKeep: 4});
    const isolatedRuleBundle = createRule(isolated);

    isolated.addMedia('video', 'cdn-a', 6400);
    const fastLocationRequest = selectRepresentation(isolatedRuleBundle.rule, isolatedRuleBundle.context);
    assert.equal(fastLocationRequest.representation.bitrateInKbit, 5000);
    assert.equal(fastLocationRequest.reason.serviceLocation, 'cdn-a');
    assert.equal(fastLocationRequest.reason.throughput, Math.round(6400 * isolated.settings.get().streaming.abr.throughput.bandwidthSafetyFactor));

    isolated.addMedia('video', 'cdn-b', 1600);
    const slowLocationRequest = selectRepresentation(isolatedRuleBundle.rule, isolatedRuleBundle.context);
    assert.equal(slowLocationRequest.representation.bitrateInKbit, 1000);
    assert.equal(slowLocationRequest.reason.serviceLocation, 'cdn-b');
    assert.notEqual(slowLocationRequest.representation.bitrateInKbit, fastLocationRequest.representation.bitrateInKbit);

    isolated.addMedia('video', 'cdn-a', 3200);
    const backOnFastLocationRequest = selectRepresentation(isolatedRuleBundle.rule, isolatedRuleBundle.context);
    assert.equal(backOnFastLocationRequest.representation.bitrateInKbit, 2500);
    assert.equal(backOnFastLocationRequest.reason.serviceLocation, 'cdn-a');
    assert.equal(backOnFastLocationRequest.reason.throughput, Math.round(4800 * isolated.settings.get().streaming.abr.throughput.bandwidthSafetyFactor));

    // Without isolation the same sequence yields the historical mixed estimate and a different selection
    const mixed = createHarness({isolateByServiceLocation: false, maxMeasurementsToKeep: 4});
    const mixedRuleBundle = createRule(mixed);
    mixed.addMedia('video', 'cdn-a', 6400);
    mixed.addMedia('video', 'cdn-b', 1600);
    const mixedRequest = selectRepresentation(mixedRuleBundle.rule, mixedRuleBundle.context);
    assert.equal(mixedRequest.reason.throughput, Math.round(4000 * mixed.settings.get().streaming.abr.throughput.bandwidthSafetyFactor));
    assert.equal(mixedRequest.representation.bitrateInKbit, 2500);
    assert.notEqual(mixedRequest.representation.bitrateInKbit, slowLocationRequest.representation.bitrateInKbit);

    isolated.controller.reset();
    mixed.controller.reset();
});
