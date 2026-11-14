//=============================================================================
//
// Render/EntryInterface.cpp
//
//=============================================================================

#include "EntryInterface.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

namespace M4
{

static bool IsFloatLeafType(HLSLBaseType baseType)
{
    return baseType == HLSLBaseType_Float  || baseType == HLSLBaseType_Float2 ||
           baseType == HLSLBaseType_Float3 || baseType == HLSLBaseType_Float4 ||
           baseType == HLSLBaseType_Half   || baseType == HLSLBaseType_Half2 ||
           baseType == HLSLBaseType_Half3  || baseType == HLSLBaseType_Half4;
}

EntryInterfacePlan::EntryInterfacePlan()
{
    m_tree                  = NULL;
    m_target                = EntryTarget_VertexShader;
    m_glslLegacy            = false;
    m_mslAttributeCallback  = NULL;
    m_numColorSlots         = 0;
    m_hasPositionOutput     = false;
}

int EntryInterfacePlan::GetNumLeaves() const
{
    return static_cast<int>(m_leaves.size());
}

const EntryInterfaceLeaf& EntryInterfacePlan::GetLeaf(int index) const
{
    return m_leaves[static_cast<size_t>(index)];
}

int EntryInterfacePlan::GetNumInputLeaves() const
{
    int count = 0;
    for (size_t i = 0; i < m_leaves.size(); ++i)
        if (m_leaves[i].direction == EntryDirection_Input)
            ++count;
    return count;
}

int EntryInterfacePlan::GetNumOutputLeaves() const
{
    int count = 0;
    for (size_t i = 0; i < m_leaves.size(); ++i)
        if (m_leaves[i].direction == EntryDirection_Output)
            ++count;
    return count;
}

int EntryInterfacePlan::GetNumColorSlots() const
{
    return m_numColorSlots;
}

bool EntryInterfacePlan::HasPositionOutput() const
{
    return m_hasPositionOutput;
}

void EntryInterfacePlan::ParseSemanticNameIndex(const char* semantic, std::string& name,
                                                unsigned int& index) const
{
    const char* p = semantic;
    while (*p && !isdigit(static_cast<unsigned char>(*p)))
        ++p;
    name.assign(semantic, p);
    index = static_cast<unsigned int>(atoi(p));
}

std::string EntryInterfacePlan::SanitizeStageName(const std::string& path)
{
    std::string name;
    name.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i)
    {
        char c = path[i];
        if (c == '@')
            continue;
        name.push_back(c == '.' ? '_' : c);
    }
    return name;
}

bool EntryInterfacePlan::CheckNoResources(HLSLRoot* root, HLSLFunction* entryFunction,
                                          std::string& error)
{
    for (HLSLStatement* statement = root->statement; statement != NULL;
         statement = statement->nextStatement)
    {
        if (statement->nodeType == HLSLNodeType_Declaration)
        {
            HLSLDeclaration* declaration = static_cast<HLSLDeclaration*>(statement);
            if (IsSamplerType(declaration->type.baseType) ||
                declaration->type.baseType == HLSLBaseType_Texture)
            {
                error = "recursive entry interfaces do not support texture/sampler resource '" +
                        std::string(declaration->name) + "'";
                return false;
            }
            if ((declaration->type.flags & HLSLTypeFlag_Static) == 0)
            {
                error = "recursive entry interfaces do not support uniform global '" +
                        std::string(declaration->name) + "'";
                return false;
            }
        }
        else if (statement->nodeType == HLSLNodeType_Buffer)
        {
            HLSLBuffer* buffer = static_cast<HLSLBuffer*>(statement);
            error = "recursive entry interfaces do not support constant buffer '" +
                    std::string(buffer->name) + "'";
            return false;
        }
    }

    for (HLSLArgument* argument = entryFunction->argument; argument != NULL;
         argument = argument->nextArgument)
    {
        if (argument->modifier == HLSLArgumentModifier_Uniform)
        {
            error = "recursive entry interfaces do not support uniform argument '" +
                    std::string(argument->name) + "'";
            return false;
        }
    }

    return true;
}

