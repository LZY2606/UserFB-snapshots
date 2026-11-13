// Regression tests for AArch64 instruction relocation under a split
// "instruction bytes" / "virtual address" model.
//
// The caller owns a readable copy of the instruction bytes at a host address
// that has nothing to do with the virtual address the instructions occupy in
// the firmware image. GenRelocateCode must fetch instructions only from the
// buffer and compute every PC-relative target from origin->addr().
//
// The generated sequence is checked with an independent bit-field decoder and
// a small restricted interpreter. No generated machine code is executed here,
// and the offline memory adapter only stores bytes: it does not install any
// process hook.

#include "InstructionRelocation/InstructionRelocation.h"

#include "../support/cases.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t kNop = 0xD503201Fu;
constexpr uint32_t kBrX17 = 0xD61F0220u;
constexpr int kTmpReg = 17;

// ---- independent instruction encoders (input instructions) -----------------

uint32_t encode_ldr_literal(int opc, int v, int64_t byte_offset, int rt) {
  int64_t imm19 = byte_offset >> 2;
  return 0x18000000u | (uint32_t(opc) << 30) | (uint32_t(v) << 26) |
         (uint32_t(imm19 & 0x7FFFF) << 5) | uint32_t(rt & 0x1F);
}

uint32_t encode_b(int64_t byte_offset) {
  int64_t imm26 = byte_offset >> 2;
  return 0x14000000u | uint32_t(imm26 & 0x3FFFFFF);
}

uint32_t encode_adr(int64_t byte_offset, int rd) {
  uint64_t imm = uint64_t(byte_offset);
  uint32_t immlo = uint32_t(imm & 0x3);
  uint32_t immhi = uint32_t((imm >> 2) & 0x7FFFF);
  return 0x10000000u | (immlo << 29) | (immhi << 5) | uint32_t(rd & 0x1F);
}

uint32_t encode_adrp(int64_t byte_offset, int rd) {
  // The byte offset must be page aligned; the page number goes into immhi:immlo.
  uint64_t pages = uint64_t(byte_offset >> 12);
  uint32_t immlo = uint32_t(pages & 0x3);
  uint32_t immhi = uint32_t((pages >> 2) & 0x7FFFF);
  return 0x90000000u | (immlo << 29) | (immhi << 5) | uint32_t(rd & 0x1F);
}

// ---- independent instruction decoders --------------------------------------

inline uint32_t field(uint32_t w, int lo, int hi) {
  return (w >> lo) & ((1u << (hi - lo + 1)) - 1);
}

bool is_move_wide(uint32_t w) {
  return (w & 0x1F800000u) == 0x12800000u;
}

bool is_unsigned_load_store(uint32_t w) {
  return (w & 0x3B000000u) == 0x39000000u;
}

struct LoadInfo {
  bool valid = false;
  bool simd = false;
  int size = 0; // bits 31:30
  int opc = 0;  // bits 23:22
  int rn = 0;
  int rt = 0;
  int imm12 = 0;

  int access_bytes() const {
    if (simd) {
      if (size == 0 && opc == 1)
        return 1; // LDR B
      if (size == 0 && opc == 3)
        return 16; // LDR Q
      if (size == 1 && opc == 1)
        return 2; // LDR H
      if (size == 2 && opc == 1)
        return 4; // LDR S
      if (size == 3 && opc == 1)
        return 8; // LDR D
      return 0;
    }
    return 1 << size;
  }
};

LoadInfo decode_unsigned_load_store(uint32_t w) {
  LoadInfo info;
  if (!is_unsigned_load_store(w))
    return info;
  info.valid = true;
  info.simd = field(w, 26, 26) != 0;
  info.size = int(field(w, 30, 31));
  info.opc = int(field(w, 22, 23));
  info.imm12 = int(field(w, 10, 21));
  info.rn = int(field(w, 5, 9));
  info.rt = int(field(w, 0, 4));
  return info;
}

struct LiteralInfo {
  bool valid = false;
  int opc = 0;
  int v = 0;
  int rt = 0;
  int64_t offset = 0;
};

