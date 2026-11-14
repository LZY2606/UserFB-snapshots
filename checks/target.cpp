// Target checks for generalized (non-square) float matrix support.
//
// shape:  result types visible in the real AST and rejection of illegal calls.
// algebra: numeric correctness of construction, row reads, transpose and chained
//          multiplication for both GLSL output paths. Two independent engines are used:
//          a plain row-major HLSL reference and a column-major GLSL-storage evaluator that
//          walks the actual syntax tree.
// compat: legacy shaders, HLSL write-back round trips and repeated generation.

#include "HLSLParser.h"
#include "HLSLTree.h"
#include "GLSLGenerator.h"
#include "HLSLGenerator.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

// driver.c is compiled with the C++ driver in this harness, so it shares C++ linkage.
#include "task_support/check.h"

using namespace M4;

namespace
{

const char* const kFile = "target.hlsl";

HLSLTree* ParseSource(const char* source, Allocator& allocator)
{
    HLSLTree* tree = new (allocator.New<HLSLTree>()) HLSLTree(&allocator);
    HLSLParser parser(&allocator, kFile, source, std::strlen(source));
    if (!parser.Parse(tree))
    {
        return NULL;
    }
    return tree;
}

bool TranslateGLSL(const char* source, unsigned flags, std::string& out, bool& parseOk)
{
    parseOk = false;
    Allocator* allocator = new Allocator();
    HLSLTree* tree = new HLSLTree(allocator);
    HLSLParser parser(allocator, kFile, source, std::strlen(source));
    if (!parser.Parse(tree))
    {
        delete allocator;
        return false;
    }
    parseOk = true;
    GLSLGenerator::Options options;
    options.flags = flags;
    GLSLGenerator generator;
    const bool ok = generator.Generate(tree, GLSLGenerator::Target_FragmentShader,
        GLSLGenerator::Version_140, "main", options);
    if (ok)
    {
        out = generator.GetResult();
    }
    delete allocator;
    return ok;
}

bool TranslateGLSL(const char* source, unsigned flags, std::string& out)
{
    bool parseOk = false;
    return TranslateGLSL(source, flags, out, parseOk);
}

// Assertion for the value returning evaluator: records a failure and yields a default value.
#define EVAL_CHECK(x) \
    do { if (!(x)) { task_check(0, #x, __FILE__, __LINE__); return Value(); } } while (0)

// Captures stdout (where the tokenizer reports errors) for the lifetime of the object.
class OutputCapture
{
public:
    OutputCapture()
    {
        // The test sandbox only permits writes inside the working tree.
        std::snprintf(m_path, sizeof(m_path), ".task-build/task_capture_XXXXXX");
        m_fd = mkstemp(m_path);
        if (m_fd < 0)
        {
            // Fallback for sandboxes where mkstemp cannot use the system temp location.
            std::snprintf(m_path, sizeof(m_path), ".task-build/task_capture.log");
            m_fd = open(m_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
        }
        std::fflush(stdout);
        m_saved = dup(STDOUT_FILENO);
        dup2(m_fd, STDOUT_FILENO);
    }

    ~OutputCapture()
    {
        std::fflush(stdout);
        dup2(m_saved, STDOUT_FILENO);
        close(m_saved);
        close(m_fd);
        unlink(m_path);
    }

    std::string Read()
    {
        std::fflush(stdout);
        std::string result;
        char buffer[512];
        lseek(m_fd, 0, SEEK_SET);
        ssize_t count = 0;
        while ((count = read(m_fd, buffer, sizeof(buffer))) > 0)
        {
            result.append(buffer, (size_t)count);
        }
        lseek(m_fd, 0, SEEK_SET);
        return result;
    }

private:
    char m_path[256];
    int  m_fd;
    int  m_saved;
};

// ----------------------------------------------------------------------------
// AST node collection.
// ----------------------------------------------------------------------------

struct MatrixRowAccessVisitor : public HLSLTreeVisitor
{
    HLSLArrayAccess* found;
    MatrixRowAccessVisitor() : found(NULL) {}

    virtual void VisitArrayAccess(HLSLArrayAccess* node)
    {
        if (found == NULL && !node->array->expressionType.array &&
            IsMatrixType(node->array->expressionType.baseType))
        {
            found = node;
        }
        HLSLTreeVisitor::VisitArrayAccess(node);
    }
};

struct FirstIntrinsicVisitor : public HLSLTreeVisitor
{
    const char* name;
    HLSLFunctionCall* found;
    FirstIntrinsicVisitor(const char* n) : name(n), found(NULL) {}

    virtual void VisitFunctionCall(HLSLFunctionCall* node)
    {
        if (found == NULL && node->function != NULL && String_Equal(node->function->name, name))
        {
            found = node;
        }
        HLSLTreeVisitor::VisitFunctionCall(node);
    }
};

struct DeclarationCollector : public HLSLTreeVisitor
{
    std::vector<HLSLDeclaration*> declarations;

    virtual void VisitDeclaration(HLSLDeclaration* node)
    {
        declarations.push_back(node);
        HLSLTreeVisitor::VisitDeclaration(node);
    }
};

HLSLExpression* FindAssignment(HLSLTree* tree, const char* variable)
{
    DeclarationCollector collector;
    collector.VisitRoot(tree->GetRoot());
    for (size_t i = 0; i < collector.declarations.size(); ++i)
    {
        if (String_Equal(collector.declarations[i]->name, variable))
        {
            return collector.declarations[i]->assignment;
        }
    }
    return NULL;
}

// ----------------------------------------------------------------------------
// Engine A: plain row-major HLSL reference.
// ----------------------------------------------------------------------------

struct RefMatrix
{
    int rows;
    int cols;
    float v[4][4];
};

struct RefValue
{
    bool isMatrix;
    int length;
    RefMatrix matrix;
    float vector[4];
};

RefValue MakeRefVector(int length, const float values[])
{
    RefValue result;
    result.isMatrix = false;
    result.length = length;
    for (int i = 0; i < length; ++i)
    {
        result.vector[i] = values[i];
    }
    return result;
}

RefValue RefRowMatrix(int rows, int cols, const float values[])
{
    RefValue result;
    result.isMatrix = true;
    result.matrix.rows = rows;
    result.matrix.cols = cols;
    for (int r = 0; r < rows; ++r)
    {
        for (int c = 0; c < cols; ++c)
        {
            result.matrix.v[r][c] = values[r * cols + c];
        }
    }
    return result;
}

RefValue RefTranspose(const RefValue& input)
{
    RefValue result;
    result.isMatrix = true;
    result.matrix.rows = input.matrix.cols;
    result.matrix.cols = input.matrix.rows;
    for (int r = 0; r < input.matrix.rows; ++r)
    {
        for (int c = 0; c < input.matrix.cols; ++c)
        {
            result.matrix.v[c][r] = input.matrix.v[r][c];
        }
    }
    return result;
}

RefValue RefMul(const RefValue& left, const RefValue& right)
{
    RefValue result;
    if (left.isMatrix && right.isMatrix)
    {
        result.isMatrix = true;
        result.matrix.rows = left.matrix.rows;
        result.matrix.cols = right.matrix.cols;
        for (int r = 0; r < result.matrix.rows; ++r)
        {
            for (int c = 0; c < result.matrix.cols; ++c)
            {
                float sum = 0.0f;
                for (int k = 0; k < left.matrix.cols; ++k)
                {
                    sum += left.matrix.v[r][k] * right.matrix.v[k][c];
                }
                result.matrix.v[r][c] = sum;
            }
        }
        return result;
    }

    result.isMatrix = false;
    if (left.isMatrix)
    {
        result.length = left.matrix.rows;
        for (int r = 0; r < result.length; ++r)
        {
            float sum = 0.0f;
            for (int k = 0; k < left.matrix.cols; ++k)
            {
                sum += left.matrix.v[r][k] * right.vector[k];
            }
            result.vector[r] = sum;
        }
    }
    else
    {
        result.length = right.matrix.cols;
        for (int c = 0; c < result.length; ++c)
        {
            float sum = 0.0f;
            for (int k = 0; k < right.matrix.rows; ++k)
            {
                sum += left.vector[k] * right.matrix.v[k][c];
            }
            result.vector[c] = sum;
        }
    }
    return result;
}

// ----------------------------------------------------------------------------
// Engine B: GLSL column-major storage evaluator over the actual AST.
// Matrices store element (r, c) at e[c * rows + r], exactly like the generated GLSL.
// ----------------------------------------------------------------------------

struct Value
{
    bool isMatrix;
    int rows;
    int cols;       // Matrix columns; vectors use cols == 1.
    float e[16];

    float Get(int r, int c) const { return e[c * rows + r]; }
    void Set(int r, int c, float value) { e[c * rows + r] = value; }
};

class Evaluator
{
public:
    struct Binding
    {
        std::string name;
        Value value;
    };
    std::vector<Binding> environment;

    void Bind(const char* name, const Value& value)
    {
        Binding binding;
        binding.name = name;
        binding.value = value;
        environment.push_back(binding);
    }

    Value* Find(const char* name)
    {
        for (size_t i = 0; i < environment.size(); ++i)
        {
            if (environment[i].name == name)
            {
                return &environment[i].value;
            }
        }
        return NULL;
    }

    Value Evaluate(HLSLExpression* expression)
    {
        switch (expression->nodeType)
        {
        case HLSLNodeType_LiteralExpression:
            {
                HLSLLiteralExpression* literal = (HLSLLiteralExpression*)expression;
                Value result;
                std::memset(&result, 0, sizeof(result));
                result.isMatrix = false;
                result.rows = 1;
                result.cols = 1;
                result.e[0] = (literal->type == HLSLBaseType_Int) ? (float)literal->iValue : literal->fValue;
                return result;
            }

        case HLSLNodeType_IdentifierExpression:
            {
                HLSLIdentifierExpression* identifier = (HLSLIdentifierExpression*)expression;
                Value* value = Find(identifier->name);
                EVAL_CHECK(value != NULL);
                return *value;
            }

        case HLSLNodeType_ConstructorExpression:
            return EvaluateConstructor((HLSLConstructorExpression*)expression);

        case HLSLNodeType_UnaryExpression:
            {
                HLSLUnaryExpression* unary = (HLSLUnaryExpression*)expression;
                Value operand = Evaluate(unary->expression);
                if (unary->unaryOp != HLSLUnaryOp_Negative)
                {
                    task_check(0, "unary negative", __FILE__, __LINE__);
                    return Value();
                }
                const int count = operand.isMatrix ? operand.rows * operand.cols : operand.rows;
                for (int i = 0; i < count; ++i)
                {
                    operand.e[i] = -operand.e[i];
                }
                return operand;
            }

        case HLSLNodeType_BinaryExpression:
            return EvaluateBinary((HLSLBinaryExpression*)expression);

        case HLSLNodeType_ArrayAccess:
            return EvaluateArrayAccess((HLSLArrayAccess*)expression);

        case HLSLNodeType_MemberAccess:
            return EvaluateMemberAccess((HLSLMemberAccess*)expression);

        case HLSLNodeType_FunctionCall:
            return EvaluateCall((HLSLFunctionCall*)expression);

        default:
            EVAL_CHECK(false);
            return Value();
        }
    }

    Value EvaluateConstructor(HLSLConstructorExpression* constructor)
    {
        const HLSLBaseType baseType = constructor->type.baseType;
        std::vector<float> components;
        HLSLExpression* argument = constructor->argument;
        while (argument != NULL)
        {
            Value value = Evaluate(argument);
            const int count = value.isMatrix ? value.rows * value.cols : value.rows;
            for (int i = 0; i < count; ++i)
            {
                components.push_back(value.e[i]);
            }
            argument = argument->nextExpression;
        }

        Value result;
        std::memset(&result, 0, sizeof(result));
        if (IsMatrixType(baseType))
        {
            result.isMatrix = true;
            result.rows = GetMatrixRows(baseType);
            result.cols = GetMatrixCols(baseType);
            // HLSL supplies row ordered components; store column major.
            for (int r = 0; r < result.rows; ++r)
            {
                for (int c = 0; c < result.cols; ++c)
                {
                    result.Set(r, c, components[(size_t)r * result.cols + c]);
                }
            }
        }
        else
        {
            result.isMatrix = false;
            result.rows = (int)components.size();
            result.cols = 1;
            for (int i = 0; i < result.rows; ++i)
            {
                result.e[i] = components[i];
            }
        }
        return result;
    }

    Value EvaluateBinary(HLSLBinaryExpression* binary)
    {
        Value left = Evaluate(binary->expression1);
        Value right = Evaluate(binary->expression2);
        if (binary->binaryOp != HLSLBinaryOp_Add && binary->binaryOp != HLSLBinaryOp_Sub &&
            binary->binaryOp != HLSLBinaryOp_Mul && binary->binaryOp != HLSLBinaryOp_Div)
        {
            task_check(0, "arithmetic binary op", __FILE__, __LINE__);
            return Value();
        }

        const bool scalarLeft = !left.isMatrix && left.rows == 1;
        const bool scalarRight = !right.isMatrix && right.rows == 1;
        if (scalarLeft || scalarRight)
        {
            if (scalarLeft && scalarRight)
            {
                right.e[0] = Apply(binary->binaryOp, left.e[0], right.e[0]);
                return right;
            }
            Value& target = scalarLeft ? right : left;
            const float scalar = scalarLeft ? left.e[0] : right.e[0];
            const int count = target.isMatrix ? target.rows * target.cols : target.rows;
            for (int i = 0; i < count; ++i)
            {
                target.e[i] = scalarLeft ? Apply(binary->binaryOp, scalar, target.e[i])
                                         : Apply(binary->binaryOp, target.e[i], scalar);
            }
            return target;
        }

        EVAL_CHECK(left.isMatrix == right.isMatrix);
        EVAL_CHECK(left.rows == right.rows && left.cols == right.cols);
        const int count = left.isMatrix ? left.rows * left.cols : left.rows;
        for (int i = 0; i < count; ++i)
        {
            left.e[i] = Apply(binary->binaryOp, left.e[i], right.e[i]);
        }
        return left;
    }

    static float Apply(HLSLBinaryOp op, float a, float b)
    {
        switch (op)
        {
        case HLSLBinaryOp_Add: return a + b;
        case HLSLBinaryOp_Sub: return a - b;
        case HLSLBinaryOp_Mul: return a * b;
        case HLSLBinaryOp_Div: return a / b;
        default:
            task_check(0, "supported scalar op", __FILE__, __LINE__);
            return 0.0f;
        }
    }

    Value EvaluateArrayAccess(HLSLArrayAccess* arrayAccess)
    {
        Value object = Evaluate(arrayAccess->array);
        EVAL_CHECK(object.isMatrix);
        Value indexValue = Evaluate(arrayAccess->index);
        const int row = (int)indexValue.e[0];
        EVAL_CHECK(row >= 0 && row < object.rows);

        Value result;
        std::memset(&result, 0, sizeof(result));
        result.isMatrix = false;
        result.rows = object.cols;
        result.cols = 1;
        for (int c = 0; c < object.cols; ++c)
        {
            result.e[c] = object.Get(row, c);
        }
        return result;
    }

    Value EvaluateMemberAccess(HLSLMemberAccess* memberAccess)
    {
        Value object = Evaluate(memberAccess->object);
        EVAL_CHECK(!object.isMatrix);
        const char* field = memberAccess->field;
        Value result;
        std::memset(&result, 0, sizeof(result));
        result.isMatrix = false;
        result.cols = 1;
        result.rows = (int)std::strlen(field);
        for (int i = 0; i < result.rows; ++i)
        {
            int component = 0;
            switch (field[i])
            {
            case 'x': case 'r': component = 0; break;
            case 'y': case 'g': component = 1; break;
            case 'z': case 'b': component = 2; break;
            case 'w': case 'a': component = 3; break;
            default: EVAL_CHECK(false); break;
            }
            result.e[i] = object.e[component];
        }
        return result;
    }

    Value EvaluateCall(HLSLFunctionCall* call)
    {
        const char* name = call->function->name;
        if (String_Equal(name, "transpose"))
        {
            Value input = Evaluate(call->argument);
            EVAL_CHECK(input.isMatrix);
            Value result;
            std::memset(&result, 0, sizeof(result));
            result.isMatrix = true;
            result.rows = input.cols;
            result.cols = input.rows;
            for (int r = 0; r < input.rows; ++r)
            {
                for (int c = 0; c < input.cols; ++c)
                {
                    result.Set(c, r, input.Get(r, c));
                }
            }
            return result;
        }

        EVAL_CHECK(String_Equal(name, "mul"));
        Value left = Evaluate(call->argument);
        Value right = Evaluate(call->argument->nextExpression);

        Value result;
        std::memset(&result, 0, sizeof(result));
        if (left.isMatrix && right.isMatrix)
        {
            result.isMatrix = true;
            result.rows = left.rows;
            result.cols = right.cols;
            for (int r = 0; r < result.rows; ++r)
            {
                for (int c = 0; c < result.cols; ++c)
                {
                    float sum = 0.0f;
                    for (int k = 0; k < left.cols; ++k)
                    {
                        sum += left.Get(r, k) * right.Get(k, c);
                    }
                    result.Set(r, c, sum);
                }
            }
            return result;
        }

        result.isMatrix = false;
        result.cols = 1;
        if (left.isMatrix)
        {
            result.rows = left.rows;
            for (int r = 0; r < result.rows; ++r)
            {
                float sum = 0.0f;
                for (int k = 0; k < left.cols; ++k)
                {
                    sum += left.Get(r, k) * right.e[k];
                }
                result.e[r] = sum;
            }
        }
        else
        {
            result.rows = right.cols;
            for (int c = 0; c < result.rows; ++c)
            {
                float sum = 0.0f;
                for (int k = 0; k < right.rows; ++k)
                {
                    sum += left.e[k] * right.Get(k, c);
                }
                result.e[c] = sum;
            }
        }
        return result;
    }
};

// Evaluates every initialized local declaration of main in source order and binds the result
// so later expressions can reference the named variables.
void BuildMainEnvironment(HLSLTree* tree, Evaluator& evaluator)
{
    HLSLFunction* mainFunction = tree->FindFunction("main");
    HLSLStatement* statement = mainFunction->statement;
    while (statement != NULL)
    {
        if (statement->nodeType == HLSLNodeType_Declaration)
        {
            HLSLDeclaration* declaration = (HLSLDeclaration*)statement;
            while (declaration != NULL)
            {
                if (declaration->assignment != NULL)
                {
                    evaluator.Bind(declaration->name, evaluator.Evaluate(declaration->assignment));
                }
                declaration = declaration->nextDeclaration;
            }
        }
        statement = statement->nextStatement;
    }
}

RefValue ToReference(const Value& value)
{
    RefValue result;
    result.isMatrix = value.isMatrix;
    if (value.isMatrix)
    {
        result.matrix.rows = value.rows;
        result.matrix.cols = value.cols;
        for (int r = 0; r < value.rows; ++r)
        {
            for (int c = 0; c < value.cols; ++c)
            {
                result.matrix.v[r][c] = value.Get(r, c);
            }
        }
    }
    else
    {
        result.length = value.rows;
        for (int i = 0; i < value.rows; ++i)
        {
            result.vector[i] = value.e[i];
        }
    }
    return result;
}

bool CloseEnough(float a, float b)
{
    return std::fabs(a - b) < 1e-4f;
}

bool ValuesMatch(const RefValue& expected, const RefValue& actual)
{
    if (expected.isMatrix != actual.isMatrix)
    {
        return false;
    }
    if (expected.isMatrix)
    {
        if (expected.matrix.rows != actual.matrix.rows || expected.matrix.cols != actual.matrix.cols)
        {
            return false;
        }
        for (int r = 0; r < expected.matrix.rows; ++r)
        {
            for (int c = 0; c < expected.matrix.cols; ++c)
            {
                if (!CloseEnough(expected.matrix.v[r][c], actual.matrix.v[r][c]))
                {
                    return false;
                }
            }
        }
    }
    else
    {
        if (expected.length != actual.length)
        {
            return false;
        }
        for (int i = 0; i < expected.length; ++i)
        {
            if (!CloseEnough(expected.vector[i], actual.vector[i]))
            {
                return false;
            }
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// shape
// ----------------------------------------------------------------------------

int shape_locals_and_containers()
{
    static const char* source =
        "struct S { float2x3 field; float3x2 other; };\n"
        "float3x2 helper(float2x3 input, out float2 row) { row = input[0]; return transpose(input); }\n"
        "float4 main(float2 uv:TEXCOORD0):COLOR0 {\n"
        "  float2x3 local = float2x3(1,2,3,4,5,6);\n"
        "  float2x3 array[3];\n"
        "  array[1] = local;\n"
        "  S structure;\n"
        "  structure.field = local;\n"
        "  float3 row = local[1];\n"
        "  return float4(row, 1.0);\n"
        "}\n";

    Allocator allocator;
    HLSLTree* tree = ParseSource(source, allocator);
    TASK_CHECK(tree != NULL);

    HLSLExpression* local = FindAssignment(tree, "local");
    TASK_CHECK(local != NULL);
    TASK_CHECK(local->expressionType.baseType == HLSLBaseType_Float2x3);

    HLSLStruct* structure = tree->FindGlobalStruct("S");
    TASK_CHECK(structure != NULL);
    TASK_CHECK(structure->field->type.baseType == HLSLBaseType_Float2x3);
    TASK_CHECK(structure->field->nextField->type.baseType == HLSLBaseType_Float3x2);

    HLSLFunction* helper = tree->FindFunction("helper");
    TASK_CHECK(helper != NULL);
    TASK_CHECK(helper->returnType.baseType == HLSLBaseType_Float3x2);
    TASK_CHECK(helper->argument->type.baseType == HLSLBaseType_Float2x3);

    MatrixRowAccessVisitor arrayVisitor;
    arrayVisitor.VisitRoot(tree->GetRoot());
    TASK_CHECK(arrayVisitor.found != NULL);
    TASK_CHECK(arrayVisitor.found->expressionType.baseType == HLSLBaseType_Float3);
    return 0;
}

int shape_mul_dimensions()
{
    static const char* source =
        "float4 main(float2 uv:TEXCOORD0):COLOR0 {\n"
        "  float2x3 a = float2x3(1,2,3,4,5,6);\n"
        "  float3x2 b = transpose(a);\n"
        "  float3x4 c = float3x4(1,2,3,4,5,6,7,8,9,10,11,12);\n"
        "  float2x4 d = mul(a, c);\n"
        "  float2 leftVector = mul(a, float3(1,2,3));\n"
        "  float2 rightVector = mul(float3(1,2,3), b);\n"
        "  return float4(leftVector, rightVector);\n"
        "}\n";

    Allocator allocator;
    HLSLTree* tree = ParseSource(source, allocator);
    TASK_CHECK(tree != NULL);

    HLSLExpression* d = FindAssignment(tree, "d");
    TASK_CHECK(d != NULL && d->nodeType == HLSLNodeType_FunctionCall);
    TASK_CHECK(d->expressionType.baseType == HLSLBaseType_Float2x4);

    HLSLExpression* leftVector = FindAssignment(tree, "leftVector");
    TASK_CHECK(leftVector->expressionType.baseType == HLSLBaseType_Float2);

    HLSLExpression* rightVector = FindAssignment(tree, "rightVector");
    TASK_CHECK(rightVector->expressionType.baseType == HLSLBaseType_Float2);
    return 0;
}

int shape_transpose_swaps()
{
    static const char* source =
        "float4 main(float2 uv:TEXCOORD0):COLOR0 {\n"
        "  float2x4 a = float2x4(1,2,3,4,5,6,7,8);\n"
        "  float4x2 b = transpose(a);\n"
        "  float3x2 c = float3x2(1,2,3,4,5,6);\n"
        "  float2x3 d = transpose(c);\n"
        "  return float4(b[0], d[0].x);\n"
        "}\n";

    Allocator allocator;
    HLSLTree* tree = ParseSource(source, allocator);
    TASK_CHECK(tree != NULL);

    TASK_CHECK(FindAssignment(tree, "b")->expressionType.baseType == HLSLBaseType_Float4x2);
    TASK_CHECK(FindAssignment(tree, "d")->expressionType.baseType == HLSLBaseType_Float2x3);

    FirstIntrinsicVisitor visitor("transpose");
    visitor.VisitRoot(tree->GetRoot());
    TASK_CHECK(visitor.found != NULL);
    TASK_CHECK(visitor.found->expressionType.baseType == HLSLBaseType_Float4x2);
    return 0;
}

static bool RejectsWithLocation(const char* source)
{
    OutputCapture capture;
    Allocator allocator;
    HLSLTree* tree = ParseSource(source, allocator);
    const std::string message = capture.Read();
    return tree == NULL &&
           message.find(kFile) != std::string::npos &&
           message.find("(1)") != std::string::npos;
}

int shape_constructor_validation()
{
    static const char* tooFew =
        "float4 main(float2 uv:TEXCOORD0):COLOR0 { float2x3 m = float2x3(1,2,3,4,5); return float4(m[0],1); }";
    static const char* tooMany =
        "float4 main(float2 uv:TEXCOORD0):COLOR0 { float2x3 m = float2x3(1,2,3,4,5,6,7); return float4(m[0],1); }";
    static const char* badVector =
        "float4 main(float2 uv:TEXCOORD0):COLOR0 { float2x3 m = float2x3(float4(1,2,3,4), float3(1,2,3)); return float4(m[0],1); }";

    TASK_CHECK(RejectsWithLocation(tooFew));
    TASK_CHECK(RejectsWithLocation(tooMany));
    TASK_CHECK(RejectsWithLocation(badVector));
    return 0;
}

int shape_mul_validation()
{
    static const char* vectorMismatch =
        "float4 main(float2 uv:TEXCOORD0):COLOR0 { return float4(mul(float3(1,2,3), float2x3(1,2,3,4,5,6)),1); }";
    static const char* matrixMismatch =
        "float4 main(float2 uv:TEXCOORD0):COLOR0 { return float4(mul(float2x3(1,2,3,4,5,6), float2x4(1,2,3,4,5,6,7,8)),1); }";
    static const char* noTruncation =
        "float4 main(float2 uv:TEXCOORD0):COLOR0 { float3x3 m; float2x3 n; n = m; return float4(n[0],1); }";

    TASK_CHECK(RejectsWithLocation(vectorMismatch));
    TASK_CHECK(RejectsWithLocation(matrixMismatch));
    TASK_CHECK(RejectsWithLocation(noTruncation));
    return 0;
}

int shape_row_index_validation()
{
    static const char* sideEffect =
        "float4 main(float2 uv:TEXCOORD0):COLOR0 { float2x3 m; int i = 0; return float4(m[i++], 1); }";
    static const char* outOfRange =
        "float4 main(float2 uv:TEXCOORD0):COLOR0 { float2x3 m; return float4(m[2], 1); }";

    TASK_CHECK(RejectsWithLocation(sideEffect));
    TASK_CHECK(RejectsWithLocation(outOfRange));
    return 0;
}

// ----------------------------------------------------------------------------
// algebra
// ----------------------------------------------------------------------------

const char* kAlgebraShader =
    "float4 main(float2 uv:TEXCOORD0):COLOR0 {\n"
    "  float2x3 a = float2x3(1, 2, 3, 4, 5, 6);\n"
    "  float3x4 b = float3x4(7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18);\n"
    "  float4x2 c = float4x2(19, 20, 21, 22, 23, 24, 25, 26);\n"
    "  float3 rowA1 = a[1];\n"
    "  float2x3 at = transpose(transpose(a));\n"
    "  float3x2 t = transpose(a);\n"
    "  float2 mv = mul(a, float3(1, 2, 3));\n"
    "  float2 vm = mul(float3(1, 2, 3), t);\n"
    "  float2x4 ab = mul(a, b);\n"
    "  float2x2 abc = mul(mul(a, b), c);\n"
    "  float2 chain = mul(mul(a, b), float4(1, 1, 1, 1));\n"
    "  return float4(mv, vm);\n"
    "}\n";

int CheckAlgebraShader(unsigned flags)
{
    std::string glsl;
    bool parseOk = false;
    TASK_CHECK(TranslateGLSL(kAlgebraShader, flags, glsl, parseOk) && parseOk);
    TASK_CHECK(glsl.find("mat3x2") != std::string::npos);
    TASK_CHECK(glsl.find("mat4x3") != std::string::npos);
    TASK_CHECK(glsl.find("mat2x4") != std::string::npos);
    if (flags & GLSLGenerator::Flag_LowerMatrixMultiplication)
    {
        TASK_CHECK(glsl.find("matrix_mul") != std::string::npos);
    }

    Allocator allocator;
    HLSLTree* tree = ParseSource(kAlgebraShader, allocator);
    TASK_CHECK(tree != NULL);

    Evaluator evaluator;
    BuildMainEnvironment(tree, evaluator);
    Value a = evaluator.Evaluate(FindAssignment(tree, "a"));
    float aValues[6] = { 1, 2, 3, 4, 5, 6 };
    RefValue refA = RefRowMatrix(2, 3, aValues);
    TASK_CHECK(ValuesMatch(refA, ToReference(a)));

    // Row read.
    Value rowA1 = evaluator.Evaluate(FindAssignment(tree, "rowA1"));
    float expectedRow[3] = { 4, 5, 6 };
    TASK_CHECK(ValuesMatch(MakeRefVector(3, expectedRow), ToReference(rowA1)));

    // Double transpose is the identity and storage survives the round trip.
    Value at = evaluator.Evaluate(FindAssignment(tree, "at"));
    TASK_CHECK(ValuesMatch(refA, ToReference(at)));

    // Single transpose swaps the dimensions.
    Value t = evaluator.Evaluate(FindAssignment(tree, "t"));
    RefValue refT = RefTranspose(refA);
    TASK_CHECK(t.rows == 3 && t.cols == 2);
    TASK_CHECK(ValuesMatch(refT, ToReference(t)));

    // The transposed column-major storage must really carry swapped element positions.
    TASK_CHECK(CloseEnough(t.Get(0, 0), 1.0f) && CloseEnough(t.Get(2, 1), 6.0f));
    TASK_CHECK(CloseEnough(t.Get(1, 0), 2.0f) && CloseEnough(t.Get(0, 1), 4.0f));

    // Matrix * vector.
    float vector3[3] = { 1, 2, 3 };
    RefValue refVector3 = MakeRefVector(3, vector3);
    Value mv = evaluator.Evaluate(FindAssignment(tree, "mv"));
    TASK_CHECK(ValuesMatch(RefMul(refA, refVector3), ToReference(mv)));

    // Vector * matrix.
    Value vm = evaluator.Evaluate(FindAssignment(tree, "vm"));
    TASK_CHECK(ValuesMatch(RefMul(refVector3, refT), ToReference(vm)));

    // Matrix * matrix.
    float bValues[12] = { 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18 };
    RefValue refB = RefRowMatrix(3, 4, bValues);
    Value ab = evaluator.Evaluate(FindAssignment(tree, "ab"));
    TASK_CHECK(ab.rows == 2 && ab.cols == 4);
    TASK_CHECK(ValuesMatch(RefMul(refA, refB), ToReference(ab)));

    // Chained matrix multiplication.
    float cValues[8] = { 19, 20, 21, 22, 23, 24, 25, 26 };
    RefValue refC = RefRowMatrix(4, 2, cValues);
    Value abc = evaluator.Evaluate(FindAssignment(tree, "abc"));
    TASK_CHECK(abc.rows == 2 && abc.cols == 2);
    TASK_CHECK(ValuesMatch(RefMul(RefMul(refA, refB), refC), ToReference(abc)));

    // Matrix chain ending in a vector.
    float vector4[4] = { 1, 1, 1, 1 };
    Value chain = evaluator.Evaluate(FindAssignment(tree, "chain"));
    TASK_CHECK(ValuesMatch(RefMul(RefMul(refA, refB), MakeRefVector(4, vector4)), ToReference(chain)));
    return 0;
}

int algebra_normal_path()
{
    TASK_CHECK(CheckAlgebraShader(0) == 0);
    return 0;
}

int algebra_lowered_path()
{
    TASK_CHECK(CheckAlgebraShader(GLSLGenerator::Flag_LowerMatrixMultiplication) == 0);
    return 0;
}

int algebra_distinct_values_preserve_order()
{
    static const char* source =
        "float4 main(float2 uv:TEXCOORD0):COLOR0 {\n"
        "  float2x3 a = float2x3(1, 2, 3, 4, 5, 6);\n"
        "  float3x2 t = transpose(a);\n"
        "  float2x3 back = transpose(t);\n"
        "  return float4(back[0], back[1][2]);\n"
        "}\n";

    Allocator allocator;
    HLSLTree* tree = ParseSource(source, allocator);
    TASK_CHECK(tree != NULL);
    Evaluator evaluator;
    BuildMainEnvironment(tree, evaluator);
    Value back = evaluator.Evaluate(FindAssignment(tree, "back"));

    // Asymmetric data must keep its exact row/column positions after a transpose round trip.
    TASK_CHECK(CloseEnough(back.Get(0, 0), 1.0f));
    TASK_CHECK(CloseEnough(back.Get(0, 1), 2.0f));
    TASK_CHECK(CloseEnough(back.Get(0, 2), 3.0f));
    TASK_CHECK(CloseEnough(back.Get(1, 0), 4.0f));
    TASK_CHECK(CloseEnough(back.Get(1, 1), 5.0f));
    TASK_CHECK(CloseEnough(back.Get(1, 2), 6.0f));
    return 0;
}

// ----------------------------------------------------------------------------
// compat
// ----------------------------------------------------------------------------

int compat_legacy_square_shader()
{
    static const char* source =
        "float4 main(float2 uv:TEXCOORD0):COLOR0 {\n"
        "  float3x3 m = float3x3(1,2,3,4,5,6,7,8,9);\n"
        "  return float4(mul(float3(1,2,3),m),1);\n"
        "}\n";

    std::string glsl;
    TASK_CHECK(TranslateGLSL(source, 0, glsl));
    TASK_CHECK(glsl.find("mat3") != std::string::npos);
    TASK_CHECK(glsl.find("void main") != std::string::npos);

    std::string lowered;
    TASK_CHECK(TranslateGLSL(source, GLSLGenerator::Flag_LowerMatrixMultiplication, lowered));
    TASK_CHECK(lowered.find("matrix_mul") != std::string::npos);
    return 0;
}

int compat_legacy_rectangular_shader()
{
    static const char* source =
        "float4 main(float2 uv:TEXCOORD0):COLOR0 { float4x3 m; return float4(mul(float4(1,2,3,4),m),1); }";

    std::string glsl;
    TASK_CHECK(TranslateGLSL(source, 0, glsl));
    TASK_CHECK(glsl.find("mat3x4 m;") != std::string::npos);
    TASK_CHECK(glsl.find("matrix_mul") != std::string::npos);
    return 0;
}

int compat_hlsl_round_trip()
{
    static const char* source =
        "float2x3 makeMatrix() { return float2x3(1,2,3,4,5,6); }\n"
        "float4 main(float2 uv:TEXCOORD0):COLOR0 {\n"
        "  float2x3 a = makeMatrix();\n"
        "  float3x2 t = transpose(a);\n"
        "  float2 result = mul(a, float3(1,2,3));\n"
        "  return float4(result, t[0].x, 1);\n"
        "}\n";

    Allocator allocator;
    HLSLTree* tree = ParseSource(source, allocator);
    TASK_CHECK(tree != NULL);

    // Legacy write-back is the D3D9 syntax this parser can ingest again.
    HLSLGenerator hlslGenerator;
    TASK_CHECK(hlslGenerator.Generate(tree, HLSLGenerator::Target_PixelShader, "main", true));
    const std::string written = hlslGenerator.GetResult();
    TASK_CHECK(written.find("float2x3") != std::string::npos);
    TASK_CHECK(written.find("float3x2") != std::string::npos);
    TASK_CHECK(written.find("transpose") != std::string::npos);
    TASK_CHECK(written.find("mul") != std::string::npos);

    // The written HLSL has to parse again and translate successfully.
    std::string glsl;
    TASK_CHECK(TranslateGLSL(written.c_str(), 0, glsl));
    TASK_CHECK(glsl.find("mat3x2") != std::string::npos);
    return 0;
}

int compat_repeated_generation()
{
    static const char* source =
        "float4 main(float2 uv:TEXCOORD0):COLOR0 {\n"
        "  float2x3 a = float2x3(1,2,3,4,5,6);\n"
        "  float3x2 t = transpose(a);\n"
        "  float2 result = mul(a, float3(1,2,3));\n"
        "  return float4(result, t[0].x);\n"
        "}\n";

    Allocator allocator;
    HLSLTree* tree = ParseSource(source, allocator);
    TASK_CHECK(tree != NULL);

    GLSLGenerator::Options options;
    options.flags = GLSLGenerator::Flag_LowerMatrixMultiplication;

    GLSLGenerator first;
    TASK_CHECK(first.Generate(tree, GLSLGenerator::Target_FragmentShader,
        GLSLGenerator::Version_140, "main", options));
    const std::string firstOutput = first.GetResult();

    GLSLGenerator second;
    TASK_CHECK(second.Generate(tree, GLSLGenerator::Target_FragmentShader,
        GLSLGenerator::Version_140, "main", options));
    TASK_CHECK(firstOutput == second.GetResult());

    // The tree's recorded types must be unchanged by generating output twice.
    TASK_CHECK(FindAssignment(tree, "a")->expressionType.baseType == HLSLBaseType_Float2x3);
    TASK_CHECK(FindAssignment(tree, "t")->expressionType.baseType == HLSLBaseType_Float3x2);
    return 0;
}

int compat_reject_unsupported_modes()
{
    static const char* source =
        "float4 main(float2 uv:TEXCOORD0):COLOR0 { float2x3 m; return float4(m[0],1); }";

    const unsigned rejectedFlags[2] = {
        GLSLGenerator::Flag_EmulateConstantBuffer,
        GLSLGenerator::Flag_PackMatrixRowMajor
    };
    for (int i = 0; i < 2; ++i)
    {
        Allocator allocator;
        HLSLTree* tree = ParseSource(source, allocator);
        TASK_CHECK(tree != NULL);
        GLSLGenerator generator;
        GLSLGenerator::Options options;
        options.flags = rejectedFlags[i];
        TASK_CHECK(!generator.Generate(tree, GLSLGenerator::Target_FragmentShader,
            GLSLGenerator::Version_140, "main", options));
    }
    return 0;
}

} // namespace

const struct task_case task_cases[] =
{
    { "shape", "shape_locals_and_containers", shape_locals_and_containers },
    { "shape", "shape_mul_dimensions",        shape_mul_dimensions },
    { "shape", "shape_transpose_swaps",       shape_transpose_swaps },
    { "shape", "shape_constructor_validation", shape_constructor_validation },
    { "shape", "shape_mul_validation",        shape_mul_validation },
    { "shape", "shape_row_index_validation",  shape_row_index_validation },

    { "algebra", "algebra_normal_path",                  algebra_normal_path },
    { "algebra", "algebra_lowered_path",                 algebra_lowered_path },
    { "algebra", "algebra_distinct_values_preserve_order", algebra_distinct_values_preserve_order },

    { "compat", "compat_legacy_square_shader",      compat_legacy_square_shader },
    { "compat", "compat_legacy_rectangular_shader", compat_legacy_rectangular_shader },
    { "compat", "compat_hlsl_round_trip",           compat_hlsl_round_trip },
    { "compat", "compat_repeated_generation",       compat_repeated_generation },
    { "compat", "compat_reject_unsupported_modes",  compat_reject_unsupported_modes },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
