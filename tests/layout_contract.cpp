// Layout contract tests for SPIRV-Reflect.
//
// Two independently derived views of the same SPIR-V materials are checked
// against each other:
//
//   1. Byte addresses. A small, library-free layout reference (this file)
//      computes the byte address of every accessed scalar and writes a unique
//      asymmetric sentinel there. The address is then recomputed solely from
//      the library's reflected members, relative offsets, array strides and
//      matrix dimensions, and the sentinel is read back.
//
//   2. Descriptor relocation. Set/binding rewrites are exercised in both the
//      default copy mode and the NO_COPY mode, then verified three ways:
//      re-enumerated reflection views, a fresh module rebuilt from GetCode(),
//      and an independent scan of the OpDecorate words that never reuses a
//      reflection result as its expected answer.
//
// No shader compiler is needed at test time: every SPIR-V module is a
// committed binary under tests/layout_materials (plus checks/layout.spv).

#include "spirv_reflect.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Test accounting and failure reporting.
// ---------------------------------------------------------------------------

uint64_t g_assertions = 0;
uint64_t g_cases = 0;
std::string g_fixture = "<none>";
std::string g_failure_reason;
std::string g_failure_location;

struct ContractFailure {
  std::string reason;
};

// Assertions are identical in normal and mutation modes: there is no branch
// anywhere in this file on how the library was built.
#define CHECK(cond, detail)                                                                                           \
  do {                                                                                                                \
    ++g_assertions;                                                                                                   \
    if (!(cond)) {                                                                                                    \
      std::ostringstream os;                                                                                          \
      os << "fixture=" << g_fixture << " " << detail;                                                                 \
      g_failure_reason = os.str();                                                                                    \
      g_failure_location = std::string(__FILE__) + ":" + std::to_string(__LINE__);                                    \
      throw ContractFailure{g_failure_reason};                                                                        \
    }                                                                                                                 \
  } while (0)

// ---------------------------------------------------------------------------
// Module loading.
// ---------------------------------------------------------------------------

std::vector<uint32_t> LoadWords(const std::string& path) {
  std::ifstream in(path.c_str(), std::ios::binary | std::ios::ate);
  CHECK(static_cast<bool>(in), "cannot open " << path);
  std::streamoff bytes = in.tellg();
  CHECK(bytes > 0 && (bytes % 4) == 0, "bad size for " << path);
  std::vector<uint32_t> words(static_cast<size_t>(bytes) / 4);
  in.seekg(0);
  in.read(reinterpret_cast<char*>(words.data()), bytes);
  CHECK(static_cast<bool>(in), "short read of " << path);
  return words;
}

// ---------------------------------------------------------------------------
// Independent SPIR-V OpDecorate scan.
//
// This parser knows nothing about SPIRV-Reflect. It walks the raw word stream
// and records, per decorated <id>, the word positions and values of the
// DescriptorSet (34) and Binding (33) decoration operands. It is the source
// of truth used to judge relocation results.
// ---------------------------------------------------------------------------

struct DecorateSite {
  uint32_t target_id = 0;
  uint32_t set_word = 0;    // word index of the DescriptorSet literal
  uint32_t bind_word = 0;   // word index of the Binding literal
  uint32_t set_value = 0;
  uint32_t bind_value = 0;
  bool has_set = false;
  bool has_bind = false;
};

std::map<uint32_t, DecorateSite> ScanDecorations(const std::vector<uint32_t>& code) {
  CHECK(code.size() > 5, "module too short to scan");
  CHECK(code[0] == 0x07230203u, "bad magic while independently scanning OpDecorate");
  std::map<uint32_t, DecorateSite> sites;
  size_t pos = 5;
  while (pos < code.size()) {
    uint32_t first = code[pos];
    uint32_t word_count = first >> 16;
    uint32_t op = first & 0xFFFF;
    CHECK(word_count >= 1, "instruction word count zero at " << pos);
    CHECK(pos + word_count <= code.size(), "instruction runs past end at " << pos);
    if (op == SpvOpDecorate && word_count >= 4) {
      uint32_t target = code[pos + 1];
      uint32_t decoration = code[pos + 2];
      DecorateSite& site = sites[target];
      site.target_id = target;
      if (decoration == SpvDecorationBinding) {
        site.bind_word = static_cast<uint32_t>(pos + 3);
        site.bind_value = code[pos + 3];
        site.has_bind = true;
      } else if (decoration == SpvDecorationDescriptorSet) {
        site.set_word = static_cast<uint32_t>(pos + 3);
        site.set_value = code[pos + 3];
        site.has_set = true;
      }
    }
    pos += word_count;
  }
  return sites;
}

bool IsAllowedWord(uint32_t word_index, const std::vector<uint32_t>& allowed) {
  for (uint32_t w : allowed) {
    if (w == word_index) return true;
  }
  return false;
}

// Compare two equally sized modules word by word. Only the literal operand
// words of DescriptorSet/Binding decorations listed in `allowed` may differ.
// Layout decorations, function bodies and unrelated resources are therefore
// covered implicitly, and the first offending position is reported.
void AssertOnlyAllowedWordsChanged(const std::vector<uint32_t>& before, const std::vector<uint32_t>& after,
                                   const std::vector<uint32_t>& allowed, const char* stage) {
  CHECK(before.size() == after.size(), stage << ": module length changed " << before.size() << " -> " << after.size());
  for (size_t i = 0; i < before.size(); ++i) {
    if (before[i] != after[i]) {
      CHECK(IsAllowedWord(static_cast<uint32_t>(i), allowed),
            stage << ": word " << i << " changed outside DescriptorSet/Binding operands (" << before[i] << "->"
                  << after[i] << ")");
    }
  }
}

// ---------------------------------------------------------------------------
// Library-independent layout reference.
//
// The reference records, for each accessed scalar, the byte address derived
// from the std140/std430 facts of the material (member offsets, array strides
// and matrix strides are literal spec facts of the fixture) and a unique
// asymmetric sentinel. Sentinels are placed in a flat byte-addressed model;
// the library walk must land on the same word.
// ---------------------------------------------------------------------------

