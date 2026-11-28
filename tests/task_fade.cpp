// Volume automation frame-time tests for SoLoud's null driver.
//
// Convention under test: every control call happens between two mix() calls
// and starts from the audio already delivered. For a call made at output
// position t0, output frame k (k = 1, 2, ...) after the call is at
// t0 + k / samplerate. A linear fade from v0 to v1 over T seconds therefore
// takes the value v0 + (v1 - v0) * (k / (T * samplerate)) and holds v1 once
// k >= T * samplerate. The first sample of an LFO is likewise evaluated at
// k = 1, using sin(2*pi*k/(T*samplerate)) around the midpoint of the bounds.

#include "task_support/check.h"

#include "soloud.h"
#include "soloud_bus.h"

#include <vector>
#include <cmath>
#include <algorithm>

namespace
{
	const double kRate = 48000.0;
	const float kAmp = 0.25f;

	typedef std::vector<unsigned int> Blocks;

	class ConstInstance : public SoLoud::AudioSourceInstance
	{
	public:
		float mL;
		float mR;
		ConstInstance(float aL, float aR) : mL(aL), mR(aR) {}
		unsigned int getAudio(float *aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize) override
		{
			for (unsigned int i = 0; i < aSamplesToRead; i++)
			{
				aBuffer[i] = mL;
				if (mChannels > 1)
					aBuffer[aBufferSize + i] = mR;
			}
			return aSamplesToRead;
		}
		bool hasEnded() override { return false; }
	};

	class ConstSource : public SoLoud::AudioSource
	{
	public:
		float mL;
		float mR;
		ConstSource(float aL, float aR = 0.0f, unsigned int aChannels = 1)
		{
			mL = aL;
			mR = aR;
			mBaseSamplerate = 48000.0f;
			mChannels = aChannels;
		}
		SoLoud::AudioSourceInstance *createInstance() override
		{
			return new ConstInstance(mL, mChannels > 1 ? mR : mL);
		}
	};

