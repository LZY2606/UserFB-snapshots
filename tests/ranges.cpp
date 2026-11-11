// Subresource-range tests for the FrameGraph.
//
// A CPU-only texture (numeric cell array, create/destroy counters and an
// event log) is driven through partial mip/layer/aspect accesses. Every
// scenario is mirrored by an independent per-cell dependency model that
// computes the expected surviving pass set and the expected final values;
// the FrameGraph results are then compared against that model.

#include "fg/FrameGraph.hpp"
#include "fg/Subresource.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kColorAspect = 1u;
constexpr uint32_t kDepthAspect = 2u;
constexpr uint32_t kBothAspects = kColorAspect | kDepthAspect;
constexpr uint32_t kMipCount = 3u;
constexpr uint32_t kLayerCount = 2u;
constexpr uint32_t kCellsPerAspect = kMipCount * kLayerCount; // 6
constexpr uint32_t kCellCount = 2u * kCellsPerAspect;         // 12
constexpr int kUninitialized = -1;

const SubresourceDomain kDomain{kMipCount, kLayerCount, kBothAspects};

uint32_t cellIndex(uint32_t aspect, uint32_t mip, uint32_t layer) {
  const auto aspectIndex = aspect == kColorAspect ? 0u : 1u;
  return aspectIndex * kCellsPerAspect + mip * kLayerCount + layer;
}

// Expands a (already normalized) range into the flat cell indices it covers.
std::vector<uint32_t> expand(const SubresourceRange &r) {
  std::vector<uint32_t> cells;
  for (uint32_t aspectBit = 1u; aspectBit <= kDepthAspect; aspectBit <<= 1) {
    if ((r.getAspectMask() & aspectBit) == 0) continue;
    for (uint32_t mip = r.getFirstMip(); mip < r.getLastMip(); ++mip)
      for (uint32_t layer = r.getFirstLayer(); layer < r.getLastLayer();
           ++layer)
        cells.push_back(cellIndex(aspectBit, mip, layer));
  }
  return cells;
}

void check(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "CHECK FAILED: " << message << '\n';
    std::abort();
  }
}

//
// CPU texture: a real FrameGraph resource T.
// The SubresourceDomain lives outside T::Desc on purpose.
//
struct CpuTexture {
  struct Desc {
    std::string tag;
  };

  inline static int creates{0};
  inline static int destroys{0};
  inline static std::vector<std::string> events;

  static void resetStats() {
    creates = 0;
    destroys = 0;
    events.clear();
  }

  std::array<int, kCellCount> cells{};

  void create(const Desc &desc, void *) {
    ++creates;
    cells.fill(kUninitialized);
    events.push_back("create " + desc.tag);
  }
  void destroy(const Desc &desc, void *) {
    ++destroys;
    events.push_back("destroy " + desc.tag);
  }

  int get(uint32_t aspect, uint32_t mip, uint32_t layer) const {
    return cells[cellIndex(aspect, mip, layer)];
  }
  void set(uint32_t aspect, uint32_t mip, uint32_t layer, int value) {
    cells[cellIndex(aspect, mip, layer)] = value;
  }
};

// Duck-typed adapter so the same fill ops work on the texture and on the
// reference model's plain array.
struct ArrayAdapter {
  std::array<int, kCellCount> *values;

  void set(uint32_t aspect, uint32_t mip, uint32_t layer, int value) {
    (*values)[cellIndex(aspect, mip, layer)] = value;
  }
  int get(uint32_t aspect, uint32_t mip, uint32_t layer) const {
    return (*values)[cellIndex(aspect, mip, layer)];
  }
};

// Fills every cell of a range with a distinctive pass-derived value.
auto fillOp(const SubresourceRange &range, int passId) {
  return [=](auto &target) {
    for (const auto cell : expand(range)) {
      const auto aspect = cell < kCellsPerAspect ? kColorAspect
                                                 : kDepthAspect;
      const auto local = cell % kCellsPerAspect;
      target.set(aspect, local / kLayerCount, local % kLayerCount,
                 passId * 100 + static_cast<int>(cell));
    }
  };
}

std::vector<std::string> executedPassNames() {
  std::vector<std::string> names;
  for (const auto &event : CpuTexture::events) {
    if (event.rfind("exec ", 0) == 0) names.push_back(event.substr(5));
  }
  return names;
}

bool containsName(const std::vector<std::string> &names,
                  const std::string &name) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

//
// Independent per-cell dependency reference model.
//
struct RefPass {
  std::string name;
  bool sideEffect{false};
  std::vector<uint32_t> reads;
  std::vector<uint32_t> writes;
  std::function<void(ArrayAdapter)> apply;
};