struct RefScalar {
  std::string binding;       // block variable name
  std::vector<std::string> path;  // member names from the block root
  uint32_t array_index = 0;       // index when the last-but-one node is an array (all fixtures are 1-D)
  bool has_array_index = false;
  int matrix_row = -1;            // >=0 when the final node is a matrix
  int matrix_col = -1;
  int vector_component = -1;      // >=0 when the final node is a vector
  uint32_t ref_address = 0;
  uint32_t sentinel = 0;
};

// Matrix component addressing from reference (spec) facts.
uint32_t MatrixComponentAddress(uint32_t base, uint32_t rows, uint32_t cols, bool row_major, uint32_t matrix_stride,
                                uint32_t row, uint32_t col) {
  (void)rows;
  (void)cols;
  if (row_major) {
    // Each row starts at MatrixStride; components within a row are scalars.
    return base + row * matrix_stride + col * 4;
  }
  // Column major: each column starts at MatrixStride.
  return base + col * matrix_stride + row * 4;
}

// ---------------------------------------------------------------------------
// Library-side address walk.
//
// Only reflected relative offsets, array strides and matrix dimensions are
// used. Array-of-struct children expose absolute_offset == 0 and relative
// member offsets, which is exactly the convention consumed here: every
// descent adds the child's relative offset to the running address.
// ---------------------------------------------------------------------------

const SpvReflectBlockVariable* FindMember(const SpvReflectBlockVariable& parent, const std::string& name) {
  for (uint32_t i = 0; i < parent.member_count; ++i) {
    const char* n = parent.members[i].name;
    if (n != nullptr && name == n) return &parent.members[i];
  }
  return nullptr;
}

uint32_t EffectiveArrayStride(const SpvReflectBlockVariable& v) {
  if (v.array.dims_count > 0) return v.array.stride;  // fixed array
  // Runtime arrays carry their stride on the OpTypeRuntimeArray type
  // description rather than on the block variable.
  if (v.type_description != nullptr && v.type_description->op == SpvOpTypeRuntimeArray) {
    return v.type_description->traits.array.stride;
  }
  return 0;
}

uint32_t LibraryAddress(const SpvReflectBlockVariable& block, const RefScalar& q) {
  uint32_t addr = 0;
  const SpvReflectBlockVariable* node = &block;
  bool index_applied = false;
  for (size_t i = 0; i < q.path.size(); ++i) {
    const SpvReflectBlockVariable* child = FindMember(*node, q.path[i]);
    CHECK(child != nullptr, "library tree has no member '" << q.path[i] << "'");
    addr += child->offset;  // relative offset, including AoS element members
    // The single array node on this path (fixed or runtime) contributes the
    // element index exactly once; later members are element-relative.
    bool is_array = child->array.dims_count > 0 ||
                    (child->type_description != nullptr && child->type_description->op == SpvOpTypeRuntimeArray);
    if (q.has_array_index && !index_applied && is_array) {
      addr += q.array_index * EffectiveArrayStride(*child);
      index_applied = true;
    }
    node = child;
  }
  if (q.has_array_index) CHECK(index_applied, "no array node found while resolving indexed scalar");
  if (q.matrix_row >= 0) {
    bool row_major = (node->decoration_flags & SPV_REFLECT_DECORATION_ROW_MAJOR) != 0;
    addr = MatrixComponentAddress(addr, node->numeric.matrix.row_count, node->numeric.matrix.column_count, row_major,
                                  node->numeric.matrix.stride, static_cast<uint32_t>(q.matrix_row),
                                  static_cast<uint32_t>(q.matrix_col));
  } else if (q.vector_component >= 0) {
    addr += static_cast<uint32_t>(q.vector_component) * 4;
  }
  return addr;
}

const SpvReflectDescriptorBinding* FindBinding(const SpvReflectShaderModule& m, const std::string& name) {
  for (uint32_t i = 0; i < m.descriptor_binding_count; ++i) {
    const SpvReflectDescriptorBinding& b = m.descriptor_bindings[i];
    if (b.name != nullptr && name == b.name) return &m.descriptor_bindings[i];
  }
  return nullptr;
}

// Write reference sentinels into a flat byte model, then resolve every scalar
// through the reflected tree and require both addresses to hold the sentinel.
void CrossCheckScalars(const SpvReflectShaderModule& m, const std::vector<RefScalar>& refs, size_t model_size) {
  std::vector<uint32_t> memory(model_size / 4, 0);
  for (size_t i = 0; i < refs.size(); ++i) {
    const RefScalar& q = refs[i];
    CHECK((q.ref_address % 4) == 0, "reference scalar not word aligned: " << q.ref_address);
    CHECK(q.ref_address + 4 <= memory.size() * 4, "reference address past model: " << q.ref_address);
    uint32_t word = q.ref_address / 4;
    CHECK(memory[word] == 0, "two reference scalars share address " << q.ref_address);
    memory[word] = q.sentinel;
  }
  for (size_t i = 0; i < refs.size(); ++i) {
    const RefScalar& q = refs[i];
    const SpvReflectDescriptorBinding* b = FindBinding(m, q.binding);
    CHECK(b != nullptr, "binding '" << q.binding << "' missing from reflection");
    uint32_t lib_addr = LibraryAddress(b->block, q);
    CHECK(lib_addr == q.ref_address,
          "scalar '" << q.binding << "/" << q.path.back() << "' library address " << lib_addr << " != reference "
                     << q.ref_address);
    CHECK(memory[lib_addr / 4] == q.sentinel,
          "sentinel mismatch at " << lib_addr << " for '" << q.binding << "/" << q.path.back() << "'");
  }
}

uint32_t NextSentinel(uint32_t* counter) {
  // Large, distinct, asymmetric values (never 0/1/powers of two).
  return 1000003u + 37u * (*counter)++;
}

