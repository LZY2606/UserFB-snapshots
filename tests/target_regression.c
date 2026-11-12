/*
 * tests/target_regression.c
 *
 * Regression tests for the emu2413 rhythm section: the noise and phase
 * relations shared by hi-hat, snare and cymbal. The production source is
 * included directly so the static synthesis kernel is covered; the fixed
 * script (checks/target.sh) therefore does not link emu2413.c again.
 *
 * All expected values come from independent references maintained by this
 * file: a software copy of the 23-bit noise register, the documented five
 * phase bits of the short-noise network, and a quantization reference built
 * from the mathematical definitions (sin, log2, exp2). Expected values are
 * never derived from the production sin/exp tables nor from to_linear() /
 * lookup_exp_table(). Reading the live object state is only used to place
 * controlled inputs and to locate a failure.
 *
 * Groups (wired up by checks/target_main.c):
 *   uf_noise_clock - long-noise schedule (14/2/2 clocks between the drums),
 *                    exhaustive short-noise bit combinations, and which
 *                    beat's phase feeds one internal sample.
 *   uf_percussion  - table conventions and integer-exact percussion output
 *                    at controlled phases and attenuations.
 *   uf_render      - end-to-end rendering through the public C API.
 */
#include "harness.h"
#include "emu2413.c"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* failure reporting: always the FIRST differing sample/case, part and  */
/* the conflicting values.                                              */
/* ------------------------------------------------------------------ */
static void uf_expect_eq(const char *group, long sample, const char *part, long actual, long expected) {
  ++uf_assertions;
  if (actual != expected) {
    fprintf(stderr, "FIRST-DIFF group=%s sample=%ld part=%s actual=%ld expected=%ld\n", group, sample, part,
            actual, expected);
    exit(2);
  }
}

/* ------------------------------------------------------------------ */
/* independent references                                               */
/* ------------------------------------------------------------------ */

/* 23-bit noise register, clocked `cycles` times. */
static uint32_t ref_noise_clock(uint32_t n, int cycles) {
  int i;
  for (i = 0; i < cycles; i++) {
    if (n & 1)
      n ^= 0x800200u;
    n >>= 1;
  }
  return n;
}

/* short noise from the five phase bits: hh pg_out bits 2, 7, 3 and cym
 * pg_out bits 3, 5, combined as (h2^h7)|(h3^c5)|(c3^c5). */
static uint8_t ref_short_noise(uint32_t pg_hh, uint32_t pg_cym) {
  const uint32_t h2 = (pg_hh >> 2) & 1, h7 = (pg_hh >> 7) & 1, h3 = (pg_hh >> 3) & 1;
  const uint32_t c3 = (pg_cym >> 3) & 1, c5 = (pg_cym >> 5) & 1;
  return (uint8_t)((h2 ^ h7) | (h3 ^ c5) | (c3 ^ c5));
}

/* exp table entry from its definition: round((2^(x/256) - 1) * 1024) */
static uint16_t ref_exp_table(int x) { return (uint16_t)llround((exp2((double)x / 256.0) - 1.0) * 1024.0); }

/* log-sin table entry from its definition:
 *   quarter wave: round(-log2(sin((x + 0.5) * pi / 512)) * 256)  (half-cell sampling)
 *   second quadrant mirrored, negative half wave flagged with 0x8000. */
static uint16_t ref_fullsin(int x) {
  if (x < 256)
    return (uint16_t)llround(-log2(sin((x + 0.5) * _PI_ / 512.0)) * 256.0);
  if (x < 512)
    return ref_fullsin(511 - x);
  return (uint16_t)(ref_fullsin(x - 512) | 0x8000);
}

/* amplitude quantization rebuilt from the references above: integer
 * attenuation steps, ones-complement for the negative half wave and the
 * final <<1 shift. */
static int16_t ref_to_linear(uint16_t h, uint32_t eg_out, uint32_t tll, uint32_t am) {
  uint32_t att;
  uint16_t i;
  int16_t t, res;
  if (eg_out > EG_MAX)
    return 0;
  att = eg_out + tll + am;
  if (att > EG_MUTE)
    att = EG_MUTE;
  i = (uint16_t)(h + (uint16_t)(att << 4));
  t = (int16_t)(ref_exp_table((i & 0xff) ^ 0xff) + 1024);
  res = (int16_t)(t >> ((i & 0x7f00) >> 8));
  return (int16_t)(((i & 0x8000) ? ~res : res) << 1);
}

