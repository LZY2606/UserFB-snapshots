/*
SoLoud audio engine
Copyright (c) 2013-2020 Jari Komppa

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

   1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.

   2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.

   3. This notice may not be removed or altered from any source
   distribution.
*/

// Optional filter latency compensation (see soloud_latency.h).

#include <new>
#include <string.h>
#include "soloud_internal.h"
#include "soloud_bus.h"
#include "soloud_latency.h"

namespace SoLoud
{
	// Default filter instance introduces no pure delay.
	unsigned int FilterInstance::getLatencyFrames() const
	{
		return 0;
	}

	void AudioSourceInstance::clearLatencyCompensation()
	{
		delete[] mLatencyRing;
		mLatencyRing = 0;
		mLatencyRingPos = 0;
		mLatencyCompensation = 0;
	}

	unsigned int Soloud::voiceFilterLatencyFrames_internal(const AudioSourceInstance *aVoice) const
	{
		unsigned int total = 0;
		for (unsigned int i = 0; i < FILTERS_PER_STREAM; i++)
		{
			if (aVoice->mFilter[i])
			{
				// Serial filters: latencies add.
				total += aVoice->mFilter[i]->getLatencyFrames();
			}
		}
		return total;
	}

	void Soloud::applyLatencyCompensation_internal(AudioSourceInstance *aVoice, float *aBuffer, unsigned int aSamples, unsigned int aBufferSize)
	{
		unsigned int delay = aVoice->mLatencyCompensation;
		float *ring = aVoice->mLatencyRing;
		if (delay == 0 || ring == 0 || aSamples == 0)
			return;

		// The ring is planar: channel c owns the interval [c*delay, (c+1)*delay),
		// so every channel has an independent delay line and the planar buffer
		// stride (aBufferSize) is never mistaken for a frame count.
		const unsigned int channels = aVoice->mChannels;
		const unsigned int startPos = aVoice->mLatencyRingPos;
		unsigned int endPos = startPos;
		for (unsigned int c = 0; c < channels; c++)
		{
			float *line = ring + c * delay;
			float *buf = aBuffer + c * aBufferSize;
			unsigned int pos = startPos;
			for (unsigned int i = 0; i < aSamples; i++)
			{
				float incoming = buf[i];
				buf[i] = line[pos];
				line[pos] = incoming;
				pos++;
				if (pos == delay)
					pos = 0;
			}
			endPos = pos;
		}
		// Every channel advances by the same number of frames.
		aVoice->mLatencyRingPos = endPos;
	}

	void Soloud::clearFilterLatencyCompensation_internal()
	{
		for (unsigned int i = 0; i < mHighestVoice; i++)
		{
			if (mVoice[i])
				mVoice[i]->clearLatencyCompensation();
		}
		mLatencyCompensationReady = false;
		mLatencyCompensationTotal = 0;
	}

	unsigned int Soloud::getFilterLatencyCompensationFrames() const
	{
		return mLatencyCompensationReady ? mLatencyCompensationTotal : 0;
	}

	namespace
	{
		// Per-voice planner bookkeeping.
		struct Planner
		{
			Soloud *mSoloud;
			unsigned char mVisitState[VOICE_COUNT]; // 0 unseen, 1 on stack, 2 done
			unsigned int mJunction[VOICE_COUNT];    // J(b) for bus voices

			Planner(Soloud *aSoloud) : mSoloud(aSoloud)
			{
				for (unsigned int i = 0; i < VOICE_COUNT; i++)
				{
					mVisitState[i] = 0;
					mJunction[i] = 0;
				}
			}

			// Resolve a bus handle to the voice playing that bus, or -1.
			// Handle 0 (root) also returns -1.
			int voiceFromBusHandle(handle aBusHandle) const
			{
				if (aBusHandle == 0)
					return -1;
				for (unsigned int i = 0; i < mSoloud->mHighestVoice; i++)
				{
					if (mSoloud->mVoice[i] &&
						mSoloud->getHandleFromVoice_internal(i) == aBusHandle)
						return (int)i;
				}
				return -1;
			}

			static bool supportedChannels(unsigned int aChannels)
			{
				return aChannels == 1 || aChannels == 2 || aChannels == 6;
			}

