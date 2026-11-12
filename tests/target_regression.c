#include "harness.h"
#include <string.h>
#include <stdlib.h>

/* Controllable allocator hooks: all three macros must be overridden together,
   otherwise the library falls back to its default definitions for all of them. */
static int uf_deny;
static long uf_alloc_calls;
static void* uf_malloc(size_t n) { ++uf_alloc_calls; return uf_deny ? (void*)0 : malloc(n); }
static void* uf_realloc(void* p, size_t n) { ++uf_alloc_calls; return uf_deny ? (void*)0 : realloc(p, n); }
static void uf_free(void* p) { free(p); }
#define TSF_MALLOC uf_malloc
#define TSF_REALLOC uf_realloc
#define TSF_FREE uf_free
#define TSF_IMPLEMENTATION
#include "tsf.h"
#include "font_fixture.h"

/* Like UF_CHECK but reports which region/slot went wrong first. */
#define UF_CHECK_AT(tag, i, x) do { ++uf_assertions; if(!(x)) { fprintf(stderr,"ASSERT %s:%d: %s [%s %d]\n",__FILE__,__LINE__,#x,tag,(int)(i)); exit(2); } } while(0)

static int uf_live(tsf* f)
{
	int n = 0, i;
	for (i = 0; i < f->voiceNum; i++)
		if (f->voices[i].playingPreset != -1 && f->voices[i].ampenv.segment < TSF_SEGMENT_RELEASE) n++;
	return n;
}

static int uf_releasing(tsf* f)
{
	int n = 0, i;
	for (i = 0; i < f->voiceNum; i++)
		if (f->voices[i].playingPreset != -1 && f->voices[i].ampenv.segment == TSF_SEGMENT_RELEASE) n++;
	return n;
}

static void uf_stereo_energy(const float* p, int samples, double* left, double* right)
{
	int i;
	*left = *right = 0;
	for (i = 0; i < samples; i++)
	{
		*left += (double)p[i * 2] * p[i * 2];
		*right += (double)p[i * 2 + 1] * p[i * 2 + 1];
	}
}

static int uf_channels_differ(const float* p, int samples)
{
	int i;
	for (i = 0; i < samples; i++)
		if (p[i * 2] != p[i * 2 + 1]) return 1;
	return 0;
}

static void uf_render_chunks(tsf* f, float* out, int samples, int chunk)
{
	int done = 0;
	while (done < samples)
	{
		int n = samples - done;
		if (n > chunk) n = chunk;
		tsf_render_float(f, out + (size_t)done * 2, n, 0);
		done += n;
	}
}

