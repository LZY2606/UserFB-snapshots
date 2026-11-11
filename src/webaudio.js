JSMpeg.AudioOutput.WebAudio = (function() { "use strict";

var WebAudioOut = function(options) {
	this.context = WebAudioOut.CachedContext =
		WebAudioOut.CachedContext ||
		new (window.AudioContext || window.webkitAudioContext)();

	this.gain = this.context.createGain();
	this.destination = this.gain;

	// Keep track of the number of connections to this AudioContext, so we
	// can safely close() it when we're the only one connected to it.
	this.gain.connect(this.context.destination);
	this.context._connections = (this.context._connections || 0) + 1;
	
	this.startTime = 0;
	this.buffer = null;
	this.wallclockStartTime = 0;
	this.volume = 1;
	this.enabled = true;

	// Keep track of the source nodes that are still alive. Nodes remove
	// themselves when they finish playing, so this never grows beyond the
	// audio that can actually still be heard. Older entries are dropped
	// wholesale whenever this output is stopped or reset.
	this.activeSources = new Set();
	this.destroyed = false;

	this.unlocked = !WebAudioOut.NeedsUnlocking();

	Object.defineProperty(this, 'enqueuedTime', {get: this.getEnqueuedTime});
};

WebAudioOut.prototype.clearSources = function() {
	var sources = this.activeSources;
	this.activeSources = new Set();

	sources.forEach(function(source) {
		// Detach the ended handler first, so an ended event that arrives
		// late cannot interfere with a newer playback generation.
		source.onended = null;
		try {
			source.stop();
		} catch (e) {}
		try {
			source.disconnect();
		} catch (e) {}
	});
};

WebAudioOut.prototype.setStartTimeToNow = function() {
	this.startTime = this.context.currentTime;
	this.wallclockStartTime = JSMpeg.Now();
};

WebAudioOut.prototype.destroy = function() {
	if (this.destroyed) {
		return;
	}
	this.destroyed = true;
	this.unlockCallback = null;

	// Stop everything this output has scheduled and forget its timing before
	// releasing the shared context.
	this.clearSources();
	this.setStartTimeToNow();

	this.gain.disconnect();
	this.context._connections--;

	if (this.context._connections === 0) {
		this.context.close();
		WebAudioOut.CachedContext = null;
	}
};

WebAudioOut.prototype.play = function(sampleRate, left, right) {
	if (!this.enabled || this.destroyed) {
		return;
	}

	// If the context is not unlocked yet, we simply advance the start time
	// to "fake" actually playing audio. This will keep the video in sync.
	if (!this.unlocked) {
		var ts = JSMpeg.Now()
		if (this.wallclockStartTime < ts) {
			this.wallclockStartTime = ts;
		}
		this.wallclockStartTime += left.length / sampleRate;
		return;
	}


	this.gain.gain.value = this.volume;

	var buffer = this.context.createBuffer(2, left.length, sampleRate);
	buffer.getChannelData(0).set(left);
	buffer.getChannelData(1).set(right);

	var source = this.context.createBufferSource();
	source.buffer = buffer;
	source.connect(this.destination);

	// Release this node as soon as it has played out. A late ended event
	// only removes the node it belongs to; it never touches the timing of
	// a newer playback generation.
	var self = this;
	source.onended = function() {
		self.activeSources.delete(source);
	};
	this.activeSources.add(source);

	var now = this.context.currentTime;
	var duration = buffer.duration;
	if (this.startTime < now) {
		this.startTime = now;
		this.wallclockStartTime = JSMpeg.Now();
	}

	source.start(this.startTime);
	this.startTime += duration;
	this.wallclockStartTime += duration;
};

WebAudioOut.prototype.stop = function() {
	if (this.destroyed) {
		return;
	}

	// Remove all audio this output scheduled. Just muting the gain would
	// leave the nodes alive with their timing, so that they'd become
	// audible again as soon as the gain is restored.
	this.gain.gain.value = 0;
	this.clearSources();
	this.setStartTimeToNow();
};

WebAudioOut.prototype.getEnqueuedTime = function() {
	// The AudioContext.currentTime is only updated every so often, so if we
	// want to get exact timing, we need to rely on the system time.
	return Math.max(this.wallclockStartTime - JSMpeg.Now(), 0)
};

WebAudioOut.prototype.resetEnqueuedTime = function() {
	if (this.destroyed) {
		return;
	}

	// Discard any audio that is still scheduled and start a fresh timeline
	// at the current playable position.
	this.clearSources();
	this.setStartTimeToNow();
};

WebAudioOut.prototype.unlock = function(callback) {
	if (this.destroyed) {
		return;
	}

	if (this.unlocked) {
		if (callback) {
			callback();
		}
		return;
	}

	this.unlockCallback = callback;

	// Create empty buffer and play it
	var buffer = this.context.createBuffer(1, 1, 22050);
	var source = this.context.createBufferSource();
	source.buffer = buffer;
	source.connect(this.destination);

	// The probe tone belongs to this output, too: stop() and destroy() must
	// clean it up like any other scheduled source.
	var self = this;
	source.onended = function() {
		self.activeSources.delete(source);
	};
	this.activeSources.add(source);

	source.start(0);

	setTimeout(this.checkIfUnlocked.bind(this, source, 0), 0);
};

WebAudioOut.prototype.checkIfUnlocked = function(source, attempt) {
	// A poll that arrives after destroy() must not revive the output or
	// invoke the user's callback.
	if (this.destroyed) {
		return;
	}

	if (
		source.playbackState === source.PLAYING_STATE ||
		source.playbackState === source.FINISHED_STATE
	) {
		this.unlocked = true;
		if (this.unlockCallback) {
			this.unlockCallback();
			this.unlockCallback = null;
		}
	}
	else if (attempt < 10) {
		// Jeez, what a shit show. Thanks iOS!
		setTimeout(this.checkIfUnlocked.bind(this, source, attempt+1), 100);
	}
};

WebAudioOut.NeedsUnlocking = function() {
	return /iPhone|iPad|iPod/i.test(navigator.userAgent);
};

WebAudioOut.IsSupported = function() {
	return (window.AudioContext || window.webkitAudioContext);
};

WebAudioOut.CachedContext = null;

return WebAudioOut;

})();