/* Percussion assertion with diagnostics that separate a wrong waveform
 * phase selection from a wrong amplitude quantization: if the actual value
 * matches the reference of one of the OTHER candidate phases at the same
 * attenuation, the phase select is at fault, otherwise the amplitude is. */
static void uf_perc_expect(const char *part, long casenum, int16_t actual, int16_t expected,
                           const uint32_t *phases, int nphases, uint32_t chosen, uint32_t eg_out, uint32_t tll) {
  int i;
  ++uf_assertions;
  if (actual == expected)
    return;
  for (i = 0; i < nphases; i++) {
    if (phases[i] == chosen)
      continue;
    if (ref_to_linear(ref_fullsin((int)phases[i]), eg_out, tll, 0) == actual) {
      fprintf(stderr,
              "FIRST-DIFF group=percussion sample=%ld part=%s kind=phase-select actual=%d expected=%d "
              "actual_matches_phase=0x%x chosen_phase=0x%x eg_out=%u tll=%u\n",
              casenum, part, (int)actual, (int)expected, phases[i], chosen, eg_out, tll);
      exit(2);
    }
  }
  fprintf(stderr,
          "FIRST-DIFF group=percussion sample=%ld part=%s kind=amplitude-quant actual=%d expected=%d "
          "chosen_phase=0x%x eg_out=%u tll=%u\n",
          casenum, part, (int)actual, (int)expected, chosen, eg_out, tll);
  exit(2);
}

/* ------------------------------------------------------------------ */
/* shared register programs                                             */
/* ------------------------------------------------------------------ */

/* rhythm mode with all five drums keyed on, fnum/block set so the phases
 * advance, volumes at maximum. */
static void drums_setup(OPLL *o) {
  OPLL_writeReg(o, 0x16, 0x51);
  OPLL_writeReg(o, 0x17, 0x87);
  OPLL_writeReg(o, 0x18, 0xc1);
  OPLL_writeReg(o, 0x26, 0x09);
  OPLL_writeReg(o, 0x27, 0x09);
  OPLL_writeReg(o, 0x28, 0x09);
  OPLL_writeReg(o, 0x36, 0x00);
  OPLL_writeReg(o, 0x37, 0x00);
  OPLL_writeReg(o, 0x38, 0x00);
  OPLL_writeReg(o, 0x0e, 0x3f);
}

/* one melody channel (untouched ROM preset) plus the rhythm section. */
static void render_prog(OPLL *o) {
  OPLL_writeReg(o, 0x30, 0x10); /* ch1: preset 1 (violin), volume 0 */
  OPLL_writeReg(o, 0x10, 0x93);
  OPLL_writeReg(o, 0x20, 0x19); /* key on */
  OPLL_writeReg(o, 0x16, 0x51);
  OPLL_writeReg(o, 0x17, 0x87);
  OPLL_writeReg(o, 0x18, 0xc1);
  OPLL_writeReg(o, 0x26, 0x09);
  OPLL_writeReg(o, 0x27, 0x09);
  OPLL_writeReg(o, 0x28, 0x09);
  OPLL_writeReg(o, 0x36, 0x01);
  OPLL_writeReg(o, 0x37, 0x11);
  OPLL_writeReg(o, 0x38, 0x11);
  OPLL_writeReg(o, 0x0e, 0x3f); /* rhythm mode, all drums keyed */
}

