//
// Regression tests for glslang/MachineIndependent/constantArgumentSpecialization.
//
// Registered through task_support/check.h (task_cases / task_case_count) in
// the four groups binding, ownership, transitive, and budget. Every case
// builds real shader IR through parse/link, runs the specialization pass on
// the linked intermediate, and inspects functions, parameters, calls, and
// symbols directly, including feeding the result to GlslangToSpv and
// checking the generated module for consistent function definitions,
// parameters, and call operands.
//

#include "task_support/check.h"

#include "glslang/Public/ShaderLang.h"
#include "glslang/Public/ResourceLimits.h"
#include "glslang/MachineIndependent/localintermediate.h"
#include "glslang/MachineIndependent/constantArgumentSpecialization.h"
#include "SPIRV/GlslangToSpv.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

bool ensureInit()
{
    static const bool initialized = glslang::InitializeProcess();
    return initialized;
}

struct Compiled {
    std::unique_ptr<glslang::TShader> shader;
    std::unique_ptr<glslang::TProgram> program;
    glslang::TIntermediate* ir = nullptr;
};

bool compile(const char* source, Compiled& out)
{
    out.shader.reset(new glslang::TShader(EShLangFragment));
    out.shader->setStrings(&source, 1);
    out.shader->setEnvInput(glslang::EShSourceGlsl, EShLangFragment, glslang::EShClientVulkan, 450);
    out.shader->setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    out.shader->setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    EShMessages messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    if (! out.shader->parse(GetDefaultResources(), 450, false, messages))
        return false;
    out.program.reset(new glslang::TProgram);
    out.program->addShader(out.shader.get());
    if (! out.program->link(messages))
        return false;
    out.ir = out.program->getIntermediate(EShLangFragment);
    return out.ir != nullptr && out.ir->getTreeRoot() != nullptr;
}

struct FunctionView {
    glslang::TIntermAggregate* def = nullptr;
    glslang::TIntermAggregate* params = nullptr;
    TIntermNode* body = nullptr;
};

std::map<std::string, FunctionView> collectFunctions(glslang::TIntermediate& ir)
{
    std::map<std::string, FunctionView> result;
    glslang::TIntermAggregate* root = ir.getTreeRoot()->getAsAggregate();
    for (TIntermNode* child : root->getSequence()) {
        glslang::TIntermAggregate* aggregate = child ? child->getAsAggregate() : nullptr;
        if (aggregate != nullptr && aggregate->getOp() == glslang::EOpFunction) {
            FunctionView view;
            view.def = aggregate;
            view.params = aggregate->getSequence()[0]->getAsAggregate();
            view.body = aggregate->getSequence()[1];
            result[aggregate->getName().c_str()] = view;
        }
    }
    return result;
}

// Generic subtree collector used by the checks below.
struct Collector : glslang::TIntermTraverser {
    std::vector<glslang::TIntermAggregate*> calls;
    std::vector<glslang::TIntermSymbol*> symbols;
    std::vector<glslang::TIntermConstantUnion*> constants;

    Collector() : TIntermTraverser(true, false, false, false, true) { }

    bool visitAggregate(glslang::TVisit, glslang::TIntermAggregate* node) override
    {
        if (node->getOp() == glslang::EOpFunctionCall && node->isUserDefined())
            calls.push_back(node);
        return true;
    }
    void visitSymbol(glslang::TIntermSymbol* node) override { symbols.push_back(node); }
    void visitConstantUnion(glslang::TIntermConstantUnion* node) override { constants.push_back(node); }
};

Collector collect(TIntermNode* subtree)
{
    Collector collector;
    if (subtree != nullptr)
        subtree->traverse(&collector);
    return collector;
}

// Collect every AST node pointer in a subtree, including parameter lists and
// declaration symbols, to check that versions share no mutable nodes.
void collectNodes(TIntermNode* node, std::set<TIntermNode*>& out)
{
    if (node == nullptr || ! out.insert(node).second)
        return;
    if (glslang::TIntermAggregate* aggregate = node->getAsAggregate()) {
        for (TIntermNode* child : aggregate->getSequence())
            collectNodes(child, out);
    } else if (glslang::TIntermBinary* binary = node->getAsBinaryNode()) {
        collectNodes(binary->getLeft(), out);
        collectNodes(binary->getRight(), out);
    } else if (glslang::TIntermUnary* unary = node->getAsUnaryNode()) {
        collectNodes(unary->getOperand(), out);
    } else if (glslang::TIntermSelection* selection = node->getAsSelectionNode()) {
        collectNodes(selection->getCondition(), out);
        collectNodes(selection->getTrueBlock(), out);
        collectNodes(selection->getFalseBlock(), out);
    } else if (glslang::TIntermLoop* loop = node->getAsLoopNode()) {
        collectNodes(loop->getTest(), out);
        collectNodes(loop->getBody(), out);
        collectNodes(loop->getTerminal(), out);
    } else if (glslang::TIntermBranch* branch = node->getAsBranchNode()) {
        collectNodes(branch->getExpression(), out);
    } else if (glslang::TIntermSwitch* switchNode = node->getAsSwitchNode()) {
        collectNodes(switchNode->getCondition(), out);
        collectNodes(switchNode->getBody(), out);
    } else if (glslang::TIntermVariableDecl* decl = node->getAsVariableDecl()) {
        collectNodes(decl->getDeclSymbol(), out);
        collectNodes(decl->getInitNode(), out);
    }
}

