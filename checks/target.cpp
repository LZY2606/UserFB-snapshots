//=============================================================================
//
// checks/target.cpp
//
// Tests for the backend independent recursive (flattened) entry interface
// plan and its GLSL/MSL marshalling. Everything goes through the real parser
// and generators; no generator is simulated.
//
//=============================================================================

#include "task_support/check.h"

#include "HLSLParser.h"
#include "GLSLGenerator.h"
#include "MSLGenerator.h"
#include "EntryInterface.h"

#include <string>
#include <string.h>
#include <stdio.h>

using namespace M4;

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

bool ParseTree(HLSLTree& tree, Allocator& allocator, const char* source)
{
    HLSLParser parser(&allocator, "target.hlsl", source, strlen(source));
    return parser.Parse(&tree);
}

bool Contains(const char* haystack, const char* needle)
{
    return haystack != NULL && strstr(haystack, needle) != NULL;
}

int CountNeedle(const char* haystack, const char* needle)
{
    int count = 0;
    size_t length = strlen(needle);
    const char* p = haystack;
    while ((p = strstr(p, needle)) != NULL)
    {
        ++count;
        p += length;
    }
    return count;
}

int FindLeaf(const EntryInterfacePlan& plan, const char* path)
{
    for (int i = 0; i < plan.GetNumLeaves(); ++i)
    {
        if (plan.GetLeaf(i).path == path)
            return i;
    }
    return -1;
}

int AttrIndex(const char* name, unsigned int index)
{
    (void)name;
    return static_cast<int>(index);
}

int AttrPlusThree(const char* name, unsigned int index)
{
    (void)name;
    return static_cast<int>(index) + 3;
}

int AttrReject(const char* name, unsigned int index)
{
    (void)name;
    (void)index;
    return -1;
}

const char* kNestedVs =
    "struct Leaf { float2 uv:TEXCOORD5; float4 extra:TEXCOORD6; };\n"
    "struct Nested { float2 uv:TEXCOORD7; float4 extra:TEXCOORD8; };\n"
    "struct Inputs { float4 pos:POSITION; Nested data; };\n"
    "struct VOut { float4 pos:SV_Position; float3 tint:COLOR0; float2 vary:TEXCOORD3; };\n"
    "VOut helper(VOut v) { v.vary = v.vary + 1.0; return v; }\n"
    "VOut main(Inputs input, inout Leaf side) {\n"
    "  VOut o; o.pos=input.pos; o.tint=input.data.uv.xyx; o.vary=side.uv;\n"
    "  side.uv=input.data.uv; return helper(o); }";

const char* kNestedFs =
    "struct FIn { float4 pos:SV_Position; float2 uv:TEXCOORD0; };\n"
    "struct FOut { float4 c1:COLOR1; float4 c0:SV_Target0; float d:SV_Depth; };\n"
    "FOut main(FIn i, inout float3 feed:COLOR2) {\n"
    "  FOut o; o.c1=float4(i.uv,0,1); o.c0=float4(1); o.d=i.pos.z; feed=feed+1.0; return o; }";

// ===========================================================================
// flatten group: the backend independent plan
// ===========================================================================

int flatten_nested_plan()
{
    Allocator allocator;
    HLSLTree tree(&allocator);
    TASK_CHECK(ParseTree(tree, allocator, kNestedVs));
    HLSLFunction* entry = tree.FindFunction("main");
    TASK_CHECK(entry != NULL);

    EntryInterfacePlan plan;
    std::string error;
    TASK_CHECK(plan.Build(&tree, entry, EntryTarget_VertexShader, /*glslLegacy=*/false,
                          AttrIndex, error));
    TASK_CHECK(error.empty());

    // 5 input leaves (POSITION, 2 nested, 2 inout) and 5 output leaves
    // (2 inout, position, color, custom varying).
    TASK_CHECK(plan.GetNumLeaves() == 10);
    TASK_CHECK(plan.GetNumInputLeaves() == 5);
    TASK_CHECK(plan.GetNumOutputLeaves() == 5);
    TASK_CHECK(plan.HasPositionOutput());

    int idx = FindLeaf(plan, "input.data.uv");
    TASK_CHECK(idx >= 0);
    const EntryInterfaceLeaf& leaf = plan.GetLeaf(idx);
    TASK_CHECK(leaf.direction == EntryDirection_Input);
    TASK_CHECK(leaf.baseType == HLSLBaseType_Float2);
    TASK_CHECK(strcmp(leaf.semantic, "TEXCOORD7") == 0);
    TASK_CHECK(leaf.memberChain == ".data.uv");
    TASK_CHECK(leaf.argumentIndex == 0);
    TASK_CHECK(!leaf.isReturn);
    TASK_CHECK(leaf.glslTarget == "TEXCOORD7");
    TASK_CHECK(leaf.mslTarget == "attribute(7)");
    TASK_CHECK(!leaf.builtIn);

    idx = FindLeaf(plan, "side.uv");
    TASK_CHECK(idx >= 0);
    const EntryInterfaceLeaf& sideIn = plan.GetLeaf(idx);
    TASK_CHECK(sideIn.direction == EntryDirection_Input);
    TASK_CHECK(sideIn.glslTarget == "TEXCOORD5");

    // inout appears once in each direction.
    int sideInCount = 0, sideOutCount = 0;
    for (int i = 0; i < plan.GetNumLeaves(); ++i)
    {
        const EntryInterfaceLeaf& l = plan.GetLeaf(i);
        if (l.path == "side.uv" || l.path == "side.extra")
        {
            if (l.direction == EntryDirection_Input) ++sideInCount;
            if (l.direction == EntryDirection_Output) ++sideOutCount;
        }
    }
    TASK_CHECK(sideInCount == 2 && sideOutCount == 2);

    idx = FindLeaf(plan, "@return.pos");
    TASK_CHECK(idx >= 0);
    const EntryInterfaceLeaf& pos = plan.GetLeaf(idx);
    TASK_CHECK(pos.direction == EntryDirection_Output);
    TASK_CHECK(pos.isReturn);
    TASK_CHECK(pos.builtIn);
    TASK_CHECK(pos.glslTarget == "gl_Position");
    TASK_CHECK(pos.mslTarget == "position");
    TASK_CHECK(pos.memberChain == ".pos");

    idx = FindLeaf(plan, "@return.tint");
    TASK_CHECK(idx >= 0);
    const EntryInterfaceLeaf& tint = plan.GetLeaf(idx);
    TASK_CHECK(tint.baseType == HLSLBaseType_Float3);
    TASK_CHECK(tint.glslTarget == "frag_COLOR0");
    TASK_CHECK(tint.mslTarget == "user(COLOR0)");

    // Stage struct names are unique identifiers derived from the path.
    std::string stageName = plan.GetLeaf(FindLeaf(plan, "input.pos")).stageName;
    TASK_CHECK(stageName == "input_pos");
    return 0;
}

