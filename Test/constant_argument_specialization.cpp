//
// Copyright (C) 2026 The Khronos Group Inc.
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
//    Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//
//    Redistributions in binary form must reproduce the above
//    copyright notice, this list of conditions and the following
//    disclaimer in the documentation and/or other materials provided
//    with the distribution.
//
//    Neither the name of 3Dlabs Inc. Ltd. nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
// OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.
//

//
// Regression tests for glslang::SpecializeConstantArguments.  Registered
// through task_support/check.h; each case returns 0 on success.
//

#include "task_support/check.h"

#include "glslang/Public/ShaderLang.h"
#include "glslang/Public/ResourceLimits.h"
#include "glslang/MachineIndependent/localintermediate.h"
#include "glslang/MachineIndependent/constantArgumentSpecialization.h"
#include "SPIRV/GlslangToSpv.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

// The driver (task_support/driver.c) knows nothing about glslang process
// lifetime, so manage it with a static object here.  Only initialization is
// needed; the process image is torn down at exit anyway, and finalizing
// glslang from a static destructor races its own static teardown.
struct TProcessLifetime {
    TProcessLifetime() { glslang::InitializeProcess(); }
} processLifetime;

const char* const kMarker = "#cas#";

// Compile and link one fragment shader; the object keeps the IR alive.
struct TShaderUnit {
    glslang::TShader shader{EShLangFragment};
    glslang::TProgram program;

    bool compile(const char* source, bool keepUncalled = false)
    {
        shader.setStrings(&source, 1);
        shader.setEnvInput(glslang::EShSourceGlsl, EShLangFragment, glslang::EShClientVulkan, 450);
        shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
        shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
        EShMessages messages = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
        if (keepUncalled)
            messages = (EShMessages)(messages | EShMsgKeepUncalled);
        if (!shader.parse(GetDefaultResources(), 450, false, messages))
            return false;
        program.addShader(&shader);
        return program.link(messages);
    }

    glslang::TIntermediate* ir() { return program.getIntermediate(EShLangFragment); }
};

// Generic read-only recorder over a subtree.
struct TRecorder : glslang::TIntermTraverser {
    std::vector<glslang::TIntermAggregate*> aggregates;
    std::vector<glslang::TIntermConstantUnion*> constants;
    std::vector<glslang::TIntermSymbol*> symbols;
    std::vector<glslang::TIntermBinary*> binaries;
    std::vector<glslang::TIntermUnary*> unaries;
    std::vector<glslang::TIntermSelection*> selections;
    std::vector<glslang::TIntermLoop*> loops;
    std::vector<glslang::TIntermBranch*> branches;
    std::vector<glslang::TIntermSwitch*> switches;
    std::vector<glslang::TIntermVariableDecl*> decls;

    TRecorder() : glslang::TIntermTraverser(true, false, false, false, true) { }

    bool visitAggregate(glslang::TVisit, glslang::TIntermAggregate* n) override { aggregates.push_back(n); return true; }
    void visitConstantUnion(glslang::TIntermConstantUnion* n) override { constants.push_back(n); }
    void visitSymbol(glslang::TIntermSymbol* n) override { symbols.push_back(n); }
    bool visitBinary(glslang::TVisit, glslang::TIntermBinary* n) override { binaries.push_back(n); return true; }
    bool visitUnary(glslang::TVisit, glslang::TIntermUnary* n) override { unaries.push_back(n); return true; }
    bool visitSelection(glslang::TVisit, glslang::TIntermSelection* n) override { selections.push_back(n); return true; }
    bool visitLoop(glslang::TVisit, glslang::TIntermLoop* n) override { loops.push_back(n); return true; }
    bool visitBranch(glslang::TVisit, glslang::TIntermBranch* n) override { branches.push_back(n); return true; }
    bool visitSwitch(glslang::TVisit, glslang::TIntermSwitch* n) override { switches.push_back(n); return true; }
    bool visitVariableDecl(glslang::TVisit, glslang::TIntermVariableDecl* n) override { decls.push_back(n); return true; }

    // Every node pointer this recorder was shown, for sharing checks.
    std::set<const void*> nodeSet() const
    {
        std::set<const void*> nodes;
        for (auto* n : aggregates) nodes.insert(n);
        for (auto* n : constants) nodes.insert(n);
        for (auto* n : symbols) nodes.insert(n);
        for (auto* n : binaries) nodes.insert(n);
        for (auto* n : unaries) nodes.insert(n);
        for (auto* n : selections) nodes.insert(n);
        for (auto* n : loops) nodes.insert(n);
        for (auto* n : branches) nodes.insert(n);
        for (auto* n : switches) nodes.insert(n);
        for (auto* n : decls) nodes.insert(n);
        return nodes;
    }

    std::vector<glslang::TIntermAggregate*> userCalls() const
    {
        std::vector<glslang::TIntermAggregate*> calls;
        for (auto* n : aggregates)
            if (n->getOp() == glslang::EOpFunctionCall && n->isUserDefined())
                calls.push_back(n);
        return calls;
    }

    std::set<long long> symbolIds() const
    {
        std::set<long long> ids;
        for (auto* s : symbols)
            ids.insert(s->getId());
        return ids;
    }

