// Tests for frame-accurate volume automation (fadeVolume / fadeGlobalVolume /
// oscillateVolume / oscillateGlobalVolume) with the null driver.
//
// The task_cases registry below is consumed by task_support/driver.c, which
// runs the envelope, clock and routing groups and prints GROUP/TARGET lines.

#include "task_support/check.h"
#include "soloud.h"

#include <cmath>
#include <vector>

namespace
{
	const double PI = 3.14159265358979323846;
	const unsigned RATE = 48000;

	// A steady, low-amplitude mono source. Low amplitude keeps us out of the
	// clipper so PCM equals source * gain to floating point precision.
	class ConstantInstance : public SoLoud::AudioSourceInstance
	{
	public:
		float mValue;
		explicit ConstantInstance(float aValue) : mValue(aValue) {}
		unsigned int getAudio(float *aBuffer, unsigned int aSamplesToRead, unsigned int) override
		{
			for (unsigned int i = 0; i < aSamplesToRead; ++i)
				aBuffer[i] = mValue;
			return aSamplesToRead;
		}
		bool hasEnded() override { return false; }
	};

	class Constant : public SoLoud::AudioSource
	{
	public:
		Constant(float aValue, unsigned aChannels = 1) : mValue(aValue)
		{
			mBaseSamplerate = (float)RATE;
			mChannels = aChannels;
		}
		SoLoud::AudioSourceInstance *createInstance() override { return new ConstantInstance(mValue); }
		float mValue;
	};

	struct Engine
	{
		SoLoud::Soloud *mSoloud;
		explicit Engine(unsigned aChannels = 1)
		{
			// Soloud is ~139 KB; keep it off the stack.
			mSoloud = new SoLoud::Soloud();
			mSoloud->init(0, SoLoud::Soloud::NULLDRIVER, RATE, 512, aChannels);
			mSoloud->setPostClipScaler(1.0f);
			mSoloud->setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);
		}
		~Engine()
		{
			mSoloud->deinit();
			delete mSoloud;
		}
		SoLoud::Soloud &soloud() { return *mSoloud; }
	};

	// Render `aFrames` output frames in `aBlock`-sized mix calls (the last one
	// may be shorter), appending interleaved PCM to aPcm.
	void renderInto(SoLoud::Soloud &aS, unsigned aFrames, unsigned aBlock, std::vector<float> &aPcm, unsigned aChannels)
	{
		unsigned done = 0;
		while (done < aFrames)
		{
			unsigned n = aBlock;
			if (n == 0 || n > aFrames - done) n = aFrames - done;
			std::vector<float> buf((size_t)n * aChannels);
			aS.mix(buf.data(), n);
			aPcm.insert(aPcm.end(), buf.begin(), buf.end());
			done += n;
		}
	}

	// Frame f, channel c of the interleaved history collected by renderInto.
	float sampleAt(const std::vector<float> &aPcm, unsigned aChannels, unsigned aFrame, unsigned aChannel)
	{
		return aPcm[(size_t)aFrame * aChannels + aChannel];
	}

	// Reference linear fade gain for the k-th frame after the control call
	// (k is one-based): starts at aFrom and reaches aTo after aDuration
	// seconds, then holds.
	double linearGain(double aFrom, double aTo, double aDuration, unsigned aK)
	{
		double t = aK / (double)RATE;
		if (t >= aDuration) return aTo;
		return aFrom + (aTo - aFrom) * (t / aDuration);
	}

	// Reference LFO gain for the k-th frame after the control call.
	double lfoGain(double aLow, double aHigh, double aPeriod, unsigned aK)
	{
		double mid = (aLow + aHigh) * 0.5;
		double amp = (aHigh - aLow) * 0.5;
		double t = aK / (double)RATE;
		return mid + amp * std::sin(2.0 * PI * t / aPeriod);
	}

	// A mono source played directly to the output is scaled by 1/sqrt(2)
	// by SoLoud's mono expansion; use this to turn a gain into an expected
	// PCM level for the constant source.
	const double MONO = 1.0 / 1.41421356237309504880;

	// Warm the engine so the first block's ramp-up is out of the way.
	void warm(SoLoud::Soloud &aS, unsigned aChannels = 1)
	{
		std::vector<float> tmp(1024 * aChannels);
		aS.mix(tmp.data(), 1024);
	}
}