LiteralInfo decode_literal_load(uint32_t w) {
  LiteralInfo info;
  if ((w & 0x3B000000u) != 0x18000000u)
    return info;
  info.valid = true;
  info.opc = int(field(w, 30, 31));
  info.v = int(field(w, 26, 26));
  info.rt = int(field(w, 0, 4));
  int64_t imm19 = int64_t(field(w, 5, 23));
  if (imm19 & (1 << 18))
    imm19 |= ~((1ll << 19) - 1);
  info.offset = imm19 << 2;
  return info;
}

// ---- virtual-address -> host-bytes map for the interpreter -----------------

struct Region {
  uint64_t base;
  const uint8_t *bytes;
  size_t size;
};

struct AddressSpace {
  std::vector<Region> regions;

  void map(uint64_t base, const void *bytes, size_t size) {
    regions.push_back(Region{base, (const uint8_t *)bytes, size});
  }

  const uint8_t *find(uint64_t addr, size_t size) const {
    for (const auto &r : regions) {
      if (addr >= r.base && addr + size <= r.base + r.size)
        return r.bytes + (addr - r.base);
    }
    return nullptr;
  }

  uint64_t read(uint64_t addr, size_t size) const {
    const uint8_t *p = find(addr, size);
    if (!p)
      throw std::runtime_error("interpreter read from unmapped address");
    uint64_t value = 0;
    std::memcpy(&value, p, size);
    return value;
  }
};

// ---- restricted interpreter ------------------------------------------------

// Models only the instruction forms relocation is allowed to emit: move-wide,
// one load of any supported class from [x17, #0], a PC-relative literal load
// for in-stub data, and a final `br x17`. Anything else is a test failure,
// which catches unexpected registers, SP usage, flag writes or constant
// inlining. Literal loads inside the generated sequence are resolved against
// the relocated image itself (this is how the branch stub reads its own data
// slot), so that path is modeled too.
struct Machine {
  uint64_t x[32] = {};
  uint8_t v[32][16] = {};
  AddressSpace space;
  const std::vector<uint32_t> *code = nullptr;
  uint64_t relo_base = 0;
  size_t pc = 0;
  bool prefetch_seen = false;
  bool memory_accessed = false;
  bool branched = false;

  void init_state() {
    for (int i = 0; i < 32; ++i)
      x[i] = 0xA5A5A5A500000000ull | uint64_t(i);
    for (auto &reg : v)
      std::memset(reg, 0xAA, sizeof(reg));
  }

  uint64_t read_code(uint64_t offset, size_t size) const {
    if (!code || offset + size > code->size() * 4)
      throw std::runtime_error("literal load escaped the relocated image");
    uint8_t bytes[8] = {};
    std::memcpy(bytes, reinterpret_cast<const uint8_t *>(code->data()) + offset, size);
    uint64_t value = 0;
    std::memcpy(&value, bytes, size);
    return value;
  }