bool disjoint(const std::set<TIntermNode*>& a, const std::set<TIntermNode*>& b)
{
    for (TIntermNode* node : a)
        if (b.count(node) != 0)
            return false;
    return true;
}

bool hasIntConstant(const Collector& c, int value)
{
    for (glslang::TIntermConstantUnion* constant : c.constants)
        if (constant->getType().isScalar() && constant->getBasicType() == glslang::EbtInt &&
            constant->getConstArray().size() == 1 && constant->getConstArray()[0].getIConst() == value)
            return true;
    return false;
}

bool hasUintConstant(const Collector& c, unsigned value)
{
    for (glslang::TIntermConstantUnion* constant : c.constants)
        if (constant->getType().isScalar() && constant->getBasicType() == glslang::EbtUint &&
            constant->getConstArray().size() == 1 && constant->getConstArray()[0].getUConst() == value)
            return true;
    return false;
}

bool hasBoolConstant(const Collector& c, bool value)
{
    for (glslang::TIntermConstantUnion* constant : c.constants)
        if (constant->getType().isScalar() && constant->getBasicType() == glslang::EbtBool &&
            constant->getConstArray().size() == 1 && constant->getConstArray()[0].getBConst() == value)
            return true;
    return false;
}

long long symbolIdByName(const Collector& c, const char* name, bool& found)
{
    for (glslang::TIntermSymbol* symbol : c.symbols)
        if (symbol->getName() == name) {
            found = true;
            return symbol->getId();
        }
    found = false;
    return 0;
}

struct SpvSummary {
    unsigned functions = 0;
    std::map<unsigned, unsigned> paramCount;              // function id -> parameter count
    std::vector<std::pair<unsigned, unsigned>> calls;     // (callee id, argument count)
    std::map<unsigned, unsigned> callArgsByCalleeCount;   // not used, kept simple
};

// Run the original GlslangToSpv on the (possibly specialized) intermediate
// and verify the module's function definitions, parameters, and call
// operands agree with each other.
bool checkSpv(glslang::TIntermediate& ir, SpvSummary& summary)
{
    std::vector<unsigned> words;
    glslang::SpvOptions options;
    options.disableOptimizer = true;
    glslang::GlslangToSpv(ir, words, &options);
    if (words.size() <= 5 || words[0] != 0x07230203)
        return false;
    unsigned current = 0;
    for (size_t i = 5; i < words.size();) {
        unsigned count = words[i] >> 16;
        unsigned op = words[i] & 0xFFFF;
        if (count == 0 || i + count > words.size())
            return false;
        switch (op) {
        case 54: // OpFunction
            if (count != 5)
                return false;
            current = words[i + 2];
            summary.paramCount[current] = 0;
            ++summary.functions;
            break;
        case 55: // OpFunctionParameter
            if (current == 0 || count != 3)
                return false;
            ++summary.paramCount[current];
            break;
        case 57: // OpFunctionCall
            if (count < 4)
                return false;
            summary.calls.push_back(std::make_pair(words[i + 3], count - 4));
            break;
        case 56: // OpFunctionEnd
            current = 0;
            break;
        default:
            break;
        }
        i += count;
    }
    // Every call must target a defined function with a matching arity.
    for (const auto& call : summary.calls) {
        auto it = summary.paramCount.find(call.first);
        if (it == summary.paramCount.end() || it->second != call.second)
            return false;
    }
    return true;
}

const char kMarker[] = "@spec";

std::vector<std::string> specNames(const std::map<std::string, FunctionView>& functions)
{
    std::vector<std::string> names;
    for (const auto& entry : functions)
        if (entry.first.find(kMarker) != std::string::npos)
            names.push_back(entry.first);
    return names;
}

// ---------------------------------------------------------------------------
// binding group
// ---------------------------------------------------------------------------

const char* kMaterialShader = R"glsl(#version 450
layout(location=0) flat in int mode;
layout(location=1) in float value;
layout(location=0) out vec4 color;
float material(const in int kind, float x) {
    float y = x * 2.0;
    if (kind == 1) return y + 3.0;
    return y - 5.0;
}
void main() {
    color = vec4(material(1, value), material(2, value + 1.0), material(mode, value - 1.0), 1.0);
}
)glsl";