// ---------------------------------------------------------------------------
// envelope: the automatic value depends only on elapsed time, not on the
// block split and not on the requested length.
// ---------------------------------------------------------------------------

static int env_voice_fade_short_block()
{
	// A 64-frame fade that must finish on frame 64 exactly, even when the
	// first mix call is 128 frames long (the old behaviour stretched it).
	Engine e;
	Constant src(0.25f);
	SoLoud::handle h = e.soloud().play(src);
	warm(e.soloud());
	const double duration = 64.0 / RATE;
	e.soloud().fadeVolume(h, 0.0f, duration);

	std::vector<float> pcm;
	renderInto(e.soloud(), 128, 128, pcm, 1);

	const double start = pcm[0] / (0.25 * MONO); // gain at frame 1
	for (unsigned k = 1; k <= 64; ++k)
	{
		double expected = linearGain(1.0, 0.0, duration, k) * 0.25 * MONO;
		TASK_CHECK(std::fabs(pcm[k - 1] - expected) <= 1e-5);
	}
	for (unsigned k = 65; k <= 128; ++k)
		TASK_CHECK(std::fabs(pcm[k - 1]) <= 1e-5);
	TASK_CHECK(start < 1.0 && start > 0.95); // no click, starts near 1
	// getter reflects the last delivered frame (silence)
	TASK_CHECK(std::fabs(e.soloud().getVolume(h) - 0.0f) <= 1e-5);
	return 0;
}

static int env_voice_fade_split_equivalence()
{
	// Whole block versus chopped blocks must produce identical PCM for
	// block sizes 1, 7, 257 and the full request.
	Engine e;
	Constant src(0.25f);
	SoLoud::handle h = e.soloud().play(src);
	warm(e.soloud());
	e.soloud().fadeVolume(h, 0.0f, 96.0 / RATE);

	std::vector<float> whole;
	renderInto(e.soloud(), 256, 256, whole, 1);

	for (unsigned block : {1u, 7u, 257u})
	{
		Engine e2;
		Constant src2(0.25f);
		SoLoud::handle h2 = e2.soloud().play(src2);
		warm(e2.soloud());
		e2.soloud().fadeVolume(h2, 0.0f, 96.0 / RATE);
		std::vector<float> split;
		renderInto(e2.soloud(), 256, block, split, 1);
		for (unsigned i = 0; i < 256; ++i)
			TASK_CHECK(std::fabs(split[i] - whole[i]) <= 1e-5);
	}
	return 0;
}

static int env_global_fade_short_block()
{
	Engine e;
	Constant src(0.25f);
	e.soloud().play(src);
	warm(e.soloud());
	e.soloud().fadeGlobalVolume(0.0f, 64.0 / RATE);

	std::vector<float> pcm;
	renderInto(e.soloud(), 128, 128, pcm, 1);
	for (unsigned k = 1; k <= 64; ++k)
	{
		double expected = linearGain(1.0, 0.0, 64.0 / RATE, k) * 0.25 * MONO;
		TASK_CHECK(std::fabs(pcm[k - 1] - expected) <= 1e-5);
	}
	for (unsigned k = 65; k <= 128; ++k)
		TASK_CHECK(std::fabs(pcm[k - 1]) <= 1e-5);
	TASK_CHECK(std::fabs(e.soloud().getGlobalVolume()) <= 1e-5);
	return 0;
}

