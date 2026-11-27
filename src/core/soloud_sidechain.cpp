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

#include <math.h>
#include <string.h>
#include <climits>
#include <cmath>
#include "soloud_internal.h"

// Sample-accurate bus sidechain compression.
//
// Each bus memoizes the SAMPLE_GRANULARITY blocks it produces in a two slot
// ping-pong cache. A block holds the bus' PCM after its child voices have been
// mixed, its own filters have run and its own sidechain processing has been
// applied, but before the parent bus applies the bus' volume and pan. The
// audible route and every listening bus read the very same cached block, so
// the detector's sources advance exactly once per audio block regardless of
// how the output is split into mix() calls.

namespace SoLoud
{
	BusInstance *Soloud::busFromHandle_internal(handle aBusHandle)
	{
		// Voice group handles are never a single bus voice.
		if (aBusHandle == 0 || (aBusHandle & 0xfffff000) == 0xfffff000)
			return 0;
		int voiceno = getVoiceFromHandle_internal(aBusHandle);
		if (voiceno < 0)
			return 0;
		AudioSourceInstance *voice = mVoice[voiceno];
		if (voice == 0 || !voice->isBusInstance())
			return 0;
		return (BusInstance *)voice;
	}

	void Soloud::resetSidechainPlans_internal()
	{
		for (unsigned int i = 0; i < VOICE_COUNT; i++)
		{
			mBusDucker[i].mActive = false;
			mBusDucker[i].mTarget = 0;
			mBusDucker[i].mDetector = 0;
			mBusDucker[i].mLogGain = 0;
		}
		mMixStarted = false;
		mSidechainActive = false;
	}

	void Soloud::prepareSidechainCaches_internal()
	{
		for (unsigned int v = 0; v < mHighestVoice; v++)
		{
			if (mVoice[v] && mVoice[v]->isBusInstance())
			{
				BusInstance *bus = (BusInstance *)mVoice[v];
				for (int slot = 0; slot < 2; slot++)
				{
					if (bus->mSidechainCache[slot].mData == 0)
						bus->mSidechainCache[slot].init(SAMPLE_GRANULARITY * MAX_CHANNELS);
					bus->mSidechainCacheBlock[slot] = UINT_MAX;
				}
				bus->mSidechainBlockCounter = 0;
				bus->mSidechainReadOffset = 0;
			}
		}
	}

	bool Soloud::sidechainGraphAcyclic_internal(handle aTarget, handle aDetector)
	{
		// Evaluation dependencies between live voices:
		//  * containment: a parent bus must mix each of its child voices, so the
		//    parent depends on the child (edge parent slot -> child slot);
		//  * sidechain: a target depends on its detector (edge target -> detector).
		// The candidate edge is aTarget -> aDetector. It closes a cycle exactly
		// when the detector can already reach the target through the existing
		// edges, so the search starts at the detector and looks for the target.
		int targetVoice = getVoiceFromHandle_internal(aTarget);
		int detectorVoice = getVoiceFromHandle_internal(aDetector);
		if (targetVoice < 0 || detectorVoice < 0)
			return false;

		unsigned char seen[VOICE_COUNT];
		memset(seen, 0, sizeof(seen));
		int stack[VOICE_COUNT];
		unsigned int top = 0;
		stack[top++] = detectorVoice;
		seen[detectorVoice] = 1;

		while (top > 0)
		{
			int node = stack[--top];
			if (node == targetVoice)
				return false;

			AudioSourceInstance *voice = mVoice[node];
			if (voice == 0)
				continue;

			// Containment edges: every voice routed onto this bus must be mixed first.
			handle nodeHandle = getHandleFromVoice_internal(node);
			for (int i = 0; i < (int)mHighestVoice; i++)
			{
				if (mVoice[i] && mVoice[i]->mBusHandle == nodeHandle && !seen[i])
				{
					seen[i] = 1;
					stack[top++] = i;
				}
			}

			// Sidechain edges: if this bus is itself a ducker target, it depends
			// on its detector. Match by handle so a reused slot can't inherit a
			// plan belonging to an earlier play instance.
			if (voice->isBusInstance() && mBusDucker[node].mActive &&
				mBusDucker[node].mTarget == nodeHandle)
			{
				int dv = getVoiceFromHandle_internal(mBusDucker[node].mDetector);
				if (dv >= 0 && !seen[dv])
				{
					seen[dv] = 1;
					stack[top++] = dv;
				}
			}
		}
		return true;
	}