int binding_basic()
{
    if (! ensureInit())
        return 1;
    Compiled c;
    TASK_CHECK(compile(kMaterialShader, c));
    glslang::TIntermediate& ir = *c.ir;

    std::map<std::string, FunctionView> before = collectFunctions(ir);
    TASK_CHECK(before.size() == 2);
    std::string originalName;
    for (const auto& entry : before)
        if (entry.first != "main(")
            originalName = entry.first;
    TASK_CHECK(originalName.find("material(") == 0);
    long long originalParamId = before[originalName].params->getSequence()[0]->getAsSymbolNode()->getId();

    glslang::SpecializationStats stats;
    glslang::SpecializeConstantArguments(ir, 8, stats);
    TASK_CHECK(stats.createdVersions == 2);
    TASK_CHECK(stats.rewrittenCalls == 2);

    std::map<std::string, FunctionView> after = collectFunctions(ir);
    TASK_CHECK(after.size() == 4);

    // The original function is kept, unchanged in signature.
    TASK_CHECK(after.count(originalName) == 1);
    TASK_CHECK(after[originalName].params->getSequence().size() == 2);
    Collector originalBody = collect(after[originalName].body);
    bool found = false;
    TASK_CHECK(symbolIdByName(originalBody, "kind", found) == originalParamId && found);

    // Two versions, named stably from the original name plus the bound key.
    std::string name1 = originalName + "@spec.0i1";
    std::string name2 = originalName + "@spec.0i2";
    TASK_CHECK(after.count(name1) == 1);
    TASK_CHECK(after.count(name2) == 1);
    TASK_CHECK(specNames(after).size() == 2);

    // Versions keep the original return type and drop the bound parameter.
    for (const std::string& name : {name1, name2}) {
        const FunctionView& view = after[name];
        TASK_CHECK(view.def->getType().getBasicType() == glslang::EbtFloat);
        TASK_CHECK(view.params->getSequence().size() == 1);
        glslang::TIntermSymbol* param = view.params->getSequence()[0]->getAsSymbolNode();
        TASK_CHECK(param != nullptr);
        TASK_CHECK(param->getType().getBasicType() == glslang::EbtFloat);
        TASK_CHECK(param->getName() == "x");
        TASK_CHECK(param->getId() != originalParamId);
    }

    // The bound parameter reads became constants of the right type and value.
    Collector body1 = collect(after[name1].body);
    Collector body2 = collect(after[name2].body);
    TASK_CHECK(hasIntConstant(body1, 1));
    TASK_CHECK(hasIntConstant(body2, 2));
    TASK_CHECK(! hasIntConstant(body1, 2));
    for (glslang::TIntermSymbol* symbol : body1.symbols)
        TASK_CHECK(symbol->getId() != originalParamId);
    for (glslang::TIntermSymbol* symbol : body2.symbols)
        TASK_CHECK(symbol->getId() != originalParamId);

    // Calls in main: two rewritten to the versions with one argument left,
    // the dynamic one still calling the original with two arguments.
    Collector mainBody = collect(after["main("].body);
    TASK_CHECK(mainBody.calls.size() == 3);
    unsigned rewritten = 0, dynamic = 0;
    for (glslang::TIntermAggregate* call : mainBody.calls) {
        if (call->getName() == name1.c_str() || call->getName() == name2.c_str()) {
            ++rewritten;
            TASK_CHECK(call->getSequence().size() == 1);
            TASK_CHECK(call->getQualifierList().size() == 1);
            TASK_CHECK(call->getSequence()[0]->getAsConstantUnion() == nullptr);
        } else {
            ++dynamic;
            TASK_CHECK(call->getName() == originalName.c_str());
            TASK_CHECK(call->getSequence().size() == 2);
            TASK_CHECK(call->getQualifierList().size() == 2);
            TASK_CHECK(call->getSequence()[0]->getAsSymbolNode() != nullptr);
        }
    }
    TASK_CHECK(rewritten == 2 && dynamic == 1);

    // The specialized IR must still generate consistent SPIR-V.
    SpvSummary spv;
    TASK_CHECK(checkSpv(ir, spv));
    TASK_CHECK(spv.functions == 4);
    TASK_CHECK(spv.calls.size() == 3);
    unsigned oneParamFunctions = 0, twoParamFunctions = 0;
    for (const auto& entry : spv.paramCount) {
        if (entry.second == 1)
            ++oneParamFunctions;
        else if (entry.second == 2)
            ++twoParamFunctions;
    }
    TASK_CHECK(oneParamFunctions == 2 && twoParamFunctions == 1);
    return 0;
}

