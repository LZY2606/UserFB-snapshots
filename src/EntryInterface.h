//=============================================================================
//
// Render/EntryInterface.h
//
// Backend independent "leaf" interface plan for recursive entry point
// arguments and return values. The plan flattens nested structures into a
// list of scalar/vector leaves without rewriting the syntax tree.
//
//=============================================================================

#ifndef ENTRY_INTERFACE_H
#define ENTRY_INTERFACE_H

#include "Engine.h"
#include "HLSLTree.h"

#include <string>
#include <vector>

namespace M4
{

enum EntryDirection
{
    EntryDirection_Input,
    EntryDirection_Output,
};

enum EntryTarget
{
    EntryTarget_VertexShader,
    EntryTarget_FragmentShader,
};

/** A single flattened leaf of an entry point interface. */
struct EntryInterfaceLeaf
{
    EntryDirection      direction;          // Input or output.
    HLSLBaseType        baseType;           // Float/Half scalar or vector family.
    const char*         semantic;           // Original HLSL semantic (never NULL).
    std::string         path;               // Full member path, e.g. "input.data.uv".
    std::string         memberChain;        // Path inside the root object ("" for the root itself).
    std::string         glslTarget;         // Final GLSL variable or built-in target.
    std::string         mslTarget;          // Final Metal attribute, e.g. "attribute(3)", "color(0)".
    std::string         stageName;          // Member name in the flat MSL stage structs.
    const char*         rootName;           // Argument name, or "@return" for the return value.
    bool                isReturn;           // This leaf comes from the return value.
    int                 argumentIndex;      // Argument index, or -1 for the return value.
    bool                builtIn;            // Maps to a backend built-in variable/attribute.
    int                 colorSlot;          // Output color slot, or -1.
};

/**
 * The flattened interface plan for one entry point. Built once and shared by
 * every backend: every leaf stores both its GLSL and MSL target, so the same
 * plan drives declarations, pre-call marshalling and post-call exporting in
 * either generator. No AST node is modified while building it.
 */
class EntryInterfacePlan
{
public:

    EntryInterfacePlan();

    /** Builds the plan for an entry function. Returns false and fills error
     *  when the entry cannot be flattened (resource bindings, arrays,
     *  missing semantics, duplicate outputs, ...). The object is empty on
     *  failure. */
    bool Build(HLSLTree* tree, HLSLFunction* entryFunction, EntryTarget target,
               bool glslLegacy,
               int (*mslAttributeCallback)(const char* name, unsigned int index),
               std::string& error);

    int                 GetNumLeaves() const;
    const EntryInterfaceLeaf& GetLeaf(int index) const;

    int                 GetNumInputLeaves() const;
    int                 GetNumOutputLeaves() const;

    /** Number of color output slots required (max slot index + 1). */
    int                 GetNumColorSlots() const;

    /** True if any output leaf maps to the position built-in. */
    bool                HasPositionOutput() const;

private:

    struct RootContext
    {
        const char* rootName;
        bool        isReturn;
        int         argumentIndex;
    };

    bool CheckNoResources(HLSLRoot* root, HLSLFunction* entryFunction, std::string& error);
    bool BuildPlan(HLSLTree* tree, HLSLFunction* entryFunction, EntryTarget target,
                   bool glslLegacy,
                   int (*mslAttributeCallback)(const char* name, unsigned int index),
                   std::string& error);
    bool FlattenType(const HLSLType& type, const char* inheritedSemantic,
                     const std::string& path,
                     const RootContext& context, EntryDirection direction,
                     std::string& error);
    bool AddLeaf(const HLSLType& type, const char* semantic, const std::string& path,
                 const std::string& memberChain, const RootContext& context,
                 EntryDirection direction, std::string& error);
    bool ResolveTargets(EntryInterfaceLeaf& leaf, std::string& error);
    bool CheckDuplicates(std::string& error) const;
    void ParseSemanticNameIndex(const char* semantic, std::string& name,
                                unsigned int& index) const;
    static std::string SanitizeStageName(const std::string& path);

    HLSLTree*                       m_tree;
    EntryTarget                     m_target;
    bool                            m_glslLegacy;
    int                             (*m_mslAttributeCallback)(const char* name, unsigned int index);
    std::vector<EntryInterfaceLeaf> m_leaves;
    int                             m_numColorSlots;
    bool                            m_hasPositionOutput;
};

} // M4

#endif
