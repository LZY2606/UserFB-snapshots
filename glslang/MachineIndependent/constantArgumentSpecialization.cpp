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
// Constant-argument specialization on the typed IR.  See
// constantArgumentSpecialization.h for the contract.
//
// The pass walks the call graph from the entry point.  A user-function call
// whose "const in" int/uint/bool scalar arguments are plain constant-union
// nodes gets a key built from the callee identity plus the bound parameter
// positions, types, and values.  The key is encoded directly in the
// specialized function's name, which makes generated names stable across
// runs and lets a later call recognize (and reuse) versions it or an earlier
// call already created.  A specialized version is a deep clone of the
// original function: bound parameters are dropped from the parameter list,
// reads of them become constant nodes, and every local variable or kept
// parameter gets a fresh symbol id, while globals keep their identity.
//

#include "constantArgumentSpecialization.h"

#include "localintermediate.h"
#include "../Include/InfoSink.h"

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace glslang {

namespace {

// Marks a generated function name.  Cannot appear in a mangled user name,
// so generated names never collide with source-level functions.
const char* const kSpecializationMarker = "#cas#";

// One argument to bind: parameter position, its scalar type, and the
// call-site constant node carrying the value.
struct TBoundArgument {
    int position = 0;
    TBasicType basicType = EbtVoid;
    const TIntermConstantUnion* value = nullptr;
};

// Call 'fn' on every child of 'node', in the node's own child order.
template <typename F>
void VisitChildren(TIntermNode* node, F&& fn)
{
    if (node == nullptr)
        return;
    if (TIntermAggregate* aggregate = node->getAsAggregate()) {
        for (TIntermNode* child : aggregate->getSequence())
            fn(child);
        return;
    }
    if (TIntermBinary* binary = node->getAsBinaryNode()) {
        fn(binary->getLeft());
        fn(binary->getRight());
        return;
    }
    if (TIntermUnary* unary = node->getAsUnaryNode()) {
        fn(unary->getOperand());
        return;
    }
    if (TIntermSelection* selection = node->getAsSelectionNode()) {
        fn(selection->getCondition());
        fn(selection->getTrueBlock());
        fn(selection->getFalseBlock());
        return;
    }
    if (TIntermLoop* loop = node->getAsLoopNode()) {
        fn(loop->getTest());
        fn(loop->getBody());
        fn(loop->getTerminal());
        return;
    }
    if (TIntermBranch* branch = node->getAsBranchNode()) {
        fn(branch->getExpression());
        return;
    }
    if (TIntermSwitch* switchNode = node->getAsSwitchNode()) {
        fn(switchNode->getCondition());
        fn(switchNode->getBody());
        return;
    }
    if (TIntermMethod* method = node->getAsMethodNode()) {
        fn(method->getObject());
        return;
    }
    if (TIntermVariableDecl* decl = node->getAsVariableDecl()) {
        fn(decl->getInitNode());
        fn(decl->getDeclSymbol());
        return;
    }
}

class TConstantArgumentSpecializer {
public:
    TConstantArgumentSpecializer(TIntermediate& intermediate, unsigned maxVersions,
                                 SpecializationStats& stats)
        : intermediate(intermediate),
          maxVersions(maxVersions),
          stats(stats),
          nextId((long long)intermediate.getUniqueId())
    {
    }

    void run()
    {
        stats.createdVersions = 0;
        stats.rewrittenCalls = 0;

        TIntermNode* root = intermediate.getTreeRoot();
        if (root == nullptr)
            return;
        TIntermAggregate* rootAggregate = root->getAsAggregate();
        if (rootAggregate == nullptr ||
            (rootAggregate->getOp() != EOpSequence && rootAggregate->getOp() != EOpScope))
            return;
        rootSequence = &rootAggregate->getSequence();

        collectFunctionsAndGlobals();

        auto entry = functionDefs.find(TString(intermediate.getEntryPointMangledName().c_str()));
        if (entry == functionDefs.end())
            return;
        scanFunction(entry->second);

        if (nextId > (long long)intermediate.getUniqueId())
            intermediate.setUniqueId((unsigned long long)nextId);
    }

private:
    // Index function definitions, find ids that name global (shared) objects,
    // and count specialized versions already present from earlier calls.
    void collectFunctionsAndGlobals()
    {
        std::map<long long, int> idFunctionCount;
        for (TIntermNode* child : *rootSequence) {
            TIntermAggregate* aggregate = child->getAsAggregate();
            if (aggregate != nullptr && aggregate->getOp() == EOpFunction) {
                functionDefs[aggregate->getName()] = aggregate;
                if (aggregate->getName().find(kSpecializationMarker) != TString::npos)
                    ++existingVersions;
                std::unordered_set<long long> ids;
                collectSymbolIds(aggregate, ids);
                for (long long id : ids) {
                    ++idFunctionCount[id];
                    if (id > nextId)
                        nextId = id;
                }
            } else {
                collectSymbolIds(child, globalIds);
            }
        }
        for (long long id : globalIds)
            if (id > nextId)
                nextId = id;
        // An id referenced from more than one function cannot be function
        // local; treat it as global so its identity is always preserved.
        for (const auto& entry : idFunctionCount)
            if (entry.second > 1)
                globalIds.insert(entry.first);
    }

    void collectSymbolIds(TIntermNode* node, std::unordered_set<long long>& ids) const
    {
        if (node == nullptr)
            return;
        if (TIntermSymbol* symbol = node->getAsSymbolNode()) {
            ids.insert(symbol->getId());
            if (symbol->getConstSubtree() != nullptr)
                collectSymbolIds(symbol->getConstSubtree(), ids);
            return;
        }
        VisitChildren(node, [&](TIntermNode* child) { collectSymbolIds(child, ids); });
    }

    // Scan a function body once, following the original child order.
    void scanFunction(TIntermAggregate* function)
    {
        if (!scannedFunctions.insert(function->getName()).second)
            return;
        if (function->getSequence().size() > 1)
            scanNode(function->getSequence()[1], function);
    }

    void scanFunctionOnce(TIntermAggregate* function)
    {
        scanFunction(function);
    }

    void scanNode(TIntermNode* node, TIntermAggregate* currentFunction)
    {
        if (node == nullptr)
            return;
        TIntermAggregate* aggregate = node->getAsAggregate();
        if (aggregate != nullptr && aggregate->getOp() == EOpFunctionCall && aggregate->isUserDefined()) {
            for (TIntermNode* child : aggregate->getSequence())
                scanNode(child, currentFunction);
            handleCall(aggregate, currentFunction);
            return;
        }
        VisitChildren(node, [&](TIntermNode* child) { scanNode(child, currentFunction); });
    }

    // A bindable parameter is a "const in" int/uint/bool scalar.  Writable
    // "in", out/inout, vectors, arrays, and everything else keep the
    // original passing mechanism.
    static bool isBindableParameter(const TType& type)
    {
        if (type.getQualifier().storage != EvqConstReadOnly)
            return false;
        if (!type.isScalar())
            return false;
        switch (type.getBasicType()) {
        case EbtInt:
        case EbtUint:
        case EbtBool:
            return true;
        default:
            return false;
        }
    }

    // The argument must be a plain constant node whose type matches the
    // parameter exactly; specialization constants never qualify.
    static bool isMatchingConstant(const TIntermConstantUnion* constant, const TType& paramType)
    {
        const TType& type = constant->getType();
        if (type.getBasicType() != paramType.getBasicType())
            return false;
        if (!type.isScalar() || type.isArray())
            return false;
        if (type.getQualifier().isSpecConstant())
            return false;
        return constant->getConstArray().size() >= 1;
    }

    void handleCall(TIntermAggregate* call, TIntermAggregate* currentFunction)
    {
        auto defIt = functionDefs.find(call->getName());
        if (defIt == functionDefs.end())
            return;
        TIntermAggregate* definition = defIt->second;

        std::vector<TBoundArgument> bindings;
        findBindings(definition, call, bindings);

        if (bindings.empty()) {
            scanFunctionOnce(definition);
            return;
        }

        TString versionName = makeVersionName(call->getName(), bindings);
        auto versionIt = functionDefs.find(versionName);
        if (versionIt != functionDefs.end()) {
            // Already specialized (this call or an earlier one): reuse it,
            // even when the version budget is spent.
            rewriteCall(call, bindings, versionName);
            ++stats.rewrittenCalls;
            recordCallEdge(currentFunction, versionName);
            scanFunctionOnce(versionIt->second);
            return;
        }

        if (existingVersions + stats.createdVersions >= maxVersions) {
            // Budget spent: leave the call on the original function.
            scanFunctionOnce(definition);
            return;
        }

        TIntermAggregate* version = createVersion(definition, bindings, versionName);
        insertAfter(definition, version);
        functionDefs[versionName] = version;
        ++stats.createdVersions;
        rewriteCall(call, bindings, versionName);
        ++stats.rewrittenCalls;
        recordCallEdge(currentFunction, versionName);
        // Process the new version's body before continuing the caller, so
        // constants surfaced by the substitution join this same round.
        scanFunction(version);
    }

    void findBindings(const TIntermAggregate* definition, const TIntermAggregate* call,
                      std::vector<TBoundArgument>& bindings) const
    {
        if (definition->getSequence().size() < 2)
            return;
        const TIntermAggregate* parameters = definition->getSequence()[0]->getAsAggregate();
        if (parameters == nullptr)
            return;
        const TIntermSequence& paramSequence = parameters->getSequence();
        const TIntermSequence& arguments = call->getSequence();
        if (paramSequence.size() != arguments.size())
            return;
        for (size_t i = 0; i < paramSequence.size(); ++i) {
            const TIntermSymbol* param = paramSequence[i]->getAsSymbolNode();
            if (param == nullptr || !isBindableParameter(param->getType()))
                continue;
            const TIntermConstantUnion* constant = arguments[i]->getAsConstantUnion();
            if (constant == nullptr || !isMatchingConstant(constant, param->getType()))
                continue;
            TBoundArgument binding;
            binding.position = (int)i;
            binding.basicType = param->getType().getBasicType();
            binding.value = constant;
            bindings.push_back(binding);
        }
    }

    // The specialization key, encoded as the version's name: original
    // (overload) identity plus every bound position, type, and value.
    static TString makeVersionName(const TString& baseName, const std::vector<TBoundArgument>& bindings)
    {
        TString name = baseName;
        name += kSpecializationMarker;
        bool first = true;
        for (const TBoundArgument& binding : bindings) {
            if (!first)
                name += '#';
            first = false;
            name += std::to_string(binding.position).c_str();
            switch (binding.basicType) {
            case EbtInt:
                name += 'i';
                name += std::to_string(binding.value->getConstArray()[0].getIConst()).c_str();
                break;
            case EbtUint:
                name += 'u';
                name += std::to_string(binding.value->getConstArray()[0].getUConst()).c_str();
                break;
            default:
                name += 'b';
                name += binding.value->getConstArray()[0].getBConst() ? "1" : "0";
                break;
            }
        }
        return name;
    }

    // Redirect the call to the version and drop the bound argument slots
    // from both the argument list and the parallel qualifier list.
    static void rewriteCall(TIntermAggregate* call, const std::vector<TBoundArgument>& bindings,
                            const TString& versionName)
    {
        call->setName(versionName);
        TIntermSequence& arguments = call->getSequence();
        TQualifierList& qualifiers = call->getQualifierList();
        for (auto binding = bindings.rbegin(); binding != bindings.rend(); ++binding) {
            arguments.erase(arguments.begin() + binding->position);
            if (binding->position < (int)qualifiers.size())
                qualifiers.erase(qualifiers.begin() + binding->position);
        }
    }

    void recordCallEdge(const TIntermAggregate* caller, const TString& callee)
    {
        intermediate.addToCallGraph(infoSink, caller->getName(), callee);
    }

    void insertAfter(TIntermAggregate* definition, TIntermAggregate* version)
    {
        for (size_t i = 0; i < rootSequence->size(); ++i) {
            if ((*rootSequence)[i] == definition) {
                rootSequence->insert(rootSequence->begin() + i + 1, (TIntermNode*)version);
                return;
            }
        }
        rootSequence->push_back(version);
    }

    long long freshId()
    {
        return ++nextId;
    }

    // Build the specialized function: deep-clone the original, drop the
    // bound parameters, and substitute their reads with the constants.
    TIntermAggregate* createVersion(TIntermAggregate* definition,
                                    const std::vector<TBoundArgument>& bindings,
                                    const TString& versionName)
    {
        TIntermAggregate* parameters = definition->getSequence()[0]->getAsAggregate();

        boundConstants.clear();
        boundPositions.clear();
        for (const TBoundArgument& binding : bindings) {
            TIntermSymbol* param = parameters->getSequence()[binding.position]->getAsSymbolNode();
            boundConstants[param->getId()] = binding.value;
            boundPositions.insert(binding.position);
        }

        // Every symbol id belonging to this function that is not a global
        // and not a bound parameter gets a fresh id, so the version shares
        // no local or parameter identity with the original or other versions.
        idMap.clear();
        std::unordered_set<long long> ids;
        collectSymbolIds(definition, ids);
        for (long long id : ids) {
            if (globalIds.find(id) == globalIds.end() &&
                boundConstants.find(id) == boundConstants.end())
                idMap[id] = freshId();
        }

        TIntermAggregate* newParameters = new TIntermAggregate(EOpParameters);
        newParameters->setLoc(parameters->getLoc());
        newParameters->setType(parameters->getType());
        newParameters->setLinkType(parameters->getLinkType());
        const TIntermSequence& paramSequence = parameters->getSequence();
        for (size_t i = 0; i < paramSequence.size(); ++i) {
            if (boundPositions.find((int)i) == boundPositions.end())
                newParameters->getSequence().push_back(cloneNode(paramSequence[i]));
        }

        TIntermNode* newBody = cloneNode(definition->getSequence()[1]);

        TIntermAggregate* version = new TIntermAggregate(EOpFunction);
        version->setLoc(definition->getLoc());
        version->setType(definition->getType());
        version->setName(versionName);
        version->getSequence().push_back(newParameters);
        version->getSequence().push_back(newBody);
        version->setLinkType(definition->getLinkType());
        version->setFunctionControl(definition->getFunctionControl());
        version->setOptimize(definition->getOptimize());
        version->setDebug(definition->getDebug());
        version->setPragmaTable(definition->getPragmaTable());
        return version;
    }

    TIntermTyped* cloneTyped(TIntermTyped* node)
    {
        if (node == nullptr)
            return nullptr;
        return cloneNode(node)->getAsTyped();
    }

    // Deep-clone a subtree.  No mutable node is shared with the original:
    // bound parameter reads become fresh constant nodes, locals and kept
    // parameters are re-id'd through idMap, and globals are re-created as
    // new symbol nodes carrying the original id.
    TIntermNode* cloneNode(TIntermNode* node)
    {
        if (node == nullptr)
            return nullptr;

        if (TIntermSymbol* symbol = node->getAsSymbolNode()) {
            auto bound = boundConstants.find(symbol->getId());
            if (bound != boundConstants.end()) {
                const TIntermConstantUnion* constant = bound->second;
                TIntermConstantUnion* replacement =
                    new TIntermConstantUnion(constant->getConstArray(), constant->getType());
                replacement->setLoc(symbol->getLoc());
                if (constant->isLiteral())
                    replacement->setLiteral();
                return replacement;
            }
            long long id = symbol->getId();
            auto mapped = idMap.find(id);
            if (mapped != idMap.end())
                id = mapped->second;
            TIntermSymbol* copy =
                new TIntermSymbol(id, symbol->getName(), symbol->getStage(), symbol->getType(),
                                  &symbol->getMangledName());
            copy->setLoc(symbol->getLoc());
            copy->setConstArray(symbol->getConstArray());
            if (symbol->getConstSubtree() != nullptr)
                copy->setConstSubtree(cloneTyped(symbol->getConstSubtree()));
            copy->setFlattenSubset(symbol->getFlattenSubset());
            return copy;
        }

        if (TIntermConstantUnion* constant = node->getAsConstantUnion()) {
            TIntermConstantUnion* copy =
                new TIntermConstantUnion(constant->getConstArray(), constant->getType());
            copy->setLoc(constant->getLoc());
            if (constant->isLiteral())
                copy->setLiteral();
            return copy;
        }

        if (TIntermVariableDecl* decl = node->getAsVariableDecl()) {
            TIntermSymbol* declSymbol = decl->getDeclSymbol() == nullptr
                ? nullptr
                : cloneNode(decl->getDeclSymbol())->getAsSymbolNode();
            TIntermVariableDecl* copy = new TIntermVariableDecl(declSymbol, cloneNode(decl->getInitNode()));
            copy->setLoc(decl->getLoc());
            return copy;
        }

        if (TIntermBinary* binary = node->getAsBinaryNode()) {
            TIntermBinary* copy = new TIntermBinary(binary->getOp());
            copy->setType(binary->getType());
            copy->setLoc(binary->getLoc());
            copy->setLeft(cloneTyped(binary->getLeft()));
            copy->setRight(cloneTyped(binary->getRight()));
            copy->setOperationPrecision(binary->getOperationPrecision());
            return copy;
        }

        if (TIntermUnary* unary = node->getAsUnaryNode()) {
            TIntermUnary* copy = new TIntermUnary(unary->getOp());
            copy->setType(unary->getType());
            copy->setLoc(unary->getLoc());
            copy->setOperand(cloneTyped(unary->getOperand()));
            copy->setOperationPrecision(unary->getOperationPrecision());
            copy->setSpirvInstruction(unary->getSpirvInstruction());
            return copy;
        }

        if (TIntermAggregate* aggregate = node->getAsAggregate()) {
            TIntermAggregate* copy = new TIntermAggregate(aggregate->getOp());
            copy->setType(aggregate->getType());
            copy->setLoc(aggregate->getLoc());
            copy->setEndLoc(aggregate->getEndLoc());
            copy->setName(aggregate->getName());
            copy->getQualifierList() = aggregate->getQualifierList();
            if (aggregate->isUserDefined())
                copy->setUserDefined();
            copy->setLinkType(aggregate->getLinkType());
            copy->setFunctionControl(aggregate->getFunctionControl());
            copy->setSpirvInstruction(aggregate->getSpirvInstruction());
            for (TIntermNode* child : aggregate->getSequence())
                copy->getSequence().push_back(cloneNode(child));
            return copy;
        }

        if (TIntermSelection* selection = node->getAsSelectionNode()) {
            TIntermSelection* copy = new TIntermSelection(cloneTyped(selection->getCondition()),
                                                          cloneNode(selection->getTrueBlock()),
                                                          cloneNode(selection->getFalseBlock()),
                                                          selection->getType());
            copy->setLoc(selection->getLoc());
            if (!selection->getShortCircuit())
                copy->setNoShortCircuit();
            if (selection->getFlatten())
                copy->setFlatten();
            if (selection->getDontFlatten())
                copy->setDontFlatten();
            return copy;
        }

        if (TIntermSwitch* switchNode = node->getAsSwitchNode()) {
            TIntermSwitch* copy =
                new TIntermSwitch(cloneNode(switchNode->getCondition())->getAsTyped(),
                                  cloneNode(switchNode->getBody())->getAsAggregate());
            copy->setLoc(switchNode->getLoc());
            if (switchNode->getFlatten())
                copy->setFlatten();
            if (switchNode->getDontFlatten())
                copy->setDontFlatten();
            return copy;
        }

        if (TIntermBranch* branch = node->getAsBranchNode()) {
            TIntermBranch* copy = new TIntermBranch(branch->getFlowOp(), cloneTyped(branch->getExpression()));
            copy->setLoc(branch->getLoc());
            return copy;
        }

        if (TIntermLoop* loop = node->getAsLoopNode()) {
            TIntermLoop* copy = new TIntermLoop(cloneNode(loop->getBody()), cloneNode(loop->getTest()),
                                                cloneTyped(loop->getTerminal()), loop->testFirst());
            copy->setLoc(loop->getLoc());
            if (loop->getUnroll())
                copy->setUnroll();
            if (loop->getDontUnroll())
                copy->setDontUnroll();
            else {
                copy->setPeelCount(loop->getPeelCount());
                copy->setPartialCount(loop->getPartialCount());
            }
            copy->setLoopDependency(loop->getLoopDependency());
            copy->setMinIterations(loop->getMinIterations());
            copy->setMaxIterations(loop->getMaxIterations());
            copy->setIterationMultiple(loop->getIterationMultiple());
            copy->setMultipleWaitQueuesQCOM(loop->getMultipleWaitQueuesQCOM());
            return copy;
        }

        if (TIntermMethod* method = node->getAsMethodNode()) {
            TIntermMethod* copy =
                new TIntermMethod(cloneTyped(method->getObject()), method->getType(), method->getMethodName());
            copy->setLoc(method->getLoc());
            return copy;
        }

        return nullptr;
    }

    TIntermediate& intermediate;
    unsigned maxVersions;
    SpecializationStats& stats;
    TInfoSink infoSink;

    TIntermSequence* rootSequence = nullptr;
    std::map<TString, TIntermAggregate*> functionDefs;
    std::unordered_set<long long> globalIds;
    std::set<TString> scannedFunctions;
    unsigned existingVersions = 0;
    long long nextId;

    // Per-version clone state.
    std::unordered_map<long long, long long> idMap;
    std::unordered_map<long long, const TIntermConstantUnion*> boundConstants;
    std::set<int> boundPositions;
};

} // end anonymous namespace

void SpecializeConstantArguments(TIntermediate& intermediate, unsigned maxVersions, SpecializationStats& stats)
{
    TConstantArgumentSpecializer specializer(intermediate, maxVersions, stats);
    specializer.run();
}

} // namespace glslang