// ---------------------------------------------------------------------------
// Fixture 1: checks/layout.spv (row-major mat2x3, struct array, std430 RTA).
// ---------------------------------------------------------------------------

void CheckBaselineLayout(uint32_t* sent) {
  g_fixture = "checks/layout.spv";
  ++g_cases;
  auto words = LoadWords("checks/layout.spv");
  SpvReflectShaderModule m{};
  CHECK(spvReflectCreateShaderModule(words.size() * 4, words.data(), &m) == SPV_REFLECT_RESULT_SUCCESS,
        "baseline module rejected");
  CHECK(m.descriptor_binding_count == 2, "baseline binding count");

  const SpvReflectDescriptorBinding* ubo = FindBinding(m, "u");
  const SpvReflectDescriptorBinding* ssb = FindBinding(m, "dst");
  CHECK(ubo != nullptr && ssb != nullptr, "baseline bindings u/dst");
  CHECK(ubo->set == 1 && ubo->binding == 2, "baseline ubo set/binding");
  CHECK(ssb->set == 2 && ssb->binding == 3, "baseline ssbo set/binding");

  // Traits of the row-major mat2x3: type, offset, MatrixStride, majority.
  const SpvReflectBlockVariable* rm = FindMember(ubo->block, "rm");
  CHECK(rm != nullptr, "rm member");
  CHECK(rm->type_description->op == SpvOpTypeMatrix, "rm is OpTypeMatrix");
  CHECK(rm->offset == 0 && rm->absolute_offset == 0, "rm offset");
  CHECK(rm->numeric.matrix.column_count == 2 && rm->numeric.matrix.row_count == 3, "rm mat2x3 dims");
  CHECK(rm->numeric.matrix.stride == 16, "rm MatrixStride 16");
  CHECK((rm->decoration_flags & SPV_REFLECT_DECORATION_ROW_MAJOR) != 0, "rm row-major decoration");
  CHECK(rm->size == 48 && rm->padded_size == 48, "rm fixed size 48");

  // Struct array: relative offsets inside an element, ArrayStride, fixed size.
  const SpvReflectBlockVariable* items = FindMember(ubo->block, "items");
  CHECK(items != nullptr, "items member");
  CHECK(items->offset == 48 && items->absolute_offset == 48, "items offset 48");
  CHECK(items->array.dims_count == 1 && items->array.dims[0] == 2, "items dims [2]");
  CHECK(items->array.stride == 16, "items ArrayStride 16");
  CHECK(items->size == 32 && items->padded_size == 32, "items fixed size 32");
  const SpvReflectBlockVariable* items_w = FindMember(*items, "w");
  const SpvReflectBlockVariable* items_v = FindMember(*items, "v");
  CHECK(items_w != nullptr && items_w->offset == 12 && items_w->absolute_offset == 0, "AoS w relative offset 12");
  CHECK(items_v != nullptr && items_v->offset == 0, "AoS v relative offset 0");
  CHECK(ubo->block.size == 80 && ubo->block.padded_size == 80, "UBO fixed/padded size 80");

  // std430 runtime tail: start, stride, prefix only; no invented capacity.
  const SpvReflectBlockVariable* values = FindMember(ssb->block, "values");
  CHECK(values != nullptr, "runtime array member");
  CHECK(values->type_description->op == SpvOpTypeRuntimeArray, "values is OpTypeRuntimeArray");
  CHECK(values->offset == 16 && values->absolute_offset == 16, "runtime tail start 16");
  CHECK(values->type_description->traits.array.stride == 16, "runtime ArrayStride 16");
  CHECK(values->array.dims_count == 0, "runtime array has no fixed dimension");
  const SpvReflectBlockVariable* head = FindMember(ssb->block, "head");
  CHECK(head != nullptr && head->offset == 0 && head->size == 4, "prefix head");
  CHECK(ssb->block.size == 0 && ssb->block.padded_size == 0, "runtime block reports no fixed size");

  std::vector<RefScalar> refs;
  // rm[1][2], row-major: base 0 + row 1*16 + col 2*4 = 24.
  refs.push_back({"u", {"rm"}, 0, false, 1, 2, -1, 24u, NextSentinel(sent)});
  // items[1].w: member 48 + element 1*16 + relative w 12 = 76.
  refs.push_back({"u", {"items", "w"}, 1, true, -1, -1, -1, 76u, NextSentinel(sent)});
  // items[0].v.z: 48 + 0 + relative v 0 + component 2*4 = 56.
  refs.push_back({"u", {"items", "v"}, 0, true, -1, -1, 2, 56u, NextSentinel(sent)});
  // dst.head at 0; values[0] starts at 16 (vec4 stride 16).
  refs.push_back({"dst", {"head"}, 0, false, -1, -1, -1, 0u, NextSentinel(sent)});
  CrossCheckScalars(m, refs, 80);

  spvReflectDestroyShaderModule(&m);
}

// ---------------------------------------------------------------------------
// Fixture 2: opposite-majority non-square matrices.
// ---------------------------------------------------------------------------