/* ------------------------------------------------------------------ */
/* group 1: noise clock                                                 */
/* ------------------------------------------------------------------ */
void uf_noise_clock(void) {
  /* 1a. Track the long noise register with an independent model over 8192
   * consecutive internal samples, and verify the 14/2/2 shift-clock split
   * between the drum computations of every internal update by replaying the
   * documented schedule on a lockstep instance. */
  {
    OPLL *a = OPLL_new(3600000, 50000);
    OPLL *b = OPLL_new(3600000, 50000);
    static const char *drum_name[5] = {"bd", "hh", "sd", "tom", "cym"};
    uint64_t drum_energy[5] = {0, 0, 0, 0, 0};
    uint32_t model;
    long s;
    int i, k;
    int hh_changes = 0, sd_changes = 0, cym_changes = 0;
    int prev_hh = 0, prev_sd = 0, prev_cym = 0;

    UF_CHECK(a != NULL && b != NULL);
    drums_setup(a);
    drums_setup(b);

    model = 1;
    uf_expect_eq("noise_clock", -1, "noise-seed", (long)a->noise, 1);

    for (s = 0; s < 8192; s++) {
      if (s > 0 && (s % 512) == 0) { /* re-trigger so the drums stay audible */
        OPLL_writeReg(a, 0x0e, 0x20);
        OPLL_writeReg(b, 0x0e, 0x20);
        OPLL_writeReg(a, 0x0e, 0x3f);
        OPLL_writeReg(b, 0x0e, 0x3f);
      }
      uf_expect_eq("noise_clock", s, "noise-entry", (long)a->noise, (long)model);

      update_output(a);

      /* Replay the documented schedule on b: the drum voices are computed
       * after 14, 2 and 2 noise clocks respectively. Any other split inside
       * the real kernel shows up as a ch_out mismatch here. */
      update_ampm(b);
      update_short_noise(b);
      update_slots(b);
      for (i = 0; i < 6; i++)
        b->ch_out[i] = _MO(calc_slot_car(b, i, calc_slot_mod(b, i)));
      b->ch_out[9] = _RO(calc_slot_car(b, 6, calc_slot_mod(b, 6)));
      update_noise(b, 14);
      b->ch_out[10] = _RO(calc_slot_hat(b));
      b->ch_out[11] = _RO(calc_slot_snare(b));
      update_noise(b, 2);
      b->ch_out[12] = _RO(calc_slot_tom(b));
      b->ch_out[13] = _RO(calc_slot_cym(b));
      update_noise(b, 2);

      model = ref_noise_clock(model, 18);
      uf_expect_eq("noise_clock", s, "noise-exit", (long)a->noise, (long)model);
      uf_expect_eq("noise_clock", s, "noise-split", (long)a->noise, (long)b->noise);
      for (k = 0; k < 5; k++) {
        uf_expect_eq("noise_clock", s, drum_name[k], (long)a->ch_out[9 + k], (long)b->ch_out[9 + k]);
        drum_energy[k] += (uint64_t)((int32_t)a->ch_out[9 + k] * a->ch_out[9 + k]);
      }
      if (s) {
        if (a->ch_out[10] != prev_hh)
          hh_changes++;
        if (a->ch_out[11] != prev_sd)
          sd_changes++;
        if (a->ch_out[13] != prev_cym)
          cym_changes++;
      }
      prev_hh = a->ch_out[10];
      prev_sd = a->ch_out[11];
      prev_cym = a->ch_out[13];
    }
    /* non-vacuous guards: every drum sounded and the noise-sensitive voices
     * actually changed value during the run. */
    for (k = 0; k < 5; k++)
      UF_CHECK(drum_energy[k] > 0);
    UF_CHECK(hh_changes > 10);
    UF_CHECK(sd_changes > 10);
    UF_CHECK(cym_changes > 0);
    OPLL_delete(a);
    OPLL_delete(b);
  }

  /* 1b. Exhaustive combinations of the five phase bits feeding the short
   * noise. Catches an XOR mistakenly turned into an OR (differs when both
   * inputs of a pair are 1). */
  {
    OPLL *o = OPLL_new(3600000, 50000);
    int combo;
    UF_CHECK(o != NULL);
    for (combo = 0; combo < 32; combo++) {
      const uint32_t h2 = (combo >> 0) & 1, h7 = (combo >> 1) & 1, h3 = (combo >> 2) & 1;
      const uint32_t c3 = (combo >> 3) & 1, c5 = (combo >> 4) & 1;
      o->slot[SLOT_HH].pg_out = (h2 << 2) | (h7 << 7) | (h3 << 3);
      o->slot[SLOT_CYM].pg_out = (c3 << 3) | (c5 << 5);
      update_short_noise(o);
      uf_expect_eq("noise_clock", combo, "short_noise", (long)o->short_noise,
                   (long)((h2 ^ h7) | (h3 ^ c5) | (c3 ^ c5)));
    }
    OPLL_delete(o);
  }

  /* 1c. The short noise of one internal sample must be derived from the
   * phase state that was current when the sample started (i.e. before
   * update_slots advances the phase), not from the advanced one. */
  {
    OPLL *o = OPLL_new(3600000, 50000);
    OPLL_SLOT *hh, *cym;
    uint32_t inc_hh, inc_cym, x, y;
    int found = 0;
    UF_CHECK(o != NULL);
    OPLL_writeReg(o, 0x17, 0x50);
    OPLL_writeReg(o, 0x27, 0x08);
    OPLL_writeReg(o, 0x18, 0x60);
    OPLL_writeReg(o, 0x28, 0x08);
    OPLL_writeReg(o, 0x0e, 0x20); /* rhythm mode on, no keys */
    hh = &o->slot[SLOT_HH];
    cym = &o->slot[SLOT_CYM];
    UF_CHECK(hh->patch->PM == 0 && cym->patch->PM == 0);
    inc_hh = (((hh->fnum & 0x1ff) * 2) * ml_table[hh->patch->ML]) << hh->blk >> 2;
    inc_cym = (((cym->fnum & 0x1ff) * 2) * ml_table[cym->patch->ML]) << cym->blk >> 2;
    UF_CHECK(inc_hh > 0 && inc_cym > 0);
    for (x = 0; x < DP_WIDTH && found < 8; x += 9973) {
      for (y = 0; y < DP_WIDTH && found < 8; y += 9967) {
        const uint32_t pb_h = (x >> DP_BASE_BITS) & (PG_WIDTH - 1);
        const uint32_t pb_c = (y >> DP_BASE_BITS) & (PG_WIDTH - 1);
        const uint32_t pa_h = ((x + inc_hh) & (DP_WIDTH - 1)) >> DP_BASE_BITS;
        const uint32_t pa_c = ((y + inc_cym) & (DP_WIDTH - 1)) >> DP_BASE_BITS;
        const uint8_t s_before = ref_short_noise(pb_h, pb_c);
        const uint8_t s_after = ref_short_noise(pa_h, pa_c);
        if (s_before == s_after)
          continue; /* only beats where the choice matters are meaningful */
        hh->pg_phase = x;
        hh->pg_out = pb_h;
        cym->pg_phase = y;
        cym->pg_out = pb_c;
        update_output(o);
        uf_expect_eq("noise_clock", found, "short_noise-beat", (long)o->short_noise, (long)s_before);
        found++;
      }
    }
    UF_CHECK(found == 8);
    OPLL_delete(o);
  }
}