    bool hasIntConstant(int value) const
    {
        for (auto* c : constants)
            if (c->getBasicType() == glslang::EbtInt && c->getConstArray().size() == 1 &&
                c->getConstArray()[0].getIConst() == value)
                return true;
        return false;
    }

    bool hasUintConstant(unsigned int value) const
    {
        for (auto* c : constants)
            if (c->getBasicType() == glslang::EbtUint && c->getConstArray().size() == 1 &&
                c->getConstArray()[0].getUConst() == value)
                return true;
        return false;
    }

    bool hasBoolConstant(bool value) const
    {
        for (auto* c : constants)
            if (c->getBasicType() == glslang::EbtBool && c->getConstArray().size() == 1 &&
                c->getConstArray()[0].getBConst() == value)
                return true;
        return false;
    }
};

TRecorder record(TIntermNode* subtree)
{
    TRecorder r;
    subtree->traverse(&r);
    return r;
}

std::vector<glslang::TIntermAggregate*> functionDefs(glslang::TIntermediate* ir)
{
    std::vector<glslang::TIntermAggregate*> defs;
    glslang::TIntermAggregate* root = ir->getTreeRoot()->getAsAggregate();
    for (auto* node : root->getSequence()) {
        glslang::TIntermAggregate* aggregate = node->getAsAggregate();
        if (aggregate != nullptr && aggregate->getOp() == glslang::EOpFunction)
            defs.push_back(aggregate);
    }
    return defs;
}

// Original (source-level) function definitions whose name contains 'sub'.
std::vector<glslang::TIntermAggregate*> originalsNamed(glslang::TIntermediate* ir, const std::string& sub)
{
    std::vector<glslang::TIntermAggregate*> out;
    for (auto* def : functionDefs(ir))
        if (def->getName().find(sub.c_str()) != std::string::npos &&
            def->getName().find(kMarker) == std::string::npos)
            out.push_back(def);
    return out;
}

// Specialized versions derived from functions whose name contains 'sub'.
std::vector<glslang::TIntermAggregate*> versionsNamed(glslang::TIntermediate* ir, const std::string& sub)
{
    std::vector<glslang::TIntermAggregate*> out;
    for (auto* def : functionDefs(ir))
        if (def->getName().find(sub.c_str()) != std::string::npos &&
            def->getName().find(kMarker) != std::string::npos)
            out.push_back(def);
    return out;
}

glslang::TIntermAggregate* paramsOf(glslang::TIntermAggregate* def)
{
    return def->getSequence()[0]->getAsAggregate();
}

size_t paramCount(glslang::TIntermAggregate* def)
{
    return paramsOf(def)->getSequence().size();
}

// Feed the IR to the stock SPIR-V generator and verify the function
// definitions, formal parameters, and call operands line up.
bool spvFunctionsConsistent(glslang::TIntermediate* ir, unsigned expectedFunctions, unsigned expectedCalls)
{
    std::vector<unsigned> words;
    glslang::SpvOptions options;
    options.disableOptimizer = true;
    glslang::GlslangToSpv(*ir, words, &options);
    if (!(words.size() > 5 && words[0] == 0x07230203))
        return false;
    // SPIR-V OpFunction=54, OpFunctionParameter=55, OpFunctionEnd=56, OpFunctionCall=57.
    std::map<unsigned, unsigned> parameters;
    std::vector<std::pair<unsigned, unsigned>> calls;
    unsigned current = 0, functions = 0;
    for (size_t i = 5; i < words.size();) {
        unsigned count = words[i] >> 16, op = words[i] & 65535;
        if (count == 0 || i + count > words.size())
            return false;
        if (op == 54) {
            if (count != 5)
                return false;
            current = words[i + 2];
            parameters[current] = 0;
            ++functions;
        }
        if (op == 55) {
            if (current == 0 || count != 3)
                return false;
            ++parameters[current];
        }
        if (op == 57) {
            if (count < 4)
                return false;
            calls.push_back({words[i + 3], count - 4});
        }
        if (op == 56)
            current = 0;
        i += count;
    }
    if (functions != expectedFunctions || calls.size() != expectedCalls)
        return false;
    for (auto call : calls)
        if (!parameters.count(call.first) || parameters[call.first] != call.second)
            return false;
    return true;
}

bool callTargets(glslang::TIntermAggregate* call, glslang::TIntermAggregate* def)
{
    return call->getName() == def->getName();
}

// ---------------------------------------------------------------- binding