struct RefModel {
  bool imported{false};
  std::vector<RefPass> passes;
  std::vector<bool> needed;
  std::array<int, kCellCount> values{};
  std::optional<uint32_t> missingCell;
  std::string missingReader;

  void run() {
    needed.assign(passes.size(), false);
    values.fill(kUninitialized);

    struct Need {
      size_t reader;
      uint32_t cell;
    };
    std::vector<Need> work;
    for (size_t i = 0; i < passes.size(); ++i) {
      if (!passes[i].sideEffect) continue;
      needed[i] = true;
      for (const auto cell : passes[i].reads) work.push_back({i, cell});
    }

    while (!work.empty()) {
      const auto need = work.back();
      work.pop_back();

      // Newest writer registered before the reader.
      std::optional<size_t> producer;
      for (size_t j = 0; j < need.reader; ++j) {
        const auto &w = passes[j].writes;
        if (std::find(w.begin(), w.end(), need.cell) != w.end()) producer = j;
      }
      if (!producer) {
        if (!imported) {
          missingCell = need.cell;
          missingReader = passes[need.reader].name;
        }
        continue;
      }
      if (!needed[*producer]) {
        needed[*producer] = true;
        for (const auto cell : passes[*producer].reads)
          work.push_back({*producer, cell});
      }
    }

    ArrayAdapter adapter{&values};
    for (size_t i = 0; i < passes.size(); ++i)
      if (needed[i] && passes[i].apply) passes[i].apply(adapter);
  }

  [[nodiscard]] std::vector<std::string> neededNames() const {
    std::vector<std::string> names;
    for (size_t i = 0; i < passes.size(); ++i)
      if (needed[i]) names.push_back(passes[i].name);
    return names;
  }
};

struct Handles {
  FrameGraphResource t{-1};
};

// Snapshot of the real texture taken inside the last surviving pass,
// before the framework destroys the object.
std::array<int, kCellCount> g_actualCells{};