static int env_voice_lfo_peaks_cross_blocks()
{
	// Period 480 frames. The peak/trough land on 120/360 and must be reached
	// even though the render is made of many short blocks.
	Engine e;
	Constant src(0.25f);
	SoLoud::handle h = e.soloud().play(src);
	warm(e.soloud());
	const double period = 480.0 / RATE;
	e.soloud().oscillateVolume(h, 0.2f, 1.0f, period);

	std::vector<float> pcm;
	renderInto(e.soloud(), 600, 7, pcm, 1);
	for (unsigned k = 1; k <= 600; ++k)
	{
		double expected = lfoGain(0.2, 1.0, period, k) * 0.25 * MONO;
		TASK_CHECK(std::fabs(pcm[k - 1] - expected) <= 1e-5);
	}
	// Explicit peak/trough checks: frame 120 -> high, frame 360 -> low.
	TASK_CHECK(std::fabs(pcm[119] - 0.25 * MONO) <= 1e-5);
	TASK_CHECK(std::fabs(pcm[359] - 0.05 * MONO) <= 1e-5);
	return 0;
}

static int env_global_lfo_peaks_cross_blocks()
{
	Engine e;
	Constant src(0.25f);
	e.soloud().play(src);
	warm(e.soloud());
	const double period = 480.0 / RATE;
	e.soloud().oscillateGlobalVolume(0.2f, 1.0f, period);
	TASK_CHECK(std::fabs(e.soloud().getGlobalVolume() - 0.6f) <= 1e-5);

	std::vector<float> pcm;
	renderInto(e.soloud(), 600, 1, pcm, 1);
	for (unsigned k = 1; k <= 600; ++k)
	{
		double expected = lfoGain(0.2, 1.0, period, k) * 0.25 * MONO;
		TASK_CHECK(std::fabs(pcm[k - 1] - expected) <= 1e-5);
	}
	TASK_CHECK(std::fabs(pcm[119] - 0.25 * MONO) <= 1e-5);
	TASK_CHECK(std::fabs(pcm[359] - 0.05 * MONO) <= 1e-5);
	return 0;
}

static int env_lfo_bounds_ordered()
{
	// Passing the bounds high-then-low must still produce a low-then-high LFO
	// with the same shape as the ordered form.
	Engine a;
	Constant sa(0.25f);
	SoLoud::handle ha = a.soloud().play(sa);
	warm(a.soloud());
	a.soloud().oscillateVolume(ha, 1.0f, 0.2f, 240.0 / RATE);
	std::vector<float> pcmA;
	renderInto(a.soloud(), 240, 240, pcmA, 1);

	Engine b;
	Constant sb(0.25f);
	SoLoud::handle hb = b.soloud().play(sb);
	warm(b.soloud());
	b.soloud().oscillateVolume(hb, 0.2f, 1.0f, 240.0 / RATE);
	std::vector<float> pcmB;
	renderInto(b.soloud(), 240, 240, pcmB, 1);

	for (unsigned i = 0; i < 240; ++i)
		TASK_CHECK(std::fabs(pcmA[i] - pcmB[i]) <= 1e-5);
	return 0;
}

static int env_immediate_getter_midpoint()
{
	// Before any output the setter reports the value it will start from:
	// the LFO midpoint, or the fade's current value.
	Engine e;
	Constant src(0.25f);
	SoLoud::handle h = e.soloud().play(src);
	warm(e.soloud());

	e.soloud().setVolume(h, 1.0f);
	e.soloud().oscillateVolume(h, 0.25f, 0.75f, 480.0 / RATE);
	TASK_CHECK(std::fabs(e.soloud().getVolume(h) - 0.5f) <= 1e-6);
	e.soloud().oscillateGlobalVolume(0.1f, 0.9f, 480.0 / RATE);
	TASK_CHECK(std::fabs(e.soloud().getGlobalVolume() - 0.5f) <= 1e-6);

	// Switching to a fade before any output must continue from the midpoint.
	e.soloud().fadeVolume(h, 0.0f, 64.0 / RATE);
	TASK_CHECK(std::fabs(e.soloud().getVolume(h) - 0.5f) <= 1e-6);
	e.soloud().fadeGlobalVolume(0.0f, 64.0 / RATE);
	TASK_CHECK(std::fabs(e.soloud().getGlobalVolume() - 0.5f) <= 1e-6);

	std::vector<float> pcm;
	renderInto(e.soloud(), 8, 8, pcm, 1);
	// Frame 1 of a fade from 0.5 to 0 over 64 frames.
	double expected = linearGain(0.5, 0.0, 64.0 / RATE, 1) * linearGain(0.5, 0.0, 64.0 / RATE, 1) * 0.25 * MONO;
	TASK_CHECK(std::fabs(pcm[0] - expected) <= 1e-5);
	return 0;
}