int binding_types()
{
    if (! ensureInit())
        return 1;
    const char* source = R"glsl(#version 450
layout(location=0) out vec4 color;
float helper(const in uint u, const in bool b, const in int n, float x) {
    float r = x + float(u);
    if (b) r += 1.0;
    return r + float(n);
}
void main() {
    float a = helper(7u, true, -3, 1.0);
    float d = helper(7u, false, -3, 2.0);
    color = vec4(a, d, 0.0, 1.0);
}
)glsl";
    Compiled c;
    TASK_CHECK(compile(source, c));
    glslang::TIntermediate& ir = *c.ir;

    std::string originalName;
    for (const auto& entry : collectFunctions(ir))
        if (entry.first != "main(")
            originalName = entry.first;
    TASK_CHECK(originalName.find("helper(") == 0);

    glslang::SpecializationStats stats;
    glslang::SpecializeConstantArguments(ir, 8, stats);
    TASK_CHECK(stats.createdVersions == 2);
    TASK_CHECK(stats.rewrittenCalls == 2);

    std::map<std::string, FunctionView> after = collectFunctions(ir);
    TASK_CHECK(after.size() == 4);

    // All three scalar parameters bind together into one key per call.
    std::string nameT = originalName + "@spec.0u7.1b1.2i-3";
    std::string nameF = originalName + "@spec.0u7.1b0.2i-3";
    TASK_CHECK(after.count(nameT) == 1);
    TASK_CHECK(after.count(nameF) == 1);

    // Only the float parameter survives in each version.
    for (const std::string& name : {nameT, nameF}) {
        TASK_CHECK(after[name].params->getSequence().size() == 1);
        glslang::TIntermSymbol* param = after[name].params->getSequence()[0]->getAsSymbolNode();
        TASK_CHECK(param != nullptr && param->getName() == "x");
        TASK_CHECK(param->getType().getBasicType() == glslang::EbtFloat);
    }

    // The substituted constants carry the bound types and values.
    Collector bodyT = collect(after[nameT].body);
    TASK_CHECK(hasUintConstant(bodyT, 7u));
    TASK_CHECK(hasBoolConstant(bodyT, true));
    TASK_CHECK(hasIntConstant(bodyT, -3));
    Collector bodyF = collect(after[nameF].body);
    TASK_CHECK(hasUintConstant(bodyF, 7u));
    TASK_CHECK(hasBoolConstant(bodyF, false));
    TASK_CHECK(! hasBoolConstant(bodyF, true));
    TASK_CHECK(hasIntConstant(bodyF, -3));

    SpvSummary spv;
    TASK_CHECK(checkSpv(ir, spv));
    TASK_CHECK(spv.functions == 4);
    TASK_CHECK(spv.calls.size() == 2);
    return 0;
}

int binding_ineligible()
{
    if (! ensureInit())
        return 1;
    const char* source = R"glsl(#version 450
layout(location=0) out vec4 color;
layout(constant_id = 10) const int specMode = 2;
const int plainConst = 4;
float helper(const in int a, in int b, const in ivec2 v, out float o) {
    o = float(a + b + v.x);
    return o * 2.0;
}
void main() {
    float o1, o2, o3;
    helper(specMode, 3, ivec2(1, 2), o1);
    helper(1, 3, ivec2(1, 2), o2);
    helper(plainConst, 3, ivec2(1, 2), o3);
    color = vec4(o1, o2, o3, 1.0);
}
)glsl";
    Compiled c;
    TASK_CHECK(compile(source, c));
    glslang::TIntermediate& ir = *c.ir;

    std::string originalName;
    for (const auto& entry : collectFunctions(ir))
        if (entry.first != "main(")
            originalName = entry.first;
    TASK_CHECK(originalName.find("helper(") == 0);

    glslang::SpecializationStats stats;
    glslang::SpecializeConstantArguments(ir, 8, stats);

    // Only the two calls with a plain constant for 'a' bind; the spec
    // constant, the writable 'in' parameter, the vector parameter, and the
    // 'out' parameter never do.
    TASK_CHECK(stats.createdVersions == 2);
    TASK_CHECK(stats.rewrittenCalls == 2);

    std::map<std::string, FunctionView> after = collectFunctions(ir);
    TASK_CHECK(after.size() == 4);
    std::string name1 = originalName + "@spec.0i1";
    std::string name4 = originalName + "@spec.0i4";
    TASK_CHECK(after.count(name1) == 1);
    TASK_CHECK(after.count(name4) == 1);

    // A front-end const variable arrives as a plain constant and binds.
    Collector body4 = collect(after[name4].body);
    TASK_CHECK(hasIntConstant(body4, 4));

    // The remaining parameters keep their qualifiers: writable 'in',
    // const 'in' vector, and 'out'.
    for (const std::string& name : {name1, name4}) {
        const glslang::TIntermSequence& params = after[name].params->getSequence();
        TASK_CHECK(params.size() == 3);
        TASK_CHECK(params[0]->getAsSymbolNode()->getQualifier().storage == glslang::EvqIn);
        TASK_CHECK(params[1]->getAsSymbolNode()->getQualifier().storage == glslang::EvqConstReadOnly);
        TASK_CHECK(params[1]->getAsSymbolNode()->getType().isVector());
        TASK_CHECK(params[2]->getAsSymbolNode()->getQualifier().storage == glslang::EvqOut);
    }

    Collector mainBody = collect(after["main("].body);
    TASK_CHECK(mainBody.calls.size() == 3);
    unsigned kept = 0, rewritten = 0;
    for (glslang::TIntermAggregate* call : mainBody.calls) {
        if (call->getName() == originalName.c_str()) {
            ++kept;
            // The spec-constant call is untouched: four arguments, the
            // first one still a (spec constant) symbol, not a constant.
            TASK_CHECK(call->getSequence().size() == 4);
            TASK_CHECK(call->getQualifierList().size() == 4);
            TASK_CHECK(call->getSequence()[0]->getAsConstantUnion() == nullptr);
            glslang::TIntermSymbol* arg0 = call->getSequence()[0]->getAsSymbolNode();
            TASK_CHECK(arg0 != nullptr);
            TASK_CHECK(arg0->getQualifier().isSpecConstant());
        } else {
            ++rewritten;
            TASK_CHECK(call->getSequence().size() == 3);
            TASK_CHECK(call->getQualifierList().size() == 3);
            // The 'out' argument is still passed as an l-value symbol.
            TASK_CHECK(call->getSequence()[2]->getAsSymbolNode() != nullptr);
        }
    }
    TASK_CHECK(kept == 1 && rewritten == 2);

    SpvSummary spv;
    TASK_CHECK(checkSpv(ir, spv));
    TASK_CHECK(spv.functions == 4);
    TASK_CHECK(spv.calls.size() == 3);
    return 0;
}