//
// Group 1: inheritance across partial covers, explicit read-modify-write,
// a read spanning several partial covers and shared readers.
//
void testInheritance() {
  CpuTexture::resetStats();
  FrameGraph fg;

  const SubresourceRange colorAll{0, 3, 0, 2, kColorAspect};
  const SubresourceRange depthAll{0, 3, 0, 2, kDepthAspect};
  const SubresourceRange colorMip0{0, 1, 0, 2, kColorAspect};
  const SubresourceRange colorMip1{1, 1, 0, 2, kColorAspect};
  const SubresourceRange colorMip2{2, 1, 0, 2, kColorAspect};
  const SubresourceRange colorMip1Layer0{1, 1, 0, 1, kColorAspect};
  const SubresourceRange colorMip2Layer0{2, 1, 0, 1, kColorAspect};

  const auto &p0 = fg.addCallbackPass<Handles>(
    "P0_createColorMip0",
    [&](FrameGraph::Builder &b, Handles &d) {
      d.t = b.create<CpuTexture>("Tex", {"Tex"}, kDomain);
      d.t = b.writeRange(d.t, colorMip0);
    },
    [&](const Handles &d, FrameGraphPassResources &r, void *) {
      CpuTexture::events.push_back("exec P0_createColorMip0");
      fillOp(colorMip0, 0)(r.get<CpuTexture>(d.t));
    });

  const auto &p1 = fg.addCallbackPass<Handles>(
    "P1_fillColorMip1",
    [&](FrameGraph::Builder &b, Handles &d) {
      d.t = b.writeRange(p0.t, colorMip1);
    },
    [&](const Handles &d, FrameGraphPassResources &r, void *) {
      CpuTexture::events.push_back("exec P1_fillColorMip1");
      fillOp(colorMip1, 1)(r.get<CpuTexture>(d.t));
    });

  const auto &p2 = fg.addCallbackPass<Handles>(
    "P2_fillColorMip2",
    [&](FrameGraph::Builder &b, Handles &d) {
      d.t = b.writeRange(p1.t, colorMip2);
    },
    [&](const Handles &d, FrameGraphPassResources &r, void *) {
      CpuTexture::events.push_back("exec P2_fillColorMip2");
      fillOp(colorMip2, 2)(r.get<CpuTexture>(d.t));
    });

  const auto &p3 = fg.addCallbackPass<Handles>(
    "P3_fillDepthAll",
    [&](FrameGraph::Builder &b, Handles &d) {
      d.t = b.writeRange(p2.t, depthAll);
    },
    [&](const Handles &d, FrameGraphPassResources &r, void *) {
      CpuTexture::events.push_back("exec P3_fillDepthAll");
      fillOp(depthAll, 3)(r.get<CpuTexture>(d.t));
    });

  const auto &p4 = fg.addCallbackPass<Handles>(
    "P4_overwriteMip1Layer0",
    [&](FrameGraph::Builder &b, Handles &d) {
      d.t = b.writeRange(p3.t, colorMip1Layer0);
    },
    [&](const Handles &d, FrameGraphPassResources &r, void *) {
      CpuTexture::events.push_back("exec P4_overwriteMip1Layer0");
      fillOp(colorMip1Layer0, 4)(r.get<CpuTexture>(d.t));
    });

  // Read spanning three partial covers; side-effect keeps the graph alive.
  const auto &p5 = fg.addCallbackPass<Handles>(
    "P5_readSpanning",
    [&](FrameGraph::Builder &b, Handles &d) {
      d.t = b.readRange(p4.t, colorAll);
      b.setSideEffect();
    },
    [&](const Handles &d, FrameGraphPassResources &r, void *) {
      CpuTexture::events.push_back("exec P5_readSpanning");
      (void)r.get<CpuTexture>(d.t);
    });

  // Explicit read-modify-write of one cell.
  const auto &p6 = fg.addCallbackPass<Handles>(
    "P6_rmwMip2Layer0",
    [&](FrameGraph::Builder &b, Handles &d) {
      const auto in = b.readRange(p5.t, colorMip2Layer0);
      d.t = b.writeRange(in, colorMip2Layer0);
      b.setSideEffect();
    },
    [&](const Handles &d, FrameGraphPassResources &r, void *) {
      CpuTexture::events.push_back("exec P6_rmwMip2Layer0");
      auto &tex = r.get<CpuTexture>(d.t);
      tex.set(kColorAspect, 2, 0, tex.get(kColorAspect, 2, 0) * 10);
    });

  // Second reader sharing the mip0 region; it is the last surviving access
  // and snapshots the real texture before it gets destroyed.
  fg.addCallbackPass<Handles>(
    "P7_sharedReader",
    [&](FrameGraph::Builder &b, Handles &d) {
      d.t = b.readRange(p6.t, colorMip0);
      b.setSideEffect();
    },
    [&](const Handles &d, FrameGraphPassResources &r, void *) {
      CpuTexture::events.push_back("exec P7_sharedReader");
      g_actualCells = r.get<CpuTexture>(d.t).cells;
    });

  fg.compile();
  fg.execute();

  // -- Independent expectation:
  RefModel ref;
  ref.passes = {
    {"P0_createColorMip0", false, {}, expand(colorMip0),
     [=](ArrayAdapter a) { fillOp(colorMip0, 0)(a); }},
    {"P1_fillColorMip1", false, {}, expand(colorMip1),
     [=](ArrayAdapter a) { fillOp(colorMip1, 1)(a); }},
    {"P2_fillColorMip2", false, {}, expand(colorMip2),
     [=](ArrayAdapter a) { fillOp(colorMip2, 2)(a); }},
    {"P3_fillDepthAll", false, {}, expand(depthAll),
     [=](ArrayAdapter a) { fillOp(depthAll, 3)(a); }},
    {"P4_overwriteMip1Layer0", false, {}, expand(colorMip1Layer0),
     [=](ArrayAdapter a) { fillOp(colorMip1Layer0, 4)(a); }},
    {"P5_readSpanning", true, expand(colorAll), {}, {}},
    {"P6_rmwMip2Layer0",
     true,
     expand(colorMip2Layer0),
     expand(colorMip2Layer0),
     [](ArrayAdapter a) {
       a.set(kColorAspect, 2, 0, a.get(kColorAspect, 2, 0) * 10);
     }},
    {"P7_sharedReader", true, expand(colorMip0), {}, {}},
  };
  ref.run();
  check(!ref.missingCell.has_value(),
        "reference model should be fully defined");

  const auto actualNames = executedPassNames();
  check(actualNames == ref.neededNames(),
        "inheritance: surviving pass set mismatch");

  // P3 (depth-only producer) must be culled: no surviving depth reader.
  check(!containsName(actualNames, "P3_fillDepthAll"),
        "depth-only producer should be culled");
  // P1 must survive: P4 only overwrites one of its two cells.
  check(containsName(actualNames, "P1_fillColorMip1"),
        "partially covered producer must survive");
  // P0 must survive: shared by the spanning read and P7.
  check(containsName(actualNames, "P0_createColorMip0"),
        "shared reader must keep the upstream producer alive");
  // P6 executes once even though it both reads and writes.
  check(std::count(actualNames.begin(), actualNames.end(),
                   "P6_rmwMip2Layer0") == 1,
        "RMW pass executes exactly once");

  // The snapshot was taken from the real texture inside the last pass;
  // compare it cell by cell against the independent reference model.
  check(g_actualCells == ref.values,
        "inheritance: real texture array mismatch with reference model");
  // Depth cells were never written by a surviving pass and stay
  // uninitialized rather than being silently zeroed.
  for (uint32_t mip = 0; mip < kMipCount; ++mip)
    for (uint32_t layer = 0; layer < kLayerCount; ++layer)
      check(g_actualCells[cellIndex(kDepthAspect, mip, layer)] ==
              kUninitialized,
            "culled depth writes must leave cells uninitialized");

  std::cout << "RANGE_INHERITANCE_OK\n";
}

