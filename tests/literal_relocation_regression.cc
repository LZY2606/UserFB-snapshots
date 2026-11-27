// Offline regression tests for AArch64 instruction relocation.
//
// The relocator receives (a) a readable byte copy of firmware instructions and
// (b) the virtual address those instructions live at in the image. The two are
// intentionally different here: the virtual address is a high unmapped address
// and only the local host buffer is ever dereferenced.
//
// Generated code is never executed. A small restricted interpreter decodes the
// emitted instruction bit fields and models their effect against an abstract
// 64-bit address space, so the checks prove what the sequence would do once
// installed, without Unicorn or any executable mapping.

#include "InstructionRelocation/arm64/InstructionRelocationARM64.h"

#include "support/cases.h"

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <map>
#include <vector>

namespace {

// --- virtual addresses used for the fake firmware image ---------------------

constexpr uint64_t kCodeVa = 0xFFFF800010000000ULL;
constexpr uint64_t kDataPosVa = kCodeVa + 0x8000;
constexpr uint64_t kDataNegVa = kCodeVa - 0x8000;

// --- source instruction encoders --------------------------------------------

static uint32_t ldr_literal(uint32_t opc, uint32_t V, uint32_t rt, int64_t off) {
  uint32_t imm19 = static_cast<uint32_t>((off / 4) & 0x7FFFF);
  return 0x18000000u | (opc << 30) | (V << 26) | (imm19 << 5) | (rt & 31u);
}

static uint32_t adr_inst(uint32_t rd, int64_t imm) {
  return 0x10000000u | (static_cast<uint32_t>(imm & 3) << 29) |
         (static_cast<uint32_t>((imm >> 2) & 0x7FFFF) << 5) | rd;
}

static uint32_t adrp_inst(uint32_t rd, int64_t page_imm) {
  return 0x90000000u | (static_cast<uint32_t>(page_imm & 3) << 29) |
         (static_cast<uint32_t>((page_imm >> 2) & 0x7FFFF) << 5) | rd;
}

static uint32_t b_inst(bool link, int64_t off) {
  return 0x14000000u | (link ? 0x80000000u : 0u) |
         static_cast<uint32_t>((off / 4) & 0x3FFFFFF);
}

constexpr uint32_t NOP = 0xD503201F;

// --- abstract machine --------------------------------------------------------

struct Machine {
  uint64_t gpr[31] = {};            // x0..x30; x31 (SP/ZR) is never a destination here
  uint64_t vr[32][2] = {};          // V0..V31, [0] low 64 bits, [1] high 64 bits
  std::map<uint64_t, uint8_t> mem;  // sparse firmware address space

  void write_mem(uint64_t addr, const void *data, size_t n) {
    const auto *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < n; i++)
      mem[addr + i] = p[i];
  }