int binding_reuse_same_key()
{
    if (! ensureInit())
        return 1;
    const char* source = R"glsl(#version 450
layout(location=1) in float value;
layout(location=0) out vec4 color;
float material(const in int kind, float x) {
    return kind == 3 ? x : -x;
}
void main() {
    color = vec4(material(3, value), material(3, value + 1.0), 0.0, 1.0);
}
)glsl";
    Compiled c;
    TASK_CHECK(compile(source, c));
    glslang::TIntermediate& ir = *c.ir;

    glslang::SpecializationStats stats;
    glslang::SpecializeConstantArguments(ir, 8, stats);
    // Identical keys share one version; both call sites are rewritten.
    TASK_CHECK(stats.createdVersions == 1);
    TASK_CHECK(stats.rewrittenCalls == 2);

    std::map<std::string, FunctionView> after = collectFunctions(ir);
    TASK_CHECK(after.size() == 3);
    std::vector<std::string> versions = specNames(after);
    TASK_CHECK(versions.size() == 1);

    Collector mainBody = collect(after["main("].body);
    TASK_CHECK(mainBody.calls.size() == 2);
    for (glslang::TIntermAggregate* call : mainBody.calls) {
        TASK_CHECK(call->getName().c_str() == versions[0]);
        TASK_CHECK(call->getSequence().size() == 1);
    }

    SpvSummary spv;
    TASK_CHECK(checkSpv(ir, spv));
    TASK_CHECK(spv.functions == 3);
    TASK_CHECK(spv.calls.size() == 2);
    return 0;
}

// ---------------------------------------------------------------------------
// ownership group
// ---------------------------------------------------------------------------

const char* kOwnershipShader = R"glsl(#version 450
layout(location=0) in float value;
layout(location=0) out vec4 color;
layout(set=0, binding=0) uniform sampler2D tex;
layout(set=0, binding=1) uniform Params { float scale; } params;
float globalVar = 0.5;
float shade(const in int mode, float x) {
    float local = x * params.scale + globalVar;
    local += texture(tex, vec2(0.5)).r;
    if (mode > 0) local += float(mode);
    return local;
}
void main() {
    color = vec4(shade(1, value), shade(2, value), 0.0, 1.0);
}
)glsl";

int ownership_symbol_isolation()
{
    if (! ensureInit())
        return 1;
    Compiled c;
    TASK_CHECK(compile(kOwnershipShader, c));
    glslang::TIntermediate& ir = *c.ir;

    std::string originalName;
    for (const auto& entry : collectFunctions(ir))
        if (entry.first != "main(")
            originalName = entry.first;

    glslang::SpecializationStats stats;
    glslang::SpecializeConstantArguments(ir, 8, stats);
    TASK_CHECK(stats.createdVersions == 2);

    std::map<std::string, FunctionView> after = collectFunctions(ir);
    std::string name1 = originalName + "@spec.0i1";
    std::string name2 = originalName + "@spec.0i2";
    TASK_CHECK(after.count(name1) == 1 && after.count(name2) == 1);

    Collector originalBody = collect(after[originalName].body);
    Collector body1 = collect(after[name1].body);
    Collector body2 = collect(after[name2].body);

    // Local variables get independent symbol identities in every version.
    bool found = false;
    long long originalLocal = symbolIdByName(originalBody, "local", found);
    TASK_CHECK(found);
    long long local1 = symbolIdByName(body1, "local", found);
    TASK_CHECK(found);
    long long local2 = symbolIdByName(body2, "local", found);
    TASK_CHECK(found);
    TASK_CHECK(local1 != originalLocal && local2 != originalLocal && local1 != local2);

    // Remaining parameters also get fresh identities per version.
    long long originalX = after[originalName].params->getSequence()[1]->getAsSymbolNode()->getId();
    long long x1 = after[name1].params->getSequence()[0]->getAsSymbolNode()->getId();
    long long x2 = after[name2].params->getSequence()[0]->getAsSymbolNode()->getId();
    TASK_CHECK(x1 != originalX && x2 != originalX && x1 != x2);

    // Global resources keep pointing at the original symbols.
    const char* globals[] = {"params", "tex", "globalVar"};
    for (const char* name : globals) {
        long long originalId = symbolIdByName(originalBody, name, found);
        TASK_CHECK(found);
        long long id1 = symbolIdByName(body1, name, found);
        TASK_CHECK(found && id1 == originalId);
        long long id2 = symbolIdByName(body2, name, found);
        TASK_CHECK(found && id2 == originalId);
    }

    SpvSummary spv;
    TASK_CHECK(checkSpv(ir, spv));
    TASK_CHECK(spv.functions == 4);
    return 0;
}

