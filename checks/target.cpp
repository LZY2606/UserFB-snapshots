// Verification target for the exact batch digital engine of
// SID::clock(delta_t). The per-cycle entry SID::clock() is the reference;
// its behavior is unchanged.
//
// Groups:
//   digital - field-by-field state equivalence between clock() and
//             clock(delta_t) for both chip models, including OSC3/ENV3,
//             followed by 64 single-cycle reads and a GATE/TEST toggle to
//             expose hidden pipeline state.
//   edges   - simultaneous events and counter wraparounds: hard sync with
//             the source itself synced, noise shift pipeline collisions,
//             ADSR rate lowering (delay bug), rate counter wrap, TEST
//             hold/release, fast GATE toggling, floating DAC, combined
//             waveforms, zero frequency, 8580 write delay.
//   mixed   - one absolute-cycle register script driven through
//             alternating single-cycle and 1/7/257/long batch calls, then
//             real finite non-zero PCM through the sampling entry.
//   skip    - proof that event-free regions are truly skipped: per-cycle
//             digital updates are counted where they happen.

#include "sid.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace reSID;

static const char* group_name = "";
static long checks = 0;

static bool fail(const char* scen, long long cycle, int voice,
                 const char* field, unsigned ref, unsigned got)
{
  std::printf("FAIL %s scen=%s cycle=%lld voice=%d field=%s ref=%u got=%u\n",
              group_name, scen, cycle, voice, field, ref, got);
  return false;
}