void CheckMatricesCm(uint32_t* sent) {
  g_fixture = "tests/layout_materials/matrices_cm.spv";
  ++g_cases;
  auto words = LoadWords("tests/layout_materials/matrices_cm.spv");
  SpvReflectShaderModule m{};
  CHECK(spvReflectCreateShaderModule(words.size() * 4, words.data(), &m) == SPV_REFLECT_RESULT_SUCCESS,
        "matrices_cm module rejected");
  CHECK(m.descriptor_binding_count == 4, "matrices_cm binding count");

  const SpvReflectDescriptorBinding* uc = FindBinding(m, "uc");
  const SpvReflectDescriptorBinding* br = FindBinding(m, "br");
  CHECK(uc != nullptr && br != nullptr, "uc/br bindings");

  const SpvReflectBlockVariable* cm23 = FindMember(uc->block, "cm23");
  const SpvReflectBlockVariable* cm32 = FindMember(uc->block, "cm32");
  const SpvReflectBlockVariable* rm23 = FindMember(br->block, "rm23");
  CHECK(cm23 && cm32 && rm23, "matrix members");

  CHECK(cm23->type_description->op == SpvOpTypeMatrix, "cm23 matrix type");
  CHECK(cm23->numeric.matrix.column_count == 2 && cm23->numeric.matrix.row_count == 3, "cm23 dims");
  CHECK(cm23->numeric.matrix.stride == 16, "cm23 MatrixStride 16");
  CHECK((cm23->decoration_flags & SPV_REFLECT_DECORATION_COLUMN_MAJOR) != 0, "cm23 column-major");
  CHECK(cm23->size == 32, "cm23 size 2*16=32");

  CHECK(cm32->offset == 32, "cm32 offset 32");
  CHECK(cm32->numeric.matrix.column_count == 3 && cm32->numeric.matrix.row_count == 2, "cm32 dims");
  CHECK(cm32->numeric.matrix.stride == 16, "cm32 MatrixStride 16");
  CHECK((cm32->decoration_flags & SPV_REFLECT_DECORATION_COLUMN_MAJOR) != 0, "cm32 column-major");
  CHECK(cm32->size == 48, "cm32 size 3*16=48");

  CHECK(rm23->numeric.matrix.column_count == 2 && rm23->numeric.matrix.row_count == 3, "rm23 dims");
  CHECK(rm23->numeric.matrix.stride == 8, "rm23 MatrixStride 8 (std430)");
  CHECK((rm23->decoration_flags & SPV_REFLECT_DECORATION_ROW_MAJOR) != 0, "rm23 row-major");
  CHECK(rm23->size == 24, "rm23 size 3*8=24");
  CHECK(uc->block.size == 80 && uc->block.padded_size == 80, "uc block size 80");

  std::vector<RefScalar> refs;
  // cm23[1][2] col-major: 0 + col 1*16 + row 2*4 = 24.
  refs.push_back({"uc", {"cm23"}, 0, false, 2, 1, -1, 24u, NextSentinel(sent)});
  // cm32[2][1] col-major: member 32 + col 2*16 + row 1*4 = 68.
  refs.push_back({"uc", {"cm32"}, 0, false, 1, 2, -1, 68u, NextSentinel(sent)});
  // rm23[0][1] row-major: 0 + row 0*8 + col 1*4 = 4.
  refs.push_back({"br", {"rm23"}, 0, false, 0, 1, -1, 4u, NextSentinel(sent)});
  CrossCheckScalars(m, refs, 80);

  spvReflectDestroyShaderModule(&m);
}

// ---------------------------------------------------------------------------
// Fixture 3: matrix arrays and an array of structs.
// ---------------------------------------------------------------------------

void CheckMatrixArray(uint32_t* sent) {
  g_fixture = "tests/layout_materials/matrix_array.spv";
  ++g_cases;
  auto words = LoadWords("tests/layout_materials/matrix_array.spv");
  SpvReflectShaderModule m{};
  CHECK(spvReflectCreateShaderModule(words.size() * 4, words.data(), &m) == SPV_REFLECT_RESULT_SUCCESS,
        "matrix_array module rejected");

  const SpvReflectDescriptorBinding* ua = FindBinding(m, "ua");
  const SpvReflectDescriptorBinding* ba = FindBinding(m, "ba");
  CHECK(ua != nullptr && ba != nullptr, "ua/ba bindings");

  const SpvReflectBlockVariable* rma = FindMember(ua->block, "rma");
  const SpvReflectBlockVariable* cma = FindMember(ba->block, "cma");
  const SpvReflectBlockVariable* ps = FindMember(ba->block, "ps");
  CHECK(rma && cma && ps, "array members");

  CHECK(rma->array.dims_count == 1 && rma->array.dims[0] == 2, "rma dims [2]");
  CHECK(rma->array.stride == 48, "rma ArrayStride 48");
  CHECK(rma->numeric.matrix.column_count == 2 && rma->numeric.matrix.row_count == 3, "rma mat2x3");
  CHECK(rma->numeric.matrix.stride == 16, "rma MatrixStride 16");
  CHECK((rma->decoration_flags & SPV_REFLECT_DECORATION_ROW_MAJOR) != 0, "rma row-major");
  CHECK(rma->size == 96, "rma fixed size 2*48");
  CHECK(ua->block.size == 96 && ua->block.padded_size == 96, "ua block size 96");

  CHECK(cma->array.dims_count == 1 && cma->array.dims[0] == 3, "cma dims [3]");
  CHECK(cma->array.stride == 24, "cma ArrayStride 24");
  CHECK(cma->numeric.matrix.column_count == 3 && cma->numeric.matrix.row_count == 2, "cma mat3x2");
  CHECK(cma->numeric.matrix.stride == 8, "cma MatrixStride 8");
  CHECK((cma->decoration_flags & SPV_REFLECT_DECORATION_COLUMN_MAJOR) != 0, "cma column-major");
  CHECK(cma->size == 72, "cma fixed size 3*24");

  CHECK(ps->offset == 80, "ps offset 80");
  CHECK(ps->array.dims_count == 1 && ps->array.dims[0] == 2, "ps dims [2]");
  CHECK(ps->array.stride == 32, "ps ArrayStride 32");
  CHECK(ps->size == 64, "ps fixed size 2*32");
  const SpvReflectBlockVariable* ps_v = FindMember(*ps, "v");
  const SpvReflectBlockVariable* ps_f = FindMember(*ps, "f");
  CHECK(ps_v != nullptr && ps_v->offset == 16, "P.v offset 16 (padding 4..15 is not a component)");
  CHECK(ps_f != nullptr && ps_f->offset == 0 && ps_f->size == 4, "P.f scalar size 4");

  std::vector<RefScalar> refs;
  // rma[1][0][1] row-major: 0 + 1*48 + row 0*16 + col 1*4 = 52.
  refs.push_back({"ua", {"rma"}, 1, true, 0, 1, -1, 52u, NextSentinel(sent)});
  // cma[2][1][0] col-major: 0 + 2*24 + col 1*8 + row 0 = 56.
  refs.push_back({"ba", {"cma"}, 2, true, 0, 1, -1, 56u, NextSentinel(sent)});
  // ps[1].v.y: member 80 + 1*32 + v 16 + component 1*4 = 132.
  refs.push_back({"ba", {"ps", "v"}, 1, true, -1, -1, 1, 132u, NextSentinel(sent)});
  // ps[0].f: 80 + 0 + f 0 = 80.
  refs.push_back({"ba", {"ps", "f"}, 0, true, -1, -1, -1, 80u, NextSentinel(sent)});
  CrossCheckScalars(m, refs, 144);

  spvReflectDestroyShaderModule(&m);
}

