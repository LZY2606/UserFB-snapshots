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
#include "soloud_internal.h"

// Per-sample sidechain ducking for mixing busses.
//
// Configuration (setBusDucker) is built before the first mix; the audio
// thread only walks the preallocated links and per-bus block caches. A
// detector bus publishes every 512-frame block it produces (after its child
// mix, its own filters and its own sidechain processing, but before its
// parent applies its volume/pan) into a sequence-numbered ring cache; the
// target busses read the matching block when they produce theirs.

namespace SoLoud
{
	// Directed edge between engine voice slots, used for the cycle check and
	// the per-mix topological ordering.
	struct SidechainEdge
	{
		unsigned int mFrom;
		unsigned int mTo;
	};

	static unsigned int nextPow2(unsigned int aValue)
	{
		unsigned int v = 2;
		while (v < aValue)
			v <<= 1;
		return v;
	}

	// Walk the containment chain (mBusHandle) up to the voice mixed directly
	// by the root bus. Returns -1 if a link in the chain has disappeared.
	static int rootAncestorVoice(Soloud *aSoloud, int aVoice)
	{
		int v = aVoice;
		for (;;)
		{
			if (v == -1 || !aSoloud->mVoice[v])
				return -1;
			unsigned int parentHandle = aSoloud->mVoice[v]->mBusHandle;
			if (parentHandle == 0)
				return v;
			v = aSoloud->getVoiceFromHandle_internal(parentHandle);
		}
	}

	void Soloud::clearSidechain_internal()
	{
		unsigned int i;
		for (i = 0; i < mHighestVoice; i++)
		{
			if (mVoice[i])
			{
				mVoice[i]->mDucker = 0;
				mVoice[i]->mSidechainDetector = false;
				BusInstance *bus = 0;
				if (mVoice[i]->isBus())
					bus = (BusInstance *)mVoice[i];
				if (bus)
				{
					bus->mSidechainCache = 0;
					bus->mSidechainCacheSize = 0;
					bus->mSidechainSeq = 0;
				}
			}
		}
		for (i = 0; i < mSidechainCacheCount; i++)
			delete[] mSidechainCaches[i];
		delete[] mSidechainCaches;
		delete[] mSidechainCacheHandles;
		mSidechainCaches = 0;
		mSidechainCacheHandles = 0;
		mSidechainCacheCount = 0;
		delete[] mSidechainOrderScratch;
		mSidechainOrderScratch = 0;
		for (i = 0; i < mDuckerCount; i++)
			delete mDuckers[i];
		delete[] mDuckers;
		delete[] mDuckerTargets;
		mDuckers = 0;
		mDuckerTargets = 0;
		mDuckerCount = 0;
	}