int ownership_node_disjoint()
{
    if (! ensureInit())
        return 1;
    Compiled c;
    TASK_CHECK(compile(kOwnershipShader, c));
    glslang::TIntermediate& ir = *c.ir;

    std::string originalName;
    for (const auto& entry : collectFunctions(ir))
        if (entry.first != "main(")
            originalName = entry.first;

    glslang::SpecializationStats stats;
    glslang::SpecializeConstantArguments(ir, 8, stats);
    TASK_CHECK(stats.createdVersions == 2);

    std::map<std::string, FunctionView> after = collectFunctions(ir);
    std::string name1 = originalName + "@spec.0i1";
    std::string name2 = originalName + "@spec.0i2";
    TASK_CHECK(after.count(name1) == 1 && after.count(name2) == 1);

    // No mutable AST node is shared between the versions and the original.
    std::set<TIntermNode*> originalNodes, nodes1, nodes2;
    collectNodes(after[originalName].def, originalNodes);
    collectNodes(after[name1].def, nodes1);
    collectNodes(after[name2].def, nodes2);
    TASK_CHECK(! originalNodes.empty() && ! nodes1.empty() && ! nodes2.empty());
    TASK_CHECK(disjoint(originalNodes, nodes1));
    TASK_CHECK(disjoint(originalNodes, nodes2));
    TASK_CHECK(disjoint(nodes1, nodes2));

    // The constants plugged into the two versions are distinct nodes.
    Collector body1 = collect(after[name1].body);
    Collector body2 = collect(after[name2].body);
    for (glslang::TIntermConstantUnion* constant : body1.constants)
        TASK_CHECK(nodes2.count(constant) == 0);

    // The original body still reads its own parameter, not a constant.
    bool found = false;
    long long originalParam = after[originalName].params->getSequence()[0]->getAsSymbolNode()->getId();
    Collector originalBody = collect(after[originalName].body);
    TASK_CHECK(symbolIdByName(originalBody, "mode", found) == originalParam && found);

    SpvSummary spv;
    TASK_CHECK(checkSpv(ir, spv));
    TASK_CHECK(spv.functions == 4);
    return 0;
}

int ownership_intermediate_isolation()
{
    if (! ensureInit())
        return 1;
    // Two independent intermediates, specialized in alternation, must not
    // share versions or symbol ids.
    Compiled a, b;
    TASK_CHECK(compile(kMaterialShader, a));
    TASK_CHECK(compile(kOwnershipShader, b));

    glslang::SpecializationStats statsA, statsB;
    glslang::SpecializeConstantArguments(*a.ir, 8, statsA);
    TASK_CHECK(statsA.createdVersions == 2);
    glslang::SpecializeConstantArguments(*b.ir, 8, statsB);
    TASK_CHECK(statsB.createdVersions == 2);

    std::map<std::string, FunctionView> functionsA = collectFunctions(*a.ir);
    std::map<std::string, FunctionView> functionsB = collectFunctions(*b.ir);
    TASK_CHECK(specNames(functionsA).size() == 2);
    TASK_CHECK(specNames(functionsB).size() == 2);

    // No function definition node is shared between the two intermediates.
    for (const auto& entryA : functionsA)
        for (const auto& entryB : functionsB)
            TASK_CHECK(entryA.second.def != entryB.second.def);

    // Re-running the first intermediate with the same budget adds nothing.
    glslang::SpecializeConstantArguments(*a.ir, 8, statsA);
    TASK_CHECK(statsA.createdVersions == 0);
    TASK_CHECK(statsA.rewrittenCalls == 0);
    TASK_CHECK(collectFunctions(*a.ir).size() == functionsA.size());

    SpvSummary spvA, spvB;
    TASK_CHECK(checkSpv(*a.ir, spvA));
    TASK_CHECK(checkSpv(*b.ir, spvB));
    TASK_CHECK(spvA.functions == 4 && spvB.functions == 4);
    return 0;
}

// ---------------------------------------------------------------------------
// transitive group
// ---------------------------------------------------------------------------

