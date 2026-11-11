// ==================================================================================
// Regression check for fftconvolver::MatrixConvolver
//
// Covers:
//   - Sparse 2-input / 3-output matrix with sparse echoes crossing head/tail
//     boundaries, duplicate (accumulated) connections, a silent output and
//     impulse responses of different lengths
//   - Reference verification by independent time-domain convolution per edge
//   - Cross-channel in-place aliasing (an output overwrites another input)
//   - 1/7/31 frame chunking (sample positions must not depend on chunking)
//   - Head/tail block boundaries
//   - reset() must discard all history
//   - No heap allocation/deallocation during process()
//   - Shared FFT counts (sharing happens in the frequency domain)
//   - Validation failures, zero-length calls, post-init data mutation
// ==================================================================================

#include "MatrixConvolver.h"
#include "TwoStageFFTConvolver.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace
{

// Global heap counters: process() must not allocate or free anything
size_t g_newCalls = 0;
size_t g_arrayNewCalls = 0;
size_t g_deleteCalls = 0;
size_t g_arrayDeleteCalls = 0;

} // namespace

void* operator new(size_t n)
{
  ++g_newCalls;
  void* p = std::malloc(n ? n : 1);
  if (!p)
  {
    throw std::bad_alloc();
  }
  return p;
}

void* operator new[](size_t n)
{
  ++g_arrayNewCalls;
  void* p = std::malloc(n ? n : 1);
  if (!p)
  {
    throw std::bad_alloc();
  }
  return p;
}

void operator delete(void* p) noexcept
{
  ++g_deleteCalls;
  std::free(p);
}

void operator delete[](void* p) noexcept
{
  ++g_arrayDeleteCalls;
  std::free(p);
}

