//=============================================================================
//
// DerivativeUniformity.cpp
//
// Conservative, read-only uniformity analysis. Every value is classified as
// either uniform (identical for all fragments which reach the instruction) or
// varying. The classification is propagated through expressions, local
// variable state and function calls, and derivative operations are checked
// against the uniformity of the control flow they execute in.
//
//=============================================================================

#include "DerivativeUniformity.h"

#include "Engine.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace M4
{

namespace
{

enum FlowFlag
{
    FLOW_UNIFORM = 0,
    FLOW_VARYING = 1,
};

enum ExitKind
{
    EXIT_NONE,       // Control fell through the statement list.
    EXIT_RETURN,
    EXIT_DISCARD,
    EXIT_BREAK,
    EXIT_CONTINUE,
};

// All ways control can leave a statement list. A divergent if may produce
// several of these at once when its branches take different jumps.
struct ExecResult
{
    bool fellThrough;
    bool returned;
    bool discarded;
    bool breaked;
    bool continued;

    ExecResult()
        : fellThrough(true)
        , returned(false)
        , discarded(false)
        , breaked(false)
        , continued(false)
    {
    }

    static ExecResult Jump(ExitKind kind)
    {
        ExecResult result;
        result.fellThrough = false;
        switch (kind)
        {
        case EXIT_RETURN:    result.returned  = true; break;
        case EXIT_DISCARD:   result.discarded = true; break;
        case EXIT_BREAK:     result.breaked   = true; break;
        case EXIT_CONTINUE:  result.continued = true; break;
        default:             result.fellThrough = true; break;
        }
        return result;
    }

    void Merge(const ExecResult& other)
    {
        fellThrough = fellThrough || other.fellThrough;
        returned    = returned    || other.returned;
        discarded   = discarded   || other.discarded;
        breaked     = breaked     || other.breaked;
        continued   = continued   || other.continued;
    }

    bool HasJump() const
    {
        return returned || discarded || breaked || continued;
    }
};

// Uniformity of the control flow at the current program point.
struct FlowState
{
    bool            reachable;
    bool            uniform;
    const HLSLNode* reason;     // Divergent condition responsible, if any.

    FlowState() : reachable(true), uniform(true), reason(NULL) {}
};

// Snapshot of the entire abstract state, used to model branch reconvergence
// and loop fixed points.
struct StateSnapshot
{
    FlowState                          flow;
    std::map<const void*, int>         locals;
    std::map<const void*, int>         globals;
    std::vector<const void*>           assigned;
};

bool IsSensitiveOperation(const char* name)
{
    return String_Equal(name, "ddx") ||
           String_Equal(name, "ddy") ||
           String_Equal(name, "tex2D") ||
           String_Equal(name, "texCUBE") ||
           String_Equal(name, "tex2Dbias");
}

bool IsTextureOrDerivativeName(const char* name)
{
    // Texture loads and derivative results are always fragment dependent.
    return String_Equal(name, "ddx") ||
           String_Equal(name, "ddy") ||
           String_Equal(name, "tex2D") ||
           String_Equal(name, "tex2Dproj") ||
           String_Equal(name, "tex2Dbias") ||
           String_Equal(name, "tex2Dlod") ||
           String_Equal(name, "tex2Dgrad") ||
           String_Equal(name, "tex2Dfetch") ||
           String_Equal(name, "tex2Dcmp") ||
           String_Equal(name, "tex2DMSfetch") ||
           String_Equal(name, "tex2DArray") ||
           String_Equal(name, "tex3D") ||
           String_Equal(name, "tex3Dlod") ||
           String_Equal(name, "tex3Dbias") ||
           String_Equal(name, "texCUBE") ||
           String_Equal(name, "texCUBElod") ||
           String_Equal(name, "texCUBEbias");
}

const HLSLFunction* ResolveFunctionBody(const HLSLFunction* function)
{
    // Forward declarations point at the defining function through 'forward'.
    while (function != NULL && function->statement == NULL && function->forward != NULL)
    {
        function = function->forward;
    }
    return function;
}

} // anonymous namespace

class UniformityAnalyzer
{
public:

    explicit UniformityAnalyzer(HLSLTree* tree)
        : m_tree(tree)
        , m_aborted(false)
    {
    }

    bool Run(const char* entryName, std::vector<DerivativeDiagnostic>& diagnostics)
    {
        m_diagnostics = &diagnostics;

        HLSLFunction* entry = m_tree->FindFunction(entryName);
        if (entry == NULL)
        {
            EmitUnsupported(NULL, 0, NULL, "entry point does not exist");
            return false;
        }

        RegisterGlobals();

        // Entry point inputs carrying a semantic are interpolated varying
        // inputs; arguments without a semantic are uniform.
        std::vector<int> parameterFlow;
        for (HLSLArgument* argument = entry->argument; argument != NULL; argument = argument->nextArgument)
        {
            parameterFlow.push_back(ArgumentIsVarying(argument) ? FLOW_VARYING : FLOW_UNIFORM);
        }

        m_chain.clear();
        m_active.clear();

        CallResult result;
        AnalyzeFunction(entry, parameterFlow, result);

        return !m_aborted && diagnostics.empty();
    }

private:

    struct CallResult
    {
        std::vector<int> parameterFlow;   // Final flow of each parameter slot.
        int              returnFlow;

        CallResult() : returnFlow(FLOW_UNIFORM) {}
    };

    HLSLTree*                                   m_tree;
    std::vector<DerivativeDiagnostic>*          m_diagnostics;
    bool                                        m_aborted;

    std::map<const void*, int>                  m_locals;
    std::map<const void*, int>                  m_globals;
    std::map<std::string, const void*>          m_localNames;
    // Ordered history of assignment targets since function entry; branch
    // points slice this list to find variables assigned inside the branch.
    std::vector<const void*>                    m_assigned;

    FlowState                                   m_flow;
    std::vector<const char*>                    m_chain;
    std::set<const HLSLFunction*>               m_active;
    std::set<std::pair<const void*, const void*> > m_reported;

    // -----------------------------------------------------------------------
    // Diagnostics
    // -----------------------------------------------------------------------

    void EmitDivergent(HLSLFunctionCall* call)
    {
        const void* reasonNode = m_flow.reason;
        std::pair<const void*, const void*> key(call, reasonNode);
        if (!m_reported.insert(key).second)
        {
            return;
        }

        DerivativeDiagnostic diagnostic;
        diagnostic.type             = DerivativeDiagnostic_DivergentOperation;
        diagnostic.fileName         = call->fileName;
        diagnostic.line             = call->line;
        diagnostic.reasonFileName   = m_flow.reason ? m_flow.reason->fileName : NULL;
        diagnostic.reasonLine       = m_flow.reason ? m_flow.reason->line : 0;
        diagnostic.callChain        = m_chain;
        diagnostic.message          = "derivative operation is executed in divergent control flow";
        m_diagnostics->push_back(diagnostic);
    }

    void EmitUnsupported(const HLSLNode* node, int line, const char* fileName, const char* message)
    {
        if (m_aborted)
        {
            return;
        }
        m_aborted = true;

        DerivativeDiagnostic diagnostic;
        diagnostic.type             = DerivativeDiagnostic_Unsupported;
        diagnostic.fileName         = fileName;
        diagnostic.line             = line;
        diagnostic.reasonFileName   = NULL;
        diagnostic.reasonLine       = 0;
        diagnostic.callChain        = m_chain;
        diagnostic.message          = message;

        if (node != NULL)
        {
            diagnostic.fileName = node->fileName;
            diagnostic.line     = node->line;
        }
        m_diagnostics->push_back(diagnostic);
    }

    // -----------------------------------------------------------------------
    // State snapshots
    // -----------------------------------------------------------------------

    StateSnapshot Snapshot() const
    {
        StateSnapshot snapshot;
        snapshot.flow     = m_flow;
        snapshot.locals   = m_locals;
        snapshot.globals  = m_globals;
        snapshot.assigned = m_assigned;
        return snapshot;
    }

    void Restore(const StateSnapshot& snapshot)
    {
        m_flow     = snapshot.flow;
        m_locals   = snapshot.locals;
        m_globals  = snapshot.globals;
        m_assigned = snapshot.assigned;
    }

    static void MergeMaps(std::map<const void*, int>& dst, const std::map<const void*, int>& src)
    {
        for (std::map<const void*, int>::const_iterator it = src.begin(); it != src.end(); ++it)
        {
            dst[it->first] |= it->second;
        }
    }

    static void MergeSets(std::vector<const void*>& dst, const std::vector<const void*>& src)
    {
        dst.insert(dst.end(), src.begin(), src.end());
    }

    bool MapsEqual(const std::map<const void*, int>& a, const std::map<const void*, int>& b) const
    {
        if (a.size() != b.size())
        {
            return false;
        }
        std::map<const void*, int>::const_iterator ib = b.begin();
        for (std::map<const void*, int>::const_iterator ia = a.begin(); ia != a.end(); ++ia, ++ib)
        {
            if (ia->first != ib->first || ia->second != ib->second)
            {
                return false;
            }
        }
        return true;
    }

    // -----------------------------------------------------------------------
    // Symbol registration
    // -----------------------------------------------------------------------

    void RegisterGlobals()
    {
        HLSLStatement* statement = m_tree->GetRoot()->statement;
        while (statement != NULL)
        {
            if (statement->nodeType == HLSLNodeType_Declaration)
            {
                HLSLDeclaration* declaration = static_cast<HLSLDeclaration*>(statement);
                for (HLSLDeclaration* d = declaration; d != NULL; d = d->nextDeclaration)
                {
                    // Global variables are uniforms, cbuffer fields or static
                    // constants: every fragment sees the same initial value.
                    m_globals[d] = FLOW_UNIFORM;
                }
            }
            else if (statement->nodeType == HLSLNodeType_Buffer)
            {
                HLSLBuffer* buffer = static_cast<HLSLBuffer*>(statement);
                for (HLSLDeclaration* field = buffer->field; field != NULL; field = static_cast<HLSLDeclaration*>(field->nextStatement))
                {
                    m_globals[field] = FLOW_UNIFORM;
                }
            }
            statement = statement->nextStatement;
        }
    }

    bool ArgumentIsVarying(const HLSLArgument* argument) const
    {
        if (argument->semantic != NULL || argument->sv_semantic != NULL)
        {
            return true;
        }
        if (argument->type.baseType == HLSLBaseType_UserDefined)
        {
            HLSLStruct* structure = m_tree->FindGlobalStruct(argument->type.typeName);
            if (structure != NULL)
            {
                for (HLSLStructField* field = structure->field; field != NULL; field = field->nextField)
                {
                    if (field->semantic != NULL || field->sv_semantic != NULL)
                    {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    void RegisterLocal(HLSLDeclaration* declaration)
    {
        for (HLSLDeclaration* d = declaration; d != NULL; d = d->nextDeclaration)
        {
            m_localNames[d->name] = d;
            m_locals[d] = FLOW_UNIFORM;
        }
    }

    const void* ResolveBaseVariable(HLSLExpression* expression)
    {
        if (expression == NULL)
        {
            return NULL;
        }
        if (expression->nodeType == HLSLNodeType_IdentifierExpression)
        {
            HLSLIdentifierExpression* identifier = static_cast<HLSLIdentifierExpression*>(expression);
            std::map<std::string, const void*>::const_iterator it = m_localNames.find(identifier->name);
            if (it != m_localNames.end())
            {
                return it->second;
            }
            HLSLDeclaration* global = m_tree->FindGlobalDeclaration(identifier->name);
            return global;
        }
        if (expression->nodeType == HLSLNodeType_MemberAccess)
        {
            return ResolveBaseVariable(static_cast<HLSLMemberAccess*>(expression)->object);
        }
        if (expression->nodeType == HLSLNodeType_ArrayAccess)
        {
            return ResolveBaseVariable(static_cast<HLSLArrayAccess*>(expression)->array);
        }
        return NULL;
    }

    int GetVariableFlow(const void* key) const
    {
        std::map<const void*, int>::const_iterator it = m_locals.find(key);
        if (it != m_locals.end())
        {
            return it->second;
        }
        it = m_globals.find(key);
        if (it != m_globals.end())
        {
            return it->second;
        }
        // Unknown storage is conservatively treated as varying.
        return FLOW_VARYING;
    }

    void SetVariableFlow(const void* key, int flow)
    {
        m_assigned.push_back(key);
        if (m_locals.count(key) != 0)
        {
            m_locals[key] = flow;
        }
        else if (m_globals.count(key) != 0)
        {
            m_globals[key] = flow;
        }
        else
        {
            m_locals[key] = flow;
        }
    }

    // -----------------------------------------------------------------------
    // Constant evaluation (literal constant conditions only; no algebraic
    // equivalence proving is attempted).
    // -----------------------------------------------------------------------

    bool TryConstCondition(HLSLExpression* expression, int& value) const
    {
        if (expression == NULL)
        {
            return false;
        }
        return m_tree->GetExpressionValue(expression, value);
    }

    // -----------------------------------------------------------------------
    // Expression evaluation
    // -----------------------------------------------------------------------

    int MergeFlow(int a, int b) const
    {
        return (a == FLOW_VARYING || b == FLOW_VARYING) ? FLOW_VARYING : FLOW_UNIFORM;
    }

    int EvalExpression(HLSLExpression* expression)
    {
        if (expression == NULL || m_aborted)
        {
            return FLOW_UNIFORM;
        }

        switch (expression->nodeType)
        {
        case HLSLNodeType_LiteralExpression:
            return FLOW_UNIFORM;

        case HLSLNodeType_IdentifierExpression:
            {
                const void* key = ResolveBaseVariable(expression);
                if (key == NULL)
                {
                    return FLOW_VARYING;
                }
                return GetVariableFlow(key);
            }

        case HLSLNodeType_UnaryExpression:
            return EvalExpression(static_cast<HLSLUnaryExpression*>(expression)->expression);

        case HLSLNodeType_CastingExpression:
            return EvalExpression(static_cast<HLSLCastingExpression*>(expression)->expression);

        case HLSLNodeType_ConstructorExpression:
            {
                int flow = FLOW_UNIFORM;
                HLSLExpression* argument = static_cast<HLSLConstructorExpression*>(expression)->argument;
                while (argument != NULL)
                {
                    flow = MergeFlow(flow, EvalExpression(argument));
                    argument = argument->nextExpression;
                }
                return flow;
            }

        case HLSLNodeType_BinaryExpression:
            return EvalBinaryExpression(static_cast<HLSLBinaryExpression*>(expression));

        case HLSLNodeType_ConditionalExpression:
            {
                HLSLConditionalExpression* conditional = static_cast<HLSLConditionalExpression*>(expression);
                int flow = EvalExpression(conditional->condition);
                flow = MergeFlow(flow, EvalExpression(conditional->trueExpression));
                flow = MergeFlow(flow, EvalExpression(conditional->falseExpression));
                return flow;
            }

        case HLSLNodeType_MemberAccess:
            return EvalExpression(static_cast<HLSLMemberAccess*>(expression)->object);

        case HLSLNodeType_ArrayAccess:
            {
                HLSLArrayAccess* arrayAccess = static_cast<HLSLArrayAccess*>(expression);
                // A dynamic index contributes its own dependence to the result.
                return MergeFlow(EvalExpression(arrayAccess->array), EvalExpression(arrayAccess->index));
            }

        case HLSLNodeType_FunctionCall:
            return EvalFunctionCall(static_cast<HLSLFunctionCall*>(expression));

        case HLSLNodeType_SamplerState:
            return FLOW_UNIFORM;

        default:
            EmitUnsupported(expression, expression->line, expression->fileName,
                            "unsupported expression node in derivative uniformity analysis");
            return FLOW_VARYING;
        }
    }

    int EvalBinaryExpression(HLSLBinaryExpression* expression)
    {
        HLSLBinaryOp op = expression->binaryOp;

        if (!IsAssignOp(op))
        {
            return MergeFlow(EvalExpression(expression->expression1),
                             EvalExpression(expression->expression2));
        }

        int rhsFlow = EvalExpression(expression->expression2);
        const void* target = ResolveBaseVariable(expression->expression1);
        if (target == NULL)
        {
            EmitUnsupported(expression, expression->line, expression->fileName,
                            "unsupported assignment target in derivative uniformity analysis");
            return FLOW_VARYING;
        }

        int resultFlow = rhsFlow;
        if (op != HLSLBinaryOp_Assign)
        {
            resultFlow = MergeFlow(resultFlow, GetVariableFlow(target));
        }
        SetVariableFlow(target, resultFlow);
        return resultFlow;
    }

    int EvalFunctionCall(HLSLFunctionCall* call)
    {
        const HLSLFunction* callee = call->function;
        const char* name = callee->name;

        int argumentFlow = FLOW_UNIFORM;
        std::vector<int> argumentFlows;
        for (HLSLExpression* argument = call->argument; argument != NULL; argument = argument->nextExpression)
        {
            int flow = EvalExpression(argument);
            argumentFlows.push_back(flow);
            argumentFlow = MergeFlow(argumentFlow, flow);
        }

        const HLSLFunction* body = ResolveFunctionBody(callee);

        if (body == NULL || body->statement == NULL)
        {
            // Intrinsic function.
            if (IsSensitiveOperation(name))
            {
                if (m_flow.reachable && !m_flow.uniform)
                {
                    EmitDivergent(call);
                }
            }
            if (IsTextureOrDerivativeName(name))
            {
                return FLOW_VARYING;
            }
            return argumentFlow;
        }

        // User defined function: analyze the actual overload bound by the
        // parser, propagating the argument dependence and current control
        // dependence into the call.
        if (m_active.count(body) != 0)
        {
            EmitUnsupported(call, call->line, call->fileName,
                            "recursive function calls are not supported by derivative uniformity analysis");
            return FLOW_VARYING;
        }

        CallResult result;
        AnalyzeFunction(body, argumentFlows, result);
        if (m_aborted)
        {
            return FLOW_VARYING;
        }

        // Propagate out/inout parameter flows back onto the call targets.
        HLSLExpression* argument = call->argument;
        for (size_t i = 0; argument != NULL && i < result.parameterFlow.size(); ++i, argument = argument->nextExpression)
        {
            if (result.parameterFlow[i] == FLOW_VARYING)
            {
                const void* target = ResolveBaseVariable(argument);
                if (target != NULL)
                {
                    SetVariableFlow(target, MergeFlow(GetVariableFlow(target), FLOW_VARYING));
                }
            }
        }

        return result.returnFlow;
    }

    // -----------------------------------------------------------------------
    // Function analysis
    // -----------------------------------------------------------------------

    bool AnalyzeFunction(const HLSLFunction* function, const std::vector<int>& parameterFlow,
                         CallResult& result)
    {
        std::map<const void*, int> savedLocals      = m_locals;
        std::map<std::string, const void*> savedNames = m_localNames;
        std::vector<const void*> savedAssigned     = m_assigned;
        FlowState savedFlow                        = m_flow;

        m_locals.clear();
        m_localNames.clear();
        m_assigned.clear();
        // The callee inherits the caller's control dependence: a derivative
        // called from a helper is only safe if the call site itself is uniform.
        m_flow = savedFlow;

        size_t index = 0;
        for (HLSLArgument* argument = function->argument; argument != NULL; argument = argument->nextArgument, ++index)
        {
            int flow = index < parameterFlow.size() ? parameterFlow[index] : FLOW_UNIFORM;
            m_locals[argument] = flow;
            m_localNames[argument->name] = argument;
        }

        m_chain.push_back(function->name);
        m_active.insert(function);

        int returnFlow = FLOW_UNIFORM;
        ExecResult exec = ExecuteStatements(function->statement, &returnFlow);
        (void)exec;

        m_active.erase(function);
        m_chain.pop_back();

        // Capture the final flow of every parameter slot for out/inout args.
        result.parameterFlow.clear();
        index = 0;
        for (HLSLArgument* argument = function->argument; argument != NULL; argument = argument->nextArgument, ++index)
        {
            (void)index;
            std::map<const void*, int>::const_iterator it = m_locals.find(argument);
            result.parameterFlow.push_back(it != m_locals.end() ? it->second : FLOW_UNIFORM);
        }
        result.returnFlow = returnFlow;

        m_locals     = savedLocals;
        m_localNames = savedNames;
        m_assigned   = savedAssigned;
        m_flow       = savedFlow;

        return !m_aborted;
    }

    // -----------------------------------------------------------------------
    // Statement execution
    // -----------------------------------------------------------------------

    ExecResult ExecuteStatements(HLSLStatement* statement, int* returnFlow)
    {
        ExecResult result;
        while (statement != NULL)
        {
            if (!statement->hidden)
            {
                result = ExecuteStatement(statement, returnFlow);
                if (!result.fellThrough)
                {
                    return result;
                }
            }
            statement = statement->nextStatement;
        }
        return result;
    }

    ExecResult ExecuteStatement(HLSLStatement* statement, int* returnFlow)
    {
        if (m_aborted)
        {
            return ExecResult::Jump(EXIT_RETURN);
        }

        switch (statement->nodeType)
        {
        case HLSLNodeType_Declaration:
            ExecuteDeclaration(static_cast<HLSLDeclaration*>(statement));
            return ExecResult();

        case HLSLNodeType_ExpressionStatement:
            EvalExpression(static_cast<HLSLExpressionStatement*>(statement)->expression);
            return ExecResult();

        case HLSLNodeType_BlockStatement:
            return ExecuteStatements(static_cast<HLSLBlockStatement*>(statement)->statement, returnFlow);

        case HLSLNodeType_ReturnStatement:
            {
                HLSLReturnStatement* returnStatement = static_cast<HLSLReturnStatement*>(statement);
                if (returnStatement->expression != NULL)
                {
                    *returnFlow = MergeFlow(*returnFlow, EvalExpression(returnStatement->expression));
                }
                return ExecResult::Jump(EXIT_RETURN);
            }

        case HLSLNodeType_DiscardStatement:
            return ExecResult::Jump(EXIT_DISCARD);

        case HLSLNodeType_BreakStatement:
            return ExecResult::Jump(EXIT_BREAK);

        case HLSLNodeType_ContinueStatement:
            return ExecResult::Jump(EXIT_CONTINUE);

        case HLSLNodeType_IfStatement:
            return ExecuteIf(static_cast<HLSLIfStatement*>(statement), returnFlow);

        case HLSLNodeType_ForStatement:
            return ExecuteFor(static_cast<HLSLForStatement*>(statement), returnFlow);

        default:
            EmitUnsupported(statement, statement->line, statement->fileName,
                            "unsupported statement node in derivative uniformity analysis");
            return ExecResult::Jump(EXIT_RETURN);
        }
    }

    void ExecuteDeclaration(HLSLDeclaration* declaration)
    {
        RegisterLocal(declaration);
        for (HLSLDeclaration* d = declaration; d != NULL; d = d->nextDeclaration)
        {
            if (d->assignment != NULL)
            {
                // Initialization is not a branch assignment, so it deliberately
                // bypasses the divergence tracking in SetVariableFlow.
                int flow = EvalExpression(d->assignment);
                if (m_locals.count(d) != 0)
                {
                    m_locals[d] = flow;
                }
                else
                {
                    m_globals[d] = flow;
                }
            }
        }
    }

    void ForceAssignedVarying(StateSnapshot& state, const std::vector<const void*>& assigned)
    {
        for (size_t i = 0; i < assigned.size(); ++i)
        {
            const void* key = assigned[i];
            if (state.locals.count(key) != 0)
            {
                state.locals[key] = FLOW_VARYING;
            }
            else if (state.globals.count(key) != 0)
            {
                state.globals[key] = FLOW_VARYING;
            }
        }
    }

    // Returns the assignment history produced inside the branch: everything
    // appended after the incoming snapshot was assigned while the branch ran.
    static std::vector<const void*> LocalAssignments(const std::vector<const void*>& all,
                                                     size_t incomingCount)
    {
        std::vector<const void*> result;
        if (all.size() > incomingCount)
        {
            result.assign(all.begin() + incomingCount, all.end());
        }
        return result;
    }

    ExecResult ExecuteIf(HLSLIfStatement* node, int* returnFlow)
    {
        int constantValue = 0;
        bool constantCondition = TryConstCondition(node->condition, constantValue);

        StateSnapshot incoming = Snapshot();
        const size_t incomingAssignCount = incoming.assigned.size();
        int conditionFlow = EvalExpression(node->condition);

        bool divergent = (conditionFlow == FLOW_VARYING);

        StateSnapshot thenState;
        ExecResult thenResult;
        bool runThen = !constantCondition || constantValue != 0;
        if (runThen)
        {
            if (divergent)
            {
                m_flow.uniform = false;
                m_flow.reason  = node->condition;
            }
            thenResult = ExecuteStatements(node->statement, returnFlow);
            thenState  = Snapshot();
            Restore(incoming);
        }

        StateSnapshot elseState;
        ExecResult elseResult;
        bool hasElse = node->elseStatement != NULL;
        bool runElse = hasElse && (!constantCondition || constantValue == 0);
        if (runElse)
        {
            if (divergent)
            {
                m_flow.uniform = false;
                m_flow.reason  = node->condition;
            }
            elseResult = ExecuteStatements(node->elseStatement, returnFlow);
            elseState   = Snapshot();
            Restore(incoming);
        }

        // Combine the exits of all branch paths. A missing else is an implicit
        // path which falls through with the incoming state.
        ExecResult combined;
        combined.fellThrough = false;
        if (runThen)
        {
            combined.Merge(thenResult);
        }
        if (runElse)
        {
            combined.Merge(elseResult);
        }
        if (!hasElse)
        {
            combined.fellThrough = true;
        }

        if (combined.fellThrough)
        {
            // Join the state of every path which reaches the reconvergence
            // point. Paths that jump leave the scope and carry no state here.
            StateSnapshot merged = incoming;
            if (runThen && thenResult.fellThrough)
            {
                MergeMaps(merged.locals, thenState.locals);
                MergeMaps(merged.globals, thenState.globals);
                MergeSets(merged.assigned, thenState.assigned);
            }
            if (runElse && elseResult.fellThrough)
            {
                MergeMaps(merged.locals, elseState.locals);
                MergeMaps(merged.globals, elseState.globals);
                MergeSets(merged.assigned, elseState.assigned);
            }
            if (divergent)
            {
                // Even when both branches write the same constant, a value
                // assigned inside a divergent branch is conservatively treated
                // as varying after reconvergence.
                if (runThen && thenResult.fellThrough)
                {
                    ForceAssignedVarying(merged, LocalAssignments(thenState.assigned, incomingAssignCount));
                }
                if (runElse && elseResult.fellThrough)
                {
                    ForceAssignedVarying(merged, LocalAssignments(elseState.assigned, incomingAssignCount));
                }
            }
            Restore(merged);

            if (divergent && combined.HasJump())
            {
                // Some fragments left early; the surviving fall-through path
                // must not be treated as fully reconverged.
                m_flow.uniform = false;
                m_flow.reason  = node->condition;
            }
        }
        else
        {
            // No fragment continues past this if.
            m_flow.reachable = false;
        }

        return combined;
    }

    ExecResult ExecuteFor(HLSLForStatement* node, int* returnFlow)
    {
        if (node->initialization != NULL)
        {
            ExecuteDeclaration(node->initialization);
        }

        const bool incomingUniform = m_flow.uniform;
        const HLSLNode* incomingReason = m_flow.reason;
        bool divergentJump = false;
        const HLSLNode* divergentReason = NULL;
        ExecResult loopResult;

        const int maxIterations = 1024;
        for (int iteration = 0; iteration < maxIterations; ++iteration)
        {
            StateSnapshot head = Snapshot();
            // Each iteration starts at the loop header's control state.
            m_flow = head.flow;

            if (node->condition != NULL)
            {
                int constantValue = 0;
                if (TryConstCondition(node->condition, constantValue) && constantValue == 0)
                {
                    break;
                }
                if (EvalExpression(node->condition) == FLOW_VARYING)
                {
                    m_flow.uniform = false;
                    m_flow.reason  = node->condition;
                }
            }

            ExecResult body = ExecuteStatements(node->statement, returnFlow);
            loopResult.Merge(body);

            if (body.returned || body.discarded)
            {
                if (!body.fellThrough && !body.breaked && !body.continued)
                {
                    // Every fragment leaves the function from inside the loop.
                    return loopResult;
                }
                // Mixed paths: the surviving fragments continue iterating, but
                // control after the loop can no longer be assumed uniform.
                if (!divergentJump)
                {
                    divergentJump   = true;
                    divergentReason = m_flow.reason;
                }
            }
            if (body.breaked)
            {
                if (!m_flow.uniform && !divergentJump)
                {
                    divergentJump   = true;
                    divergentReason = m_flow.reason;
                }
                break;
            }
            if (body.continued && !m_flow.uniform && !divergentJump)
            {
                divergentJump   = true;
                divergentReason = m_flow.reason;
            }

            if (node->increment != NULL)
            {
                EvalExpression(node->increment);
            }

            // Iterate until the variable state reaches a fixed point so that
            // loop carried dependence is not missed.
            if (MapsEqual(m_locals, head.locals) && MapsEqual(m_globals, head.globals))
            {
                break;
            }
            if (iteration == maxIterations - 1)
            {
                EmitUnsupported(node, node->line, node->fileName,
                                "loop analysis did not reach a fixed point");
                return ExecResult::Jump(EXIT_RETURN);
            }
        }

        // The loop header is a natural reconvergence point, but a break,
        // continue or return taken in divergent control flow can leave
        // fragments on different paths, so stay conservative in that case.
        m_flow.uniform = incomingUniform && !divergentJump;
        m_flow.reason  = divergentJump ? divergentReason : incomingReason;
        return loopResult;
    }
};

bool ValidateDerivativeUniformity(HLSLTree* tree, const char* entry,
                                  std::vector<DerivativeDiagnostic>& diagnostics)
{
    if (tree == NULL || entry == NULL)
    {
        DerivativeDiagnostic diagnostic;
        diagnostic.type    = DerivativeDiagnostic_Unsupported;
        diagnostic.line    = 0;
        diagnostic.message = "invalid arguments to derivative uniformity analysis";
        diagnostics.push_back(diagnostic);
        return false;
    }

    UniformityAnalyzer analyzer(tree);
    return analyzer.Run(entry, diagnostics);
}

} // M4