int binding_basic_int()
{
    const char* source = R"glsl(#version 450
layout(location=0) flat in int mode;
layout(location=1) in float value;
layout(location=0) out vec4 color;
float material(const in int kind, float x) {
    float y = x * 2.0;
    if (kind == 7) return y + 3.0;
    return y - 5.0;
}
void main() {
    color = vec4(material(4, value), material(9, value + 1.0), material(mode, value - 1.0), 1.0);
}
)glsl";
    TShaderUnit unit;
    TASK_CHECK(unit.compile(source));
    glslang::TIntermediate* ir = unit.ir();
    TASK_CHECK(ir != nullptr);

    // Identity of the bound parameter before the transform.
    auto originals = originalsNamed(ir, "material");
    TASK_CHECK(originals.size() == 1);
    glslang::TIntermAggregate* material = originals[0];
    TASK_CHECK(paramCount(material) == 2);
    const long long kindId = paramsOf(material)->getSequence()[0]->getAsSymbolNode()->getId();
    const std::string materialName = material->getName().c_str();

    glslang::SpecializationStats stats;
    stats.createdVersions = 99;
    stats.rewrittenCalls = 99;
    glslang::SpecializeConstantArguments(*ir, 8, stats);
    TASK_CHECK(stats.createdVersions == 2);
    TASK_CHECK(stats.rewrittenCalls == 2);

    // Original kept for the dynamic call; two versions added.
    TASK_CHECK(functionDefs(ir).size() == 4);
    auto versions = versionsNamed(ir, "material");
    TASK_CHECK(versions.size() == 2);

    // Generated names are stable and derived from the original identity.
    std::set<std::string> versionNames;
    for (auto* v : versions)
        versionNames.insert(v->getName().c_str());
    TASK_CHECK(versionNames.count(materialName + "#cas#0i4") == 1);
    TASK_CHECK(versionNames.count(materialName + "#cas#0i9") == 1);

    for (auto* v : versions) {
        // Return type and control flow survive; only the bound slot is gone.
        TASK_CHECK(v->getType().getBasicType() == glslang::EbtFloat);
        TASK_CHECK(paramCount(v) == 1);
        glslang::TIntermSymbol* kept = paramsOf(v)->getSequence()[0]->getAsSymbolNode();
        TASK_CHECK(kept != nullptr);
        TASK_CHECK(kept->getType().getBasicType() == glslang::EbtFloat);
        TASK_CHECK(kept->getQualifier().storage == glslang::EvqIn);
        TRecorder body = record(v);
        // The bound value shows up as a plain int constant of the right value...
        const int expect = v->getName().find("0i4") != std::string::npos ? 4 : 9;
        TASK_CHECK(body.hasIntConstant(expect));
        // ...and no read of the original bound parameter remains.
        TASK_CHECK(body.symbolIds().count(kindId) == 0);
        // No const-in scalar parameter is left on the version.
        for (auto* p : paramsOf(v)->getSequence())
            TASK_CHECK(p->getAsTyped()->getQualifier().storage != glslang::EvqConstReadOnly);
    }

    // Call sites: two rewritten to the versions with one argument, the
    // dynamic one still calls the original with both arguments.
    TRecorder mainBody = record(originalsNamed(ir, "main")[0]);
    auto calls = mainBody.userCalls();
    TASK_CHECK(calls.size() == 3);
    unsigned rewritten = 0, dynamic = 0;
    for (auto* call : calls) {
        if (call->getName().find(kMarker) != std::string::npos) {
            ++rewritten;
            TASK_CHECK(call->getSequence().size() == 1);
            TASK_CHECK(call->getQualifierList().size() == 1);
            TASK_CHECK(call->getQualifierList()[0] == glslang::EvqIn);
        } else {
            ++dynamic;
            TASK_CHECK(callTargets(call, material));
            TASK_CHECK(call->getSequence().size() == 2);
            TASK_CHECK(call->getQualifierList().size() == 2);
            TASK_CHECK(call->getQualifierList()[0] == glslang::EvqConstReadOnly);
        }
    }
    TASK_CHECK(rewritten == 2 && dynamic == 1);

    TASK_CHECK(spvFunctionsConsistent(ir, 4, 3));
    return 0;
}