  uint64_t read_mem(uint64_t addr, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
      auto it = mem.find(addr + i);
      if (it == mem.end())
        throw std::runtime_error("read from unstored virtual address");
      v |= static_cast<uint64_t>(it->second) << (8 * i);
    }
    return v;
  }

  enum Kind { kNop, kMoveWide, kUnsignedAccess, kLiteralLdr, kBranch };

  struct Op {
    Kind kind;
    // move wide / load destination (31 means the GP zero register when is_vector is false)
    uint32_t rd = 0;
    bool is_vector = false;
    // unsigned access fields
    uint32_t size = 0, opc = 0, rn = 0, rt = 0;
    uint64_t addr = 0;
    uint64_t width = 0;
    bool prfm = false;
    // branch bookkeeping
    uint64_t target = 0;
    bool link = false;
  };

  static int64_t sign_extend(uint64_t x, unsigned bits) {
    uint64_t m = 1ULL << (bits - 1);
    return static_cast<int64_t>((x ^ m) - m);
  }

  std::vector<Op> run(const std::vector<uint32_t> &code) {
    std::vector<Op> trace;
    size_t pc = 0;
    // Data labels are always emitted after every instruction, so the first
    // literal-pool target marks the end of the executable stream.
    size_t pool_start = static_cast<size_t>(-1);
    while (pc < code.size() * 4) {
      if (pc >= pool_start)
        break;
      uint32_t w = code[pc / 4];
      if (w == NOP) {
        trace.push_back({kNop});
        pc += 4;
        continue;
      }

      // MOVN/MOVZ/MOVK
      if ((w & 0x1F800000u) == 0x12800000u) {
        uint32_t sf = w >> 31;
        uint32_t opc = (w >> 29) & 3;
        uint32_t hw = (w >> 21) & 3;
        uint64_t imm16 = (w >> 5) & 0xFFFF;
        uint32_t rd = w & 31;
        if (opc == 0)
          throw std::runtime_error("unexpected MOVN in relocated output");
        uint64_t shifted = imm16 << (16 * hw);
        if (rd != 31) {
          if (opc == 2) {  // MOVZ
            gpr[rd] = sf ? shifted : (shifted & 0xFFFFFFFFULL);
          } else {  // MOVK
            uint64_t mask = 0xFFFFULL << (16 * hw);
            gpr[rd] = (gpr[rd] & ~mask) | shifted;
          }
        }
        trace.push_back({kMoveWide, rd});
        pc += 4;
        continue;
      }

      // LDR/STR/PRFM (unsigned immediate offset)
      if ((w & 0x3B000000u) == 0x39000000u) {
        uint32_t size = (w >> 30) & 3;
        uint32_t V = (w >> 26) & 1;
        uint32_t opc = (w >> 22) & 3;
        uint32_t imm12 = (w >> 10) & 0xFFF;
        uint32_t rn = (w >> 5) & 31;
        uint32_t rt = w & 31;

        // In the unsigned-offset form `size` is log2 of the access width; for
        // vector loads the 128-bit form reuses size == 0.
        uint64_t scale;
        if (V == 0) {
          if (size == 2)
            scale = 4;
          else if (size == 3)
            scale = 8;
          else
            throw std::runtime_error("unexpected GP unsigned access size");
        } else {
          if (size == 0)
            scale = 16;  // LDR Q
          else if (size == 2)
            scale = 4;  // LDR S
          else if (size == 3)
            scale = 8;  // LDR D
          else
            throw std::runtime_error("unexpected vector unsigned access size");
        }
        uint64_t addr = gpr[rn] + imm12 * scale;

        Op op{kUnsignedAccess, rt, V == 1, size, opc, rn, rt, addr, scale};

        if (V == 0 && size == 3 && opc == 2) {
          // PRFM: hint only, no register or architectural state change.
          op.prfm = true;
          trace.push_back(op);
          pc += 4;
          continue;
        }

        if (V == 0) {
          if (opc != 1 && !(opc == 2 && size == 2))
            throw std::runtime_error("unexpected GP unsigned access opc");
          if (rt == 31) {
            // WZR/XZR: the load still performs the read, the result is discarded.
            (void)read_mem(addr, scale);
          } else if (opc == 2 && size == 2) {
            // LDRSW: 32-bit load sign extended into 64 bits.
            gpr[rt] = static_cast<uint64_t>(sign_extend(read_mem(addr, 4), 32));
          } else if (size == 2) {
            // LDR W: 32-bit load, upper 32 bits are cleared.
            gpr[rt] = read_mem(addr, 4) & 0xFFFFFFFFULL;
          } else {
            // LDR X: full 64-bit load.
            gpr[rt] = read_mem(addr, 8);
          }
        } else {
          // Vector loads: S/D use opc == 1, Q uses opc == 3.
          if (!((opc == 1) || (size == 0 && opc == 3)))
            throw std::runtime_error("unexpected vector unsigned access opc");
          if (scale == 4) {
            // LDR S: low 32 bits replaced, upper 96 bits keep their old value.
            uint64_t word = read_mem(addr, 4);
            vr[rt][0] = (vr[rt][0] & 0xFFFFFFFF00000000ULL) | word;
          } else if (scale == 8) {
            // LDR D: low 64 bits replaced, high 64 bits keep their old value.
            vr[rt][0] = read_mem(addr, 8);
          } else {
            // LDR Q: all 16 bytes replaced.
            vr[rt][0] = read_mem(addr, 8);
            vr[rt][1] = read_mem(addr + 8, 8);
          }
        }
        trace.push_back(op);
        pc += 4;
        continue;
      }

      // LDR Rt, literal (used by relocated branches to load the target pointer)
      if ((w & 0x3B000000u) == 0x18000000u) {
        uint32_t rt = w & 31;
        int64_t off = sign_extend((w >> 5) & 0x7FFFF, 19) * 4;
        size_t pool = static_cast<size_t>(static_cast<int64_t>(pc) + off);
        if (pool + 8 > code.size() * 4)
          throw std::runtime_error("literal pool outside relocated buffer");
        pool_start = std::min(pool_start, pool);
        uint64_t target = code[pool / 4] | (static_cast<uint64_t>(code[pool / 4 + 1]) << 32);
        if (rt != 31)
          gpr[rt] = target;
        Op op{kLiteralLdr, rt};
        op.target = target;
        trace.push_back(op);
        pc += 4;
        continue;
      }

      // BR / BLR: control leaves this segment. Record it and keep walking from
      // the next source instruction; any trailing words are the literal pool.
      if ((w & 0xFFFFFC1Fu) == 0xD61F0000u || (w & 0xFFFFFC1Fu) == 0xD63F0000u) {
        bool link = (w & 0x00200000u) != 0;
        uint32_t rn = (w >> 5) & 31;
        Op op{kBranch, 0, false, 0, 0, rn, 0, 0, 0, false, gpr[rn], link};
        trace.push_back(op);
        pc += 4;
        continue;
      }

      // Reached non-instruction data (the trailing relocated literal pool).
      if (pool_start == static_cast<size_t>(-1))
        throw std::runtime_error("relocated output contains an unmodeled instruction");
      break;
    }
    return trace;
  }

  void snapshot_gpr(uint64_t out[31]) {
    std::memcpy(out, gpr, sizeof(gpr));
  }
};