/* ------------------------------------------------------------------ */
/* group 2: percussion quantization                                     */
/* ------------------------------------------------------------------ */
void uf_percussion(void) {
  int x, ci;
  long casenum = 0;
  long nonzero = 0;

  /* 2a. The production lookup tables must match the independent math
   * references exactly: half-cell sampled quarter sine, integer attenuation
   * values, mirrored second quadrant, 0x8000-flagged negative half wave. */
  for (x = 0; x < 256; x++)
    uf_expect_eq("percussion", x, "exp_table", (long)exp_table[x], (long)ref_exp_table(x));
  for (x = 0; x < 1024; x++)
    uf_expect_eq("percussion", x, "fullsin_table", (long)fullsin_table[x], (long)ref_fullsin(x));
  for (x = 0; x < 512; x++)
    uf_expect_eq("percussion", x, "halfsin_table", (long)halfsin_table[x], (long)ref_fullsin(x));
  for (x = 512; x < 1024; x++)
    uf_expect_eq("percussion", x, "halfsin_table", (long)halfsin_table[x], 0xfff);

  /* 2b. Phase selection and amplitude quantization of the percussion slots
   * at controlled phases and attenuations: low attenuation, attenuation
   * clamped near mute, and the exact mute boundary (eg_out == EG_MAX still
   * sounds, EG_MAX+1 is silent). Expected noise/short_noise values are
   * driven from the test's own state. */
  {
    OPLL *o = OPLL_new(3600000, 50000);
    static const struct {
      uint32_t eg, tll;
    } att[] = {{0, 0}, {8, 0}, {0, 40}, {24, 16}, {100, 100}, {122, 1}, {123, 0}, {123, 4}, {124, 0}, {127, 0}};
    static const uint32_t hat_phases[4] = {0x2d0, 0x234, 0x34, 0xd0};
    static const uint32_t sd_phases[4] = {0x000, 0x100, 0x200, 0x300};
    static const uint32_t sd_pgout[4] = {0x000, 0x080, 0x100, 0x180}; /* quadrant bit clear/set, neighbor bit both ways */
    static const uint32_t cym_phases[2] = {0x100, 0x300};
    OPLL_SLOT *sl;
    int sn, nb, pi;
    int16_t actual, want;
    uint32_t phase;
    UF_CHECK(o != NULL);

    /* hi-hat: both short-noise and long-noise combinations */
    for (ci = 0; ci < 10; ci++) {
      for (sn = 0; sn < 2; sn++) {
        for (nb = 0; nb < 2; nb++) {
          phase = sn ? (nb ? 0x2d0 : 0x234) : (nb ? 0x34 : 0xd0);
          o->short_noise = (uint8_t)sn;
          o->noise = (uint32_t)nb;
          sl = &o->slot[SLOT_HH];
          sl->wave_table = wave_table_map[0];
          sl->eg_out = att[ci].eg;
          sl->tll = (uint16_t)att[ci].tll;
          actual = calc_slot_hat(o);
          want = ref_to_linear(ref_fullsin((int)phase), att[ci].eg, att[ci].tll, 0);
          if (want != 0)
            nonzero++;
          uf_perc_expect("hat", casenum++, actual, want, hat_phases, 4, phase, att[ci].eg, att[ci].tll);
        }
      }
    }

    /* snare: the phase quadrant selected by its own pg_out bit 8 */
    for (ci = 0; ci < 10; ci++) {
      for (pi = 0; pi < 4; pi++) {
        for (nb = 0; nb < 2; nb++) {
          const uint32_t bit8 = (sd_pgout[pi] >> 8) & 1;
          phase = bit8 ? (nb ? 0x300 : 0x200) : (nb ? 0x000 : 0x100);
          o->noise = (uint32_t)nb;
          sl = &o->slot[SLOT_SD];
          sl->wave_table = wave_table_map[0];
          sl->eg_out = att[ci].eg;
          sl->tll = (uint16_t)att[ci].tll;
          sl->pg_out = sd_pgout[pi];
          actual = calc_slot_snare(o);
          want = ref_to_linear(ref_fullsin((int)phase), att[ci].eg, att[ci].tll, 0);
          if (want != 0)
            nonzero++;
          uf_perc_expect("snare", casenum++, actual, want, sd_phases, 4, phase, att[ci].eg, att[ci].tll);
        }
      }
    }

    /* cymbal: the phase selected by the short noise */
    for (ci = 0; ci < 10; ci++) {
      for (sn = 0; sn < 2; sn++) {
        phase = sn ? 0x300 : 0x100;
        o->short_noise = (uint8_t)sn;
        sl = &o->slot[SLOT_CYM];
        sl->wave_table = wave_table_map[0];
        sl->eg_out = att[ci].eg;
        sl->tll = (uint16_t)att[ci].tll;
        actual = calc_slot_cym(o);
        want = ref_to_linear(ref_fullsin((int)phase), att[ci].eg, att[ci].tll, 0);
        if (want != 0)
          nonzero++;
        uf_perc_expect("cymbal", casenum++, actual, want, cym_phases, 2, phase, att[ci].eg, att[ci].tll);
      }
    }

    UF_CHECK(nonzero > 0); /* the sweep was not silently all-muted */
    OPLL_delete(o);
  }
}