//
// Group 2: culling - dead create pass, unused resource, fully covered
// producer.
//
void testCulling() {
  CpuTexture::resetStats();
  FrameGraph fg;

  struct TwoHandles {
    FrameGraphResource u{-1};
    FrameGraphResource v{-1};
  };

  const SubresourceRange mip2Only{2, 1, 0, 2, kColorAspect};
  const SubresourceRange mip0Only{0, 1, 0, 2, kColorAspect};

  // Create pass only writes a mip nobody consumes -> culled, but the
  // resource itself is still needed by later surviving passes.
  const auto &q0 = fg.addCallbackPass<TwoHandles>(
    "Q0_deadCreate",
    [&](FrameGraph::Builder &b, TwoHandles &d) {
      d.u = b.create<CpuTexture>("U", {"U"}, kDomain);
      d.u = b.writeRange(d.u, mip2Only);
    },
    [&](const TwoHandles &, FrameGraphPassResources &, void *) {
      CpuTexture::events.push_back("exec Q0_deadCreate");
    });

  const auto &q1 = fg.addCallbackPass<TwoHandles>(
    "Q1_liveFill",
    [&](FrameGraph::Builder &b, TwoHandles &d) {
      d.u = b.writeRange(q0.u, mip0Only);
    },
    [&](const TwoHandles &d, FrameGraphPassResources &r, void *) {
      CpuTexture::events.push_back("exec Q1_liveFill");
      r.get<CpuTexture>(d.u).set(kColorAspect, 0, 0, 11);
      r.get<CpuTexture>(d.u).set(kColorAspect, 0, 1, 12);
    });

  fg.addCallbackPass<TwoHandles>(
    "Q2_reader",
    [&](FrameGraph::Builder &b, TwoHandles &d) {
      d.u = b.readRange(q1.u, mip0Only);
      b.setSideEffect();
    },
    [&](const TwoHandles &d, FrameGraphPassResources &r, void *) {
      CpuTexture::events.push_back("exec Q2_reader");
      (void)r.get<CpuTexture>(d.u);
    });

  // A resource nobody ever reads: entirely culled, never allocated.
  fg.addCallbackPass<TwoHandles>(
    "Q3_unusedResource",
    [&](FrameGraph::Builder &b, TwoHandles &d) {
      d.v = b.create<CpuTexture>("V", {"V"}, kDomain);
      d.v = b.writeRange(d.v, mip0Only);
    },
    [&](const TwoHandles &, FrameGraphPassResources &, void *) {
      CpuTexture::events.push_back("exec Q3_unusedResource");
    });

  // Fully covered producer: W0 loses every cell to W1.
  const auto &w0 = fg.addCallbackPass<TwoHandles>(
    "W0_coveredProducer",
    [&](FrameGraph::Builder &b, TwoHandles &d) {
      d.v = b.create<CpuTexture>("W", {"W"}, kDomain);
      d.v = b.writeRange(d.v, mip0Only);
    },
    [&](const TwoHandles &d, FrameGraphPassResources &r, void *) {
      CpuTexture::events.push_back("exec W0_coveredProducer");
      r.get<CpuTexture>(d.v).set(kColorAspect, 0, 0, 1);
    });

  const auto &w1 = fg.addCallbackPass<TwoHandles>(
    "W1_rewrite",
    [&](FrameGraph::Builder &b, TwoHandles &d) {
      d.v = b.writeRange(w0.v, mip0Only);
    },
    [&](const TwoHandles &d, FrameGraphPassResources &r, void *) {
      CpuTexture::events.push_back("exec W1_rewrite");
      r.get<CpuTexture>(d.v).set(kColorAspect, 0, 0, 2);
      r.get<CpuTexture>(d.v).set(kColorAspect, 0, 1, 3);
    });

  fg.addCallbackPass<TwoHandles>(
    "W2_reader",
    [&](FrameGraph::Builder &b, TwoHandles &d) {
      d.v = b.readRange(w1.v, mip0Only);
      b.setSideEffect();
    },
    [&](const TwoHandles &d, FrameGraphPassResources &r, void *) {
      CpuTexture::events.push_back("exec W2_reader");
      (void)r.get<CpuTexture>(d.v);
    });

  fg.compile();
  fg.execute();

  const auto names = executedPassNames();
  check(!containsName(names, "Q0_deadCreate"),
        "create pass writing unused mip must be culled");
  check(containsName(names, "Q1_liveFill") &&
          containsName(names, "Q2_reader"),
        "live U passes must survive");
  check(!containsName(names, "Q3_unusedResource"),
        "unreferenced resource pass must be culled");
  check(!containsName(names, "W0_coveredProducer"),
        "producer fully overwritten downstream must be culled");
  check(containsName(names, "W1_rewrite") &&
          containsName(names, "W2_reader"),
        "W chain survivors");

  // U and W are allocated once each; V never is.
  check(CpuTexture::creates == 2, "exactly two transient allocations");
  check(CpuTexture::destroys == 2, "exactly two transient destructions");
  check(std::none_of(CpuTexture::events.begin(), CpuTexture::events.end(),
                     [](const std::string &e) {
                       return e.find("create V") != std::string::npos;
                     }),
        "unreferenced resource must never be created");

  std::cout << "RANGE_CULLING_OK\n";
}