  void step(uint32_t w) {
    if (w == kNop)
      return;

    if (w == kBrX17) {
      branched = true;
      return;
    }

    if (is_move_wide(w)) {
      int opc = int(field(w, 29, 30));
      int hw = int(field(w, 21, 22));
      int rd = int(field(w, 0, 4));
      uint64_t imm16 = field(w, 5, 20);
      uint64_t mask = 0xFFFFull << (16 * hw);
      uint64_t imm = imm16 << (16 * hw);
      if (opc == 0b10) { // MOVZ
        if (rd != 31)
          x[rd] = imm;
      } else if (opc == 0b11) { // MOVK
        if (rd != 31)
          x[rd] = (x[rd] & ~mask) | imm;
      } else {
        throw std::runtime_error("unexpected move-wide opcode in generated code");
      }
      return;
    }

    // PC-relative literal load of in-stub data.
    LiteralInfo lit = decode_literal_load(w);
    if (lit.valid) {
      uint64_t target = relo_base + pc * 4 + uint64_t(lit.offset);
      uint64_t offset = target - relo_base;
      if (lit.v == 0) {
        if (lit.opc == 0b01) {
          if (lit.rt != 31)
            x[lit.rt] = read_code(offset, 8);
          return;
        }
        if (lit.opc == 0b11) { // PRFM
          prefetch_seen = true;
          return;
        }
      }
      throw std::runtime_error("unexpected literal load in generated code");
    }

    LoadInfo info = decode_unsigned_load_store(w);
    if (!info.valid)
      throw std::runtime_error("unexpected instruction in generated code");
    if (info.rn == 31)
      throw std::runtime_error("generated code uses SP as load base");
    if (info.imm12 != 0)
      throw std::runtime_error("generated code uses non-zero load offset");

    uint64_t addr = x[info.rn];
    if (!info.simd) {
      if (info.size == 3 && info.opc == 0b10) { // PRFM (unsigned offset)
        prefetch_seen = true;
        return; // hint only: no memory access, no register write
      }
      if (info.opc == 0b01) { // LDR Wt / LDR Xt
        uint64_t value = space.read(addr, info.access_bytes());
        memory_accessed = true;
        if (info.rt != 31)
          x[info.rt] = value;
        return;
      }
      if (info.opc == 0b10) { // LDRSW: sign-extend 32 -> 64
        uint32_t raw = uint32_t(space.read(addr, 4));
        memory_accessed = true;
        if (info.rt != 31)
          x[info.rt] = uint64_t(int64_t(int32_t(raw)));
        return;
      }
      throw std::runtime_error("unexpected general-purpose load form");
    }

    // Scalar SIMD&FP loads clear the upper bits of the 128-bit register,
    // exactly like the original instruction; LDR Q fills all sixteen bytes.
    int bytes = info.access_bytes();
    if (bytes == 0)
      throw std::runtime_error("unexpected SIMD load form");
    memory_accessed = true;
    std::memset(v[info.rt], 0, 16);
    uint64_t low = space.read(addr, bytes > 8 ? 8 : bytes);
    std::memcpy(v[info.rt], &low, bytes > 8 ? 8 : bytes);
    if (bytes > 8) {
      uint64_t high = space.read(addr + 8, 8);
      std::memcpy(v[info.rt] + 8, &high, 8);
    }
  }
};

// ---- relocation harness ----------------------------------------------------

constexpr uint64_t kHighVmBase = 0xFFFF800000001000ull;

struct ReloResult {
  CodeMemBlock out;
  CodeMemBlock origin;
  std::vector<uint32_t> words;
};

ReloResult run_relocation(void *buffer, uint64_t vm_addr, uint32_t size, bool branch = false) {
  ReloResult result;
  result.origin = CodeMemBlock(addr_t(vm_addr), size);
  result.out = CodeMemBlock(0, 0);
  GenRelocateCode(buffer, &result.origin, &result.out, branch);
  TASK_CHECK(result.out.addr() != 0);
  TASK_CHECK(result.out.size % 4 == 0);
  result.words.resize(result.out.size / 4);
  std::memcpy(result.words.data(), (void *)result.out.addr(), result.out.size);
  return result;
}

// Checks the rewrite of the literal load that starts at word `at`: four
// move-wide instructions that build the original data address, then exactly one
// load of the expected register class and width from [x17, #0].
void check_literal_rewrite_at(const std::vector<uint32_t> &words, size_t at, int expect_size, int expect_v,
                              int expect_opc, int expect_rt) {
  TASK_CHECK(at + 5 <= words.size());
  for (size_t i = at; i < at + 4; ++i) {
    TASK_CHECK(is_move_wide(words[i]));
    TASK_CHECK(field(words[i], 0, 4) == kTmpReg); // x17 scratch register only
  }
  LoadInfo load = decode_unsigned_load_store(words[at + 4]);
  TASK_CHECK(load.valid);
  TASK_CHECK(load.simd == (expect_v != 0));
  TASK_CHECK(load.size == expect_size);
  TASK_CHECK(load.opc == expect_opc);
  TASK_CHECK(load.rn == kTmpReg);
  TASK_CHECK(load.imm12 == 0);
  TASK_CHECK(load.rt == expect_rt);
}

void check_literal_rewrite(const ReloResult &res, int expect_size, int expect_v, int expect_opc, int expect_rt) {
  TASK_CHECK(res.words.size() == 5); // movz + 3 movk + one load
  check_literal_rewrite_at(res.words, 0, expect_size, expect_v, expect_opc, expect_rt);
}