bool EntryInterfacePlan::Build(HLSLTree* tree, HLSLFunction* entryFunction, EntryTarget target,
                               bool glslLegacy,
                               int (*mslAttributeCallback)(const char* name, unsigned int index),
                               std::string& error)
{
    // A failed build must leave the plan empty, even if leaves had already
    // been accumulated before the error.
    m_leaves.clear();
    m_numColorSlots     = 0;
    m_hasPositionOutput = false;

    if (!BuildPlan(tree, entryFunction, target, glslLegacy, mslAttributeCallback, error))
    {
        m_leaves.clear();
        m_numColorSlots     = 0;
        m_hasPositionOutput = false;
        return false;
    }
    return true;
}

bool EntryInterfacePlan::BuildPlan(HLSLTree* tree, HLSLFunction* entryFunction, EntryTarget target,
                                   bool glslLegacy,
                                   int (*mslAttributeCallback)(const char* name, unsigned int index),
                                   std::string& error)
{
    m_tree              = tree;
    m_target            = target;
    m_glslLegacy        = glslLegacy;
    m_mslAttributeCallback = mslAttributeCallback;

    HLSLRoot* root = tree->GetRoot();

    if (!CheckNoResources(root, entryFunction, error))
        return false;

    int argumentIndex = 0;
    for (HLSLArgument* argument = entryFunction->argument; argument != NULL;
         argument = argument->nextArgument, ++argumentIndex)
    {
        RootContext context;
        context.rootName      = argument->name;
        context.isReturn      = false;
        context.argumentIndex = argumentIndex;

        bool isOut   = argument->modifier == HLSLArgumentModifier_Out;
        bool isInout = argument->modifier == HLSLArgumentModifier_Inout;

        if (!isOut)
        {
            if (!FlattenType(argument->type, argument->semantic, argument->name,
                             context, EntryDirection_Input, error))
                return false;
        }
        if (isOut || isInout)
        {
            if (!FlattenType(argument->type, argument->semantic, argument->name,
                             context, EntryDirection_Output, error))
                return false;
        }
    }

    if (entryFunction->returnType.baseType != HLSLBaseType_Void)
    {
        RootContext context;
        context.rootName      = "@return";
        context.isReturn      = true;
        context.argumentIndex = -1;

        if (!FlattenType(entryFunction->returnType, entryFunction->semantic, "@return",
                         context, EntryDirection_Output, error))
            return false;
    }

    if (!CheckDuplicates(error))
        return false;

    if (m_target == EntryTarget_VertexShader && !m_hasPositionOutput)
    {
        error = "Vertex shader must output a position";
        return false;
    }
    if (m_target == EntryTarget_FragmentShader && m_numColorSlots == 0)
    {
        error = "Fragment shader must output a color";
        return false;
    }
    if (GetNumInputLeaves() == 0)
    {
        error = "recursive entry interface requires at least one input leaf";
        return false;
    }

    return true;
}

bool EntryInterfacePlan::FlattenType(const HLSLType& type, const char* inheritedSemantic,
                                     const std::string& path,
                                     const RootContext& context, EntryDirection direction,
                                     std::string& error)
{
    if (type.array)
    {
        error = "array member is not supported in recursive entry interface: " + path;
        return false;
    }

    if (type.baseType == HLSLBaseType_UserDefined)
    {
        if (inheritedSemantic != NULL)
        {
            error = "semantic is not allowed on structure container: " + path;
            return false;
        }

        HLSLStruct* structure = m_tree->FindGlobalStruct(type.typeName);
        if (structure == NULL)
        {
            error = "unknown structure type '" + std::string(type.typeName) + "' at " + path;
            return false;
        }

        for (HLSLStructField* field = structure->field; field != NULL; field = field->nextField)
        {
            std::string childPath = path + "." + field->name;
            if (!FlattenType(field->type, field->semantic, childPath,
                             context, direction, error))
                return false;
        }
        return true;
    }

    if (IsSamplerType(type.baseType) || type.baseType == HLSLBaseType_Texture)
    {
        error = "resource member is not supported in recursive entry interface: " + path;
        return false;
    }

    if (!IsFloatLeafType(type.baseType))
    {
        error = "only float/float2/float3/float4 leaves are supported: " + path;
        return false;
    }

    if (inheritedSemantic == NULL)
    {
        error = "leaf member is missing a semantic: " + path;
        return false;
    }

    std::string memberChain;
    if (context.isReturn)
        memberChain = path.substr(strlen("@return"));
    else
        memberChain = path.substr(strlen(context.rootName));

    return AddLeaf(type, inheritedSemantic, path, memberChain, context, direction, error);
}

