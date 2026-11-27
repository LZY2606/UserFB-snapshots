//
// Copyright (C) 2026 Google, Inc.
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
//    Neither the name of Google Inc. nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
// BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
// ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//

//
// Constant-argument specialization over the typed IR.
//
// Starting from the entry point, user-function calls are scanned in the
// original child order of each function body. A call qualifies when one or
// more of the callee's 'const in' parameters are int/uint/bool scalars fed
// by a plain, type-matching constant-union argument. All qualified arguments
// of one call together form a key (callee identity plus, per bound argument,
// the parameter position, type, and value). The first call seen with a key
// gets a freshly cloned version of the callee: the bound parameters are
// dropped, their reads in the body become constants, locals and remaining
// parameters receive fresh symbol ids, and globals (uniforms, stage I/O,
// built-ins) keep referring to the original symbols. Later calls with the
// same key reuse the existing version. A newly encountered version's body is
// processed before the scan of the caller resumes, so constants that appear
// at helper call sites through parameter substitution take part in the same
// round.
//

#include "constantArgumentSpecialization.h"

#include "localintermediate.h"
#include "SymbolTable.h"
#include "../Include/InfoSink.h"

#include <map>
#include <set>
#include <vector>

namespace glslang {

namespace {

// Marks generated version names. Cannot appear in user or mangled names,
// because GLSL identifiers never contain '@'.
const char kSpecMarker[] = "@spec";

// One argument bound at a call site: which parameter, and which constant.
struct TBoundArgument {
    int paramIndex = 0;
    long long paramId = 0;          // symbol id of the bound parameter in the callee
    TBasicType basicType = EbtInt;
    long long intValue = 0;         // EbtInt value; EbtBool value as 0/1
    unsigned long long uintValue = 0; // EbtUint value
    const TConstUnionArray* constArray = nullptr; // value to plug in, owned by the call's constant node
    const TType* paramType = nullptr; // type of the bound parameter, owned by the callee's parameter symbol
};

class TSpecializationContext {
public:
    TSpecializationContext(TIntermediate& intermediate, unsigned maxVersions, SpecializationStats& stats)
        : intermediate(intermediate), maxVersions(maxVersions), stats(stats) { }

    void run();

private:
    void processFunction(TIntermAggregate* function);
    void processNode(TIntermNode* node, const TString& callerName);
    void handleCall(TIntermAggregate* call, const TString& callerName);
    TIntermAggregate* createVersion(TIntermAggregate* function, const std::vector<TBoundArgument>& bindings,
                                    const TString& versionName);
    void rewriteCall(TIntermAggregate* call, const TString& versionName,
                     const std::vector<TBoundArgument>& bindings);

    TIntermNode* cloneNode(TIntermNode* node);
    TIntermTyped* cloneTyped(TIntermTyped* node)
    {
        return node == nullptr ? nullptr : cloneNode(node)->getAsTyped();
    }
    TIntermNode* cloneSymbol(TIntermSymbol* symbol);
    long long mapId(long long id);

    TIntermediate& intermediate;
    unsigned maxVersions;
    SpecializationStats& stats;
    TInfoSink infoSink;

    TIntermAggregate* root = nullptr;
    unsigned existingVersions = 0;
    std::map<TString, TIntermAggregate*> functions;   // mangled name -> definition node
    std::map<TIntermAggregate*, int> defPositions;    // definition node -> index in the root sequence
    std::set<long long> globalIds;                    // ids of linker-visible global symbols
    std::set<TIntermAggregate*> processed;            // function bodies already scanned in this run