	bool initEngine(SoLoud::Soloud &aS, unsigned int aChannels)
	{
		if (aS.init(0, SoLoud::Soloud::NULLDRIVER, 48000, 512, aChannels) != 0)
			return false;
		aS.setPostClipScaler(1.0f);
		aS.setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);
		return true;
	}

	void mixFrames(SoLoud::Soloud &aS, unsigned int aFrames)
	{
		std::vector<float> buffer(aFrames * aS.getBackendChannels());
		aS.mix(buffer.data(), aFrames);
	}

	std::vector<float> renderFloat(SoLoud::Soloud &aS, const Blocks &aBlocks)
	{
		unsigned int channels = aS.getBackendChannels();
		std::vector<float> out;
		for (size_t i = 0; i < aBlocks.size(); i++)
		{
			unsigned int n = aBlocks[i];
			std::vector<float> buffer(n * channels);
			aS.mix(buffer.data(), n);
			out.insert(out.end(), buffer.begin(), buffer.end());
		}
		return out;
	}

	std::vector<short> renderShort(SoLoud::Soloud &aS, const Blocks &aBlocks)
	{
		unsigned int channels = aS.getBackendChannels();
		std::vector<short> out;
		for (size_t i = 0; i < aBlocks.size(); i++)
		{
			unsigned int n = aBlocks[i];
			std::vector<short> buffer(n * channels);
			aS.mixSigned16(buffer.data(), n);
			out.insert(out.end(), buffer.begin(), buffer.end());
		}
		return out;
	}

	// Value of a linear fade on output frame k (k starts at 1); frames at or
	// past the duration hold the target.
	double fadeAt(double aFrom, double aTo, double aDurationFrames, double aK)
	{
		if (aDurationFrames <= 0)
			return aTo;
		double x = aK / aDurationFrames;
		if (x >= 1.0)
			return aTo;
		if (x <= 0.0)
			return aFrom;
		return aFrom + (aTo - aFrom) * x;
	}

	double lfoAt(double aLo, double aHi, double aDurationFrames, double aK)
	{
		double mid = (aLo + aHi) * 0.5;
		double amp = (aHi - aLo) * 0.5;
		return mid + amp * sin(2.0 * M_PI * aK / aDurationFrames);
	}

	float maxAbsDiff(const std::vector<float> &a, const std::vector<float> &b)
	{
		float d = 0;
		for (size_t i = 0; i < a.size() && i < b.size(); i++)
			d = std::max(d, (float)fabs(a[i] - b[i]));
		return d;
	}

	// Renders a mono constant source through one voice with a voice-volume
	// fade, using the given block partition after the control call.
	std::vector<float> renderVoiceFade(const Blocks &aBlocks, float aPlayVolume, float aTo, double aDurationFrames)
	{
		ConstSource src(kAmp);
		SoLoud::Soloud s;
		if (!initEngine(s, 2))
			return std::vector<float>();
		SoLoud::handle h = s.play(src, aPlayVolume);
		s.setPanAbsolute(h, 1.0f, 1.0f);
		mixFrames(s, 256);
		double duration = aDurationFrames / kRate;
		s.fadeVolume(h, aTo, duration);
		return renderFloat(s, aBlocks);
	}

	std::vector<float> renderGlobalFade(const Blocks &aBlocks, float aPlayVolume, float aTo, double aDurationFrames)
	{
		ConstSource src(kAmp);
		SoLoud::Soloud s;
		if (!initEngine(s, 2))
			return std::vector<float>();
		SoLoud::handle h = s.play(src, aPlayVolume);
		s.setPanAbsolute(h, 1.0f, 1.0f);
		mixFrames(s, 256);
		double duration = aDurationFrames / kRate;
		s.fadeGlobalVolume(aTo, duration);
		return renderFloat(s, aBlocks);
	}

	// ------------------------------------------------------------------
	// envelope
	// ------------------------------------------------------------------

	int case_voice_fade_short_endpoint()
	{
		Blocks whole;
		whole.push_back(128);
		Blocks chopped;
		for (int i = 0; i < 8; i++)
			chopped.push_back(16);

		std::vector<float> a = renderVoiceFade(whole, 1.0f, 0.0f, 64.0);
		std::vector<float> b = renderVoiceFade(chopped, 1.0f, 0.0f, 64.0);
		TASK_CHECK(a.size() == 256 && b.size() == 256);
		TASK_CHECK(maxAbsDiff(a, b) <= 1e-5f);

		for (unsigned int frame = 0; frame < 128; frame++)
		{
			float expected = (float)(kAmp * fadeAt(1.0, 0.0, 64.0, frame + 1.0));
			TASK_CHECK(fabs(a[frame * 2] - expected) <= 1e-5f);
			TASK_CHECK(fabs(a[frame * 2 + 1] - expected) <= 1e-5f);
			if (frame >= 64)
				TASK_CHECK(fabs(a[frame * 2]) <= 1e-7f);
		}
		// The fade must be over by frame 64, not stretched over the 128 block.
		return 0;
	}

	int case_global_fade_short_endpoint()
	{
		Blocks whole;
		whole.push_back(128);
		Blocks chopped;
		for (int i = 0; i < 8; i++)
			chopped.push_back(16);

		std::vector<float> a = renderGlobalFade(whole, 1.0f, 0.0f, 64.0);
		std::vector<float> b = renderGlobalFade(chopped, 1.0f, 0.0f, 64.0);
		TASK_CHECK(a.size() == 256 && b.size() == 256);
		TASK_CHECK(maxAbsDiff(a, b) <= 1e-5f);
		for (unsigned int frame = 0; frame < 128; frame++)
		{
			float expected = (float)(kAmp * fadeAt(1.0, 0.0, 64.0, frame + 1.0));
			TASK_CHECK(fabs(a[frame * 2] - expected) <= 1e-5f);
			if (frame >= 64)
				TASK_CHECK(fabs(a[frame * 2]) <= 1e-7f);
		}
		return 0;
	}

	int case_fade_arbitrary_blocks()
	{
		Blocks single;
		single.push_back(265);
		Blocks split;
		split.push_back(1);
		split.push_back(7);
		split.push_back(257);

		std::vector<float> a = renderVoiceFade(single, 1.0f, 0.2f, 64.0);
		std::vector<float> b = renderVoiceFade(split, 1.0f, 0.2f, 64.0);
		TASK_CHECK(a.size() == 265 * 2 && b.size() == 265 * 2);
		TASK_CHECK(maxAbsDiff(a, b) <= 1e-5f);
		for (unsigned int frame = 0; frame < 265; frame++)
		{
			float expected = (float)(kAmp * fadeAt(1.0, 0.2, 64.0, frame + 1.0));
			TASK_CHECK(fabs(a[frame * 2] - expected) <= 1e-5f);
		}
		return 0;
	}

	std::vector<float> renderVoiceOsc(const Blocks &aBlocks, float aLo, float aHi, double aDurationFrames)
	{
		ConstSource src(kAmp);
		SoLoud::Soloud s;
		if (!initEngine(s, 2))
			return std::vector<float>();
		SoLoud::handle h = s.play(src, 1.0f);
		s.setPanAbsolute(h, 1.0f, 1.0f);
		mixFrames(s, 256);
		double duration = aDurationFrames / kRate;
		s.oscillateVolume(h, aLo, aHi, duration);
		return renderFloat(s, aBlocks);
	}

	std::vector<float> renderGlobalOsc(const Blocks &aBlocks, float aLo, float aHi, double aDurationFrames)
	{
		ConstSource src(kAmp);
		SoLoud::Soloud s;
		if (!initEngine(s, 2))
			return std::vector<float>();
		SoLoud::handle h = s.play(src, 1.0f);
		s.setPanAbsolute(h, 1.0f, 1.0f);
		mixFrames(s, 256);
		double duration = aDurationFrames / kRate;
		s.oscillateGlobalVolume(aLo, aHi, duration);
		return renderFloat(s, aBlocks);
	}

	int case_osc_cross_block_peak()
	{
		Blocks whole;
		whole.push_back(128);
		Blocks chopped;
		for (int i = 0; i < 16; i++)
			chopped.push_back(8);

		std::vector<float> a = renderVoiceOsc(whole, 0.2f, 1.0f, 64.0);
		std::vector<float> b = renderVoiceOsc(chopped, 0.2f, 1.0f, 64.0);
		TASK_CHECK(a.size() == 256 && b.size() == 256);
		TASK_CHECK(maxAbsDiff(a, b) <= 1e-5f);
		for (unsigned int frame = 0; frame < 128; frame++)
		{
			float expected = (float)(kAmp * lfoAt(0.2, 1.0, 64.0, frame + 1.0));
			TASK_CHECK(fabs(a[frame * 2] - expected) <= 1e-5f);
		}
		// Peak at quarter period, trough at three quarters; both are inside
		// the first 128-frame block, so endpoint interpolation would miss them.
		TASK_CHECK(fabs(a[(16 - 1) * 2] - (float)(kAmp * 1.0)) <= 1e-4f);
		TASK_CHECK(fabs(a[(48 - 1) * 2] - (float)(kAmp * 0.2)) <= 1e-4f);
		return 0;
	}

	int case_global_osc_cross_block_peak()
	{
		Blocks whole;
		whole.push_back(128);
		Blocks chopped;
		for (int i = 0; i < 16; i++)
			chopped.push_back(8);

		std::vector<float> a = renderGlobalOsc(whole, 0.2f, 1.0f, 64.0);
		std::vector<float> b = renderGlobalOsc(chopped, 0.2f, 1.0f, 64.0);
		TASK_CHECK(a.size() == 256 && b.size() == 256);
		TASK_CHECK(maxAbsDiff(a, b) <= 1e-5f);
		for (unsigned int frame = 0; frame < 128; frame++)
		{
			float expected = (float)(kAmp * lfoAt(0.2, 1.0, 64.0, frame + 1.0));
			TASK_CHECK(fabs(a[frame * 2] - expected) <= 1e-5f);
		}
		TASK_CHECK(fabs(a[(16 - 1) * 2] - (float)(kAmp * 1.0)) <= 1e-4f);
		TASK_CHECK(fabs(a[(48 - 1) * 2] - (float)(kAmp * 0.2)) <= 1e-4f);
		return 0;
	}

	int case_fade_restart_continuity()
	{
		ConstSource src(kAmp);
		SoLoud::Soloud s;
		TASK_CHECK(initEngine(s, 2));
		SoLoud::handle h = s.play(src, 0.8f);
		s.setPanAbsolute(h, 1.0f, 1.0f);
		mixFrames(s, 256);

		s.fadeVolume(h, 0.2f, 100.0 / kRate);
		mixFrames(s, 10);
		double atTen = fadeAt(0.8, 0.2, 100.0, 10.0);
		TASK_CHECK(fabs(s.getVolume(h) - atTen) <= 1e-5);

		// Re-target from the value actually delivered, not the original start.
		s.fadeVolume(h, 0.5f, 50.0 / kRate);
		TASK_CHECK(fabs(s.getVolume(h) - atTen) <= 1e-5);

		std::vector<float> out = renderFloat(s, Blocks(1, 10));
		TASK_CHECK(out.size() == 20);
		for (unsigned int frame = 0; frame < 10; frame++)
		{
			float expected = (float)(kAmp * fadeAt(atTen, 0.5, 50.0, frame + 1.0));
			TASK_CHECK(fabs(out[frame * 2] - expected) <= 1e-5f);
		}
		TASK_CHECK(fabs(s.getVolume(h) - fadeAt(atTen, 0.5, 50.0, 10.0)) <= 1e-5);
		return 0;
	}

	int case_osc_redirect_before_output()
	{
		ConstSource src(kAmp);
		SoLoud::Soloud s;
		TASK_CHECK(initEngine(s, 2));
		SoLoud::handle h = s.play(src, 1.0f);
		s.setPanAbsolute(h, 1.0f, 1.0f);
		mixFrames(s, 256);

		s.oscillateVolume(h, 0.2f, 1.0f, 64.0 / kRate);
		// Before any output the LFO sits at the zero crossing, the midpoint.
		TASK_CHECK(fabs(s.getVolume(h) - 0.6f) <= 1e-6f);

		// Switch to a fade before the oscillator has produced a frame; it must
		// continue from the midpoint.
		s.fadeVolume(h, 0.3f, 40.0 / kRate);
		TASK_CHECK(fabs(s.getVolume(h) - 0.6f) <= 1e-6f);

		std::vector<float> out = renderFloat(s, Blocks(1, 5));
		TASK_CHECK(out.size() == 10);
		for (unsigned int frame = 0; frame < 5; frame++)
		{
			float expected = (float)(kAmp * fadeAt(0.6, 0.3, 40.0, frame + 1.0));
			TASK_CHECK(fabs(out[frame * 2] - expected) <= 1e-5f);
		}
		return 0;
	}

	int case_nonpositive_duration()
	{
		ConstSource src(kAmp);
		SoLoud::Soloud s;
		TASK_CHECK(initEngine(s, 2));
		SoLoud::handle h = s.play(src, 1.0f);
		s.setPanAbsolute(h, 1.0f, 1.0f);
		mixFrames(s, 256);

		s.fadeVolume(h, 0.4f, 0.0);
		TASK_CHECK(fabs(s.getVolume(h) - 0.4f) <= 1e-6f);
		s.fadeVolume(h, 0.6f, -1.0);
		TASK_CHECK(fabs(s.getVolume(h) - 0.6f) <= 1e-6f);
		s.oscillateVolume(h, 0.3f, 0.3f, 10.0 / kRate);
		TASK_CHECK(fabs(s.getVolume(h) - 0.3f) <= 1e-6f);

		s.fadeGlobalVolume(0.5f, 0.0);
		TASK_CHECK(fabs(s.getGlobalVolume() - 0.5f) <= 1e-6f);
		s.oscillateGlobalVolume(0.5f, 0.5f, 10.0 / kRate);
		TASK_CHECK(fabs(s.getGlobalVolume() - 0.5f) <= 1e-6f);

		// Once the block ramp that belongs to a plain set has settled the
		// output is the new constant value.
		mixFrames(s, 32);
		std::vector<float> out = renderFloat(s, Blocks(1, 16));
		for (unsigned int frame = 0; frame < 16; frame++)
			TASK_CHECK(fabs(out[frame * 2] - kAmp * 0.3f * 0.5f) <= 1e-5f);
		return 0;
	}

	int case_fixed_volume_control()
	{
		ConstSource src(kAmp);
		SoLoud::Soloud s;
		TASK_CHECK(initEngine(s, 2));
		SoLoud::handle h = s.play(src, 0.75f);
		s.setPanAbsolute(h, 1.0f, 1.0f);
		mixFrames(s, 256);

		std::vector<float> a = renderFloat(s, Blocks(1, 128));
		TASK_CHECK(a.size() == 256);
		for (unsigned int frame = 0; frame < 128; frame++)
			TASK_CHECK(fabs(a[frame * 2] - kAmp * 0.75f) <= 1e-5f);
		// Same constant render split into 16-frame blocks (8 blocks = 128
		// frames).
		Blocks sixteen;
		for (int i = 0; i < 8; i++)
			sixteen.push_back(16);
		std::vector<float> b = renderFloat(s, sixteen);
		TASK_CHECK(b.size() == 256);
		TASK_CHECK(maxAbsDiff(a, b) <= 1e-5f);

		// A plain setVolume() keeps its existing anti-click ramp: within the
		// following block the gain moves from the old value toward the new one.
		s.setVolume(h, 0.25f);
		std::vector<float> ramp = renderFloat(s, Blocks(1, 16));
		TASK_CHECK(ramp.size() == 32);
		float first = ramp[0] / kAmp;
		float last = ramp[15 * 2] / kAmp;
		TASK_CHECK(first > 0.25f && first < 0.75f);
		TASK_CHECK(fabs(last - 0.25f) <= 1e-5f);
		return 0;
	}

	int case_short_output_format()
	{
		ConstSource src(kAmp);
		SoLoud::Soloud sf;
		SoLoud::Soloud ss;
		TASK_CHECK(initEngine(sf, 2));
		TASK_CHECK(initEngine(ss, 2));
		SoLoud::handle hf = sf.play(src, 1.0f);
		SoLoud::handle hs = ss.play(src, 1.0f);
		sf.setPanAbsolute(hf, 1.0f, 1.0f);
		ss.setPanAbsolute(hs, 1.0f, 1.0f);
		mixFrames(sf, 256);
		mixFrames(ss, 256);
		sf.fadeVolume(hf, 0.0f, 64.0 / kRate);
		ss.fadeVolume(hs, 0.0f, 64.0 / kRate);

		Blocks blocks;
		blocks.push_back(7);
		blocks.push_back(121);

		std::vector<float> f = renderFloat(sf, blocks);
		std::vector<short> q = renderShort(ss, blocks);
		TASK_CHECK(f.size() == 256 && q.size() == 256);
		for (unsigned int frame = 0; frame < 128; frame++)
		{
			float expected = (float)(kAmp * fadeAt(1.0, 0.0, 64.0, frame + 1.0));
			TASK_CHECK((short)(expected * 0x7fff) - q[frame * 2] <= 1);
			TASK_CHECK(q[frame * 2] - (short)(expected * 0x7fff) <= 1);
			TASK_CHECK(fabs(f[frame * 2] - expected) <= 1e-5f);
		}
		return 0;
	}

	// ------------------------------------------------------------------
	// clock
	// ------------------------------------------------------------------

	int case_voice_frame_time_getter()
	{
		ConstSource src(kAmp);
		SoLoud::Soloud s;
		TASK_CHECK(initEngine(s, 2));
		SoLoud::handle h = s.play(src, 1.0f);
		s.setPanAbsolute(h, 1.0f, 1.0f);
		mixFrames(s, 256);
		s.fadeVolume(h, 0.1f, 64.0 / kRate);

		for (unsigned int frame = 1; frame <= 8; frame++)
		{
			mixFrames(s, 1);
			// Frame k is at k/R; the getter reports the last delivered frame.
			TASK_CHECK(fabs(s.getVolume(h) - fadeAt(1.0, 0.1, 64.0, frame)) <= 1e-5);
		}
		return 0;
	}

	int case_global_frame_time_getter()
	{
		ConstSource src(kAmp);
		SoLoud::Soloud s;
		TASK_CHECK(initEngine(s, 2));
		SoLoud::handle h = s.play(src, 1.0f);
		s.setPanAbsolute(h, 1.0f, 1.0f);
		mixFrames(s, 256);
		s.fadeGlobalVolume(0.1f, 64.0 / kRate);

		for (unsigned int frame = 1; frame <= 8; frame++)
		{
			mixFrames(s, 1);
			TASK_CHECK(fabs(s.getGlobalVolume() - fadeAt(1.0, 0.1, 64.0, frame)) <= 1e-5);
		}
		return 0;
	}

	int case_pause_resume_voice()
	{
		ConstSource src(kAmp);
		SoLoud::Soloud s;
		TASK_CHECK(initEngine(s, 2));
		SoLoud::handle h = s.play(src, 1.0f);
		s.setPanAbsolute(h, 1.0f, 1.0f);
		mixFrames(s, 256);
		s.fadeVolume(h, 0.0f, 100.0 / kRate);

		mixFrames(s, 10);
		double frozen = fadeAt(1.0, 0.0, 100.0, 10.0);
		TASK_CHECK(fabs(s.getVolume(h) - frozen) <= 1e-5);

		s.setPause(h, true);
		mixFrames(s, 50);
		// The voice's own automation clock is frozen while paused.
		TASK_CHECK(fabs(s.getVolume(h) - frozen) <= 1e-5);

		s.setPause(h, false);
		std::vector<float> out = renderFloat(s, Blocks(1, 10));
		TASK_CHECK(out.size() == 20);
		for (unsigned int frame = 0; frame < 10; frame++)
		{
			// Continues at frames 11..20, not 61..70.
			float expected = (float)(kAmp * fadeAt(1.0, 0.0, 100.0, 11.0 + frame));
			TASK_CHECK(fabs(out[frame * 2] - expected) <= 1e-5f);
		}
		TASK_CHECK(fabs(s.getVolume(h) - fadeAt(1.0, 0.0, 100.0, 20.0)) <= 1e-5);
		return 0;
	}

	int case_global_clock_advances_with_output()
	{
		ConstSource src(kAmp);
		SoLoud::Soloud s;
		TASK_CHECK(initEngine(s, 2));
		SoLoud::handle h = s.play(src, 1.0f);
		s.setPanAbsolute(h, 1.0f, 1.0f);
		mixFrames(s, 256);
		s.fadeGlobalVolume(0.0f, 200.0 / kRate);

		mixFrames(s, 10);
		TASK_CHECK(fabs(s.getGlobalVolume() - fadeAt(1.0, 0.0, 200.0, 10.0)) <= 1e-5);
		s.setPauseAll(true);
		mixFrames(s, 50);
		// Rendering (and hence the main volume clock) keeps advancing even
		// though the voices are paused.
		TASK_CHECK(fabs(s.getGlobalVolume() - fadeAt(1.0, 0.0, 200.0, 60.0)) <= 1e-5);
		s.setPauseAll(false);
		mixFrames(s, 10);
		TASK_CHECK(fabs(s.getGlobalVolume() - fadeAt(1.0, 0.0, 200.0, 70.0)) <= 1e-5);
		return 0;
	}

	int case_last_frame_getter_large_block()
	{
		ConstSource src(kAmp);
		SoLoud::Soloud s;
		TASK_CHECK(initEngine(s, 2));
		SoLoud::handle h = s.play(src, 1.0f);
		s.setPanAbsolute(h, 1.0f, 1.0f);
		mixFrames(s, 256);
		s.fadeVolume(h, 0.2f, 1000.0 / kRate);
		s.fadeGlobalVolume(0.3f, 1000.0 / kRate);

		std::vector<float> out = renderFloat(s, Blocks(1, 257));
		TASK_CHECK(out.size() == 257 * 2);
		for (unsigned int frame = 0; frame < 257; frame++)
		{
			float expected = (float)(kAmp * fadeAt(1.0, 0.2, 1000.0, frame + 1.0) *
				fadeAt(1.0, 0.3, 1000.0, frame + 1.0));
			TASK_CHECK(fabs(out[frame * 2] - expected) <= 1e-5f);
		}
		// After the call the scalar getters report the last delivered frame.
		TASK_CHECK(fabs(s.getVolume(h) - fadeAt(1.0, 0.2, 1000.0, 257.0)) <= 1e-5);
		TASK_CHECK(fabs(s.getGlobalVolume() - fadeAt(1.0, 0.3, 1000.0, 257.0)) <= 1e-5);
		return 0;
	}

	// ------------------------------------------------------------------
	// routing
	// ------------------------------------------------------------------

	struct BusRig
	{
		ConstSource src;
		SoLoud::Bus bus;
		SoLoud::Soloud s;
		SoLoud::handle busHandle;
		SoLoud::handle childHandle;
		// Bus gain of the frame the bus voice last delivered (starts at the
		// unity volume used when the bus was started).
		double mGain;
		BusRig() : src(kAmp)
		{
			busHandle = 0;
			childHandle = 0;
			mGain = 1.0;
		}
		bool init()
		{
			if (!initEngine(s, 2))
				return false;
			// The bus runs at the backend rate with a point resampler, as the
			// task requires.
			bus.mBaseSamplerate = 48000.0f;
			bus.setResampler(SoLoud::Soloud::RESAMPLER_POINT);
			busHandle = s.play(bus);
			if (!busHandle)
				return false;
			childHandle = bus.play(src, 0.5f);
			if (!childHandle)
				return false;
			s.setPanAbsolute(busHandle, 1.0f, 1.0f);
			s.setPanAbsolute(childHandle, 1.0f, 1.0f);
			// Let the bus' own anti-click startup ramp settle before the
			// automation begins.
			mixFrames(s, 2048);
			mGain = 1.0;
			return true;
		}
		// Start a bus fade and keep track of the gain at the end of every
		// rendered block.
		void fadeBus(double aTo, double aDurationFrames)
		{
			s.fadeVolume(busHandle, (float)aTo, aDurationFrames / kRate);
			mGain = 1.0;
			mFrom = 1.0;
			mTo = aTo;
			mDuration = aDurationFrames;
			mFrame = 0;
		}
		void advance(unsigned int aFrames)
		{
			mFrame += aFrames;
			mGain = fadeAt(mFrom, mTo, mDuration, (double)mFrame);
		}
		double mFrom, mTo, mDuration;
		unsigned int mFrame;
	};

	int case_bus_and_global_stack()
	{
		Blocks whole;
		whole.push_back(128);
		Blocks chopped;
		for (int i = 0; i < 8; i++)
			chopped.push_back(16);

		std::vector<float> a;
		std::vector<float> b;
		double busTo = 0.0;
		double globalFrom = 1.0, globalTo = 0.2;
		{
			BusRig rig;
			TASK_CHECK(rig.init());
			rig.fadeBus(busTo, 64.0);
			rig.s.fadeGlobalVolume((float)globalTo, 128.0 / kRate);
			TASK_CHECK(fabs(rig.s.getVolume(rig.busHandle) - 1.0) <= 1e-5);
			a = renderFloat(rig.s, whole);
			rig.advance(128);
		}
		{
			BusRig rig;
			TASK_CHECK(rig.init());
			rig.fadeBus(busTo, 64.0);
			rig.s.fadeGlobalVolume((float)globalTo, 128.0 / kRate);
			b = renderFloat(rig.s, chopped);
			rig.advance(128);
		}
		TASK_CHECK(a.size() == 256 && b.size() == 256);
		TASK_CHECK(maxAbsDiff(a, b) <= 1e-5f);
		for (unsigned int frame = 0; frame < 128; frame++)
		{
			// Bus gain is applied on the same output frame as the main volume;
			// a 512-frame prefetch would make the bus gain reach zero or start
			// late and fail these per-frame values.
			double busGain = fadeAt(1.0, busTo, 64.0, frame + 1.0);
			double globalGain = fadeAt(globalFrom, globalTo, 128.0, frame + 1.0);
			float expected = (float)(kAmp * 0.5 * busGain * globalGain);
			TASK_CHECK(fabs(a[frame * 2] - expected) <= 1e-5f);
		}
		return 0;
	}

	int case_bus_child_volume_fixed()
	{
		BusRig rig;
		TASK_CHECK(rig.init());
		rig.s.fadeVolume(rig.busHandle, 0.25f, 64.0 / kRate);
		// Parent automation goes to the bus; children keep their fixed volume.
		TASK_CHECK(fabs(rig.s.getVolume(rig.busHandle) - 1.0f) <= 1e-6f);
		TASK_CHECK(fabs(rig.s.getVolume(rig.childHandle) - 0.5f) <= 1e-6f);
		std::vector<float> out = renderFloat(rig.s, Blocks(1, 32));
		TASK_CHECK(out.size() == 64);
		for (unsigned int frame = 0; frame < 32; frame++)
		{
			float expected = (float)(kAmp * 0.5 * fadeAt(1.0, 0.25, 64.0, frame + 1.0));
			TASK_CHECK(fabs(out[frame * 2] - expected) <= 1e-5f);
		}
		TASK_CHECK(fabs(rig.s.getVolume(rig.childHandle) - 0.5f) <= 1e-6f);
		return 0;
	}

	int case_bus_pause_freezes_automation()
	{
		BusRig rig;
		TASK_CHECK(rig.init());
		rig.fadeBus(0.0, 100.0);
		mixFrames(rig.s, 10);
		rig.advance(10);
		double frozen = fadeAt(1.0, 0.0, 100.0, 10.0);
		TASK_CHECK(fabs(rig.s.getVolume(rig.busHandle) - frozen) <= 1e-5);

		rig.s.setPause(rig.busHandle, true);
		mixFrames(rig.s, 40);
		TASK_CHECK(fabs(rig.s.getVolume(rig.busHandle) - frozen) <= 1e-5);
		rig.s.setPause(rig.busHandle, false);

		std::vector<float> out = renderFloat(rig.s, Blocks(1, 8));
		TASK_CHECK(out.size() == 16);
		for (unsigned int frame = 0; frame < 8; frame++)
		{
			float expected = (float)(kAmp * 0.5 * fadeAt(1.0, 0.0, 100.0, 11.0 + frame));
			TASK_CHECK(fabs(out[frame * 2] - expected) <= 1e-5f);
		}
		return 0;
	}

	int case_mono_output_fade()
	{
		ConstSource src(kAmp);
		SoLoud::Soloud s;
		TASK_CHECK(initEngine(s, 1));
		SoLoud::handle h = s.play(src, 1.0f);
		s.setPanAbsolute(h, 1.0f, 1.0f);
		mixFrames(s, 256);
		s.fadeVolume(h, 0.0f, 64.0 / kRate);
		std::vector<float> out = renderFloat(s, Blocks(1, 96));
		TASK_CHECK(out.size() == 96);
		for (unsigned int frame = 0; frame < 96; frame++)
		{
			float expected = (float)(kAmp * fadeAt(1.0, 0.0, 64.0, frame + 1.0));
			TASK_CHECK(fabs(out[frame] - expected) <= 1e-5f);
		}
		return 0;
	}

	int case_stereo_source_fade()
	{
		ConstSource src(0.2f, 0.1f, 2);
		SoLoud::Soloud s;
		TASK_CHECK(initEngine(s, 2));
		SoLoud::handle h = s.play(src, 1.0f);
		s.setPanAbsolute(h, 1.0f, 1.0f);
		mixFrames(s, 256);
		s.fadeVolume(h, 0.0f, 64.0 / kRate);
		std::vector<float> out = renderFloat(s, Blocks(1, 96));
		TASK_CHECK(out.size() == 192);
		for (unsigned int frame = 0; frame < 96; frame++)
		{
			double gain = fadeAt(1.0, 0.0, 64.0, frame + 1.0);
			TASK_CHECK(fabs(out[frame * 2] - (float)(0.2 * gain)) <= 1e-5f);
			TASK_CHECK(fabs(out[frame * 2 + 1] - (float)(0.1 * gain)) <= 1e-5f);
		}
		return 0;
	}

	int case_oscillate_bounds_order()
	{
		ConstSource src(kAmp);
		SoLoud::Soloud s;
		TASK_CHECK(initEngine(s, 2));
		SoLoud::handle h = s.play(src, 1.0f);
		s.setPanAbsolute(h, 1.0f, 1.0f);
		mixFrames(s, 256);
		// Bounds passed high-then-low must behave as lo<=hi.
		s.oscillateVolume(h, 1.0f, 0.2f, 64.0 / kRate);
		TASK_CHECK(fabs(s.getVolume(h) - 0.6f) <= 1e-6f);
		std::vector<float> out = renderFloat(s, Blocks(1, 64));
		TASK_CHECK(out.size() == 128);
		for (unsigned int frame = 0; frame < 64; frame++)
		{
			float expected = (float)(kAmp * lfoAt(0.2, 1.0, 64.0, frame + 1.0));
			TASK_CHECK(fabs(out[frame * 2] - expected) <= 1e-5f);
		}
		return 0;
	}

}