bool EntryInterfacePlan::AddLeaf(const HLSLType& type, const char* semantic,
                                 const std::string& path, const std::string& memberChain,
                                 const RootContext& context, EntryDirection direction,
                                 std::string& error)
{
    EntryInterfaceLeaf leaf;
    leaf.direction     = direction;
    leaf.baseType      = type.baseType;
    leaf.semantic      = semantic;
    leaf.path          = path;
    leaf.memberChain   = memberChain;
    leaf.rootName      = context.rootName;
    leaf.isReturn      = context.isReturn;
    leaf.argumentIndex = context.argumentIndex;
    leaf.builtIn       = false;
    leaf.colorSlot     = -1;
    leaf.stageName     = SanitizeStageName(path);

    // Distinct member paths can still sanitize to the same identifier (e.g.
    // "a_b.c" and "a.b_c"). Names only need to be unique within a direction,
    // since inputs and outputs live in separate stage structs.
    bool unique = false;
    unsigned suffix = 1;
    while (!unique)
    {
        unique = true;
        for (size_t k = 0; k < m_leaves.size(); ++k)
        {
            if (m_leaves[k].direction != leaf.direction)
                continue;
            if (m_leaves[k].stageName == leaf.stageName)
            {
                char buffer[16];
                String_Printf(buffer, sizeof(buffer), "_%u", suffix++);
                leaf.stageName = SanitizeStageName(path) + buffer;
                unique = false;
                break;
            }
        }
    }

    if (!ResolveTargets(leaf, error))
        return false;

    m_leaves.push_back(leaf);
    return true;
}