// ---------------------------------------------------------------------------
// Fixture 4: nested structs with explicit non-zero offsets, legal gaps and a
// vec3 immediately followed by a scalar.
// ---------------------------------------------------------------------------

void CheckNested(uint32_t* sent) {
  g_fixture = "tests/layout_materials/nested.spv";
  ++g_cases;
  auto words = LoadWords("tests/layout_materials/nested.spv");
  SpvReflectShaderModule m{};
  CHECK(spvReflectCreateShaderModule(words.size() * 4, words.data(), &m) == SPV_REFLECT_RESULT_SUCCESS,
        "nested module rejected");

  const SpvReflectDescriptorBinding* un = FindBinding(m, "un");
  const SpvReflectDescriptorBinding* bn = FindBinding(m, "bn");
  CHECK(un != nullptr && bn != nullptr, "un/bn bindings");

  const SpvReflectBlockVariable* x = FindMember(un->block, "x");
  const SpvReflectBlockVariable* inner = FindMember(un->block, "inner");
  const SpvReflectBlockVariable* y = FindMember(un->block, "y");
  const SpvReflectBlockVariable* q = FindMember(un->block, "q");
  const SpvReflectBlockVariable* z = FindMember(un->block, "z");
  CHECK(x && inner && y && q && z, "un members");

  // Explicit legal gap: x occupies 4 bytes but padded to the next member at 48.
  CHECK(x->offset == 0 && x->size == 4 && x->padded_size == 48, "x size 4 with explicit gap padded 48");
  CHECK(inner->offset == 48 && inner->absolute_offset == 48, "nested inner absolute offset 48");
  CHECK(inner->type_description->op == SpvOpTypeStruct, "inner is OpTypeStruct");

  // Non-array nested members carry the parent absolute offset.
  const SpvReflectBlockVariable* in_a = FindMember(*inner, "a");
  const SpvReflectBlockVariable* in_b = FindMember(*inner, "b");
  const SpvReflectBlockVariable* in_c = FindMember(*inner, "c");
  CHECK(in_a && in_b && in_c, "inner members");
  CHECK(in_a->offset == 0 && in_a->absolute_offset == 48, "inner.a absolute 48");
  CHECK(in_b->offset == 16 && in_b->absolute_offset == 64, "inner.b absolute 64");
  CHECK(in_c->offset == 28 && in_c->absolute_offset == 76, "inner.c absolute 76");
  CHECK(in_b->numeric.vector.component_count == 3, "inner.b vec3");

  CHECK(y->offset == 96 && y->absolute_offset == 96 && y->size == 4 && y->padded_size == 16,
        "y at 96 with gap to q");
  CHECK(q->offset == 112 && q->numeric.vector.component_count == 3 && q->size == 12, "q vec3 at 112");
  CHECK(z->offset == 124 && z->size == 4, "scalar z packed directly after vec3 at 124");
  CHECK(un->block.size == 128 && un->block.padded_size == 128, "un block size 128");

  const SpvReflectBlockVariable* ins = FindMember(bn->block, "ins");
  const SpvReflectBlockVariable* tail = FindMember(bn->block, "tail");
  const SpvReflectBlockVariable* v3 = FindMember(bn->block, "v3");
  const SpvReflectBlockVariable* after = FindMember(bn->block, "after");
  CHECK(ins && tail && v3 && after, "bn members");
  CHECK(ins->array.dims_count == 1 && ins->array.dims[0] == 2 && ins->array.stride == 32, "ins [2] stride 32");
  CHECK(tail->offset == 80, "explicit-gap tail at 80");
  CHECK(v3->offset == 96 && v3->numeric.vector.component_count == 3, "v3 vec3 at 96");
  CHECK(after->offset == 108, "scalar after vec3 at 108 (no padding component)");
  const SpvReflectBlockVariable* ins_c = FindMember(*ins, "c");
  CHECK(ins_c != nullptr && ins_c->offset == 28 && ins_c->absolute_offset == 0, "AoS inner c relative 28");

  std::vector<RefScalar> refs;
  refs.push_back({"un", {"x"}, 0, false, -1, -1, -1, 0u, NextSentinel(sent)});
  refs.push_back({"un", {"inner", "a"}, 0, false, -1, -1, -1, 48u, NextSentinel(sent)});
  refs.push_back({"un", {"inner", "b"}, 0, false, -1, -1, 2, 72u, NextSentinel(sent)});  // 64 + 2*4
  refs.push_back({"un", {"inner", "c"}, 0, false, -1, -1, -1, 76u, NextSentinel(sent)});
  refs.push_back({"un", {"y"}, 0, false, -1, -1, -1, 96u, NextSentinel(sent)});
  refs.push_back({"un", {"q"}, 0, false, -1, -1, 2, 120u, NextSentinel(sent)});  // 112 + 8
  refs.push_back({"un", {"z"}, 0, false, -1, -1, -1, 124u, NextSentinel(sent)});
  refs.push_back({"bn", {"ins", "a"}, 1, true, -1, -1, -1, 32u, NextSentinel(sent)});   // 1*32 + 0
  refs.push_back({"bn", {"ins", "c"}, 1, true, -1, -1, -1, 60u, NextSentinel(sent)});   // 32 + 28
  refs.push_back({"bn", {"tail"}, 0, false, -1, -1, -1, 80u, NextSentinel(sent)});
  refs.push_back({"bn", {"v3"}, 0, false, -1, -1, 2, 104u, NextSentinel(sent)});        // 96 + 8
  refs.push_back({"bn", {"after"}, 0, false, -1, -1, -1, 108u, NextSentinel(sent)});
  CrossCheckScalars(m, refs, 128);

  spvReflectDestroyShaderModule(&m);
}