int flatten_directions()
{
    // Vertex target so the custom (non-color) out/inout semantics are valid
    // outputs; this case is about import/export direction handling.
    static const char* source =
        "struct O { float4 p:POSITION; float4 c:COLOR0; };\n"
        "O main(float2 a:TEXCOORD0, in float2 b:TEXCOORD1, out float2 d:TEXCOORD2, inout float2 e:TEXCOORD3) {\n"
        "  d = a + b; O o; o.p = float4(e,0,1); o.c = float4(0); e = b; return o; }";

    Allocator allocator;
    HLSLTree tree(&allocator);
    TASK_CHECK(ParseTree(tree, allocator, source));

    EntryInterfacePlan plan;
    std::string error;
    TASK_CHECK(plan.Build(&tree, tree.FindFunction("main"), EntryTarget_VertexShader,
                          false, AttrIndex, error));

    // Plain and "in" parameters only import; "out" only exports; "inout" both.
    TASK_CHECK(FindLeaf(plan, "a") >= 0 && plan.GetLeaf(FindLeaf(plan, "a")).direction == EntryDirection_Input);
    TASK_CHECK(FindLeaf(plan, "b") >= 0 && plan.GetLeaf(FindLeaf(plan, "b")).direction == EntryDirection_Input);
    TASK_CHECK(FindLeaf(plan, "d") >= 0 && plan.GetLeaf(FindLeaf(plan, "d")).direction == EntryDirection_Output);

    int eIn = 0, eOut = 0;
    for (int i = 0; i < plan.GetNumLeaves(); ++i)
    {
        const EntryInterfaceLeaf& l = plan.GetLeaf(i);
        if (l.path == "e")
        {
            if (l.direction == EntryDirection_Input) ++eIn;
            if (l.direction == EntryDirection_Output) ++eOut;
        }
    }
    TASK_CHECK(eIn == 1 && eOut == 1);

    // The recursive return value is exported.
    int ret = FindLeaf(plan, "@return.c");
    TASK_CHECK(ret >= 0 && plan.GetLeaf(ret).direction == EntryDirection_Output);
    return 0;
}

int flatten_same_semantic_both_directions()
{
    static const char* source =
        "struct O { float4 c:COLOR0; };\n"
        "O main(float4 inColor:COLOR0) { O o; o.c = inColor; return o; }";

    Allocator allocator;
    HLSLTree tree(&allocator);
    TASK_CHECK(ParseTree(tree, allocator, source));

    EntryInterfacePlan plan;
    std::string error;
    // Same semantic on an input and an output is legal: directions differ.
    TASK_CHECK(plan.Build(&tree, tree.FindFunction("main"), EntryTarget_FragmentShader,
                          false, NULL, error));
    TASK_CHECK(error.empty());

    const EntryInterfaceLeaf& in = plan.GetLeaf(FindLeaf(plan, "inColor"));
    const EntryInterfaceLeaf& out = plan.GetLeaf(FindLeaf(plan, "@return.c"));
    TASK_CHECK(in.direction == EntryDirection_Input && out.direction == EntryDirection_Output);
    TASK_CHECK(strcmp(in.semantic, "COLOR0") == 0 && strcmp(out.semantic, "COLOR0") == 0);
    TASK_CHECK(in.glslTarget == "frag_COLOR0");
    TASK_CHECK(out.glslTarget == "rast_FragData" && out.colorSlot == 0);
    return 0;
}