	// Cycle check over the containment hierarchy (child bus -> parent bus)
	// together with the tentative sidechain edges (detector -> target).
	// aCandidateTarget/aCandidateDetector name a link that is not committed
	// yet; either may be 0 when revalidating the committed set.
	static bool sidechainHasCycle(Soloud *aSoloud, handle aCandidateTarget, handle aCandidateDetector)
	{
		unsigned int n = aSoloud->mHighestVoice;
		SidechainEdge edges[VOICE_COUNT * 2 + 8];
		unsigned int edgeCount = 0;
		unsigned int i;

		// Containment edges: every voice playing on a bus precedes that bus.
		for (i = 0; i < n; i++)
		{
			AudioSourceInstance *v = aSoloud->mVoice[i];
			if (!v || v->mBusHandle == 0)
				continue;
			int p = aSoloud->getVoiceFromHandle_internal(v->mBusHandle);
			if (p != -1 && (unsigned int)p != i)
			{
				edges[edgeCount].mFrom = i;
				edges[edgeCount].mTo = p;
				edgeCount++;
			}
		}

		// Committed sidechain edges.
		for (i = 0; i < aSoloud->mDuckerCount; i++)
		{
			if (aSoloud->mDuckerTargets[i] == aCandidateTarget)
				continue; // this one gets replaced by the candidate
			int d = aSoloud->getVoiceFromHandle_internal(aSoloud->mDuckers[i]->mDetectorHandle);
			int t = aSoloud->getVoiceFromHandle_internal(aSoloud->mDuckerTargets[i]);
			if (d != -1 && t != -1)
			{
				edges[edgeCount].mFrom = d;
				edges[edgeCount].mTo = t;
				edgeCount++;
			}
		}

		// Tentative sidechain edge.
		if (aCandidateTarget && aCandidateDetector)
		{
			int d = aSoloud->getVoiceFromHandle_internal(aCandidateDetector);
			int t = aSoloud->getVoiceFromHandle_internal(aCandidateTarget);
			if (d != -1 && t != -1)
			{
				edges[edgeCount].mFrom = d;
				edges[edgeCount].mTo = t;
				edgeCount++;
			}
		}

		// Iterative DFS with three colors (0=white, 1=gray, 2=black). Each
		// stack frame remembers the next edge to inspect.
		unsigned char color[VOICE_COUNT];
		memset(color, 0, sizeof(color));
		unsigned int stack[VOICE_COUNT];
		unsigned int cursor[VOICE_COUNT];
		for (i = 0; i < n; i++)
		{
			if (color[i] != 0)
				continue;
			unsigned int sp = 0;
			stack[sp] = i;
			cursor[sp] = 0;
			color[i] = 1;
			sp++;
			while (sp)
			{
				unsigned int v = stack[sp - 1];
				unsigned int e = cursor[sp - 1];
				while (e < edgeCount && edges[e].mFrom != v)
					e++;
				if (e == edgeCount)
				{
					color[v] = 2;
					sp--;
					continue;
				}
				cursor[sp - 1] = e + 1;
				unsigned int to = edges[e].mTo;
				if (color[to] == 1)
					return true;
				if (color[to] == 0)
				{
					color[to] = 1;
					stack[sp] = to;
					cursor[sp] = 0;
					sp++;
				}
			}
		}
		return false;
	}