static int env_refade_continues_from_automation()
{
	// Re-fading mid-transition must start from the actual automatic value,
	// not from the original target.
	Engine e;
	Constant src(0.25f);
	SoLoud::handle h = e.soloud().play(src);
	warm(e.soloud());
	e.soloud().setVolume(h, 1.0f);
	e.soloud().fadeVolume(h, 0.0f, 96.0 / RATE);
	std::vector<float> first;
	renderInto(e.soloud(), 32, 32, first, 1);

	double at32 = linearGain(1.0, 0.0, 96.0 / RATE, 32);
	TASK_CHECK(std::fabs(e.soloud().getVolume(h) - at32) <= 1e-5);
	e.soloud().fadeVolume(h, 1.0f, 64.0 / RATE);
	TASK_CHECK(std::fabs(e.soloud().getVolume(h) - at32) <= 1e-5);

	std::vector<float> second;
	renderInto(e.soloud(), 64, 64, second, 1);
	for (unsigned k = 1; k <= 64; ++k)
	{
		double expected = linearGain(at32, 1.0, 64.0 / RATE, k);
		double gain = second[k - 1] / (0.25 * MONO);
		TASK_CHECK(std::fabs(gain - expected) <= 1e-5);
	}
	return 0;
}

static int env_fixed_volume_control()
{
	// Sanity: with no automation the constant source stays constant and the
	// setter/getter pair is a plain assignment. The first block after a gain
	// change is intentionally ramped by the legacy click guard, so let that
	// settle before checking the steady state.
	Engine e;
	Constant src(0.25f);
	SoLoud::handle h = e.soloud().play(src);
	e.soloud().setVolume(h, 0.4f);
	TASK_CHECK(std::fabs(e.soloud().getVolume(h) - 0.4f) <= 1e-6);
	warm(e.soloud());
	std::vector<float> pcm;
	renderInto(e.soloud(), 64, 7, pcm, 1);
	for (unsigned i = 0; i < 64; ++i)
		TASK_CHECK(std::fabs(pcm[i] - 0.4f * 0.25f * (float)MONO) <= 1e-5);
	return 0;
}

// ---------------------------------------------------------------------------
// clock: the automation clock is the delivered output frame, per voice.
// ---------------------------------------------------------------------------

static int clock_pause_freezes_voice()
{
	// A voice paused during a fade keeps its automation position; on resume it
	// continues from where it stopped.
	Engine e;
	Constant src(0.25f);
	SoLoud::handle h = e.soloud().play(src);
	warm(e.soloud());
	e.soloud().setVolume(h, 1.0f);
	e.soloud().fadeVolume(h, 0.0f, 96.0 / RATE);

	std::vector<float> before;
	renderInto(e.soloud(), 16, 16, before, 1);
	double at16 = linearGain(1.0, 0.0, 96.0 / RATE, 16);
	TASK_CHECK(std::fabs(e.soloud().getVolume(h) - at16) <= 1e-5);

	e.soloud().setPause(h, 1);
	std::vector<float> paused;
	renderInto(e.soloud(), 160, 160, paused, 1);
	TASK_CHECK(std::fabs(e.soloud().getVolume(h) - at16) <= 1e-5);

	e.soloud().setPause(h, 0);
	std::vector<float> after;
	renderInto(e.soloud(), 16, 16, after, 1);
	for (unsigned k = 1; k <= 16; ++k)
	{
		double expected = linearGain(1.0, 0.0, 96.0 / RATE, 16 + k);
		double gain = after[k - 1] / (0.25 * MONO);
		TASK_CHECK(std::fabs(gain - expected) <= 1e-5);
	}
	return 0;
}