int transitive_nested()
{
    if (! ensureInit())
        return 1;
    const char* source = R"glsl(#version 450
layout(location=0) in float value;
layout(location=0) out vec4 color;
float inner(const in int k, float x) { return x + float(k); }
float outer(const in int m, float y) { return inner(m, y) * 2.0; }
void main() {
    color = vec4(outer(5, value), outer(6, value), 0.0, 1.0);
}
)glsl";
    Compiled c;
    TASK_CHECK(compile(source, c));
    glslang::TIntermediate& ir = *c.ir;

    std::string innerName, outerName;
    for (const auto& entry : collectFunctions(ir)) {
        if (entry.first.find("inner(") == 0)
            innerName = entry.first;
        else if (entry.first.find("outer(") == 0)
            outerName = entry.first;
    }
    TASK_CHECK(! innerName.empty() && ! outerName.empty());

    glslang::SpecializationStats stats;
    glslang::SpecializeConstantArguments(ir, 8, stats);
    // outer(5)/outer(6) each create a version, and substituting 'm' surfaces
    // a constant at the inner() call inside, specialized in the same round.
    TASK_CHECK(stats.createdVersions == 4);
    TASK_CHECK(stats.rewrittenCalls == 4);

    std::map<std::string, FunctionView> after = collectFunctions(ir);
    TASK_CHECK(after.size() == 7);
    std::string outer5 = outerName + "@spec.0i5";
    std::string inner5 = innerName + "@spec.0i5";
    std::string inner6 = innerName + "@spec.0i6";
    TASK_CHECK(after.count(outer5) == 1);
    TASK_CHECK(after.count(inner5) == 1);
    TASK_CHECK(after.count(inner6) == 1);

    // The call inside outer's version targets inner's matching version,
    // with the bound slot removed from both signature and call.
    Collector outer5Body = collect(after[outer5].body);
    TASK_CHECK(outer5Body.calls.size() == 1);
    TASK_CHECK(outer5Body.calls[0]->getName().c_str() == inner5);
    TASK_CHECK(outer5Body.calls[0]->getSequence().size() == 1);
    TASK_CHECK(after[inner5].params->getSequence().size() == 1);
    Collector inner5Body = collect(after[inner5].body);
    TASK_CHECK(hasIntConstant(inner5Body, 5));

    SpvSummary spv;
    TASK_CHECK(checkSpv(ir, spv));
    TASK_CHECK(spv.functions == 7);
    TASK_CHECK(spv.calls.size() == 4);
    return 0;
}

int transitive_shared_inner()
{
    if (! ensureInit())
        return 1;
    const char* source = R"glsl(#version 450
layout(location=0) in float value;
layout(location=0) out vec4 color;
float inner(const in int k, float x) { return x + float(k); }
float outer(const in int m, float y) { return inner(m, y) * 2.0; }
void main() {
    color = vec4(outer(5, value), inner(5, value), 0.0, 1.0);
}
)glsl";
    Compiled c;
    TASK_CHECK(compile(source, c));
    glslang::TIntermediate& ir = *c.ir;

    std::string innerName, outerName;
    for (const auto& entry : collectFunctions(ir)) {
        if (entry.first.find("inner(") == 0)
            innerName = entry.first;
        else if (entry.first.find("outer(") == 0)
            outerName = entry.first;
    }
    TASK_CHECK(! innerName.empty() && ! outerName.empty());

    glslang::SpecializationStats stats;
    glslang::SpecializeConstantArguments(ir, 8, stats);
    // inner@spec.0i5 is created while processing outer's version; the direct
    // call from main with the same key reuses it.
    TASK_CHECK(stats.createdVersions == 2);
    TASK_CHECK(stats.rewrittenCalls == 3);

    std::map<std::string, FunctionView> after = collectFunctions(ir);
    std::string inner5 = innerName + "@spec.0i5";
    TASK_CHECK(after.count(inner5) == 1);

    Collector mainBody = collect(after["main("].body);
    TASK_CHECK(mainBody.calls.size() == 2);
    bool sawOuterVersion = false, sawInnerVersion = false;
    for (glslang::TIntermAggregate* call : mainBody.calls) {
        if (call->getName().c_str() == inner5)
            sawInnerVersion = true;
        else if (call->getName().find(kMarker) != std::string::npos)
            sawOuterVersion = true;
    }
    TASK_CHECK(sawInnerVersion && sawOuterVersion);

    SpvSummary spv;
    TASK_CHECK(checkSpv(ir, spv));
    TASK_CHECK(spv.functions == 5);
    TASK_CHECK(spv.calls.size() == 3);
    return 0;
}

// ---------------------------------------------------------------------------
// budget group
// ---------------------------------------------------------------------------

int budget_zero()
{
    if (! ensureInit())
        return 1;
    Compiled c;
    TASK_CHECK(compile(kMaterialShader, c));
    glslang::TIntermediate& ir = *c.ir;

    glslang::SpecializationStats stats;
    stats.createdVersions = 99;
    stats.rewrittenCalls = 99;
    glslang::SpecializeConstantArguments(ir, 0, stats);
    // Statistics are reset even when the budget is zero.
    TASK_CHECK(stats.createdVersions == 0);
    TASK_CHECK(stats.rewrittenCalls == 0);

    // Zero budget leaves the IR untouched.
    std::map<std::string, FunctionView> after = collectFunctions(ir);
    TASK_CHECK(after.size() == 2);
    TASK_CHECK(specNames(after).empty());
    Collector mainBody = collect(after["main("].body);
    TASK_CHECK(mainBody.calls.size() == 3);
    for (glslang::TIntermAggregate* call : mainBody.calls) {
        TASK_CHECK(call->getName().find(kMarker) == std::string::npos);
        TASK_CHECK(call->getSequence().size() == 2);
    }
    return 0;
}