	result Soloud::setBusDucker(handle aTarget, handle aDetector, float aThreshold, float aRatio, float aAttackSeconds, float aReleaseSeconds)
	{
		if (!isfinite(aThreshold) || aThreshold <= 0.0f)
			return INVALID_PARAMETER;
		if (!isfinite(aRatio) || aRatio < 1.0f)
			return INVALID_PARAMETER;
		if (!isfinite(aAttackSeconds) || aAttackSeconds < 0.0f)
			return INVALID_PARAMETER;
		if (!isfinite(aReleaseSeconds) || aReleaseSeconds < 0.0f)
			return INVALID_PARAMETER;
		// Voice group handles name a group, never a single bus voice.
		if ((aTarget & 0xfffff000) == 0xfffff000 ||
			(aDetector & 0xfffff000) == 0xfffff000)
			return INVALID_PARAMETER;

		lockAudioMutex_internal();

		// The offline scenario is configured before the first mix; recheck
		// under the lock so a concurrent mix cannot race the configuration.
		if (mMixStarted)
		{
			unlockAudioMutex_internal();
			return INVALID_PARAMETER;
		}

		result res = INVALID_PARAMETER;
		int tv = getVoiceFromHandle_internal(aTarget);
		int dv = getVoiceFromHandle_internal(aDetector);
		if (tv != -1 && dv != -1 &&
			mVoice[tv]->isBus() && mVoice[dv]->isBus() &&
			aTarget != aDetector &&
			!sidechainHasCycle(this, aTarget, aDetector))
		{
			res = SO_NO_ERROR;
		}

		if (res == SO_NO_ERROR)
		{
			// Replace an existing link for this target, otherwise append.
			unsigned int slot = mDuckerCount;
			unsigned int i;
			for (i = 0; i < mDuckerCount; i++)
			{
				if (mDuckerTargets[i] == aTarget)
				{
					slot = i;
					break;
				}
			}

			AudioSourceInstance::Ducker *d = new AudioSourceInstance::Ducker;
			d->mDetectorHandle = aDetector;
			d->mThreshold = aThreshold;
			d->mRatio = aRatio;
			d->mAttackCoeff = aAttackSeconds <= 0.0f ? 0.0f : (float)exp(-1.0 / ((double)mSamplerate * aAttackSeconds));
			d->mReleaseCoeff = aReleaseSeconds <= 0.0f ? 0.0f : (float)exp(-1.0 / ((double)mSamplerate * aReleaseSeconds));
			d->mLogGain = 0.0f; // every (re)configuration starts at unity gain

			if (slot == mDuckerCount)
			{
				AudioSourceInstance::Ducker **newDuckers = new AudioSourceInstance::Ducker *[mDuckerCount + 1];
				handle *newTargets = new handle[mDuckerCount + 1];
				for (i = 0; i < mDuckerCount; i++)
				{
					newDuckers[i] = mDuckers[i];
					newTargets[i] = mDuckerTargets[i];
				}
				delete[] mDuckers;
				delete[] mDuckerTargets;
				mDuckers = newDuckers;
				mDuckerTargets = newTargets;
				mDuckerCount++;
			}
			else
			{
				delete mDuckers[slot];
			}
			mDuckers[slot] = d;
			mDuckerTargets[slot] = aTarget;

			setupSidechainCaches_internal();

			// Prepare the topological-order scratch once; mixing only reads it.
			if (!mSidechainOrderScratch)
			{
				mSidechainOrderScratch = new unsigned int[VOICE_COUNT * 6];
			}
		}

		unlockAudioMutex_internal();
		return res;
	}

	void Soloud::setupSidechainCaches_internal()
	{
		unsigned int i, j;

		// Detach everything the previous setup attached to live voices.
		for (i = 0; i < mHighestVoice; i++)
		{
			if (!mVoice[i])
				continue;
			mVoice[i]->mDucker = 0;
			mVoice[i]->mSidechainDetector = false;
			if (mVoice[i]->isBus())
			{
				BusInstance *bus = (BusInstance *)mVoice[i];
				bus->mSidechainCache = 0;
				bus->mSidechainCacheSize = 0;
				bus->mSidechainSeq = 0;
			}
		}
		for (i = 0; i < mSidechainCacheCount; i++)
			delete[] mSidechainCaches[i];
		delete[] mSidechainCaches;
		delete[] mSidechainCacheHandles;
		mSidechainCaches = 0;
		mSidechainCacheHandles = 0;
		mSidechainCacheCount = 0;

		if (mDuckerCount == 0)
			return;

		// One ring cache per distinct detector bus handle. The ring has to
		// hold every block a detector can get ahead of a target inside one mix
		// quantum (at most bufferSize/512 blocks), plus a safety margin.
		unsigned int blocksPerQuantum = (mBufferSize + SAMPLE_GRANULARITY - 1) / SAMPLE_GRANULARITY;
		unsigned int ringSize = nextPow2(mMaxActiveVoices > blocksPerQuantum + 2 ? mMaxActiveVoices : blocksPerQuantum + 2);
		mSidechainCaches = new BusInstance::SidechainBlock *[mDuckerCount];
		mSidechainCacheHandles = new handle[mDuckerCount];
		for (i = 0; i < mDuckerCount; i++)
		{
			handle detector = mDuckers[i]->mDetectorHandle;
			bool seen = false;
			for (j = 0; j < mSidechainCacheCount; j++)
			{
				if (mSidechainCacheHandles[j] == detector)
				{
					seen = true;
					break;
				}
			}
			if (seen)
				continue;

			int dv = getVoiceFromHandle_internal(detector);
			if (dv == -1 || !mVoice[dv]->isBus())
				continue; // detector stopped before mixing: links read silence
			BusInstance *detectorBus = (BusInstance *)mVoice[dv];

			BusInstance::SidechainBlock *ring = new BusInstance::SidechainBlock[ringSize];
			for (j = 0; j < ringSize; j++)
				ring[j].mSeq = 0xffffffff;
			mSidechainCacheHandles[mSidechainCacheCount] = detector;
			mSidechainCaches[mSidechainCacheCount] = ring;
			mSidechainCacheCount++;

			detectorBus->mSidechainCache = ring;
			detectorBus->mSidechainCacheSize = ringSize;
			detectorBus->mSidechainSeq = 0;
			mVoice[dv]->mSidechainDetector = true;
		}

		// Attach the envelope state to each target bus.
		for (i = 0; i < mDuckerCount; i++)
		{
			int tv = getVoiceFromHandle_internal(mDuckerTargets[i]);
			if (tv != -1 && mVoice[tv]->isBus())
				mVoice[tv]->mDucker = mDuckers[i];
		}
	}