int flatten_failures()
{
    struct Case { const char* name; const char* source; const char* pathFragment; };
    static const Case cases[] =
    {
        { "missing",
          "struct S { float4 p:POSITION; float2 q; }; void main(S s, out float4 c:COLOR0){c=s.p;}",
          "s.q" },
        { "container",
          "struct S { float4 p:POSITION; }; void main(in S s:TEXCOORD0, out float4 c:COLOR0){c=s.p;}",
          "s" },
        { "array",
          "void main(float2 a[2]:TEXCOORD0, out float4 c:COLOR0){c=float4(a[0],0,1);}",
          "a" },
        { "intLeaf",
          "struct S { int4 p:POSITION; }; void main(S s, out float4 c:COLOR0){c=float4(0);}",
          "s.p" },
        { "matrixLeaf",
          "struct S { float4x4 p:POSITION; }; void main(S s, out float4 c:COLOR0){c=float4(0);}",
          "s.p" },
        { "dupSemantic",
          "void main(float2 a:TEXCOORD0, float2 b:TEXCOORD0, out float4 c:COLOR0){c=float4(a+b,0,1);}",
          "b" },
        { "dupBuiltin",
          "struct O { float4 a:COLOR0; float4 b:SV_Target0; }; O main(float2 uv:TEXCOORD0){O o;o.a=float4(uv,0,1);o.b=o.a;return o;}",
          "@return.b" },
        { "dupReturnOut",
          "struct O { float4 c:COLOR0; }; O main(float2 uv:TEXCOORD0, out float4 c:COLOR0){c=float4(uv,0,1); O o;o.c=c;return o;}",
          "@return.c" },
        { "unknownFragmentOutput",
          "void main(float2 uv:TEXCOORD0, out float4 w:WEIRD){w=float4(uv,0,1);}",
          "w" },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        Allocator allocator;
        HLSLTree tree(&allocator);
        TASK_CHECK(ParseTree(tree, allocator, cases[i].source));

        EntryInterfacePlan plan;
        std::string error;
        bool ok = plan.Build(&tree, tree.FindFunction("main"), EntryTarget_FragmentShader,
                             false, NULL, error);
        TASK_CHECK(!ok);
        TASK_CHECK(!error.empty());
        TASK_CHECK(error.find(cases[i].pathFragment) != std::string::npos);
        TASK_CHECK(plan.GetNumLeaves() == 0);
    }
    return 0;
}

int flatten_resources_rejected()
{
    static const char* sampler =
        "sampler2D tex; float4 main(float2 uv:TEXCOORD0):COLOR0 { return tex2D(tex, uv); }";
    static const char* cbuffer =
        "cbuffer CB : register(b0) { float scale; }; float4 main(float2 uv:TEXCOORD0):COLOR0 { return float4(uv,0,scale); }";
    static const char* uniform =
        "float g_scale; float4 main(float2 uv:TEXCOORD0):COLOR0 { return float4(uv,0,g_scale); }";
    static const char* uniformArg =
        "void main(uniform float scale, out float4 c:COLOR0) { c = float4(scale); }";
    const char* sources[] = { sampler, cbuffer, uniform, uniformArg };

    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); ++i)
    {
        Allocator allocator;
        HLSLTree tree(&allocator);
        TASK_CHECK(ParseTree(tree, allocator, sources[i]));
        EntryInterfacePlan plan;
        std::string error;
        bool ok = plan.Build(&tree, tree.FindFunction("main"), EntryTarget_FragmentShader,
                             false, NULL, error);
        TASK_CHECK(!ok);
        TASK_CHECK(!error.empty());
    }
    return 0;
}

// ===========================================================================
// marshal group: generated GLSL/MSL marshalling code
// ===========================================================================

int marshal_glsl_vertex()
{
    Allocator allocator;
    HLSLTree tree(&allocator);
    TASK_CHECK(ParseTree(tree, allocator, kNestedVs));

    GLSLGenerator generator;
    GLSLGenerator::Options options;
    options.recursiveEntryInterfaces = true;
    TASK_CHECK(generator.Generate(&tree, GLSLGenerator::Target_VertexShader,
                                  GLSLGenerator::Version_140, "main", options));
    const char* out = generator.GetResult();
    TASK_CHECK(out != NULL && out[0] != 0);

    // Flat leaf interface declarations.
    TASK_CHECK(Contains(out, "in vec4 POSITION;"));
    TASK_CHECK(Contains(out, "in vec2 TEXCOORD7;"));
    TASK_CHECK(Contains(out, "out vec2 frag_TEXCOORD5;"));
    TASK_CHECK(Contains(out, "out vec3 frag_COLOR0;"));
    TASK_CHECK(Contains(out, "out vec2 frag_TEXCOORD3;"));

    // The original nested structures and helper function are retained.
    TASK_CHECK(Contains(out, "struct Inputs {"));
    TASK_CHECK(Contains(out, "struct VOut {"));
    TASK_CHECK(Contains(out, "VOut helper(VOut v)"));

    // Locals keep the original structure types and names are unique.
    TASK_CHECK(Contains(out, "Inputs input0;"));
    TASK_CHECK(Contains(out, "Leaf side1;"));

    // Full member path assignments before the call.
    TASK_CHECK(Contains(out, "input0.pos = POSITION;"));
    TASK_CHECK(Contains(out, "input0.data.uv = TEXCOORD7;"));
    TASK_CHECK(Contains(out, "input0.data.extra = TEXCOORD8;"));
    TASK_CHECK(Contains(out, "side1.uv = TEXCOORD5;"));
    TASK_CHECK(Contains(out, "side1.extra = TEXCOORD6;"));

    // The entry is called exactly once.
    TASK_CHECK(CountNeedle(out, "= main(") == 1);

    // Outputs are read back out of the locals / return value.
    TASK_CHECK(Contains(out, "frag_TEXCOORD5 = side1.uv;"));
    TASK_CHECK(Contains(out, "frag_TEXCOORD6 = side1.extra;"));
    TASK_CHECK(Contains(out, "gl_Position = entry_result0.pos;"));
    TASK_CHECK(Contains(out, "frag_COLOR0 = entry_result0.tint;"));
    TASK_CHECK(Contains(out, "frag_TEXCOORD3 = entry_result0.vary;"));

    // No flip by default.
    TASK_CHECK(!Contains(out, "vec4(1,-1,2,1)"));
    return 0;
}