bool EntryInterfacePlan::ResolveTargets(EntryInterfaceLeaf& leaf, std::string& error)
{
    const char* semantic = leaf.semantic;
    const bool output = leaf.direction == EntryDirection_Output;

    std::string semanticName;
    unsigned int semanticIndex = 0;
    ParseSemanticNameIndex(semantic, semanticName, semanticIndex);

    if (m_target == EntryTarget_VertexShader)
    {
        if (output)
        {
            if (String_Equal(semantic, "POSITION") || String_Equal(semantic, "SV_Position"))
            {
                leaf.builtIn     = true;
                leaf.glslTarget  = "gl_Position";
                leaf.mslTarget   = "position";
                m_hasPositionOutput = true;
                return true;
            }
            if (String_Equal(semantic, "PSIZE") || String_Equal(semantic, "POINT_SIZE"))
            {
                leaf.builtIn    = true;
                leaf.glslTarget = "gl_PointSize";
                leaf.mslTarget  = "point_size";
                return true;
            }
            // Custom vertex output: interpolated user attribute on both backends.
            leaf.builtIn    = false;
            leaf.glslTarget = std::string("frag_") + semantic;
            leaf.mslTarget  = std::string("user(") + semantic + ")";
            return true;
        }
        else
        {
            if (String_Equal(semantic, "SV_InstanceID") || String_Equal(semantic, "INSTANCE_ID"))
            {
                leaf.builtIn    = true;
                leaf.glslTarget = "gl_InstanceID";
                leaf.mslTarget  = "instance_id";
                return true;
            }
            if (String_Equal(semantic, "SV_VertexID") || String_Equal(semantic, "VERTEX_ID"))
            {
                if (m_glslLegacy)
                {
                    error = "vertex id input is not available in legacy GLSL: " + leaf.path;
                    return false;
                }
                leaf.builtIn    = true;
                leaf.glslTarget = "gl_VertexID";
                leaf.mslTarget  = "vertex_id";
                return true;
            }
            // Custom vertex input: MSL attribute index comes from the callback.
            // The GLSL path has no callback; its target is still resolved and
            // the Metal attribute is left for an MSL build to fill in.
            if (m_mslAttributeCallback != NULL)
            {
                int attribute = m_mslAttributeCallback(semanticName.c_str(), semanticIndex);
                if (attribute < 0)
                {
                    error = "vertex input semantic '" + std::string(semantic) +
                            "' is not mapped to an attribute at " + leaf.path;
                    return false;
                }
                char buffer[32];
                String_Printf(buffer, sizeof(buffer), "attribute(%d)", attribute);
                leaf.mslTarget = buffer;
            }
            leaf.builtIn    = false;
            leaf.glslTarget = semantic;
            return true;
        }
    }

    // Fragment shader.
    if (output)
    {
        if (String_Equal(semantic, "SV_Depth") || String_Equal(semantic, "DEPTH") ||
            String_Equal(semantic, "DEPTH_GT") || String_Equal(semantic, "DEPTH_LT"))
        {
            leaf.builtIn    = true;
            leaf.glslTarget = "gl_FragDepth";
            leaf.mslTarget  = String_Equal(semantic, "DEPTH_GT") ? "depth(greater)" :
                              String_Equal(semantic, "DEPTH_LT") ? "depth(less)" : "depth(any)";
            return true;
        }
        if (semanticName == "COLOR" || semanticName == "SV_Target")
        {
            leaf.builtIn    = true;
            leaf.colorSlot  = static_cast<int>(semanticIndex);
            leaf.glslTarget = m_glslLegacy ? "gl_FragData" : "rast_FragData";
            char buffer[32];
            String_Printf(buffer, sizeof(buffer), "color(%d)", semanticIndex);
            leaf.mslTarget = buffer;
            if (m_numColorSlots <= static_cast<int>(semanticIndex))
                m_numColorSlots = static_cast<int>(semanticIndex) + 1;
            return true;
        }
        error = "Output attribute " + std::string(semantic) +
                " does not map to any built-ins at " + leaf.path;
        return false;
    }

    // Fragment inputs.
    if (String_Equal(semantic, "SV_Position") || String_Equal(semantic, "POSITION"))
    {
        leaf.builtIn    = true;
        leaf.glslTarget = "gl_FragCoord";
        leaf.mslTarget  = "position";
        return true;
    }
    if (String_Equal(semantic, "VFACE"))
    {
        leaf.builtIn    = true;
        leaf.glslTarget = "frag_VFACE";
        leaf.mslTarget  = "front_facing";
        return true;
    }
    if (String_Equal(semantic, "TARGET_INDEX"))
    {
        leaf.builtIn    = true;
        leaf.glslTarget = "frag_TARGET_INDEX";
        leaf.mslTarget  = "render_target_array_index";
        return true;
    }
    if (String_Equal(semantic, "SAMPLE_ID"))
    {
        leaf.builtIn    = true;
        leaf.glslTarget = "frag_SAMPLE_ID";
        leaf.mslTarget  = "sample_id";
        return true;
    }

    leaf.builtIn    = false;
    leaf.glslTarget = std::string("frag_") + semantic;
    leaf.mslTarget  = std::string("user(") + semantic + ")";
    return true;
}

bool EntryInterfacePlan::CheckDuplicates(std::string& error) const
{
    for (size_t i = 0; i < m_leaves.size(); ++i)
    {
        const EntryInterfaceLeaf& a = m_leaves[i];
        for (size_t j = i + 1; j < m_leaves.size(); ++j)
        {
            const EntryInterfaceLeaf& b = m_leaves[j];
            if (a.direction != b.direction)
                continue;

            const std::string& keyA = a.builtIn ? a.mslTarget : a.semantic;
            const std::string& keyB = b.builtIn ? b.mslTarget : b.semantic;

            if (keyA == keyB)
            {
                error = std::string("duplicate ") +
                        (a.direction == EntryDirection_Input ? "input" : "output") +
                        " interface target '" + keyA + "' at " + a.path + " and " + b.path;
                return false;
            }
        }
    }
    return true;
}

} // M4
