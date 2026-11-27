/**
 * The copyright in this software is being made available under the BSD License,
 * included below. This software may be subject to other third party and contributor
 * rights, including patent rights, and no such rights are granted under this license.
 *
 * Copyright (c) 2017, Dash Industry Forum.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *  * Redistributions of source code must retain the above copyright notice, this
 *  list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *  this list of conditions and the following disclaimer in the documentation and/or
 *  other materials provided with the distribution.
 *  * Neither the name of Dash Industry Forum nor the names of its
 *  contributors may be used to endorse or promote products derived from this software
 *  without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS AS IS AND ANY
 *  EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 *  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 *  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 *  INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 *  NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 *  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 *  WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

import Constants from '../constants/Constants.js';
import FactoryMaker from '../../core/FactoryMaker.js';
import Debug from '../../core/Debug.js';
import {HTTPRequest} from '../vo/metrics/HTTPRequest.js';
import MediaPlayerEvents from '../MediaPlayerEvents.js';
import EventBus from '../../core/EventBus.js';

/**
 * Throughput generally stored in kbit/s
 * Latency generally stored in ms
 * @param {object} config
 * @ignore
 * @constructor
 */
function ThroughputModel(config) {

    config = config || {};
    const context = this.context;
    const debug = Debug(context).getInstance();
    const settings = config.settings;
    const eventBus = EventBus(context).getInstance();

    let throughputDict,
        latencyDict,
        ewmaThroughputDict,
        ewmaLatencyDict,
        scopedState,
        ewmaHalfLife,
        logger;

    function setup() {
        logger = debug.getLogger(instance);
        ewmaHalfLife = {
            bandwidthHalfLife: {
                fast: settings.get().streaming.abr.throughput.ewma.throughputFastHalfLifeSeconds,
                slow: settings.get().streaming.abr.throughput.ewma.throughputSlowHalfLifeSeconds
            },
            latencyHalfLife: {
                fast: settings.get().streaming.abr.throughput.ewma.latencyFastHalfLifeCount,
                slow: settings.get().streaming.abr.throughput.ewma.latencySlowHalfLifeCount
            }
        };
        reset();
    }

    /**
     * Use the provided request to add new entries for throughput and latency. Update the EWMA state as well.
     * @param {MediaType} mediaType
     * @param {object} httpRequest
     */
    function addEntry(mediaType, httpRequest) {
        try {

            if (!mediaType || !httpRequest || !httpRequest.trace || !httpRequest.trace.length) {
                return;
            }

            const isIsolated = _isIsolatedByServiceLocation();

            if (!isIsolated) {
                _createSettingsForMediaType(mediaType);
            }

            // In isolated mode only media downloads belong to a service location. The manifest request is
            // intentionally not stored anywhere, it must neither switch the current scope nor pollute it.
            if (isIsolated && httpRequest.type === HTTPRequest.MPD_TYPE) {
                return;
            }

            const latencyInMs = (httpRequest.tresponse.getTime() - httpRequest.trequest.getTime()) || 1;
            let throughputValues = _calculateThroughputValues(httpRequest, latencyInMs);
            throughputValues.latencyInMs = latencyInMs;

            if (isNaN(throughputValues.value) || !isFinite(throughputValues.value)) {
                return;
            }

            // Get estimated throughput (etp, in kbits/s) from CMSD response headers
            if (httpRequest.cmsd) {
                const etp = httpRequest.cmsd.dynamic && httpRequest.cmsd.dynamic.etp ? httpRequest.cmsd.dynamic.etp : null;
                if (etp) {
                    // Apply weight ratio on etp
                    const etpWeightRatio = settings.get().streaming.cmsd.abr.etpWeightRatio;
                    if (etpWeightRatio > 0 && etpWeightRatio <= 1) {
                        throughputValues.value = (throughputValues.value * (1 - etpWeightRatio)) + (etp * etpWeightRatio);
                    }
                }
            }

            const cacheReferenceTime = (httpRequest._tfinish.getTime() - httpRequest.trequest.getTime());

            if (_isCachedResponse(mediaType, cacheReferenceTime, httpRequest)) {
                logger.debug(`${mediaType} Assuming segment ${httpRequest.url} came from cache, ignoring it for throughput calculation`);
                return;
            }

            logger.debug(`Added throughput entry for ${mediaType}: ${throughputValues.value} kbit/s`)
            throughputValues.serviceLocation = httpRequest._serviceLocation;

            let dicts;
            if (isIsolated) {
                // The service location of the last accepted media segment defines the current scope of this media type.
                const serviceLocation = _normalizeServiceLocation(httpRequest._serviceLocation);
                dicts = _getOrCreateScope(mediaType, serviceLocation);
                _setCurrentServiceLocation(mediaType, serviceLocation);
            } else {
                dicts = {
                    throughputDict: throughputDict[mediaType],
                    latencyDict: latencyDict[mediaType],
                    ewmaThroughputDict: ewmaThroughputDict[mediaType],
                    ewmaLatencyDict: ewmaLatencyDict[mediaType]
                };
            }

            dicts.throughputDict.push(throughputValues);
            dicts.latencyDict.push({ value: latencyInMs });
            _cleanupDict(dicts);

            eventBus.trigger(MediaPlayerEvents.THROUGHPUT_MEASUREMENT_STORED, { throughputValues })

            if (isIsolated || httpRequest.type !== HTTPRequest.MPD_TYPE) {
                _updateEwmaValues(dicts.ewmaThroughputDict, throughputValues.value, settings.get().streaming.abr.throughput.ewma.weightDownloadTimeMultiplicationFactor * throughputValues.downloadTimeInMs, ewmaHalfLife.bandwidthHalfLife);
                _updateEwmaValues(dicts.ewmaLatencyDict, latencyInMs, 1, ewmaHalfLife.latencyHalfLife);
            }
        } catch (e) {
            logger.error(e);
        }
    }

    /**
     * Returns true if throughput and latency measurements shall be scoped by service location.
     * @return {boolean}
     * @private
     */
    function _isIsolatedByServiceLocation() {
        return settings.get().streaming.abr.throughput.isolateByServiceLocation === true;
    }

    /**
     * Service locations are distinguished by their original string. Missing values, null and the empty
     * string all describe the same unnamed scope.
     * @param {*} serviceLocation
     * @return {string}
     * @private
     */
    function _normalizeServiceLocation(serviceLocation) {
        return (serviceLocation === undefined || serviceLocation === null || serviceLocation === '') ? '' : serviceLocation;
    }

    /**
     * Returns the media type state used for isolated measurements. A Map is used so that a service
     * location named e.g. "__proto__" is treated like any other regular scope.
     * @param {MediaType} mediaType
     * @return {object}
     * @private
     */
    function _getMediaTypeScopeState(mediaType) {
        let mediaTypeState = scopedState.get(mediaType);
        if (!mediaTypeState) {
            mediaTypeState = {
                scopes: new Map(),
                currentServiceLocation: null
            };
            scopedState.set(mediaType, mediaTypeState);
        }
        return mediaTypeState;
    }

    /**
     * @param {MediaType} mediaType
     * @param {string} serviceLocation
     * @return {object}
     * @private
     */
    function _getOrCreateScope(mediaType, serviceLocation) {
        const mediaTypeState = _getMediaTypeScopeState(mediaType);
        let scope = mediaTypeState.scopes.get(serviceLocation);
        if (!scope) {
            scope = {
                throughputDict: [],
                latencyDict: [],
                ewmaThroughputDict: {
                    fastEstimate: 0,
                    slowEstimate: 0,
                    totalWeight: 0
                },
                ewmaLatencyDict: {
                    fastEstimate: 0,
                    slowEstimate: 0,
                    totalWeight: 0
                }
            };
            mediaTypeState.scopes.set(serviceLocation, scope);
        }
        return scope;
    }

    /**
     * @param {MediaType} mediaType
     * @param {string} serviceLocation
     * @private
     */
    function _setCurrentServiceLocation(mediaType, serviceLocation) {
        _getMediaTypeScopeState(mediaType).currentServiceLocation = serviceLocation;
    }

    /**
     * Returns the scope of the last accepted measurement for the media type or null if there is none.
     * @param {MediaType} mediaType
     * @param {string} dictName
     * @return {*}
     * @private
     */
    function _getCurrentScopeDict(mediaType, dictName) {
        if (!mediaType) {
            return undefined;
        }
        const mediaTypeState = scopedState.get(mediaType);
        if (!mediaTypeState || mediaTypeState.currentServiceLocation === null) {
            return undefined;
        }
        const scope = mediaTypeState.scopes.get(mediaTypeState.currentServiceLocation);
        return scope ? scope[dictName] : undefined;
    }

    /**
     * Returns the current service location of a media type. Returns null if no media measurement has
     * been accepted for that media type yet, and the empty string for the unnamed scope.
     * @param {MediaType} mediaType
     * @return {string|null}
     */
    function getCurrentServiceLocation(mediaType) {
        if (!mediaType) {
            return null;
        }
        const mediaTypeState = scopedState.get(mediaType);
        if (!mediaTypeState || mediaTypeState.currentServiceLocation === null) {
            return null;
        }
        return mediaTypeState.currentServiceLocation;
    }

    /**
     * Returns the throughput in kbit/s , the download time in ms and the downloaded bytes for an HTTP request
     * @param {object} httpRequest
     * @param {number} latencyInMs
     * @return {object}
     * @private
     */
    function _calculateThroughputValues(httpRequest, latencyInMs) {

        // Low latency is enabled, we used the fetch API and received chunks
        if (httpRequest._fileLoaderType && httpRequest._fileLoaderType === Constants.FILE_LOADER_TYPES.FETCH) {
            return _calculateThroughputValuesForFetch(httpRequest);
        }

        // Standard case, we used standard XHR requests
        else {
            return _calculateThroughputValuesForXhr(httpRequest, latencyInMs);
        }
    }

    /**
     * Calculates the throughput for requests using the Fetch API
     * @param {object} httpRequest
     * @param {number} latencyInMs
     * @return {number}
     * @private
     */
    function _calculateThroughputValuesForFetch(httpRequest) {
        const downloadedBytes = httpRequest.trace.reduce((prev, curr) => prev + curr.b[0], 0);
        const downloadTimeInMs = httpRequest.trace.reduce((prev, curr) => prev + curr.d, 0);
        let throughputInKbit = NaN;

        if (settings.get().streaming.abr.throughput.useNetworkInformationApi.fetch) {
            throughputInKbit = _deriveThroughputFromNetworkApi()
        }

        if (isNaN(throughputInKbit)) {
            throughputInKbit = Math.round((8 * downloadedBytes) / downloadTimeInMs); // bits/ms = kbits/s
        }

        return {
            downloadedBytes,
            value: throughputInKbit,
            downloadTimeInMs
        };
    }

    /**
     * Returns the throughput in kbit/s and the download time in ms for requests using XHR
     * @param {object} httpRequest
     * @param {number} latencyInMs
     * @return {object}
     * @private
     */
    function _calculateThroughputValuesForXhr(httpRequest, latencyInMs) {
        let downloadedBytes = NaN;
        let downloadTimeInMs = NaN;
        let deriveThroughputViaResourceTimingApi = false;

        // Calculate the throughput using the ResourceTimingAPI if available
        if (settings.get().streaming.abr.throughput.useResourceTimingApi && httpRequest._resourceTimingValues) {
            downloadedBytes = httpRequest._resourceTimingValues.transferSize;
            downloadTimeInMs = httpRequest._resourceTimingValues.responseEnd - httpRequest._resourceTimingValues.responseStart;
            deriveThroughputViaResourceTimingApi = true;
        }

        // Use the standard throughput calculation if we can not use the Resource Timing API
        else {
            // We need at least two entries in the traces. The first entry includes the latency and the XHR progress event was thrown once bytes have already been received.
            // The second progress event can be set in relation to the first progress event and therefor gives us more accurate values
            if (httpRequest.trace.length <= 1) {
                return { throughput: NaN, downloadTimeInMs: NaN }
            }
            downloadedBytes = httpRequest.trace.reduce((prev, curr) => prev + curr.b[0], 0) - httpRequest.trace[0].b[0];
            downloadTimeInMs = Math.max(httpRequest.trace.reduce((prev, curr) => prev + curr.d, 0) - httpRequest.trace[0].d, 1);
        }

        // If available and enabled use the Network Information API
        let throughputInKbit = NaN;
        if (!deriveThroughputViaResourceTimingApi && settings.get().streaming.abr.throughput.useNetworkInformationApi.xhr) {
            throughputInKbit = _deriveThroughputFromNetworkApi()
        }

        if (isNaN(throughputInKbit)) {
            const referenceTimeInMs = settings.get().streaming.abr.throughput.useDeadTimeLatency ? downloadTimeInMs : downloadTimeInMs + latencyInMs;
            throughputInKbit = Math.round((8 * downloadedBytes) / referenceTimeInMs) // bits/ms = kbits/s
        }

        return {
            downloadedBytes,
            value: throughputInKbit,
            downloadTimeInMs
        };
    }

    /**
     * Return the current estimated bandwidth based on NetworkInformation.downlink if the API is available
     * @returns {*|number}
     * @private
     */
    function _deriveThroughputFromNetworkApi() {
        // NetworkInformation.downlink: Returns the effective bandwidth estimate in megabits per second, rounded to the nearest multiple of 25 kilobits per seconds.
        if (navigator && navigator.connection && !isNaN(navigator.connection.downlink) && navigator.connection.downlink > 0) {
            return navigator.connection.downlink * 1000
        }

        return NaN
    }

    /**
     * Check if the response was cached.
     * @param {MediaType} mediaType
     * @param {number} cacheReferenceTime
     * @param {object} httpRequest
     * @return {boolean}
     * @private
     */
    function _isCachedResponse(mediaType, cacheReferenceTime, httpRequest) {

        if (settings.get().streaming.abr.throughput.useResourceTimingApi && httpRequest._resourceTimingValues) {
            return httpRequest._resourceTimingValues.transferSize === 0 && httpRequest._resourceTimingValues.decodedBodySize > 0
        }

        if (isNaN(cacheReferenceTime)) {
            return false;
        }
        if (mediaType === Constants.VIDEO) {
            return cacheReferenceTime < settings.get().streaming.cacheLoadThresholds[Constants.VIDEO];
        } else if (mediaType === Constants.AUDIO) {
            return cacheReferenceTime < settings.get().streaming.cacheLoadThresholds[Constants.AUDIO];
        }
    }

    /**
     *
     * @param {object} ewmaObj
     * @param {number} value
     * @param {number} weight
     * @param {object} halfLife
     * @private
     */
    function _updateEwmaValues(ewmaObj, value, weight, halfLife) {
        // Note about startup:
        // Estimates start at 0, so early values are underestimated.
        // This effect is countered in getAverageEwma() by dividing the estimates by:
        //     1 - Math.pow(0.5, ewmaObj.totalWeight / halfLife)

        const fastAlpha = Math.pow(0.5, weight / halfLife.fast);
        ewmaObj.fastEstimate = (1 - fastAlpha) * value + fastAlpha * ewmaObj.fastEstimate;

        const slowAlpha = Math.pow(0.5, weight / halfLife.slow);
        ewmaObj.slowEstimate = (1 - slowAlpha) * value + slowAlpha * ewmaObj.slowEstimate;

        ewmaObj.totalWeight += weight;
    }

    /**
     * Shift old entries once we reached the threshold. The window is applied to the dict that is being
     * written to, which in isolated mode belongs to a single service location.
     * @param {object} dicts
     * @private
     */
    function _cleanupDict(dicts) {
        const maxMeasurementsToKeep = settings.get().streaming.abr.throughput.sampleSettings.maxMeasurementsToKeep;
        if (dicts.throughputDict.length > maxMeasurementsToKeep) {
            dicts.throughputDict.shift();
        }

        if (dicts.latencyDict.length > maxMeasurementsToKeep) {
            dicts.latencyDict.shift();
        }
    }

    /**
     * Setup the dict objects for a specific media type
     * @param mediaType
     * @private
     */
    function _createSettingsForMediaType(mediaType) {
        throughputDict[mediaType] = throughputDict[mediaType] || [];
        latencyDict[mediaType] = latencyDict[mediaType] || [];
        ewmaThroughputDict[mediaType] = ewmaThroughputDict[mediaType] || {
            fastEstimate: 0,
            slowEstimate: 0,
            totalWeight: 0
        };
        ewmaLatencyDict[mediaType] = ewmaLatencyDict[mediaType] || { fastEstimate: 0, slowEstimate: 0, totalWeight: 0 };
    }

    function getThroughputDict(mediaType) {
        if (!mediaType) {
            return throughputDict
        }
        if (_isIsolatedByServiceLocation()) {
            const dict = _getCurrentScopeDict(mediaType, 'throughputDict');
            return dict || [];
        }
        return throughputDict[mediaType];
    }

    function getEwmaThroughputDict(mediaType) {
        if (!mediaType) {
            return ewmaThroughputDict
        }
        if (_isIsolatedByServiceLocation()) {
            return _getCurrentScopeDict(mediaType, 'ewmaThroughputDict');
        }
        return ewmaThroughputDict[mediaType]
    }

    function getLatencyDict(mediaType) {
        if (!mediaType) {
            return latencyDict
        }
        if (_isIsolatedByServiceLocation()) {
            const dict = _getCurrentScopeDict(mediaType, 'latencyDict');
            return dict || [];
        }
        return latencyDict[mediaType];
    }

    function getEwmaLatencyDict(mediaType) {
        if (!mediaType) {
            return ewmaLatencyDict
        }
        if (_isIsolatedByServiceLocation()) {
            return _getCurrentScopeDict(mediaType, 'ewmaLatencyDict');
        }
        return ewmaLatencyDict[mediaType];
    }

    function getEwmaHalfLife() {
        return ewmaHalfLife;
    }

    /**
     * Reset all values
     */
    function reset() {
        throughputDict = {};
        latencyDict = {};
        ewmaThroughputDict = {};
        ewmaLatencyDict = {};
        // Per media type service location state. Resetting removes the stored history and the current scope.
        scopedState = new Map();
    }

    const instance = {
        addEntry,
        getCurrentServiceLocation,
        getThroughputDict,
        getEwmaThroughputDict,
        getEwmaLatencyDict,
        getEwmaHalfLife,
        getLatencyDict,
        reset
    };

    setup();

    return instance;
}

ThroughputModel.__dashjs_factory_name = 'ThroughputModel';
export default FactoryMaker.getClassFactory(ThroughputModel);