void uf_layers(void)
{
	float pcm[512 * 2];
	double el, er;

	/* Two layers of one exclusive class start together as one group. */
	{
		tsf* f = make_font(2, 7);
		unsigned p1;
		int i, k;
		UF_CHECK(f != 0);
		tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
		UF_CHECK(tsf_note_on(f, 0, 60, 1.0f) == 1);
		UF_CHECK(uf_live(f) == 2);
		UF_CHECK(uf_releasing(f) == 0);
		UF_CHECK(f->voices[0].playIndex == f->voices[1].playIndex);
		UF_CHECK(f->voices[0].region != f->voices[1].region);
		/* each layer keeps its own pan: layer 0 hard left, layer 1 hard right */
		UF_CHECK(f->voices[0].panFactorLeft == 1.0f && f->voices[0].panFactorRight == 0.0f);
		UF_CHECK(f->voices[1].panFactorLeft == 0.0f && f->voices[1].panFactorRight == 1.0f);
		/* each layer keeps its own sample phase */
		UF_CHECK(f->voices[0].sourceSamplePosition == (double)f->voices[0].region->offset);
		UF_CHECK(f->voices[1].sourceSamplePosition == (double)f->voices[1].region->offset);
		UF_CHECK(f->voices[0].region->offset != f->voices[1].region->offset);
		/* both sides of the stereo image carry their own layer */
		tsf_render_float(f, pcm, 512, 0);
		uf_stereo_energy(pcm, 512, &el, &er);
		UF_CHECK(el > 0.001 && er > 0.001);
		UF_CHECK(uf_channels_differ(pcm, 512));
		UF_CHECK(uf_live(f) == 2 && uf_releasing(f) == 0);

		/* A repeated note-on cuts the old group with a quick release and
		   starts the new group completely; the new group keeps sounding
		   after the cut one has faded. */
		p1 = f->voices[0].playIndex;
		UF_CHECK(tsf_note_on(f, 0, 60, 1.0f) == 1);
		UF_CHECK(uf_live(f) == 2);
		UF_CHECK(uf_releasing(f) == 2);
		for (i = 0; i < f->voiceNum; i++)
		{
			struct tsf_voice* v = &f->voices[i];
			if (v->playingPreset == -1) continue;
			if (v->playIndex == p1)
				UF_CHECK_AT("slot", i, v->ampenv.segment == TSF_SEGMENT_RELEASE && v->ampenv.parameters.release == 0.0f);
			else
				UF_CHECK_AT("slot", i, v->ampenv.segment < TSF_SEGMENT_RELEASE);
		}
		for (k = 0; k < 40; k++) tsf_render_float(f, pcm, 512, 0);
		UF_CHECK(uf_live(f) == 2);
		UF_CHECK(tsf_active_voice_count(f) == 2);
		tsf_close(f);
	}

	/* Six layers of one class all start and share one play index. */
	{
		tsf* f = make_font(6, 7);
		unsigned p;
		int i;
		UF_CHECK(f != 0);
		tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
		UF_CHECK(tsf_note_on(f, 0, 60, 1.0f) == 1);
		UF_CHECK(uf_live(f) == 6);
		UF_CHECK(uf_releasing(f) == 0);
		p = f->voices[0].playIndex;
		for (i = 0; i < f->voiceNum; i++)
			if (f->voices[i].playingPreset != -1)
				UF_CHECK_AT("slot", i, f->voices[i].playIndex == p);
		tsf_render_float(f, pcm, 512, 0);
		uf_stereo_energy(pcm, 512, &el, &er);
		UF_CHECK(el > 0.001 && er > 0.001);
		tsf_close(f);
	}

	/* A match range that only hits some of the layers starts exactly those. */
	{
		tsf* f = make_font(2, 7);
		UF_CHECK(f != 0);
		tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
		f->presets[0].regions[1].lokey = 61;
		UF_CHECK(tsf_note_on(f, 0, 60, 1.0f) == 1);
		UF_CHECK(uf_live(f) == 1);
		UF_CHECK(uf_releasing(f) == 0);
		UF_CHECK(tsf_note_on(f, 0, 61, 1.0f) == 1);
		UF_CHECK(uf_live(f) == 2); /* old layer was cut, both regions match key 61 */
		UF_CHECK(uf_releasing(f) == 1);
		tsf_close(f);
	}

	/* Multiple exclusive classes: only same-class old sounds are cut. */
	{
		tsf* f = make_font(2, 7);
		unsigned p1;
		int i, survivors;
		UF_CHECK(f != 0);
		tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
		f->presets[0].regions[1].group = 9; /* layers now in different classes */
		UF_CHECK(tsf_note_on(f, 0, 60, 1.0f) == 1);
		UF_CHECK(uf_live(f) == 2);
		p1 = f->voices[0].playIndex;
		f->presets[0].regions[1].lokey = 61; /* next note-on only matches the class-7 region */
		UF_CHECK(tsf_note_on(f, 0, 60, 1.0f) == 1);
		UF_CHECK(uf_live(f) == 2);      /* class-9 survivor + new class-7 layer */
		UF_CHECK(uf_releasing(f) == 1); /* old class-7 layer cut */
		survivors = 0;
		for (i = 0; i < f->voiceNum; i++)
		{
			struct tsf_voice* v = &f->voices[i];
			if (v->playingPreset != -1 && v->playIndex == p1 && v->ampenv.segment < TSF_SEGMENT_RELEASE)
			{
				UF_CHECK_AT("slot", i, v->region->group == 9);
				survivors++;
			}
		}
		UF_CHECK(survivors == 1);
		tsf_close(f);
	}

	/* Class 0 never cuts anything. */
	{
		tsf* f = make_font(2, 0);
		UF_CHECK(f != 0);
		tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
		UF_CHECK(tsf_note_on(f, 0, 60, 1.0f) == 1);
		UF_CHECK(tsf_note_on(f, 0, 60, 1.0f) == 1);
		UF_CHECK(tsf_note_on(f, 0, 60, 1.0f) == 1);
		UF_CHECK(uf_live(f) == 6);
		UF_CHECK(uf_releasing(f) == 0);
		tsf_close(f);
	}

	/* Different presets do not cut each other even with the same class. */
	{
		tsf* f = make_font(2, 7);
		UF_CHECK(f != 0);
		tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
		UF_CHECK(tsf_note_on(f, 0, 60, 1.0f) == 1);
		UF_CHECK(tsf_note_on(f, 1, 60, 1.0f) == 1);
		UF_CHECK(uf_live(f) == 4);
		UF_CHECK(uf_releasing(f) == 0);
		tsf_close(f);
	}

	/* One note-off releases the earliest still-held group as a whole. */
	{
		tsf* f = make_font(2, 0);
		unsigned p1, p2;
		int i, rel1, live2;
		UF_CHECK(f != 0);
		tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
		UF_CHECK(tsf_note_on(f, 0, 60, 1.0f) == 1);
		p1 = f->voices[0].playIndex;
		UF_CHECK(tsf_note_on(f, 0, 60, 1.0f) == 1);
		p2 = p1;
		for (i = 0; i < f->voiceNum; i++)
			if (f->voices[i].playingPreset != -1 && f->voices[i].playIndex != p1) p2 = f->voices[i].playIndex;
		UF_CHECK(p2 != p1);
		tsf_note_off(f, 0, 60);
		rel1 = live2 = 0;
		for (i = 0; i < f->voiceNum; i++)
		{
			struct tsf_voice* v = &f->voices[i];
			if (v->playingPreset == -1) continue;
			if (v->playIndex == p1) { UF_CHECK_AT("slot", i, v->ampenv.segment == TSF_SEGMENT_RELEASE); rel1++; }
			else if (v->playIndex == p2) { UF_CHECK_AT("slot", i, v->ampenv.segment < TSF_SEGMENT_RELEASE); live2++; }
		}
		UF_CHECK(rel1 == 2 && live2 == 2);
		tsf_note_off(f, 0, 60);
		UF_CHECK(uf_live(f) == 0);
		tsf_close(f);
	}

	/* Events at the same absolute sample positions give identical output no
	   matter whether rendering is requested in frames of 1, 7, 31 or all at once. */
	{
		enum { N = 128 };
		float outA[N * 4], outB[N * 4], outC[N * 4], outD[N * 4];
		tsf* a = make_font(2, 0);
		tsf* b = make_font(2, 0);
		tsf* c = make_font(2, 0);
		tsf* d = make_font(2, 0);
		UF_CHECK(a && b && c && d);
		tsf_set_output(a, TSF_STEREO_INTERLEAVED, 22050, 0);
		tsf_set_output(b, TSF_STEREO_INTERLEAVED, 22050, 0);
		tsf_set_output(c, TSF_STEREO_INTERLEAVED, 22050, 0);
		tsf_set_output(d, TSF_STEREO_INTERLEAVED, 22050, 0);
		UF_CHECK(tsf_note_on(a, 0, 60, 1.0f) == 1);
		UF_CHECK(tsf_note_on(b, 0, 60, 1.0f) == 1);
		UF_CHECK(tsf_note_on(c, 0, 60, 1.0f) == 1);
		UF_CHECK(tsf_note_on(d, 0, 60, 1.0f) == 1);
		uf_render_chunks(a, outA, N, 1);
		uf_render_chunks(b, outB, N, 7);
		uf_render_chunks(c, outC, N, 31);
		uf_render_chunks(d, outD, N, N);
		/* second event at the same absolute sample position N on all four */
		UF_CHECK(tsf_note_on(a, 0, 64, 1.0f) == 1);
		UF_CHECK(tsf_note_on(b, 0, 64, 1.0f) == 1);
		UF_CHECK(tsf_note_on(c, 0, 64, 1.0f) == 1);
		UF_CHECK(tsf_note_on(d, 0, 64, 1.0f) == 1);
		uf_render_chunks(a, outA + N * 2, N, 1);
		uf_render_chunks(b, outB + N * 2, N, 7);
		uf_render_chunks(c, outC + N * 2, N, 31);
		uf_render_chunks(d, outD + N * 2, N, N);
		UF_CHECK(memcmp(outA, outB, sizeof outA) == 0);
		UF_CHECK(memcmp(outA, outC, sizeof outA) == 0);
		UF_CHECK(memcmp(outA, outD, sizeof outA) == 0);
		tsf_close(a);
		tsf_close(b);
		tsf_close(c);
		tsf_close(d);
	}

	/* The existing render_short / reset / copy entry points keep working. */
	{
		tsf* f = make_font(2, 7);
		tsf* c;
		short spcm[512 * 2];
		long senergy;
		int i, k;
		UF_CHECK(f != 0);
		tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
		UF_CHECK(tsf_note_on(f, 0, 60, 1.0f) == 1);
		c = tsf_copy(f);
		UF_CHECK(c != 0);
		UF_CHECK(tsf_active_voice_count(c) == 0); /* copies start without voices */
		UF_CHECK(tsf_note_on(c, 0, 60, 1.0f) == 1); /* and can play on their own */
		UF_CHECK(uf_live(c) == 2);
		UF_CHECK(uf_live(f) == 2);
		tsf_render_short(f, spcm, 512, 0);
		senergy = 0;
		for (i = 0; i < 512 * 2; i++) senergy += spcm[i] * spcm[i];
		UF_CHECK(senergy > 0);
		tsf_reset(f);
		for (k = 0; k < 10; k++) tsf_render_float(f, pcm, 512, 0);
		UF_CHECK(tsf_active_voice_count(f) == 0);
		tsf_close(f);
		tsf_close(c);
	}
}