/* ------------------------------------------------------------------ */
/* group 3: rendering through the public API                            */
/* ------------------------------------------------------------------ */

/* Run n output samples on a mono and a center-panned stereo instance in
 * lockstep. The mono sample must equal both stereo channels exactly, the
 * noise register must track the test's own model (advanced by the number
 * of internal updates that actually ran, observed via eg_counter), and if
 * `fresh` is given the mono stream must equal a fresh instance's stream
 * sample for sample. */
static void render_pair_run(OPLL *m, OPLL *st, OPLL *fresh, int n, uint32_t *model, uint64_t *energy, long *sidx) {
  int s;
  for (s = 0; s < n; s++, (*sidx)++) {
    const uint32_t eg_before = m->eg_counter;
    const int16_t vm = OPLL_calc(m);
    int32_t vst[2];
    OPLL_calcStereo(st, vst);
    uf_expect_eq("render", *sidx, "stereo-lr", (long)vst[0], (long)vst[1]);
    uf_expect_eq("render", *sidx, "mono-vs-stereo", (long)vm, (long)vst[0]);
    if (fresh) {
      const int16_t vf = OPLL_calc(fresh);
      uf_expect_eq("render", *sidx, "reset-restart", (long)vm, (long)vf);
    }
    *model = ref_noise_clock(*model, 18 * (int)(m->eg_counter - eg_before));
    uf_expect_eq("render", *sidx, "noise", (long)m->noise, (long)*model);
    *energy += (uint64_t)((int32_t)vm * vm);
  }
}

