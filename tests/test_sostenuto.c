/* Test for the sostenuto pedal (CC66) support in tsf.h.
   Builds a minimal SoundFont in memory (no external files) with
     - preset 0: two regions on the same key (one note-on spawns 2 voices)
     - preset 1: one region in an exclusive class (quick kill on retrigger)
   and verifies voice lifecycles and PCM energy through the public API.
   Compiles as both C99 and C++11. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TSF_IMPLEMENTATION
#include "../tsf.h"

#define SAMPLE_RATE 44100
#define KEY_A 60
#define KEY_B 64
#define KEY_C 67

/* ------------------------------------------------------------------ */
/* Minimal in-memory SF2 builder                                       */
/* ------------------------------------------------------------------ */

static unsigned char* g_p;
static void put_raw(const void* d, size_t n) { memcpy(g_p, d, n); g_p += n; }
static void put_tag(const char* t) { memcpy(g_p, t, 4); g_p += 4; }
static void put16(int v) { g_p[0] = (unsigned char)(v & 255); g_p[1] = (unsigned char)((v >> 8) & 255); g_p += 2; }
static void put32(unsigned long v) { g_p[0] = (unsigned char)(v & 255); g_p[1] = (unsigned char)((v >> 8) & 255); g_p[2] = (unsigned char)((v >> 16) & 255); g_p[3] = (unsigned char)((v >> 24) & 255); g_p += 4; }
static void put_name(const char* s) { char n[20]; memset(n, 0, 20); strncpy(n, s, 19); put_raw(n, 20); }
static unsigned char* chunk_begin(const char* id) { unsigned char* sz; put_tag(id); sz = g_p; g_p += 4; return sz; }
static void chunk_end(unsigned char* sz) { unsigned long n = (unsigned long)(g_p - sz - 4); sz[0] = (unsigned char)(n & 255); sz[1] = (unsigned char)((n >> 8) & 255); sz[2] = (unsigned char)((n >> 16) & 255); sz[3] = (unsigned char)((n >> 24) & 255); }

static void put_phdr(const char* name, int preset, int bank, int bag)
{
	put_name(name); put16(preset); put16(bank); put16(bag); put32(0); put32(0); put32(0);
}
static void put_bag(int gen, int mod) { put16(gen); put16(mod); }
static void put_gen(int oper, int amount) { put16(oper); put16(amount); }
static void put_mod10(void) { char z[10]; memset(z, 0, 10); put_raw(z, 10); }
static void put_inst(const char* name, int bag) { put_name(name); put16(bag); }
static void put_shdr(const char* name, unsigned long start, unsigned long end, unsigned long ls, unsigned long le, unsigned long rate, int pitch, int corr, int link, int type)
{
	put_name(name); put32(start); put32(end); put32(ls); put32(le); put32(rate);
	g_p[0] = (unsigned char)pitch; g_p += 1;
	g_p[0] = (unsigned char)(signed char)corr; g_p += 1;
	put16(link); put16(type);
}