	void Soloud::orderSidechainVoices_internal()
	{
		unsigned int n = mActiveVoiceCount;
		if (n < 2 || mDuckerCount == 0 || !mSidechainOrderScratch)
			return;

		// Scratch layout, all prepared at configuration time (6 * VOICE_COUNT):
		//   indegree[0..V), ordered[0..V), edge from[0..2V), edge to[0..2V)
		unsigned int *indegree = mSidechainOrderScratch;
		unsigned int *ordered = mSidechainOrderScratch + VOICE_COUNT;
		unsigned int *edgeFrom = mSidechainOrderScratch + VOICE_COUNT * 2;
		unsigned int *edgeTo = mSidechainOrderScratch + VOICE_COUNT * 4;
		unsigned int edgeCount = 0;
		unsigned int i, j;

		// Containment edges among the active voices.
		for (i = 0; i < n; i++)
		{
			AudioSourceInstance *v = mVoice[mActiveVoice[i]];
			if (!v || v->mBusHandle == 0)
				continue;
			int p = getVoiceFromHandle_internal(v->mBusHandle);
			if (p == -1)
				continue;
			for (j = 0; j < n; j++)
			{
				if (mActiveVoice[j] == (unsigned int)p)
				{
					edgeFrom[edgeCount] = mActiveVoice[i];
					edgeTo[edgeCount] = mActiveVoice[j];
					edgeCount++;
					break;
				}
			}
		}

		// Sidechain edges among the active voices. A nested detector only
		// produces a block when an ancestor bus runs, so the ordering edge is
		// raised to the two root-ancestor busses; containment edges then carry
		// the rest of the ordering inside each subtree.
		for (i = 0; i < mDuckerCount; i++)
		{
			int d = getVoiceFromHandle_internal(mDuckers[i]->mDetectorHandle);
			int t = getVoiceFromHandle_internal(mDuckerTargets[i]);
			if (d == -1 || t == -1)
				continue;
			int dRoot = rootAncestorVoice(this, d);
			int tRoot = rootAncestorVoice(this, t);
			if (dRoot == -1 || tRoot == -1)
				continue;
			bool dActive = false, tActive = false;
			for (j = 0; j < n; j++)
			{
				if (mActiveVoice[j] == (unsigned int)dRoot) dActive = true;
				if (mActiveVoice[j] == (unsigned int)tRoot) tActive = true;
			}
			if (dActive && tActive)
			{
				edgeFrom[edgeCount] = dRoot;
				edgeTo[edgeCount] = tRoot;
				edgeCount++;
			}
		}

		// Kahn's algorithm; ties keep the original active-voice order.
		for (i = 0; i < n; i++)
			indegree[mActiveVoice[i]] = 0;
		for (i = 0; i < edgeCount; i++)
			indegree[edgeTo[i]]++;

		unsigned int outCount = 0;
		unsigned int processed = 0;
		while (processed < n)
		{
			bool progressed = false;
			for (i = 0; i < n; i++)
			{
				unsigned int v = mActiveVoice[i];
				if (indegree[v] != 0 || indegree[v] == 0xffffffff)
					continue;
				indegree[v] = 0xffffffff;
				ordered[outCount++] = v;
				processed++;
				progressed = true;
				for (j = 0; j < edgeCount; j++)
				{
					if (edgeFrom[j] == v)
						indegree[edgeTo[j]]--;
				}
			}
			if (!progressed)
				break; // defensive: configuration was validated as acyclic
		}

		if (outCount == n)
		{
			for (i = 0; i < n; i++)
				mActiveVoice[i] = ordered[i];
		}
	}