//
// A pass writing several regions of one resource in a single callback still
// executes exactly once even when different regions feed different readers.
//
void testMultiRegionWriteOnce() {
  CpuTexture::resetStats();
  FrameGraph fg;

  const SubresourceRange mip0{0, 1, 0, 2, kColorAspect};
  const SubresourceRange mip1{1, 1, 0, 2, kColorAspect};

  const auto &producer = fg.addCallbackPass<Handles>(
    "M0_twoRegions",
    [&](FrameGraph::Builder &b, Handles &d) {
      d.t = b.create<CpuTexture>("M", {"M"}, kDomain);
      d.t = b.writeRange(d.t, mip0);
      d.t = b.writeRange(d.t, mip1);
    },
    [&](const Handles &d, FrameGraphPassResources &r, void *) {
      CpuTexture::events.push_back("exec M0_twoRegions");
      r.get<CpuTexture>(d.t).set(kColorAspect, 0, 0, 1);
      r.get<CpuTexture>(d.t).set(kColorAspect, 1, 0, 2);
    });

  fg.addCallbackPass<Handles>(
    "M1_readMip0",
    [&](FrameGraph::Builder &b, Handles &d) {
      d.t = b.readRange(producer.t, mip0);
      b.setSideEffect();
    },
    [&](const Handles &d, FrameGraphPassResources &r, void *) {
      CpuTexture::events.push_back("exec M1_readMip0");
      (void)r.get<CpuTexture>(d.t);
    });
  fg.addCallbackPass<Handles>(
    "M2_readMip1",
    [&](FrameGraph::Builder &b, Handles &d) {
      d.t = b.readRange(producer.t, mip1);
      b.setSideEffect();
    },
    [&](const Handles &d, FrameGraphPassResources &r, void *) {
      CpuTexture::events.push_back("exec M2_readMip1");
      (void)r.get<CpuTexture>(d.t);
    });

  fg.compile();
  fg.execute();

  const auto names = executedPassNames();
  check(std::count(names.begin(), names.end(), "M0_twoRegions") == 1,
        "multi-region writer executes exactly once");
  check(CpuTexture::creates == 1 && CpuTexture::destroys == 1,
        "one T object for the whole logical resource");
}

