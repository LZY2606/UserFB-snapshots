/*
 * Copyright (C) 2016 Bilibili. All Rights Reserved.
 *
 * @author zheng qian <xqq@xqq.im>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import EventEmitter from 'events';
import Log from '../utils/logger.js';
import Browser from '../utils/browser';
import MSEEvents from './mse-events';
import {IllegalStateException} from '../utils/exception.js';

// Identifiable reason carried by ERROR events emitted when a codec switch fails
const MSEErrorReason = {
    CODEC_CHANGE_UNSUPPORTED: 'codec_change_unsupported'
};

// Media Source Extensions controller
class MSEController {

    constructor(config) {
        this.TAG = 'MSEController';

        this._config = config;
        this._emitter = new EventEmitter();

        if (this._config.isLive && this._config.autoCleanupSourceBuffer == undefined) {
            // For live stream, do auto cleanup by default
            this._config.autoCleanupSourceBuffer = true;
        }

        this.e = {
            onSourceOpen: this._onSourceOpen.bind(this),
            onSourceEnded: this._onSourceEnded.bind(this),
            onSourceClose: this._onSourceClose.bind(this),
            onStartStreaming: this._onStartStreaming.bind(this),
            onEndStreaming: this._onEndStreaming.bind(this),
            onQualityChange: this._onQualityChange.bind(this),
            onSourceBufferError: this._onSourceBufferError.bind(this),
            onSourceBufferUpdateEnd: this._onSourceBufferUpdateEnd.bind(this)
        };

        // Use ManagedMediaSource only if w3c MediaSource is not available (e.g. iOS Safari)
        this._useManagedMediaSource = (typeof self.ManagedMediaSource === 'function') && !(typeof self.MediaSource === 'function');

        this._mediaSource = null;
        this._mediaSourceObjectURL = null;

        this._mediaElementProxy = null;

        this._isBufferFull = false;
        this._hasPendingEos = false;

        this._requireSetMediaDuration = false;
        this._pendingMediaDuration = 0;

        this._pendingSourceBufferInit = [];
        // MIME type currently effective on the SourceBuffer (null until the buffer is created)
        this._mimeTypes = {
            video: null,
            audio: null
        };
        // Latest MIME type requested by init segments, even if its codec switch has not
        // taken effect yet. A mismatch against _mimeTypes means a switch is still queued.
        this._requestedMimeTypes = {
            video: null,
            audio: null
        };
        // A track is stopped after an unsupported codec transition, until flush() resets it
        this._stoppedTracks = {
            video: false,
            audio: false
        };
        this._sourceBuffers = {
            video: null,
            audio: null
        };
        this._lastInitSegments = {
            video: null,
            audio: null
        };
        // The init segment that actually took effect for the current MIME type
        this._effectiveInitSegments = {
            video: null,
            audio: null
        };
        this._pendingSegments = {
            video: [],
            audio: []
        };
        this._pendingRemoveRanges = {
            video: [],
            audio: []
        };
    }

    destroy() {
        if (this._mediaSource) {
            this.shutdown();
        }
        if (this._mediaSourceObjectURL) {
            this.revokeObjectURL();
        }
        this.e = null;
        this._emitter.removeAllListeners();
        this._emitter = null;
    }

    on(event, listener) {
        this._emitter.addListener(event, listener);
    }

    off(event, listener) {
        this._emitter.removeListener(event, listener);
    }

    initialize(mediaElementProxy) {
        if (this._mediaSource) {
            throw new IllegalStateException('MediaSource has been attached to an HTMLMediaElement!');
        }

        if (this._useManagedMediaSource) {
            Log.v(this.TAG, 'Using ManagedMediaSource');
        }

        const ms = this._mediaSource = this._useManagedMediaSource ? new self.ManagedMediaSource() : new self.MediaSource();
        ms.addEventListener('sourceopen', this.e.onSourceOpen);
        ms.addEventListener('sourceended', this.e.onSourceEnded);
        ms.addEventListener('sourceclose', this.e.onSourceClose);

        if (this._useManagedMediaSource) {
            ms.addEventListener('startstreaming', this.e.onStartStreaming);
            ms.addEventListener('endstreaming', this.e.onEndStreaming);
            ms.addEventListener('qualitychange', this.e.onQualityChange);
        }

        this._mediaElementProxy = mediaElementProxy;
    }

    shutdown() {
        if (this._mediaSource) {
            const ms = this._mediaSource;
            for (const type in this._sourceBuffers) {
                // pending segments should be discard
                const ps = this._pendingSegments[type];
                ps.splice(0, ps.length);
                this._pendingSegments[type] = null;
                this._pendingRemoveRanges[type] = null;
                this._lastInitSegments[type] = null;
                this._effectiveInitSegments[type] = null;

                // remove all sourcebuffers
                const sb = this._sourceBuffers[type];
                if (sb) {
                    if (ms.readyState !== 'closed') {
                        // ms edge can throw an error: Unexpected call to method or property access
                        try {
                            ms.removeSourceBuffer(sb);
                        } catch (error) {
                            Log.e(this.TAG, error.message);
                        }
                        sb.removeEventListener('error', this.e.onSourceBufferError);
                        sb.removeEventListener('updateend', this.e.onSourceBufferUpdateEnd);
                    }
                    this._mimeTypes[type] = null;
                    this._sourceBuffers[type] = null;
                }
            }
            if (ms.readyState === 'open') {
                try {
                    ms.endOfStream();
                } catch (error) {
                    Log.e(this.TAG, error.message);
                }
            }
            this._mediaElementProxy = null;
            ms.removeEventListener('sourceopen', this.e.onSourceOpen);
            ms.removeEventListener('sourceended', this.e.onSourceEnded);
            ms.removeEventListener('sourceclose', this.e.onSourceClose);
            if (this._useManagedMediaSource) {
                ms.removeEventListener('startstreaming', this.e.onStartStreaming);
                ms.removeEventListener('endstreaming', this.e.onEndStreaming);
                ms.removeEventListener('qualitychange', this.e.onQualityChange);
            }
            this._pendingSourceBufferInit = [];
            this._requestedMimeTypes = {video: null, audio: null};
            this._effectiveInitSegments = {video: null, audio: null};
            this._stoppedTracks = {video: false, audio: false};
            this._isBufferFull = false;
            this._mediaSource = null;
        }
    }

    isManagedMediaSource() {
        return this._useManagedMediaSource;
    }

    getObject() {
        if (!this._mediaSource) {
            throw new IllegalStateException('MediaSource has not been initialized yet!');
        }
        return this._mediaSource;
    }

    getHandle() {
        if (!this._mediaSource) {
            throw new IllegalStateException('MediaSource has not been initialized yet!');
        }
        return this._mediaSource.handle;
    }

    getObjectURL() {
        if (!this._mediaSource) {
            throw new IllegalStateException('MediaSource has not been initialized yet!');
        }

        if (this._mediaSourceObjectURL == null) {
            this._mediaSourceObjectURL = URL.createObjectURL(this._mediaSource);
        }
        return this._mediaSourceObjectURL;
    }

    revokeObjectURL() {
        if (this._mediaSourceObjectURL) {
            URL.revokeObjectURL(this._mediaSourceObjectURL);
            this._mediaSourceObjectURL = null;
        }
    }

    _buildMimeType(initSegment) {
        const is = initSegment;
        let mimeType = `${is.container}`;
        if (is.codec && is.codec.length > 0) {
            if (is.codec === 'opus' && Browser.name === 'safari') {
                is.codec = 'Opus';
            }
            mimeType += `;codecs=${is.codec}`;
        }
        return mimeType;
    }

    appendInitSegment(initSegment, deferred = undefined) {
        if (this._stoppedTracks[initSegment.type]) {
            // This track is parked after a failed codec transition.
            // flush() must clear the failure state before new init segments are accepted.
            return;
        }

        const is = initSegment;
        const mimeType = this._buildMimeType(is);

        Log.v(this.TAG, 'Received Initialization Segment, mimeType: ' + mimeType);
        // Keep the latest received init segment for flush() / Safari reappend, but it only
        // represents an effective type once the corresponding append has actually run.
        this._lastInitSegments[is.type] = is;
        // Record the requested type separately: an init segment whose changeType() has not
        // succeeded yet must not become the current effective MIME type.
        this._requestedMimeTypes[is.type] = mimeType;
        // The target type travels with the segment, so the queued switch happens in order.
        // Tagging before deferral keeps the transition intact while sourceopen / startstreaming
        // are still being waited for.
        is._mimeType = mimeType;

        if (!this._mediaSource || this._mediaSource.readyState !== 'open' || this._mediaSource.streaming === false) {
            // sourcebuffer creation requires mediaSource.readyState === 'open'
            // so we defer the sourcebuffer creation, until sourceopen event triggered
            this._pendingSourceBufferInit.push(is);
            // make sure that this InitSegment is in the front of pending segments queue
            this._pendingSegments[is.type].push(is);
            return;
        }

        if (!this._mimeTypes[is.type]) {
            // First chance: create the sourcebuffer with the initial MIME type
            try {
                const sb = this._sourceBuffers[is.type] = this._mediaSource.addSourceBuffer(mimeType);
                sb.addEventListener('error', this.e.onSourceBufferError);
                sb.addEventListener('updateend', this.e.onSourceBufferUpdateEnd);
            } catch (error) {
                Log.e(this.TAG, error.message);
                this._emitter.emit(MSEEvents.ERROR, {code: error.code, msg: error.message});
                this._requestedMimeTypes[is.type] = null;
                this._lastInitSegments[is.type] = null;
                delete is._mimeType;
                return;
            }
            this._mimeTypes[is.type] = mimeType;
        } else if (mimeType !== this._mimeTypes[is.type]) {
            Log.v(this.TAG, `Notice: ${is.type} mimeType changed, origin: ${this._mimeTypes[is.type]}, target: ${mimeType}`);
        }

        if (!deferred) {
            // deferred means this InitSegment has been pushed to pendingSegments queue
            this._pendingSegments[is.type].push(is);
        }

        if (this._sourceBuffers[is.type] && !this._sourceBuffers[is.type].updating) {
            this._doAppendSegments();
        }

        if (Browser.name === 'safari' && is.container === 'audio/mpeg' && is.mediaDuration > 0) {
            // 'audio/mpeg' track under Safari may cause MediaElement's duration to be NaN
            // Manually correct MediaSource.duration to make progress bar seekable, and report right duration
            this._requireSetMediaDuration = true;
            this._pendingMediaDuration = is.mediaDuration / 1000;  // in seconds
            this._updateMediaSourceDuration();
        }
    }

    appendMediaSegment(mediaSegment) {
        const ms = mediaSegment;

        if (this._stoppedTracks[ms.type]) {
            // Drop media belonging to a track parked by a failed codec transition
            return;
        }

        this._pendingSegments[ms.type].push(ms);

        if (this._config.autoCleanupSourceBuffer && this._needCleanupSourceBuffer()) {
            this._doCleanupSourceBuffer();
        }

        const sb = this._sourceBuffers[ms.type];
        if (sb && !sb.updating && !this._hasPendingRemoveRanges()) {
            this._doAppendSegments();
        }
    }

    flush() {
        // remove all appended buffers
        for (const type in this._sourceBuffers) {
            if (!this._sourceBuffers[type]) {
                continue;
            }

            // A flush reopens a track parked by a failed codec transition and clears the
            // failure state, so the init segment can be resubmitted afterwards. Retries
            // still cannot loop: a re-failing transition parks the track again.
            this._stoppedTracks[type] = false;
            this._requestedMimeTypes[type] = this._mimeTypes[type];

            // abort current buffer append algorithm
            const sb = this._sourceBuffers[type];
            if (this._mediaSource.readyState === 'open') {
                try {
                    // If range removal algorithm is running, InvalidStateError will be throwed
                    // Ignore it.
                    sb.abort();
                } catch (error) {
                    Log.e(this.TAG, error.message);
                }
            }

            // pending segments should be discard
            const ps = this._pendingSegments[type];
            ps.splice(0, ps.length);

            if (this._mediaSource.readyState === 'closed') {
                // Parent MediaSource object has been detached from HTMLMediaElement
                continue;
            }

            // Worker detachment can precede the MediaSource readyState change.
            let buffered;
            try {
                buffered = sb.buffered;
            } catch (error) {
                if (error && error.name === 'InvalidStateError') {
                    this._pendingRemoveRanges[type].length = 0;
                    continue;
                }
                throw error;
            }

            // record ranges to be remove from SourceBuffer
            for (let i = 0; i < buffered.length; i++) {
                const start = buffered.start(i);
                const end = buffered.end(i);
                this._pendingRemoveRanges[type].push({start, end});
            }

            // if sb is not updating, let's remove ranges now!
            if (!sb.updating) {
                this._doRemoveRanges();
            }

            // Safari 10 may get InvalidStateError in the later appendBuffer() after SourceBuffer.remove() call
            // Internal parser's state may be invalid at this time. Re-append last InitSegment to workaround.
            // Related issue: https://bugs.webkit.org/show_bug.cgi?id=159230
            if (Browser.name === 'safari') {
                // Re-append the init segment that actually took effect. A newly received,
                // not-yet-switched init segment (its codec never became effective) is not reused.
                const effectiveInitSegment = this._effectiveInitSegments[type] || this._lastInitSegments[type];
                if (effectiveInitSegment
                        && (!this._mimeTypes[type] || this._buildMimeType(effectiveInitSegment) === this._mimeTypes[type])) {
                    this._pendingSegments[type].push(effectiveInitSegment);
                    if (!sb.updating) {
                        this._doAppendSegments();
                    }
                }
            }
        }
    }

    endOfStream() {
        const ms = this._mediaSource;
        const sb = this._sourceBuffers;
        if (!ms || ms.readyState !== 'open') {
            if (ms && ms.readyState === 'closed' && this._hasPendingSegments()) {
                // If MediaSource hasn't turned into open state, and there're pending segments
                // Mark pending endOfStream, defer call until all pending segments appended complete
                this._hasPendingEos = true;
            }
            return;
        }
        if (sb.video && sb.video.updating || sb.audio && sb.audio.updating) {
            // If any sourcebuffer is updating, defer endOfStream operation
            // See _onSourceBufferUpdateEnd()
            this._hasPendingEos = true;
        } else {
            this._hasPendingEos = false;
            // Notify media data loading complete
            // This is helpful for correcting total duration to match last media segment
            // Otherwise MediaElement's ended event may not be triggered
            ms.endOfStream();
        }
    }

    _needCleanupSourceBuffer() {
        if (!this._config.autoCleanupSourceBuffer) {
            return false;
        }

        const currentTime = this._mediaElementProxy.getCurrentTime();

        for (const type in this._sourceBuffers) {
            const sb = this._sourceBuffers[type];
            if (sb) {
                const buffered = sb.buffered;
                if (buffered.length >= 1) {
                    if (currentTime - buffered.start(0) >= this._config.autoCleanupMaxBackwardDuration) {
                        return true;
                    }
                }
            }
        }

        return false;
    }

    _doCleanupSourceBuffer() {
        const currentTime = this._mediaElementProxy.getCurrentTime();

        for (const type in this._sourceBuffers) {
            const sb = this._sourceBuffers[type];
            if (sb) {
                const buffered = sb.buffered;
                let doRemove = false;

                for (let i = 0; i < buffered.length; i++) {
                    const start = buffered.start(i);
                    const end = buffered.end(i);

                    if (start <= currentTime && currentTime < end + 3) {  // padding 3 seconds
                        if (currentTime - start >= this._config.autoCleanupMaxBackwardDuration) {
                            doRemove = true;
                            const removeEnd = currentTime - this._config.autoCleanupMinBackwardDuration;
                            this._pendingRemoveRanges[type].push({start: start, end: removeEnd});
                        }
                    } else if (end < currentTime) {
                        doRemove = true;
                        this._pendingRemoveRanges[type].push({start: start, end: end});
                    }
                }

                if (doRemove && !sb.updating) {
                    this._doRemoveRanges();
                }
            }
        }
    }

    _updateMediaSourceDuration() {
        const sb = this._sourceBuffers;
        if (this._mediaElementProxy.getReadyState() === 0 || this._mediaSource.readyState !== 'open') {
            return;
        }
        if ((sb.video && sb.video.updating) || (sb.audio && sb.audio.updating)) {
            return;
        }

        const current = this._mediaSource.duration;
        const target = this._pendingMediaDuration;

        if (target > 0 && (isNaN(current) || target > current)) {
            Log.v(this.TAG, `Update MediaSource duration from ${current} to ${target}`);
            this._mediaSource.duration = target;
        }

        this._requireSetMediaDuration = false;
        this._pendingMediaDuration = 0;
    }

    _doRemoveRanges() {
        for (const type in this._pendingRemoveRanges) {
            if (!this._sourceBuffers[type] || this._sourceBuffers[type].updating) {
                continue;
            }
            const sb = this._sourceBuffers[type];
            const ranges = this._pendingRemoveRanges[type];
            while (ranges.length && !sb.updating) {
                const range = ranges.shift();
                sb.remove(range.start, range.end);
            }
        }
    }

    _onCodecChangeFailure(type, oldMimeType, newMimeType, error) {
        // Park the track: the new type and its following media stay queued but are not
        // appended, while the other independent track keeps working. The effective MIME
        // type is deliberately left untouched, so the already buffered data stays valid.
        this._stoppedTracks[type] = true;
        Log.e(this.TAG, `Codec change failed for ${type} track, ${oldMimeType} -> ${newMimeType}: ${error.message}`);
        this._emitter.emit(MSEEvents.ERROR, {
            reason: MSEErrorReason.CODEC_CHANGE_UNSUPPORTED,
            code: error.code,
            msg: error.message,
            type,
            oldMimeType,
            newMimeType
        });
    }

    _doAppendSegments() {
        const pendingSegments = this._pendingSegments;

        for (const type in pendingSegments) {
            const sb = this._sourceBuffers[type];
            if (!sb || sb.updating || this._mediaSource.streaming === false || this._stoppedTracks[type]) {
                continue;
            }

            if (pendingSegments[type].length === 0) {
                continue;
            }

            const segment = pendingSegments[type].shift();

            // An init segment whose target MIME is not the effective one yet must switch
            // the SourceBuffer codec before its data is appended.
            if (segment._mimeType !== undefined && segment._mimeType !== this._mimeTypes[type]) {
                const targetMimeType = segment._mimeType;
                const oldMimeType = this._mimeTypes[type];

                if (typeof sb.changeType !== 'function') {
                    pendingSegments[type].unshift(segment);
                    this._onCodecChangeFailure(type, oldMimeType, targetMimeType,
                        new Error('SourceBuffer.changeType() is not supported by this browser'));
                    continue;
                }

                try {
                    // changeType() is synchronous and requires the SourceBuffer not updating
                    sb.changeType(targetMimeType);
                } catch (error) {
                    // Re-queue untouched: later segments (e.g. another codec's init segment)
                    // must not overtake the init segment whose switch failed
                    pendingSegments[type].unshift(segment);
                    this._onCodecChangeFailure(type, oldMimeType, targetMimeType, error);
                    continue;
                }

                this._mimeTypes[type] = targetMimeType;
                // Notify exactly once per completed switch, before the new init segment append
                this._emitter.emit(MSEEvents.CODEC_CHANGE, {
                    type,
                    oldMimeType,
                    newMimeType: targetMimeType
                });
                // fall through and append this init segment under the new type
            }

            // An init segment whose MIME is effective becomes the reference init segment
            // for flush() / Safari reappend. A received-but-not-yet-switched init segment
            // never reaches this point.
            if (segment._mimeType !== undefined && segment._mimeType === this._mimeTypes[type]) {
                this._effectiveInitSegments[type] = segment;
            }

            if (typeof segment.timestampOffset === 'number' && isFinite(segment.timestampOffset)) {
                // For MPEG audio stream in MSE, if unbuffered-seeking occurred
                // We need explicitly set timestampOffset to the desired point in timeline for mpeg SourceBuffer.
                const currentOffset = sb.timestampOffset;
                const targetOffset = segment.timestampOffset / 1000;  // in seconds

                const delta = Math.abs(currentOffset - targetOffset);
                if (delta > 0.1) {  // If time delta > 100ms
                    Log.v(this.TAG, `Update MPEG audio timestampOffset from ${currentOffset} to ${targetOffset}`);
                    sb.timestampOffset = targetOffset;
                }
                delete segment.timestampOffset;
            }

            if (!segment.data || segment.data.byteLength === 0) {
                // Ignore empty buffer
                continue;
            }

            try {
                sb.appendBuffer(segment.data);
                this._isBufferFull = false;
            } catch (error) {
                this._pendingSegments[type].unshift(segment);
                if (error.code === 22) {  // QuotaExceededError
                    /* Notice that FireFox may not throw QuotaExceededError if SourceBuffer is full
                     * Currently we can only do lazy-load to avoid SourceBuffer become scattered.
                     * SourceBuffer eviction policy may be changed in future version of FireFox.
                     *
                     * Related issues:
                     * https://bugzilla.mozilla.org/show_bug.cgi?id=1279885
                     * https://bugzilla.mozilla.org/show_bug.cgi?id=1280023
                     */

                    // report buffer full, abort network IO
                    if (!this._isBufferFull) {
                        this._emitter.emit(MSEEvents.BUFFER_FULL);
                    }
                    this._isBufferFull = true;
                } else {
                    Log.e(this.TAG, error.message);
                    this._emitter.emit(MSEEvents.ERROR, {code: error.code, msg: error.message});
                }
            }
        }
    }

    _onSourceOpen() {
        Log.v(this.TAG, 'MediaSource onSourceOpen');
        this._mediaSource.removeEventListener('sourceopen', this.e.onSourceOpen);
        // deferred sourcebuffer creation / initialization
        if (this._pendingSourceBufferInit.length > 0) {
            const pendings = this._pendingSourceBufferInit;
            while (pendings.length) {
                const segment = pendings.shift();
                this.appendInitSegment(segment, true);
            }
        }
        // there may be some pending media segments, append them
        if (this._hasPendingSegments()) {
            this._doAppendSegments();
        }
        this._emitter.emit(MSEEvents.SOURCE_OPEN);
    }

    _onStartStreaming() {
        Log.v(this.TAG, 'ManagedMediaSource onStartStreaming');
        // streaming flipped back to true: continue the same pending append order,
        // including codec switches waiting at the head of a track queue
        if (this._hasPendingSegments()) {
            this._doAppendSegments();
        }
        this._emitter.emit(MSEEvents.START_STREAMING);
    }

    _onEndStreaming() {
        Log.v(this.TAG, 'ManagedMediaSource onEndStreaming');
        this._emitter.emit(MSEEvents.END_STREAMING);
    }

    _onQualityChange() {
        Log.v(this.TAG, 'ManagedMediaSource onQualityChange');
    }

    _onSourceEnded() {
        // fired on endOfStream
        Log.v(this.TAG, 'MediaSource onSourceEnded');
    }

    _onSourceClose() {
        // fired on detaching from media element
        Log.v(this.TAG, 'MediaSource onSourceClose');
        if (this._mediaSource && this.e != null) {
            this._mediaSource.removeEventListener('sourceopen', this.e.onSourceOpen);
            this._mediaSource.removeEventListener('sourceended', this.e.onSourceEnded);
            this._mediaSource.removeEventListener('sourceclose', this.e.onSourceClose);
            if (this._useManagedMediaSource) {
                this._mediaSource.removeEventListener('startstreaming', this.e.onStartStreaming);
                this._mediaSource.removeEventListener('endstreaming', this.e.onEndStreaming);
                this._mediaSource.removeEventListener('qualitychange', this.e.onQualityChange);
            }
        }
    }

    _hasPendingSegments() {
        const ps = this._pendingSegments;
        return ps.video.length > 0 || ps.audio.length > 0;
    }

    _hasPendingRemoveRanges() {
        const prr = this._pendingRemoveRanges;
        return prr.video.length > 0 || prr.audio.length > 0;
    }

    _onSourceBufferUpdateEnd() {
        if (this._requireSetMediaDuration) {
            this._updateMediaSourceDuration();
        } else if (this._hasPendingRemoveRanges()) {
            this._doRemoveRanges();
        } else if (this._hasPendingSegments()) {
            this._doAppendSegments();
        } else if (this._hasPendingEos) {
            this.endOfStream();
        }
        this._emitter.emit(MSEEvents.UPDATE_END);
    }

    _onSourceBufferError(e) {
        Log.e(this.TAG, `SourceBuffer Error: ${e}`);
        // this error might not always be fatal, just ignore it
    }

}

export default MSEController;
