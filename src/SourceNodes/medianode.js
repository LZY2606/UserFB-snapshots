//Matthew Shotton, R&D User Experience,© BBC 2015
import SourceNode, { SOURCENODESTATE } from "./sourcenode";

class MediaNode extends SourceNode {
    /**
     * Initialise an instance of a MediaNode.
     * This should not be called directly, but extended by other Node Types which use a `HTMLMediaElement`.
     */
    constructor(
        src,
        gl,
        renderGraph,
        currentTime,
        globalPlaybackRate = 1.0,
        sourceOffset = 0,
        preloadTime = 4,
        mediaElementCache = undefined,
        attributes = {}
    ) {
        super(src, gl, renderGraph, currentTime);
        this._preloadTime = preloadTime;
        this._sourceOffset = sourceOffset;
        this._globalPlaybackRate = globalPlaybackRate;
        this._mediaElementCache = mediaElementCache;
        this._playbackRate = 1.0;
        this._playbackRateUpdated = true;
        // A sorted list of playback-rate change points ("knots") on the context
        // timeline. Each entry is {time, mediaPosition, rate} and means: from
        // timeline time `time` onwards the source was at `mediaPosition` and
        // advances at `rate` units of media per unit of timeline time. The
        // first entry is created when the node is sequenced, using the rate
        // current at that moment.
        this._playbackRateTimeline = [];
        // Whether the stop time was set explicitly via stop()/stopAt(). An
        // explicit stop time always wins over the one inferred from the media
        // duration and the recorded playback rates.
        this._stopTimeExplicit = false;
        this._attributes = Object.assign({ volume: 1.0 }, attributes);
        this._loopElement = false;
        this._isElementPlaying = false;
        if (this._attributes.loop) {
            this._loopElement = this._attributes.loop;
        }
    }

    /**
     * Set the local playback rate of this node: how much media time is consumed
     * per unit of timeline time. The rate applied to the underlying element is
     * the product of this rate and the VideoContext's playbackRate.
     *
     * The change is recorded at the node's current timeline position. Media
     * positions accumulated under previous rates are preserved, and seeking
     * back into an earlier region of the timeline still uses the rates that
     * were recorded there. Setting a new rate after seeking backwards replaces
     * any changes recorded at or after the current position, while earlier
     * history is kept. Setting a rate before the node is sequenced establishes
     * the initial rate for the whole segment. Only finite numbers greater than
     * 0 are accepted; anything else throws a RangeError and leaves the previous
     * rate and recorded history untouched.
     *
     * @param {number} playbackRate - the local playback rate, 1.0 by default.
     *
     * @example
     * //A 10 second clip starting at timeline time 10, with sourceOffset 1.
     * var videoNode = ctx.video("video.mp4", 1);
     * videoNode.startAt(10);
     * videoNode.playbackRate = 2;
     * ctx.currentTime = 12; //the element sits at media time 5
     * videoNode.playbackRate = 0.5; //the frame doesn't jump
     * ctx.currentTime = 14; //media time 6
     * ctx.currentTime = 11; //seeking back lands on media time 3
     * videoNode.playbackRate = 1; //replaces the change recorded at 12
     * ctx.currentTime = 14; //media time 6 again
     */
    set playbackRate(playbackRate) {
        if (
            typeof playbackRate !== "number" ||
            isNaN(playbackRate) ||
            !isFinite(playbackRate) ||
            playbackRate <= 0
        ) {
            throw new RangeError("playbackRate must be a finite number greater than 0");
        }
        if (!isNaN(this._startTime) && !this._usesLegacyPlaybackRatePath()) {
            //Record the rate change at the node's current timeline position.
            this._ensurePlaybackRateTimeline();
            let changeTime = Math.max(this._currentTime, this._startTime);
            let mediaPosition = this._mediaTimeAt(changeTime);
            //Discard changes recorded at or after this position; earlier
            //history is preserved. Repeated sets at the same time replace each
            //other, so the last one wins.
            while (
                this._playbackRateTimeline.length > 0 &&
                this._playbackRateTimeline[this._playbackRateTimeline.length - 1].time >=
                    changeTime
            ) {
                this._playbackRateTimeline.pop();
            }
            this._playbackRateTimeline.push({
                time: changeTime,
                mediaPosition: mediaPosition,
                rate: playbackRate
            });
        }
        this._playbackRate = playbackRate;
        this._playbackRateUpdated = true;
        this._updateAutomaticStopTime();
    }

    set stretchPaused(stretchPaused) {
        super.stretchPaused = stretchPaused;
        if (this._element) {
            if (this._stretchPaused) {
                this._element.pause();
            } else {
                if (this._state === SOURCENODESTATE.playing) {
                    this._element.play();
                }
            }
        }
    }

    get stretchPaused() {
        return this._stretchPaused;
    }

    get playbackRate() {
        return this._playbackRate;
    }

