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

#if 0
bool has_relo_label_at(relo_ctx_t *ctx, addr_t addr) {
  if (ctx->label_map.count(addr)) {
    return true;
  }
  return false;
}

PseudoLabel *relo_label_create_or_get(relo_ctx_t *ctx, addr_t addr) {
  if (!ctx->label_map.count(addr)) {
    auto *label = new PseudoLabel(addr);
    ctx->label_map[addr] = label;
  }
  return ctx->label_map[addr];
}

int64_t relo_label_link_offset(relo_ctx_t *ctx, pcrel_type_t pcrel_type, int64_t offset) {
  auto is_offset_undefined = [ctx](int64_t offset) -> bool {
    if (ctx->buffer_cursor + offset < ctx->buffer || ctx->buffer_cursor + offset > ctx->buffer + ctx->buffer_size) {
      return true;
    }
    return false;
  };

  auto is_offset_uninitialized = [ctx](int64_t offset) -> bool {
    if (ctx->buffer_cursor + offset > ctx->buffer && ctx->buffer_cursor + offset < ctx->buffer + ctx->buffer_size) {
      if (!ctx->relocated_offset_map.count(ctx->buffer_cursor + offset - ctx->buffer_cursor))
        return true;
    }
    return false;
  };

  addr_t label_vmaddr = relo_cur_src_vmaddr(ctx) + offset;
  if (pcrel_type == RELO_ARM64_RELOC_PAGE21) {
    label_vmaddr = arm64_trunc_page(label_vmaddr);
  }

  auto *label = relo_label_create_or_get(ctx, label_vmaddr);
  if (is_offset_undefined(offset)) { // pc relative target is beyond our scope
    label->link_to(PseudoLabel::kLabelImm19, relo_cur_src_vmaddr(ctx), (addr_t)ctx->buffer_cursor - ctx->mapped_addr);
    return 0;
  } else if (is_offset_uninitialized(offset)) { // pc relative target is in our control, but not handle yet
    label->link_to(PseudoLabel::kLabelImm19, relo_cur_src_vmaddr(ctx), (addr_t)ctx->buffer_cursor - ctx->mapped_addr);
    return 0;
  } else { // pc relative target is already handled
    off_t off = ctx->buffer_cursor + offset - ctx->buffer;
    off_t relocated_off = label->pos();
    int64_t new_offset = relo_dst_offset_to_vmaddr(ctx, relocated_off) - relo_src_offset_to_vmaddr(ctx, off);
    return new_offset;
  }
}
#endif

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
  // Instruction fetch only ever reads from `buffer`. `origin->addr()` is the
  // virtual address of the same instructions inside the image and is used
  // exclusively for PC-relative target computation; it is never dereferenced.
  uint8_t *buffer;
  uint32_t origin_offset;
  uint32_t preferred_size;
  uint32_t relocated_insn_count;

  CodeMemBlock *origin;
  CodeMemBlock relocated{};
  CodeMemBuffer *relocated_buffer;

  relo_ctx_t(void *in_buffer, MemBlock *origin) : origin(origin) {
    buffer = (uint8_t *)in_buffer;
    origin_offset = 0;
    preferred_size = origin->size;
  }

  uint32_t preferred_relo_size() {
    return preferred_size;
  }

  void correct_final_relo_size() {
    origin->resize(relo_size());
  }

  uint32_t relo_size() {
    return origin_offset;
  }

  addr_t origin_start() {
    return origin->addr();
  }

  addr_t origin_cursor() {
    return origin->addr() + origin_offset;
  }

  uint32_t origin_off() {
    return origin_offset;
  }

  uint32_t fetch_next_insn() {
    uint32_t inst;
    memcpy(&inst, buffer + origin_offset, sizeof(inst));
    return inst;
  }

  void advance_origin() {
    origin_offset += sizeof(uint32_t);
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

// Encode a "load/store register (unsigned offset)" instruction with offset #0
// from `rn`. `op` selects the access class: general-purpose vs SIMD&FP register,
// the access width and the sign-extension behavior. This is what lets a literal
// load keep its original register class instead of being degraded to a 64-bit
// general-purpose load.
static inline uint32_t encode_load_store_unsigned_offset(LoadStoreOp op, CPURegister rt, Register rn) {
  return LoadStoreUnsignedOffsetFixed | static_cast<uint32_t>(op) | Rn(rn) | Rt(rt);
}

int relo_ctx_t::relocate(bool branch) {
  TurboAssembler turbo_assembler_;
#undef _
#define _ turbo_assembler_. // NOLINT

  this->relocated_buffer = turbo_assembler_.code_buffer();

  while (relo_size() < preferred_relo_size()) {
    record_relo_start();

    uint32_t inst = fetch_next_insn();
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
      char opc = bits(inst, 30, 31);
      bool v = bit(inst, 26);

      // Reproduce the original access class: V selects general-purpose vs
      // SIMD&FP registers, opc selects the width / sign extension / prefetch.
      // Rt is passed through untouched so Rt=31 keeps its original meaning
      // (XZR/WZR for general registers, V31 for SIMD&FP registers).
      uint32_t load_inst;
      if (!v) {
        switch (opc) {
        case 0b00: // LDR Wt: 32-bit load, writes zero-extend into the 64-bit register
          load_inst = encode_load_store_unsigned_offset(OP_W(LDR), W(rt), TMP_REG_0);
          break;
        case 0b01: // LDR Xt: 64-bit load
          load_inst = encode_load_store_unsigned_offset(OP_X(LDR), X(rt), TMP_REG_0);
          break;
        case 0b10: // LDRSW Xt: 32-bit signed load, sign-extend into 64 bits
          load_inst = encode_load_store_unsigned_offset(OP(LDRSW), X(rt), TMP_REG_0);
          break;
        default: // PRFM: keep the prefetch hint, never turn it into a real load
          load_inst = encode_load_store_unsigned_offset(OP(PRFM), X(rt), TMP_REG_0);
          break;
        }
      } else {
        // Rt=31 is V31 here, a real SIMD&FP destination. The scalar widths
        // zero the upper part of the 128-bit register, and LDR Q fills it all.
        CPURegister vt = CPURegister::Create(rt, 32, CPURegister::kSIMD_FP_Register_32);
        LoadStoreOp vop = OP_S(LDR);
        switch (opc) {
        case 0b00: // LDR St
          break;
        case 0b01: // LDR Dt
          vt = CPURegister::Create(rt, 64, CPURegister::kSIMD_FP_Register_64);
          vop = OP_D(LDR);
          break;
        case 0b10: // LDR Qt
          vt = CPURegister::Create(rt, 128, CPURegister::kSIMD_FP_Register_128);
          vop = OP_Q(LDR);
          break;
        default:
          UNREACHABLE();
          break;
        }
        load_inst = encode_load_store_unsigned_offset(vop, vt, TMP_REG_0);
      }

      {
        // Materialize the *original* data address at run time; the loaded
        // value is never baked into a constant pool here, so a second
        // execution observes whatever the data holds then.
        _ Mov(TMP_REG_0, dst);
        _ Emit(load_inst);
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

    this->advance_origin();
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
