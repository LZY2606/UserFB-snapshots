#include "platform_detect_macro.h"

#if defined(TARGET_ARCH_ARM64)

#include "InstructionRelocation/arm64/InstructionRelocationARM64.h"

#include "dobby/dobby_internal.h"

#include "core/arch/arm64/registers-arm64.h"
#include "core/assembler/assembler-arm64.h"
#include "core/codegen/codegen-arm64.h"

#include "inst_constants.h"
#include "inst_decode_encode_kit.h"

using namespace zz::arm64;

#if defined(DOBBY_DEBUG)
#define debug_nop() _ nop()
#else
#define debug_nop()
#endif

#define arm64_trunc_page(x) ((x) & (~(0x1000 - 1)))
#define arm64_round_page(x) trunc_page((x) + (0x1000 - 1))

// ---

static inline bool inst_is_b_bl(uint32_t instr) {
  return (instr & UnconditionalBranchFixedMask) == UnconditionalBranchFixed;
}

static inline bool inst_is_ldr_literal(uint32_t instr) {
  return ((instr & LoadRegLiteralFixedMask) == LoadRegLiteralFixed);
}

static inline bool inst_is_adr(uint32_t instr) {
  return (instr & PCRelAddressingFixedMask) == PCRelAddressingFixed && (instr & PCRelAddressingMask) == ADR;
}

static inline bool inst_is_adrp(uint32_t instr) {
  return (instr & PCRelAddressingFixedMask) == PCRelAddressingFixed && (instr & PCRelAddressingMask) == ADRP;
}

static inline bool inst_is_b_cond(uint32_t instr) {
  return (instr & ConditionalBranchFixedMask) == ConditionalBranchFixed;
}

static inline bool inst_is_compare_b(uint32_t instr) {
  return (instr & CompareBranchFixedMask) == CompareBranchFixed;
}

static inline bool inst_is_test_b(uint32_t instr) {
  return (instr & TestBranchFixedMask) == TestBranchFixed;
}

struct relo_ctx_t {
  // Readable copy of the source instructions. This is the only memory the
  // relocator is allowed to fetch from; it need not be mapped at origin->addr().
  uint8_t *buffer;

  addr_t cursor;
  uint32_t relocated_insn_count;

  CodeMemBlock *origin;
  CodeMemBlock relocated{};
  CodeMemBuffer *relocated_buffer;

  explicit relo_ctx_t(void *buffer, MemBlock *origin) : buffer((uint8_t *)buffer), origin(origin) {
    cursor = origin->addr();
  }

  uint32_t preferred_relo_size() {
    return origin->size;
  }

  void correct_final_relo_size() {
    origin->resize(relo_size());
  }

  uint32_t relo_size() {
    return (uintptr_t)cursor - origin->addr();
  }

  addr_t origin_start() {
    return origin->addr();
  }

  // Virtual address of the current source instruction, used for PC relative
  // target calculation only. It is never dereferenced.
  addr_t origin_cursor() {
    return origin->addr() + relo_size();
  }

  uint32_t origin_off() {
    return (uintptr_t)cursor - origin->addr();
  }

  // Fetch the current source instruction from the caller supplied buffer.
  uint32_t load_origin_inst() {
    return *(uint32_t *)(buffer + origin_off());
  }

  addr_t relocated_start() {
    return relocated.addr();
  }

  addr_t relocated_cursor() {
    return relocated.addr() + relocated_buffer->size();
  }

  uint32_t relocated_off() {
    return relocated_buffer->size();
  }

  void record_relo_start() {
    DEBUG_LOG("relo: origin_off: %p, relocated_off: %p", origin_off(), relocated_off());
  }

  int relocate(bool branch);
};

#define DEFINE_DATA_LABEL(data, name) auto name##_data_label = _ createDataLabel(data);

