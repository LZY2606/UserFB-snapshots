/*
 * Independent rendering session checks.
 *
 * The reference for every interleaving test is an exclusive run of the same
 * module with the same settings and the same read/seek order.  A separate
 * case proves that this reference, produced through ModPlug_LoadWithSettings,
 * is byte-identical to a decode through the legacy ModPlug_Load /
 * ModPlug_SetSettings API (whose output is additionally pinned by the golden
 * PCM files checked by task_support/baseline.cpp).
 */

#include "task_support/check.h"
#include "fixture.h"

#include <vector>
#include <cstring>
#include <cstdio>

namespace {

// Everything observable about one handle's read sequence.
struct Track
{
	unsigned frame;
	std::vector<unsigned char> pcm;
	std::vector<int> got;
	std::vector<int> order;
	std::vector<int> row;
};

// Drives "frames" worth of irregular small reads (including single-frame
// reads that stop in the middle of a tick) through the file and records the
// exact byte counts, PCM and order/row after every block.
bool RunBlocks(ModPlugFile *f, unsigned frame, unsigned frames, Track &t)
{
	t.frame = frame;
	unsigned total = 0;
	for (unsigned i = 0; total < frames; ++i)
	{
		unsigned n = session_fixture::block(i);
		if (n > frames - total) n = frames - total;
		std::vector<unsigned char> b(n * frame + 8, 0xa5);
		int got = ModPlug_Read(f, b.data(), (int)(n * frame));
		if (!task_check(got == (int)(n * frame), "short read", __FILE__, __LINE__)) return false;
		// The library must never write past the requested bytes.
		for (unsigned j = n * frame; j < b.size(); ++j)
		{
			if (!task_check(b[j] == 0xa5, "buffer overrun", __FILE__, __LINE__)) return false;
		}
		t.pcm.insert(t.pcm.end(), b.begin(), b.begin() + got);
		t.got.push_back(got);
		t.order.push_back(ModPlug_GetCurrentOrder(f));
		t.row.push_back(ModPlug_GetCurrentRow(f));
		total += n;
	}
	return true;
}

bool SameTrack(const Track &a, const Track &b, const char *what)
{
	if (!task_check(a.frame == b.frame, what, __FILE__, __LINE__)) return false;
	if (!task_check(a.got == b.got, what, __FILE__, __LINE__)) return false;
	if (!task_check(a.order == b.order, what, __FILE__, __LINE__)) return false;
	if (!task_check(a.row == b.row, what, __FILE__, __LINE__)) return false;
	if (!task_check(a.pcm == b.pcm, what, __FILE__, __LINE__)) return false;
	return true;
}

unsigned AudibleBytes(const Track &t, const ModPlug_Settings &s)
{
	unsigned char silent = (s.mBits == 8) ? 0x80 : 0x00;
	unsigned audible = 0;
	for (size_t i = 0; i < t.pcm.size(); ++i)
		if (t.pcm[i] != silent) ++audible;
	return audible;
}

ModPlugFile *OpenLegacy(const std::vector<unsigned char> &mod,
                        const ModPlug_Settings &s)
{
	// Legacy files read the global settings at load time; do not restore the
	// globals before the file has been read, since effect switches take
	// effect immediately on legacy sessions.  Callers use ScopedGlobalSettings
	// to restore once they are done with the file.
	ModPlug_SetSettings(&s);
	return ModPlug_Load(mod.data(), (int)mod.size());
}

// Restores the process-global settings at scope exit so cases that exercise
// ModPlug_SetSettings() cannot leak configuration into later cases.
struct ScopedGlobalSettings
{
	ModPlug_Settings saved;
	ScopedGlobalSettings() { ModPlug_GetSettings(&saved); }
	~ScopedGlobalSettings() { ModPlug_SetSettings(&saved); }
};

ModPlugFile *OpenExplicit(const std::vector<unsigned char> &mod,
                          const ModPlug_Settings &s)
{
	return ModPlug_LoadWithSettings(mod.data(), (int)mod.size(), &s);
}

const unsigned kFrames = 24000;

// ---------------- formats ----------------

// The new explicit-load path must reproduce the legacy global-settings path
// exactly for every supported output preset.
int CaseExplicitMatchesLegacy()
{
	ScopedGlobalSettings restore;
	const char *names[] = {"dry16", "wet16", "mono8", "stereo32"};
	const auto mod = session_fixture::module(0);
	for (const char *name : names)
	{
		const ModPlug_Settings s = session_fixture::settings(name);
		const unsigned frame = (unsigned)s.mChannels * s.mBits / 8;

		Track legacy;
		ModPlugFile *fl = OpenLegacy(mod, s);
		if (!task_check(fl != NULL, name, __FILE__, __LINE__)) return 1;
		if (!task_check(ModPlug_NumChannels(fl) == 4, name, __FILE__, __LINE__)) return 1;
		if (!RunBlocks(fl, frame, kFrames, legacy)) { ModPlug_Unload(fl); return 1; }
		const int legacyOrder = ModPlug_GetCurrentOrder(fl);
		const int legacyRow = ModPlug_GetCurrentRow(fl);
		ModPlug_Unload(fl);

		Track explicitTrack;
		ModPlugFile *fe = OpenExplicit(mod, s);
		if (!task_check(fe != NULL, name, __FILE__, __LINE__)) return 1;
		if (!RunBlocks(fe, frame, kFrames, explicitTrack)) { ModPlug_Unload(fe); return 1; }
		const int explicitOrder = ModPlug_GetCurrentOrder(fe);
		const int explicitRow = ModPlug_GetCurrentRow(fe);
		ModPlug_Unload(fe);

		if (!task_check(explicitOrder == legacyOrder, name, __FILE__, __LINE__)) return 1;
		if (!task_check(explicitRow == legacyRow, name, __FILE__, __LINE__)) return 1;
		if (!SameTrack(explicitTrack, legacy, name)) return 1;
		if (!task_check(AudibleBytes(explicitTrack, s) >= 100, name, __FILE__, __LINE__)) return 1;
	}
	return 0;
}

// Interleaving kind 1: identical settings/effect parameters, different audio
// content (different notes and waveform).
int CaseInterleavedSameSettings()
{
	const ModPlug_Settings s = session_fixture::settings("wet16");
	const unsigned frame = (unsigned)s.mChannels * s.mBits / 8;
	const auto modA = session_fixture::module(0);
	const auto modB = session_fixture::module(1);

	Track refA, refB;
	ModPlugFile *ra = OpenExplicit(modA, s);
	ModPlugFile *rb = OpenExplicit(modB, s);
	if (!task_check(ra && rb, "reference open", __FILE__, __LINE__)) return 1;
	if (!RunBlocks(ra, frame, kFrames, refA)) return 1;
	if (!RunBlocks(rb, frame, kFrames, refB)) return 1;
	ModPlug_Unload(ra);
	ModPlug_Unload(rb);
	// The two modules really do render different music.
	if (!task_check(refA.pcm != refB.pcm, "modules differ", __FILE__, __LINE__)) return 1;

	ModPlugFile *fa = OpenExplicit(modA, s);
	ModPlugFile *fb = OpenExplicit(modB, s);
	if (!task_check(fa && fb, "interleaved open", __FILE__, __LINE__)) return 1;
	Track ia, ib;
	ia.frame = ib.frame = frame;
	unsigned total = 0;
	for (unsigned i = 0; total < kFrames; ++i)
	{
		unsigned n = session_fixture::block(i);
		if (n > kFrames - total) n = kFrames - total;
		std::vector<unsigned char> ba(n * frame), bb(n * frame);
		int ga = ModPlug_Read(fa, ba.data(), (int)(n * frame));
		int gb = ModPlug_Read(fb, bb.data(), (int)(n * frame));
		if (!task_check(ga == (int)(n * frame) && gb == (int)(n * frame), "interleaved read", __FILE__, __LINE__)) return 1;
		ia.pcm.insert(ia.pcm.end(), ba.begin(), ba.end());
		ib.pcm.insert(ib.pcm.end(), bb.begin(), bb.end());
		ia.got.push_back(ga); ib.got.push_back(gb);
		ia.order.push_back(ModPlug_GetCurrentOrder(fa)); ib.order.push_back(ModPlug_GetCurrentOrder(fb));
		ia.row.push_back(ModPlug_GetCurrentRow(fa));     ib.row.push_back(ModPlug_GetCurrentRow(fb));
		total += n;
	}
	ModPlug_Unload(fa);
	ModPlug_Unload(fb);
	if (!SameTrack(ia, refA, "A interleaved vs exclusive")) return 1;
	if (!SameTrack(ib, refB, "B interleaved vs exclusive")) return 1;
	return 0;
}

// Interleaving kind 2: different frequency, channel count, bit width,
// resampling mode and effect parameters in the same process.
int CaseInterleavedFormats()
{
	struct Lane { const char *name; unsigned variant; };
	const Lane lanes[] = {{"dry16", 0}, {"mono8", 1}, {"stereo32", 0}, {"wet16", 1}};
	const unsigned laneCount = sizeof(lanes) / sizeof(lanes[0]);
	ModPlug_Settings settings[4];
	unsigned frame[4];
	std::vector<unsigned char> mods[2];
	mods[0] = session_fixture::module(0);
	mods[1] = session_fixture::module(1);
	Track refs[4];
	for (unsigned k = 0; k < laneCount; ++k)
	{
		settings[k] = session_fixture::settings(lanes[k].name);
		frame[k] = (unsigned)settings[k].mChannels * settings[k].mBits / 8;
		ModPlugFile *r = OpenExplicit(mods[lanes[k].variant], settings[k]);
		if (!task_check(r != NULL, lanes[k].name, __FILE__, __LINE__)) return 1;
		if (!RunBlocks(r, frame[k], kFrames, refs[k])) return 1;
		ModPlug_Unload(r);
		if (!task_check(AudibleBytes(refs[k], settings[k]) >= 100, lanes[k].name, __FILE__, __LINE__)) return 1;
	}

	ModPlugFile *files[4];
	Track inter[4];
	for (unsigned k = 0; k < laneCount; ++k)
	{
		files[k] = OpenExplicit(mods[lanes[k].variant], settings[k]);
		if (!task_check(files[k] != NULL, lanes[k].name, __FILE__, __LINE__)) return 1;
		inter[k].frame = frame[k];
	}
	unsigned total = 0;
	for (unsigned i = 0; total < kFrames; ++i)
	{
		unsigned n = session_fixture::block(i);
		if (n > kFrames - total) n = kFrames - total;
		for (unsigned k = 0; k < laneCount; ++k)
		{
			std::vector<unsigned char> b(n * frame[k]);
			int got = ModPlug_Read(files[k], b.data(), (int)(n * frame[k]));
			if (!task_check(got == (int)(n * frame[k]), lanes[k].name, __FILE__, __LINE__)) return 1;
			inter[k].pcm.insert(inter[k].pcm.end(), b.begin(), b.end());
			inter[k].got.push_back(got);
			inter[k].order.push_back(ModPlug_GetCurrentOrder(files[k]));
			inter[k].row.push_back(ModPlug_GetCurrentRow(files[k]));
		}
		total += n;
	}
	for (unsigned k = 0; k < laneCount; ++k)
	{
		ModPlug_Unload(files[k]);
		if (!SameTrack(inter[k], refs[k], lanes[k].name)) return 1;
	}
	return 0;
}

// All four resampling modes must be accepted by an independent session and
// survive interleaving identically.  Also exercises stereo separation, the
// polyphony cap and a non-zero loop count.
int CaseResamplingModesAndLimits()
{
	const auto modA = session_fixture::module(0);
	const auto modB = session_fixture::module(1);
	for (int mode = MODPLUG_RESAMPLE_NEAREST; mode <= MODPLUG_RESAMPLE_FIR; ++mode)
	{
		ModPlug_Settings s = session_fixture::settings("dry16");
		s.mResamplingMode = mode;
		const unsigned frame = (unsigned)s.mChannels * s.mBits / 8;

		Track refA, refB;
		ModPlugFile *ra = OpenExplicit(modA, s);
		ModPlugFile *rb = OpenExplicit(modB, s);
		if (!task_check(ra && rb, "reference open", __FILE__, __LINE__)) return 1;
		if (!RunBlocks(ra, frame, kFrames, refA)) return 1;
		if (!RunBlocks(rb, frame, kFrames, refB)) return 1;
		ModPlug_Unload(ra);
		ModPlug_Unload(rb);
		if (!task_check(AudibleBytes(refA, s) >= 100, "audible resampled output", __FILE__, __LINE__)) return 1;

		ModPlugFile *fa = OpenExplicit(modA, s);
		ModPlugFile *fb = OpenExplicit(modB, s);
		if (!task_check(fa && fb, "interleaved open", __FILE__, __LINE__)) return 1;
		Track ia, ib;
		ia.frame = ib.frame = frame;
		unsigned total = 0;
		for (unsigned i = 0; total < kFrames; ++i)
		{
			unsigned n = session_fixture::block(i);
			if (n > kFrames - total) n = kFrames - total;
			std::vector<unsigned char> ba(n * frame), bb(n * frame);
			if (!task_check(ModPlug_Read(fa, ba.data(), (int)(n * frame)) == (int)(n * frame), "read A", __FILE__, __LINE__)) return 1;
			if (!task_check(ModPlug_Read(fb, bb.data(), (int)(n * frame)) == (int)(n * frame), "read B", __FILE__, __LINE__)) return 1;
			ia.pcm.insert(ia.pcm.end(), ba.begin(), ba.end());
			ib.pcm.insert(ib.pcm.end(), bb.begin(), bb.end());
			ia.got.push_back((int)(n * frame)); ib.got.push_back((int)(n * frame));
			ia.order.push_back(ModPlug_GetCurrentOrder(fa)); ib.order.push_back(ModPlug_GetCurrentOrder(fb));
			ia.row.push_back(ModPlug_GetCurrentRow(fa));     ib.row.push_back(ModPlug_GetCurrentRow(fb));
			total += n;
		}
		ModPlug_Unload(fa);
		ModPlug_Unload(fb);
		if (!SameTrack(ia, refA, "resample A")) return 1;
		if (!SameTrack(ib, refB, "resample B")) return 1;
	}

	ModPlug_Settings limited = session_fixture::settings("wet16");
	limited.mStereoSeparation = 200;
	limited.mMaxMixChannels = 32;
	limited.mLoopCount = 1;
	const unsigned frame = (unsigned)limited.mChannels * limited.mBits / 8;
	ModPlugFile *f = OpenExplicit(modA, limited);
	if (!task_check(f != NULL, "limited session open", __FILE__, __LINE__)) return 1;
	Track t;
	if (!RunBlocks(f, frame, kFrames, t)) return 1;
	if (!task_check(AudibleBytes(t, limited) >= 100, "limited session audible", __FILE__, __LINE__)) return 1;
	ModPlug_Unload(f);
	return 0;
}

// ---------------- dsp_history ----------------

// The settings struct is copied at load time: mutating the caller's copy or
// the global settings afterwards must not reach the independent handle, and
// an explicit load must not change the global defaults either.
int CaseSettingsCopy()
{
	ScopedGlobalSettings restore;
	const auto mod = session_fixture::module(0);
	ModPlug_Settings s = session_fixture::settings("wet16");
	const unsigned frame = (unsigned)s.mChannels * s.mBits / 8;

	Track ref;
	ModPlugFile *r = OpenExplicit(mod, s);
	if (!task_check(r != NULL, "reference open", __FILE__, __LINE__)) return 1;
	if (!RunBlocks(r, frame, kFrames, ref)) return 1;
	ModPlug_Unload(r);

	ModPlug_Settings marker = session_fixture::settings("mono8");
	ModPlug_SetSettings(&marker);
	ModPlugFile *f = OpenExplicit(mod, s);
	if (!task_check(f != NULL, "explicit open", __FILE__, __LINE__)) return 1;

	ModPlug_Settings afterLoad;
	ModPlug_GetSettings(&afterLoad);
	if (!task_check(memcmp(&afterLoad, &marker, sizeof(marker)) == 0,
	                "explicit load leaves global settings alone", __FILE__, __LINE__)) return 1;

	// Destroy the caller's struct contents and push incompatible globals.
	std::memset(&s, 0, sizeof(s));
	ModPlug_Settings hostile = session_fixture::settings("stereo32");
	hostile.mFlags = 0;
	ModPlug_SetSettings(&hostile);

	Track got;
	if (!RunBlocks(f, frame, kFrames, got)) return 1;
	ModPlug_Unload(f);
	if (!SameTrack(got, ref, "private settings copy")) return 1;
	return 0;
}

// Wet presets engage every persistent master effect (noise reduction, reverb,
// megabass, surround).  Interleaving wet tracks through tiny blocks must keep
// each handle's filter history and delay lines, and the output must match an
// exclusive run sample-for-sample.  A switch-only or per-block-effect-reset
// implementation breaks this equality.
int CaseDspContinuity()
{
	const ModPlug_Settings wet = session_fixture::settings("wet16");
	const ModPlug_Settings dry = session_fixture::settings("dry16");
	const unsigned frame = (unsigned)wet.mChannels * wet.mBits / 8;
	const auto modA = session_fixture::module(0);
	const auto modB = session_fixture::module(1);

	Track wetRef, dryRef;
	ModPlugFile *rw = OpenExplicit(modA, wet);
	ModPlugFile *rd = OpenExplicit(modA, dry);
	if (!task_check(rw && rd, "reference open", __FILE__, __LINE__)) return 1;
	if (!RunBlocks(rw, frame, kFrames, wetRef)) return 1;
	if (!RunBlocks(rd, frame, kFrames, dryRef)) return 1;
	ModPlug_Unload(rw);
	ModPlug_Unload(rd);
	// Sanity: the effect switches really change the rendered audio.
	if (!task_check(wetRef.pcm != dryRef.pcm, "wet and dry presets differ", __FILE__, __LINE__)) return 1;

	ModPlugFile *fa = OpenExplicit(modA, wet);
	ModPlugFile *fb = OpenExplicit(modB, wet);
	if (!task_check(fa && fb, "interleaved open", __FILE__, __LINE__)) return 1;
	Track ia, ib;
	ia.frame = ib.frame = frame;
	unsigned total = 0;
	for (unsigned i = 0; total < kFrames; ++i)
	{
		unsigned n = session_fixture::block(i);
		if (n > kFrames - total) n = kFrames - total;
		std::vector<unsigned char> ba(n * frame), bb(n * frame);
		if (!task_check(ModPlug_Read(fa, ba.data(), (int)(n * frame)) == (int)(n * frame), "read A", __FILE__, __LINE__)) return 1;
		if (!task_check(ModPlug_Read(fb, bb.data(), (int)(n * frame)) == (int)(n * frame), "read B", __FILE__, __LINE__)) return 1;
		ia.pcm.insert(ia.pcm.end(), ba.begin(), ba.end());
		ib.pcm.insert(ib.pcm.end(), bb.begin(), bb.end());
		ia.got.push_back((int)(n * frame)); ib.got.push_back((int)(n * frame));
		ia.order.push_back(ModPlug_GetCurrentOrder(fa)); ib.order.push_back(ModPlug_GetCurrentOrder(fb));
		ia.row.push_back(ModPlug_GetCurrentRow(fa));     ib.row.push_back(ModPlug_GetCurrentRow(fb));
		total += n;
	}
	ModPlug_Unload(fa);
	ModPlug_Unload(fb);

	Track refB;
	ModPlugFile *rb = OpenExplicit(modB, wet);
	if (!task_check(rb != NULL, "reference B open", __FILE__, __LINE__)) return 1;
	if (!RunBlocks(rb, frame, kFrames, refB)) return 1;
	ModPlug_Unload(rb);

	if (!SameTrack(ia, wetRef, "wet A continuity")) return 1;
	if (!SameTrack(ib, refB, "wet B continuity")) return 1;
	return 0;
}

struct HookRec
{
	unsigned calls;
	// CSoundFile::Read() invokes the hook as hook(buffer, nsamples, channels):
	// the second size argument is the sample count, the third is the channel
	// count (see the ModPlug_InitMixerCallback documentation).
	unsigned long lastSamples;
	unsigned long lastChannels;
};
HookRec gHookA, gHookB;

void HookA(int *, unsigned long nsamples, unsigned long channels)
{
	++gHookA.calls;
	gHookA.lastSamples = nsamples;
	gHookA.lastChannels = channels;
}

void HookB(int *, unsigned long nsamples, unsigned long channels)
{
	++gHookB.calls;
	gHookB.lastSamples = nsamples;
	gHookB.lastChannels = channels;
}

// Callbacks belong to individual handles, including their size arguments.
int CaseIndependentCallbacks()
{
	const ModPlug_Settings sA = session_fixture::settings("wet16");     // stereo
	const ModPlug_Settings sB = session_fixture::settings("mono8");     // mono
	const unsigned fA = (unsigned)sA.mChannels * sA.mBits / 8;
	const unsigned fB = (unsigned)sB.mChannels * sB.mBits / 8;
	const auto modA = session_fixture::module(0);
	const auto modB = session_fixture::module(1);

	gHookA = gHookB = HookRec();
	ModPlugFile *fa = OpenExplicit(modA, sA);
	ModPlugFile *fb = OpenExplicit(modB, sB);
	if (!task_check(fa && fb, "open", __FILE__, __LINE__)) return 1;
	ModPlug_InitMixerCallback(fa, HookA);
	ModPlug_InitMixerCallback(fb, HookB);

	const unsigned frames = 6000;
	std::vector<unsigned char> ba(frames * fA), bb(frames * fB);
	// Interleave a handful of blocks on each handle.
	for (unsigned i = 0; i < 8; ++i)
	{
		unsigned n = session_fixture::block(i);
		if (!task_check(ModPlug_Read(fa, ba.data(), (int)(n * fA)) == (int)(n * fA), "read A", __FILE__, __LINE__)) return 1;
		if (!task_check(ModPlug_Read(fb, bb.data(), (int)(n * fB)) == (int)(n * fB), "read B", __FILE__, __LINE__)) return 1;
	}
	if (!task_check(gHookA.calls > 0 && gHookB.calls > 0, "both callbacks fire", __FILE__, __LINE__)) return 1;
	if (!task_check(gHookA.lastChannels == 2, "A channel argument", __FILE__, __LINE__)) return 1;
	if (!task_check(gHookB.lastChannels == 1, "B channel argument", __FILE__, __LINE__)) return 1;
	if (!task_check(gHookA.lastSamples > 0 && gHookB.lastSamples > 0, "sample argument", __FILE__, __LINE__)) return 1;

	const unsigned callsABefore = gHookA.calls;
	const unsigned callsBBefore = gHookB.calls;
	// Reading B must never invoke A's callback.
	if (!task_check(ModPlug_Read(fb, bb.data(), (int)(fB * 256)) == (int)(fB * 256), "read B alone", __FILE__, __LINE__)) return 1;
	if (!task_check(gHookA.calls == callsABefore, "A callback untouched by B", __FILE__, __LINE__)) return 1;
	if (!task_check(gHookB.calls > callsBBefore, "B callback advances", __FILE__, __LINE__)) return 1;

	ModPlug_UnloadMixerCallback(fa);
	if (!task_check(ModPlug_Read(fa, ba.data(), (int)(fA * 256)) == (int)(fA * 256), "read A after hook clear", __FILE__, __LINE__)) return 1;
	if (!task_check(gHookA.calls == callsABefore, "A callback stays cleared", __FILE__, __LINE__)) return 1;
	const unsigned callsBNow = gHookB.calls;
	if (!task_check(ModPlug_Read(fb, bb.data(), (int)(fB * 256)) == (int)(fB * 256), "B still readable", __FILE__, __LINE__)) return 1;
	if (!task_check(gHookB.calls > callsBNow, "B callback survives A clearing", __FILE__, __LINE__)) return 1;

	ModPlug_Unload(fa);
	const unsigned callsBAfterUnload = gHookB.calls;
	if (!task_check(ModPlug_Read(fb, bb.data(), (int)(fB * 256)) == (int)(fB * 256), "B survives A unload", __FILE__, __LINE__)) return 1;
	if (!task_check(gHookB.calls > callsBAfterUnload, "callback survives unload", __FILE__, __LINE__)) return 1;
	ModPlug_Unload(fb);
	return 0;
}

// ---------------- lifecycle ----------------

// Loads, reads, seeks and unloads of other handles (including legacy handles
// and global setting changes) must not advance or reset an independent
// session.
int CaseOtherHandleLifecycle()
{
	ScopedGlobalSettings restore;
	const ModPlug_Settings sA = session_fixture::settings("dry16");
	const ModPlug_Settings sB = session_fixture::settings("wet16");
	const unsigned fA = (unsigned)sA.mChannels * sA.mBits / 8;
	const unsigned fB = (unsigned)sB.mChannels * sB.mBits / 8;
	const auto modA = session_fixture::module(0);
	const auto modB = session_fixture::module(1);

	Track refA;
	ModPlugFile *r = OpenExplicit(modA, sA);
	if (!task_check(r != NULL, "reference open", __FILE__, __LINE__)) return 1;
	if (!RunBlocks(r, fA, kFrames, refA)) return 1;
	ModPlug_Unload(r);

	ModPlugFile *fa = OpenExplicit(modA, sA);
	ModPlugFile *fb = OpenExplicit(modB, sB);
	if (!task_check(fa && fb, "open", __FILE__, __LINE__)) return 1;
	Track got;
	got.frame = fA;
	unsigned total = 0;
	for (unsigned i = 0; total < kFrames; ++i)
	{
		unsigned n = session_fixture::block(i);
		if (n > kFrames - total) n = kFrames - total;
		std::vector<unsigned char> ba(n * fA), bb(n * fB);
		if (!task_check(ModPlug_Read(fa, ba.data(), (int)(n * fA)) == (int)(n * fA), "read A", __FILE__, __LINE__)) return 1;
		if (!task_check(ModPlug_Read(fb, bb.data(), (int)(n * fB)) == (int)(n * fB), "read B", __FILE__, __LINE__)) return 1;
		got.pcm.insert(got.pcm.end(), ba.begin(), ba.end());
		got.got.push_back((int)(n * fA));
		got.order.push_back(ModPlug_GetCurrentOrder(fa));
		got.row.push_back(ModPlug_GetCurrentRow(fa));

		// Disturb the world around A: seek B, unload B, load a third explicit
		// handle, load/unload a legacy handle, and rewrite global settings.
		const int orderBefore = ModPlug_GetCurrentOrder(fa);
		const int rowBefore = ModPlug_GetCurrentRow(fa);
		ModPlug_Seek(fb, 150);
		if (!task_check(ModPlug_GetCurrentOrder(fa) == orderBefore, "seek B spares A order", __FILE__, __LINE__)) return 1;
		if (!task_check(ModPlug_GetCurrentRow(fa) == rowBefore, "seek B spares A row", __FILE__, __LINE__)) return 1;
		ModPlug_Unload(fb);
		ModPlugFile *fc = OpenExplicit(modB, sB);
		if (!task_check(fc != NULL, "C open", __FILE__, __LINE__)) return 1;
		std::vector<unsigned char> bc(fB * 64);
		if (!task_check(ModPlug_Read(fc, bc.data(), (int)bc.size()) == (int)bc.size(), "read C", __FILE__, __LINE__)) return 1;
		ModPlugFile *legacy = OpenLegacy(modB, sB);
		if (!task_check(legacy != NULL, "legacy open", __FILE__, __LINE__)) return 1;
		ModPlug_Unload(legacy);
		ModPlug_Settings hostile = session_fixture::settings("mono8");
		ModPlug_SetSettings(&hostile);
		ModPlug_Unload(fc);
		fb = OpenExplicit(modB, sB);
		if (!task_check(fb != NULL, "B reopen", __FILE__, __LINE__)) return 1;
		if (!task_check(ModPlug_Read(fb, bb.data(), (int)(n * fB)) == (int)(n * fB), "B reread", __FILE__, __LINE__)) return 1;
		total += n;
	}
	ModPlug_Unload(fa);
	ModPlug_Unload(fb);
	if (!SameTrack(got, refA, "A unaffected by foreign lifecycle")) return 1;
	return 0;
}

// Approximate seek stays per-handle and interleaved seeks match an exclusive
// run with the same seek sequence.  NULL settings are rejected.
int CaseSeekIsolationAndNullArgs()
{
	if (!task_check(ModPlug_LoadWithSettings(session_fixture::module(0).data(), 1, NULL) == NULL,
	                "NULL settings rejected", __FILE__, __LINE__)) return 1;

	const ModPlug_Settings sA = session_fixture::settings("stereo32");
	const ModPlug_Settings sB = session_fixture::settings("dry16");
	const unsigned fA = (unsigned)sA.mChannels * sA.mBits / 8;
	const unsigned fB = (unsigned)sB.mChannels * sB.mBits / 8;
	const auto modA = session_fixture::module(0);
	const auto modB = session_fixture::module(1);

	// Exclusive reference for A, including a seek in the middle.
	ModPlugFile *ra = OpenExplicit(modA, sA);
	if (!task_check(ra != NULL, "reference open", __FILE__, __LINE__)) return 1;
	Track refA;
	refA.frame = fA;
	unsigned total = 0;
	bool sought = false;
	for (unsigned i = 0; total < kFrames; ++i)
	{
		unsigned n = session_fixture::block(i);
		if (n > kFrames - total) n = kFrames - total;
		std::vector<unsigned char> b(n * fA);
		if (!task_check(ModPlug_Read(ra, b.data(), (int)(n * fA)) == (int)(n * fA), "reference read", __FILE__, __LINE__)) return 1;
		refA.pcm.insert(refA.pcm.end(), b.begin(), b.end());
		refA.got.push_back((int)(n * fA));
		refA.order.push_back(ModPlug_GetCurrentOrder(ra));
		refA.row.push_back(ModPlug_GetCurrentRow(ra));
		total += n;
		if (!sought && total >= kFrames / 2)
		{
			ModPlug_Seek(ra, 200);
			sought = true;
		}
	}
	ModPlug_Unload(ra);
	if (!task_check(sought, "seek happened", __FILE__, __LINE__)) return 1;

	ModPlugFile *fa = OpenExplicit(modA, sA);
	ModPlugFile *fb = OpenExplicit(modB, sB);
	if (!task_check(fa && fb, "interleaved open", __FILE__, __LINE__)) return 1;
	Track gotA;
	gotA.frame = fA;
	total = 0;
	sought = false;
	for (unsigned i = 0; total < kFrames; ++i)
	{
		unsigned n = session_fixture::block(i);
		if (n > kFrames - total) n = kFrames - total;
		std::vector<unsigned char> ba(n * fA), bb(n * fB);
		if (!task_check(ModPlug_Read(fa, ba.data(), (int)(n * fA)) == (int)(n * fA), "read A", __FILE__, __LINE__)) return 1;
		if (!task_check(ModPlug_Read(fb, bb.data(), (int)(n * fB)) == (int)(n * fB), "read B", __FILE__, __LINE__)) return 1;
		gotA.pcm.insert(gotA.pcm.end(), ba.begin(), ba.end());
		gotA.got.push_back((int)(n * fA));
		gotA.order.push_back(ModPlug_GetCurrentOrder(fa));
		gotA.row.push_back(ModPlug_GetCurrentRow(fa));
		total += n;
		if (!sought && total >= kFrames / 2)
		{
			// The seek on A must not move B's playback position.
			const int bOrderBefore = ModPlug_GetCurrentOrder(fb);
			const int bRowBefore = ModPlug_GetCurrentRow(fb);
			ModPlug_Seek(fa, 200);
			if (!task_check(ModPlug_GetCurrentOrder(fb) == bOrderBefore, "seek A spares B order", __FILE__, __LINE__)) return 1;
			if (!task_check(ModPlug_GetCurrentRow(fb) == bRowBefore, "seek A spares B row", __FILE__, __LINE__)) return 1;
			sought = true;
		}
	}
	ModPlug_Unload(fa);
	ModPlug_Unload(fb);
	if (!SameTrack(gotA, refA, "seek sequence matches exclusive run")) return 1;
	return 0;
}

} // namespace