    // Per-clone state, reset by createVersion().
    std::map<long long, long long> idRemap;                       // original local id -> fresh id
    std::map<long long, const TBoundArgument*> boundByParamId;    // bound parameter id -> argument
};

// Allocate a symbol id that cannot collide with anything already in this
// intermediate, and remember it so later passes stay unique as well.
long long TSpecializationContext::mapId(long long id)
{
    if (globalIds.count(id) != 0 || TSymbolTable::isBuiltInSymbol(id))
        return id;
    auto it = idRemap.find(id);
    if (it != idRemap.end())
        return it->second;
    long long fresh = static_cast<long long>(intermediate.getUniqueId()) + 1;
    intermediate.setUniqueId(static_cast<unsigned long long>(fresh));
    idRemap[id] = fresh;
    return fresh;
}

TIntermNode* TSpecializationContext::cloneSymbol(TIntermSymbol* symbol)
{
    // A read of a bound parameter becomes the constant it was bound to.
    auto bound = boundByParamId.find(symbol->getId());
    if (bound != boundByParamId.end()) {
        const TBoundArgument* argument = bound->second;
        TType constType;
        constType.shallowCopy(*argument->paramType);
        constType.getQualifier().storage = EvqConst;
        TIntermConstantUnion* constant = new TIntermConstantUnion(*argument->constArray, constType);
        constant->setLoc(symbol->getLoc());
        return constant;
    }

    TIntermSymbol* copy = new TIntermSymbol(mapId(symbol->getId()), symbol->getName(), symbol->getStage(),
                                            symbol->getType(), &symbol->getMangledName());
    copy->setConstArray(symbol->getConstArray());
    if (symbol->getConstSubtree() != nullptr)
        copy->setConstSubtree(cloneTyped(symbol->getConstSubtree()));
    copy->setFlattenSubset(symbol->getFlattenSubset());
    copy->setLoc(symbol->getLoc());
    return copy;
}

// Deep-copy a subtree. No mutable node is shared with the source tree:
// every node is re-created; only immutable payloads (types, const-union
// arrays, names) are shared, matching how the front end itself copies nodes.
TIntermNode* TSpecializationContext::cloneNode(TIntermNode* node)
{
    if (node == nullptr)
        return nullptr;

    if (TIntermSymbol* symbol = node->getAsSymbolNode())
        return cloneSymbol(symbol);

    if (TIntermConstantUnion* constant = node->getAsConstantUnion()) {
        TIntermConstantUnion* copy = new TIntermConstantUnion(constant->getConstArray(), constant->getType());
        if (constant->isLiteral())
            copy->setLiteral();
        copy->setLoc(constant->getLoc());
        return copy;
    }

    if (TIntermBinary* binary = node->getAsBinaryNode()) {
        TIntermBinary* copy = new TIntermBinary(binary->getOp());
        copy->setType(binary->getType());
        copy->setOperationPrecision(binary->getOperationPrecision());
        copy->setLeft(cloneTyped(binary->getLeft()));
        copy->setRight(cloneTyped(binary->getRight()));
        copy->setLoc(binary->getLoc());
        return copy;
    }

    if (TIntermUnary* unary = node->getAsUnaryNode()) {
        TIntermUnary* copy = new TIntermUnary(unary->getOp(), unary->getWritableType());
        copy->setOperationPrecision(unary->getOperationPrecision());
        copy->setOperand(cloneTyped(unary->getOperand()));
        copy->setSpirvInstruction(unary->getSpirvInstruction());
        copy->setLoc(unary->getLoc());
        return copy;
    }

    if (TIntermAggregate* aggregate = node->getAsAggregate()) {
        TIntermAggregate* copy = new TIntermAggregate(aggregate->getOp());
        copy->setType(aggregate->getType());
        copy->setOperationPrecision(aggregate->getOperationPrecision());
        copy->setName(aggregate->getName());
        if (aggregate->isUserDefined())
            copy->setUserDefined();
        copy->getQualifierList() = aggregate->getQualifierList();
        copy->setOptimize(aggregate->getOptimize());
        copy->setDebug(aggregate->getDebug());
        copy->setLinkType(aggregate->getLinkType());
        copy->setFunctionControl(aggregate->getFunctionControl());
        copy->setSpirvInstruction(aggregate->getSpirvInstruction());
        copy->setEndLoc(aggregate->getEndLoc());
        copy->setLoc(aggregate->getLoc());
        for (TIntermNode* child : aggregate->getSequence())
            copy->getSequence().push_back(cloneNode(child));
        return copy;
    }

    if (TIntermSelection* selection = node->getAsSelectionNode()) {
        TIntermSelection* copy = new TIntermSelection(cloneTyped(selection->getCondition()),
                                                      cloneNode(selection->getTrueBlock()),
                                                      cloneNode(selection->getFalseBlock()),
                                                      selection->getType());
        if (! selection->getShortCircuit())
            copy->setNoShortCircuit();
        if (selection->getFlatten())
            copy->setFlatten();
        if (selection->getDontFlatten())
            copy->setDontFlatten();
        copy->setLoc(selection->getLoc());
        return copy;
    }

    if (TIntermLoop* loop = node->getAsLoopNode()) {
        TIntermLoop* copy = new TIntermLoop(cloneNode(loop->getBody()), cloneNode(loop->getTest()),
                                            cloneTyped(loop->getTerminal()), loop->testFirst());
        if (loop->getUnroll())
            copy->setUnroll();
        if (loop->getDontUnroll())
            copy->setDontUnroll();
        copy->setLoopDependency(loop->getLoopDependency());
        copy->setMinIterations(loop->getMinIterations());
        copy->setMaxIterations(loop->getMaxIterations());
        copy->setIterationMultiple(loop->getIterationMultiple());
        if (loop->getPeelCount() != 0)
            copy->setPeelCount(loop->getPeelCount());
        if (loop->getPartialCount() != 0)
            copy->setPartialCount(loop->getPartialCount());
        copy->setLoc(loop->getLoc());
        return copy;
    }

    if (TIntermBranch* branch = node->getAsBranchNode()) {
        TIntermBranch* copy = new TIntermBranch(branch->getFlowOp(), cloneTyped(branch->getExpression()));
        copy->setLoc(branch->getLoc());
        return copy;
    }

    if (TIntermSwitch* switchNode = node->getAsSwitchNode()) {
        TIntermSwitch* copy = new TIntermSwitch(cloneTyped(switchNode->getCondition()->getAsTyped()),
                                                static_cast<TIntermAggregate*>(cloneNode(switchNode->getBody())));
        if (switchNode->getFlatten())
            copy->setFlatten();
        if (switchNode->getDontFlatten())
            copy->setDontFlatten();
        copy->setLoc(switchNode->getLoc());
        return copy;
    }

    if (TIntermVariableDecl* decl = node->getAsVariableDecl()) {
        TIntermVariableDecl* copy = new TIntermVariableDecl(
            static_cast<TIntermSymbol*>(cloneNode(decl->getDeclSymbol())), cloneNode(decl->getInitNode()));
        copy->setLoc(decl->getLoc());
        return copy;
    }

    if (TIntermMethod* method = node->getAsMethodNode()) {
        TIntermMethod* copy = new TIntermMethod(cloneTyped(method->getObject()), method->getType(),
                                                method->getMethodName());
        copy->setLoc(method->getLoc());
        return copy;
    }

    return nullptr;
}

// Build the stable, collision-free name for a version: the callee's mangled
// name plus a full description of the bound arguments.
static TString buildVersionName(const TString& calleeName, const std::vector<TBoundArgument>& bindings)
{
    TString name = calleeName;
    name += kSpecMarker;
    for (const TBoundArgument& binding : bindings) {
        name += ".";
        name += std::to_string(binding.paramIndex).c_str();
        switch (binding.basicType) {
        case EbtInt:
            name += "i";
            name += std::to_string(binding.intValue).c_str();
            break;
        case EbtUint:
            name += "u";
            name += std::to_string(binding.uintValue).c_str();
            break;
        default:
            name += "b";
            name += binding.intValue != 0 ? "1" : "0";
            break;
        }
    }
    return name;
}

// Clone 'function' with the bound parameters removed and their uses replaced
// by constants, and link the clone into the root sequence right after the
// original definition.
TIntermAggregate* TSpecializationContext::createVersion(TIntermAggregate* function,
                                                        const std::vector<TBoundArgument>& bindings,
                                                        const TString& versionName)
{
    idRemap.clear();
    boundByParamId.clear();
    std::set<int> boundPositions;
    for (const TBoundArgument& binding : bindings) {
        boundByParamId[binding.paramId] = &binding;
        boundPositions.insert(binding.paramIndex);
    }

    // Parameter list, minus the bound slots. Remaining parameters get fresh
    // symbol ids through cloneNode().
    TIntermAggregate* oldParams = function->getSequence()[0]->getAsAggregate();
    TIntermAggregate* newParams = new TIntermAggregate(EOpParameters);
    newParams->setType(TType(EbtVoid));
    newParams->setLinkType(oldParams->getLinkType());
    newParams->setLoc(oldParams->getLoc());
    int index = 0;
    for (TIntermNode* param : oldParams->getSequence()) {
        if (boundPositions.count(index++) == 0)
            newParams->getSequence().push_back(cloneNode(param));
    }

    TIntermNode* newBody = cloneNode(function->getSequence()[1]);

    TIntermAggregate* version = new TIntermAggregate(EOpFunction);
    version->getSequence().push_back(newParams);
    version->getSequence().push_back(newBody);
    version->setType(function->getType());
    version->setName(versionName);
    version->setOptimize(function->getOptimize());
    version->setDebug(function->getDebug());
    version->setLinkType(function->getLinkType());
    version->setFunctionControl(function->getFunctionControl());
    version->setEndLoc(function->getEndLoc());
    version->setLoc(function->getLoc());

    // Splice the version into the root sequence just behind its original.
    int position = defPositions[function];
    TIntermSequence& topLevel = root->getSequence();
    topLevel.insert(topLevel.begin() + position + 1, version);
    for (auto& entry : defPositions) {
        if (entry.second > position)
            ++entry.second;
    }
    defPositions[version] = position + 1;
    functions[versionName] = version;

    return version;
}

// Repoint a call at a version, dropping the bound arguments and their
// qualifiers so the call signature matches the version's parameter list.
void TSpecializationContext::rewriteCall(TIntermAggregate* call, const TString& versionName,
                                         const std::vector<TBoundArgument>& bindings)
{
    std::set<int> boundPositions;
    for (const TBoundArgument& binding : bindings)
        boundPositions.insert(binding.paramIndex);

    TIntermSequence newArgs;
    TQualifierList newQualifiers;
    int index = 0;
    for (TIntermNode* arg : call->getSequence()) {
        if (boundPositions.count(index) == 0) {
            newArgs.push_back(arg);
            newQualifiers.push_back(call->getQualifierList()[index]);
        }
        ++index;
    }
    call->getSequence() = newArgs;
    call->getQualifierList() = newQualifiers;
    call->setName(versionName);
}

void TSpecializationContext::handleCall(TIntermAggregate* call, const TString& callerName)
{
    auto defIt = functions.find(call->getName());
    if (defIt == functions.end())
        return;
    TIntermAggregate* callee = defIt->second;
    if (callee->getSequence().size() < 2)
        return;
    TIntermAggregate* params = callee->getSequence()[0]->getAsAggregate();
    if (params == nullptr)
        return;
    TIntermSequence& paramSeq = params->getSequence();
    TIntermSequence& args = call->getSequence();
    if (paramSeq.size() != args.size() || args.size() != call->getQualifierList().size())
        return;

    // Gather every bindable (parameter, argument) pair of this call.
    std::vector<TBoundArgument> bindings;
    for (size_t p = 0; p < paramSeq.size(); ++p) {
        TIntermSymbol* paramSymbol = paramSeq[p]->getAsSymbolNode();
        if (paramSymbol == nullptr)
            continue;
        const TType& paramType = paramSymbol->getType();
        if (paramType.getQualifier().storage != EvqConstReadOnly || ! paramType.isScalar())
            continue;
        TBasicType basicType = paramType.getBasicType();
        if (basicType != EbtInt && basicType != EbtUint && basicType != EbtBool)
            continue;
        // Only a plain, type-matching constant node binds; anything else
        // (spec constants, conversions, expressions with side effects)
        // keeps being passed the original way.
        TIntermConstantUnion* constant = args[p]->getAsConstantUnion();
        if (constant == nullptr)
            continue;
        const TType& argType = constant->getType();
        if (! argType.isScalar() || argType.getBasicType() != basicType ||
            argType.getQualifier().isSpecConstant() || constant->getConstArray().size() < 1)
            continue;

        TBoundArgument binding;
        binding.paramIndex = static_cast<int>(p);
        binding.paramId = paramSymbol->getId();
        binding.basicType = basicType;
        binding.constArray = &constant->getConstArray();
        binding.paramType = &paramType;
        switch (basicType) {
        case EbtInt:
            binding.intValue = constant->getConstArray()[0].getIConst();
            break;
        case EbtUint:
            binding.uintValue = constant->getConstArray()[0].getUConst();
            break;
        default:
            binding.intValue = constant->getConstArray()[0].getBConst() ? 1 : 0;
            break;
        }
        bindings.push_back(binding);
    }
    if (bindings.empty())
        return;

    TString versionName = buildVersionName(callee->getName(), bindings);

    TIntermAggregate* version = nullptr;
    auto versionIt = functions.find(versionName);
    if (versionIt != functions.end()) {
        version = versionIt->second;
    } else {
        // The budget covers every version held by this intermediate,
        // including ones created by earlier runs.
        if (existingVersions + stats.createdVersions >= maxVersions)
            return;
        version = createVersion(callee, bindings, versionName);
        ++stats.createdVersions;
    }

    rewriteCall(call, versionName, bindings);
    ++stats.rewrittenCalls;
    intermediate.addToCallGraph(infoSink, callerName, versionName);

    // Process the version's body before returning to the caller, so that
    // constants surfaced by the parameter substitution join this round.
    processFunction(version);
}

void TSpecializationContext::processFunction(TIntermAggregate* function)
{
    if (! processed.insert(function).second)
        return;
    if (function->getSequence().size() > 1)
        processNode(function->getSequence()[1], function->getName());
}

void TSpecializationContext::processNode(TIntermNode* node, const TString& callerName)
{
    if (node == nullptr)
        return;

    if (TIntermAggregate* aggregate = node->getAsAggregate()) {
        if (aggregate->getOp() == EOpFunctionCall && aggregate->isUserDefined())
            handleCall(aggregate, callerName);
        for (TIntermNode* child : aggregate->getSequence())
            processNode(child, callerName);
        return;
    }
    if (TIntermBinary* binary = node->getAsBinaryNode()) {
        processNode(binary->getLeft(), callerName);
        processNode(binary->getRight(), callerName);
        return;
    }
    if (TIntermUnary* unary = node->getAsUnaryNode()) {
        processNode(unary->getOperand(), callerName);
        return;
    }
    if (TIntermSelection* selection = node->getAsSelectionNode()) {
        processNode(selection->getCondition(), callerName);
        processNode(selection->getTrueBlock(), callerName);
        processNode(selection->getFalseBlock(), callerName);
        return;
    }
    if (TIntermLoop* loop = node->getAsLoopNode()) {
        processNode(loop->getTest(), callerName);
        processNode(loop->getBody(), callerName);
        processNode(loop->getTerminal(), callerName);
        return;
    }
    if (TIntermBranch* branch = node->getAsBranchNode()) {
        processNode(branch->getExpression(), callerName);
        return;
    }
    if (TIntermSwitch* switchNode = node->getAsSwitchNode()) {
        processNode(switchNode->getCondition(), callerName);
        processNode(switchNode->getBody(), callerName);
        return;
    }
    if (TIntermVariableDecl* decl = node->getAsVariableDecl()) {
        processNode(decl->getInitNode(), callerName);
        return;
    }
}

void TSpecializationContext::run()
{
    root = intermediate.getTreeRoot() ? intermediate.getTreeRoot()->getAsAggregate() : nullptr;
    if (root == nullptr)
        return;

    // Index function definitions and linker-visible globals.
    TIntermSequence& topLevel = root->getSequence();
    int position = 0;
    for (TIntermNode* child : topLevel) {
        TIntermAggregate* aggregate = child->getAsAggregate();
        if (aggregate == nullptr) {
            ++position;
            continue;
        }
        if (aggregate->getOp() == EOpFunction) {
            functions[aggregate->getName()] = aggregate;
            defPositions[aggregate] = position;
            if (aggregate->getName().find(kSpecMarker) != TString::npos)
                ++existingVersions;
        } else if (aggregate->getOp() == EOpLinkerObjects) {
            for (TIntermNode* object : aggregate->getSequence()) {
                if (TIntermSymbol* symbol = object->getAsSymbolNode())
                    globalIds.insert(symbol->getId());
            }
        }
        ++position;
    }

    auto entry = functions.find(TString(intermediate.getEntryPointMangledName().c_str()));
    if (entry == functions.end())
        return;
    processFunction(entry->second);
}

} // end anonymous namespace

void SpecializeConstantArguments(TIntermediate& intermediate, unsigned maxVersions, SpecializationStats& stats)
{
    stats.createdVersions = 0;
    stats.rewrittenCalls = 0;

    // A zero budget never disturbs the IR.
    if (maxVersions == 0)
        return;
    if (intermediate.getTreeRoot() == nullptr)
        return;

    TSpecializationContext context(intermediate, maxVersions, stats);
    context.run();
}

} // end namespace glslang