// Compare every digital field exposed through read_state() plus the
// readable digital registers OSC3/ENV3 and the data bus. The bus ttl
// counter itself is allowed to differ and is not compared.
static bool cmp(SID& ref, SID& got, const char* scen, long long cycle)
{
  SID::State a = ref.read_state();
  SID::State b = got.read_state();
  ++checks;
  for (int i = 0; i < 3; i++) {
#define CMPF(name) \
    if (a.name[i] != b.name[i]) \
      return fail(scen, cycle, i, #name, (unsigned)a.name[i], (unsigned)b.name[i]);
    CMPF(accumulator)
    CMPF(shift_register)
    CMPF(shift_register_reset)
    CMPF(shift_pipeline)
    CMPF(pulse_output)
    CMPF(floating_output_ttl)
    CMPF(rate_counter)
    CMPF(rate_counter_period)
    CMPF(exponential_counter)
    CMPF(exponential_counter_period)
    CMPF(envelope_counter)
    CMPF(envelope_state)
    CMPF(hold_zero)
    CMPF(envelope_pipeline)
#undef CMPF
  }
  if (a.bus_value != b.bus_value)
    return fail(scen, cycle, -1, "bus_value", a.bus_value, b.bus_value);
  if (a.write_pipeline != b.write_pipeline)
    return fail(scen, cycle, -1, "write_pipeline",
                (unsigned)a.write_pipeline, (unsigned)b.write_pipeline);
  if (a.write_address != b.write_address)
    return fail(scen, cycle, -1, "write_address", a.write_address, b.write_address);
  unsigned ro = ref.read(0x1b), go = got.read(0x1b);
  if (ro != go)
    return fail(scen, cycle, 2, "OSC3", ro, go);
  unsigned re = ref.read(0x1c), ge = got.read(0x1c);
  if (re != ge)
    return fail(scen, cycle, 2, "ENV3", re, ge);
  unsigned rb = ref.read(0x05), gb = got.read(0x05);
  if (rb != gb)
    return fail(scen, cycle, -1, "bus_read", rb, gb);
  return true;
}

struct W {
  cycle_count at;
  reg8 off;
  reg8 val;
};

// Drive two chips with the same absolute-cycle register script. The
// reference advances strictly by single cycles, the candidate by batch
// calls with the given chunk pattern (clamped to the next write so that
// writes land on identical absolute cycles). State is compared after
// every batch call, then for 64 further single cycles, and finally after
// a GATE/TEST toggle to expose hidden pipeline state.
static bool run_pair(chip_model model, const std::vector<W>& ws,
                     cycle_count total, const int* chunks, int nchunks,
                     const char* scen)
{
  SID ref, got;
  ref.reset();
  got.reset();
  ref.set_chip_model(model);
  got.set_chip_model(model);

  cycle_count t = 0;
  size_t wi = 0;
  int ci = 0;
  while (t < total) {
    while (wi < ws.size() && ws[wi].at <= t) {
      ref.write(ws[wi].off, ws[wi].val);
      got.write(ws[wi].off, ws[wi].val);
      ++wi;
    }
    cycle_count room = (wi < ws.size() ? ws[wi].at : total) - t;
    if (room <= 0) room = 1;
    cycle_count n = chunks[ci++ % nchunks];
    if (n > room) n = room;
    if (n > total - t) n = total - t;
    for (cycle_count k = 0; k < n; k++) ref.clock();
    got.clock(n);
    t += n;
    if (!cmp(ref, got, scen, t)) return false;
  }

  // 64 single cycles on both: hidden pipelines must produce identical
  // readable trajectories.
  for (int i = 0; i < 64; i++) {
    ref.clock();
    got.clock();
    if (!cmp(ref, got, scen, t + 1 + i)) return false;
  }

  // Toggle GATE and TEST on the next cycle; the following trajectory must
  // still be identical.
  for (int v = 0; v < 3; v++) {
    reg8 cr = 0x49; // test | pulse | gate
    ref.write(v*7 + 4, cr);
    got.write(v*7 + 4, cr);
  }
  for (int i = 0; i < 300; i++) {
    ref.clock();
    got.clock();
    if (!cmp(ref, got, scen, t + 65 + i)) return false;
  }
  for (int v = 0; v < 3; v++) {
    ref.write(v*7 + 4, 0x21); // saw | gate
    got.write(v*7 + 4, 0x21);
  }
  for (int i = 0; i < 300; i++) {
    ref.clock();
    got.clock();
    if (!cmp(ref, got, scen, t + 365 + i)) return false;
  }
  return true;
}

static void put(std::vector<W>& ws, cycle_count at, int off, int val)
{
  W w;
  w.at = at;
  w.off = (reg8)off;
  w.val = (reg8)val;
  ws.push_back(w);
}

// Deterministic pseudo-random source for write scripts.
struct Lcg {
  unsigned s;
  Lcg(unsigned seed) : s(seed) {}
  unsigned next() { s = s*1664525u + 1013904223u; return s >> 8; }
};

// ---------------------------------------------------------------------------
// digital group
// ---------------------------------------------------------------------------

static bool digital_model(chip_model model, const char* tag)
{
  // Three active voices with different characters.
  std::vector<W> ws;
  put(ws, 0, 24, 15);           // volume
  put(ws, 0, 0, 0x45); put(ws, 0, 1, 0x23);
  put(ws, 0, 5, 0x09); put(ws, 0, 6, 0xf1);
  put(ws, 0, 4, 0x21);          // saw, gate
  put(ws, 0, 7, 0x34); put(ws, 0, 8, 0x12);
  put(ws, 0, 9, 0x56); put(ws, 0, 10, 0x04);
  put(ws, 0, 12, 0x44); put(ws, 0, 13, 0xa8);
  put(ws, 0, 11, 0x41);         // pulse, gate
  put(ws, 0, 14, 0x56); put(ws, 0, 15, 0x34);
  put(ws, 0, 19, 0x11); put(ws, 0, 20, 0x33);
  put(ws, 0, 18, 0x81);         // noise, gate
  // Mid-run register activity.
  put(ws, 40000, 1, 0x30);
  put(ws, 60000, 4, 0x20);      // gate off
  put(ws, 60500, 4, 0x21);      // gate on
  put(ws, 80000, 18, 0x80);
  char scen[64];
  {
    const int chunks[] = {257};
    std::snprintf(scen, sizeof scen, "digital-%s-c257", tag);
    if (!run_pair(model, ws, 120000, chunks, 1, scen)) return false;
  }
  {
    const int chunks[] = {1, 7, 257, 5000};
    std::snprintf(scen, sizeof scen, "digital-%s-mixed", tag);
    if (!run_pair(model, ws, 120000, chunks, 4, scen)) return false;
  }
  {
    const int chunks[] = {100000};
    std::snprintf(scen, sizeof scen, "digital-%s-long", tag);
    if (!run_pair(model, ws, 120000, chunks, 1, scen)) return false;
  }
  return true;
}

static bool digital_sync_ring(chip_model model, const char* tag)
{
  // Sync chains and ring modulation between skippable waveforms.
  std::vector<W> ws;
  put(ws, 0, 24, 15);
  put(ws, 0, 0, 0x00); put(ws, 0, 1, 0x40);
  put(ws, 0, 4, 0x11);                    // tri, gate
  put(ws, 0, 7, 0x00); put(ws, 0, 8, 0x30);
  put(ws, 0, 11, 0x27);                   // saw, ring, sync, gate
  put(ws, 0, 14, 0x00); put(ws, 0, 15, 0x25);
  put(ws, 0, 18, 0x13);                   // tri, sync, gate
  put(ws, 0, 5, 0x09); put(ws, 0, 6, 0xf1);
  put(ws, 0, 12, 0x09); put(ws, 0, 13, 0xf1);
  put(ws, 0, 19, 0x09); put(ws, 0, 20, 0xf1);
  char scen[64];
  const int chunks[] = {7, 257, 1, 1000};
  std::snprintf(scen, sizeof scen, "digital-%s-syncring", tag);
  return run_pair(model, ws, 200000, chunks, 4, scen);
}

static bool group_digital()
{
  return digital_model(MOS6581, "6581")
    && digital_model(MOS8580, "8580")
    && digital_sync_ring(MOS6581, "6581")
    && digital_sync_ring(MOS8580, "8580");
}

// ---------------------------------------------------------------------------
// edges group
// ---------------------------------------------------------------------------

static bool edge_sync_simultaneous()
{
  // All three accumulators aligned with equal frequencies: MSB rises are
  // simultaneous. Voice 2 is synced by voice 1 while voice 1 is itself
  // synced by voice 0 on the same cycle; voice 2 must not be reset then.
  std::vector<W> ws;
  put(ws, 0, 24, 15);
  put(ws, 0, 0, 0x00); put(ws, 0, 1, 0x40);
  put(ws, 0, 4, 0x20);            // saw
  put(ws, 0, 7, 0x00); put(ws, 0, 8, 0x40);
  put(ws, 0, 11, 0x22);           // saw + sync
  put(ws, 0, 14, 0x00); put(ws, 0, 15, 0x40);
  put(ws, 0, 18, 0x22);           // saw + sync
  // Desynchronize accumulator phases, then re-align via TEST.
  put(ws, 30000, 1, 0x41);
  put(ws, 60000, 4, 0x28);        // test all? no: voice 0 only
  put(ws, 60000, 11, 0x2a);
  put(ws, 60000, 18, 0x2a);
  put(ws, 61000, 4, 0x20);
  put(ws, 61000, 11, 0x22);
  put(ws, 61000, 18, 0x22);
  const int chunks[] = {257, 1, 7, 1000};
  return run_pair(MOS6581, ws, 150000, chunks, 4, "edges-sync6581")
    && run_pair(MOS8580, ws, 150000, chunks, 4, "edges-sync8580");
}

static bool edge_noise_pipeline()
{
  // Bit 19 rises every ~2.5 cycles, constantly colliding with the pending
  // 2-cycle noise shift pipeline.
  std::vector<W> ws;
  put(ws, 0, 24, 15);
  put(ws, 0, 0, 0x66); put(ws, 0, 1, 0x66);
  put(ws, 0, 4, 0x81);            // noise, gate
  put(ws, 0, 5, 0x09); put(ws, 0, 6, 0xf1);
  put(ws, 0, 7, 0xcd); put(ws, 0, 8, 0x7a);
  put(ws, 0, 11, 0x81);
  put(ws, 0, 12, 0x09); put(ws, 0, 13, 0xf1);
  const int chunks[] = {7, 257, 1};
  return run_pair(MOS6581, ws, 100000, chunks, 3, "edges-noise6581")
    && run_pair(MOS8580, ws, 100000, chunks, 3, "edges-noise8580");
}

static bool edge_adsr_delay_bug()
{
  // Let the rate counter climb with a slow attack, then lower the rate
  // period far below the counter: the counter must wrap through 0x7fff
  // before the envelope steps (ADSR delay bug).
  std::vector<W> ws;
  put(ws, 0, 24, 15);
  put(ws, 0, 5, 0xf0);            // A=15 (period 31250)
  put(ws, 0, 6, 0xf0);
  put(ws, 0, 4, 0x21);            // gate on
  put(ws, 20000, 5, 0x00);        // A=0 (period 8) while counter is high
  put(ws, 70000, 5, 0x2a);        // lower it again mid-wrap
  put(ws, 120000, 4, 0x20);       // gate off
  put(ws, 120100, 6, 0xf2);       // change release while releasing
  const int chunks[] = {257, 1000, 7};
  return run_pair(MOS6581, ws, 200000, chunks, 3, "edges-adsr6581")
    && run_pair(MOS8580, ws, 200000, chunks, 3, "edges-adsr8580");
}

static bool edge_test_bit()
{
  // Hold TEST beyond the shift register reset time, then release into a
  // running noise oscillator.
  std::vector<W> ws;
  put(ws, 0, 24, 15);
  put(ws, 0, 0, 0x00); put(ws, 0, 1, 0x20);
  put(ws, 0, 4, 0x89);            // noise + test
  put(ws, 0, 5, 0x09); put(ws, 0, 6, 0xf1);
  put(ws, 40000, 4, 0x81);        // test off (6581 reset is 35000 cycles)
  put(ws, 90000, 4, 0x89);        // test again
  put(ws, 95000, 4, 0x81);
  const int chunks[] = {1000, 257, 7};
  if (!run_pair(MOS6581, ws, 150000, chunks, 3, "edges-test6581")) return false;
  // The 8580 shift register reset is ~2.5M cycles; exercise the countdown
  // without waiting for completion.
  std::vector<W> ws2;
  put(ws2, 0, 24, 15);
  put(ws2, 0, 0, 0x00); put(ws2, 0, 1, 0x20);
  put(ws2, 0, 4, 0x89);
  put(ws2, 0, 5, 0x09); put(ws2, 0, 6, 0xf1);
  put(ws2, 100000, 4, 0x81);
  return run_pair(MOS8580, ws2, 200000, chunks, 3, "edges-test8580");
}

static bool edge_gate_toggle()
{
  // Rapid GATE toggling: state pipeline never settles.
  std::vector<W> ws;
  put(ws, 0, 24, 15);
  put(ws, 0, 0, 0x00); put(ws, 0, 1, 0x10);
  put(ws, 0, 5, 0x02); put(ws, 0, 6, 0x84);
  put(ws, 0, 4, 0x20);
  for (int i = 0; i < 60; i++) {
    put(ws, 100 + i*6, 4, (i & 1) ? 0x20 : 0x21);
  }
  put(ws, 500, 4, 0x21);
  const int chunks[] = {1, 7, 257};
  return run_pair(MOS6581, ws, 60000, chunks, 3, "edges-gate6581")
    && run_pair(MOS8580, ws, 60000, chunks, 3, "edges-gate8580");
}

static bool edge_floating_dac()
{
  // Waveform register cleared: the DAC input floats and fades away.
  std::vector<W> ws;
  put(ws, 0, 24, 15);
  put(ws, 0, 0, 0x00); put(ws, 0, 1, 0x10);
  put(ws, 0, 4, 0x21);            // saw, gate
  put(ws, 0, 5, 0x09); put(ws, 0, 6, 0xf1);
  put(ws, 10000, 4, 0x01);        // waveform 0, keep gate
  const int chunks[] = {100000, 257, 7};
  // 6581 floating ttl is 182000 + up to 12*1500 cycles of bitfade.
  if (!run_pair(MOS6581, ws, 220000, chunks, 3, "edges-float6581")) return false;
  // 8580 ttl is 4.4M cycles; exercise the countdown only.
  return run_pair(MOS8580, ws, 220000, chunks, 3, "edges-float8580");
}

static bool edge_combined()
{
  // Combined waveforms: shift register writeback (6581+8580) and the 6581
  // accumulator writeback force cycle-by-cycle regions, which must still
  // match the reference exactly.
  std::vector<W> ws;
  put(ws, 0, 24, 15);
  put(ws, 0, 0, 0x00); put(ws, 0, 1, 0x30);
  put(ws, 0, 5, 0x09); put(ws, 0, 6, 0xf1);
  put(ws, 0, 4, 0x91);            // noise+tri, gate
  put(ws, 20000, 4, 0xa1);        // noise+saw
  put(ws, 40000, 4, 0xc1);        // noise+pulse
  put(ws, 60000, 4, 0xf1);        // all
  put(ws, 80000, 4, 0x31);        // tri+saw (6581 accumulator writeback)
  put(ws, 100000, 4, 0x61);       // saw+pulse
  put(ws, 120000, 4, 0x71);
  put(ws, 140000, 4, 0x81);       // pure noise again
  const int chunks[] = {257, 7, 1000};
  return run_pair(MOS6581, ws, 200000, chunks, 3, "edges-comb6581")
    && run_pair(MOS8580, ws, 200000, chunks, 3, "edges-comb8580");
}

static bool edge_zero_freq()
{
  // Zero frequency with a running envelope and a static pulse output.
  std::vector<W> ws;
  put(ws, 0, 24, 15);
  put(ws, 0, 2, 0x00); put(ws, 0, 3, 0x08);
  put(ws, 0, 4, 0x41);            // pulse, gate, freq = 0
  put(ws, 0, 5, 0x09); put(ws, 0, 6, 0xf1);
  put(ws, 40000, 3, 0x04);        // move pw across the static accumulator
  const int chunks[] = {257, 1000};
  return run_pair(MOS6581, ws, 100000, chunks, 2, "edges-zero6581")
    && run_pair(MOS8580, ws, 100000, chunks, 2, "edges-zero8580");
}

static bool edge_8580_write_delay()
{
  // MOS8580 register writes are delayed one cycle; dense writes at the
  // start of batch calls must not leak into the first cycle.
  std::vector<W> ws;
  put(ws, 0, 24, 15);
  put(ws, 0, 4, 0x21);
  put(ws, 0, 5, 0x09); put(ws, 0, 6, 0xf1);
  for (int i = 0; i < 40; i++) {
    put(ws, 100 + i*97, 1, (i*37) & 0xff);
    put(ws, 100 + i*97, 0, (i*11) & 0xff);
    put(ws, 101 + i*97, 5, (i*7) & 0xff);
  }
  const int chunks[] = {1, 2, 3, 7, 257};
  return run_pair(MOS8580, ws, 20000, chunks, 5, "edges-wr8580");
}

static bool edge_exponential_boundaries()
{
  // Full attack/decay/release crossing every exponential period boundary
  // (0x5d, 0x36, 0x1a, 0x0e, 0x06, 0x00) with chunk edges straddling them.
  std::vector<W> ws;
  put(ws, 0, 24, 15);
  put(ws, 0, 0, 0x00); put(ws, 0, 1, 0x08);
  put(ws, 0, 5, 0x00); put(ws, 0, 6, 0x00); // A=0 D=0 S=0 R=0
  put(ws, 0, 4, 0x21);
  put(ws, 30000, 4, 0x20);        // release
  put(ws, 45000, 4, 0x21);        // attack again from the floor
  const int chunks[] = {257, 7, 1000};
  return run_pair(MOS6581, ws, 80000, chunks, 3, "edges-exp6581")
    && run_pair(MOS8580, ws, 80000, chunks, 3, "edges-exp8580");
}

static bool group_edges()
{
  return edge_sync_simultaneous()
    && edge_noise_pipeline()
    && edge_adsr_delay_bug()
    && edge_test_bit()
    && edge_gate_toggle()
    && edge_floating_dac()
    && edge_combined()
    && edge_zero_freq()
    && edge_8580_write_delay()
    && edge_exponential_boundaries();
}

// ---------------------------------------------------------------------------
// mixed group
// ---------------------------------------------------------------------------

static bool group_mixed()
{
  // One absolute-cycle register script, driven through alternating
  // single-cycle and 1/7/257/long batch calls on both chip models.
  for (int m = 0; m < 2; m++) {
    chip_model model = m ? MOS8580 : MOS6581;
    std::vector<W> ws;
    Lcg rnd(0x5eed1234u + m);
    put(ws, 0, 24, 15);
    cycle_count at = 0;
    for (int i = 0; i < 240; i++) {
      at += 1 + (rnd.next() % 900);
      int which = rnd.next() % 10;
      int voice = rnd.next() % 3;
      int base = voice*7;
      switch (which) {
      case 0: put(ws, at, base + 0, rnd.next() & 0xff); break;
      case 1: put(ws, at, base + 1, rnd.next() & 0xff); break;
      case 2: put(ws, at, base + 2, rnd.next() & 0xff); break;
      case 3: put(ws, at, base + 3, rnd.next() & 0x0f); break;
      case 4: put(ws, at, base + 4, rnd.next() & 0xff); break;
      case 5: put(ws, at, base + 5, rnd.next() & 0xff); break;
      case 6: put(ws, at, base + 6, rnd.next() & 0xff); break;
      case 7: put(ws, at, 21 + (rnd.next() % 2), rnd.next() & 0xff); break;
      case 8: put(ws, at, 23, rnd.next() & 0xff); break;
      default: put(ws, at, 24, rnd.next() & 0x1f); break;
      }
    }
    // Make sure something audible is running at the end.
    put(ws, at + 10, 0, 0x45); put(ws, at + 10, 1, 0x23);
    put(ws, at + 10, 5, 0x00); put(ws, at + 10, 6, 0xf0);
    put(ws, at + 10, 4, 0x21);
    put(ws, at + 10, 24, 15);
    const int chunks[] = {1, 7, 257, 5000, 100000};
    if (!run_pair(model, ws, at + 200000, chunks, 5,
                  m ? "mixed-8580" : "mixed-6581")) return false;

    // Generate real, finite, non-zero PCM through the sampling entry.
    SID snd;
    snd.reset();
    snd.set_chip_model(model);
    if (!snd.set_sampling_parameters(985248, SAMPLE_FAST, 44100)) return false;
    for (size_t i = 0; i < ws.size(); i++) {
      snd.write(ws[i].off, ws[i].val);
      snd.clock(ws[i].at > 0 ? 97 : 0);
    }
    snd.write(0, 0x45); snd.write(1, 0x23);
    snd.write(5, 0x00); snd.write(6, 0xf0);
    snd.write(4, 0x21); snd.write(24, 15);
    short pcm[512];
    cycle_count cycles = 20000;
    int n = snd.clock(cycles, pcm, 512);
    if (n < 1)
      return fail(m ? "mixed-8580" : "mixed-6581", 0, -1, "pcm_frames", 1, 0);
    long energy = 0;
    for (int i = 0; i < n; i++) {
      short v = pcm[i];
      energy += v < 0 ? -(long)v : (long)v;
    }
    ++checks;
    if (energy == 0)
      return fail(m ? "mixed-8580" : "mixed-6581", 0, -1, "pcm_energy", 1, 0);
  }
  return true;
}

// ---------------------------------------------------------------------------
// skip group
// ---------------------------------------------------------------------------

static bool skip_scenario(chip_model model, const std::vector<W>& ws,
                          cycle_count total, const char* scen)
{
  // Reference: single cycle clocking.
  SID ref;
  ref.reset();
  ref.set_chip_model(model);
  for (size_t i = 0; i < ws.size(); i++) ref.write(ws[i].off, ws[i].val);
  for (cycle_count t = 0; t < total; t++) ref.clock();

  // Candidate: one batch call, counting the digital work.
  SID got;
  got.reset();
  got.set_chip_model(model);
  for (size_t i = 0; i < ws.size(); i++) got.write(ws[i].off, ws[i].val);
  SID::reset_debug_counters();
  got.clock(total);
  unsigned long cycles = SID::debug_cycle_updates;
  unsigned long events = SID::debug_event_updates;

  if (!cmp(ref, got, scen, total)) return false;

  std::printf("SKIP %s cycles=%lu events=%lu\n", scen, cycles, events);
  // Hard limit: cycle-by-cycle digital updates must be essentially absent.
  if (cycles > 64)
    return fail(scen, total, -1, "cycle_updates", 64, (unsigned)cycles);
  ++checks;
  return true;
}

static bool group_skip()
{
  // Reset state, zero frequency, GATE off: 1M cycles must be pure skipping.
  {
    std::vector<W> ws;
    if (!skip_scenario(MOS6581, ws, 1000000, "skip-idle6581")) return false;
    if (!skip_scenario(MOS8580, ws, 1000000, "skip-idle8580")) return false;
  }
  // Low frequency sawtooth with a sustaining envelope: the envelope rests
  // at the sustain level and the oscillator only sees rare bit-19/MSB
  // events. Proves the idle case is not a reset special case.
  {
    std::vector<W> ws;
    put(ws, 0, 24, 15);
    put(ws, 0, 0, 0x00); put(ws, 0, 1, 0x01); // freq 0x100
    put(ws, 0, 5, 0x00); put(ws, 0, 6, 0xf0); // A=0 S=15
    put(ws, 0, 4, 0x21);                      // saw, gate
    if (!skip_scenario(MOS6581, ws, 1000000, "skip-saw6581")) return false;
    if (!skip_scenario(MOS8580, ws, 1000000, "skip-saw8580")) return false;
  }
  // Low frequency noise with a sustaining envelope: periodic shift
  // register events, still no cycle-by-cycle updates.
  {
    std::vector<W> ws;
    put(ws, 0, 24, 15);
    put(ws, 0, 0, 0x00); put(ws, 0, 1, 0x01);
    put(ws, 0, 5, 0x00); put(ws, 0, 6, 0xf0);
    put(ws, 0, 4, 0x81);                      // noise, gate
    if (!skip_scenario(MOS6581, ws, 1000000, "skip-noise6581")) return false;
    if (!skip_scenario(MOS8580, ws, 1000000, "skip-noise8580")) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv)
{
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s digital|edges|mixed|skip\n", argv[0]);
    return 2;
  }
  group_name = argv[1];
  bool ok;
  if (!std::strcmp(group_name, "digital")) ok = group_digital();
  else if (!std::strcmp(group_name, "edges")) ok = group_edges();
  else if (!std::strcmp(group_name, "mixed")) ok = group_mixed();
  else if (!std::strcmp(group_name, "skip")) ok = group_skip();
  else {
    std::fprintf(stderr, "unknown group %s\n", group_name);
    return 2;
  }
  if (!ok) return 1;
  if (checks < 1) {
    std::fprintf(stderr, "no checks performed\n");
    return 1;
  }
  std::printf("PASS %s checks=%ld\n", group_name, checks);
  return 0;
}