// Executes the generated sequence word by word.
Machine execute(const std::vector<uint32_t> &words, const AddressSpace &space, uint64_t relo_base = 0x20000000) {
  Machine m;
  m.init_state();
  m.space = space;
  m.code = &words;
  m.relo_base = relo_base;
  for (size_t i = 0; i < words.size(); ++i) {
    m.pc = i;
    m.step(words[i]);
    if (m.branched)
      break; // the trailing data slot is not executable
  }
  return m;
}

// ---- test cases ------------------------------------------------------------

// 1. Instructions are read from the low host buffer, while the literal target
//    is computed from a completely different, high virtual address.
void test_split_buffer_and_high_vmaddr() {
  alignas(8) uint8_t data[16];
  for (int i = 0; i < 16; ++i)
    data[i] = uint8_t(0x10 + i);

  uint32_t buffer[1] = {encode_ldr_literal(0b01, 0, 8, 3)};
  auto res = run_relocation(buffer, kHighVmBase, sizeof(buffer));

  TASK_CHECK(res.origin.size == 4); // consumed one full instruction
  TASK_CHECK(res.origin.addr() == kHighVmBase);
  check_literal_rewrite(res, 3, 0, 0b01, 3);

  AddressSpace space;
  space.map(kHighVmBase + 8, data, sizeof(data));
  Machine m = execute(res.words, space);

  uint64_t expected;
  std::memcpy(&expected, data, 8);
  TASK_CHECK(m.x[3] == expected);
  TASK_CHECK(m.x[kTmpReg] == kHighVmBase + 8); // accessed the original data address
  TASK_CHECK(m.x[0] == (0xA5A5A5A500000000ull | 0));
}

// 2. Negative literal offset below the virtual-address base.
void test_negative_literal_offset() {
  alignas(8) uint8_t data[16];
  for (int i = 0; i < 16; ++i)
    data[i] = uint8_t(0x80 + i);

  uint32_t buffer[1] = {encode_ldr_literal(0b01, 0, -8, 5)};
  auto res = run_relocation(buffer, kHighVmBase, sizeof(buffer));
  check_literal_rewrite(res, 3, 0, 0b01, 5);

  AddressSpace space;
  space.map(kHighVmBase - 8, data, sizeof(data));
  Machine m = execute(res.words, space);

  uint64_t expected;
  std::memcpy(&expected, data, 8);
  TASK_CHECK(m.x[5] == expected);
  TASK_CHECK(m.x[kTmpReg] == kHighVmBase - 8);
}

// 3. Each literal access class keeps its own register class and access width.
void test_literal_access_classes() {
  struct ClassCase {
    int opc;
    int v;
    int expect_size;
    int expect_v;
    int expect_opc;
  };
  const ClassCase cases[] = {
      {0b00, 0, 2, 0, 0b01},  // LDR Wt
      {0b01, 0, 3, 0, 0b01},  // LDR Xt
      {0b10, 0, 2, 0, 0b10},  // LDRSW Xt
      {0b00, 1, 2, 1, 0b01},  // LDR St
      {0b01, 1, 3, 1, 0b01},  // LDR Dt
      {0b10, 1, 0, 1, 0b11},  // LDR Qt
  };

  alignas(16) uint8_t data[16];
  for (int i = 0; i < 16; ++i)
    data[i] = uint8_t(0xC0 + i);

  for (const auto &c : cases) {
    uint32_t buffer[1] = {encode_ldr_literal(c.opc, c.v, 0, 2)};
    auto res = run_relocation(buffer, kHighVmBase, sizeof(buffer));
    check_literal_rewrite(res, c.expect_size, c.expect_v, c.expect_opc, 2);

    // The address is materialized as code; the data is never inlined.
    TASK_CHECK(res.out.size == 20);

    AddressSpace space;
    space.map(kHighVmBase, data, sizeof(data));
    Machine m = execute(res.words, space);

    if (c.v == 0 && c.opc == 0b00) { // LDR W: four-byte access, upper half cleared
      TASK_CHECK(m.x[2] == 0xC3C2C1C0u);
      TASK_CHECK((m.x[2] >> 32) == 0);
    } else if (c.v == 0 && c.opc == 0b01) { // LDR X
      uint64_t expected;
      std::memcpy(&expected, data, 8);
      TASK_CHECK(m.x[2] == expected);
    } else if (c.v == 0 && c.opc == 0b10) { // LDRSW sign extension
      uint32_t raw;
      std::memcpy(&raw, data, 4);
      TASK_CHECK(m.x[2] == uint64_t(int64_t(int32_t(raw))));
    } else if (c.v == 1 && c.opc == 0b00) { // LDR S: 4 bytes low, upper zeroed
      TASK_CHECK(std::memcmp(m.v[2], data, 4) == 0);
      for (int i = 4; i < 16; ++i)
        TASK_CHECK(m.v[2][i] == 0);
    } else if (c.v == 1 && c.opc == 0b01) { // LDR D: 8 bytes low, upper zeroed
      TASK_CHECK(std::memcmp(m.v[2], data, 8) == 0);
      for (int i = 8; i < 16; ++i)
        TASK_CHECK(m.v[2][i] == 0);
    } else { // LDR Q: all sixteen bytes preserved
      TASK_CHECK(std::memcmp(m.v[2], data, 16) == 0);
    }
  }
}

