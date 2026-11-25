// latency_render - end-to-end demo/verification of filter latency compensation.
//
// Builds a fixed stereo graph entirely on the NULLDRIVER:
//
//   source A -> delay 40 -> delay 60 -\
//                                      +-> Bus merge -> delay 50 -> root
//   source B (bypass) -----------------/
//
// A has 100 frames of serial filter latency, B has none. At the Bus merge B
// receives a 100 frame compensation delay; both branches then pass the Bus
// filter (50), so the reported root latency is 150.
//
// The program renders real mixed PCM through Soloud::mix(), writes raw
// interleaved 32-bit floats to the path given as its only argument, reads the
// file back and checks pulse positions/amplitudes independently. Exits
// non-zero on any failure. No audio device is opened.

#include "soloud.h"
#include "soloud_bus.h"
#include "soloud_filter.h"
#include "soloud_latency.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

namespace
{
	constexpr unsigned int SR = 48000;
	constexpr unsigned int CH = 2;
	constexpr unsigned int WARM = 256;
	constexpr unsigned int FRAMES = 8192;

	constexpr unsigned int DA1 = 40;
	constexpr unsigned int DA2 = 60;
	constexpr unsigned int DBUS = 50;
	constexpr unsigned int ROOT_LATENCY = DA1 + DA2 + DBUS; // 150

	constexpr unsigned int TAG_A = 2000;
	constexpr unsigned int TAG_B = 2400;
	constexpr unsigned int ALIGN = 3000;

	class DelayInstance : public SoLoud::FilterInstance
	{
	public:
		unsigned int mDelay;
		unsigned int mPos = 0;
		std::vector<float> mRing;

		explicit DelayInstance(unsigned int d)
		    : mDelay(d), mRing((size_t)(d ? d : 1) * CH, 0.0f)
		{
		}

		unsigned int getLatencyFrames() const override { return mDelay; }

		void filter(float *buf, unsigned int n, unsigned int stride,
		            unsigned int channels, float, SoLoud::time) override
		{
			if (!mDelay)
				return;
			for (unsigned int i = 0; i < n; ++i)
			{
				for (unsigned int c = 0; c < channels; ++c)
				{
					float &slot = mRing[(size_t)c * mDelay + mPos];
					float x = buf[(size_t)c * stride + i];
					buf[(size_t)c * stride + i] = slot;
					slot = x;
				}
				++mPos;
				if (mPos == mDelay)
					mPos = 0;
			}
		}
	};

	class DelayFilter : public SoLoud::Filter
	{
	public:
		unsigned int mDelay;
		explicit DelayFilter(unsigned int d) : mDelay(d) {}
		SoLoud::FilterInstance *createInstance() override
		{
			return new DelayInstance(mDelay);
		}
	};

	class PulseInstance : public SoLoud::AudioSourceInstance
	{
	public:
		unsigned int mPos = 0;
		unsigned int mTagBase = TAG_A;
		unsigned getAudio(float *buf, unsigned int n, unsigned int stride) override
		{
			for (unsigned int c = 0; c < mChannels; ++c)
			{
				for (unsigned int i = 0; i < n; ++i)
				{
					unsigned int f = mPos + i;
					float v = 0.0f;
					// Tag pulse (per-channel offset) and shared align pulse.
					if (f == mTagBase + c * 3)
						v = 0.2f * (c + 1);
					if (f == ALIGN)
						v += 0.05f;
					buf[(size_t)c * stride + i] = v;
				}
			}
			mPos += n;
			return n;
		}
		bool hasEnded() override { return false; }
	};

	class Pulse : public SoLoud::AudioSource
	{
	public:
		unsigned int mTagBase;
		explicit Pulse(unsigned int tagBase) : mTagBase(tagBase)
		{
			mChannels = CH;
			mBaseSamplerate = (float)SR;
		}
		SoLoud::AudioSourceInstance *createInstance() override
		{
			PulseInstance *p = new PulseInstance();
			p->mTagBase = mTagBase;
			return p;
		}
	};

	bool closeTo(float got, float want, float eps = 1e-5f)
	{
		return std::fabs(got - want) <= eps;
	}

	float at(const std::vector<float> &pcm, unsigned frame, unsigned ch)
	{
		return pcm[(size_t)frame * CH + ch];
	}
}