// ---------------------------------------------------------------------------
// Fixture 5: std430 runtime array of structs.
// ---------------------------------------------------------------------------

void CheckRtaStruct(uint32_t* sent) {
  g_fixture = "tests/layout_materials/rta_struct.spv";
  ++g_cases;
  auto words = LoadWords("tests/layout_materials/rta_struct.spv");
  SpvReflectShaderModule m{};
  CHECK(spvReflectCreateShaderModule(words.size() * 4, words.data(), &m) == SPV_REFLECT_RESULT_SUCCESS,
        "rta_struct module rejected");

  const SpvReflectDescriptorBinding* r = FindBinding(m, "r");
  CHECK(r != nullptr, "r binding");
  const SpvReflectBlockVariable* head = FindMember(r->block, "head");
  const SpvReflectBlockVariable* cells = FindMember(r->block, "cells");
  CHECK(head != nullptr && cells != nullptr, "head/cells members");

  CHECK(head->offset == 0 && head->size == 4 && head->padded_size == 4, "prefix head");
  CHECK(cells->type_description->op == SpvOpTypeRuntimeArray, "cells OpTypeRuntimeArray");
  CHECK(cells->offset == 16 && cells->absolute_offset == 16, "runtime tail start 16");
  CHECK(cells->type_description->traits.array.stride == 16, "runtime ArrayStride 16");
  CHECK(cells->array.dims_count == 0, "no fixed capacity for runtime array");
  const SpvReflectBlockVariable* cell_v = FindMember(*cells, "v");
  const SpvReflectBlockVariable* cell_w = FindMember(*cells, "w");
  CHECK(cell_v != nullptr && cell_v->offset == 0 && cell_v->numeric.vector.component_count == 3, "Cell.v vec3");
  CHECK(cell_w != nullptr && cell_w->offset == 12, "Cell.w at relative 12");
  CHECK(r->block.size == 0 && r->block.padded_size == 0, "runtime block has no fixed size");

  std::vector<RefScalar> refs;
  refs.push_back({"r", {"head"}, 0, false, -1, -1, -1, 0u, NextSentinel(sent)});
  // cells[0].v.x: tail 16 + 0*16 + v 0 + 0 = 16.
  refs.push_back({"r", {"cells", "v"}, 0, true, -1, -1, 0, 16u, NextSentinel(sent)});
  // cells[2].w: 16 + 2*16 + w 12 = 60.
  refs.push_back({"r", {"cells", "w"}, 2, true, -1, -1, -1, 60u, NextSentinel(sent)});
  CrossCheckScalars(m, refs, 96);  // model room for the explicitly indexed cells

  spvReflectDestroyShaderModule(&m);
}

// ---------------------------------------------------------------------------
// Relocation contract, run once per module ownership mode.
// ---------------------------------------------------------------------------

struct ExpectedBinding {
  uint32_t set;
  uint32_t binding;
};

void EnumerateAndMatch(const SpvReflectShaderModule& m,
                       const std::map<uint32_t, ExpectedBinding>& expected_by_id, const char* stage) {
  // Module-level enumeration.
  uint32_t set_count = 0;
  CHECK(spvReflectEnumerateDescriptorSets(&m, &set_count, nullptr) == SPV_REFLECT_RESULT_SUCCESS,
        stage << ": enumerate module sets count");
  std::vector<SpvReflectDescriptorSet*> sets(set_count);
  CHECK(spvReflectEnumerateDescriptorSets(&m, &set_count, sets.data()) == SPV_REFLECT_RESULT_SUCCESS,
        stage << ": enumerate module sets");
  uint32_t seen = 0;
  for (uint32_t si = 0; si < set_count; ++si) {
    for (uint32_t bi = 0; bi < sets[si]->binding_count; ++bi) {
      const SpvReflectDescriptorBinding* b = sets[si]->bindings[bi];
      auto it = expected_by_id.find(b->spirv_id);
      CHECK(it != expected_by_id.end(), stage << ": unexpected reflected id " << b->spirv_id);
      CHECK(b->set == it->second.set && b->binding == it->second.binding,
            stage << ": module enum id " << b->spirv_id << " got (" << b->set << "," << b->binding << ") want ("
                  << it->second.set << "," << it->second.binding << ")");
      ++seen;
    }
  }
  CHECK(seen == expected_by_id.size(), stage << ": module enum saw " << seen << " of " << expected_by_id.size());

  // Entry-point enumeration through the main entry point.
  const SpvReflectEntryPoint* ep = spvReflectGetEntryPoint(&m, "main");
  CHECK(ep != nullptr, stage << ": main entry point");
  uint32_t ep_set_count = 0;
  CHECK(spvReflectEnumerateEntryPointDescriptorSets(&m, "main", &ep_set_count, nullptr) == SPV_REFLECT_RESULT_SUCCESS,
        stage << ": enumerate entry sets count");
  std::vector<SpvReflectDescriptorSet*> ep_sets(ep_set_count);
  CHECK(spvReflectEnumerateEntryPointDescriptorSets(&m, "main", &ep_set_count, ep_sets.data()) ==
            SPV_REFLECT_RESULT_SUCCESS,
        stage << ": enumerate entry sets");
  seen = 0;
  for (uint32_t si = 0; si < ep_set_count; ++si) {
    for (uint32_t bi = 0; bi < ep_sets[si]->binding_count; ++bi) {
      const SpvReflectDescriptorBinding* b = ep_sets[si]->bindings[bi];
      auto it = expected_by_id.find(b->spirv_id);
      CHECK(it != expected_by_id.end(), stage << ": unexpected entry id " << b->spirv_id);
      CHECK(b->set == it->second.set && b->binding == it->second.binding,
            stage << ": entry enum id " << b->spirv_id << " got (" << b->set << "," << b->binding << ") want ("
                  << it->second.set << "," << it->second.binding << ")");
      ++seen;
    }
  }
  CHECK(seen == expected_by_id.size(), stage << ": entry enum saw " << seen);
}