static int clock_pause_freezes_lfo()
{
	Engine e;
	Constant src(0.25f);
	SoLoud::handle h = e.soloud().play(src);
	warm(e.soloud());
	const double period = 240.0 / RATE;
	e.soloud().oscillateVolume(h, 0.2f, 1.0f, period);

	std::vector<float> before;
	renderInto(e.soloud(), 60, 20, before, 1);
	// Frame 60 (one-based) is exactly a quarter period -> peak.
	TASK_CHECK(std::fabs(before[59] - 0.25 * MONO) <= 1e-5);

	e.soloud().setPause(h, 1);
	std::vector<float> paused;
	renderInto(e.soloud(), 240, 240, paused, 1);
	e.soloud().setPause(h, 0);
	std::vector<float> after;
	renderInto(e.soloud(), 120, 7, after, 1);
	for (unsigned k = 1; k <= 120; ++k)
	{
		double expected = lfoGain(0.2, 1.0, period, 60 + k) * 0.25 * MONO;
		TASK_CHECK(std::fabs(after[k - 1] - expected) <= 1e-5);
	}
	return 0;
}

static int clock_bus_pause_freezes()
{
	// Pausing the Bus voice freezes that Bus's own automation while the main
	// volume clock keeps running.
	Engine e;
	Constant src(0.5f);
	SoLoud::Bus bus;
	bus.setChannels(1);
	bus.mBaseSamplerate = (float)RATE;
	bus.setResampler(SoLoud::Soloud::RESAMPLER_POINT);
	SoLoud::handle bh = e.soloud().play(bus);
	bus.play(src);
	e.soloud().setPanAbsolute(bh, 1.0f, 1.0f);
	warm(e.soloud());
	e.soloud().setVolume(bh, 1.0f);
	e.soloud().fadeVolume(bh, 0.0f, 96.0 / RATE);
	e.soloud().oscillateGlobalVolume(0.2f, 1.0f, 240.0 / RATE);

	std::vector<float> before;
	renderInto(e.soloud(), 24, 8, before, 1);
	const double busGain = 1.0 / std::sqrt(2.0);
	for (unsigned k = 1; k <= 24; ++k)
	{
		double expected = 0.5 * busGain * linearGain(1.0, 0.0, 96.0 / RATE, k) * lfoGain(0.2, 1.0, 240.0 / RATE, k);
		TASK_CHECK(std::fabs(before[k - 1] - expected) <= 1e-5);
	}

	e.soloud().setPause(bh, 1);
	std::vector<float> paused;
	renderInto(e.soloud(), 120, 120, paused, 1);
	for (unsigned i = 0; i < 120; ++i)
		TASK_CHECK(std::fabs(paused[i]) <= 1e-5); // bus paused -> silence

	e.soloud().setPause(bh, 0);
	std::vector<float> after;
	renderInto(e.soloud(), 24, 7, after, 1);
	for (unsigned k = 1; k <= 24; ++k)
	{
		// Bus automation resumes at frame 24; main clock advanced by 120.
		double expected = 0.5 * busGain * linearGain(1.0, 0.0, 96.0 / RATE, 24 + k) * lfoGain(0.2, 1.0, 240.0 / RATE, 144 + k);
		TASK_CHECK(std::fabs(after[k - 1] - expected) <= 1e-5);
	}
	return 0;
}

static int clock_global_runs_while_voice_paused()
{
	// A voice pause must not stop the main volume clock.
	Engine e;
	Constant src(0.25f);
	SoLoud::handle h = e.soloud().play(src);
	warm(e.soloud());
	e.soloud().oscillateGlobalVolume(0.2f, 1.0f, 240.0 / RATE);

	std::vector<float> first;
	renderInto(e.soloud(), 60, 60, first, 1);
	TASK_CHECK(std::fabs(first[59] - 0.25 * MONO) <= 1e-5); // quarter period peak

	e.soloud().setPause(h, 1);
	std::vector<float> paused;
	renderInto(e.soloud(), 240, 240, paused, 1);
	// Main clock advanced by 240 frames while the voice was silent.
	TASK_CHECK(std::fabs(e.soloud().getGlobalVolume() - (float)lfoGain(0.2, 1.0, 240.0 / RATE, 300)) <= 1e-5);

	e.soloud().setPause(h, 0);
	std::vector<float> after;
	renderInto(e.soloud(), 60, 60, after, 1);
	for (unsigned k = 1; k <= 60; ++k)
	{
		double expected = lfoGain(0.2, 1.0, 240.0 / RATE, 300 + k) * 0.25 * MONO;
		TASK_CHECK(std::fabs(after[k - 1] - expected) <= 1e-5);
	}
	return 0;
}