int marshal_glsl_vertex_flip_and_legacy()
{
    Allocator allocator;
    HLSLTree tree(&allocator);
    TASK_CHECK(ParseTree(tree, allocator, kNestedVs));

    GLSLGenerator generator;
    GLSLGenerator::Options options;
    options.recursiveEntryInterfaces = true;
    options.flags |= GLSLGenerator::Flag_FlipPositionOutput;
    TASK_CHECK(generator.Generate(&tree, GLSLGenerator::Target_VertexShader,
                                  GLSLGenerator::Version_110, "main", options));
    const char* out = generator.GetResult();

    // Legacy 110 qualifiers and the frag_ prefix are preserved.
    TASK_CHECK(Contains(out, "#version 110"));
    TASK_CHECK(Contains(out, "attribute vec4 POSITION;"));
    TASK_CHECK(Contains(out, "varying vec2 frag_TEXCOORD5;"));

    // The position flip is applied exactly once.
    TASK_CHECK(CountNeedle(out, "vec4(1,-1,2,1)") == 1);
    TASK_CHECK(Contains(out, "gl_Position = temp * vec4(1,-1,2,1)"));
    TASK_CHECK(CountNeedle(out, "= main(") == 1);
    return 0;
}

int marshal_glsl_fragment()
{
    Allocator allocator;
    HLSLTree tree(&allocator);
    TASK_CHECK(ParseTree(tree, allocator, kNestedFs));

    GLSLGenerator generator;
    GLSLGenerator::Options options;
    options.recursiveEntryInterfaces = true;
    TASK_CHECK(generator.Generate(&tree, GLSLGenerator::Target_FragmentShader,
                                  GLSLGenerator::Version_140, "main", options));
    const char* out = generator.GetResult();

    // SV_Position is imported from gl_FragCoord and custom inputs use frag_.
    TASK_CHECK(Contains(out, "i1.pos = gl_FragCoord;"));
    TASK_CHECK(Contains(out, "i1.uv = frag_TEXCOORD0;"));
    TASK_CHECK(Contains(out, "feed1 = frag_COLOR2;"));

    // Nested return and out colors size the modern color array.
    TASK_CHECK(Contains(out, "out vec4 rast_FragData[3];"));
    TASK_CHECK(Contains(out, "rast_FragData[2] = feed1;"));
    TASK_CHECK(Contains(out, "rast_FragData[1] = entry_result0.c1;"));
    TASK_CHECK(Contains(out, "rast_FragData[0] = entry_result0.c0;"));

    // Depth is clamped to 0..1.
    TASK_CHECK(Contains(out, "gl_FragDepth = clamp(float(entry_result0.d), 0.0, 1.0);"));
    TASK_CHECK(CountNeedle(out, "= main(") == 1);

    // Legacy 110 uses gl_FragData and attribute/varying spelling.
    GLSLGenerator legacy;
    TASK_CHECK(legacy.Generate(&tree, GLSLGenerator::Target_FragmentShader,
                               GLSLGenerator::Version_110, "main", options));
    const char* legacyOut = legacy.GetResult();
    TASK_CHECK(Contains(legacyOut, "varying vec2 frag_TEXCOORD0;"));
    TASK_CHECK(Contains(legacyOut, "gl_FragData[1]"));
    TASK_CHECK(!Contains(legacyOut, "rast_FragData"));
    return 0;
}