// --- relocation helper ------------------------------------------------------

static std::vector<uint32_t> relocate(const std::vector<uint32_t> &input, uint64_t va, bool branch,
                                      CodeMemBlock *origin_out = nullptr) {
  CodeMemBlock origin(va, static_cast<size_t>(input.size() * 4));
  CodeMemBlock out;
  GenRelocateCode(const_cast<uint32_t *>(input.data()), &origin, &out, branch);
  if (origin_out)
    *origin_out = origin;
  if (out.addr() == 0 || (out.size % 4) != 0)
    throw std::runtime_error("relocation produced invalid output block");
  const auto *words = reinterpret_cast<const uint32_t *>(out.addr());
  return std::vector<uint32_t>(words, words + out.size / 4);
}

static bool bytes_contain(const std::vector<uint32_t> &code, const uint8_t *needle, size_t n) {
  const auto *raw = reinterpret_cast<const uint8_t *>(code.data());
  size_t total = code.size() * 4;
  if (n > total)
    return false;
  for (size_t i = 0; i + n <= total; i++) {
    if (std::memcmp(raw + i, needle, n) == 0)
      return true;
  }
  return false;
}

// --- cases ------------------------------------------------------------------

void case_separate_buffer_and_virtual_address() {
  // Host-readable buffer at a low stack address; origin claims a high VA that
  // is never mapped or dereferenced. Positive and negative literal offsets,
  // ordinary NOPs interleaved, cursor advancement checked.
  std::vector<uint32_t> input = {
      NOP,
      ldr_literal(0b00, 0, 3, static_cast<int64_t>(kDataPosVa) - static_cast<int64_t>(kCodeVa) - 4),
      NOP,
      ldr_literal(0b01, 0, 4, static_cast<int64_t>(kDataNegVa) - static_cast<int64_t>(kCodeVa) - 12),
      NOP,
  };

  Machine m;
  uint32_t w_value = 0xAABBCCDD;
  uint64_t x_value = 0x0123456789ABCDEFULL;
  m.write_mem(kDataPosVa, &w_value, 4);
  m.write_mem(kDataNegVa, &x_value, 8);

  CodeMemBlock origin;
  auto code = relocate(input, kCodeVa, false, &origin);

  TASK_CHECK(origin.size == input.size() * 4);          // full input length consumed
  TASK_CHECK(code.size() * 4 == 4 + 20 + 4 + 20 + 4);     // NOP + 5-insn relo + NOP + 5-insn relo + NOP

  auto trace = m.run(code);
  TASK_CHECK(m.gpr[3] == w_value);                       // zero extended W load
  TASK_CHECK(m.gpr[4] == x_value);                       // full X load

  // Every emitted instruction must be on the interpreter whitelist and x17 is
  // the only scratch register introduced by the relocator.
  for (auto &op : trace) {
    if (op.kind == Machine::kMoveWide)
      TASK_CHECK(op.rd == 17);
    if (op.kind == Machine::kUnsignedAccess) {
      TASK_CHECK(op.rn == 17);
      TASK_CHECK(op.rt == 3 || op.rt == 4);
    }
  }

  // Mutate firmware data and re-run: the generated sequence dereferences the
  // original address, it must not have baked the build-machine value in.
  uint32_t w_new = 0x55667788;
  uint64_t x_new = 0xFEDCBA9876543210ULL;
  m.write_mem(kDataPosVa, &w_new, 4);
  m.write_mem(kDataNegVa, &x_new, 8);
  (void)m.run(code);
  TASK_CHECK(m.gpr[3] == w_new);
  TASK_CHECK(m.gpr[4] == x_new);

  TASK_CHECK(!bytes_contain(code, reinterpret_cast<const uint8_t *>(&w_value), 4));
  TASK_CHECK(!bytes_contain(code, reinterpret_cast<const uint8_t *>(&x_value), 8));
}