int main(int argc, char **argv)
{
	if (argc != 2)
	{
		std::fprintf(stderr, "usage: %s <output.pcm>\n", argv[0]);
		return 2;
	}
	const char *outPath = argv[1];

	SoLoud::Soloud sol;
	Pulse a(TAG_A), b(TAG_B);
	DelayFilter d1(DA1), d2(DA2), dbus(DBUS);
	SoLoud::Bus bus;

	if (sol.init(0, SoLoud::Soloud::NULLDRIVER, SR, 1024, CH) !=
	    SoLoud::SO_NO_ERROR)
	{
		std::fprintf(stderr, "init failed\n");
		return 1;
	}
	sol.setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);
	sol.setPostClipScaler(1.0f);

	bus.setChannels(CH);
	bus.mBaseSamplerate = (float)SR;
	bus.setResampler(SoLoud::Soloud::RESAMPLER_POINT);
	bus.setFilter(0, &dbus);
	SoLoud::handle bh = sol.play(bus);
	sol.setPanAbsolute(bh, 1.0f, 1.0f);

	a.setFilter(0, &d1);
	a.setFilter(1, &d2);
	SoLoud::handle ha = bus.play(a);
	sol.setPanAbsolute(ha, 1.0f, 1.0f);
	sol.setAutoStop(ha, false);

	SoLoud::handle hb = bus.play(b); // bypass branch
	sol.setPanAbsolute(hb, 1.0f, 1.0f);
	sol.setAutoStop(hb, false);

	if (sol.prepareFilterLatencyCompensation() != SoLoud::SO_NO_ERROR)
	{
		std::fprintf(stderr, "prepare failed\n");
		return 1;
	}
	if (sol.getFilterLatencyCompensationFrames() != ROOT_LATENCY)
	{
		std::fprintf(stderr, "root latency %u != expected %u\n",
		             sol.getFilterLatencyCompensationFrames(), ROOT_LATENCY);
		return 1;
	}

	// Warm-up mix (settles pan smoothing), discarded from the file.
	std::vector<float> warm((size_t)WARM * CH, 0.0f);
	sol.mix(warm.data(), WARM);

	std::vector<float> pcm((size_t)FRAMES * CH, 0.0f);
	sol.mix(pcm.data(), FRAMES);
	sol.deinit();

	// Write raw interleaved float32 PCM.
	{
		std::ofstream f(outPath, std::ios::binary | std::ios::trunc);
		if (!f)
		{
			std::fprintf(stderr, "cannot open %s for write\n", outPath);
			return 1;
		}
		f.write(reinterpret_cast<const char *>(pcm.data()),
		        (std::streamsize)(pcm.size() * sizeof(float)));
		if (!f)
		{
			std::fprintf(stderr, "write failed\n");
			return 1;
		}
	}

	// Read it back from disk and independently verify pulse placement.
	std::ifstream in(outPath, std::ios::binary);
	if (!in)
	{
		std::fprintf(stderr, "cannot reopen %s\n", outPath);
		return 1;
	}
	in.seekg(0, std::ios::end);
	std::streamoff bytes = in.tellg();
	in.seekg(0, std::ios::beg);
	if (bytes != (std::streamoff)(pcm.size() * sizeof(float)))
	{
		std::fprintf(stderr, "file size %lld unexpected\n", (long long)bytes);
		return 1;
	}
	std::vector<float> disk(pcm.size());
	in.read(reinterpret_cast<char *>(disk.data()), bytes);
	if (!in)
	{
		std::fprintf(stderr, "read failed\n");
		return 1;
	}

	int failures = 0;
	auto check = [&](bool ok, const char *what, unsigned frame, unsigned ch) {
		if (!ok)
		{
			std::fprintf(stderr, "FAIL %s frame=%u ch=%u got=%g\n", what, frame,
			             ch, at(disk, frame, ch));
			++failures;
		}
	};

	for (unsigned c = 0; c < CH; ++c)
	{
		unsigned fa = TAG_A + c * 3 + ROOT_LATENCY - WARM;
		unsigned fb = TAG_B + c * 3 + ROOT_LATENCY - WARM;
		unsigned fm = ALIGN + ROOT_LATENCY - WARM;
		float tagAmp = 0.2f * (c + 1);
		float tagBamp = 0.2f * (c + 1);

		check(fa < FRAMES && closeTo(at(disk, fa, c), tagAmp), "tagA", fa, c);
		check(fb < FRAMES && closeTo(at(disk, fb, c), tagBamp), "tagB", fb, c);
		// At the align frame A and B both contribute 0.05 per channel;
		// their tag amplitudes are not present at that source frame.
		check(fm < FRAMES && closeTo(at(disk, fm, c), 0.10f), "align", fm, c);
		// Single-sample pulse shape.
		check(fa > 0 && std::fabs(at(disk, fa - 1, c)) < 1e-5f, "tagA-prev", fa, c);
		check(std::fabs(at(disk, fa + 1, c)) < 1e-5f, "tagA-next", fa, c);
	}

	if (failures)
	{
		std::fprintf(stderr, "latency_render: %d verification failures\n", failures);
		return 1;
	}
	std::printf("latency_render OK: %u frames, root latency %u, %s\n", FRAMES,
	            ROOT_LATENCY, outPath);
	return 0;
}