//
// A retained producer reads a *different* resource; that upstream producer
// must be retained as well even though the triggering demand came through
// another resource.
//
void testCrossResourceTracking() {
  CpuTexture::resetStats();
  FrameGraph fg;

  const SubresourceRange mip0{0, 1, 0, 2, kColorAspect};
  struct Two {
    FrameGraphResource a{-1};
    FrameGraphResource b{-1};
  };

  const auto &s0 = fg.addCallbackPass<Two>(
    "S0_produceA",
    [&](FrameGraph::Builder &b, Two &d) {
      d.a = b.create<CpuTexture>("A", {"A"}, kDomain);
      d.a = b.writeRange(d.a, mip0);
    },
    [&](const Two &d, FrameGraphPassResources &r, void *) {
      CpuTexture::events.push_back("exec S0_produceA");
      r.get<CpuTexture>(d.a).set(kColorAspect, 0, 0, 41);
    });

  const auto &p1 = fg.addCallbackPass<Two>(
    "P1_consumeAProduceB",
    [&](FrameGraph::Builder &b, Two &d) {
      d.a = b.readRange(s0.a, mip0); // cross-resource input
      d.b = b.create<CpuTexture>("B", {"B"}, kDomain);
      d.b = b.writeRange(d.b, mip0);
    },
    [&](const Two &d, FrameGraphPassResources &r, void *) {
      CpuTexture::events.push_back("exec P1_consumeAProduceB");
      const auto input = r.get<CpuTexture>(d.a).get(kColorAspect, 0, 0);
      r.get<CpuTexture>(d.b).set(kColorAspect, 0, 0, input + 1);
    });

  bool sawValue = false;
  fg.addCallbackPass<Two>(
    "P2_consumeB",
    [&](FrameGraph::Builder &b, Two &d) {
      d.b = b.readRange(p1.b, mip0);
      b.setSideEffect();
    },
    [&](const Two &d, FrameGraphPassResources &r, void *) {
      CpuTexture::events.push_back("exec P2_consumeB");
      sawValue =
        r.get<CpuTexture>(d.b).get(kColorAspect, 0, 0) == 42;
    });

  fg.compile();
  fg.execute();

  const auto names = executedPassNames();
  check(containsName(names, "S0_produceA"),
        "upstream producer of a retained pass's cross-resource read survives");
  check(containsName(names, "P1_consumeAProduceB") &&
          containsName(names, "P2_consumeB"),
        "the rest of the chain survives");
  check(sawValue, "value propagated across resources in registration order");
  // Both A and B are real, distinct, single objects.
  check(CpuTexture::creates == 2 && CpuTexture::destroys == 2,
        "two logical resources, each allocated and destroyed once");
}

//
// Group 3: lifetime brackets every surviving access; imported resources
// are neither created nor destroyed, yet writes to them are retained.
//
void testLifetime() {
  CpuTexture::resetStats();
  FrameGraph fg;

  const SubresourceRange mip0{0, 1, 0, 2, kColorAspect};
  const SubresourceRange mip1{1, 1, 0, 2, kColorAspect};

  const auto &l0 = fg.addCallbackPass<Handles>(
    "L0_create",
    [&](FrameGraph::Builder &b, Handles &d) {
      d.t = b.create<CpuTexture>("L", {"L"}, kDomain);
      d.t = b.writeRange(d.t, mip0);
    },
    [&](const Handles &d, FrameGraphPassResources &r, void *) {
      CpuTexture::events.push_back("exec L0_create");
      r.get<CpuTexture>(d.t).set(kColorAspect, 0, 0, 5);
    });

  const auto &l1 = fg.addCallbackPass<Handles>(
    "L1_fillMip1",
    [&](FrameGraph::Builder &b, Handles &d) {
      d.t = b.writeRange(l0.t, mip1);
    },
    [&](const Handles &d, FrameGraphPassResources &r, void *) {
      CpuTexture::events.push_back("exec L1_fillMip1");
      r.get<CpuTexture>(d.t).set(kColorAspect, 1, 0, 6);
    });

  // A culled pass in between: must not influence lifetime brackets. It
  // still renames the handle during setup, so later passes consume the
  // newest version even though the callback itself never runs.
  FrameGraphResource afterLx{-1};
  fg.addCallbackPass<Handles>(
    "Lx_culled",
    [&](FrameGraph::Builder &b, Handles &d) {
      d.t = b.writeRange(l1.t, SubresourceRange{2, 1, 0, 1, kColorAspect});
      afterLx = d.t;
    },
    [&](const Handles &, FrameGraphPassResources &, void *) {
      CpuTexture::events.push_back("exec Lx_culled");
    });

  fg.addCallbackPass<Handles>(
    "L2_readMip0",
    [&](FrameGraph::Builder &b, Handles &d) {
      d.t = b.readRange(afterLx, mip0);
      b.setSideEffect();
    },
    [&](const Handles &d, FrameGraphPassResources &r, void *) {
      CpuTexture::events.push_back("exec L2_readMip0");
      (void)r.get<CpuTexture>(d.t);
    });

  fg.addCallbackPass<Handles>(
    "L3_readMip1",
    [&](FrameGraph::Builder &b, Handles &d) {
      d.t = b.readRange(afterLx, mip1);
      b.setSideEffect();
    },
    [&](const Handles &d, FrameGraphPassResources &r, void *) {
      CpuTexture::events.push_back("exec L3_readMip1");
      (void)r.get<CpuTexture>(d.t);
    });

  // Imported resource: a write with no readers is retained automatically,
  // but no create/destroy events are emitted for it.
  CpuTexture imported;
  imported.cells.fill(7);
  const auto external = fg.import<CpuTexture>(
    "External", {"External"}, std::move(imported), kDomain);
  bool importedWriteRan = false;
  fg.addCallbackPass<Handles>(
    "L4_importedWrite",
    [&](FrameGraph::Builder &b, Handles &d) {
      d.t = b.writeRange(external, mip0);
    },
    [&](const Handles &d, FrameGraphPassResources &r, void *) {
      CpuTexture::events.push_back("exec L4_importedWrite");
      r.get<CpuTexture>(d.t).set(kColorAspect, 0, 0, 9);
      importedWriteRan = true;
    });

  fg.compile();
  fg.execute();

  const auto &events = CpuTexture::events;
  const auto pos = [&](const std::string &e) {
    const auto it = std::find(events.begin(), events.end(), e);
    check(it != events.end(), "missing event: " + e);
    return static_cast<size_t>(it - events.begin());
  };

  const auto createL = pos("create L");
  const auto destroyL = pos("destroy L");
  check(createL < pos("exec L0_create"),
        "create must precede first surviving access");
  check(destroyL > pos("exec L3_readMip1"),
        "destroy must follow last surviving access");

  // Every surviving access of L sits between the two lifetime events.
  for (const auto &access :
       {"exec L0_create", "exec L1_fillMip1", "exec L2_readMip0",
        "exec L3_readMip1"}) {
    const auto i = pos(access);
    check(createL < i && i < destroyL,
          std::string{"access '"} + access + "' outside lifetime brackets");
  }
  check(!containsName(executedPassNames(), "Lx_culled"),
        "the middle pass must be culled");

  check(std::none_of(events.begin(), events.end(),
                     [](const std::string &e) {
                       return e.find("External") != std::string::npos;
                     }),
        "imported resource must not be created or destroyed");
  check(importedWriteRan, "write to imported resource is auto-retained");
  check(CpuTexture::creates == 1 && CpuTexture::destroys == 1,
        "only the transient resource is allocated");

  std::cout << "RANGE_LIFETIME_OK\n";
}