void uf_render(void) {
  /* A. clock = 3600000, rate = 50000 == clock/72: the rate converter is
   * bypassed, so each OPLL_calc is exactly one internal update. Melody and
   * rhythm coexist; verify the per-channel outputs and the mix wiring
   * against the relations established above. */
  {
    OPLL *o = OPLL_new(3600000, 50000);
    uint64_t ch_energy[14] = {0};
    uint32_t model = 1;
    long s;
    int i;
    UF_CHECK(o != NULL);
    UF_CHECK(o->conv == NULL); /* converter really is bypassed */
    render_prog(o);
    for (s = 0; s < 4096; s++) {
      const int16_t v = OPLL_calc(o);
      int16_t mix = 0;
      model = ref_noise_clock(model, 18);
      uf_expect_eq("render", s, "noise", (long)o->noise, (long)model);
      for (i = 0; i < 14; i++) {
        mix = (int16_t)(mix + o->ch_out[i]);
        ch_energy[i] += (uint64_t)((int32_t)o->ch_out[i] * o->ch_out[i]);
      }
      uf_expect_eq("render", s, "mix", (long)v, (long)mix);
    }
    UF_CHECK(ch_energy[0] > 0);  /* plain melody note, unmodified preset */
    UF_CHECK(ch_energy[9] > 0);  /* bass drum */
    UF_CHECK(ch_energy[10] > 0); /* hi-hat */
    UF_CHECK(ch_energy[11] > 0); /* snare */
    UF_CHECK(ch_energy[12] > 0); /* tom */
    UF_CHECK(ch_energy[13] > 0); /* cymbal */
    OPLL_delete(o);
  }

  /* B. 44100 Hz output through the rate converter. Two independent
   * instances (mono and stereo with identical left/right pan) must produce
   * the same stream; the sequence includes the rhythm section being turned
   * off and on again and repeated resets, after which the audible stream
   * and the noise register must restart exactly like a fresh instance. */
  {
    OPLL *m = OPLL_new(3579545, 44100);
    OPLL *st = OPLL_new(3579545, 44100);
    OPLL *fresh;
    uint32_t model = 1;
    uint64_t energy = 0, seg = 0;
    long sidx = 0;
    int r;
    UF_CHECK(m != NULL && st != NULL);
    UF_CHECK(m->conv != NULL); /* converter really is active */

    render_prog(m);
    render_prog(st);
    render_pair_run(m, st, NULL, 600, &model, &energy, &sidx);

    /* rhythm off: the melody must stay audible */
    OPLL_writeReg(m, 0x0e, 0x00);
    OPLL_writeReg(st, 0x0e, 0x00);
    render_pair_run(m, st, NULL, 300, &model, &seg, &sidx);
    UF_CHECK(seg > 0);
    energy += seg;

    /* rhythm on again */
    OPLL_writeReg(m, 0x0e, 0x3f);
    OPLL_writeReg(st, 0x0e, 0x3f);
    seg = 0;
    render_pair_run(m, st, NULL, 600, &model, &seg, &sidx);
    UF_CHECK(seg > 0);
    energy += seg;

    /* repeated reset: noise restarts from the seed and the post-reset
     * stream is identical to a fresh instance running the same program. */
    for (r = 0; r < 2; r++) {
      OPLL_reset(m);
      OPLL_reset(st);
      uf_expect_eq("render", sidx, "noise-restart", (long)m->noise, 1);
      model = 1;
      render_prog(m);
      render_prog(st);
      fresh = OPLL_new(3579545, 44100);
      UF_CHECK(fresh != NULL);
      render_prog(fresh);
      seg = 0;
      render_pair_run(m, st, fresh, 600, &model, &seg, &sidx);
      UF_CHECK(seg > 10000); /* audible again after the reset */
      energy += seg;
      OPLL_delete(fresh);
    }
    UF_CHECK(energy > 0);
    OPLL_delete(m);
    OPLL_delete(st);
  }
}