    /**
     * Looping, stretched-paused and infinite-duration media keep the legacy
     * time mapping (media time advances 1:1 with the timeline) and don't get a
     * local playback-rate history.
     */
    _usesLegacyPlaybackRatePath() {
        if (this._loopElement) return true;
        if (this._stretchPaused) return true;
        if (this._element !== undefined) {
            let duration = this._element.duration;
            if (typeof duration === "number" && !isNaN(duration) && !isFinite(duration))
                return true;
        }
        return false;
    }

    _ensurePlaybackRateTimeline() {
        if (this._playbackRateTimeline.length === 0 && !isNaN(this._startTime)) {
            this._playbackRateTimeline.push({
                time: this._startTime,
                mediaPosition: this._sourceOffset,
                rate: this._playbackRate
            });
        }
    }

    /**
     * Map a time on the context timeline to a position within the source
     * media, taking the source offset and all recorded playback-rate changes
     * into account.
     */
    _mediaTimeAt(time) {
        if (isNaN(this._startTime) || time <= this._startTime) return this._sourceOffset;
        if (this._usesLegacyPlaybackRatePath()) {
            return this._sourceOffset + (time - this._startTime);
        }
        this._ensurePlaybackRateTimeline();
        let activeKnot = this._playbackRateTimeline[0];
        for (let knot of this._playbackRateTimeline) {
            if (knot.time > time) break;
            activeKnot = knot;
        }
        return activeKnot.mediaPosition + (time - activeKnot.time) * activeKnot.rate;
    }

    /**
     * The inverse of _mediaTimeAt: the timeline time at which the given source
     * media position is reached, derived from the recorded playback rates.
     */
    _timelineTimeForMediaPosition(mediaPosition) {
        this._ensurePlaybackRateTimeline();
        for (let i = this._playbackRateTimeline.length - 1; i >= 0; i--) {
            let knot = this._playbackRateTimeline[i];
            if (mediaPosition >= knot.mediaPosition) {
                return knot.time + (mediaPosition - knot.mediaPosition) / knot.rate;
            }
        }
        return this._startTime;
    }

    /**
     * Infer the stop time from the remaining media length and the recorded
     * playback rates. Never overrides an explicit stop()/stopAt() time, and
     * not applicable to media on the legacy path.
     */
    _updateAutomaticStopTime() {
        if (this._stopTimeExplicit) return;
        if (isNaN(this._startTime)) return;
        if (this._usesLegacyPlaybackRatePath()) return;
        if (this._element === undefined) return;
        let duration = this._element.duration;
        if (typeof duration !== "number" || isNaN(duration) || !isFinite(duration)) return;
        let stopTime = this._timelineTimeForMediaPosition(duration);
        if (stopTime !== this._stopTime) {
            this._stopTime = stopTime;
            this._triggerCallbacks("durationchange", this.duration);
        }
    }

    get elementURL() {
        return this._elementURL;
    }

    /**
     * @property {Boolean}
     * @summary - Check if the element is waiting on the network to continue playback
     */

    get _buffering() {
        if (this._element) {
            return this._element.readyState < HTMLMediaElement.HAVE_FUTURE_DATA;
        }

        return false;
    }

    set volume(volume) {
        this._attributes.volume = volume;
        if (this._element !== undefined) this._element.volume = this._attributes.volume;
    }

    _triggerLoad() {
        // If the user hasn't supplied an element, videocontext is responsible for the element
        if (this._isResponsibleForElementLifeCycle) {
            if (this._mediaElementCache) {
                /**
                 * Get a cached video element and also pass this instance so the
                 * cache can access the current play state.
                 */
                this._element = this._mediaElementCache.getElementAndLinkToNode(this);
            } else {
                this._element = document.createElement(this._elementType);
                this._element.setAttribute("crossorigin", "anonymous");
                this._element.setAttribute("webkit-playsinline", "");
                this._element.setAttribute("playsinline", "");
                // This seems necessary to allow using video as a texture. See:
                // https://bugs.chromium.org/p/chromium/issues/detail?id=898550
                // https://github.com/pixijs/pixi.js/issues/5996
                this._element.preload = "auto";
                this._playbackRateUpdated = true;
            }
            this._element.volume = this._attributes.volume;
            if (window.MediaStream !== undefined && this._elementURL instanceof MediaStream) {
                this._element.srcObject = this._elementURL;
            } else {
                this._element.src = this._elementURL;
            }
        }
        // at this stage either the user or the element cache should have provided an element
        if (this._element) {
            for (let key in this._attributes) {
                this._element[key] = this._attributes[key];
            }

            //Map the timeline position through the recorded playback rates.
            //Before the segment starts this stays at the source offset.
            this._element.currentTime = this._mediaTimeAt(this._currentTime);
            this._element.onerror = () => {
                if (this._element === undefined) return;
                console.debug("Error with element", this._element);
                this._state = SOURCENODESTATE.error;
                //Event though there's an error ready should be set to true so the node can output transparenn
                this._ready = true;
                this._triggerCallbacks("error");
            };
        } else {
            // If the element doesn't exist for whatever reason enter the error state.
            this._state = SOURCENODESTATE.error;
            this._ready = true;
            this._triggerCallbacks("error");
        }

        this._loadTriggered = true;
    }