static int clock_getter_matches_last_frame()
{
	// getVolume/getGlobalVolume report the value at the last delivered frame.
	Engine e;
	Constant src(0.25f);
	SoLoud::handle h = e.soloud().play(src);
	warm(e.soloud());
	e.soloud().setVolume(h, 1.0f);
	e.soloud().fadeVolume(h, 0.0f, 100.0 / RATE);
	e.soloud().fadeGlobalVolume(0.0f, 200.0 / RATE);

	std::vector<float> pcm;
	renderInto(e.soloud(), 33, 7, pcm, 1);
	TASK_CHECK(std::fabs(e.soloud().getVolume(h) - (float)linearGain(1.0, 0.0, 100.0 / RATE, 33)) <= 1e-5);
	TASK_CHECK(std::fabs(e.soloud().getGlobalVolume() - (float)linearGain(1.0, 0.0, 200.0 / RATE, 33)) <= 1e-5);
	return 0;
}

static int clock_nonpositive_duration_is_immediate()
{
	// Original behaviour for non-positive durations: apply right away.
	Engine e;
	Constant src(0.25f);
	SoLoud::handle h = e.soloud().play(src);
	warm(e.soloud());
	e.soloud().setVolume(h, 1.0f);
	e.soloud().fadeVolume(h, 0.125f, 0.0);
	TASK_CHECK(std::fabs(e.soloud().getVolume(h) - 0.125f) <= 1e-6);
	// The legacy click ramp applies on the first block; wait it out, then the
	// volume is the plain fixed value.
	warm(e.soloud());
	std::vector<float> pcm;
	renderInto(e.soloud(), 16, 16, pcm, 1);
	for (unsigned i = 0; i < 16; ++i)
		TASK_CHECK(std::fabs(pcm[i] - 0.125f * 0.25f * (float)MONO) <= 1e-5);
	return 0;
}

// ---------------------------------------------------------------------------
// routing: automation multiplies at the original gain stage; bus and main
// gain are applied to the same output frame; formats and splits agree.
// ---------------------------------------------------------------------------

static int routing_bus_plus_global()
{
	// A bus fade and a main fade must multiply on the same output frame.
	Engine e;
	Constant src(0.5f);
	SoLoud::Bus bus;
	bus.setChannels(1);
	bus.mBaseSamplerate = (float)RATE;
	bus.setResampler(SoLoud::Soloud::RESAMPLER_POINT);
	SoLoud::handle bh = e.soloud().play(bus);
	bus.play(src);
	e.soloud().setPanAbsolute(bh, 1.0f, 1.0f);
	warm(e.soloud());

	// Reference: the bus route scales a mono source by 1 / sqrt(2).
	const double busGain = 1.0 / std::sqrt(2.0);
	e.soloud().setVolume(bh, 1.0f);
	e.soloud().fadeVolume(bh, 0.0f, 64.0 / RATE);
	e.soloud().fadeGlobalVolume(0.0f, 32.0 / RATE);

	std::vector<float> pcm;
	renderInto(e.soloud(), 96, 96, pcm, 1);
	for (unsigned k = 1; k <= 96; ++k)
	{
		double expected = 0.5 * busGain * linearGain(1.0, 0.0, 64.0 / RATE, k) * linearGain(1.0, 0.0, 32.0 / RATE, k);
		TASK_CHECK(std::fabs(pcm[k - 1] - expected) <= 1e-5);
	}
	return 0;
}

