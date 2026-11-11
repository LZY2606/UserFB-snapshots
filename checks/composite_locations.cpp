// Checks for composite location description (DW_OP_piece /
// DW_OP_bit_piece) support in expr::evaluate.  Each case evaluates a
// hand-written DWARF expression embedded in a minimal in-memory
// .debug_info/.debug_abbrev pair and compares the resulting
// expr_result against the expected pieces.

#include "dwarf/dwarf++.hh"

#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using dwarf::expr_result;

namespace {

// A minimal DWARF loader providing a single compilation unit whose
// root DIE has a DW_AT_location attribute holding the given
// expression bytes.
struct sections : dwarf::loader {
    std::vector<unsigned char> info;
    std::vector<unsigned char> abbrev{1, 0x11, 0, 2, 0x18, 0, 0, 0};

    sections(const std::vector<unsigned char> &expr) {
        // Unit header: version 4, abbrev offset 0, address size 8,
        // then the root DIE: abbrev code 1, one exprloc attribute.
        std::vector<unsigned char> unit{4, 0, 0, 0, 0, 0, 8, 1};
        size_t n = expr.size();
        do {
            unsigned char b = n & 0x7f;
            n >>= 7;
            if (n)
                b |= 0x80;
            unit.push_back(b);
        } while (n);
        unit.insert(unit.end(), expr.begin(), expr.end());
        uint32_t len = (uint32_t)unit.size();
        info = {(unsigned char)len, (unsigned char)(len >> 8),
                (unsigned char)(len >> 16), (unsigned char)(len >> 24)};
        info.insert(info.end(), unit.begin(), unit.end());
    }