void uf_admission(void)
{
	float pcmA[512 * 2], pcmB[512 * 2];

	/* Failed growth on a fresh instance changes nothing and can be retried. */
	{
		tsf* f = make_font(2, 7);
		UF_CHECK(f != 0);
		tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
		uf_deny = 1;
		UF_CHECK(tsf_note_on(f, 0, 60, 1.0f) == 0);
		uf_deny = 0;
		UF_CHECK(f->voiceNum == 0);
		UF_CHECK(f->voices == 0);
		UF_CHECK(f->voicePlayIndex == 0);
		UF_CHECK(tsf_active_voice_count(f) == 0);
		UF_CHECK(tsf_note_on(f, 0, 60, 1.0f) == 1);
		UF_CHECK(uf_live(f) == 2);
		tsf_close(f);
	}

	/* A failed note-on leaves the playing state bit-identical to a control
	   instance that never received the call. */
	{
		tsf* f = make_font(6, 0); /* class 0: nothing is reusable, growth is required */
		tsf* ctrl = make_font(6, 0);
		int i, k, voiceNumBefore;
		unsigned playIndexBefore;
		UF_CHECK(f && ctrl);
		tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
		tsf_set_output(ctrl, TSF_STEREO_INTERLEAVED, 22050, 0);
		UF_CHECK(tsf_note_on(f, 0, 60, 1.0f) == 1);
		UF_CHECK(tsf_note_on(ctrl, 0, 60, 1.0f) == 1);
		tsf_render_float(f, pcmA, 100, 0);
		tsf_render_float(ctrl, pcmB, 100, 0);
		voiceNumBefore = f->voiceNum;
		playIndexBefore = f->voicePlayIndex;
		uf_deny = 1;
		UF_CHECK(tsf_note_on(f, 0, 60, 1.0f) == 0);
		uf_deny = 0;
		UF_CHECK(f->voiceNum == voiceNumBefore);
		UF_CHECK(f->voicePlayIndex == playIndexBefore);
		UF_CHECK(uf_live(f) == 6 && uf_releasing(f) == 0);
		/* envelopes and sample positions of the existing voices are untouched */
		for (i = 0; i < f->voiceNum; i++)
		{
			if (f->voices[i].playingPreset == -1) continue;
			UF_CHECK_AT("slot", i, f->voices[i].sourceSamplePosition == ctrl->voices[i].sourceSamplePosition);
			UF_CHECK_AT("slot", i, f->voices[i].ampenv.segment == ctrl->voices[i].ampenv.segment);
			UF_CHECK_AT("slot", i, f->voices[i].ampenv.level == ctrl->voices[i].ampenv.level);
			UF_CHECK_AT("slot", i, f->voices[i].playIndex == ctrl->voices[i].playIndex);
		}
		/* rendering side by side with the control stays sample-identical */
		for (k = 0; k < 8; k++)
		{
			tsf_render_float(f, pcmA, 512, 0);
			tsf_render_float(ctrl, pcmB, 512, 0);
			UF_CHECK(memcmp(pcmA, pcmB, sizeof pcmA) == 0);
		}
		/* retry with allocation allowed plays the whole group */
		UF_CHECK(tsf_note_on(f, 0, 60, 1.0f) == 1);
		UF_CHECK(uf_live(f) == 12);
		tsf_close(f);
		tsf_close(ctrl);
	}

	/* Fixed pool: exact fit, then one group too many fails atomically and
	   the note-on hot path performs no allocation at all. */
	{
		tsf* f = make_font(2, 0);
		unsigned playIndexBefore;
		UF_CHECK(f != 0);
		tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
		UF_CHECK(tsf_set_max_voices(f, 4) == 1);
		uf_alloc_calls = 0;
		UF_CHECK(tsf_note_on(f, 0, 60, 1.0f) == 1);
		UF_CHECK(tsf_note_on(f, 0, 61, 1.0f) == 1); /* exact fit */
		UF_CHECK(uf_live(f) == 4);
		playIndexBefore = f->voicePlayIndex;
		UF_CHECK(tsf_note_on(f, 0, 62, 1.0f) == 0); /* no slot left for the whole group */
		UF_CHECK(uf_alloc_calls == 0);
		UF_CHECK(f->voicePlayIndex == playIndexBefore);
		UF_CHECK(uf_live(f) == 4 && uf_releasing(f) == 0);
		tsf_close(f);
	}

	/* Fixed pool one slot short of the whole group: fails without changes. */
	{
		tsf* f = make_font(2, 0);
		unsigned playIndexBefore;
		UF_CHECK(f != 0);
		tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
		UF_CHECK(tsf_set_max_voices(f, 3) == 1);
		uf_alloc_calls = 0;
		UF_CHECK(tsf_note_on(f, 0, 60, 1.0f) == 1);
		playIndexBefore = f->voicePlayIndex;
		UF_CHECK(tsf_note_on(f, 0, 61, 1.0f) == 0); /* 1 free slot, group needs 2 */
		UF_CHECK(uf_alloc_calls == 0);
		UF_CHECK(f->voicePlayIndex == playIndexBefore);
		UF_CHECK(uf_live(f) == 2 && uf_releasing(f) == 0);
		tsf_close(f);
	}

	/* Fixed pool: slots of the old group that this note-on cuts anyway count
	   toward admission and are taken over. */
	{
		tsf* f = make_font(2, 7);
		unsigned p1;
		int i;
		UF_CHECK(f != 0);
		tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
		UF_CHECK(tsf_set_max_voices(f, 2) == 1);
		uf_alloc_calls = 0;
		UF_CHECK(tsf_note_on(f, 0, 60, 1.0f) == 1);
		p1 = f->voices[0].playIndex;
		UF_CHECK(tsf_note_on(f, 0, 60, 1.0f) == 1);
		UF_CHECK(uf_alloc_calls == 0);
		UF_CHECK(uf_live(f) == 2);
		UF_CHECK(uf_releasing(f) == 0);
		for (i = 0; i < f->voiceNum; i++)
			UF_CHECK_AT("slot", i, f->voices[i].playIndex != p1);
		tsf_close(f);
	}

	/* Fixed pool: release candidates are reused, the one furthest into its
	   release first. */
	{
		tsf* f = make_font(1, 0);
		UF_CHECK(f != 0);
		tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
		UF_CHECK(tsf_set_max_voices(f, 3) == 1);
		uf_alloc_calls = 0;
		UF_CHECK(tsf_note_on(f, 0, 60, 1.0f) == 1); /* slot 0 */
		tsf_render_float(f, pcmA, 300, 0);
		tsf_note_off(f, 0, 60);
		UF_CHECK(tsf_note_on(f, 0, 61, 1.0f) == 1); /* slot 1 */
		tsf_render_float(f, pcmA, 100, 0);
		tsf_note_off(f, 0, 61);
		UF_CHECK(tsf_note_on(f, 0, 62, 1.0f) == 1); /* free slot 2 first */
		UF_CHECK(f->voices[2].playingKey == 62);
		UF_CHECK(tsf_note_on(f, 0, 63, 1.0f) == 1); /* slot 0 is furthest into release */
		UF_CHECK(uf_alloc_calls == 0);
		UF_CHECK(f->voices[0].playingKey == 63);
		UF_CHECK(f->voices[1].playingKey == 61 && f->voices[1].ampenv.segment == TSF_SEGMENT_RELEASE);
		tsf_close(f);
	}

	/* Tied release candidates are taken in slot order. */
	{
		tsf* f = make_font(2, 0);
		UF_CHECK(f != 0);
		tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
		UF_CHECK(tsf_set_max_voices(f, 3) == 1);
		UF_CHECK(tsf_note_on(f, 0, 60, 1.0f) == 1); /* slots 0 and 1 */
		tsf_note_off(f, 0, 60);                     /* both release at the same time */
		tsf_render_float(f, pcmA, 100, 0);
		UF_CHECK(tsf_note_on(f, 0, 61, 1.0f) == 1); /* free slot 2, then tied slot 0 */
		UF_CHECK(f->voices[2].playingKey == 61);
		UF_CHECK(f->voices[0].playingKey == 61);
		UF_CHECK(f->voices[1].playingKey == 60 && f->voices[1].ampenv.segment == TSF_SEGMENT_RELEASE);
		tsf_close(f);
	}

	/* Silent legal calls keep their return conventions and are not reported
	   as memory failures. */
	{
		tsf* f = make_font(2, 7);
		unsigned playIndexBefore;
		UF_CHECK(f != 0);
		tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
		f->presets[0].regions[0].lokey = 61;
		f->presets[0].regions[1].lokey = 61;
		playIndexBefore = f->voicePlayIndex;
		UF_CHECK(tsf_note_on(f, 0, 60, 1.0f) == 1); /* no region matches */
		UF_CHECK(f->voicePlayIndex == playIndexBefore);
		UF_CHECK(tsf_active_voice_count(f) == 0);
		UF_CHECK(tsf_note_on(f, 5, 60, 1.0f) == 1); /* preset index out of range */
		UF_CHECK(tsf_bank_note_on(f, 0, 99, 60, 1.0f) == 0); /* preset does not exist */
		uf_deny = 1;
		UF_CHECK(tsf_note_on(f, 0, 60, 0.0f) == 1); /* zero velocity is a note-off */
		uf_deny = 0;
		UF_CHECK(tsf_bank_note_on(f, 0, 0, 61, 1.0f) == 1); /* bank entry plays */
		UF_CHECK(uf_live(f) == 2);
		tsf_close(f);
	}
}