const char* kBudgetShader = R"glsl(#version 450
layout(location=1) in float value;
layout(location=0) out vec4 color;
float material(const in int kind, float x) {
    return kind == 1 ? x : -x;
}
void main() {
    color = vec4(material(1, value), material(1, value + 1.0), material(2, value - 1.0));
}
)glsl";

int budget_cap_and_reuse()
{
    if (! ensureInit())
        return 1;
    Compiled c;
    TASK_CHECK(compile(kBudgetShader, c));
    glslang::TIntermediate& ir = *c.ir;

    glslang::SpecializationStats stats;
    glslang::SpecializeConstantArguments(ir, 1, stats);
    // Budget of one: the first key creates the only version, the second
    // call with the same key reuses it, the third key is left alone.
    TASK_CHECK(stats.createdVersions == 1);
    TASK_CHECK(stats.rewrittenCalls == 2);

    std::map<std::string, FunctionView> after = collectFunctions(ir);
    TASK_CHECK(after.size() == 3);
    std::vector<std::string> versions = specNames(after);
    TASK_CHECK(versions.size() == 1);
    TASK_CHECK(versions[0].find(".0i1") != std::string::npos);

    Collector mainBody = collect(after["main("].body);
    TASK_CHECK(mainBody.calls.size() == 3);
    unsigned rewritten = 0, kept = 0;
    for (glslang::TIntermAggregate* call : mainBody.calls) {
        if (call->getName().find(kMarker) != std::string::npos) {
            ++rewritten;
            TASK_CHECK(call->getSequence().size() == 1);
        } else {
            ++kept;
            TASK_CHECK(call->getSequence().size() == 2);
        }
    }
    TASK_CHECK(rewritten == 2 && kept == 1);

    SpvSummary spv;
    TASK_CHECK(checkSpv(ir, spv));
    TASK_CHECK(spv.functions == 3);
    TASK_CHECK(spv.calls.size() == 3);
    return 0;
}

int budget_rerun_growth()
{
    if (! ensureInit())
        return 1;
    Compiled c;
    TASK_CHECK(compile(kBudgetShader, c));
    glslang::TIntermediate& ir = *c.ir;

    glslang::SpecializationStats stats;
    glslang::SpecializeConstantArguments(ir, 1, stats);
    TASK_CHECK(stats.createdVersions == 1 && stats.rewrittenCalls == 2);
    TASK_CHECK(collectFunctions(ir).size() == 3);

    // Same budget again: existing versions count against it, so nothing
    // changes, but nothing breaks either.
    glslang::SpecializeConstantArguments(ir, 1, stats);
    TASK_CHECK(stats.createdVersions == 0);
    TASK_CHECK(stats.rewrittenCalls == 0);
    TASK_CHECK(collectFunctions(ir).size() == 3);

    // Raising the budget picks up the call that was left over.
    glslang::SpecializeConstantArguments(ir, 3, stats);
    TASK_CHECK(stats.createdVersions == 1);
    TASK_CHECK(stats.rewrittenCalls == 1);
    std::map<std::string, FunctionView> after = collectFunctions(ir);
    TASK_CHECK(after.size() == 4);
    TASK_CHECK(specNames(after).size() == 2);

    Collector mainBody = collect(after["main("].body);
    for (glslang::TIntermAggregate* call : mainBody.calls)
        TASK_CHECK(call->getName().find(kMarker) != std::string::npos);

    // Repeating with the same budget creates no duplicates.
    glslang::SpecializeConstantArguments(ir, 3, stats);
    TASK_CHECK(stats.createdVersions == 0);
    TASK_CHECK(stats.rewrittenCalls == 0);
    TASK_CHECK(collectFunctions(ir).size() == 4);

    SpvSummary spv;
    TASK_CHECK(checkSpv(ir, spv));
    TASK_CHECK(spv.functions == 4);
    TASK_CHECK(spv.calls.size() == 3);
    return 0;
}

} // end anonymous namespace

const struct task_case task_cases[] = {
    {"binding", "basic", binding_basic},
    {"binding", "types", binding_types},
    {"binding", "ineligible", binding_ineligible},
    {"binding", "reuse_same_key", binding_reuse_same_key},
    {"ownership", "symbol_isolation", ownership_symbol_isolation},
    {"ownership", "node_disjoint", ownership_node_disjoint},
    {"ownership", "intermediate_isolation", ownership_intermediate_isolation},
    {"transitive", "nested", transitive_nested},
    {"transitive", "shared_inner", transitive_shared_inner},
    {"budget", "zero", budget_zero},
    {"budget", "cap_and_reuse", budget_cap_and_reuse},
    {"budget", "rerun_growth", budget_rerun_growth},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