			// Validate every live voice against the supported fixed-routing
			// subset. Also proves every mBusHandle points to a live bus.
			bool validate()
			{
				if (mSoloud->mSamplerate != 48000)
					return false;
				if (!supportedChannels(mSoloud->mChannels))
					return false;
				if (mSoloud->mResampler != Soloud::RESAMPLER_POINT)
					return false;

				for (unsigned int i = 0; i < mSoloud->mHighestVoice; i++)
				{
					AudioSourceInstance *voice = mSoloud->mVoice[i];
					if (!voice)
						continue;

					if (!supportedChannels(voice->mChannels))
						return false;
					if (voice->mFlags & AudioSourceInstance::PAUSED)
						return false;
					if (voice->mFlags & AudioSourceInstance::PROCESS_3D)
						return false;
					if (voice->mDelaySamples != 0)
						return false;
					if (voice->mSetRelativePlaySpeed != 1.0f ||
						voice->mOverallRelativePlaySpeed != 1.0f ||
						voice->mSamplerate != 48000.0f)
						return false;

					BusInstance *asBus = dynamic_cast<BusInstance *>(voice);
					if (asBus && asBus->mParent->getResampler() != Soloud::RESAMPLER_POINT)
						return false;

					// Every route must lead to a live bus voice (or the root).
					int parent = voiceFromBusHandle(voice->mBusHandle);
					if (voice->mBusHandle != 0 && parent < 0)
						return false;
					if (parent >= 0 && dynamic_cast<BusInstance *>(mSoloud->mVoice[parent]) == 0)
						return false;
				}
				return true;
			}

			// Compute J(aBusHandle): the longest arrival path, in frames, at
			// the given junction (handle 0 is the root junction). While doing
			// so, assign and allocate every child's compensation delay.
			// Returns INVALID_PARAMETER on a routing cycle, OUT_OF_MEMORY on
			// allocation failure, or SO_NO_ERROR.
			result planJunction(handle aBusHandle, unsigned int &aJunction)
			{
				unsigned int longest = 0;

				// First pass: find the longest path and check the topology.
				for (unsigned int i = 0; i < mSoloud->mHighestVoice; i++)
				{
					AudioSourceInstance *voice = mSoloud->mVoice[i];
					if (!voice || voice->mBusHandle != aBusHandle)
						continue;

					if (mVisitState[i] == 1)
						return INVALID_PARAMETER; // routing cycle
					if (mVisitState[i] == 0)
					{
						mVisitState[i] = 1;
						BusInstance *asBus = dynamic_cast<BusInstance *>(voice);
						if (asBus)
						{
							unsigned int childJunction = 0;
							handle ownHandle = mSoloud->getHandleFromVoice_internal(i);
							result r = planJunction(ownHandle, childJunction);
							if (r != SO_NO_ERROR)
								return r;
							mJunction[i] = childJunction;
						}
						mVisitState[i] = 2;
					}

					unsigned int path = mSoloud->voiceFilterLatencyFrames_internal(voice);
					if (dynamic_cast<BusInstance *>(voice))
						path += mJunction[i];
					if (path > longest)
						longest = path;
				}

				// Second pass: each child is delayed by the junction alignment
				// point minus its own arrival path.
				for (unsigned int i = 0; i < mSoloud->mHighestVoice; i++)
				{
					AudioSourceInstance *voice = mSoloud->mVoice[i];
					if (!voice || voice->mBusHandle != aBusHandle)
						continue;

					unsigned int path = mSoloud->voiceFilterLatencyFrames_internal(voice);
					if (dynamic_cast<BusInstance *>(voice))
						path += mJunction[i];

					unsigned int compensation = longest - path;
					if (compensation == 0)
						continue;

					unsigned int floats = compensation * voice->mChannels;
					float *ring = new (std::nothrow) float[floats];
					if (ring == 0)
						return OUT_OF_MEMORY;
					memset(ring, 0, sizeof(float) * floats);
					voice->mLatencyRing = ring;
					voice->mLatencyRingPos = 0;
					voice->mLatencyCompensation = compensation;
				}

				aJunction = longest;
				return SO_NO_ERROR;
			}
		};
	}

	result Soloud::prepareFilterLatencyCompensation()
	{
		lockAudioMutex_internal();

		// Only one plan, and it must be built before any mixing happened.
		if (mMixStarted || mLatencyCompensationReady)
		{
			unlockAudioMutex_internal();
			return INVALID_PARAMETER;
		}

		// Global filters sit behind the root junction and cannot be aligned
		// against anything inside the voice graph.
		for (unsigned int i = 0; i < FILTERS_PER_STREAM; i++)
		{
			if (mFilter[i] || mFilterInstance[i])
			{
				unlockAudioMutex_internal();
				return INVALID_PARAMETER;
			}
		}

		Planner planner(this);
		if (!planner.validate())
		{
			unlockAudioMutex_internal();
			return INVALID_PARAMETER;
		}

		unsigned int rootJunction = 0;
		result outcome = planner.planJunction(0, rootJunction);
		if (outcome != SO_NO_ERROR)
		{
			// Never leave a half-built plan behind.
			clearFilterLatencyCompensation_internal();
			unlockAudioMutex_internal();
			return outcome;
		}
		mLatencyCompensationTotal = rootJunction;
		mLatencyCompensationReady = true;

		unlockAudioMutex_internal();
		return SO_NO_ERROR;
	}
};
