/*
SoLoud audio engine
Copyright (c) 2013-2020 Jari Komppa

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include "soloud_latency.h"

#include <algorithm>
#include <vector>

namespace SoLoud
{
	// -----------------------------------------------------------------------
	// FilterInstance latency default
	// -----------------------------------------------------------------------

	unsigned int FilterInstance::getLatencyFrames() const
	{
		// The default filter instance introduces no pure delay.
		return 0;
	}

	// -----------------------------------------------------------------------
	// LatencyDelayLine
	// -----------------------------------------------------------------------

	LatencyDelayLine::LatencyDelayLine()
		: mDelayFrames(0)
		, mPosition(0)
	{
	}

	void LatencyDelayLine::configure(unsigned int aDelayFrames, unsigned int aChannels)
	{
		mDelayFrames = aDelayFrames;
		mPosition = 0;
		if (aDelayFrames == 0 || aChannels == 0)
		{
			mRing.clear();
			return;
		}
		// Channel-blocked layout: [channel 0: d frames][channel 1: d frames]...
		// Value initialization zeroes the rings (compensation starts at silence).
		const size_t total = (size_t)aDelayFrames * (size_t)aChannels;
		mRing.assign(total, 0.0f);
	}

	void LatencyDelayLine::clear()
	{
		mRing.clear();
		mDelayFrames = 0;
		mPosition = 0;
	}

	void LatencyDelayLine::reset()
	{
		mPosition = 0;
		std::fill(mRing.begin(), mRing.end(), 0.0f);
	}

	void LatencyDelayLine::process(float *aBuffer,
	                               unsigned int aSamples,
	                               unsigned int aStride,
	                               unsigned int aChannels)
	{
		if (mDelayFrames == 0 || aSamples == 0 || aChannels == 0 || !aBuffer)
			return;
		// Defensive: only process channels for which storage was allocated.
		const unsigned int storedChannels =
		    mDelayFrames ? (unsigned int)(mRing.size() / mDelayFrames) : 0;
		if (storedChannels == 0)
			return;
		if (aChannels > storedChannels)
			aChannels = storedChannels;

		const unsigned int d = mDelayFrames;
		unsigned int pos = mPosition;

		for (unsigned int c = 0; c < aChannels; ++c)
		{
			float *plane = aBuffer + (size_t)c * (size_t)aStride;
			float *ring = mRing.data() + (size_t)c * (size_t)d;
			// Every channel processes the same number of frames, so the
			// cursor ends at the same position for all of them.
			unsigned int p = pos;
			for (unsigned int i = 0; i < aSamples; ++i)
			{
				float incoming = plane[i];
				plane[i] = ring[p];
				ring[p] = incoming;
				++p;
				if (p == d)
					p = 0;
			}
		}

		// Advance the shared cursor by aSamples modulo d.
		mPosition = (pos + (aSamples % d)) % d;
	}

	// -----------------------------------------------------------------------
	// LatencyPlan
	// -----------------------------------------------------------------------

	LatencyPlan::LatencyPlan()
		: mRootLatencyFrames(0)
	{
	}

	// -----------------------------------------------------------------------
	// Soloud wiring
	// -----------------------------------------------------------------------

	void Soloud::discardFilterLatencyCompensation_internal()
	{
		LatencyPlan *old = mLatencyPlan;
		mLatencyPlan = NULL;
		mFilterLatencyCompensationFrames = 0;
		mLatencyMixStarted = false;
		delete old;
	}

	unsigned int Soloud::getFilterLatencyCompensationFrames() const
	{
		return mFilterLatencyCompensationFrames;
	}

	void Soloud::applyFilterLatencyCompensation_internal(unsigned int aVoiceIndex,
	                                                     float *aBuffer,
	                                                     unsigned int aSamples,
	                                                     unsigned int aStride)
	{
		LatencyPlan *plan = mLatencyPlan;
		if (!plan || !aBuffer || aSamples == 0)
			return;
		if (aVoiceIndex >= VOICE_COUNT || !mVoice[aVoiceIndex])
			return;
		plan->mLine[aVoiceIndex].process(
		    aBuffer, aSamples, aStride, mVoice[aVoiceIndex]->mChannels);
	}

	result Soloud::prepareFilterLatencyCompensation()
	{
		// Require an initialized engine.
		if (mSamplerate == 0 || !mAudioThreadMutex)
			return INVALID_PARAMETER;
		// The plan describes the graph before the first mix; once the
		// render has started the routing/filter state is fixed and prepare
		// must not be called again.
		if (mLatencyMixStarted)
			return INVALID_PARAMETER;

		lockAudioMutex_internal();

		const unsigned int N = VOICE_COUNT;

		// Build the routing forest. parent[i] is the parent voice slot,
		// -1 for root (bus handle 0), -2 when slot i is unused.
		std::vector<int> parent(N, -2);
		std::vector<unsigned long long> selfLatency(N, 0);
		std::vector<unsigned long long> subtreeLatency(N, 0);
		std::vector<unsigned long long> mergeLatency(N, 0);
		std::vector<std::vector<unsigned int> > children(N);
		std::vector<unsigned int> rootChildren;
		bool valid = true;

		for (unsigned int i = 0; i < N && valid; ++i)
		{
			AudioSourceInstance *voice = mVoice[i];
			if (!voice)
				continue;

			// Serial latency of the voice's own filter chain. These are
			// the live instances created by play()/Bus::setFilter().
			unsigned long long own = 0;
			for (unsigned int f = 0; f < FILTERS_PER_STREAM; ++f)
			{
				if (voice->mFilter[f])
					own += voice->mFilter[f]->getLatencyFrames();
			}
			selfLatency[i] = own;

			unsigned int busHandle = voice->mBusHandle;
			if (busHandle == 0)
			{
				parent[i] = -1;
				rootChildren.push_back(i);
				continue;
			}

			// Resolve the parent bus handle with explicit bounds checks;
			// do not trust the packed handle's slot field blindly.
			unsigned int packedSlot = busHandle & 0xfffu;
			if (packedSlot == 0)
			{
				valid = false;
				break;
			}
			int p = (int)packedSlot - 1;
			if (p < 0 || (unsigned int)p >= N || !mVoice[p] ||
			    getHandleFromVoice_internal((unsigned int)p) != busHandle)
			{
				valid = false;
				break;
			}
			parent[i] = p;
			children[p].push_back(i);
		}

		// Validate parent chains: every live voice must reach the root and
		// the graph must be acyclic.
		if (valid)
		{
			for (unsigned int i = 0; i < N; ++i)
			{
				if (parent[i] == -2)
					continue;
				unsigned int steps = 0;
				int cur = (int)i;
				std::vector<unsigned char> seen(N, 0);
				while (cur >= 0)
				{
					if ((unsigned int)cur >= N || parent[cur] == -2 || seen[cur])
					{
						valid = false;
						break;
					}
					seen[cur] = 1;
					cur = parent[cur];
					if (++steps > N)
					{
						valid = false;
						break;
					}
				}
				if (!valid)
					break;
			}
		}

		// Compute mergeLatency(p) = max subtreeLatency(c) over children c
		// and subtreeLatency(i) = selfLatency(i) + mergeLatency(i), with a
		// memoized DFS. Cycles were rejected above.
		if (valid)
		{
			std::vector<unsigned char> state(N, 0); // 0 new, 1 visiting, 2 done

			struct SoloudLatencyPlanner
			{
				static unsigned long long solve(
				    unsigned int i,
				    std::vector<unsigned char> &aState,
				    std::vector<std::vector<unsigned int> > &aChildren,
				    std::vector<unsigned long long> &aSelf,
				    std::vector<unsigned long long> &aMerge,
				    std::vector<unsigned long long> &aSubtree)
				{
					if (aState[i] == 2)
						return aSubtree[i];
					aState[i] = 1;
					unsigned long long worst = 0;
					for (size_t k = 0; k < aChildren[i].size(); ++k)
					{
						unsigned int c = aChildren[i][k];
						unsigned long long v = solve(c, aState, aChildren,
						                             aSelf, aMerge, aSubtree);
						if (v > worst)
							worst = v;
					}
					aMerge[i] = worst;
					aSubtree[i] = aSelf[i] + worst;
					aState[i] = 2;
					return aSubtree[i];
				}
			};

			for (unsigned int i = 0; i < N; ++i)
			{
				if (parent[i] == -2)
					continue;
				SoloudLatencyPlanner::solve(i, state, children,
				                            selfLatency, mergeLatency,
				                            subtreeLatency);
			}
		}

		unsigned long long rootMerge = 0;
		unsigned long long rootLatency = 0;
		if (valid)
		{
			for (size_t k = 0; k < rootChildren.size(); ++k)
			{
				unsigned int c = rootChildren[k];
				if (subtreeLatency[c] > rootMerge)
					rootMerge = subtreeLatency[c];
			}
			// Global filters hang off the single final output chain, so
			// their latency adds to the reported total but needs no
			// alignment delay. The supported render has none.
			unsigned long long globalLatency = 0;
			for (unsigned int f = 0; f < FILTERS_PER_STREAM; ++f)
			{
				if (mFilterInstance[f])
					globalLatency += mFilterInstance[f]->getLatencyFrames();
			}
			rootLatency = rootMerge + globalLatency;
		}

		// Build the new plan in a local owner first; a failure here leaves
		// mLatencyPlan (and thus the mix path) completely untouched.
		LatencyPlan *newPlan = NULL;
		if (valid)
		{
			try
			{
				newPlan = new LatencyPlan();
				newPlan->mRootLatencyFrames =
				    rootLatency > 0xffffffffull ? 0xffffffffu
				                               : (unsigned int)rootLatency;

				for (unsigned int i = 0; i < N; ++i)
				{
					if (parent[i] == -2)
						continue;
					unsigned long long parentMerge;
					if (parent[i] < 0)
						parentMerge = rootMerge;
					else
						parentMerge = mergeLatency[parent[i]];
					// parentMerge is the max of the siblings' subtree
					// latencies and therefore >= subtreeLatency[i].
					unsigned long long slack64 =
					    parentMerge >= subtreeLatency[i]
					        ? parentMerge - subtreeLatency[i]
					        : 0;
					if (slack64 > 0xffffffffull)
					{
						valid = false;
						break;
					}
					unsigned int slack = (unsigned int)slack64;
					unsigned int channels =
					    mVoice[i]->mChannels ? mVoice[i]->mChannels : 1u;
					if (channels > MAX_CHANNELS)
						channels = MAX_CHANNELS;
					newPlan->mLine[i].configure(slack, channels);
				}
			}
			catch (...)
			{
				valid = false;
			}
		}

		if (!valid)
		{
			delete newPlan;
			unlockAudioMutex_internal();
			return INVALID_PARAMETER;
		}

		// Commit atomically. Mixing has not started, and the audio mutex
		// is held, so the mix path can never observe a half-built plan.
		LatencyPlan *oldPlan = mLatencyPlan;
		mLatencyPlan = newPlan;
		mFilterLatencyCompensationFrames = newPlan->mRootLatencyFrames;
		delete oldPlan;

		unlockAudioMutex_internal();
		return SO_NO_ERROR;
	}
}