// Emit an unsigned-offset load/store/prfm with base x17 and offset 0.
// `op` is one of the LoadStoreOp OP_* encodings, so the access width and
// register class come straight from the original instruction instead of
// being forced to a 64-bit general purpose load.
static inline uint32_t encode_unsigned_offset_x17(uint32_t op, int rt) {
  return LoadStoreUnsignedOffsetFixed | op | (TMP_REG_0.code() << kRnShift) | (rt << kRtShift);
}

int relo_ctx_t::relocate(bool branch) {
  TurboAssembler turbo_assembler_;
#undef _
#define _ turbo_assembler_. // NOLINT

  this->relocated_buffer = turbo_assembler_.code_buffer();

  while (relo_size() < preferred_relo_size()) {
    record_relo_start();

    uint32_t inst = load_origin_inst();
    if (inst_is_b_bl(inst)) {
      DEBUG_LOG("%d:relo <b_bl> at %p", relocated_insn_count++, origin_cursor());

      int64_t offset = decode_imm26_offset(inst);
      addr_t dst = origin_cursor() + offset;
      DEFINE_DATA_LABEL(dst, dst);
      {
        _ Ldr(TMP_REG_0, dst_data_label);
        if ((inst & UnconditionalBranchMask) == BL) {
          _ blr(TMP_REG_0);
        } else {
          _ br(TMP_REG_0);
        }
      }

    } else if (inst_is_ldr_literal(inst)) {
      DEBUG_LOG("%d:relo <ldr_literal> at %p", relocated_insn_count++, origin_cursor());

      int64_t offset = decode_imm19_offset(inst);
      addr_t dst = origin_cursor() + offset;

      int rt = decode_rt(inst);
      int opc = bits(inst, 30, 31);
      int v = bit(inst, 26);

      // Resolve the original literal address into x17, then do a normal
      // unsigned-offset access of the exact same register class and width.
      // Only x17 is added to the clobber set; the destination register is
      // written by the original instruction anyway.
      _ Mov(TMP_REG_0, dst);

      if (v == 0) {
        // General purpose / prefetch space.
        switch (opc) {
        case 0b00:
          // LDR Wt, literal: 32-bit load; rt == 31 encodes WZR (value discarded).
          _ Emit(encode_unsigned_offset_x17(OP_W(LDR), rt));
          break;
        case 0b01:
          // LDR Xt, literal: 64-bit load; rt == 31 encodes XZR.
          _ Emit(encode_unsigned_offset_x17(OP_X(LDR), rt));
          break;
        case 0b10:
          // LDRSW Xt, literal: 32-bit load, sign extended into 64 bits;
          // rt == 31 encodes XZR.
          _ Emit(encode_unsigned_offset_x17(OP(LDRSW), rt));
          break;
        case 0b11:
          // PRFM prfop, literal: keep it a hint against the original data
          // address, never turn it into a real load. The Rt field holds the
          // prfop, not a register number.
          _ Emit(encode_unsigned_offset_x17(OP(PRFM), rt));
          break;
        }
      } else {
        // SIMD/FP space. Rt == 31 is V31, a real vector register, unlike the
        // general purpose case where 31 is the zero register.
        switch (opc) {
        case 0b00:
          // LDR St, literal: 32-bit vector load, upper lanes keep original behavior.
          _ Emit(encode_unsigned_offset_x17(OP_S(LDR), rt));
          break;
        case 0b01:
          // LDR Dt, literal: 64-bit vector load, upper lanes keep original behavior.
          _ Emit(encode_unsigned_offset_x17(OP_D(LDR), rt));
          break;
        case 0b10:
          // LDR Qt, literal: full 128-bit (16 byte) vector load.
          _ Emit(encode_unsigned_offset_x17(OP_Q(LDR), rt));
          break;
        case 0b11:
          // Encoding reserved in the SIMD/FP literal space.
          UNIMPLEMENTED();
          break;
        }
      }
    } else if (inst_is_adr(inst)) {
      DEBUG_LOG("%d:relo <adr> at %p", relocated_insn_count++, origin_cursor());

      int64_t offset = decode_immhi_immlo_offset(inst);
      addr_t dst = origin_cursor() + offset;

      int rd = decode_rd(inst);

      {
        _ Mov(X(rd), dst);
        ;
      }
    } else if (inst_is_adrp(inst)) {
      DEBUG_LOG("%d:relo <adrp> at %p", relocated_insn_count++, origin_cursor());

      int64_t offset = decode_immhi_immlo_zero12_offset(inst);
      addr_t dst = origin_cursor() + offset;
      dst = arm64_trunc_page(dst);

      int rd = decode_rd(inst);

      {
        _ Mov(X(rd), dst);
        ;
      }
    } else if (inst_is_b_cond(inst)) {
      DEBUG_LOG("%d:relo <b_cond> at %p", relocated_insn_count++, origin_cursor());

      int64_t offset = decode_imm19_offset(inst);
      addr_t dst = origin_cursor() + offset;

      uint32_t branch_inst = inst;
      {
        char cond = bits(inst, 0, 3);
        cond = cond ^ 1;
        set_bits(branch_inst, 0, 3, cond);

        int64_t offset = 4 * 3;
        uint32_t imm19 = offset >> 2;
        set_bits(branch_inst, 5, 23, imm19);
      }

      DEFINE_DATA_LABEL(dst, dst);

      {
        _ Emit(branch_inst);
        {
          _ Ldr(TMP_REG_0, dst_data_label);
          _ br(TMP_REG_0);
        }
      }
    } else if (inst_is_compare_b(inst)) {
      DEBUG_LOG("%d:relo <compare_b> at %p", relocated_insn_count++, origin_cursor());

      int64_t offset = decode_imm19_offset(inst);
      addr_t dst = origin_cursor() + offset;

      uint32_t branch_inst = inst;
      {
        char op = bit(inst, 24);
        op = op ^ 1;
        set_bit(branch_inst, 24, op);

        int64_t offset = 4 * 3;
        uint32_t imm19 = offset >> 2;
        set_bits(branch_inst, 5, 23, imm19);
      }

      DEFINE_DATA_LABEL(dst, dst);

      {
        _ Emit(branch_inst);
        {
          _ Ldr(TMP_REG_0, dst_data_label);
          _ br(TMP_REG_0);
        }
      }
    } else if (inst_is_test_b(inst)) {
      DEBUG_LOG("%d:relo <test_b> at %p", relocated_insn_count++, origin_cursor());

      int64_t offset = decode_imm14_offset(inst);
      addr_t dst = origin_cursor() + offset;

      uint32_t branch_inst = inst;
      {
        char op = bit(inst, 24);
        op = op ^ 1;
        set_bit(branch_inst, 24, op);

        int64_t offset = 4 * 3;
        uint32_t imm14 = offset >> 2;
        set_bits(branch_inst, 5, 18, imm14);
      }

      DEFINE_DATA_LABEL(dst, dst);

      {
        _ Emit(branch_inst);
        {
          _ Ldr(TMP_REG_0, dst_data_label);
          _ br(TMP_REG_0);
        }
      }
    } else {
      _ Emit(inst);
    }

    this->cursor += sizeof(uint32_t);
  }

  correct_final_relo_size();

  // TODO: if last instr is unlink branch, ignore it
  if (branch) {
    CodeGen codegen(&turbo_assembler_);
    codegen.LiteralLdrBranch(origin_cursor());
  }

  turbo_assembler_.relocDataLabels();

  relocated = AssemblerCodeBuilder::FinalizeFromTurboAssembler(&turbo_assembler_);
  return 0;
}

void GenRelocateCode(void *buffer, CodeMemBlock *origin, CodeMemBlock *relocated, bool branch) {
  relo_ctx_t ctx(buffer, origin);
  ctx.relocate(branch);
  *relocated = ctx.relocated;
}

void GenRelocateCodeAndBranch(void *buffer, CodeMemBlock *origin, CodeMemBlock *relocated) {
  GenRelocateCode(buffer, origin, relocated, true);
}

#endif