int binding_argument_kinds()
{
    const char* source = R"glsl(#version 450
layout(location=0) flat in int mode;
layout(location=1) in float value;
layout(location=0) out vec4 color;
layout(constant_id=0) const int specKind = 3;
const int foldedKind = 5;
float pick(const in uint sel, const in bool flag, const in ivec2 pair, in int dyn, float x) {
    return x + float(sel) + (flag ? 10.0 : 20.0) + float(pair.x) + float(dyn);
}
float tagged(const in int k, float x) { return k == 2 ? x : -x; }
void main() {
    color = vec4(pick(3u, true, ivec2(1, 2), mode, value),
                 pick(4u, false, ivec2(3, 4), mode, value + 1.0),
                 tagged(2, value), 1.0);
    color.x += tagged(specKind, value);
    color.y += tagged(foldedKind, value);
}
)glsl";
    TShaderUnit unit;
    TASK_CHECK(unit.compile(source));
    glslang::TIntermediate* ir = unit.ir();

    glslang::SpecializationStats stats;
    glslang::SpecializeConstantArguments(*ir, 16, stats);
    // uint+bool pairs bind; the ivec2 const-in and the writable 'in' do not.
    // The literal tagged(2, ...) binds, and so does the front-end const
    // 'foldedKind', which the parser has already folded to a plain constant
    // node at the call site.  The specialization constant stays a symbol
    // and is not bound.
    TASK_CHECK(stats.createdVersions == 4);
    TASK_CHECK(stats.rewrittenCalls == 4);
    TASK_CHECK(functionDefs(ir).size() == 7);

    auto pickVersions = versionsNamed(ir, "pick");
    TASK_CHECK(pickVersions.size() == 2);
    std::set<std::string> names;
    for (auto* v : pickVersions)
        names.insert(v->getName().c_str());
    TASK_CHECK(names.begin()->find("#cas#0u") != std::string::npos);
    bool sawTrue = false, sawFalse = false;
    for (auto* v : pickVersions) {
        // pair, dyn, x remain, in order.
        TASK_CHECK(paramCount(v) == 3);
        auto& seq = paramsOf(v)->getSequence();
        TASK_CHECK(seq[0]->getAsTyped()->getBasicType() == glslang::EbtInt && seq[0]->getAsTyped()->isVector());
        TASK_CHECK(seq[1]->getAsTyped()->getBasicType() == glslang::EbtInt && seq[1]->getAsTyped()->isScalar());
        TASK_CHECK(seq[1]->getAsTyped()->getQualifier().storage == glslang::EvqIn);
        TASK_CHECK(seq[2]->getAsTyped()->getBasicType() == glslang::EbtFloat);
        TRecorder body = record(v);
        if (v->getName().find("0u3") != std::string::npos) {
            TASK_CHECK(body.hasUintConstant(3u));
            TASK_CHECK(body.hasBoolConstant(true));
            sawTrue = true;
        } else {
            TASK_CHECK(body.hasUintConstant(4u));
            TASK_CHECK(body.hasBoolConstant(false));
            sawFalse = true;
        }
    }
    TASK_CHECK(sawTrue && sawFalse);

    auto taggedVersions = versionsNamed(ir, "tagged");
    TASK_CHECK(taggedVersions.size() == 2);
    bool sawTwo = false, sawFive = false;
    for (auto* v : taggedVersions) {
        TASK_CHECK(paramCount(v) == 1);
        TRecorder body = record(v);
        if (v->getName().find("0i2") != std::string::npos) {
            TASK_CHECK(body.hasIntConstant(2));
            sawTwo = true;
        } else {
            TASK_CHECK(v->getName().find("0i5") != std::string::npos);
            TASK_CHECK(body.hasIntConstant(5));
            sawFive = true;
        }
    }
    TASK_CHECK(sawTwo && sawFive);

    // Only the spec-constant call stays on the original tagged, with its
    // (non-constant-node) argument list unaltered.
    TRecorder mainBody = record(originalsNamed(ir, "main")[0]);
    unsigned originalTaggedCalls = 0;
    for (auto* call : mainBody.userCalls()) {
        if (call->getName().find("tagged") != std::string::npos &&
            call->getName().find(kMarker) == std::string::npos) {
            ++originalTaggedCalls;
            TASK_CHECK(call->getSequence().size() == 2);
            TASK_CHECK(call->getSequence()[0]->getAsConstantUnion() == nullptr);
            TASK_CHECK(call->getSequence()[0]->getAsSymbolNode() != nullptr);
        }
    }
    TASK_CHECK(originalTaggedCalls == 1);

    TASK_CHECK(spvFunctionsConsistent(ir, 7, 5));
    return 0;
}

int binding_out_inout_kept()
{
    const char* source = R"glsl(#version 450
layout(location=0) flat in int mode;
layout(location=1) in float value;
layout(location=0) out vec4 color;
void combine(const in int lane, float src, out float dst, inout float acc) {
    dst = src * float(lane);
    acc += dst;
}
void main() {
    float a = value;
    float b = 0.0;
    float c = 1.0;
    combine(5, value, b, a);
    combine(mode, value + 1.0, c, a);
    color = vec4(a, b, c, 1.0);
}
)glsl";
    TShaderUnit unit;
    TASK_CHECK(unit.compile(source));
    glslang::TIntermediate* ir = unit.ir();

    glslang::SpecializationStats stats;
    glslang::SpecializeConstantArguments(*ir, 4, stats);
    TASK_CHECK(stats.createdVersions == 1);
    TASK_CHECK(stats.rewrittenCalls == 1);
    TASK_CHECK(functionDefs(ir).size() == 3);

    auto versions = versionsNamed(ir, "combine");
    TASK_CHECK(versions.size() == 1);
    glslang::TIntermAggregate* version = versions[0];
    // src/dst/acc keep their order, types, and qualifiers.
    TASK_CHECK(paramCount(version) == 3);
    auto& seq = paramsOf(version)->getSequence();
    TASK_CHECK(seq[0]->getAsTyped()->getQualifier().storage == glslang::EvqIn);
    TASK_CHECK(seq[1]->getAsTyped()->getQualifier().storage == glslang::EvqOut);
    TASK_CHECK(seq[2]->getAsTyped()->getQualifier().storage == glslang::EvqInOut);
    for (auto* p : seq)
        TASK_CHECK(p->getAsTyped()->getBasicType() == glslang::EbtFloat);
    TASK_CHECK(record(version).hasIntConstant(5));

    TRecorder mainBody = record(originalsNamed(ir, "main")[0]);
    auto calls = mainBody.userCalls();
    TASK_CHECK(calls.size() == 2);
    for (auto* call : calls) {
        if (callTargets(call, version)) {
            TASK_CHECK(call->getSequence().size() == 3);
            TASK_CHECK(call->getQualifierList().size() == 3);
            TASK_CHECK(call->getQualifierList()[0] == glslang::EvqIn);
            TASK_CHECK(call->getQualifierList()[1] == glslang::EvqOut);
            TASK_CHECK(call->getQualifierList()[2] == glslang::EvqInOut);
        } else {
            // The dynamic call keeps all four slots.
            TASK_CHECK(call->getSequence().size() == 4);
            TASK_CHECK(call->getQualifierList().size() == 4);
            TASK_CHECK(call->getQualifierList()[0] == glslang::EvqConstReadOnly);
            TASK_CHECK(call->getQualifierList()[2] == glslang::EvqOut);
            TASK_CHECK(call->getQualifierList()[3] == glslang::EvqInOut);
        }
    }

    TASK_CHECK(spvFunctionsConsistent(ir, 3, 2));
    return 0;
}

