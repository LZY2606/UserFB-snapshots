// Copyright 2011-2022, Molecular Matters GmbH <office@molecular-matters.com>
// See LICENSE.txt for licensing details (2-clause BSD License: https://opensource.org/licenses/BSD-2-Clause)

#pragma once

#include "PDB_RawFile.h"

#include <cstdint>
#include <string>
#include <vector>


// Offline analysis helpers for module symbol streams.
//
// The entry point below deliberately does not perform DBI module discovery: callers tell it which
// module stream number and symbol-region length to analyze. It reads through the normal
// RawFile::CreateMSFStream / ModuleSymbolStream entry points, enumerates the CodeView record
// versions already supported by this library (S_OBJNAME and S_GDATA32), and owns all returned
// names and numeric values. In particular, results never point into a temporary CoalescedMSFStream
// buffer.
//
// Lifetime:
//   * The returned vectors/strings are self-contained and remain valid after the RawFile and any
//     temporary streams used during analysis are destroyed.
//   * PhysicalFileRange values are absolute byte offsets/sizes into the original container bytes.
//     Dereferencing that evidence requires the caller to keep the original file/image buffer
//     alive; the analysis result itself does not retain that buffer.
//   * section + sectionOffset is a section-relative location, not an RVA or runtime address:
//     no image section table is consulted here.
namespace PDB
{
	struct ModuleDataSymbol
	{
		// Module (object) name from S_OBJNAME, e.g. "alpha.obj".
		std::string moduleName;

		// Variable name from S_GDATA32, copied out of the coalesced record.
		std::string variableName;

		// Type index reported verbatim by the record; no type resolution or width guessing.
		uint32_t typeIndex = 0u;

		// Section number and section-relative offset. Not an RVA.
		uint16_t section = 0u;
		uint32_t sectionOffset = 0u;

		// Module stream number supplied by the caller.
		uint32_t streamIndex = 0u;

		// Start offset and exact byte size of the variable record inside the stream.
		uint32_t recordOffset = 0u;
		uint32_t recordSize = 0u;

		// Original-file byte intervals covering exactly this record, in stream logical order.
		// Discontinuous physical pages produce multiple entries.
		std::vector<PhysicalFileRange> physicalRanges;
	};

	struct ModuleSymbolAnalysisResult
	{
		std::vector<ModuleDataSymbol> dataSymbols;
	};

	// Analyzes one module symbol stream. "symbolRegionSize" is the caller-provided size of the
	// symbol region (may be <= the full stream size). Every S_GDATA32 record yields one entry;
	// same-named variables are never merged because streamIndex + recordOffset identify them.
	PDB_NO_DISCARD ModuleSymbolAnalysisResult AnalyzeModuleDataSymbols(const RawFile& rawFile, uint16_t moduleStreamIndex, uint32_t symbolRegionSize);

	// Naming aliases for the same reusable interface.
	using ModuleSymbolLocation = ModuleDataSymbol;
	using ModuleDataSymbolAnalysis = ModuleSymbolAnalysisResult;
}