    /**
     * _load has two functions:
     *
     * 1. `_triggerLoad` which ensures the element has the correct src and is at the correct currentTime,
     *     so that the browser can start fetching media.
     *
     * 2.  `shouldPollForElementReadyState` waits until the element has a "readState" that signals there
     *     is enough media to start playback. This is a little confusing as currently structured.
     *     We're using the _update loop to poll the _load function which checks the element status.
     *     When ready we fire off the "loaded callback"
     *
     */

    _load() {
        super._load();

        /**
         * We've got to be careful here as _load is called many times whilst waiting for the element to buffer
         * and this function should only be called once.
         * This is step one in what should be a more thorough refactor
         */
        if (!this._loadTriggered) {
            this._triggerLoad();
        }

        const shouldPollForElementReadyState = this._element !== undefined;
        /**
         * this expression is effectively polling the element, waiting for it to buffer
         * it gets called a lot of time
         */
        if (shouldPollForElementReadyState) {
            if (this._element.readyState > 3 && !this._element.seeking) {
                // at this point the element has enough data for current playback position
                // and at least a couple of frames into the future

                // Check if the duration has changed. Update if necessary.
                // this could potentially go in the normal update loop but I don't want to change
                // too many things at once
                // The stop time is inferred from the remaining media length and
                // the recorded playback rates, unless it was set explicitly.
                this._updateAutomaticStopTime();

                // signal to user that this node has "loaded"
                if (this._ready !== true) {
                    this._triggerCallbacks("loaded");
                    this._playbackRateUpdated = true;
                }

                this._ready = true;
            } else {
                if (this._state !== SOURCENODESTATE.error) {
                    this._ready = false;
                }
            }
        }
    }

    _unload() {
        super._unload();
        if (this._isResponsibleForElementLifeCycle && this._element !== undefined) {
            this._element.removeAttribute("src");
            this._element.srcObject = undefined;
            this._element.load();
            for (let key in this._attributes) {
                this._element.removeAttribute(key);
            }
            // Unlink this form the cache, freeing up the element for another media node
            if (this._mediaElementCache)
                this._mediaElementCache.unlinkNodeFromElement(this._element);
            this._element = undefined;
            if (!this._mediaElementCache) delete this._element;
        }
        // reset class to initial state
        this._ready = false;
        this._isElementPlaying = false;
        // For completeness. I couldn't find a path that required reuse of this._loadTriggered after _unload.
        this._loadTriggered = false;
    }

    _seek(time) {
        super._seek(time);
        if (this.state === SOURCENODESTATE.playing || this.state === SOURCENODESTATE.paused) {
            if (this._element === undefined) this._load();
            //Use the same timeline-to-media mapping as _triggerLoad so seeks
            //and (re)loads always land on the same source position.
            this._element.currentTime = this._mediaTimeAt(this._currentTime);
            this._ready = false;
        }
        if (
            (this._state === SOURCENODESTATE.sequenced || this._state === SOURCENODESTATE.ended) &&
            this._element !== undefined
        ) {
            this._unload();
        }
    }

    _update(currentTime, triggerTextureUpdate = true) {
        //if (!super._update(currentTime)) return false;
        super._update(currentTime, triggerTextureUpdate);
        //check if the media has ended
        if (this._element !== undefined) {
            if (this._element.ended) {
                this._state = SOURCENODESTATE.ended;
                this._triggerCallbacks("ended");
            }
        }

        if (
            this._startTime - this._currentTime <= this._preloadTime &&
            this._state !== SOURCENODESTATE.waiting &&
            this._state !== SOURCENODESTATE.ended
        )
            this._load();

        if (this._state === SOURCENODESTATE.playing) {
            if (this._playbackRateUpdated) {
                this._element.playbackRate = this._globalPlaybackRate * this._playbackRate;
                this._playbackRateUpdated = false;
            }
            if (!this._isElementPlaying) {
                this._element.play();
                if (this._stretchPaused) {
                    this._element.pause();
                }
                this._isElementPlaying = true;
            }
            return true;
        } else if (this._state === SOURCENODESTATE.paused) {
            this._element.pause();
            this._isElementPlaying = false;
            return true;
        } else if (this._state === SOURCENODESTATE.ended && this._element !== undefined) {
            this._element.pause();
            if (this._isElementPlaying) {
                this._unload();
            }
            return false;
        }
    }

    stop(time) {
        let result = super.stop(time);
        //An explicit stop time always wins over the inferred one; loading
        //metadata or later playback-rate changes must not override it.
        if (result) this._stopTimeExplicit = true;
        return result;
    }

    stopAt(time) {
        let result = super.stopAt(time);
        if (result) this._stopTimeExplicit = true;
        return result;
    }

    clearTimelineState() {
        super.clearTimelineState();
        //Discard the recorded playback-rate changes; the mapping is rebuilt
        //from the node's current rate when the node is re-sequenced.
        this._playbackRateTimeline = [];
        this._stopTimeExplicit = false;
        if (this._element !== undefined) {
            this._element.pause();
            this._isElementPlaying = false;
        }
        this._unload();
    }

    destroy() {
        if (this._element) this._element.pause();
        super.destroy();
    }
}

export default MediaNode;