	void Soloud::sidechainProcessBlock_internal(BusInstance *aBus, float *aBlock, unsigned int aSamples, unsigned int aStride)
	{
		AudioSourceInstance *self = (AudioSourceInstance *)aBus;
		unsigned int seq = aBus->mSidechainSeq;

		// 1. Drive this bus' own envelope from its detector's cached block.
		AudioSourceInstance::Ducker *ducker = self->mDucker;
		if (ducker)
		{
			const float *detectorPcm = 0;
			unsigned int detectorChannels = 0;
			int dv = getVoiceFromHandle_internal(ducker->mDetectorHandle);
			if (dv != -1 && mVoice[dv]->isBus())
			{
				BusInstance *detector = (BusInstance *)mVoice[dv];
				if (detector->mSidechainCache && detector->mSidechainCacheSize)
				{
					BusInstance::SidechainBlock &slot =
						detector->mSidechainCache[seq & (detector->mSidechainCacheSize - 1)];
					if (slot.mSeq == seq)
					{
						detectorPcm = slot.mPcm;
						detectorChannels = mVoice[dv]->mChannels;
					}
				}
			}

			const float exponent = 1.0f - 1.0f / ducker->mRatio;
			float logGain = ducker->mLogGain;
			unsigned int i, c;
			for (i = 0; i < aSamples; i++)
			{
				float peak = 0.0f;
				if (detectorPcm)
				{
					for (c = 0; c < detectorChannels; c++)
					{
						float v = fabsf(detectorPcm[c * SAMPLE_GRANULARITY + i]);
						if (v > peak)
							peak = v;
					}
				}
				// A missing cache block is treated as silence: peak stays 0,
				// so the target gains unity and the envelope releases.
				float targetGain = 1.0f;
				if (peak > ducker->mThreshold)
					targetGain = powf(ducker->mThreshold / peak, exponent);
				float targetLog = logf(targetGain);
				float coeff = (targetLog < logGain) ? ducker->mAttackCoeff : ducker->mReleaseCoeff;
				logGain = coeff * logGain + (1.0f - coeff) * targetLog;
				float gain = expf(logGain);
				for (c = 0; c < self->mChannels; c++)
					aBlock[c * aStride + i] *= gain;
			}
			ducker->mLogGain = logGain;
		}

		// 2. Publish the processed block for every bus listening to this one.
		//    The sample point is post child mix/filters/sidechain, pre the
		//    parent bus applying this bus' volume and pan.
		if (self->mSidechainDetector && aBus->mSidechainCache && aBus->mSidechainCacheSize)
		{
			BusInstance::SidechainBlock &slot =
				aBus->mSidechainCache[seq & (aBus->mSidechainCacheSize - 1)];
			slot.mSeq = seq;
			unsigned int c;
			for (c = 0; c < self->mChannels && c < MAX_CHANNELS; c++)
			{
				memcpy(slot.mPcm + c * SAMPLE_GRANULARITY,
					aBlock + c * aStride,
					sizeof(float) * aSamples);
			}
		}
		aBus->mSidechainSeq++;
	}
};