int marshal_glsl_reserved_names()
{
    // "input", "output" and "mix" are GLSL reserved words; existing
    // sanitization must keep generated locals and member accesses consistent.
    static const char* source =
        "struct S { float4 mix:POSITION; float2 output:TEXCOORD0; };\n"
        "struct O { float4 p:SV_Position; float4 c:COLOR0; };\n"
        "O main(S input) { O o; o.p = input.mix; o.c = float4(input.output,0,1); return o; }";

    Allocator allocator;
    HLSLTree tree(&allocator);
    TASK_CHECK(ParseTree(tree, allocator, source));

    GLSLGenerator generator;
    GLSLGenerator::Options options;
    options.recursiveEntryInterfaces = true;
    TASK_CHECK(generator.Generate(&tree, GLSLGenerator::Target_VertexShader,
                                  GLSLGenerator::Version_140, "main", options));
    const char* out = generator.GetResult();
    TASK_CHECK(Contains(out, "S input0;"));
    TASK_CHECK(Contains(out, "input0.mix0 = POSITION;"));
    TASK_CHECK(Contains(out, "input0.output0 = TEXCOORD0;"));
    TASK_CHECK(Contains(out, "gl_Position = entry_result0.p;"));
    TASK_CHECK(Contains(out, "frag_COLOR0 = entry_result0.c;"));
    return 0;
}

int marshal_fragment_special_inputs()
{
    // VFACE is a real Metal built-in but an ordinary frag_ varying in this
    // GLSL backend; SV_Position is gl_FragCoord. The same leaf drives both
    // declarations from the shared plan.
    static const char* source =
        "struct FIn { float4 pos:SV_Position; float face:VFACE; float2 uv:TEXCOORD0; };\n"
        "float4 main(FIn i):COLOR0 { return float4(i.uv, i.face, 1.0); }";

    Allocator allocator;
    HLSLTree tree(&allocator);
    TASK_CHECK(ParseTree(tree, allocator, source));

    GLSLGenerator glsl;
    GLSLGenerator::Options glslOptions;
    glslOptions.recursiveEntryInterfaces = true;
    TASK_CHECK(glsl.Generate(&tree, GLSLGenerator::Target_FragmentShader,
                             GLSLGenerator::Version_140, "main", glslOptions));
    const char* glslOut = glsl.GetResult();
    TASK_CHECK(Contains(glslOut, "in float frag_VFACE;"));
    TASK_CHECK(Contains(glslOut, "in vec2 frag_TEXCOORD0;"));
    TASK_CHECK(Contains(glslOut, "i1.pos = gl_FragCoord;"));
    TASK_CHECK(Contains(glslOut, "i1.face = frag_VFACE;"));

    MSLGenerator msl;
    MSLGenerator::Options mslOptions;
    mslOptions.recursiveEntryInterfaces = true;
    TASK_CHECK(msl.Generate(&tree, MSLGenerator::Target_FragmentShader, "main", mslOptions));
    const char* mslOut = msl.GetResult();
    TASK_CHECK(Contains(mslOut, "i_face [[front_facing]];"));
    TASK_CHECK(Contains(mslOut, "i_pos [[position]];"));
    return 0;
}

int marshal_msl_vertex()
{
    Allocator allocator;
    HLSLTree tree(&allocator);
    TASK_CHECK(ParseTree(tree, allocator, kNestedVs));

    MSLGenerator generator;
    MSLGenerator::Options options;
    options.recursiveEntryInterfaces = true;
    options.attributeCallback = AttrPlusThree;
    TASK_CHECK(generator.Generate(&tree, MSLGenerator::Target_VertexShader, "main", options));
    const char* out = generator.GetResult();

    // Flat stage structures plus the retained nested shader class.
    TASK_CHECK(Contains(out, "struct main_stage_in {"));
    TASK_CHECK(Contains(out, "struct main_stage_out {"));
    TASK_CHECK(Contains(out, "struct Vertex_Shader {"));
    TASK_CHECK(Contains(out, "VOut helper(VOut v)"));

    // Callback driven attribute numbering and user/position tags.
    TASK_CHECK(Contains(out, "input_pos [[attribute(3)]];"));
    TASK_CHECK(Contains(out, "input_data_uv [[attribute(10)]];"));
    TASK_CHECK(Contains(out, "side_uv [[attribute(8)]];"));
    TASK_CHECK(Contains(out, "return_pos [[position]];"));
    TASK_CHECK(Contains(out, "return_tint [[user(COLOR0)]];"));
    TASK_CHECK(Contains(out, "side_uv [[user(TEXCOORD5)]];"));

    // stage_in marshalling along full member paths.
    TASK_CHECK(Contains(out, "main_stage_in stage [[stage_in]]"));
    TASK_CHECK(Contains(out, "input1.data.uv = stage.input_data_uv;"));
    TASK_CHECK(Contains(out, "side1.extra = stage.side_extra;"));

    // Exactly one call into the shader class method.
    TASK_CHECK(CountNeedle(out, "main.main_") == 1);

    // The output tuple is consumed leaf by leaf; it is never assigned to the
    // stage output as a whole.
    TASK_CHECK(Contains(out, "entry_out.side.uv"));
    TASK_CHECK(Contains(out, "entry_out.__result.pos"));
    TASK_CHECK(!Contains(out, "= entry_out;"));

    // The nested class structs carry no direction dependent tags.
    const char* classStart = strstr(out, "struct Vertex_Shader {");
    const char* classEnd = strstr(classStart, "};");
    TASK_CHECK(classStart != NULL && classEnd != NULL);
    std::string classBody(classStart, classEnd - classStart);
    TASK_CHECK(classBody.find("[[") == std::string::npos);
    return 0;
}