// Re-scan the current code independently and compare against the expected map.
void IndependentScanMatch(const std::vector<uint32_t>& code,
                          const std::map<uint32_t, ExpectedBinding>& expected_by_id, const char* stage) {
  auto sites = ScanDecorations(code);
  CHECK(sites.size() >= expected_by_id.size(), stage << ": independent scan found too few decorated ids");
  for (const auto& kv : expected_by_id) {
    auto it = sites.find(kv.first);
    CHECK(it != sites.end() && it->second.has_set && it->second.has_bind,
          stage << ": id " << kv.first << " missing set/binding decorations");
    CHECK(it->second.set_value == kv.second.set && it->second.bind_value == kv.second.binding,
          stage << ": bytecode id " << kv.first << " at words (set=" << it->second.set_word
                << ", binding=" << it->second.bind_word << ") has (" << it->second.set_value << ","
                << it->second.bind_value << ") want (" << kv.second.set << "," << kv.second.binding << ")");
  }
}

void CheckRelocationMode(SpvReflectModuleFlags flags, const char* mode_name) {
  g_fixture = std::string("tests/layout_materials/matrices_cm.spv#relocation:") + mode_name;
  ++g_cases;
  const std::string path = "tests/layout_materials/matrices_cm.spv";

  // Writable, long-lived backing storage for the NO_COPY mode; also serves as
  // the pristine snapshot in both modes.
  std::vector<uint32_t> backing = LoadWords(path);
  const std::vector<uint32_t> pristine = backing;

  auto baseline_sites = ScanDecorations(pristine);
  std::map<uint32_t, ExpectedBinding> expected;
  for (const auto& kv : baseline_sites) {
    if (kv.second.has_set && kv.second.has_bind) {
      expected[kv.first] = {kv.second.set_value, kv.second.bind_value};
    }
  }
  CHECK(expected.size() == 4, "four decorated descriptor variables");

  SpvReflectShaderModule m{};
  CHECK(spvReflectCreateShaderModule2(flags, backing.size() * 4, backing.data(), &m) == SPV_REFLECT_RESULT_SUCCESS,
        mode_name << ": module creation failed");
  EnumerateAndMatch(m, expected, mode_name);

  // --- Migration A: cross-set move of one binding (set 0/binding 0 -> 5/7).
  const SpvReflectDescriptorBinding* moving = nullptr;
  {
    uint32_t count = 0;
    CHECK(spvReflectEnumerateDescriptorBindings(&m, &count, nullptr) == SPV_REFLECT_RESULT_SUCCESS,
          mode_name << ": count bindings before migration A");
    std::vector<SpvReflectDescriptorBinding*> all(count);
    CHECK(spvReflectEnumerateDescriptorBindings(&m, &count, all.data()) == SPV_REFLECT_RESULT_SUCCESS,
          mode_name << ": enumerate bindings before migration A");
    for (auto* b : all) {
      if (b->set == 0 && b->binding == 0) moving = b;
    }
  }
  CHECK(moving != nullptr, mode_name << ": locate (0,0) binding");
  uint32_t moving_id = moving->spirv_id;
  const DecorateSite& moving_site = baseline_sites.at(moving_id);

  // The sampler is an unrelated resource: remember its id from the pristine
  // reflection so its operands can be judged after the set migration.
  uint32_t sampler_id = 0;
  {
    uint32_t count = 0;
    CHECK(spvReflectEnumerateDescriptorBindings(&m, &count, nullptr) == SPV_REFLECT_RESULT_SUCCESS,
          mode_name << ": count bindings for sampler");
    std::vector<SpvReflectDescriptorBinding*> all(count);
    CHECK(spvReflectEnumerateDescriptorBindings(&m, &count, all.data()) == SPV_REFLECT_RESULT_SUCCESS,
          mode_name << ": enumerate bindings for sampler");
    for (auto* b : all) {
      if (b->set == 0 && b->binding == 2) sampler_id = b->spirv_id;
    }
  }
  CHECK(sampler_id != 0 && baseline_sites.count(sampler_id), mode_name << ": locate sampler binding");

  CHECK(spvReflectChangeDescriptorBindingNumbers(&m, moving, 7, 5) == SPV_REFLECT_RESULT_SUCCESS,
        mode_name << ": cross-set migration failed");
  expected[moving_id] = {5, 7};

  // Pointers were invalidated by the set change: re-acquire everything.
  EnumerateAndMatch(m, expected, mode_name);

  uint32_t code_size = spvReflectGetCodeSize(&m);
  const uint32_t* code_ptr = spvReflectGetCode(&m);
  CHECK(code_ptr != nullptr && code_size == backing.size() * 4, mode_name << ": GetCode size");
  std::vector<uint32_t> after_a(code_ptr, code_ptr + code_size / 4);
  IndependentScanMatch(after_a, expected, mode_name);
  AssertOnlyAllowedWordsChanged(pristine, after_a,
                                {moving_site.set_word, moving_site.bind_word}, mode_name);
  CHECK(after_a[moving_site.set_word] == 5 && after_a[moving_site.bind_word] == 7,
        mode_name << ": migration A literal words");

  // A fresh module rebuilt from the rewritten code must agree with the scan.
  {
    SpvReflectShaderModule rebuilt{};
    CHECK(spvReflectCreateShaderModule(after_a.size() * 4, after_a.data(), &rebuilt) == SPV_REFLECT_RESULT_SUCCESS,
          mode_name << ": rebuild after A failed");
    EnumerateAndMatch(rebuilt, expected, mode_name);
    spvReflectDestroyShaderModule(&rebuilt);
  }

  // Ownership semantics for the caller's array.
  if (flags & SPV_REFLECT_MODULE_FLAG_NO_COPY) {
    CHECK(backing == after_a, mode_name << ": NO_COPY backing array must track bytecode edits");
  } else {
    CHECK(backing == pristine, mode_name << ": copy mode must leave the caller's array untouched");
  }

  // --- Migration B: move the whole remaining set 0 to set 9.
  // Re-acquire a fresh set pointer after migration A.
  const SpvReflectDescriptorSet* set_zero = nullptr;
  {
    uint32_t count = 0;
    CHECK(spvReflectEnumerateDescriptorSets(&m, &count, nullptr) == SPV_REFLECT_RESULT_SUCCESS,
          mode_name << ": count sets before migration B");
    std::vector<SpvReflectDescriptorSet*> sets(count);
    CHECK(spvReflectEnumerateDescriptorSets(&m, &count, sets.data()) == SPV_REFLECT_RESULT_SUCCESS,
          mode_name << ": enumerate sets before migration B");
    for (auto* s : sets) {
      if (s->set == 0) set_zero = s;
    }
  }
  CHECK(set_zero != nullptr && set_zero->binding_count == 3, mode_name << ": remaining set 0 has 3 bindings");

  std::vector<uint32_t> b_set_words;
  for (uint32_t i = 0; i < set_zero->binding_count; ++i) {
    b_set_words.push_back(baseline_sites.at(set_zero->bindings[i]->spirv_id).set_word);
    expected[set_zero->bindings[i]->spirv_id].set = 9;
  }
  CHECK(spvReflectChangeDescriptorSetNumber(&m, set_zero, 9) == SPV_REFLECT_RESULT_SUCCESS,
        mode_name << ": whole-set migration failed");

  // Re-acquire again: the set change invalidated binding/set pointers.
  EnumerateAndMatch(m, expected, mode_name);
  code_ptr = spvReflectGetCode(&m);
  std::vector<uint32_t> after_b(code_ptr, code_ptr + code_size / 4);
  IndependentScanMatch(after_b, expected, mode_name);

  // Cumulative difference: migration A's two words plus the three set words.
  std::vector<uint32_t> allowed = b_set_words;
  allowed.push_back(moving_site.set_word);
  allowed.push_back(moving_site.bind_word);
  AssertOnlyAllowedWordsChanged(pristine, after_b, allowed, mode_name);

  // The unrelated sampler keeps its original binding operand; only its set moved.
  const DecorateSite& sampler_site = baseline_sites.at(sampler_id);
  CHECK(after_b[sampler_site.bind_word] == pristine[sampler_site.bind_word],
        mode_name << ": sampler binding operand must not change");
  CHECK(after_b[sampler_site.bind_word] == 2, mode_name << ": sampler stays on binding 2");
  CHECK(after_b[sampler_site.set_word] == 9, mode_name << ": sampler moved with its set to 9");

  // Rebuild from the final code and match the independently derived expectation.
  {
    SpvReflectShaderModule rebuilt{};
    CHECK(spvReflectCreateShaderModule(after_b.size() * 4, after_b.data(), &rebuilt) == SPV_REFLECT_RESULT_SUCCESS,
          mode_name << ": rebuild after B failed");
    EnumerateAndMatch(rebuilt, expected, mode_name);
    spvReflectDestroyShaderModule(&rebuilt);
  }

  if (flags & SPV_REFLECT_MODULE_FLAG_NO_COPY) {
    CHECK(backing == after_b, mode_name << ": NO_COPY backing tracks final bytecode");
  } else {
    CHECK(backing == pristine, mode_name << ": copy mode backing still pristine");
  }

  spvReflectDestroyShaderModule(&m);
}