// ---------------------------------------------------------------- ownership

int ownership_locals_and_params()
{
    const char* source = R"glsl(#version 450
layout(location=1) in float value;
layout(location=0) out vec4 color;
layout(set = 0, binding = 0) uniform Mat { float factor; } mat;
float shade(const in int path, float x) {
    float t = x * mat.factor;
    float u = t + float(path);
    return u * 2.0;
}
void main() {
    color = vec4(shade(4, value), shade(8, value + 1.0), shade(4, value + 2.0), 1.0);
}
)glsl";
    TShaderUnit unit;
    TASK_CHECK(unit.compile(source));
    glslang::TIntermediate* ir = unit.ir();

    glslang::SpecializationStats stats;
    glslang::SpecializeConstantArguments(*ir, 8, stats);
    // shade(4, ...) appears twice: one version, two rewritten calls.
    TASK_CHECK(stats.createdVersions == 2);
    TASK_CHECK(stats.rewrittenCalls == 3);

    auto originals = originalsNamed(ir, "shade");
    TASK_CHECK(originals.size() == 1);
    auto versions = versionsNamed(ir, "shade");
    TASK_CHECK(versions.size() == 2);

    // Global (shared) symbol ids are those visible at the top level.
    std::set<long long> globalIds;
    for (auto* node : ir->getTreeRoot()->getAsAggregate()->getSequence()) {
        auto* aggregate = node->getAsAggregate();
        if (aggregate != nullptr && aggregate->getOp() == glslang::EOpLinkerObjects) {
            TRecorder linkage = record(aggregate);
            globalIds = linkage.symbolIds();
        }
    }
    TASK_CHECK(!globalIds.empty());

    // Local ids (locals plus kept parameter) of each function must be
    // disjoint between the original and each version, and between versions.
    std::vector<std::set<long long>> localIds;
    for (auto* def : {originals[0], versions[0], versions[1]}) {
        std::set<long long> ids = record(def).symbolIds();
        for (long long id : globalIds)
            ids.erase(id);
        TASK_CHECK(!ids.empty());
        localIds.push_back(ids);
    }
    for (size_t a = 0; a < localIds.size(); ++a)
        for (size_t b = a + 1; b < localIds.size(); ++b)
            for (long long id : localIds[a])
                TASK_CHECK(localIds[b].count(id) == 0);

    // The uniform block reference inside each version still names the
    // original global symbol.
    for (auto* v : versions) {
        std::set<long long> ids = record(v).symbolIds();
        bool sharesGlobal = false;
        for (long long id : ids)
            if (globalIds.count(id))
                sharesGlobal = true;
        TASK_CHECK(sharesGlobal);
    }

    // No mutable AST node is shared between the original and a version.
    std::set<const void*> originalNodes = record(originals[0]).nodeSet();
    for (auto* v : versions) {
        std::set<const void*> versionNodes = record(v).nodeSet();
        for (const void* node : versionNodes)
            TASK_CHECK(originalNodes.count(node) == 0);
    }

    TASK_CHECK(spvFunctionsConsistent(ir, 4, 3));
    return 0;
}

int ownership_globals_identity()
{
    const char* source = R"glsl(#version 450
layout(location=0) flat in int mode;
layout(location=1) in float value;
layout(location=0) out vec4 color;
layout(set = 0, binding = 0) uniform UBO { float factor; } ubo;
float shade(const in int path, float x) {
    float t = x * ubo.factor + value;
    return t + float(path);
}
void main() {
    color = vec4(shade(4, value), shade(mode, value), 0.0, 1.0);
}
)glsl";
    TShaderUnit unit;
    TASK_CHECK(unit.compile(source));
    glslang::TIntermediate* ir = unit.ir();

    // Ids the stage input and the uniform block have at top level.
    long long valueId = -1, uboId = -1;
    for (auto* node : ir->getTreeRoot()->getAsAggregate()->getSequence()) {
        auto* aggregate = node->getAsAggregate();
        if (aggregate != nullptr && aggregate->getOp() == glslang::EOpLinkerObjects) {
            for (auto* sym : record(aggregate).symbols) {
                if (sym->getName() == "value")
                    valueId = sym->getId();
                if (sym->getName() == "ubo")
                    uboId = sym->getId();
            }
        }
    }
    TASK_CHECK(valueId >= 0 && uboId >= 0);

    glslang::SpecializationStats stats;
    glslang::SpecializeConstantArguments(*ir, 4, stats);
    TASK_CHECK(stats.createdVersions == 1);
    TASK_CHECK(stats.rewrittenCalls == 1);

    auto versions = versionsNamed(ir, "shade");
    TASK_CHECK(versions.size() == 1);
    TRecorder body = record(versions[0]);
    // Stage input and uniform block inside the version point at the
    // original global symbols.
    bool sawValue = false, sawUbo = false;
    for (auto* sym : body.symbols) {
        if (sym->getId() == valueId)
            sawValue = true;
        if (sym->getId() == uboId)
            sawUbo = true;
    }
    TASK_CHECK(sawValue && sawUbo);

    // The same ids are what main uses.
    TRecorder mainBody = record(originalsNamed(ir, "main")[0]);
    TASK_CHECK(mainBody.symbolIds().count(valueId) == 1);

    // The kept parameter is a fresh symbol, not the original's.
    auto originals = originalsNamed(ir, "shade");
    long long originalX = paramsOf(originals[0])->getSequence()[1]->getAsSymbolNode()->getId();
    long long versionX = paramsOf(versions[0])->getSequence()[0]->getAsSymbolNode()->getId();
    TASK_CHECK(originalX != versionX);
    TASK_CHECK(paramsOf(versions[0])->getSequence()[0]->getAsSymbolNode()->getName() == "x");

    TASK_CHECK(spvFunctionsConsistent(ir, 3, 2));
    return 0;
}