// External linkage: task_support/driver.c reaches these through the extern
// declarations in task_support/check.h.
// The documented global defaults stay intact and explicit loads never mutate
// them.  Earlier cases restore the globals through ScopedGlobalSettings.
int CaseGlobalDefaultsPreserved()
{
	ScopedGlobalSettings restore;
	ModPlug_Settings d;
	ModPlug_GetSettings(&d);
	if (!task_check(d.mChannels == 2, "default channels", __FILE__, __LINE__)) return 1;
	if (!task_check(d.mBits == 16, "default bits", __FILE__, __LINE__)) return 1;
	if (!task_check(d.mFrequency == 44100, "default frequency", __FILE__, __LINE__)) return 1;
	if (!task_check(d.mResamplingMode == MODPLUG_RESAMPLE_LINEAR, "default resampling", __FILE__, __LINE__)) return 1;
	if (!task_check(d.mStereoSeparation == 128, "default separation", __FILE__, __LINE__)) return 1;
	if (!task_check(d.mMaxMixChannels == 32, "default max channels", __FILE__, __LINE__)) return 1;
	if (!task_check((d.mFlags & (MODPLUG_ENABLE_OVERSAMPLING | MODPLUG_ENABLE_NOISE_REDUCTION))
	                == (MODPLUG_ENABLE_OVERSAMPLING | MODPLUG_ENABLE_NOISE_REDUCTION),
	                "default effect flags", __FILE__, __LINE__)) return 1;

	// An explicit load with very different settings leaves the defaults alone.
	ModPlug_Settings before;
	ModPlug_GetSettings(&before);
	ModPlug_Settings other = session_fixture::settings("mono8");
	ModPlugFile *f = OpenExplicit(session_fixture::module(1), other);
	if (!task_check(f != NULL, "explicit open", __FILE__, __LINE__)) return 1;
	ModPlug_Settings after;
	ModPlug_GetSettings(&after);
	if (!task_check(memcmp(&before, &after, sizeof(before)) == 0,
	                "explicit load preserves global defaults", __FILE__, __LINE__)) return 1;
	ModPlug_Unload(f);
	return 0;
}

const task_case task_cases[] =
{
	{"formats",     "explicit_matches_legacy",   CaseExplicitMatchesLegacy},
	{"formats",     "interleaved_same_settings", CaseInterleavedSameSettings},
	{"formats",     "interleaved_formats",       CaseInterleavedFormats},
	{"formats",     "resampling_modes_limits",   CaseResamplingModesAndLimits},
	{"dsp_history", "settings_copy",             CaseSettingsCopy},
	{"dsp_history", "dsp_continuity",            CaseDspContinuity},
	{"dsp_history", "independent_callbacks",     CaseIndependentCallbacks},
	{"lifecycle",   "other_handle_lifecycle",    CaseOtherHandleLifecycle},
	{"lifecycle",   "seek_isolation_null_args",  CaseSeekIsolationAndNullArgs},
	{"lifecycle",   "global_defaults_preserved", CaseGlobalDefaultsPreserved},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
