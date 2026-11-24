// Copyright 2011-2022, Molecular Matters GmbH <office@molecular-matters.com>
// See LICENSE.txt for licensing details (2-clause BSD License: https://opensource.org/licenses/BSD-2-Clause)

#include "PDB_PCH.h"
#include "PDB_ModuleDataSymbolAnalysis.h"
#include "PDB_ModuleSymbolStream.h"
#include "PDB_Util.h"
#include "Foundation/PDB_BitUtil.h"

#include <algorithm>
#include <cstring>


// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
PDB::ModuleSymbolAnalysisResult PDB::AnalyzeModuleDataSymbols(const RawFile& rawFile, uint16_t moduleStreamIndex, uint32_t symbolRegionSize)
{
	ModuleSymbolAnalysisResult result;

	// Go through the library's normal module-symbol reader. It coalesces pages, so names that
	// straddle a physical page boundary appear as contiguous NUL-terminated strings.
	ModuleSymbolStream moduleSymbols(rawFile, moduleStreamIndex, symbolRegionSize);

	std::string currentModuleName;

	// ModuleSymbolStream::ForEachSymbol hides its running offset, so track it here with the exact
	// same stepping rule. The callback visits records in order, keeping the offsets in lockstep.
	size_t currentRecordOffset = sizeof(uint32_t);

	moduleSymbols.ForEachSymbol([&](const CodeView::DBI::Record* record)
	{
		const CodeView::DBI::SymbolRecordKind kind = record->header.kind;
		const uint32_t recordPayloadSize = GetCodeViewRecordSize(record);
		const uint32_t recordByteSize = static_cast<uint32_t>(sizeof(CodeView::DBI::RecordHeader)) + recordPayloadSize;
		const uint32_t recordOffset = static_cast<uint32_t>(currentRecordOffset);

		if (kind == CodeView::DBI::SymbolRecordKind::S_OBJNAME)
		{
			const char* moduleName = record->data.S_OBJNAME.name;
			if (moduleName != nullptr)
			{
				currentModuleName.assign(moduleName);
			}
		}
		else if (kind == CodeView::DBI::SymbolRecordKind::S_GDATA32)
		{
			const auto& gdata = record->data.S_GDATA32;

			// The fixed S_GDATA32 payload is typeIndex(4) + offset(4) + section(2), so bound the
			// name to whatever bytes the record actually stores. recordPayloadSize already excludes
			// the 2-byte record kind.
			const size_t fixedPayloadSize = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint16_t);
			const size_t maxNameBytes = (recordPayloadSize >= fixedPayloadSize)
				? static_cast<size_t>(recordPayloadSize) - fixedPayloadSize
				: 0u;

			const char* name = gdata.name;
			size_t nameLength = 0u;
			if (name != nullptr && maxNameBytes != 0u)
			{
				// Bounded scan instead of strnlen (POSIX visibility differs under strict -std=c++17).
				const void* terminator = std::memchr(name, '\0', maxNameBytes);
				nameLength = (terminator != nullptr)
					? static_cast<size_t>(static_cast<const char*>(terminator) - name)
					: maxNameBytes;
			}

			ModuleDataSymbol symbol;
			symbol.moduleName = currentModuleName;
			symbol.variableName.assign(name, nameLength);
			symbol.typeIndex = gdata.typeIndex;
			symbol.section = gdata.section;
			symbol.sectionOffset = gdata.offset;
			symbol.streamIndex = moduleStreamIndex;
			symbol.recordOffset = recordOffset;
			symbol.recordSize = recordByteSize;

			// Evidence mapping comes from RawFile's normalized directory, never from a separately
			// parsed container directory.
			symbol.physicalRanges = rawFile.GetStreamPhysicalRanges(moduleStreamIndex, recordOffset, recordByteSize);

			result.dataSymbols.push_back(std::move(symbol));
		}

		currentRecordOffset = BitUtil::RoundUpToMultiple<size_t>(
			currentRecordOffset + sizeof(CodeView::DBI::RecordHeader) + recordPayloadSize, 4u);
	});

	return result;
}