// ---------------------------------------------------------------- transitive

int transitive_nested_substitution()
{
    const char* source = R"glsl(#version 450
layout(location=1) in float value;
layout(location=0) out vec4 color;
float leaf(const in int k, float x) { return k == 3 ? x * 2.0 : x * 3.0; }
float mid(const in int route, float y) { return leaf(route, y) + leaf(1, y); }
void main() {
    color = vec4(mid(6, value), mid(2, value + 1.0), 0.0, 1.0);
}
)glsl";
    TShaderUnit unit;
    TASK_CHECK(unit.compile(source));
    glslang::TIntermediate* ir = unit.ir();

    glslang::SpecializationStats stats;
    glslang::SpecializeConstantArguments(*ir, 16, stats);
    // mid gets 2 versions; inside them, leaf(route, y) turns into a constant
    // call and leaf(1, y) is shared: 3 leaf versions in the same round.
    TASK_CHECK(stats.createdVersions == 5);
    TASK_CHECK(stats.rewrittenCalls == 6);
    TASK_CHECK(functionDefs(ir).size() == 8);

    auto midVersions = versionsNamed(ir, "mid");
    TASK_CHECK(midVersions.size() == 2);
    auto leafVersions = versionsNamed(ir, "leaf");
    TASK_CHECK(leafVersions.size() == 3);

    std::set<std::string> leafNames;
    for (auto* v : leafVersions)
        leafNames.insert(v->getName().c_str());
    std::string leafBase = originalsNamed(ir, "leaf")[0]->getName().c_str();
    TASK_CHECK(leafNames.count(leafBase + "#cas#0i6") == 1);
    TASK_CHECK(leafNames.count(leafBase + "#cas#0i1") == 1);
    TASK_CHECK(leafNames.count(leafBase + "#cas#0i2") == 1);

    // Each mid version calls only specialized leaf versions now: the one
    // for its own substituted 'route' value, plus the shared leaf(1, ...).
    for (auto* mv : midVersions) {
        auto calls = record(mv).userCalls();
        TASK_CHECK(calls.size() == 2);
        std::set<std::string> targets;
        for (auto* call : calls) {
            TASK_CHECK(call->getName().find(kMarker) != std::string::npos);
            TASK_CHECK(call->getSequence().size() == 1);
            targets.insert(call->getName().c_str());
        }
        const bool forSix = mv->getName().find("0i6") != std::string::npos;
        TASK_CHECK(targets.count(leafBase + (forSix ? "#cas#0i6" : "#cas#0i2")) == 1);
        TASK_CHECK(targets.count(leafBase + "#cas#0i1") == 1);
    }

    // The bound values landed inside the leaf versions.
    for (auto* lv : leafVersions) {
        const int expect = lv->getName().find("0i6") != std::string::npos ? 6
                         : lv->getName().find("0i2") != std::string::npos ? 2 : 1;
        TASK_CHECK(record(lv).hasIntConstant(expect));
    }

    // The original mid is kept (with its two original leaf calls) for any
    // dynamic caller, so SPIR-V sees 8 functions and 8 calls in total.
    TASK_CHECK(spvFunctionsConsistent(ir, 8, 8));
    return 0;
}