static int build_sf2(unsigned char* out)
{
	unsigned char *riffsize, *listsize;
	short pcm[512];
	int i;
	for (i = 0; i < 512; i++) pcm[i] = (short)((i % 32) < 16 ? 10000 : -10000);

	g_p = out;
	put_tag("RIFF"); riffsize = g_p; g_p += 4; put_tag("sfbk");

	/* sdta */
	put_tag("LIST"); listsize = g_p; g_p += 4; put_tag("sdta");
	put_tag("smpl"); put32(sizeof(pcm)); put_raw(pcm, sizeof(pcm));
	chunk_end(listsize);

	/* pdta */
	put_tag("LIST"); listsize = g_p; g_p += 4; put_tag("pdta");

	put_tag("phdr"); put32(3 * 38);
	put_phdr("sostest", 0, 0, 0);   /* preset 0: two regions per key */
	put_phdr("sostexcl", 1, 0, 1);  /* preset 1: exclusive class region */
	put_phdr("EOP", 0, 0, 2);

	put_tag("pbag"); put32(3 * 4);
	put_bag(0, 0); /* preset 0 zone -> pgen 0 */
	put_bag(1, 0); /* preset 1 zone -> pgen 1 */
	put_bag(2, 0); /* terminal */

	put_tag("pmod"); put32(10); put_mod10();

	put_tag("pgen"); put32(3 * 4);
	put_gen(41, 0); /* instrument 0 */
	put_gen(41, 1); /* instrument 1 */
	put_gen(0, 0);  /* terminal */

	put_tag("inst"); put32(3 * 22);
	put_inst("instmain", 0);
	put_inst("instexcl", 3);
	put_inst("EOI", 5);

	put_tag("ibag"); put32(6 * 4);
	put_bag(0, 0);  /* inst0 global: igen 0..1 */
	put_bag(2, 0);  /* inst0 zone1: igen 2..3 */
	put_bag(4, 0);  /* inst0 zone2: igen 4..5 */
	put_bag(6, 0);  /* inst1 global: igen 6..7 */
	put_bag(8, 0);  /* inst1 zone:   igen 8..9 */
	put_bag(10, 0); /* terminal */

	put_tag("imod"); put32(10); put_mod10();

	put_tag("igen"); put32(11 * 4);
	put_gen(54, 1);      /* SampleModes: loop continuously */
	put_gen(38, -2400);  /* VolEnvRelease: 0.25s */
	put_gen(17, -200);   /* Pan -0.2 */
	put_gen(53, 0);      /* SampleID 0 */
	put_gen(17, 200);    /* Pan +0.2 */
	put_gen(53, 0);      /* SampleID 0 */
	put_gen(54, 1);      /* SampleModes: loop continuously */
	put_gen(38, -2400);  /* VolEnvRelease: 0.25s */
	put_gen(57, 7);      /* ExclusiveClass 7 */
	put_gen(53, 0);      /* SampleID 0 */
	put_gen(0, 0);       /* terminal */

	put_tag("shdr"); put32(2 * 46);
	put_shdr("square", 0, 511, 0, 512, SAMPLE_RATE, 60, 0, 0, 1);
	put_shdr("EOS", 0, 0, 0, 0, 0, 0, 0, 0, 0);
	chunk_end(listsize);

	chunk_end(riffsize);
	return (int)(g_p - out);
}

/* ------------------------------------------------------------------ */
/* Test helpers                                                        */
/* ------------------------------------------------------------------ */

static unsigned char g_sf2[16384];
static int g_sf2size;
static float g_buf[65536];
static int g_failures = 0;
static int g_checks = 0;

static tsf* new_font(void)
{
	tsf* f = tsf_load_memory(g_sf2, g_sf2size);
	if (!f) { printf("FATAL: tsf_load_memory failed\n"); exit(1); }
	tsf_set_output(f, TSF_MONO, SAMPLE_RATE, 0.0f);
	return f;
}

/* Render mono samples and return the mean absolute PCM energy. */
static double render_energy(tsf* f, int samples)
{
	double sum = 0.0;
	int i;
	if (samples > (int)(sizeof(g_buf) / sizeof(g_buf[0]))) { printf("FATAL: render buffer too small\n"); exit(1); }
	tsf_render_float(f, g_buf, samples, 0);
	for (i = 0; i < samples; i++) sum += fabs((double)g_buf[i]);
	return sum / samples;
}

static void check(const char* name, int cond, tsf* f, double energy)
{
	g_checks++;
	printf("  %-58s voices=%2d energy=%9.6f  %s\n", name, tsf_active_voice_count(f), energy, cond ? "OK" : "FAIL");
	if (!cond) g_failures++;
}

/* Render past the 0.25s release envelope, then require full silence. */
static void expect_silence(tsf* f, const char* name)
{
	double e;
	render_energy(f, 20000);
	e = render_energy(f, 4096);
	check(name, tsf_active_voice_count(f) == 0 && e < 1e-9, f, e);
}