void uf_channel_audio(void)
{
	float pcm[512 * 2];
	double el, er;

	/* The same preset playing on another channel is outside the exclusion
	   scope; re-triggering one channel only cuts its own old group. */
	{
		tsf* f = make_font(2, 7);
		unsigned p_ch0, p_ch1;
		int i, ch1live;
		UF_CHECK(f != 0);
		tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
		UF_CHECK(tsf_channel_set_bank_preset(f, 0, 0, 0) == 1);
		UF_CHECK(tsf_channel_set_bank_preset(f, 1, 0, 0) == 1);
		UF_CHECK(tsf_channel_note_on(f, 0, 60, 1.0f) == 1);
		UF_CHECK(uf_live(f) == 2);
		p_ch0 = f->voices[0].playIndex;
		UF_CHECK(tsf_channel_note_on(f, 1, 60, 1.0f) == 1);
		UF_CHECK(uf_live(f) == 4); /* channel 0 group not cut by channel 1 */
		UF_CHECK(uf_releasing(f) == 0);
		p_ch1 = f->voices[2].playIndex;
		UF_CHECK(p_ch1 != p_ch0);
		for (i = 0; i < f->voiceNum; i++)
		{
			struct tsf_voice* v = &f->voices[i];
			if (v->playingPreset == -1) continue;
			UF_CHECK_AT("slot", i, v->playingChannel == (i < 2 ? 0 : 1));
		}
		/* both layers of both channels reach the stereo output */
		tsf_render_float(f, pcm, 512, 0);
		uf_stereo_energy(pcm, 512, &el, &er);
		UF_CHECK(el > 0.001 && er > 0.001);
		UF_CHECK(uf_channels_differ(pcm, 512));
		/* re-triggering channel 0 cuts only its own old group */
		UF_CHECK(tsf_channel_note_on(f, 0, 60, 1.0f) == 1);
		UF_CHECK(uf_live(f) == 4);
		ch1live = 0;
		for (i = 0; i < f->voiceNum; i++)
		{
			struct tsf_voice* v = &f->voices[i];
			if (v->playingPreset == -1) continue;
			UF_CHECK_AT("slot", i, v->playIndex != p_ch0);
			if (v->playingChannel == 1)
			{
				UF_CHECK_AT("slot", i, v->ampenv.segment < TSF_SEGMENT_RELEASE);
				UF_CHECK_AT("slot", i, v->playIndex == p_ch1);
				ch1live++;
			}
		}
		UF_CHECK(ch1live == 2);
		tsf_close(f);
	}

	/* The sustain pedal cannot turn the quick release of a cut group back
	   into a normal release. */
	{
		tsf* f = make_font(2, 7);
		unsigned p1;
		int i, k;
		UF_CHECK(f != 0);
		tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
		UF_CHECK(tsf_channel_set_bank_preset(f, 0, 0, 0) == 1);
		UF_CHECK(tsf_channel_set_sustain(f, 0, 1) == 1);
		UF_CHECK(tsf_channel_note_on(f, 0, 60, 1.0f) == 1);
		p1 = f->voices[0].playIndex;
		UF_CHECK(tsf_channel_note_on(f, 0, 60, 1.0f) == 1); /* cuts the sustained group */
		UF_CHECK(uf_live(f) == 2);
		UF_CHECK(uf_releasing(f) == 2);
		for (i = 0; i < f->voiceNum; i++)
		{
			struct tsf_voice* v = &f->voices[i];
			if (v->playingPreset != -1 && v->playIndex == p1)
				UF_CHECK_AT("slot", i, v->ampenv.parameters.release == 0.0f);
		}
		for (k = 0; k < 40; k++) tsf_render_float(f, pcm, 512, 0);
		UF_CHECK(uf_live(f) == 2);                /* new group still sounds */
		UF_CHECK(tsf_active_voice_count(f) == 2); /* cut group is gone for good */
		tsf_close(f);
	}

	/* A channel note-off releases that channel's whole earliest group and
	   leaves the other channel's voices alone. */
	{
		tsf* f = make_font(2, 0);
		int i;
		UF_CHECK(f != 0);
		tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
		UF_CHECK(tsf_channel_set_bank_preset(f, 0, 0, 0) == 1);
		UF_CHECK(tsf_channel_set_bank_preset(f, 1, 0, 0) == 1);
		UF_CHECK(tsf_channel_note_on(f, 0, 60, 1.0f) == 1);
		UF_CHECK(tsf_channel_note_on(f, 1, 60, 1.0f) == 1);
		UF_CHECK(uf_live(f) == 4);
		tsf_channel_note_off(f, 0, 60);
		for (i = 0; i < f->voiceNum; i++)
		{
			struct tsf_voice* v = &f->voices[i];
			if (v->playingPreset == -1) continue;
			if (v->playingChannel == 0)
				UF_CHECK_AT("slot", i, v->ampenv.segment == TSF_SEGMENT_RELEASE);
			else
				UF_CHECK_AT("slot", i, v->ampenv.segment < TSF_SEGMENT_RELEASE);
		}
		tsf_close(f);
	}

	/* The remaining sound-off entry points keep working. */
	{
		tsf* f = make_font(2, 7);
		int k;
		UF_CHECK(f != 0);
		tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
		UF_CHECK(tsf_channel_set_bank_preset(f, 0, 0, 0) == 1);
		UF_CHECK(tsf_channel_note_on(f, 0, 60, 1.0f) == 1);
		UF_CHECK(tsf_channel_note_on(f, 0, 64, 1.0f) == 1);
		tsf_channel_sounds_off_all(f, 0);
		for (k = 0; k < 10; k++) tsf_render_float(f, pcm, 512, 0);
		UF_CHECK(tsf_active_voice_count(f) == 0);
		UF_CHECK(tsf_channel_note_on(f, 0, 60, 1.0f) == 1);
		UF_CHECK(uf_live(f) == 2);
		tsf_note_off_all(f);
		UF_CHECK(uf_live(f) == 0);
		tsf_close(f);
	}
}