// 4. PRFM stays a prefetch hint: no load, no register write, no memory access.
void test_prfm_stays_hint() {
  uint32_t buffer[1] = {encode_ldr_literal(0b11, 0, 0x100, 0b01110)};
  auto res = run_relocation(buffer, kHighVmBase, sizeof(buffer));
  check_literal_rewrite(res, 3, 0, 0b10, 0b01110);

  Machine m;
  m.init_state();
  for (uint32_t w : res.words) // intentionally no mapping for the target address
    m.step(w);
  TASK_CHECK(m.prefetch_seen);
  TASK_CHECK(!m.memory_accessed);
  TASK_CHECK(m.x[kTmpReg] == kHighVmBase + 0x100); // address was still computed
  for (int i = 0; i < 32; ++i) {
    if (i == kTmpReg)
      continue;
    TASK_CHECK(m.x[i] == (0xA5A5A5A500000000ull | uint64_t(i)));
  }
}

// 5. Rt=31 means XZR/WZR for general-purpose registers but V31 for SIMD&FP.
void test_rt31_register_class() {
  alignas(16) uint8_t data[16];
  for (int i = 0; i < 16; ++i)
    data[i] = uint8_t(0x40 + i);

  auto check_general_write_discarded = [&](int opc, int expect_size) {
    uint32_t buffer[1] = {encode_ldr_literal(opc, 0, 0, 31)};
    auto res = run_relocation(buffer, kHighVmBase, sizeof(buffer));
    check_literal_rewrite(res, expect_size, 0, 0b01, 31);
    AddressSpace space;
    space.map(kHighVmBase, data, sizeof(data));
    Machine m = execute(res.words, space);
    TASK_CHECK(m.memory_accessed);
    for (int i = 0; i < 32; ++i) {
      if (i == kTmpReg)
        continue;
      TASK_CHECK(m.x[i] == (0xA5A5A5A500000000ull | uint64_t(i)));
    }
  };
  check_general_write_discarded(0b00, 2); // LDR W31
  check_general_write_discarded(0b01, 3); // LDR X31

  { // LDR Q31: vector register 31 is a real destination, all 16 bytes preserved.
    uint32_t buffer[1] = {encode_ldr_literal(0b10, 1, 0, 31)};
    auto res = run_relocation(buffer, kHighVmBase, sizeof(buffer));
    check_literal_rewrite(res, 0, 1, 0b11, 31);
    AddressSpace space;
    space.map(kHighVmBase, data, sizeof(data));
    Machine m = execute(res.words, space);
    TASK_CHECK(std::memcmp(m.v[31], data, 16) == 0);
    TASK_CHECK(m.v[0][0] == 0xAA); // no other vector register touched
  }
}