int marshal_msl_fragment()
{
    Allocator allocator;
    HLSLTree tree(&allocator);
    TASK_CHECK(ParseTree(tree, allocator, kNestedFs));

    MSLGenerator generator;
    MSLGenerator::Options options;
    options.recursiveEntryInterfaces = true;
    TASK_CHECK(generator.Generate(&tree, MSLGenerator::Target_FragmentShader, "main", options));
    const char* out = generator.GetResult();

    TASK_CHECK(Contains(out, "struct main_stage_in {"));
    TASK_CHECK(Contains(out, "struct main_stage_out {"));

    // Fragment custom inputs and built-ins.
    TASK_CHECK(Contains(out, "i_pos [[position]];"));
    TASK_CHECK(Contains(out, "i_uv [[user(TEXCOORD0)]];"));
    TASK_CHECK(Contains(out, "feed [[user(COLOR2)]];"));

    // Colors and depth map to the right Metal targets.
    TASK_CHECK(Contains(out, "feed [[color(2)]];"));
    TASK_CHECK(Contains(out, "return_c1 [[color(1)]];"));
    TASK_CHECK(Contains(out, "return_c0 [[color(0)]];"));
    TASK_CHECK(Contains(out, "return_d [[depth(any)]];"));

    // Tuple return mapped into individual stage leaves.
    TASK_CHECK(Contains(out, "stageOut.feed = entry_out.feed;"));
    TASK_CHECK(Contains(out, "stageOut.return_d = entry_out.__result.d;"));
    TASK_CHECK(CountNeedle(out, "main.main_") == 1);
    return 0;
}

int marshal_out_argument()
{
    // A pure "out" parameter returns through the MSL output tuple and must not
    // be passed to the generated class method (its signature drops out args).
    static const char* source =
        "struct O { float4 p:SV_Position; float4 c:COLOR0; };\n"
        "O main(float4 pos:POSITION, float2 uv:TEXCOORD0, out float4 extra:TEXCOORD2) {\n"
        "  O o; o.p=pos; o.c=float4(uv,0,1); extra=float4(uv,1,1); return o; }";

    Allocator allocator;
    HLSLTree tree(&allocator);
    TASK_CHECK(ParseTree(tree, allocator, source));

    MSLGenerator msl;
    MSLGenerator::Options mslOptions;
    mslOptions.recursiveEntryInterfaces = true;
    mslOptions.attributeCallback = AttrIndex;
    TASK_CHECK(msl.Generate(&tree, MSLGenerator::Target_VertexShader, "main", mslOptions));
    const char* mslOut = msl.GetResult();
    // The method call carries only in/inout arguments; the out local is absent.
    TASK_CHECK(Contains(mslOut, "auto entry_out = main.main_2(pos1, uv1);"));
    // The out value is read from the tuple field into its stage output leaf.
    TASK_CHECK(Contains(mslOut, "stageOut.extra = entry_out.extra;"));
    TASK_CHECK(Contains(mslOut, "extra [[user(TEXCOORD2)]];"));
    // The tuple field is present inside the shader class.
    TASK_CHECK(Contains(mslOut, "float4 extra;"));

    GLSLGenerator glsl;
    GLSLGenerator::Options glslOptions;
    glslOptions.recursiveEntryInterfaces = true;
    TASK_CHECK(glsl.Generate(&tree, GLSLGenerator::Target_VertexShader,
                             GLSLGenerator::Version_140, "main", glslOptions));
    const char* glslOut = glsl.GetResult();
    // GLSL keeps the out local and passes it, exporting after the call.
    TASK_CHECK(Contains(glslOut, "vec4 extra1;"));
    TASK_CHECK(Contains(glslOut, "O entry_result0 = main(pos1, uv1, extra1);"));
    TASK_CHECK(Contains(glslOut, "frag_TEXCOORD2 = extra1;"));
    return 0;
}

int marshal_msl_missing_callback()
{
    static const char* source =
        "void main(float2 uv:TEXCOORD0, out float4 c:POSITION){c=float4(uv,0,1);}";

    Allocator allocator;
    HLSLTree tree(&allocator);
    TASK_CHECK(ParseTree(tree, allocator, source));

    MSLGenerator generator;
    MSLGenerator::Options options;
    options.recursiveEntryInterfaces = true;
    options.attributeCallback = AttrReject;
    bool ok = generator.Generate(&tree, MSLGenerator::Target_VertexShader, "main", options);
    TASK_CHECK(!ok);
    TASK_CHECK(generator.GetResult() == NULL || generator.GetResult()[0] == 0);
    TASK_CHECK(options.attributeCallback == AttrReject);
    return 0;
}

