//=============================================================================
//
// DerivativeUniformity.h
//
// Optional offline analysis that flags derivative operations (ddx/ddy and
// texture samples with implicit LOD) which may execute in non-uniform control
// flow. The analysis is purely read-only: it never mutates the syntax tree.
//
//=============================================================================

#ifndef DERIVATIVE_UNIFORMITY_H
#define DERIVATIVE_UNIFORMITY_H

#include "HLSLTree.h"

#include <vector>

namespace M4
{

enum DerivativeDiagnosticType
{
    // A sensitive operation may execute inside divergent control flow.
    DerivativeDiagnostic_DivergentOperation,
    // Analysis could not be completed (recursion or unsupported construct).
    DerivativeDiagnostic_Unsupported,
};

struct DerivativeDiagnostic
{
    DerivativeDiagnosticType type;

    // Location of the sensitive call (ddx, tex2D, ...), or of the unsupported
    // construct for unsupported diagnostics.
    const char* fileName;
    int         line;

    // Location of the divergent condition which makes the call unsafe. NULL
    // for unsupported diagnostics.
    const char* reasonFileName;
    int         reasonLine;

    // Names of the functions on the path from the entry point to the call,
    // entry point first, function containing the call last.
    std::vector<const char*> callChain;

    // Human readable explanation.
    const char* message;

    DerivativeDiagnostic()
        : type(DerivativeDiagnostic_DivergentOperation)
        , fileName(NULL)
        , line(0)
        , reasonFileName(NULL)
        , reasonLine(0)
        , message(NULL)
    {
    }
};

/**
 * Runs the uniformity analysis starting at the specified entry point.
 *
 * Returns true when no risk was found and the analysis covered the whole
 * reachable tree. Returns false (with diagnostics appended) when a sensitive
 * operation is reachable through divergent control flow or when the analysis
 * could not be completed. The syntax tree is never modified.
 */
bool ValidateDerivativeUniformity(HLSLTree* tree, const char* entry,
                                  std::vector<DerivativeDiagnostic>& diagnostics);

} // M4

#endif