void case_ldr_w_zero_extends_and_rereads() {
  std::vector<uint32_t> input = {
      ldr_literal(0b00, 0, 0, 0x4000),
  };
  Machine m;
  m.gpr[0] = 0xDEADBEEFCAFEBABEULL;  // preload upper half with garbage
  uint32_t value = 0x87654321;
  m.write_mem(kCodeVa + 0x4000, &value, 4);

  auto code = relocate(input, kCodeVa, false);
  TASK_CHECK(code.size() == 5);  // movz/movk x4 + one load
  auto trace = m.run(code);
  TASK_CHECK(m.gpr[0] == 0x0000000087654321ULL);  // high half cleared

  bool saw_load = false;
  for (auto &op : trace) {
    if (op.kind == Machine::kMoveWide)
      TASK_CHECK(op.rd == 17);
    if (op.kind == Machine::kUnsignedAccess) {
      TASK_CHECK(!op.is_vector && op.width == 4 && op.rt == 0 && op.rn == 17);
      saw_load = true;
    }
  }
  TASK_CHECK(saw_load);

  uint32_t value2 = 0xFFFFFFFF;
  m.write_mem(kCodeVa + 0x4000, &value2, 4);
  (void)m.run(code);
  TASK_CHECK(m.gpr[0] == 0x00000000FFFFFFFFULL);
}

void case_ldr_x_full_width_and_rereads() {
  std::vector<uint32_t> input = {
      ldr_literal(0b01, 0, 1, -0x2000),
  };
  Machine m;
  uint64_t value = 0xA5A51234DEADBEEFULL;
  m.write_mem(kCodeVa - 0x2000, &value, 8);

  auto code = relocate(input, kCodeVa, false);
  (void)m.run(code);
  TASK_CHECK(m.gpr[1] == value);
  for (auto &op : m.run(code)) {
    if (op.kind == Machine::kUnsignedAccess)
      TASK_CHECK(op.width == 8 && op.rt == 1 && !op.is_vector);
  }

  uint64_t value2 = 0x0011223344556677ULL;
  m.write_mem(kCodeVa - 0x2000, &value2, 8);
  (void)m.run(code);
  TASK_CHECK(m.gpr[1] == value2);
  TASK_CHECK(!bytes_contain(code, reinterpret_cast<const uint8_t *>(&value), 8));
}