int marshal_failure_recovery()
{
    static const char* bad =
        "struct S { float4 p:POSITION; float2 q; }; void main(S s, out float4 c:COLOR0){c=s.p;}";

    Allocator badAlloc;
    HLSLTree badTree(&badAlloc);
    TASK_CHECK(ParseTree(badTree, badAlloc, bad));

    Allocator goodAlloc;
    HLSLTree goodTree(&goodAlloc);
    TASK_CHECK(ParseTree(goodTree, goodAlloc, kNestedFs));

    GLSLGenerator::Options options;
    options.recursiveEntryInterfaces = true;

    GLSLGenerator generator;
    TASK_CHECK(!generator.Generate(&badTree, GLSLGenerator::Target_FragmentShader,
                                   GLSLGenerator::Version_140, "main", options));
    TASK_CHECK(generator.GetResult() == NULL || generator.GetResult()[0] == 0);

    // The same generator must still succeed and must not inherit state
    // (error flag, color slot count) from the failed run.
    TASK_CHECK(generator.Generate(&goodTree, GLSLGenerator::Target_FragmentShader,
                                  GLSLGenerator::Version_140, "main", options));
    const char* out = generator.GetResult();
    TASK_CHECK(out != NULL && out[0] != 0);
    TASK_CHECK(Contains(out, "out vec4 rast_FragData[3];"));

    // A second valid run on the same generator is stable.
    size_t firstLength = strlen(out);
    TASK_CHECK(generator.Generate(&goodTree, GLSLGenerator::Target_FragmentShader,
                                  GLSLGenerator::Version_140, "main", options));
    TASK_CHECK(strlen(generator.GetResult()) == firstLength);
    TASK_CHECK(strcmp(generator.GetResult(), out) == 0);
    return 0;
}

int marshal_alternating_backends()
{
    // The same structure is used as input and output. Direction dependent
    // tags must never be written back into the shared AST nodes.
    static const char* source =
        "struct Data { float2 uv:TEXCOORD0; float4 p:POSITION; };\n"
        "Data main(Data d) { Data o; o.uv=d.uv; o.p=d.p; return o; }";

    Allocator allocator;
    HLSLTree tree(&allocator);
    TASK_CHECK(ParseTree(tree, allocator, source));

    MSLGenerator::Options mslOptions;
    mslOptions.recursiveEntryInterfaces = true;
    mslOptions.attributeCallback = AttrIndex;
    GLSLGenerator::Options glslOptions;
    glslOptions.recursiveEntryInterfaces = true;

    MSLGenerator msl1;
    TASK_CHECK(msl1.Generate(&tree, MSLGenerator::Target_VertexShader, "main", mslOptions));
    std::string firstMsl = msl1.GetResult();

    GLSLGenerator glsl;
    TASK_CHECK(glsl.Generate(&tree, GLSLGenerator::Target_VertexShader,
                             GLSLGenerator::Version_140, "main", glslOptions));
    TASK_CHECK(Contains(glsl.GetResult(), "in vec4 POSITION;"));
    TASK_CHECK(Contains(glsl.GetResult(), "gl_Position = entry_result0.p;"));

    MSLGenerator msl2;
    TASK_CHECK(msl2.Generate(&tree, MSLGenerator::Target_VertexShader, "main", mslOptions));
    TASK_CHECK(firstMsl == std::string(msl2.GetResult()));

    // The AST struct fields were never tagged.
    HLSLStruct* structure = tree.FindGlobalStruct("Data");
    TASK_CHECK(structure != NULL);
    for (HLSLStructField* field = structure->field; field != NULL; field = field->nextField)
        TASK_CHECK(field->sv_semantic == NULL);
    return 0;
}

int marshal_tree_not_mutated()
{
    Allocator allocator;
    HLSLTree tree(&allocator);
    TASK_CHECK(ParseTree(tree, allocator, kNestedVs));

    // Remember the original hidden flags; repeated generation must not flip
    // any AST node.
    int originalHidden = 0;
    for (HLSLStatement* s = tree.GetRoot()->statement; s != NULL; s = s->nextStatement)
        if (s->hidden) ++originalHidden;

    MSLGenerator::Options mslOptions;
    mslOptions.recursiveEntryInterfaces = true;
    mslOptions.attributeCallback = AttrIndex;
    for (int run = 0; run < 3; ++run)
    {
        MSLGenerator msl;
        TASK_CHECK(msl.Generate(&tree, MSLGenerator::Target_VertexShader, "main", mslOptions));
    }

    int hiddenAfter = 0;
    for (HLSLStatement* s = tree.GetRoot()->statement; s != NULL; s = s->nextStatement)
        if (s->hidden) ++hiddenAfter;
    TASK_CHECK(hiddenAfter == originalHidden);

    HLSLStruct* structure = tree.FindGlobalStruct("VOut");
    TASK_CHECK(structure != NULL);
    for (HLSLStructField* field = structure->field; field != NULL; field = field->nextField)
        TASK_CHECK(field->sv_semantic == NULL);
    return 0;
}

// ===========================================================================
// compat group: legacy mode output is byte identical and resources still work
// ===========================================================================

#include "checks/target_golden.inc"

int compat_default_flag_is_off()
{
    GLSLGenerator::Options glslOptions;
    MSLGenerator::Options mslOptions;
    TASK_CHECK(glslOptions.recursiveEntryInterfaces == false);
    TASK_CHECK(mslOptions.recursiveEntryInterfaces == false);
    return 0;
}