namespace
{

typedef std::vector<float> Buf;
typedef fftconvolver::MatrixConnection Connection;

const size_t NUM_IN = 2;
const size_t NUM_OUT = 3;
const size_t HEAD_BLOCK = 4;
const size_t TAIL_BLOCK = 16;

// Deterministic pseudo-random input signal
float PseudoSample(size_t i, unsigned salt)
{
  unsigned z = static_cast<unsigned>(i) * 1103515245u + 12345u + salt * 2654435761u;
  z ^= (z >> 13);
  z *= 2246822519u;
  z ^= (z >> 16);
  return static_cast<float>(static_cast<int>(z & 0xffffu) - 32768) / 32768.0f;
}

// Independent time-domain reference for one edge (input before playback is zero)
float ReferenceEdge(const Buf& input, const Buf& ir, long n)
{
  double sum = 0.0;
  for (size_t k=0; k<ir.size(); ++k)
  {
    const long idx = n - static_cast<long>(k);
    if (idx >= 0)
    {
      sum += static_cast<double>(ir[k]) * static_cast<double>(input[static_cast<size_t>(idx)]);
    }
  }
  return static_cast<float>(sum);
}

struct EdgeDef
{
  size_t input;
  size_t output;
  Buf ir;
};

// Builds the sparse matrix used throughout the check
std::vector<EdgeDef> BuildEdges()
{
  std::vector<EdgeDef> edges;

  // in0 -> out0, short dense response (head only, 2 head partitions)
  {
    EdgeDef e;
    e.input = 0;
    e.output = 0;
    e.ir.resize(5);
    for (size_t i=0; i<e.ir.size(); ++i)
    {
      e.ir[i] = 0.35f - 0.11f * static_cast<float>(i);
    }
    edges.push_back(e);
  }

  // in1 -> out0, response crossing the head/tail0 boundary (length 20 > T=16)
  {
    EdgeDef e;
    e.input = 1;
    e.output = 0;
    e.ir.resize(20);
    for (size_t i=0; i<e.ir.size(); ++i)
    {
      e.ir[i] = 0.25f * PseudoSample(i, 7u);
    }
    edges.push_back(e);
  }

  // Duplicate connection on the same channel pair: must be added, not replace
  {
    EdgeDef e;
    e.input = 0;
    e.output = 0;
    e.ir.resize(3);
    e.ir[0] = 0.6f;
    e.ir[1] = -0.4f;
    e.ir[2] = 0.2f;
    edges.push_back(e);
  }

  // in0 -> out1, long sparse echo response crossing all three partition ranges:
  // head [0,16), tail0 [16,32), long tail [32,41)
  {
    EdgeDef e;
    e.input = 0;
    e.output = 1;
    Buf(41, 0.0f).swap(e.ir);
    e.ir[0]  = 0.8f;
    e.ir[5]  = -0.5f;
    e.ir[15] = 0.3f;   // last head tap
    e.ir[16] = 0.45f;  // first tail0 tap
    e.ir[17] = -0.25f;
    e.ir[31] = 0.4f;   // last tail0 tap
    e.ir[32] = -0.6f;  // first long-tail tap
    e.ir[40] = 0.22f;  // tail of the long-tail partition
    edges.push_back(e);
  }

  // in1 -> out1, yet another length (33 = 2*T + 1), dense
  {
    EdgeDef e;
    e.input = 1;
    e.output = 1;
    e.ir.resize(33);
    for (size_t i=0; i<e.ir.size(); ++i)
    {
      e.ir[i] = 0.15f * PseudoSample(i, 13u);
    }
    edges.push_back(e);
  }

  // Output 2 intentionally has no connection at all => must stay silent

  return edges;
}

std::vector<Connection> BuildConnectionTable(const std::vector<EdgeDef>& edges,
                                             bool includeEmptyEdge)
{
  std::vector<Connection> table;
  for (size_t i=0; i<edges.size(); ++i)
  {
    Connection c;
    c.input = edges[i].input;
    c.output = edges[i].output;
    c.ir = edges[i].ir.empty() ? nullptr : edges[i].ir.data();
    c.irLen = edges[i].ir.size();
    table.push_back(c);
  }

  if (includeEmptyEdge)
  {
    // An empty response must simply contribute nothing
    Connection empty;
    empty.input = 1;
    empty.output = 0;
    empty.ir = nullptr;
    empty.irLen = 0;
    table.push_back(empty);
  }
  return table;
}

bool InitMatrix(fftconvolver::MatrixConvolver& mc,
                const std::vector<EdgeDef>& edges,
                bool includeEmptyEdge = true)
{
  std::vector<Connection> table = BuildConnectionTable(edges, includeEmptyEdge);
  return mc.init(NUM_IN, NUM_OUT, HEAD_BLOCK, TAIL_BLOCK,
                 table.data(), table.size());
}

void ReferenceOutputs(const Buf& in0, const Buf& in1, size_t len,
                      const std::vector<EdgeDef>& edges,
                      Buf& ref0, Buf& ref1, Buf& ref2)
{
  ref0.assign(len, 0.0f);
  ref1.assign(len, 0.0f);
  ref2.assign(len, 0.0f);
  for (long n=0; n<static_cast<long>(len); ++n)
  {
    for (size_t e=0; e<edges.size(); ++e)
    {
      const Buf& src = (edges[e].input == 0) ? in0 : in1;
      const float v = ReferenceEdge(src, edges[e].ir, n);
      if (edges[e].output == 0)
      {
        ref0[static_cast<size_t>(n)] += v;
      }
      else
      {
        assert(edges[e].output == 1);
        ref1[static_cast<size_t>(n)] += v;
      }
    }
  }
}

void CheckClose(const Buf& got, const Buf& ref, const char* label, float tol)
{
  assert(got.size() == ref.size());
  float maxDiff = 0.0f;
  for (size_t i=0; i<got.size(); ++i)
  {
    const float diff = std::fabs(got[i] - ref[i]);
    maxDiff = std::max(maxDiff, diff);
    if (!(diff <= tol))
    {
      std::fprintf(stderr, "%s mismatch at %zu: got %.8f ref %.8f diff %.3e\n",
                   label, i, got[i], ref[i], diff);
      assert(false);
    }
  }
  std::printf("  %s max abs error %.3e (tol %.0e)\n", label, maxDiff, tol);
}

// Runs one stream through the matrix with a given chunk size.
// With aliasOutput0ToInput1, output channel 0 shares its buffer with a working
// copy of input channel 1 (cross-channel in-place operation).
void RunChunked(fftconvolver::MatrixConvolver& mc,
                const Buf& in0, const Buf& in1, size_t len,
                size_t chunk, bool aliasOutput0ToInput1,
                Buf& out0, Buf& out1, Buf& out2)
{
  out0.assign(len, 0.0f);
  out1.assign(len, 0.0f);
  out2.assign(len, 0.0f);

  Buf work0 = in0;
  Buf work1 = in1;

  size_t pos = 0;
  while (pos < len)
  {
    const size_t n = std::min(chunk, len - pos);

    if (aliasOutput0ToInput1)
    {
      // Restore the not-yet-consumed part of input 1, which the previous
      // chunk may have overwritten through the aliased output 0.
      std::memcpy(work1.data() + pos, in1.data() + pos, n * sizeof(float));
    }

    const fftconvolver::Sample* inputs[NUM_IN] = {
      work0.data() + pos,
      work1.data() + pos
    };
    fftconvolver::Sample* outputs[NUM_OUT];
    outputs[0] = aliasOutput0ToInput1 ? (work1.data() + pos) : (out0.data() + pos);
    outputs[1] = out1.data() + pos;
    outputs[2] = out2.data() + pos;

    const size_t newBefore = g_newCalls + g_arrayNewCalls;
    const size_t deleteBefore = g_deleteCalls + g_arrayDeleteCalls;

    mc.process(inputs, outputs, n);

    const size_t newAfter = g_newCalls + g_arrayNewCalls;
    const size_t deleteAfter = g_deleteCalls + g_arrayDeleteCalls;
    assert(newAfter == newBefore);       // No allocation during process()
    assert(deleteAfter == deleteBefore); // No deallocation during process()

    pos += n;
  }

  if (aliasOutput0ToInput1)
  {
    out0 = work1;
  }
}

void TestCorrectnessAndAliasing(const std::vector<EdgeDef>& edges)
{
  const size_t len = 97; // 6 full tail blocks + 1 sample; crosses every boundary
  Buf in0(len), in1(len);
  for (size_t i=0; i<len; ++i)
  {
    in0[i] = PseudoSample(i, 1u);
    in1[i] = PseudoSample(i, 2u);
  }

  Buf ref0, ref1, ref2;
  ReferenceOutputs(in0, in1, len, edges, ref0, ref1, ref2);

  const size_t chunks[] = {1, 7, 31, len};
  for (size_t ci=0; ci<sizeof(chunks)/sizeof(chunks[0]); ++ci)
  {
    for (int alias=0; alias<2; ++alias)
    {
      fftconvolver::MatrixConvolver mc;
      assert(InitMatrix(mc, edges));

      Buf out0, out1, out2;
      RunChunked(mc, in0, in1, len, chunks[ci], alias != 0, out0, out1, out2);

      CheckClose(out0, ref0, "out0", 3e-4f);
      CheckClose(out1, ref1, "out1", 3e-4f);
      CheckClose(out2, ref2, "out2", 0.0f); // Silent output: exact zeros

      // The unconnected output must be bit-exact zero
      for (size_t i=0; i<len; ++i)
      {
        assert(out2[i] == 0.0f);
      }
    }
  }
}

void TestResetDiscardsHistory(const std::vector<EdgeDef>& edges)
{
  const size_t preLen = 30;
  const size_t postLen = 64;

  Buf pre0(preLen), pre1(preLen);
  Buf post0(postLen), post1(postLen);
  for (size_t i=0; i<preLen; ++i)
  {
    pre0[i] = PseudoSample(i, 21u);
    pre1[i] = PseudoSample(i, 22u);
  }
  for (size_t i=0; i<postLen; ++i)
  {
    post0[i] = PseudoSample(i, 31u);
    post1[i] = PseudoSample(i, 32u);
  }

  // Convolver 1: play some history, then reset and re-initialize
  fftconvolver::MatrixConvolver mcReset;
  assert(InitMatrix(mcReset, edges));
  {
    Buf o0, o1, o2;
    RunChunked(mcReset, pre0, pre1, preLen, 7, false, o0, o1, o2);
  }
  mcReset.reset();
  assert(InitMatrix(mcReset, edges));

  Buf reset0, reset1, reset2;
  RunChunked(mcReset, post0, post1, postLen, 7, true, reset0, reset1, reset2);

  // Convolver 2: freshly initialized, only the post-reset stream
  fftconvolver::MatrixConvolver mcFresh;
  assert(InitMatrix(mcFresh, edges));
  Buf fresh0, fresh1, fresh2;
  RunChunked(mcFresh, post0, post1, postLen, 7, true, fresh0, fresh1, fresh2);

  // No pre-reset tail may survive: the reset convolver matches the fresh one
  CheckClose(reset0, fresh0, "reset out0", 3e-4f);
  CheckClose(reset1, fresh1, "reset out1", 3e-4f);
  CheckClose(reset2, fresh2, "reset out2", 0.0f);

  // And both match the zero-history time-domain reference
  Buf ref0, ref1, ref2;
  ReferenceOutputs(post0, post1, postLen, edges, ref0, ref1, ref2);
  CheckClose(reset0, ref0, "reset ref out0", 3e-4f);
  CheckClose(reset1, ref1, "reset ref out1", 3e-4f);
}

void TestFFTSharing(const std::vector<EdgeDef>& edges)
{
  // Head-active edges: 5 (all non-empty responses have head partitions)
  const size_t headEdges = 5;
  const size_t headInputs = 2;
  const size_t headOutputs = 2;
  const size_t longTailEdges = 2;
  const size_t tailInputs = 2;
  const size_t tailOutputs = 1;

  // ---- Clean run: 64 frames = 16 head blocks = 4 tail blocks, one call ----
  {
    fftconvolver::MatrixConvolver mc;
    assert(InitMatrix(mc, edges));
    mc.resetFFTStats();

    const size_t len = 64;
    Buf in0(len), in1(len), o0(len), o1(len), o2(len);
    for (size_t i=0; i<len; ++i)
    {
      in0[i] = PseudoSample(i, 41u);
      in1[i] = PseudoSample(i, 42u);
    }
    const fftconvolver::Sample* inputs[NUM_IN] = {in0.data(), in1.data()};
    fftconvolver::Sample* outputs[NUM_OUT] = {o0.data(), o1.data(), o2.data()};
    mc.process(inputs, outputs, len);

    const fftconvolver::MatrixConvolver::FFTStats s = mc.fftStats();

    // Shared forward FFTs: one per active input per head block
    assert(s.headForward == 16 * headInputs);
    // Shared inverse FFTs: one per active output per head block
    assert(s.headInverse == 16 * headOutputs);
    // A per-edge implementation would multiply both counts by the edge count
    assert(s.headForward < 16 * headEdges);
    assert(s.headInverse < 16 * headEdges);

    assert(s.tailForward == 4 * tailInputs);
    assert(s.tailInverse == 4 * tailOutputs);
    assert(s.tailInverse < 4 * longTailEdges);

    std::printf("fft clean run: head fwd=%zu inv=%zu (per-edge would be %zu), "
                "tail fwd=%zu inv=%zu (per-edge inv would be %zu)\n",
                s.headForward, s.headInverse, 16 * headEdges,
                s.tailForward, s.tailInverse, 4 * longTailEdges);
  }

  // ---- Short calls: the recomputed partial-block FFTs must not be multiplied
  //      by the number of connections ----
  {
    fftconvolver::MatrixConvolver mc;
    assert(InitMatrix(mc, edges));
    mc.resetFFTStats();

    float a[1] = {0.25f};
    float b[1] = {-0.5f};
    float z0[1], z1[1], z2[1];
    for (int call=0; call<3; ++call)
    {
      const fftconvolver::Sample* inputs[NUM_IN] = {a, b};
      fftconvolver::Sample* outputs[NUM_OUT] = {z0, z1, z2};
      mc.process(inputs, outputs, 1);
    }

    const fftconvolver::MatrixConvolver::FFTStats s = mc.fftStats();
    assert(s.headForward == 3 * headInputs);
    assert(s.headInverse == 3 * headOutputs);
    assert(s.headForward < 3 * headEdges);
    assert(s.headInverse < 3 * headEdges);
    assert(s.tailForward == 0);
    assert(s.tailInverse == 0);

    std::printf("fft short calls: head fwd=%zu inv=%zu (per-edge would be %zu)\n",
                s.headForward, s.headInverse, 3 * headEdges);
  }
}

void TestZeroLengthDoesNotAdvance(const std::vector<EdgeDef>& edges)
{
  fftconvolver::MatrixConvolver mc;
  assert(InitMatrix(mc, edges));

  mc.resetFFTStats();
  const size_t newBefore = g_newCalls + g_arrayNewCalls;
  const size_t deleteBefore = g_deleteCalls + g_arrayDeleteCalls;

  mc.process(nullptr, nullptr, 0);

  assert(g_newCalls + g_arrayNewCalls == newBefore);
  assert(g_deleteCalls + g_arrayDeleteCalls == deleteBefore);
  const fftconvolver::MatrixConvolver::FFTStats s = mc.fftStats();
  assert(s.headForward == 0 && s.headInverse == 0);
  assert(s.tailForward == 0 && s.tailInverse == 0);

  // Subsequent processing still matches the zero-history reference
  const size_t len = 40;
  Buf in0(len), in1(len), o0(len), o1(len), o2(len);
  for (size_t i=0; i<len; ++i)
  {
    in0[i] = PseudoSample(i, 51u);
    in1[i] = PseudoSample(i, 52u);
  }
  const fftconvolver::Sample* inputs[NUM_IN] = {in0.data(), in1.data()};
  fftconvolver::Sample* outputs[NUM_OUT] = {o0.data(), o1.data(), o2.data()};
  mc.process(inputs, outputs, len);

  Buf ref0, ref1, ref2;
  ReferenceOutputs(in0, in1, len, edges, ref0, ref1, ref2);
  CheckClose(o0, ref0, "zerolen out0", 3e-4f);
  CheckClose(o1, ref1, "zerolen out1", 3e-4f);
}

void TestInitCopiesAndValidates(std::vector<EdgeDef> edges)
{
  // ---- The convolver keeps independent copies of the responses ----
  fftconvolver::MatrixConvolver mc;
  assert(InitMatrix(mc, edges));

  const size_t len = 50;
  Buf in0(len), in1(len);
  for (size_t i=0; i<len; ++i)
  {
    in0[i] = PseudoSample(i, 61u);
    in1[i] = PseudoSample(i, 62u);
  }
  Buf ref0, ref1, ref2;
  ReferenceOutputs(in0, in1, len, edges, ref0, ref1, ref2);

  // Destroy the caller's response data and connection storage
  for (size_t e=0; e<edges.size(); ++e)
  {
    std::fill(edges[e].ir.begin(), edges[e].ir.end(),
              std::numeric_limits<float>::quiet_NaN());
  }
  edges.clear();

  Buf o0(len), o1(len), o2(len);
  const fftconvolver::Sample* inputs[NUM_IN] = {in0.data(), in1.data()};
  fftconvolver::Sample* outputs[NUM_OUT] = {o0.data(), o1.data(), o2.data()};
  mc.process(inputs, outputs, len);
  CheckClose(o0, ref0, "copied out0", 3e-4f);
  CheckClose(o1, ref1, "copied out1", 3e-4f);

  // ---- Explicit failure cases ----
  fftconvolver::MatrixConvolver bad;
  std::vector<EdgeDef> goodEdges = BuildEdges();
  std::vector<Connection> table = BuildConnectionTable(goodEdges, true);

  assert(!bad.init(0, NUM_OUT, HEAD_BLOCK, TAIL_BLOCK, table.data(), table.size()));
  assert(!bad.init(NUM_IN, 0, HEAD_BLOCK, TAIL_BLOCK, table.data(), table.size()));
  assert(!bad.init(NUM_IN, NUM_OUT, 0, TAIL_BLOCK, table.data(), table.size()));
  assert(!bad.init(NUM_IN, NUM_OUT, HEAD_BLOCK, 0, table.data(), table.size()));
  assert(!bad.init(NUM_IN, NUM_OUT, TAIL_BLOCK, HEAD_BLOCK, table.data(), table.size()));
  assert(!bad.init(NUM_IN, NUM_OUT, HEAD_BLOCK, TAIL_BLOCK, nullptr, table.size()));

  {
    std::vector<Connection> illegal = table;
    illegal[0].input = NUM_IN;
    assert(!bad.init(NUM_IN, NUM_OUT, HEAD_BLOCK, TAIL_BLOCK, illegal.data(), illegal.size()));
  }
  {
    std::vector<Connection> illegal = table;
    illegal[1].output = NUM_OUT + 5;
    assert(!bad.init(NUM_IN, NUM_OUT, HEAD_BLOCK, TAIL_BLOCK, illegal.data(), illegal.size()));
  }
  {
    std::vector<Connection> illegal = table;
    illegal[0].ir = nullptr;
    illegal[0].irLen = 4;
    assert(!bad.init(NUM_IN, NUM_OUT, HEAD_BLOCK, TAIL_BLOCK, illegal.data(), illegal.size()));
  }
  {
    Buf nanIr(8, 0.1f);
    nanIr[3] = std::numeric_limits<float>::quiet_NaN();
    std::vector<Connection> illegal = table;
    illegal[0].ir = nanIr.data();
    illegal[0].irLen = nanIr.size();
    assert(!bad.init(NUM_IN, NUM_OUT, HEAD_BLOCK, TAIL_BLOCK, illegal.data(), illegal.size()));
  }
  {
    Buf infIr(8, 0.1f);
    infIr[2] = std::numeric_limits<float>::infinity();
    std::vector<Connection> illegal = table;
    illegal[0].ir = infIr.data();
    illegal[0].irLen = infIr.size();
    assert(!bad.init(NUM_IN, NUM_OUT, HEAD_BLOCK, TAIL_BLOCK, illegal.data(), illegal.size()));
  }

  // A failed init must leave the convolver reset (silent outputs, no crash)
  {
    Buf o0(len), o1(len), o2(len);
    const fftconvolver::Sample* inPtrs[NUM_IN] = {in0.data(), in1.data()};
    fftconvolver::Sample* outPtrs[NUM_OUT] = {o0.data(), o1.data(), o2.data()};
    bad.process(inPtrs, outPtrs, len);
    for (size_t i=0; i<len; ++i)
    {
      assert(o0[i] == 0.0f && o1[i] == 0.0f && o2[i] == 0.0f);
    }
  }

  // An entirely empty connection table is valid: all outputs stay silent
  fftconvolver::MatrixConvolver empty;
  assert(empty.init(NUM_IN, NUM_OUT, HEAD_BLOCK, TAIL_BLOCK, nullptr, 0));
  {
    Buf o0(len, 1.0f), o1(len, 1.0f), o2(len, 1.0f);
    const fftconvolver::Sample* inPtrs[NUM_IN] = {in0.data(), in1.data()};
    fftconvolver::Sample* outPtrs[NUM_OUT] = {o0.data(), o1.data(), o2.data()};
    empty.process(inPtrs, outPtrs, len);
    for (size_t i=0; i<len; ++i)
    {
      assert(o0[i] == 0.0f && o1[i] == 0.0f && o2[i] == 0.0f);
    }
  }
}

void TestVariousConfigurations()
{
  struct Config
  {
    size_t head;
    size_t tail;
    size_t len;
    size_t chunk;
    size_t len0; // in0->out0
    size_t len1; // in1->out0
    size_t len2; // in0->out1
    size_t len3; // duplicate in0->out1
  };
  const Config configs[] = {
    // head == tail (Q = 1), long multi-partition response, 1-frame calls
    {8, 8, 60, 1, 27, 12, 19, 14},
    // tiny head, response spanning 3 tail partitions => full ring wrap
    {2, 8, 70, 31, 33, 22, 25, 19},
    // head-only responses: every IR shorter than the tail block
    {4, 16, 50, 7, 13, 5, 15, 9},
    // tail0-only responses: T < irLen < 2T, no long-tail FFT object, 1-frame calls
    {4, 16, 50, 1, 20, 31, 17, 25},
    // long stream crossing many head and tail boundaries
    {8, 32, 120, 13, 100, 40, 70, 66},
    // minimum head partition size, 7-frame chunks
    {1, 8, 45, 7, 21, 9, 17, 11}
  };

  for (size_t cfg=0; cfg<sizeof(configs)/sizeof(configs[0]); ++cfg)
  {
    const size_t head = configs[cfg].head;
    const size_t tail = configs[cfg].tail;
    const size_t len = configs[cfg].len;
    const size_t lens[4] = {configs[cfg].len0, configs[cfg].len1,
                            configs[cfg].len2, configs[cfg].len3};
    const size_t pairs[4][2] = {{0,0}, {1,0}, {0,1}, {0,1}};

    std::vector<EdgeDef> edges;
    for (size_t k=0; k<4; ++k)
    {
      EdgeDef e;
      e.input = pairs[k][0];
      e.output = pairs[k][1];
      e.ir.resize(lens[k]);
      for (size_t i=0; i<e.ir.size(); ++i)
      {
        e.ir[i] = (i % 5 == 0)
                      ? (0.3f * PseudoSample(i, 100u + 17u * cfg + k))
                      : (0.08f * PseudoSample(i, 200u + 17u * cfg + k));
      }
      edges.push_back(e);
    }
    // Output 2 stays unconnected

    Buf in0(len), in1(len);
    for (size_t i=0; i<len; ++i)
    {
      in0[i] = PseudoSample(i, 500u + cfg);
      in1[i] = PseudoSample(i, 600u + cfg);
    }

    fftconvolver::MatrixConvolver mc;
    std::vector<Connection> table = BuildConnectionTable(edges, false);
    assert(mc.init(2, 3, head, tail, table.data(), table.size()));

    Buf o0, o1, o2;
    RunChunked(mc, in0, in1, len, configs[cfg].chunk, (cfg % 2) != 0, o0, o1, o2);

    Buf ref0, ref1, ref2;
    ReferenceOutputs(in0, in1, len, edges, ref0, ref1, ref2);

    char label[32];
    std::snprintf(label, sizeof(label), "cfg%zu out0", cfg);
    CheckClose(o0, ref0, label, 1e-3f);
    std::snprintf(label, sizeof(label), "cfg%zu out1", cfg);
    CheckClose(o1, ref1, label, 1e-3f);
    CheckClose(o2, ref2, "silent out2", 0.0f);
  }
}

// Cross-checks the matrix against the existing single-channel TwoStageFFTConvolver
// (one instance per edge, summed per output). This proves the matrix reproduces
// the established two-stage head/tail semantics, including zero block latency.
void TestEquivalenceWithTwoStage(const std::vector<EdgeDef>& edges)
{
  const size_t len = 113;
  Buf in0(len), in1(len);
  for (size_t i=0; i<len; ++i)
  {
    in0[i] = PseudoSample(i, 71u);
    in1[i] = PseudoSample(i, 72u);
  }

  // Reference via the library's own two-stage convolvers
  std::vector<fftconvolver::TwoStageFFTConvolver*> singles(edges.size());
  std::vector<Buf> singleOut(edges.size(), Buf(len, 0.0f));
  for (size_t e=0; e<edges.size(); ++e)
  {
    singles[e] = new fftconvolver::TwoStageFFTConvolver();
    assert(singles[e]->init(HEAD_BLOCK, TAIL_BLOCK,
                            edges[e].ir.data(), edges[e].ir.size()));
    const Buf& src = (edges[e].input == 0) ? in0 : in1;
    size_t pos = 0;
    while (pos < len)
    {
      const size_t n = std::min(size_t(7), len - pos);
      singles[e]->process(src.data() + pos, singleOut[e].data() + pos, n);
      pos += n;
    }
  }

  Buf ref0(len, 0.0f), ref1(len, 0.0f), ref2(len, 0.0f);
  for (size_t e=0; e<edges.size(); ++e)
  {
    Buf& dst = (edges[e].output == 0) ? ref0 : ref1;
    for (size_t i=0; i<len; ++i)
    {
      dst[i] += singleOut[e][i];
    }
  }

  for (size_t e=0; e<edges.size(); ++e)
  {
    delete singles[e];
  }

  fftconvolver::MatrixConvolver mc;
  assert(InitMatrix(mc, edges));
  Buf o0, o1, o2;
  RunChunked(mc, in0, in1, len, 31, true, o0, o1, o2);

  CheckClose(o0, ref0, "twostage out0", 3e-4f);
  CheckClose(o1, ref1, "twostage out1", 3e-4f);
  CheckClose(o2, ref2, "twostage out2", 0.0f);
}

} // namespace

int main()
{
  const std::vector<EdgeDef> edges = BuildEdges();

  TestCorrectnessAndAliasing(edges);
  std::puts("correctness + cross-channel in-place aliasing: OK");

  TestVariousConfigurations();
  std::puts("additional matrix configurations (head==tail, ring wrap, head-only, tail0-only): OK");

  TestEquivalenceWithTwoStage(edges);
  std::puts("equivalence with summed TwoStageFFTConvolver instances: OK");

  TestResetDiscardsHistory(edges);
  std::puts("reset discards history: OK");

  TestFFTSharing(edges);
  std::puts("frequency-domain FFT sharing: OK");

  TestZeroLengthDoesNotAdvance(edges);
  std::puts("zero-length call: OK");

  TestInitCopiesAndValidates(edges);
  std::puts("init copies and validation: OK");

  std::puts("MATRIX_CONVOLUTION_OK");
  return 0;
}