static int routing_bus_global_split_equivalence()
{
	Engine a;
	Constant sa(0.5f);
	SoLoud::Bus ba;
	ba.setChannels(1);
	ba.mBaseSamplerate = (float)RATE;
	ba.setResampler(SoLoud::Soloud::RESAMPLER_POINT);
	SoLoud::handle bha = a.soloud().play(ba);
	ba.play(sa);
	warm(a.soloud());
	a.soloud().fadeVolume(bha, 0.0f, 64.0 / RATE);
	a.soloud().fadeGlobalVolume(0.0f, 32.0 / RATE);
	std::vector<float> whole;
	renderInto(a.soloud(), 128, 128, whole, 1);

	for (unsigned block : {1u, 7u, 257u})
	{
		Engine b;
		Constant sb(0.5f);
		SoLoud::Bus bb;
		bb.setChannels(1);
		bb.mBaseSamplerate = (float)RATE;
		bb.setResampler(SoLoud::Soloud::RESAMPLER_POINT);
		SoLoud::handle bhb = b.soloud().play(bb);
		bb.play(sb);
		warm(b.soloud());
		b.soloud().fadeVolume(bhb, 0.0f, 64.0 / RATE);
		b.soloud().fadeGlobalVolume(0.0f, 32.0 / RATE);
		std::vector<float> split;
		renderInto(b.soloud(), 128, block, split, 1);
		for (unsigned i = 0; i < 128; ++i)
			TASK_CHECK(std::fabs(split[i] - whole[i]) <= 1e-5);
	}
	return 0;
}

static int routing_voice_split_stereo()
{
	// Stereo output must be block-size independent too.
	Engine a(2);
	Constant sa(0.25f);
	SoLoud::handle ha = a.soloud().play(sa);
	warm(a.soloud(), 2);
	a.soloud().fadeVolume(ha, 0.0f, 96.0 / RATE);
	std::vector<float> whole;
	renderInto(a.soloud(), 160, 160, whole, 2);

	Engine b(2);
	Constant sb(0.25f);
	SoLoud::handle hb = b.soloud().play(sb);
	warm(b.soloud(), 2);
	b.soloud().fadeVolume(hb, 0.0f, 96.0 / RATE);
	std::vector<float> split;
	renderInto(b.soloud(), 160, 7, split, 2);

	for (unsigned f = 0; f < 160; ++f)
	{
		TASK_CHECK(std::fabs(sampleAt(split, 2, f, 0) - sampleAt(whole, 2, f, 0)) <= 1e-5);
		TASK_CHECK(std::fabs(sampleAt(split, 2, f, 1) - sampleAt(whole, 2, f, 1)) <= 1e-5);
	}
	// Frames must be equal across the left and right channels (fixed pan).
	for (unsigned f = 0; f < 160; ++f)
		TASK_CHECK(std::fabs(sampleAt(whole, 2, f, 0) - sampleAt(whole, 2, f, 1)) <= 1e-5);
	return 0;
}

static int routing_short_output_format()
{
	// mixSigned16 must carry the same envelope as float mix, and must be
	// block-size independent; allow one quantisation unit.
	Engine e;
	Constant src(0.25f);
	SoLoud::handle h = e.soloud().play(src);
	warm(e.soloud());
	e.soloud().fadeVolume(h, 0.0f, 64.0 / RATE);

	std::vector<short> pcm(64);
	e.soloud().mixSigned16(pcm.data(), 64);
	for (unsigned k = 1; k <= 64; ++k)
	{
		double expected = linearGain(1.0, 0.0, 64.0 / RATE, k) * 0.25 * MONO;
		short want = (short)(expected * 0x7fff);
		TASK_CHECK(std::abs((int)pcm[k - 1] - (int)want) <= 1);
	}

	// Chopped 16-bit blocks must match the single block.
	Engine e2;
	Constant src2(0.25f);
	SoLoud::handle h2 = e2.soloud().play(src2);
	warm(e2.soloud());
	e2.soloud().fadeVolume(h2, 0.0f, 64.0 / RATE);
	std::vector<short> split(64);
	for (unsigned n = 0; n < 64; n += 7)
	{
		unsigned count = (n + 7 > 64) ? 64 - n : 7;
		e2.soloud().mixSigned16(split.data() + n, count);
	}
	for (unsigned i = 0; i < 64; ++i)
		TASK_CHECK(std::abs((int)split[i] - (int)pcm[i]) <= 1);
	return 0;
}