void case_ldrsw_sign_extends() {
  std::vector<uint32_t> input = {
      ldr_literal(0b10, 0, 2, 0x1000),
  };
  Machine m;
  auto code = relocate(input, kCodeVa, false);

  uint32_t neg = 0x80000000;
  m.write_mem(kCodeVa + 0x1000, &neg, 4);
  auto trace = m.run(code);
  TASK_CHECK(m.gpr[2] == 0xFFFFFFFF80000000ULL);
  for (auto &op : trace) {
    if (op.kind == Machine::kUnsignedAccess)
      TASK_CHECK(op.width == 4 && op.rt == 2 && !op.is_vector && !op.prfm);
  }

  uint32_t pos = 0x7FFFFFFF;
  m.write_mem(kCodeVa + 0x1000, &pos, 4);
  (void)m.run(code);
  TASK_CHECK(m.gpr[2] == 0x000000007FFFFFFFULL);

  uint32_t allones = 0xFFFFFFFF;
  m.write_mem(kCodeVa + 0x1000, &allones, 4);
  (void)m.run(code);
  TASK_CHECK(m.gpr[2] == 0xFFFFFFFFFFFFFFFFULL);
}

void case_gp_rt31_is_zero_register() {
  // Rt == 31 in the GP space is WZR/XZR: no architectural register is written.
  std::vector<uint32_t> input = {
      ldr_literal(0b00, 0, 31, 0x100),
      NOP,
      ldr_literal(0b01, 0, 31, 0x100),
      NOP,
      ldr_literal(0b10, 0, 31, 0x100),
  };
  Machine m;
  uint64_t data[3] = {0x1122334455667788ULL, 0x99AABBCCDDEEFF00ULL, 0x0102030405060708ULL};
  m.write_mem(kCodeVa + 0x100, data, sizeof(data));
  for (auto &v : m.gpr)
    v = 0x0BADF00D0BADF00DULL;

  uint64_t before[31];
  m.snapshot_gpr(before);
  auto code = relocate(input, kCodeVa, false);
  auto trace = m.run(code);
  uint64_t after[31];
  m.snapshot_gpr(after);
  // x17 is the relocation scratch register; every other GPR must be untouched.
  for (unsigned i = 0; i < 31; i++) {
    if (i != 17)
      TASK_CHECK(before[i] == after[i]);
  }
  // x17 is left holding the last resolved literal address.
  TASK_CHECK(after[17] == kCodeVa + 16 + 0x100);

  unsigned loads = 0;
  for (auto &op : trace) {
    if (op.kind == Machine::kUnsignedAccess) {
      TASK_CHECK(op.rt == 31 && !op.is_vector && !op.prfm);
      loads++;
    }
  }
  TASK_CHECK(loads == 3);
}

void case_ldr_s_preserves_upper_lanes() {
  std::vector<uint32_t> input = {
      ldr_literal(0b00, 1, 5, 0x800),
  };
  Machine m;
  m.vr[5][0] = 0x1111111122222222ULL;
  m.vr[5][1] = 0x3333333344444444ULL;
  uint32_t value = 0xCAFEBABE;
  m.write_mem(kCodeVa + 0x800, &value, 4);

  auto code = relocate(input, kCodeVa, false);
  auto trace = m.run(code);
  TASK_CHECK((m.vr[5][0] & 0xFFFFFFFFULL) == value);
  TASK_CHECK((m.vr[5][0] >> 32) == 0x11111111ULL);
  TASK_CHECK(m.vr[5][1] == 0x3333333344444444ULL);
  for (auto &op : trace) {
    if (op.kind == Machine::kUnsignedAccess)
      TASK_CHECK(op.is_vector && op.width == 4 && op.rt == 5);
  }

  uint32_t value2 = 0x0BADF00D;
  m.write_mem(kCodeVa + 0x800, &value2, 4);
  (void)m.run(code);
  TASK_CHECK((m.vr[5][0] & 0xFFFFFFFFULL) == value2);
  TASK_CHECK(m.vr[5][1] == 0x3333333344444444ULL);
}