// 6. Re-running the same generated sequence after the data changes observes
//    the new value: the address is materialized at run time, not baked data.
void test_second_read_sees_updated_data() {
  alignas(8) uint8_t data[8];
  uint64_t first = 0x0123456789ABCDEFull;
  uint64_t second = 0xFEDCBA9876543210ull;
  std::memcpy(data, &first, 8);

  uint32_t buffer[1] = {encode_ldr_literal(0b01, 0, 4, 9)};
  auto res = run_relocation(buffer, kHighVmBase, sizeof(buffer));
  check_literal_rewrite(res, 3, 0, 0b01, 9);
  TASK_CHECK(res.out.size == 20);

  AddressSpace space;
  space.map(kHighVmBase + 4, data, sizeof(data));

  Machine before = execute(res.words, space);
  TASK_CHECK(before.x[9] == first);

  std::memcpy(data, &second, 8);
  Machine after = execute(res.words, space);
  TASK_CHECK(after.x[9] == second);
  TASK_CHECK(after.x[9] != before.x[9]);
}

// 7. Ordinary instructions are copied one-to-one, interleaved with rewrites,
//    and the input cursor advances by exactly the relocated instruction length.
void test_interleaved_ordinary_instructions_and_cursor() {
  alignas(8) uint8_t data[8];
  uint64_t value = 0xDEADBEEFCAFEF00Dull;
  std::memcpy(data, &value, 8);

  uint32_t buffer[4] = {
      kNop,
      encode_ldr_literal(0b01, 0, 8, 1),
      kNop,
      kNop,
  };
  auto res = run_relocation(buffer, kHighVmBase, sizeof(buffer));

  TASK_CHECK(res.origin.size == sizeof(buffer)); // 16 bytes consumed
  TASK_CHECK(res.out.size == 4 + 20 + 4 + 4);

  // Ordinary instructions keep their encoding; only the literal is expanded.
  TASK_CHECK(res.words[0] == kNop);
  check_literal_rewrite_at(res.words, 1, 3, 0, 0b01, 1);
  TASK_CHECK(res.words[6] == kNop);
  TASK_CHECK(res.words[7] == kNop);

  AddressSpace space;
  space.map(kHighVmBase + 12, data, sizeof(data)); // load at origin+4, offset +8
  Machine m = execute(res.words, space);
  TASK_CHECK(m.x[1] == value);
}

// 8. ADR/ADRP and direct branches still compute targets from origin->addr(),
//    not from the host buffer address.
void test_pc_relative_targets_use_virtual_addr() {
  { // ADR x7, #+0x1234
    uint32_t buffer[1] = {encode_adr(0x1234, 7)};
    auto res = run_relocation(buffer, kHighVmBase, sizeof(buffer));
    TASK_CHECK(res.origin.size == 4);
    TASK_CHECK(res.words.size() == 4); // movz + 3 movk
    Machine m = execute(res.words, AddressSpace{});
    TASK_CHECK(m.x[7] == kHighVmBase + 0x1234);
  }

  { // ADR x7, #-0x100: negative offset stays relative to the virtual address
    uint32_t buffer[1] = {encode_adr(-0x100, 7)};
    auto res = run_relocation(buffer, kHighVmBase, sizeof(buffer));
    Machine m = execute(res.words, AddressSpace{});
    TASK_CHECK(m.x[7] == kHighVmBase - 0x100);
  }

  { // ADRP x3, #+0x2000: target is page aligned
    uint32_t buffer[1] = {encode_adrp(0x2000, 3)};
    auto res = run_relocation(buffer, kHighVmBase, sizeof(buffer));
    Machine m = execute(res.words, AddressSpace{});
    TASK_CHECK(m.x[3] == (kHighVmBase & ~0xFFFull) + 0x2000);
  }

  { // B #+0x40: the stub carries the virtual target, never the buffer address.
    uint32_t buffer[1] = {encode_b(0x40)};
    auto res = run_relocation(buffer, kHighVmBase, sizeof(buffer));
    TASK_CHECK(res.origin.size == 4);
    TASK_CHECK(res.out.size >= 12);

    // The first instruction loads the target from the stub's own data slot,
    // and the sequence ends with `br x17`. Executing the stub must arrive at
    // the virtual target computed from origin->addr().
    auto br_target = [&](const std::vector<uint32_t> &words) -> uint64_t {
      Machine m = execute(words, AddressSpace{});
      TASK_CHECK(m.branched);
      return m.x[kTmpReg];
    };
    TASK_CHECK(br_target(res.words) == kHighVmBase + 0x40);

    // Only the buffer bytes are fetched: no address of the host buffer is
    // baked into the generated bytes.
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(res.out.addr());
    for (size_t i = 0; i + 8 <= res.out.size; i += 4) {
      uint64_t value;
      std::memcpy(&value, bytes + i, 8);
      TASK_CHECK(value != (uint64_t)(uintptr_t)buffer);
    }
  }
}