void WriteEvidence(const char* stage, int exit_code) {
  std::ofstream out("tests/layout-evidence.json");
  out << "{\n";
  out << "  \"stage\": \"" << stage << "\",\n";
  out << "  \"exit_code\": " << exit_code << ",\n";
  out << "  \"fixture\": \"" << g_fixture << "\",\n";
  out << "  \"location\": \"" << g_failure_location << "\",\n";
  out << "  \"assertion\": \"" << g_failure_reason << "\",\n";
  out << "  \"assertions_executed\": " << g_assertions << ",\n";
  out << "  \"cases_executed\": " << g_cases << "\n";
  out << "}\n";
}

}  // namespace

int main(int argc, char** argv) {
  const char* stage = (argc > 1) ? argv[1] : "normal";
  try {
    uint32_t sent = 0;
    CheckBaselineLayout(&sent);
    CheckMatricesCm(&sent);
    CheckMatrixArray(&sent);
    CheckNested(&sent);
    CheckRtaStruct(&sent);
    CheckRelocationMode(SPV_REFLECT_MODULE_FLAG_NONE, "copy");
    CheckRelocationMode(SPV_REFLECT_MODULE_FLAG_NO_COPY, "no_copy");
  } catch (const ContractFailure& f) {
    std::printf("LAYOUT_CONTRACT_FAIL %s\n", f.reason.c_str());
    WriteEvidence(stage, 1);
    return 1;
  } catch (const std::exception& e) {
    g_failure_reason = std::string("unexpected exception: ") + e.what();
    std::printf("LAYOUT_CONTRACT_FAIL %s\n", g_failure_reason.c_str());
    WriteEvidence(stage, 2);
    return 2;
  }
  std::printf("LAYOUT_NORMAL_OK cases=%llu assertions=%llu\n",
              static_cast<unsigned long long>(g_cases),
              static_cast<unsigned long long>(g_assertions));
  return 0;
}