void case_ldr_d_preserves_upper_64() {
  std::vector<uint32_t> input = {
      ldr_literal(0b01, 1, 6, -0x400),
  };
  Machine m;
  m.vr[6][0] = 0xAAAAAAAAAAAAAAAAULL;
  m.vr[6][1] = 0xBBBBBBBBBBBBBBBBULL;
  uint64_t value = 0x0F0F0F0FF0F0F0F0ULL;
  m.write_mem(kCodeVa - 0x400, &value, 8);

  auto code = relocate(input, kCodeVa, false);
  auto trace = m.run(code);
  TASK_CHECK(m.vr[6][0] == value);
  TASK_CHECK(m.vr[6][1] == 0xBBBBBBBBBBBBBBBBULL);
  for (auto &op : trace) {
    if (op.kind == Machine::kUnsignedAccess)
      TASK_CHECK(op.is_vector && op.width == 8 && op.rt == 6);
  }

  uint64_t value2 = 0x13579BDF2468ACE0ULL;
  m.write_mem(kCodeVa - 0x400, &value2, 8);
  (void)m.run(code);
  TASK_CHECK(m.vr[6][0] == value2);
  TASK_CHECK(m.vr[6][1] == 0xBBBBBBBBBBBBBBBBULL);
}

void case_ldr_q_loads_all_16_bytes() {
  std::vector<uint32_t> input = {
      ldr_literal(0b10, 1, 7, 0x2000),
  };
  Machine m;
  m.vr[7][0] = 0xAAAAAAAAAAAAAAAAULL;
  m.vr[7][1] = 0xBBBBBBBBBBBBBBBBULL;
  uint64_t data[2] = {0x0102030405060708ULL, 0x090A0B0C0D0E0F10ULL};
  m.write_mem(kCodeVa + 0x2000, data, 16);

  auto code = relocate(input, kCodeVa, false);
  auto trace = m.run(code);
  TASK_CHECK(m.vr[7][0] == data[0]);
  TASK_CHECK(m.vr[7][1] == data[1]);
  for (auto &op : trace) {
    if (op.kind == Machine::kUnsignedAccess)
      TASK_CHECK(op.is_vector && op.width == 16 && op.rt == 7);
  }

  uint64_t data2[2] = {0x1020304050607080ULL, 0x90A0B0C0D0E0F000ULL};
  m.write_mem(kCodeVa + 0x2000, data2, 16);
  (void)m.run(code);
  TASK_CHECK(m.vr[7][0] == data2[0]);
  TASK_CHECK(m.vr[7][1] == data2[1]);
}

void case_vector_rt31_is_v31() {
  // Rt == 31 in the vector space names V31, a real destination register.
  std::vector<uint32_t> input = {
      ldr_literal(0b10, 1, 31, 0x1000),
  };
  Machine m;
  m.vr[31][0] = 0x7777777777777777ULL;
  m.vr[31][1] = 0x8888888888888888ULL;
  uint64_t data[2] = {0xAAAABBBBCCCCDDDDULL, 0xEEEEFFFF00001111ULL};
  m.write_mem(kCodeVa + 0x1000, data, 16);

  auto code = relocate(input, kCodeVa, false);
  auto trace = m.run(code);
  TASK_CHECK(m.vr[31][0] == data[0]);
  TASK_CHECK(m.vr[31][1] == data[1]);
  bool saw_v31 = false;
  for (auto &op : trace) {
    if (op.kind == Machine::kUnsignedAccess) {
      TASK_CHECK(op.is_vector && op.rt == 31 && op.width == 16);
      saw_v31 = true;
    }
  }
  TASK_CHECK(saw_v31);
}