// 9. Existing callers that pass the same address for buffer and origin keep
//    working.
void test_legacy_same_buffer_and_origin_address() {
  alignas(8) struct {
    uint32_t code[2];
    uint64_t data;
  } image;
  image.code[0] = kNop;
  image.code[1] = encode_ldr_literal(0b01, 0, 4, 4);
  image.data = 0x55AA55AA12345678ull;

  uint64_t vm = uint64_t((uintptr_t)&image);
  auto res = run_relocation(image.code, vm, sizeof(image.code));
  TASK_CHECK(res.origin.size == 8);
  TASK_CHECK(res.words[0] == kNop);

  AddressSpace space;
  space.map(vm + 8, &image.data, sizeof(image.data));
  Machine m = execute(res.words, space);
  TASK_CHECK(m.x[4] == image.data);
}

// 10. The `branch` parameter keeps its meaning: append a tail branch back to
//     the instruction right after the consumed bytes, addressed in the image's
//     virtual address space (not the host buffer address).
void test_branch_parameter_unchanged() {
  uint32_t buffer[2] = {encode_ldr_literal(0b01, 0, 8, 6), kNop};
  uint64_t next_va = kHighVmBase + 8;

  auto no_branch = run_relocation(buffer, kHighVmBase, sizeof(buffer), false);
  auto with_branch = run_relocation(buffer, kHighVmBase, sizeof(buffer), true);

  // Confirmed bytes are the same in both cases...
  TASK_CHECK(no_branch.origin.size == 8);
  TASK_CHECK(with_branch.origin.size == 8);
  // ...and the branch flag only appends the synthesized exit sequence.
  TASK_CHECK(with_branch.out.size > no_branch.out.size);

  std::vector<uint32_t> tail(with_branch.words.begin() + no_branch.words.size(), with_branch.words.end());
  TASK_CHECK(tail.size() >= 4); // ldr literal + br, then an 8-byte address slot
  LiteralInfo exit_ldr = decode_literal_load(tail[0]);
  TASK_CHECK(exit_ldr.valid);
  TASK_CHECK(exit_ldr.v == 0 && exit_ldr.opc == 0b01); // LDR Xt
  TASK_CHECK(exit_ldr.rt == kTmpReg);
  TASK_CHECK(tail[1] == kBrX17);

  // Executing the tail lands on the virtual address of the next origin
  // instruction, using the stub's own data slot for the load.
  alignas(8) uint8_t data[8] = {};
  AddressSpace space;
  space.map(kHighVmBase + 8, data, sizeof(data));
  Machine m = execute(with_branch.words, space);
  TASK_CHECK(m.branched);
  TASK_CHECK(m.x[kTmpReg] == next_va);
}

} // namespace

void register_cases(std::vector<OfflineCase> &cases) {
  cases.push_back({"split_buffer_and_high_vmaddr", test_split_buffer_and_high_vmaddr});
  cases.push_back({"negative_literal_offset", test_negative_literal_offset});
  cases.push_back({"literal_access_classes", test_literal_access_classes});
  cases.push_back({"prfm_stays_hint", test_prfm_stays_hint});
  cases.push_back({"rt31_register_class", test_rt31_register_class});
  cases.push_back({"second_read_sees_updated_data", test_second_read_sees_updated_data});
  cases.push_back({"interleaved_ordinary_and_cursor", test_interleaved_ordinary_instructions_and_cursor});
  cases.push_back({"pc_relative_targets_use_virtual_addr", test_pc_relative_targets_use_virtual_addr});
  cases.push_back({"legacy_same_buffer_and_origin_address", test_legacy_same_buffer_and_origin_address});
  cases.push_back({"branch_parameter_unchanged", test_branch_parameter_unchanged});
}