int transitive_reachable_only()
{
    const char* source = R"glsl(#version 450
layout(location=0) flat in int mode;
layout(location=1) in float value;
layout(location=0) out vec4 color;
float leaf(const in int k, float x) { return k == 3 ? x * 2.0 : x * 3.0; }
float mid(int route, float y) { return leaf(1, y) + float(route); }
float unused(const in int k, float x) { return leaf(2, x) + float(k); }
void main() {
    color = vec4(mid(mode, value), 0.0, 0.0, 1.0);
}
)glsl";
    TShaderUnit unit;
    TASK_CHECK(unit.compile(source, true /* keep uncalled functions */));
    glslang::TIntermediate* ir = unit.ir();
    TASK_CHECK(functionDefs(ir).size() == 4);

    glslang::SpecializationStats stats;
    glslang::SpecializeConstantArguments(*ir, 16, stats);
    // mid itself is called dynamically, but its body is reachable, so the
    // constant leaf(1, y) inside specializes.  'unused' is unreachable and
    // must not be expanded: no unused version, no leaf#cas#0i2.
    TASK_CHECK(stats.createdVersions == 1);
    TASK_CHECK(stats.rewrittenCalls == 1);
    TASK_CHECK(functionDefs(ir).size() == 5);
    TASK_CHECK(versionsNamed(ir, "leaf").size() == 1);
    TASK_CHECK(versionsNamed(ir, "unused").empty());
    TASK_CHECK(versionsNamed(ir, "mid").empty());

    // The constant call inside 'unused' is untouched.
    auto unusedCalls = record(originalsNamed(ir, "unused")[0]).userCalls();
    TASK_CHECK(unusedCalls.size() == 1);
    TASK_CHECK(unusedCalls[0]->getSequence().size() == 2);
    TASK_CHECK(unusedCalls[0]->getSequence()[0]->getAsConstantUnion() != nullptr);

    TASK_CHECK(spvFunctionsConsistent(ir, 5, 3));
    return 0;
}

// ---------------------------------------------------------------- budget

int budget_limit_and_resume()
{
    const char* source = R"glsl(#version 450
layout(location=1) in float value;
layout(location=0) out vec4 color;
float f(const in int k, float x) { return k == 1 ? x : -x; }
void main() {
    color = vec4(f(1, value), f(2, value + 1.0), f(3, value + 2.0), 1.0);
}
)glsl";
    TShaderUnit unit;
    TASK_CHECK(unit.compile(source));
    glslang::TIntermediate* ir = unit.ir();

    // A total budget of one version: only the first call specializes.
    glslang::SpecializationStats stats;
    glslang::SpecializeConstantArguments(*ir, 1, stats);
    TASK_CHECK(stats.createdVersions == 1);
    TASK_CHECK(stats.rewrittenCalls == 1);
    TASK_CHECK(functionDefs(ir).size() == 3);
    auto versions = versionsNamed(ir, "f");
    TASK_CHECK(versions.size() == 1);
    TASK_CHECK(versions[0]->getName().find("#cas#0i1") != std::string::npos);
    {
        auto calls = record(originalsNamed(ir, "main")[0]).userCalls();
        TASK_CHECK(calls.size() == 3);
        unsigned oneArg = 0, twoArg = 0;
        for (auto* call : calls) {
            if (call->getSequence().size() == 1)
                ++oneArg;
            else {
                ++twoArg;
                TASK_CHECK(call->getName().find(kMarker) == std::string::npos);
            }
        }
        TASK_CHECK(oneArg == 1 && twoArg == 2);
    }

    // Same budget again: no new copies, nothing changes.
    glslang::SpecializeConstantArguments(*ir, 1, stats);
    TASK_CHECK(stats.createdVersions == 0);
    TASK_CHECK(stats.rewrittenCalls == 0);
    TASK_CHECK(functionDefs(ir).size() == 3);

    // Raising the budget resumes where the last call stopped.
    glslang::SpecializeConstantArguments(*ir, 3, stats);
    TASK_CHECK(stats.createdVersions == 2);
    TASK_CHECK(stats.rewrittenCalls == 2);
    TASK_CHECK(functionDefs(ir).size() == 5);
    TASK_CHECK(versionsNamed(ir, "f").size() == 3);
    {
        auto calls = record(originalsNamed(ir, "main")[0]).userCalls();
        TASK_CHECK(calls.size() == 3);
        for (auto* call : calls) {
            TASK_CHECK(call->getSequence().size() == 1);
            TASK_CHECK(call->getName().find(kMarker) != std::string::npos);
        }
    }

    TASK_CHECK(spvFunctionsConsistent(ir, 5, 3));
    return 0;
}

int budget_zero_is_noop()
{
    const char* source = R"glsl(#version 450
layout(location=1) in float value;
layout(location=0) out vec4 color;
float f(const in int k, float x) { return k == 1 ? x : -x; }
void main() {
    color = vec4(f(1, value), f(2, value + 1.0), 0.0, 1.0);
}
)glsl";
    TShaderUnit unit;
    TASK_CHECK(unit.compile(source));
    glslang::TIntermediate* ir = unit.ir();

    glslang::SpecializationStats stats;
    glslang::SpecializeConstantArguments(*ir, 0, stats);
    TASK_CHECK(stats.createdVersions == 0);
    TASK_CHECK(stats.rewrittenCalls == 0);
    TASK_CHECK(functionDefs(ir).size() == 2);
    TASK_CHECK(versionsNamed(ir, "f").empty());
    auto calls = record(originalsNamed(ir, "main")[0]).userCalls();
    TASK_CHECK(calls.size() == 2);
    for (auto* call : calls) {
        TASK_CHECK(call->getSequence().size() == 2);
        TASK_CHECK(call->getQualifierList().size() == 2);
    }

    TASK_CHECK(spvFunctionsConsistent(ir, 2, 2));
    return 0;
}