void case_prfm_stays_a_hint() {
  // Rt field of a literal PRFM carries the prfop, not a register number.
  const uint32_t prfop = 5;
  std::vector<uint32_t> input = {
      ldr_literal(0b11, 0, prfop, 0x3000),
      NOP,
      ldr_literal(0b11, 0, prfop, -0x3000),
  };
  Machine m;
  for (auto &v : m.gpr)
    v = 0x1234123412341234ULL;
  for (auto &v : m.vr) {
    v[0] = 0x5555555555555555ULL;
    v[1] = 0x6666666666666666ULL;
  }
  m.write_mem(kCodeVa + 0x3000, &prfop, 4);
  m.write_mem(kCodeVa + 8 - 0x3000, &prfop, 4);

  uint64_t gpr_before[31];
  m.snapshot_gpr(gpr_before);
  uint64_t vr_before[32][2];
  std::memcpy(vr_before, m.vr, sizeof(vr_before));

  auto code = relocate(input, kCodeVa, false);
  auto trace = m.run(code);

  uint64_t gpr_after[31];
  m.snapshot_gpr(gpr_after);
  // x17 is the relocation scratch register; the hint must not touch anything else.
  for (unsigned i = 0; i < 31; i++) {
    if (i != 17)
      TASK_CHECK(gpr_before[i] == gpr_after[i]);
  }
  TASK_CHECK(std::memcmp(vr_before, m.vr, sizeof(vr_before)) == 0);

  unsigned hints = 0;
  for (auto &op : trace) {
    if (op.kind == Machine::kUnsignedAccess) {
      TASK_CHECK(op.prfm);                 // kept as PRFM, not converted into a load
      TASK_CHECK(op.rt == prfop);          // prfop preserved
      TASK_CHECK(op.rn == 17);             // address still resolved through x17
      TASK_CHECK(op.addr == (hints == 0 ? kCodeVa + 0x3000 : kCodeVa + 8 - 0x3000));
      hints++;
    }
  }
  TASK_CHECK(hints == 2);

  // Re-running with changed data must remain side-effect free.
  uint32_t other = 0x99;
  m.write_mem(kCodeVa + 0x3000, &other, 4);
  (void)m.run(code);
  for (unsigned i = 0; i < 31; i++) {
    if (i != 17)
      TASK_CHECK(gpr_before[i] == m.gpr[i]);
  }
}

void case_adr_adrp_use_origin_va() {
  // ADR/ADRP targets derive from the virtual origin address, even though the
  // instruction bytes only exist in the local buffer.
  std::vector<uint32_t> input = {
      NOP,
      adr_inst(8, 0x1000),
      NOP,
      adrp_inst(9, 2),
      NOP,
  };
  auto code = relocate(input, kCodeVa, false);
  Machine m;
  auto trace = m.run(code);

  TASK_CHECK(m.gpr[8] == kCodeVa + 4 + 0x1000);
  TASK_CHECK(m.gpr[9] == (kCodeVa & ~0xFFFULL) + (2ULL << 12));

  for (auto &op : trace) {
    if (op.kind == Machine::kMoveWide)
      TASK_CHECK(op.rd == 17 || op.rd == 8 || op.rd == 9);
  }
}

void case_unconditional_branch_targets_origin_va() {
  std::vector<uint32_t> input = {
      b_inst(false, 0x4000),
      NOP,
      b_inst(true, -0x10),
  };
  CodeMemBlock origin;
  auto code = relocate(input, kCodeVa, false, &origin);
  TASK_CHECK(origin.size == 12);

  Machine m;
  auto trace = m.run(code);

  const Machine::Op *b_branch = nullptr;
  const Machine::Op *bl_branch = nullptr;
  for (auto &op : trace) {
    if (op.kind == Machine::kBranch) {
      TASK_CHECK(!op.link ? b_branch == nullptr : bl_branch == nullptr);
      (op.link ? bl_branch : b_branch) = &op;
    }
    if (op.kind == Machine::kLiteralLdr)
      TASK_CHECK(op.rd == 17);
  }
  TASK_CHECK(b_branch != nullptr && bl_branch != nullptr);
  TASK_CHECK(b_branch->target == kCodeVa + 0x4000);
  TASK_CHECK(bl_branch->target == kCodeVa + 8 - 0x10);
  TASK_CHECK(!b_branch->link && bl_branch->link);
}