int compat_glsl_vertex_golden()
{
    Allocator allocator;
    HLSLTree tree(&allocator);
    TASK_CHECK(ParseTree(tree, allocator, kGoldenVs));
    GLSLGenerator generator;
    TASK_CHECK(generator.Generate(&tree, GLSLGenerator::Target_VertexShader,
                                  GLSLGenerator::Version_140, "main"));
    TASK_CHECK(strcmp(generator.GetResult(), kGoldenGlslVs) == 0);
    return 0;
}

int compat_glsl_fragment_golden()
{
    Allocator allocator;
    HLSLTree tree(&allocator);
    TASK_CHECK(ParseTree(tree, allocator, kGoldenFs));
    GLSLGenerator generator;
    TASK_CHECK(generator.Generate(&tree, GLSLGenerator::Target_FragmentShader,
                                  GLSLGenerator::Version_140, "main"));
    TASK_CHECK(strcmp(generator.GetResult(), kGoldenGlslFs) == 0);
    return 0;
}

int compat_msl_vertex_golden()
{
    Allocator allocator;
    HLSLTree tree(&allocator);
    TASK_CHECK(ParseTree(tree, allocator, kGoldenVs));
    MSLGenerator generator;
    MSLGenerator::Options options;
    options.attributeCallback = AttrIndex;
    TASK_CHECK(generator.Generate(&tree, MSLGenerator::Target_VertexShader, "main", options));
    TASK_CHECK(strcmp(generator.GetResult(), kGoldenMslVs) == 0);
    return 0;
}

int compat_msl_fragment_golden()
{
    Allocator allocator;
    HLSLTree tree(&allocator);
    TASK_CHECK(ParseTree(tree, allocator, kGoldenFs));
    MSLGenerator generator;
    TASK_CHECK(generator.Generate(&tree, MSLGenerator::Target_FragmentShader, "main"));
    TASK_CHECK(strcmp(generator.GetResult(), kGoldenMslFs) == 0);
    return 0;
}

int compat_legacy_resources_still_supported()
{
    static const char* source =
        "sampler2D tex;\n"
        "cbuffer CB : register(b0) { float scale; };\n"
        "float4 main(float2 uv:TEXCOORD0):COLOR0 { return tex2D(tex, uv) * scale; }";

    Allocator allocator;
    HLSLTree tree(&allocator);
    TASK_CHECK(ParseTree(tree, allocator, source));

    GLSLGenerator glsl;
    TASK_CHECK(glsl.Generate(&tree, GLSLGenerator::Target_FragmentShader,
                             GLSLGenerator::Version_140, "main"));
    TASK_CHECK(Contains(glsl.GetResult(), "uniform sampler2D tex;"));
    TASK_CHECK(Contains(glsl.GetResult(), "layout (std140) uniform CB {"));

    MSLGenerator msl;
    MSLGenerator::Options options;
    options.attributeCallback = AttrIndex;
    TASK_CHECK(msl.Generate(&tree, MSLGenerator::Target_FragmentShader, "main", options));
    TASK_CHECK(Contains(msl.GetResult(), "texture(0)"));
    TASK_CHECK(Contains(msl.GetResult(), "buffer(0)"));
    return 0;
}

// ===========================================================================
// Registration
// ===========================================================================

} // namespace

// The test driver (task_support/driver.c) is compiled as C++ by the harness,
// so these definitions match the C++ linkage of the declarations in check.h.
// They must stay at global scope: an anonymous namespace would give them
// internal linkage and hide them from the driver.
const task_case task_cases[] =
{
    // flatten
    { "flatten", "nested_plan",                flatten_nested_plan },
    { "flatten", "directions",                 flatten_directions },
    { "flatten", "same_semantic_both_ways",    flatten_same_semantic_both_directions },
    { "flatten", "failures",                   flatten_failures },
    { "flatten", "resources_rejected",         flatten_resources_rejected },

    // marshal
    { "marshal", "glsl_vertex",                marshal_glsl_vertex },
    { "marshal", "glsl_vertex_flip_legacy",    marshal_glsl_vertex_flip_and_legacy },
    { "marshal", "glsl_fragment",              marshal_glsl_fragment },
    { "marshal", "glsl_reserved_names",        marshal_glsl_reserved_names },
    { "marshal", "fragment_special_inputs",    marshal_fragment_special_inputs },
    { "marshal", "msl_vertex",                 marshal_msl_vertex },
    { "marshal", "msl_fragment",               marshal_msl_fragment },
    { "marshal", "out_argument",               marshal_out_argument },
    { "marshal", "msl_missing_callback",       marshal_msl_missing_callback },
    { "marshal", "failure_recovery",           marshal_failure_recovery },
    { "marshal", "alternating_backends",       marshal_alternating_backends },
    { "marshal", "tree_not_mutated",           marshal_tree_not_mutated },

    // compat
    { "compat", "default_flag_off",            compat_default_flag_is_off },
    { "compat", "glsl_vertex_golden",          compat_glsl_vertex_golden },
    { "compat", "glsl_fragment_golden",        compat_glsl_fragment_golden },
    { "compat", "msl_vertex_golden",           compat_msl_vertex_golden },
    { "compat", "msl_fragment_golden",         compat_msl_fragment_golden },
    { "compat", "legacy_resources",            compat_legacy_resources_still_supported },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
