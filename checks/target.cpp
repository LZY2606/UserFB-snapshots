//=============================================================================
//
// target.cpp
//
// Unit tests for the optional derivative uniformity analysis and its GLSL
// generator integration. Tests parse synthetic HLSL, run the real analysis
// and the real generator; no external shader compiler or GPU is involved.
//
//=============================================================================

#include "suite.h"

#include "HLSLParser.h"
#include "HLSLTree.h"
#include "DerivativeUniformity.h"
#include "GLSLGenerator.h"

#include <string>
#include <vector>

using namespace M4;

namespace
{

int g_count;

// Every counted check goes through this macro so the returned count matches
// the number of assertions the fixed driver observed.
#define CK(condition)                              \
    do {                                            \
        UNIT_CHECK(condition);                      \
        ++g_count;                                  \
    } while (0)

bool ParseShader(HLSLTree* tree, Allocator* allocator, const char* source)
{
    HLSLParser parser(allocator, "target.hlsl", source, std::string(source).size());
    return parser.Parse(tree);
}

const DerivativeDiagnostic* FindDiagnostic(const std::vector<DerivativeDiagnostic>& diagnostics,
                                           int line)
{
    for (size_t i = 0; i < diagnostics.size(); ++i)
    {
        if (diagnostics[i].line == line)
        {
            return &diagnostics[i];
        }
    }
    return NULL;
}

bool ChainEquals(const DerivativeDiagnostic& diagnostic,
                 const char* const* expected, size_t expectedCount)
{
    if (diagnostic.callChain.size() != expectedCount)
    {
        return false;
    }
    for (size_t i = 0; i < expectedCount; ++i)
    {
        if (!String_Equal(diagnostic.callChain[i], expected[i]))
        {
            return false;
        }
    }
    return true;
}

int RunFlowGroup()
{
    g_count = 0;

    // 1) A normal if reconverges: the derivative after the join runs in
    // uniform control flow even though both branches wrote the variable.
    {
        const char* source =
            "float4 main(float2 uv:TEXCOORD0):COLOR0 {\n"       // line 1
            "  float x = 0.0;\n"                                // line 2
            "  if (uv.x > 0.0) { x = 1.0; } else { x = 1.0; }\n" // line 3
            "  return float4(ddx(x),0,0,1);\n"                  // line 4
            "}\n";                                              // line 5
        Allocator allocator;
        HLSLTree tree(&allocator);
        CK(ParseShader(&tree, &allocator, source));
        std::vector<DerivativeDiagnostic> diagnostics;
        CK(ValidateDerivativeUniformity(&tree, "main", diagnostics));
        CK(diagnostics.empty());
    }

    // 2) A divergent if which returns early means the code after it cannot be
    // assumed to run for every fragment: the later ddx is risky.
    {
        const char* source =
            "float4 main(float2 uv:TEXCOORD0):COLOR0 {\n"       // line 1
            "  if (uv.x > 0.0) { return float4(0,0,0,1); }\n"   // line 2
            "  return float4(ddx(uv.y),0,0,1);\n"               // line 3
            "}\n";                                              // line 4
        Allocator allocator;
        HLSLTree tree(&allocator);
        CK(ParseShader(&tree, &allocator, source));
        std::vector<DerivativeDiagnostic> diagnostics;
        CK(!ValidateDerivativeUniformity(&tree, "main", diagnostics));
        CK(diagnostics.size() == 1);
        const DerivativeDiagnostic* diagnostic = FindDiagnostic(diagnostics, 3);
        CK(diagnostic != NULL);
        if (diagnostic != NULL)
        {
            CK(diagnostic->type == DerivativeDiagnostic_DivergentOperation);
            CK(diagnostic->reasonLine == 2);
            const char* chain[] = { "main" };
            CK(ChainEquals(*diagnostic, chain, 1));
        }
    }

    // 3) A variable assigned inside a divergent loop carried branch is varying
    // at the loop exit, so a condition built from it is divergent.
    {
        const char* source =
            "float4 main(float2 uv:TEXCOORD0):COLOR0 {\n"       // line 1
            "  float x = 1.0;\n"                                // line 2
            "  for (int i = 0; i < 3; i++) {\n"                 // line 3
            "    if (uv.x > 0.0) { x = 2.0; }\n"                // line 4
            "  }\n"                                             // line 5
            "  if (x > 0.0) { return float4(ddx(uv.y),0,0,1); }\n" // line 6
            "  return float4(0,0,0,1);\n"                       // line 7
            "}\n";                                              // line 8
        Allocator allocator;
        HLSLTree tree(&allocator);
        CK(ParseShader(&tree, &allocator, source));
        std::vector<DerivativeDiagnostic> diagnostics;
        CK(!ValidateDerivativeUniformity(&tree, "main", diagnostics));
        const DerivativeDiagnostic* diagnostic = FindDiagnostic(diagnostics, 6);
        CK(diagnostic != NULL);
        if (diagnostic != NULL)
        {
            CK(diagnostic->reasonLine == 6);
            const char* chain[] = { "main" };
            CK(ChainEquals(*diagnostic, chain, 1));
        }
    }

    // 4) A break taken under a varying condition leaves the loop with
    // fragments on different paths; the derivative after the loop is risky.
    {
        const char* source =
            "float4 main(float2 uv:TEXCOORD0):COLOR0 {\n"       // line 1
            "  for (int i = 0; i < 3; i++) {\n"                 // line 2
            "    if (uv.x > 0.0) { break; }\n"                  // line 3
            "  }\n"                                             // line 4
            "  return float4(ddx(uv.y),0,0,1);\n"               // line 5
            "}\n";                                              // line 6
        Allocator allocator;
        HLSLTree tree(&allocator);
        CK(ParseShader(&tree, &allocator, source));
        std::vector<DerivativeDiagnostic> diagnostics;
        CK(!ValidateDerivativeUniformity(&tree, "main", diagnostics));
        const DerivativeDiagnostic* diagnostic = FindDiagnostic(diagnostics, 5);
        CK(diagnostic != NULL);
        if (diagnostic != NULL)
        {
            // The reason is the inner divergent if, not the loop.
            CK(diagnostic->reasonLine == 3);
        }
    }

    // 5) The same break on a uniform loop condition reconverges normally.
    {
        const char* source =
            "float4 main(float2 uv:TEXCOORD0):COLOR0 {\n"       // line 1
            "  for (int i = 0; i < 3; i++) {\n"                 // line 2
            "    if (i > 0) { break; }\n"                       // line 3
            "  }\n"                                             // line 4
            "  return float4(ddx(uv.y),0,0,1);\n"               // line 5
            "}\n";                                              // line 6
        Allocator allocator;
        HLSLTree tree(&allocator);
        CK(ParseShader(&tree, &allocator, source));
        std::vector<DerivativeDiagnostic> diagnostics;
        CK(ValidateDerivativeUniformity(&tree, "main", diagnostics));
        CK(diagnostics.empty());
    }

    // 6) A literal constant condition prunes the dead branch without any
    // algebraic equivalence proving.
    {
        const char* source =
            "float4 main(float2 uv:TEXCOORD0):COLOR0 {\n"       // line 1
            "  float x = 0.0;\n"                                // line 2
            "  if (true) { x = ddx(uv.y); }\n"                  // line 3
            "  return float4(x,0,0,1);\n"                       // line 4
            "}\n";                                              // line 5
        Allocator allocator;
        HLSLTree tree(&allocator);
        CK(ParseShader(&tree, &allocator, source));
        std::vector<DerivativeDiagnostic> diagnostics;
        CK(ValidateDerivativeUniformity(&tree, "main", diagnostics));
        CK(diagnostics.empty());
    }

    // 7) discard on a varying condition is an early exit as well.
    {
        const char* source =
            "float4 main(float2 uv:TEXCOORD0):COLOR0 {\n"       // line 1
            "  if (uv.x > 0.0) { discard; }\n"                  // line 2
            "  return float4(ddx(uv.y),0,0,1);\n"               // line 3
            "}\n";                                              // line 4
        Allocator allocator;
        HLSLTree tree(&allocator);
        CK(ParseShader(&tree, &allocator, source));
        std::vector<DerivativeDiagnostic> diagnostics;
        CK(!ValidateDerivativeUniformity(&tree, "main", diagnostics));
        const DerivativeDiagnostic* diagnostic = FindDiagnostic(diagnostics, 3);
        CK(diagnostic != NULL);
        if (diagnostic != NULL)
        {
            CK(diagnostic->reasonLine == 2);
        }
    }

    return g_count;
}

int RunCallsGroup()
{
    g_count = 0;

    // 1) The same helper is called on both a uniform and a varying path: only
    // the varying call chain is reported, with the full function chain.
    {
        const char* source =
            "sampler2D tex;\n"                                  // line 1
            "float g;\n"                                        // line 2
            "float helper(float2 uv) {\n"                       // line 3
            "  return tex2D(tex, uv).x;\n"                      // line 4
            "}\n"                                               // line 5
            "float4 main(float2 uv:TEXCOORD0):COLOR0 {\n"       // line 6
            "  float a = 0.0;\n"                                // line 7
            "  if (g > 0.0) { a = helper(uv); }\n"              // line 8
            "  if (uv.x > 0.0) { a = helper(uv); }\n"           // line 9
            "  return float4(a,0,0,1);\n"                       // line 10
            "}\n";                                              // line 11
        Allocator allocator;
        HLSLTree tree(&allocator);
        CK(ParseShader(&tree, &allocator, source));
        std::vector<DerivativeDiagnostic> diagnostics;
        CK(!ValidateDerivativeUniformity(&tree, "main", diagnostics));
        CK(diagnostics.size() == 1);
        const DerivativeDiagnostic* diagnostic = FindDiagnostic(diagnostics, 4);
        CK(diagnostic != NULL);
        if (diagnostic != NULL)
        {
            CK(diagnostic->reasonLine == 9);
            const char* chain[] = { "main", "helper" };
            CK(ChainEquals(*diagnostic, chain, 2));
        }
    }

    // 2) Overloaded helpers resolve to their actual bound functions; each
    // risky overload produces its own report with its own source line.
    {
        const char* source =
            "sampler2D tex;\n"                                  // line 1
            "float pick(float v) { return ddx(v); }\n"          // line 2
            "float pick(float2 v) { return tex2D(tex, v).x; }\n" // line 3
            "float4 main(float2 uv:TEXCOORD0):COLOR0 {\n"       // line 4
            "  float a = 0.0;\n"                                // line 5
            "  if (uv.x > 0.0) { a = pick(uv.x); }\n"           // line 6
            "  if (uv.y > 0.0) { a += pick(uv); }\n"            // line 7
            "  return float4(a,0,0,1);\n"                       // line 8
            "}\n";                                              // line 9
        Allocator allocator;
        HLSLTree tree(&allocator);
        CK(ParseShader(&tree, &allocator, source));
        std::vector<DerivativeDiagnostic> diagnostics;
        CK(!ValidateDerivativeUniformity(&tree, "main", diagnostics));
        CK(diagnostics.size() == 2);
        const DerivativeDiagnostic* scalarCall = FindDiagnostic(diagnostics, 2);
        const DerivativeDiagnostic* vectorCall = FindDiagnostic(diagnostics, 3);
        CK(scalarCall != NULL);
        CK(vectorCall != NULL);
        if (scalarCall != NULL)
        {
            CK(scalarCall->reasonLine == 6);
            const char* chain[] = { "main", "pick" };
            CK(ChainEquals(*scalarCall, chain, 2));
        }
        if (vectorCall != NULL)
        {
            CK(vectorCall->reasonLine == 7);
            const char* chain[] = { "main", "pick" };
            CK(ChainEquals(*vectorCall, chain, 2));
        }
    }

    // 3) Multi-layer non-recursive helpers carry the complete call chain and
    // the caller's control dependence.
    {
        const char* source =
            "float inner(float v) { return ddx(v); }\n"         // line 1
            "float middle(float v) { return inner(v); }\n"      // line 2
            "float outer(float v) { return middle(v); }\n"      // line 3
            "float4 main(float2 uv:TEXCOORD0):COLOR0 {\n"       // line 4
            "  if (uv.x > 0.0) { return float4(outer(uv.y),0,0,1); }\n" // line 5
            "  return float4(0,0,0,1);\n"                       // line 6
            "}\n";                                              // line 7
        Allocator allocator;
        HLSLTree tree(&allocator);
        CK(ParseShader(&tree, &allocator, source));
        std::vector<DerivativeDiagnostic> diagnostics;
        CK(!ValidateDerivativeUniformity(&tree, "main", diagnostics));
        CK(diagnostics.size() == 1);
        const DerivativeDiagnostic* diagnostic = FindDiagnostic(diagnostics, 1);
        CK(diagnostic != NULL);
        if (diagnostic != NULL)
        {
            CK(diagnostic->reasonLine == 5);
            const char* chain[] = { "main", "outer", "middle", "inner" };
            CK(ChainEquals(*diagnostic, chain, 4));
        }
    }

    // 4) Functions never reached from the entry point are not analyzed.
    {
        const char* source =
            "float unused() { return ddx(1.0); }\n"             // line 1
            "float4 main(float2 uv:TEXCOORD0):COLOR0 {\n"       // line 2
            "  return float4(uv,0,1);\n"                        // line 3
            "}\n";                                              // line 4
        Allocator allocator;
        HLSLTree tree(&allocator);
        CK(ParseShader(&tree, &allocator, source));
        std::vector<DerivativeDiagnostic> diagnostics;
        CK(ValidateDerivativeUniformity(&tree, "main", diagnostics));
        CK(diagnostics.empty());
    }

    // 5) Recursion is reported explicitly as unsupported, never silently
    // allowed through.
    {
        const char* source =
            "float rec(int i) {\n"                              // line 1
            "  if (i > 0) { return rec(i-1); }\n"               // line 2
            "  return ddx(1.0);\n"                              // line 3
            "}\n"                                               // line 4
            "float4 main(float2 uv:TEXCOORD0):COLOR0 {\n"       // line 5
            "  return float4(rec(2),0,0,1);\n"                  // line 6
            "}\n";                                              // line 7
        Allocator allocator;
        HLSLTree tree(&allocator);
        CK(ParseShader(&tree, &allocator, source));
        std::vector<DerivativeDiagnostic> diagnostics;
        CK(!ValidateDerivativeUniformity(&tree, "main", diagnostics));
        CK(!diagnostics.empty());
        bool sawUnsupported = false;
        for (size_t i = 0; i < diagnostics.size(); ++i)
        {
            sawUnsupported = sawUnsupported ||
                             diagnostics[i].type == DerivativeDiagnostic_Unsupported;
        }
        CK(sawUnsupported);
    }

    // 6) tex2Dlod and tex2Dgrad are not derivative operations themselves, but
    // their values still propagate as varying dependence.
    {
        const char* source =
            "sampler2D tex;\n"                                  // line 1
            "float4 main(float2 uv:TEXCOORD0):COLOR0 {\n"       // line 2
            "  float4 a = tex2Dlod(tex, float4(uv,0,0));\n"     // line 3
            "  float4 b = tex2Dgrad(tex, uv, float2(1,0), float2(0,1));\n" // line 4
            "  if (a.x > 0.0) { return float4(ddx(b.y),0,0,1); }\n" // line 5
            "  return float4(0,0,0,1);\n"                       // line 6
            "}\n";                                              // line 7
        Allocator allocator;
        HLSLTree tree(&allocator);
        CK(ParseShader(&tree, &allocator, source));
        std::vector<DerivativeDiagnostic> diagnostics;
        CK(!ValidateDerivativeUniformity(&tree, "main", diagnostics));
        const DerivativeDiagnostic* diagnostic = FindDiagnostic(diagnostics, 5);
        CK(diagnostic != NULL);
        if (diagnostic != NULL)
        {
            CK(diagnostic->reasonLine == 5);
        }
    }

    return g_count;
}

int RunCompatGroup()
{
    g_count = 0;

    static const char* riskySource =
        "float4 main(float2 uv:TEXCOORD0):COLOR0 {\n"
        "  float x=0.0;\n"
        "  if(uv.x>0.0) x=ddx(uv.y);\n"
        "  return float4(x,0.0,0.0,1.0);\n"
        "}\n";

    static const char* safeSource =
        "float4 main(float2 uv:TEXCOORD0):COLOR0 {\n"
        "  return float4(ddx(uv.y),0.0,0.0,1.0);\n"
        "}\n";

    // 1) With the default options the historical GLSL output is produced
    // verbatim, including the dFdx inside the if (the baseline path).
    {
        Allocator allocator;
        HLSLTree tree(&allocator);
        CK(ParseShader(&tree, &allocator, riskySource));

        GLSLGenerator generator;
        CK(generator.Generate(&tree, GLSLGenerator::Target_FragmentShader,
                              GLSLGenerator::Version_140, "main"));
        const std::string before(generator.GetResult());
        CK(before.find("dFdx") != std::string::npos);
        CK(before.find("if") != std::string::npos);

        // Running the analysis, even a failing one, must not mutate the tree:
        // default generation afterwards is byte identical.
        std::vector<DerivativeDiagnostic> diagnostics;
        CK(!ValidateDerivativeUniformity(&tree, "main", diagnostics));

        GLSLGenerator generator2;
        CK(generator2.Generate(&tree, GLSLGenerator::Target_FragmentShader,
                               GLSLGenerator::Version_140, "main"));
        CK(before == std::string(generator2.GetResult()));
    }

    // 2) Enabling the check rejects the risky shader before any output and
    // never leaves a half-written shader behind.
    {
        Allocator allocator;
        HLSLTree tree(&allocator);
        CK(ParseShader(&tree, &allocator, riskySource));

        GLSLGenerator generator;
        GLSLGenerator::Options options;
        options.checkDerivativeUniformity = true;
        CK(!generator.Generate(&tree, GLSLGenerator::Target_FragmentShader,
                               GLSLGenerator::Version_140, "main", options));
        CK(generator.GetResult() != NULL);
        CK(generator.GetResult()[0] == '\0');
    }

    // 3) A clean shader still generates normally with the check enabled.
    {
        Allocator allocator;
        HLSLTree tree(&allocator);
        CK(ParseShader(&tree, &allocator, safeSource));

        GLSLGenerator generator;
        GLSLGenerator::Options options;
        options.checkDerivativeUniformity = true;
        CK(generator.Generate(&tree, GLSLGenerator::Target_FragmentShader,
                              GLSLGenerator::Version_140, "main", options));
        CK(std::string(generator.GetResult()).find("dFdx") != std::string::npos);
    }

    // 4) Repeated analysis and different entry points give stable reports.
    {
        Allocator allocator;
        HLSLTree tree(&allocator);
        CK(ParseShader(&tree, &allocator, riskySource));

        std::vector<DerivativeDiagnostic> first;
        std::vector<DerivativeDiagnostic> second;
        CK(!ValidateDerivativeUniformity(&tree, "main", first));
        CK(!ValidateDerivativeUniformity(&tree, "main", second));
        CK(first.size() == second.size());
        CK(first.size() == 1);
        if (first.size() == 1 && second.size() == 1)
        {
            CK(first[0].line == second[0].line);
            CK(first[0].reasonLine == second[0].reasonLine);
            CK(first[0].callChain.size() == second[0].callChain.size());
        }
    }

    // 5) A missing entry point fails cleanly with an explicit diagnostic.
    {
        Allocator allocator;
        HLSLTree tree(&allocator);
        CK(ParseShader(&tree, &allocator, safeSource));

        std::vector<DerivativeDiagnostic> diagnostics;
        CK(!ValidateDerivativeUniformity(&tree, "missing", diagnostics));
        CK(!diagnostics.empty());
        CK(diagnostics[0].type == DerivativeDiagnostic_Unsupported);
    }

    // 6) The check defaults to off in the options structure.
    {
        GLSLGenerator::Options options;
        CK(!options.checkDerivativeUniformity);
    }

    return g_count;
}

} // anonymous namespace

// The fixed driver (checks/target_main.c) is built with the C++ driver, so
// this uses ordinary C++ linkage.
int unit_run_group(const char* group)
{
    if (String_Equal(group, "flow"))
    {
        return RunFlowGroup();
    }
    if (String_Equal(group, "calls"))
    {
        return RunCallsGroup();
    }
    if (String_Equal(group, "compat"))
    {
        return RunCompatGroup();
    }
    return 0;
}