void case_same_buffer_origin_address_compat() {
  // Existing callers pass the same pointer as buffer and origin address.
  alignas(16) uint32_t input[2] = {
      ldr_literal(0b00, 0, 0, 0x100),
      NOP,
  };
  CodeMemBlock origin(reinterpret_cast<addr_t>(input), sizeof(input));
  CodeMemBlock out;
  GenRelocateCode(input, &origin, &out, false);
  TASK_CHECK(origin.size == sizeof(input));
  TASK_CHECK(out.addr() != 0 && out.size % 4 == 0);

  const auto *words = reinterpret_cast<const uint32_t *>(out.addr());
  std::vector<uint32_t> code(words, words + out.size / 4);

  Machine m;
  uint32_t value = 0x42424242;
  m.write_mem(reinterpret_cast<uint64_t>(input) + 0x100, &value, 4);
  (void)m.run(code);
  TASK_CHECK(m.gpr[0] == value);
}

void case_branch_tail_emitted_when_requested() {
  std::vector<uint32_t> input = {NOP};
  CodeMemBlock origin;
  auto code = relocate(input, kCodeVa, true, &origin);
  TASK_CHECK(origin.size == 4);
  // NOP + LDR x17, literal + BR x17 + 8-byte literal pool = 5 words.
  TASK_CHECK(code.size() == 5);
  TASK_CHECK(code[0] == NOP);
  TASK_CHECK((code[1] & 0xFFFFFC1Fu) == 0x58000000u + 17);  // LDR X17, literal
  TASK_CHECK(code[2] == 0xD61F0220u);                         // BR X17
  TASK_CHECK(code[3] == static_cast<uint32_t>(kCodeVa + 4));
  TASK_CHECK(code[4] == static_cast<uint32_t>((kCodeVa + 4) >> 32));
  TASK_CHECK((code.size() * 4 - 8) / 4 == 3);

  Machine m;
  auto trace = m.run(code);
  const Machine::Op *tail = nullptr;
  for (auto &op : trace) {
    if (op.kind == Machine::kBranch)
      tail = &op;
  }
  TASK_CHECK(tail != nullptr && tail->target == kCodeVa + 4);
}

}  // namespace

void register_cases(std::vector<OfflineCase> &cases) {
  cases.push_back({"separate buffer and virtual origin address", case_separate_buffer_and_virtual_address});
  cases.push_back({"ldr w zero extends and re-reads updated data", case_ldr_w_zero_extends_and_rereads});
  cases.push_back({"ldr x full 64 bit width and re-reads updated data", case_ldr_x_full_width_and_rereads});
  cases.push_back({"ldrsw sign extends", case_ldrsw_sign_extends});
  cases.push_back({"gp rt31 addresses the zero register", case_gp_rt31_is_zero_register});
  cases.push_back({"ldr s preserves upper vector lanes", case_ldr_s_preserves_upper_lanes});
  cases.push_back({"ldr d preserves upper vector 64 bits", case_ldr_d_preserves_upper_64});
  cases.push_back({"ldr q loads all 16 bytes", case_ldr_q_loads_all_16_bytes});
  cases.push_back({"vector rt31 is v31", case_vector_rt31_is_v31});
  cases.push_back({"prfm remains a hint", case_prfm_stays_a_hint});
  cases.push_back({"adr adrp compute from origin virtual address", case_adr_adrp_use_origin_va});
  cases.push_back({"unconditional branches target origin virtual address", case_unconditional_branch_targets_origin_va});
  cases.push_back({"same buffer and origin address still works", case_same_buffer_origin_address_compat});
  cases.push_back({"branch tail emitted when requested", case_branch_tail_emitted_when_requested});
}