const struct task_case task_cases[] = {
	{ "envelope", "voice_fade_short_endpoint", case_voice_fade_short_endpoint },
	{ "envelope", "global_fade_short_endpoint", case_global_fade_short_endpoint },
	{ "envelope", "fade_arbitrary_blocks", case_fade_arbitrary_blocks },
	{ "envelope", "osc_cross_block_peak", case_osc_cross_block_peak },
	{ "envelope", "global_osc_cross_block_peak", case_global_osc_cross_block_peak },
	{ "envelope", "fade_restart_continuity", case_fade_restart_continuity },
	{ "envelope", "osc_redirect_before_output", case_osc_redirect_before_output },
	{ "envelope", "nonpositive_duration", case_nonpositive_duration },
	{ "envelope", "fixed_volume_control", case_fixed_volume_control },
	{ "envelope", "short_output_format", case_short_output_format },

	{ "clock", "voice_frame_time_getter", case_voice_frame_time_getter },
	{ "clock", "global_frame_time_getter", case_global_frame_time_getter },
	{ "clock", "pause_resume_voice", case_pause_resume_voice },
	{ "clock", "global_clock_advances_with_output", case_global_clock_advances_with_output },
	{ "clock", "last_frame_getter_large_block", case_last_frame_getter_large_block },

	{ "routing", "bus_and_global_stack", case_bus_and_global_stack },
	{ "routing", "bus_child_volume_fixed", case_bus_child_volume_fixed },
	{ "routing", "bus_pause_freezes_automation", case_bus_pause_freezes_automation },
	{ "routing", "mono_output_fade", case_mono_output_fade },
	{ "routing", "stereo_source_fade", case_stereo_source_fade },
	{ "routing", "oscillate_bounds_order", case_oscillate_bounds_order },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