	result Soloud::setBusDucker(handle aTarget, handle aDetector, float aThreshold,
		float aRatio, float aAttackSeconds, float aReleaseSeconds)
	{
		if (aTarget == 0 || aDetector == 0)
			return INVALID_PARAMETER;
		if ((aTarget & 0xfffff000) == 0xfffff000 ||
			(aDetector & 0xfffff000) == 0xfffff000)
			return INVALID_PARAMETER;
		if (!std::isfinite(aThreshold) || !(aThreshold > 0.0f))
			return INVALID_PARAMETER;
		if (!std::isfinite(aRatio) || !(aRatio >= 1.0f))
			return INVALID_PARAMETER;
		if (!std::isfinite(aAttackSeconds) || aAttackSeconds < 0.0f)
			return INVALID_PARAMETER;
		if (!std::isfinite(aReleaseSeconds) || aReleaseSeconds < 0.0f)
			return INVALID_PARAMETER;

		lockAudioMutex_internal();
		result res = INVALID_PARAMETER;

		if (mMixStarted)
		{
			unlockAudioMutex_internal();
			return INVALID_PARAMETER;
		}

		int targetVoice = getVoiceFromHandle_internal(aTarget);
		int detectorVoice = getVoiceFromHandle_internal(aDetector);
		if (targetVoice >= 0 && detectorVoice >= 0 &&
			mVoice[targetVoice]->isBusInstance() &&
			mVoice[detectorVoice]->isBusInstance() &&
			targetVoice != detectorVoice &&
			sidechainGraphAcyclic_internal(aTarget, aDetector))
		{
			// All caching needed by the audio path is prepared up front.
			prepareSidechainCaches_internal();
			BusDucker &ducker = mBusDucker[targetVoice];
			ducker.mActive = true;
			ducker.mTarget = aTarget;
			ducker.mDetector = aDetector;
			ducker.mThreshold = aThreshold;
			ducker.mRatio = aRatio;
			// exp(-1 / (sampleRate * time)); a zero time means an instant jump.
			ducker.mAttackCoeff = aAttackSeconds == 0.0f ? 0.0f :
				(float)exp(-1.0 / ((double)mSamplerate * aAttackSeconds));
			ducker.mReleaseCoeff = aReleaseSeconds == 0.0f ? 0.0f :
				(float)exp(-1.0 / ((double)mSamplerate * aReleaseSeconds));
			// A (re)configured connection starts at unity gain.
			ducker.mLogGain = 0.0f;
			mSidechainActive = true;
			res = SO_NO_ERROR;
		}

		unlockAudioMutex_internal();
		return res;
	}

	void Soloud::sidechainBlock_internal(BusInstance *aBus, unsigned int aBlockIndex)
	{
		unsigned int slot = aBlockIndex & 1;
		if (aBus->mSidechainCacheBlock[slot] == aBlockIndex)
		{
			// Already produced for this audio block; all readers share the result.
			return;
		}

		unsigned int busHandle = aBus->mParent->mChannelHandle;
		float *destination = aBus->mSidechainCache[slot].mData;

		// Clear the block before accumulating the child voices into it.
		for (unsigned int ch = 0; ch < aBus->mChannels; ch++)
		{
			memset(destination + ch * SAMPLE_GRANULARITY, 0,
				sizeof(float) * SAMPLE_GRANULARITY);
		}

		// Mix child sound sources (and, recursively, child busses) once.
		mixBus_internal(destination, SAMPLE_GRANULARITY, SAMPLE_GRANULARITY,
			aBus->mScratch.mData, busHandle, aBus->mSamplerate,
			aBus->mChannels, aBus->mParent->mResampler);

		// The bus' own filters run before its own sidechain compression.
		for (unsigned int f = 0; f < FILTERS_PER_STREAM; f++)
		{
			if (aBus->mFilter[f])
			{
				aBus->mFilter[f]->filter(destination, SAMPLE_GRANULARITY,
					SAMPLE_GRANULARITY, aBus->mChannels, aBus->mSamplerate,
					mStreamTime);
			}
		}

		// Compression, driven by the detector's matching block. The plan is bound
		// to the live play instance's handle, never just to the voice slot.
		int voiceno = getVoiceFromHandle_internal(busHandle);
		if (voiceno >= 0 && mBusDucker[voiceno].mActive &&
			mBusDucker[voiceno].mTarget == busHandle)
		{
			sidechainEnvelope_internal(voiceno, destination,
				SAMPLE_GRANULARITY, SAMPLE_GRANULARITY, aBlockIndex);
		}

		aBus->mSidechainCacheBlock[slot] = aBlockIndex;
	}

	void Soloud::sidechainEnvelope_internal(unsigned int aVoice, float *aBuffer,
		unsigned int aSamples, unsigned int aStride, unsigned int aBlockIndex)
	{
		BusDucker &ducker = mBusDucker[aVoice];
		BusInstance *detector = busFromHandle_internal(ducker.mDetector);
		float *detectorData = 0;
		unsigned int detectorChannels = 0;
		if (detector)
		{
			// Block indices are global audio time / SAMPLE_GRANULARITY for every
			// bus: all busses advance at speed 1 on block-aligned requests.
			sidechainBlock_internal(detector, aBlockIndex);
			unsigned int detectorSlot = aBlockIndex & 1;
			if (detector->mSidechainCacheBlock[detectorSlot] == aBlockIndex)
			{
				detectorData = detector->mSidechainCache[detectorSlot].mData;
				detectorChannels = detector->mChannels;
			}
		}

		const float exponent = 1.0f - 1.0f / ducker.mRatio;
		float logGain = ducker.mLogGain;
		AudioSourceInstance *target = mVoice[aVoice];

		for (unsigned int i = 0; i < aSamples; i++)
		{
			// A is the largest absolute sample over all detector channels.
			float peak = 0.0f;
			if (detectorData)
			{
				for (unsigned int ch = 0; ch < detectorChannels; ch++)
				{
					float v = fabsf(detectorData[ch * aStride + i]);
					if (v > peak)
						peak = v;
				}
			}

			float targetLogGain;
			if (peak <= ducker.mThreshold)
			{
				targetLogGain = 0.0f;
			}
			else
			{
				targetLogGain = (float)log((double)ducker.mThreshold / (double)peak) * exponent;
			}

			// Gain going down uses the attack time, going up the release time.
			float coefficient = (targetLogGain < logGain) ?
				ducker.mAttackCoeff : ducker.mReleaseCoeff;
			logGain = coefficient * logGain + (1.0f - coefficient) * targetLogGain;

			float gain = (float)exp((double)logGain);
			for (unsigned int ch = 0; ch < target->mChannels; ch++)
			{
				aBuffer[ch * aStride + i] *= gain;
			}
		}

		ducker.mLogGain = logGain;
	}
};