//
// Group 4: illegal declarations are rejected without executing callbacks,
// and uninitialized transient reads are reported with full details.
//
void testErrors() {
  // -- Invalid domain:
  {
    bool threw = false;
    try {
      FrameGraph fg;
      fg.addCallbackPass(
        "badDomain",
        [&](FrameGraph::Builder &b, FrameGraph::NoData &) {
          (void)b.create<CpuTexture>("bad", {"bad"},
                                     SubresourceDomain{0, 2, kColorAspect});
        },
        [](const FrameGraph::NoData &, FrameGraphPassResources &, void *) {
          CpuTexture::events.push_back("exec must-not-run");
        });
    } catch (const std::invalid_argument &) {
      threw = true;
    }
    check(threw, "zero-mip domain must be rejected");
  }

  // Helper: graph with one color mip0 already written.
  auto makeGraphWithMip0 = [](FrameGraph &fg, FrameGraphResource &h) {
    fg.addCallbackPass<Handles>(
      "creator",
      [&](FrameGraph::Builder &b, Handles &d) {
        d.t = b.create<CpuTexture>("t", {"t"}, kDomain);
        d.t = b.writeRange(d.t,
                           SubresourceRange{0, 1, 0, 2, kColorAspect});
        h = d.t;
      },
      [](const Handles &, FrameGraphPassResources &, void *) {});
  };

  // -- Empty range:
  {
    bool threw = false;
    try {
      FrameGraph fg;
      FrameGraphResource h{-1};
      makeGraphWithMip0(fg, h);
      fg.addCallbackPass<Handles>(
        "emptyRange",
        [&](FrameGraph::Builder &b, Handles &d) {
          d.t = b.writeRange(h,
                             SubresourceRange{0, 0, 0, 2, kColorAspect});
        },
        [](const Handles &, FrameGraphPassResources &, void *) {
          CpuTexture::events.push_back("exec must-not-run");
        });
    } catch (const std::invalid_argument &) {
      threw = true;
    }
    check(threw, "empty range must be rejected");
  }

  // -- Out-of-bounds range:
  {
    bool threw = false;
    try {
      FrameGraph fg;
      FrameGraphResource h{-1};
      makeGraphWithMip0(fg, h);
      fg.addCallbackPass<Handles>(
        "outOfBounds",
        [&](FrameGraph::Builder &b, Handles &d) {
          d.t = b.readRange(h,
                            SubresourceRange{2, 2, 0, 2, kColorAspect});
        },
        [](const Handles &, FrameGraphPassResources &, void *) {
          CpuTexture::events.push_back("exec must-not-run");
        });
    } catch (const std::invalid_argument &) {
      threw = true;
    }
    check(threw, "out-of-bounds range must be rejected");
  }

  // -- Illegal aspect:
  {
    bool threw = false;
    try {
      FrameGraph fg;
      FrameGraphResource h{-1};
      makeGraphWithMip0(fg, h);
      fg.addCallbackPass<Handles>(
        "badAspect",
        [&](FrameGraph::Builder &b, Handles &d) {
          d.t = b.readRange(h, SubresourceRange{0, 1, 0, 1, 4u});
        },
        [](const Handles &, FrameGraphPassResources &, void *) {
          CpuTexture::events.push_back("exec must-not-run");
        });
    } catch (const std::invalid_argument &) {
      threw = true;
    }
    check(threw, "illegal aspect must be rejected");
    check(std::none_of(CpuTexture::events.begin(), CpuTexture::events.end(),
                       [](const std::string &e) {
                         return e.find("must-not-run") != std::string::npos;
                       }),
          "rejected declarations must not run callbacks");
  }

  // -- Uninitialized transient read:
  {
    FrameGraph fg;
    struct Data {
      FrameGraphResource t{-1};
    };
    const auto &writer = fg.addCallbackPass<Data>(
      "writerMip0",
      [&](FrameGraph::Builder &b, Data &d) {
        d.t = b.create<CpuTexture>("Partial", {"Partial"}, kDomain);
        d.t = b.writeRange(d.t,
                           SubresourceRange{0, 1, 0, 2, kColorAspect});
      },
      [&](const Data &d, FrameGraphPassResources &r, void *) {
        r.get<CpuTexture>(d.t).set(kColorAspect, 0, 0, 1);
      });

    bool executed = false;
    fg.addCallbackPass<Data>(
      "readerMip1",
      [&](FrameGraph::Builder &b, Data &d) {
        d.t = b.readRange(writer.t,
                          SubresourceRange{1, 1, 0, 2, kColorAspect});
        b.setSideEffect();
      },
      [&](const Data &, FrameGraphPassResources &, void *) {
        executed = true;
      });

    bool caught = false;
    try {
      fg.compile();
    } catch (const FrameGraphRangeError &e) {
      caught = true;
      check(e.getPassName() == "readerMip1", "error names the pass");
      check(e.getResourceName() == "Partial", "error names the resource");
      const auto missing = e.getMissingRange();
      check(missing.getFirstMip() == 1 && missing.getMipCount() == 1,
            "error carries the missing mip range");
      check(missing.getAspectMask() == kColorAspect,
            "error carries the missing aspect");
    }
    check(caught, "uninitialized transient read must throw at compile time");
    check(!executed, "execute callbacks must not run after compile error");
  }

  // -- The same read is legal for an imported resource (fully defined):
  {
    CpuTexture::resetStats();
    FrameGraph fg;
    CpuTexture external;
    external.cells.fill(3);
    const auto h = fg.import<CpuTexture>("Imported", {"Imported"},
                                         std::move(external), kDomain);
    bool ran = false;
    fg.addCallbackPass<Handles>(
      "importedReader",
      [&](FrameGraph::Builder &b, Handles &d) {
        d.t = b.readRange(h,
                          SubresourceRange{2, 1, 1, 1, kDepthAspect});
        b.setSideEffect();
      },
      [&](const Handles &d, FrameGraphPassResources &r, void *) {
        check(r.get<CpuTexture>(d.t).get(kDepthAspect, 2, 1) == 3,
              "imported cells are pre-defined");
        ran = true;
      });
    fg.compile(); // Must not throw.
    fg.execute();
    check(ran, "imported read executes");
    check(CpuTexture::creates == 0 && CpuTexture::destroys == 0,
          "imported resource is not allocated");
  }

  std::cout << "RANGE_ERRORS_OK\n";
}

} // namespace

int main() {
  testInheritance();
  testCulling();
  testMultiRegionWriteOnce();
  testCrossResourceTracking();
  testLifetime();
  testErrors();
  std::cout << "ALL RANGE TESTS PASSED\n";
  return 0;
}