int budget_spent_still_reuses()
{
    const char* source = R"glsl(#version 450
layout(location=1) in float value;
layout(location=0) out vec4 color;
float f(const in int k, float x) { return k == 1 ? x : -x; }
void main() {
    color = vec4(f(1, value), f(1, value + 1.0), f(2, value + 2.0), 1.0);
}
)glsl";
    TShaderUnit unit;
    TASK_CHECK(unit.compile(source));
    glslang::TIntermediate* ir = unit.ir();

    glslang::SpecializationStats stats;
    glslang::SpecializeConstantArguments(*ir, 1, stats);
    // Both f(1, ...) calls reuse the single version the budget allows;
    // f(2, ...) stays on the original.
    TASK_CHECK(stats.createdVersions == 1);
    TASK_CHECK(stats.rewrittenCalls == 2);
    TASK_CHECK(functionDefs(ir).size() == 3);
    auto versions = versionsNamed(ir, "f");
    TASK_CHECK(versions.size() == 1);
    TASK_CHECK(versions[0]->getName().find("#cas#0i1") != std::string::npos);

    auto calls = record(originalsNamed(ir, "main")[0]).userCalls();
    TASK_CHECK(calls.size() == 3);
    unsigned toVersion = 0, toOriginal = 0;
    for (auto* call : calls) {
        if (callTargets(call, versions[0])) {
            ++toVersion;
            TASK_CHECK(call->getSequence().size() == 1);
        } else {
            ++toOriginal;
            TASK_CHECK(call->getSequence().size() == 2);
            TASK_CHECK(call->getSequence()[0]->getAsConstantUnion() != nullptr);
            TASK_CHECK(call->getSequence()[0]->getAsConstantUnion()->getConstArray()[0].getIConst() == 2);
        }
    }
    TASK_CHECK(toVersion == 2 && toOriginal == 1);

    TASK_CHECK(spvFunctionsConsistent(ir, 3, 3));
    return 0;
}

int budget_interleaved_intermediates()
{
    const char* source = R"glsl(#version 450
layout(location=1) in float value;
layout(location=0) out vec4 color;
float f(const in int k, float x) { return k == 1 ? x : -x; }
void main() {
    color = vec4(f(1, value), f(2, value + 1.0), 0.0, 1.0);
}
)glsl";
    TShaderUnit unitA, unitB;
    TASK_CHECK(unitA.compile(source));
    TASK_CHECK(unitB.compile(source));
    glslang::TIntermediate* irA = unitA.ir();
    glslang::TIntermediate* irB = unitB.ir();

    // Alternate between the two intermediates; each must keep its own
    // versions and symbol ids.
    glslang::SpecializationStats statsA, statsB;
    glslang::SpecializeConstantArguments(*irA, 1, statsA);
    TASK_CHECK(statsA.createdVersions == 1);
    glslang::SpecializeConstantArguments(*irB, 1, statsB);
    TASK_CHECK(statsB.createdVersions == 1);
    TASK_CHECK(functionDefs(irA).size() == 3);
    TASK_CHECK(functionDefs(irB).size() == 3);

    glslang::SpecializeConstantArguments(*irA, 2, statsA);
    TASK_CHECK(statsA.createdVersions == 1);
    TASK_CHECK(statsA.rewrittenCalls == 1);
    TASK_CHECK(functionDefs(irA).size() == 4);
    // irB is untouched by the call on irA.
    TASK_CHECK(functionDefs(irB).size() == 3);

    glslang::SpecializeConstantArguments(*irB, 2, statsB);
    TASK_CHECK(statsB.createdVersions == 1);
    TASK_CHECK(statsB.rewrittenCalls == 1);
    TASK_CHECK(functionDefs(irB).size() == 4);

    // Same source, same budget: identical version names in both, but the
    // symbol ids behind them are allocated independently per intermediate.
    auto versionsA = versionsNamed(irA, "f");
    auto versionsB = versionsNamed(irB, "f");
    TASK_CHECK(versionsA.size() == 2 && versionsB.size() == 2);
    std::set<std::string> namesA, namesB;
    for (auto* v : versionsA)
        namesA.insert(v->getName().c_str());
    for (auto* v : versionsB)
        namesB.insert(v->getName().c_str());
    TASK_CHECK(namesA == namesB);

    // Symbol ids stay unique within each intermediate.
    for (auto* ir : {irA, irB}) {
        std::set<long long> seen;
        for (auto* def : functionDefs(ir))
            for (long long id : record(def).symbolIds())
                TASK_CHECK(seen.insert(id).second);
    }

    TASK_CHECK(spvFunctionsConsistent(irA, 4, 2));
    TASK_CHECK(spvFunctionsConsistent(irB, 4, 2));
    return 0;
}

} // end anonymous namespace

const struct task_case task_cases[] = {
    {"binding", "basic_int", binding_basic_int},
    {"binding", "argument_kinds", binding_argument_kinds},
    {"binding", "out_inout_kept", binding_out_inout_kept},
    {"ownership", "locals_and_params", ownership_locals_and_params},
    {"ownership", "globals_identity", ownership_globals_identity},
    {"transitive", "nested_substitution", transitive_nested_substitution},
    {"transitive", "reachable_only", transitive_reachable_only},
    {"budget", "limit_and_resume", budget_limit_and_resume},
    {"budget", "zero_is_noop", budget_zero_is_noop},
    {"budget", "spent_still_reuses", budget_spent_still_reuses},
    {"budget", "interleaved_intermediates", budget_interleaved_intermediates},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