static void expect_sound(tsf* f, const char* name, int voices)
{
	double e = render_energy(f, 512);
	check(name, tsf_active_voice_count(f) == voices && e > 0.02, f, e);
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

/* A down, CC66 on, B down, release A then B: only A is locked.
   Then hand A over from CC66 to CC64, finally release CC64. */
static void test_sequence_a(void)
{
	tsf* f = new_font();
	printf("T1: sostenuto snapshot then handoff CC66 -> CC64\n");
	tsf_channel_set_presetindex(f, 0, 0);
	tsf_channel_note_on(f, 0, KEY_A, 1.0f);
	expect_sound(f, "A down -> 2 voices (double region)", 2);
	if (tsf_channel_set_sostenuto(f, 0, 1) != 1) check("set_sostenuto returns 1", 0, f, 0);
	tsf_channel_note_on(f, 0, KEY_B, 1.0f);
	expect_sound(f, "B down after pedal -> 4 voices", 4);
	tsf_channel_note_off(f, 0, KEY_A);
	expect_sound(f, "A up -> A locked by CC66, still 4 voices", 4);
	tsf_channel_note_off(f, 0, KEY_B);
	expect_sound(f, "B up -> B enters release, voices still counted", 4);
	render_energy(f, 20000);
	expect_sound(f, "B released to silence, A locked -> 2 voices", 2);
	tsf_channel_set_sustain(f, 0, 1);
	tsf_channel_set_sostenuto(f, 0, 1); /* repeat: no re-snapshot, no-op */
	tsf_channel_set_sostenuto(f, 0, 0);
	expect_sound(f, "CC66 off with CC64 on -> A handed to sustain", 2);
	tsf_channel_set_sustain(f, 0, 0);
	expect_silence(f, "CC64 off -> A releases");
	tsf_close(f);
}

/* A down, CC64 on, A up, CC66 on, CC64 off: CC66 must not catch the
   already-released A, so A releases when CC64 goes off. */
static void test_sequence_b(void)
{
	tsf* f = new_font();
	printf("T2: CC66 must not lock an already released note\n");
	tsf_channel_set_presetindex(f, 0, 0);
	tsf_channel_note_on(f, 0, KEY_A, 1.0f);
	tsf_channel_set_sustain(f, 0, 1);
	tsf_channel_note_off(f, 0, KEY_A);
	expect_sound(f, "A held by CC64", 2);
	tsf_channel_set_sostenuto(f, 0, 1);
	expect_sound(f, "CC66 on -> no new lock, A still held by CC64 only", 2);
	tsf_channel_set_sustain(f, 0, 0);
	expect_silence(f, "CC64 off -> A releases (CC66 did not catch it)");
	tsf_channel_set_sostenuto(f, 0, 0);
	tsf_close(f);
}

/* Both pedals hold a released note; lifting one must not kill it. */
static void test_double_pedal(void)
{
	tsf* f;
	printf("T3: both pedals, either release order\n");

	f = new_font();
	tsf_channel_set_presetindex(f, 0, 0);
	tsf_channel_note_on(f, 0, KEY_A, 1.0f);
	tsf_channel_set_sustain(f, 0, 1);
	tsf_channel_set_sostenuto(f, 0, 1);
	tsf_channel_note_off(f, 0, KEY_A);
	tsf_channel_set_sostenuto(f, 0, 0);
	expect_sound(f, "CC66 off first -> CC64 still holds A", 2);
	tsf_channel_set_sustain(f, 0, 0);
	expect_silence(f, "CC64 off -> A releases");
	tsf_close(f);

	f = new_font();
	tsf_channel_set_presetindex(f, 0, 0);
	tsf_channel_note_on(f, 0, KEY_A, 1.0f);
	tsf_channel_set_sustain(f, 0, 1);
	tsf_channel_set_sostenuto(f, 0, 1);
	tsf_channel_note_off(f, 0, KEY_A);
	tsf_channel_set_sustain(f, 0, 0);
	expect_sound(f, "CC64 off first -> CC66 still holds A", 2);
	tsf_channel_set_sostenuto(f, 0, 0);
	expect_silence(f, "CC66 off -> A releases");
	tsf_close(f);
}

/* Repeated CC66-on must not take a new snapshot; CC66 via MIDI CC with
   64 as the on/off threshold. */
static void test_repeat_and_threshold(void)
{
	tsf* f = new_font();
	printf("T4: repeated CC66 on + CC64 threshold via midi_control\n");
	tsf_channel_set_presetindex(f, 0, 0);
	tsf_channel_note_on(f, 0, KEY_A, 1.0f);
	if (tsf_channel_midi_control(f, 0, 66, 64) != 1) check("midi_control returns 1", 0, f, 0);
	tsf_channel_note_off(f, 0, KEY_A); /* A locked */
	tsf_channel_note_on(f, 0, KEY_B, 1.0f);
	tsf_channel_midi_control(f, 0, 66, 127); /* repeat: no re-snapshot */
	tsf_channel_midi_control(f, 0, 66, 100); /* repeat: no re-snapshot */
	tsf_channel_note_off(f, 0, KEY_B); /* B must NOT be locked */
	render_energy(f, 20000);
	expect_sound(f, "B released, only A locked -> 2 voices", 2);
	tsf_channel_midi_control(f, 0, 66, 63); /* below threshold: pedal off */
	expect_silence(f, "CC66 value 63 -> pedal off -> A releases");
	tsf_close(f);
}

/* Same key restruck while the first press is locked: the next note-off
   goes to the newest still-held group and must not be swallowed. */
static void test_same_key_retrigger(void)
{
	tsf* f = new_font();
	printf("T5: same key retrigger while locked\n");
	tsf_channel_set_presetindex(f, 0, 0);
	tsf_channel_note_on(f, 0, KEY_A, 1.0f);
	tsf_channel_set_sostenuto(f, 0, 1);
	tsf_channel_note_off(f, 0, KEY_A); /* first A locked */
	tsf_channel_note_on(f, 0, KEY_A, 1.0f);
	expect_sound(f, "A restruck -> 4 voices", 4);
	tsf_channel_note_off(f, 0, KEY_A); /* ends second A, first stays locked */
	render_energy(f, 20000);
	expect_sound(f, "second A released, first A locked -> 2 voices", 2);
	tsf_channel_set_sostenuto(f, 0, 0);
	expect_silence(f, "CC66 off -> first A releases");
	tsf_close(f);
}

/* Two channels playing the same key must not share pedal state. */
static void test_channel_isolation(void)
{
	tsf* f = new_font();
	double e;
	printf("T6: channel isolation\n");
	tsf_channel_set_presetindex(f, 0, 0);
	tsf_channel_set_presetindex(f, 1, 0);
	tsf_channel_note_on(f, 0, KEY_A, 1.0f);
	tsf_channel_set_sostenuto(f, 0, 1);
	tsf_channel_note_off(f, 0, KEY_A); /* locked on channel 0 */
	tsf_channel_note_on(f, 1, KEY_A, 1.0f);
	expect_sound(f, "same key on ch1 -> 4 voices", 4);
	tsf_channel_note_off(f, 1, KEY_A); /* releases ch1 only */
	render_energy(f, 20000);
	expect_sound(f, "ch1 A released, ch0 A locked -> 2 voices", 2);
	tsf_channel_set_sostenuto(f, 1, 1); /* ch1 snapshot: nothing held there */
	tsf_channel_set_sostenuto(f, 1, 0); /* ch1 pedal off must not touch ch0 */
	e = render_energy(f, 512);
	check("ch1 pedal off leaves ch0 lock intact", tsf_active_voice_count(f) == 2 && e > 0.02, f, e);
	tsf_channel_set_sostenuto(f, 0, 0);
	expect_silence(f, "ch0 CC66 off -> A releases");
	tsf_close(f);
}

/* Voice slot reuse under a max polyphony limit must not leak the old
   snapshot into new voices. */
static void test_slot_reuse(void)
{
	tsf* f = new_font();
	printf("T7: slot reuse with max voices\n");
	tsf_set_max_voices(f, 4);
	tsf_channel_set_presetindex(f, 0, 0);
	tsf_channel_note_on(f, 0, KEY_A, 1.0f);
	tsf_channel_set_sostenuto(f, 0, 1);
	tsf_channel_note_off(f, 0, KEY_A); /* A locked: 2 voices */
	tsf_channel_note_on(f, 0, KEY_B, 1.0f);
	tsf_channel_note_off(f, 0, KEY_B); /* B releasing: 2 voices */
	tsf_channel_note_on(f, 0, KEY_C, 1.0f); /* steals B's releasing slots */
	expect_sound(f, "C steals B's slots -> A(2) + C(2) = 4 voices", 4);
	tsf_channel_set_sostenuto(f, 0, 0); /* ends A only; C is still down */
	render_energy(f, 20000);
	expect_sound(f, "CC66 off -> A releases, C (still down) unaffected", 2);
	tsf_channel_note_off(f, 0, KEY_C);
	expect_silence(f, "C up -> C releases");
	tsf_close(f);
}

/* Exclusive class quick kill of a locked voice; the slot must come back
   clean for later notes. */
static void test_exclusive_group(void)
{
	tsf* f = new_font();
	printf("T8: exclusive class kill of a locked voice\n");
	tsf_channel_set_presetindex(f, 3, 1);
	tsf_channel_note_on(f, 3, KEY_A, 1.0f);
	tsf_channel_set_sostenuto(f, 3, 1);
	tsf_channel_note_off(f, 3, KEY_A); /* A locked */
	expect_sound(f, "A locked (exclusive preset, 1 region)", 1);
	tsf_channel_note_on(f, 3, KEY_B, 1.0f); /* exclusive kill on A */
	render_energy(f, 20000);
	expect_sound(f, "B down, A quick-killed -> 1 voice", 1);
	tsf_channel_set_sostenuto(f, 3, 0); /* must not end B (key still down) */
	expect_sound(f, "CC66 off -> B still held by its key", 1);
	tsf_channel_note_off(f, 3, KEY_B);
	expect_silence(f, "B up -> B releases");
	tsf_close(f);
}

/* CC121 lifts both pedals (releasing held notes) but keeps held keys
   sounding; CC123 and note_off_all override the pedals into release;
   CC120 does a quick release. */
static void test_cc_resets(void)
{
	tsf* f;
	printf("T9: CC121 / CC123 / note_off_all / CC120\n");

	f = new_font();
	tsf_channel_set_presetindex(f, 0, 0);
	tsf_channel_note_on(f, 0, KEY_A, 1.0f);
	tsf_channel_set_sostenuto(f, 0, 1);
	tsf_channel_note_off(f, 0, KEY_A); /* A locked */
	tsf_channel_note_on(f, 0, KEY_B, 1.0f); /* B key still down */
	tsf_channel_set_sustain(f, 0, 1);
	tsf_channel_midi_control(f, 0, 121, 0); /* both pedals off */
	render_energy(f, 20000);
	expect_sound(f, "CC121 -> A released, B (key down) keeps sounding", 2);
	tsf_channel_note_off(f, 0, KEY_B);
	expect_silence(f, "B up after CC121 -> normal release");
	tsf_close(f);

	f = new_font();
	tsf_channel_set_presetindex(f, 0, 0);
	tsf_channel_note_on(f, 0, KEY_A, 1.0f);
	tsf_channel_set_sostenuto(f, 0, 1);
	tsf_channel_note_off(f, 0, KEY_A);
	tsf_channel_midi_control(f, 0, 123, 0); /* all notes off overrides pedal */
	expect_silence(f, "CC123 overrides CC66 -> A releases");
	tsf_close(f);

	f = new_font();
	tsf_channel_set_presetindex(f, 0, 0);
	tsf_channel_note_on(f, 0, KEY_A, 1.0f);
	tsf_channel_set_sostenuto(f, 0, 1);
	tsf_channel_note_off(f, 0, KEY_A);
	tsf_channel_note_off_all(f, 0);
	expect_silence(f, "note_off_all overrides CC66 -> A releases");
	tsf_close(f);

	f = new_font();
	tsf_channel_set_presetindex(f, 0, 0);
	tsf_channel_note_on(f, 0, KEY_A, 1.0f);
	tsf_channel_set_sostenuto(f, 0, 1);
	tsf_channel_note_off(f, 0, KEY_A);
	tsf_channel_midi_control(f, 0, 120, 0); /* all sound off: quick release */
	expect_silence(f, "CC120 quick release -> silence");
	tsf_close(f);
}

/* tsf_reset drops voices and channel state; a copied instance is
   independent of the original. */
static void test_reset_and_copy(void)
{
	tsf* f = new_font();
	tsf* g;
	double e;
	printf("T10: tsf_reset and tsf_copy\n");
	tsf_channel_set_presetindex(f, 0, 0);
	tsf_channel_note_on(f, 0, KEY_A, 1.0f);
	tsf_channel_set_sostenuto(f, 0, 1);
	tsf_channel_note_off(f, 0, KEY_A);
	tsf_reset(f);
	expect_silence(f, "tsf_reset -> locked voice gone");
	tsf_channel_set_presetindex(f, 0, 0); /* channels were freed by reset */
	tsf_channel_note_on(f, 0, KEY_A, 1.0f);
	expect_sound(f, "playable again after reset", 2);
	tsf_channel_note_off(f, 0, KEY_A);
	expect_silence(f, "no stale pedal after reset -> A releases");

	g = tsf_copy(f);
	tsf_channel_set_presetindex(g, 0, 0);
	tsf_channel_note_on(g, 0, KEY_A, 1.0f);
	tsf_channel_set_sostenuto(g, 0, 1);
	tsf_channel_note_off(g, 0, KEY_A); /* locked on the copy */
	e = render_energy(g, 512);
	check("copy holds its own locked note", tsf_active_voice_count(g) == 2 && e > 0.02, g, e);
	e = render_energy(f, 512);
	check("original unaffected by copy's pedal", tsf_active_voice_count(f) == 0 && e < 1e-9, f, e);
	tsf_channel_set_sostenuto(g, 0, 0);
	expect_silence(g, "copy CC66 off -> A releases");
	tsf_close(g);
	tsf_close(f);
}

/* Direct preset note_on/note_off (no channels) must be unchanged. */
static void test_direct_preset(void)
{
	tsf* f = new_font();
	printf("T11: direct preset note_on/note_off unchanged\n");
	tsf_note_on(f, 0, KEY_A, 1.0f);
	expect_sound(f, "direct note_on -> 2 voices", 2);
	tsf_note_off(f, 0, KEY_A);
	expect_silence(f, "direct note_off -> release");
	tsf_close(f);
}

/* Events at the same sample positions must render identically no matter
   how the render calls are chunked (in units of the effect sample block,
   which is the library's envelope granularity). */
static void test_render_chunking(void)
{
	static float buf1[40000], buf2[40000];
	const int steps[] = { 1024, 768, 512, 3072, 20032, 5120 };
	int nsteps = (int)(sizeof(steps) / sizeof(steps[0]));
	int i, pos = 0, total = 0, identical;
	tsf* f1 = new_font();
	tsf* f2 = new_font();
	printf("T12: render chunking invariance\n");
	tsf_channel_set_presetindex(f1, 0, 0);
	tsf_channel_set_presetindex(f2, 0, 0);
	for (i = 0; i < nsteps; i++)
	{
		int n = steps[i], done;
		switch (i)
		{
			case 0: tsf_channel_note_on(f1, 0, KEY_A, 1.0f); tsf_channel_note_on(f2, 0, KEY_A, 1.0f); break;
			case 1: tsf_channel_set_sostenuto(f1, 0, 1); tsf_channel_set_sostenuto(f2, 0, 1);
			        tsf_channel_note_on(f1, 0, KEY_B, 1.0f); tsf_channel_note_on(f2, 0, KEY_B, 1.0f); break;
			case 2: tsf_channel_note_off(f1, 0, KEY_A); tsf_channel_note_off(f2, 0, KEY_A); break;
			case 3: tsf_channel_note_off(f1, 0, KEY_B); tsf_channel_note_off(f2, 0, KEY_B); break;
			case 4: tsf_channel_set_sostenuto(f1, 0, 0); tsf_channel_set_sostenuto(f2, 0, 0); break;
			default: break;
		}
		tsf_render_float(f1, buf1 + pos, n, 0);
		for (done = 0; done < n;)
		{
			int c = (n - done > 64 ? 64 : n - done);
			tsf_render_float(f2, buf2 + pos + done, c, 0);
			done += c;
		}
		pos += n;
	}
	total = pos;
	identical = (memcmp(buf1, buf2, (size_t)total * sizeof(float)) == 0);
	check("chunked (64) and single-call renders are identical", identical, f1, 0);
	check("both end silent", tsf_active_voice_count(f1) == 0 && tsf_active_voice_count(f2) == 0, f2, 0);
	tsf_close(f1);
	tsf_close(f2);
}

int main(void)
{
	g_sf2size = build_sf2(g_sf2);
	if (g_sf2size <= 0 || g_sf2size > (int)sizeof(g_sf2)) { printf("FATAL: sf2 build\n"); return 1; }

	test_sequence_a();
	test_sequence_b();
	test_double_pedal();
	test_repeat_and_threshold();
	test_same_key_retrigger();
	test_channel_isolation();
	test_slot_reuse();
	test_exclusive_group();
	test_cc_resets();
	test_reset_and_copy();
	test_direct_preset();
	test_render_chunking();

	printf("%s (%d checks, %d failures)\n", (g_failures ? "SOSTENUTO TESTS FAILED" : "SOSTENUTO TESTS PASSED"), g_checks, g_failures);
	return (g_failures ? 1 : 0);
}