    const void *load(dwarf::section_type t, size_t *n) override {
        auto *v = t == dwarf::section_type::info ? &info :
            t == dwarf::section_type::abbrev ? &abbrev : nullptr;
        if (!v)
            return nullptr;
        *n = v->size();
        return v->data();
    }
};

// Evaluate the given expression bytes as a DW_AT_location.  The DWARF
// object is destroyed before this returns, so results must not depend
// on its lifetime.
expr_result eval(const std::vector<unsigned char> &expr_bytes,
                 std::initializer_list<dwarf::taddr> args = {}) {
    auto s = std::make_shared<sections>(expr_bytes);
    dwarf::dwarf d(s);
    const auto &u = d.compilation_units().at(0);
    return u.root()[dwarf::DW_AT::location]
        .as_exprloc().evaluate(&dwarf::no_expr_context, args);
}

expr_result eval_with(dwarf::expr_context *ctx,
                      const std::vector<unsigned char> &expr_bytes) {
    auto s = std::make_shared<sections>(expr_bytes);
    dwarf::dwarf d(s);
    const auto &u = d.compilation_units().at(0);
    return u.root()[dwarf::DW_AT::location].as_exprloc().evaluate(ctx);
}

[[noreturn]] void fail(const std::string &msg, int line) {
    throw std::runtime_error("line " + std::to_string(line) + ": " + msg);
}

#define CHECK(cond) do { if (!(cond)) fail("check failed: " #cond, __LINE__); } while (0)

using source = expr_result::piece::source;

void check_piece(const expr_result::piece &p, uint64_t bit_offset,
                 uint64_t bit_size, source src, uint64_t value,
                 uint64_t source_bit_offset) {
    CHECK(p.bit_offset == bit_offset);
    CHECK(p.bit_size == bit_size);
    CHECK(p.location_type == src);
    if (src != source::implicit)
        CHECK(p.implicit_data.empty());
    if (src == source::address || src == source::reg || src == source::literal)
        CHECK(p.value == value);
    CHECK(p.source_bit_offset == source_bit_offset);
}

// Append the 8-byte little-endian encoding of addr.
void append_addr(std::vector<unsigned char> &v, uint64_t addr) {
    for (int i = 0; i < 8; i++)
        v.push_back((unsigned char)(addr >> (i * 8)));
}

// Case 1: an object split between a register and memory.
void case_mixed_reg_mem() {
    // DW_OP_reg0; DW_OP_piece 4; DW_OP_addr 0x1122334455667788; DW_OP_piece 4
    std::vector<unsigned char> e{0x50, 0x93, 0x04, 0x03};
    append_addr(e, 0x1122334455667788);
    e.insert(e.end(), {0x93, 0x04});

    auto r = eval(e);
    CHECK(r.location_type == expr_result::type::composite);
    CHECK(r.pieces.size() == 2);
    check_piece(r.pieces[0], 0, 32, source::reg, 0, 0);
    check_piece(r.pieces[1], 32, 32, source::address, 0x1122334455667788, 0);
}

// Case 2: three bit pieces with per-source bit offsets.
void case_three_bit_pieces() {
    // DW_OP_reg1; DW_OP_bit_piece 4 0;
    // DW_OP_reg2; DW_OP_bit_piece 4 4;
    // DW_OP_lit7; DW_OP_stack_value; DW_OP_bit_piece 8 0
    auto r = eval({0x51, 0x9d, 0x04, 0x00,
                   0x52, 0x9d, 0x04, 0x04,
                   0x37, 0x9f, 0x9d, 0x08, 0x00});
    CHECK(r.location_type == expr_result::type::composite);
    CHECK(r.pieces.size() == 3);
    check_piece(r.pieces[0], 0, 4, source::reg, 1, 0);
    check_piece(r.pieces[1], 4, 4, source::reg, 2, 4);
    check_piece(r.pieces[2], 8, 8, source::literal, 7, 0);
}

// Case 3: a piece with no preceding location description is an
// unavailable (empty) piece and stays in sequence.
void case_missing_middle() {
    // DW_OP_reg0; DW_OP_piece 4; DW_OP_piece 4;
    // DW_OP_addr 0x2000; DW_OP_piece 4
    std::vector<unsigned char> e{0x50, 0x93, 0x04, 0x93, 0x04, 0x03};
    append_addr(e, 0x2000);
    e.insert(e.end(), {0x93, 0x04});

    auto r = eval(e);
    CHECK(r.location_type == expr_result::type::composite);
    CHECK(r.pieces.size() == 3);
    check_piece(r.pieces[0], 0, 32, source::reg, 0, 0);
    check_piece(r.pieces[1], 32, 32, source::empty, 0, 0);
    check_piece(r.pieces[2], 64, 32, source::address, 0x2000, 0);
}

// Case 4: implicit piece bytes are owned by the result; they survive
// both the destruction of the DWARF object (which happened before
// eval returned) and copies of the result.
void case_owned_implicit_data() {
    // DW_OP_implicit_value 2 0xAA 0xBB; DW_OP_piece 2
    auto r = eval({0x9e, 0x02, 0xAA, 0xBB, 0x93, 0x02});
    CHECK(r.location_type == expr_result::type::composite);
    CHECK(r.pieces.size() == 1);
    check_piece(r.pieces[0], 0, 16, source::implicit, 0, 0);
    CHECK(r.pieces[0].implicit_data.size() == 2);
    CHECK((unsigned char)r.pieces[0].implicit_data[0] == 0xAA);
    CHECK((unsigned char)r.pieces[0].implicit_data[1] == 0xBB);

    auto copy = r;
    CHECK(copy.pieces.size() == 1);
    CHECK(copy.pieces[0].implicit_data.size() == 2);
    CHECK((unsigned char)copy.pieces[0].implicit_data[0] == 0xAA);
    CHECK((unsigned char)copy.pieces[0].implicit_data[1] == 0xBB);
}

// Case 5: after a piece, neither the location type nor stack operands
// carry over into the next piece.
void case_state_cleared_between_pieces() {
    // DW_OP_lit1; DW_OP_reg3; DW_OP_piece 2;
    // DW_OP_piece 2;
    // DW_OP_reg5; DW_OP_piece 2
    auto r = eval({0x31, 0x53, 0x93, 0x02,
                   0x93, 0x02,
                   0x55, 0x93, 0x02});
    CHECK(r.location_type == expr_result::type::composite);
    CHECK(r.pieces.size() == 3);
    check_piece(r.pieces[0], 0, 16, source::reg, 3, 0);
    // The lit1 operand and the reg3 location type must not leak into
    // this piece: with a cleared stack it is an unavailable piece.
    check_piece(r.pieces[1], 16, 16, source::empty, 0, 0);
    check_piece(r.pieces[2], 32, 16, source::reg, 5, 0);
}

// Case 6: corrupt inputs are rejected.  Bad LEB128 and truncated
// implicit data take the library's usual section-underflow path;
// dangling location descriptions and overflowing sizes are
// expr_errors.
void case_corrupt_inputs() {
    // Bad LEB128: DW_OP_piece with an unterminated ULEB128 operand.
    try {
        eval({0x50, 0x93, 0x80});
        CHECK(!"expected underflow_error");
    } catch (const std::underflow_error &) {
    }

    // Truncated implicit data: 4 bytes promised, 1 present.
    try {
        eval({0x9e, 0x04, 0xAA, 0x93, 0x04});
        CHECK(!"expected underflow_error");
    } catch (const std::underflow_error &) {
    }

    // A location description left dangling after the final piece.
    try {
        eval({0x50, 0x93, 0x04, 0x51});
        CHECK(!"expected expr_error");
    } catch (const dwarf::expr_error &) {
    }

    // DW_OP_bit_piece whose source bit range wraps around:
    // size = UINT64_MAX, offset = 1.
    try {
        eval({0x50, 0x9d,
              0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01,
              0x01});
        CHECK(!"expected expr_error");
    } catch (const dwarf::expr_error &) {
    }

    // DW_OP_piece whose byte size overflows when converted to bits:
    // size = 2**61 bytes.
    try {
        eval({0x50, 0x93,
              0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x20});
        CHECK(!"expected expr_error");
    } catch (const dwarf::expr_error &) {
    }
}

struct reg_context : dwarf::expr_context {
    dwarf::taddr regs[2] = {0x1000, 0x2000};
    dwarf::taddr reg(unsigned regnum) override {
        if (regnum >= 2)
            throw dwarf::expr_error("bad register");
        return regs[regnum];
    }
};

// Case 7: zero-length pieces are preserved in order, and register
// based addressing computes memory addresses via the context.
void case_zero_length_and_breg() {
    // DW_OP_breg0 +8; DW_OP_piece 0; DW_OP_breg1 -4; DW_OP_piece 4
    reg_context ctx;
    auto r = eval_with(&ctx, {0x70, 0x08, 0x93, 0x00,
                              0x71, 0x7c, 0x93, 0x04});
    CHECK(r.location_type == expr_result::type::composite);
    CHECK(r.pieces.size() == 2);
    check_piece(r.pieces[0], 0, 0, source::address, 0x1008, 0);
    check_piece(r.pieces[1], 0, 32, source::address, 0x1ffc, 0);
}

// Case 8: single-location results, plain arithmetic expressions, and
// caller-supplied arguments behave as before.
void case_legacy_results() {
    // DW_OP_reg5
    auto r = eval({0x55});
    CHECK(r.location_type == expr_result::type::reg);
    CHECK(r.value == 5);
    CHECK(r.pieces.empty());

    // DW_OP_lit1; DW_OP_lit2; DW_OP_plus
    r = eval({0x31, 0x32, 0x22});
    CHECK(r.location_type == expr_result::type::address);
    CHECK(r.value == 3);
    CHECK(r.pieces.empty());

    // DW_OP_nop with a caller-supplied argument on the stack
    r = eval({0x96}, {42});
    CHECK(r.location_type == expr_result::type::address);
    CHECK(r.value == 42);
    CHECK(r.pieces.empty());
}

} // namespace

int main() {
    struct {
        const char *name;
        void (*fn)();
    } cases[] = {
        {"mixed register and memory pieces", case_mixed_reg_mem},
        {"three bit pieces", case_three_bit_pieces},
        {"unavailable middle piece", case_missing_middle},
        {"owned implicit piece data", case_owned_implicit_data},
        {"state cleared between pieces", case_state_cleared_between_pieces},
        {"corrupt inputs", case_corrupt_inputs},
        {"zero-length piece and breg", case_zero_length_and_breg},
        {"legacy single-location results", case_legacy_results},
    };

    int failures = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        try {
            cases[i].fn();
            std::cout << "ok " << (i + 1) << " - " << cases[i].name << "\n";
        } catch (const std::exception &e) {
            std::cout << "not ok " << (i + 1) << " - " << cases[i].name
                      << ": " << e.what() << "\n";
            failures++;
        }
    }
    if (failures) {
        std::cout << "COMPOSITE_LOCATIONS_FAILED failures=" << failures << "\n";
        return 1;
    }
    std::cout << "COMPOSITE_LOCATIONS_OK cases="
              << (sizeof(cases) / sizeof(cases[0])) << "\n";
    return 0;
}