static int routing_bus_child_volume_fixed()
{
	// Bus child voices keep their fixed set volume; only the bus (and main)
	// faders are automated.
	Engine e;
	Constant src(0.5f);
	SoLoud::Bus bus;
	bus.setChannels(1);
	bus.mBaseSamplerate = (float)RATE;
	bus.setResampler(SoLoud::Soloud::RESAMPLER_POINT);
	SoLoud::handle bh = e.soloud().play(bus);
	bus.play(src);
	e.soloud().setPanAbsolute(bh, 1.0f, 1.0f);
	e.soloud().setVolume(bh, 0.5f);
	// Let the legacy click ramp settle, then the gain is steady.
	warm(e.soloud());
	// Fade only the child voice; a bus child is not required to automate, so
	// the child stays at its set volume and the output is constant.
	std::vector<float> pcm;
	renderInto(e.soloud(), 32, 32, pcm, 1);
	const float expected = 0.5f * 0.5f * (float)(1.0 / std::sqrt(2.0));
	for (unsigned i = 0; i < 32; ++i)
		TASK_CHECK(std::fabs(pcm[i] - expected) <= 1e-5);
	return 0;
}

static int routing_automation_before_clip()
{
	// Automation happens before the clipper: a moderate gain fade keeps the
	// signal inside the limiter and scales linearly.
	Engine e;
	Constant src(0.9f);
	SoLoud::handle h = e.soloud().play(src);
	warm(e.soloud());
	e.soloud().fadeVolume(h, 0.3f, 32.0 / RATE);
	std::vector<float> pcm;
	renderInto(e.soloud(), 32, 4, pcm, 1);
	for (unsigned k = 1; k <= 32; ++k)
	{
		double gain = linearGain(1.0, 0.3, 32.0 / RATE, k);
		double expected = 0.9 * gain * (1.0 / std::sqrt(2.0));
		TASK_CHECK(std::fabs(pcm[k - 1] - expected) <= 1e-5);
	}
	return 0;
}

// ---------------------------------------------------------------------------
// Registry consumed by task_support/driver.c
// ---------------------------------------------------------------------------

const struct task_case task_cases[] =
{
	{"envelope", "voice_fade_short_block", env_voice_fade_short_block},
	{"envelope", "voice_fade_split_equivalence", env_voice_fade_split_equivalence},
	{"envelope", "global_fade_short_block", env_global_fade_short_block},
	{"envelope", "voice_lfo_peaks_cross_blocks", env_voice_lfo_peaks_cross_blocks},
	{"envelope", "global_lfo_peaks_cross_blocks", env_global_lfo_peaks_cross_blocks},
	{"envelope", "lfo_bounds_ordered", env_lfo_bounds_ordered},
	{"envelope", "immediate_getter_midpoint", env_immediate_getter_midpoint},
	{"envelope", "refade_continues_from_automation", env_refade_continues_from_automation},
	{"envelope", "fixed_volume_control", env_fixed_volume_control},

	{"clock", "pause_freezes_voice", clock_pause_freezes_voice},
	{"clock", "pause_freezes_lfo", clock_pause_freezes_lfo},
	{"clock", "bus_pause_freezes", clock_bus_pause_freezes},
	{"clock", "global_runs_while_voice_paused", clock_global_runs_while_voice_paused},
	{"clock", "getter_matches_last_frame", clock_getter_matches_last_frame},
	{"clock", "nonpositive_duration_is_immediate", clock_nonpositive_duration_is_immediate},

	{"routing", "bus_plus_global", routing_bus_plus_global},
	{"routing", "bus_global_split_equivalence", routing_bus_global_split_equivalence},
	{"routing", "voice_split_stereo", routing_voice_split_stereo},
	{"routing", "short_output_format", routing_short_output_format},
	{"routing", "bus_child_volume_fixed", routing_bus_child_volume_fixed},
	{"routing", "automation_before_clip", routing_automation_before_clip},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
