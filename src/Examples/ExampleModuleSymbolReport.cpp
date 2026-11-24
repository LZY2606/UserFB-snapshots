// Offline example: builds the three synthetic analysis containers from task_support material,
// reads them through ValidateFile / CreateRawFile / AnalyzeModuleDataSymbols, and writes a TSV
// table of module data-symbol locations plus physical file evidence.
//
// The program never executes the images and never rewrites the input bytes; the containers are
// constructed in memory only. All table values come from actual reads.
//
// Complete offline build + run commands (from the repository root):
//   bash task_support/build.sh
//   c++ -std=c++17 -O2 -I. -Isrc -Itask_support \
//     src/Examples/ExampleModuleSymbolReport.cpp \
//     .task-build/libraw_pdb.a -o .task-build/module_symbol_report
//   mkdir -p analysis
//   ./.task-build/module_symbol_report analysis/module_symbols.tsv

#include "PDB_PCH.h"
#include "PDB.h"
#include "PDB_RawFile.h"
#include "PDB_ModuleDataSymbolAnalysis.h"

#include "analysis_fixture.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace
{
	using fixture::Bytes;

	const char* FormatName(PDB::ContainerFormat format)
	{
		return format == PDB::ContainerFormat::SmallMSF2 ? "SmallMSF2" : "MSF7";
	}

	std::string Hex32(uint32_t value)
	{
		char buffer[16];
		std::snprintf(buffer, sizeof(buffer), "0x%x", value);
		return std::string(buffer);
	}

	std::string HexOffset(uint32_t value)
	{
		char buffer[16];
		std::snprintf(buffer, sizeof(buffer), "0x%x", value);
		return std::string(buffer);
	}

	struct InputImage
	{
		std::string id;
		std::string fileName;
		Bytes bytes;
	};

	// Formats physical intervals as "offset:length;offset:length" (decimal).
	std::string FormatRangesDecimal(const std::vector<PDB::PhysicalFileRange>& ranges)
	{
		std::string result;
		for (size_t i = 0; i < ranges.size(); ++i)
		{
			char buffer[64];
			std::snprintf(buffer, sizeof(buffer), "%s%llu:%u",
				i == 0 ? "" : ";",
				static_cast<unsigned long long>(ranges[i].fileOffset),
				ranges[i].size);
			result += buffer;
		}
		return result;
	}

	// Formats physical intervals as "0xOFFSET:length(decimal);..." for cross-checking pages.
	std::string FormatRangesHex(const std::vector<PDB::PhysicalFileRange>& ranges)
	{
		std::string result;
		for (size_t i = 0; i < ranges.size(); ++i)
		{
			char buffer[64];
			std::snprintf(buffer, sizeof(buffer), "%s0x%llx:%u",
				i == 0 ? "" : ";",
				static_cast<unsigned long long>(ranges[i].fileOffset),
				ranges[i].size);
			result += buffer;
		}
		return result;
	}
}

int main(int argc, char** argv)
{
	const std::string outputPath = (argc >= 2) ? argv[1] : "analysis/module_symbols.tsv";

	// Input 1: existing MSF7 serializer over analysis_streams(false).
	// Input 2: Small MSF serializer over the same unchanged streams.
	// Input 3: analysis_streams(true) (alpha at 0x60) with firstDataPage=80.
	const std::vector<fixture::Bytes> standardStreams = fixture::analysis_streams(false);
	const std::vector<fixture::Bytes> changedStreams = fixture::analysis_streams(true);

	std::vector<InputImage> inputs;
	inputs.push_back(InputImage { "input1_msf7", "input1_msf7.pdb", fixture::msf(standardStreams) });
	inputs.push_back(InputImage { "input2_small_msf2", "input2_small_msf2.pdb", fixture::small_msf(standardStreams) });
	inputs.push_back(InputImage { "input3_small_msf2_alpha60_firstpage80", "input3_small_msf2_alpha60_firstpage80.pdb", fixture::small_msf(changedStreams, std::vector<bool>(), 80u) });

	// Persist the generated container bytes next to the TSV so physical_file ranges can be
	// traced back to actual file pages (e.g. with dd or the verification snippet in the .md).
	std::string outputDirectory;
	{
		const size_t slash = outputPath.find_last_of('/');
		outputDirectory = (slash == std::string::npos) ? std::string(".") : outputPath.substr(0, slash);
	}
	for (const InputImage& input : inputs)
	{
		std::ofstream imageFile(outputDirectory + "/" + input.fileName, std::ios::binary | std::ios::trunc);
		if (!imageFile)
		{
			std::fprintf(stderr, "error: cannot write %s/%s\n", outputDirectory.c_str(), input.fileName.c_str());
			return 5;
		}
		imageFile.write(reinterpret_cast<const char*>(input.bytes.data()), static_cast<std::streamsize>(input.bytes.size()));
	}

	std::ofstream tsv(outputPath, std::ios::binary | std::ios::trunc);
	if (!tsv)
	{
		std::fprintf(stderr, "error: cannot open %s for writing\n", outputPath.c_str());
		return 1;
	}

	tsv << "input_id\tcontainer_format\tblock_size\tfree_block_map_index\tblock_count\tdirectory_size"
	       "\tmodule_name\tstream_number\trecord_offset_dec\trecord_offset_hex\trecord_size"
	       "\tvariable_name\ttype_index_hex\ttype_index_dec\tsection_number"
	       "\tsection_offset_hex\tsection_offset_dec\tphysical_range_count"
	       "\tphysical_ranges_dec_offset:length\tphysical_ranges_hex\timage_file\n";

	unsigned totalRows = 0u;

	for (const InputImage& input : inputs)
	{
		const PDB::ErrorCode errorCode = PDB::ValidateFile(input.bytes.data(), input.bytes.size());
		if (errorCode != PDB::ErrorCode::Success)
		{
			std::fprintf(stderr, "error: ValidateFile failed for %s: %d\n", input.id.c_str(), static_cast<int>(errorCode));
			return 2;
		}

		// The input buffer is owned here and outlives every read.
		PDB::RawFile rawFile = PDB::CreateRawFile(input.bytes.data());
		const PDB::ContainerInfo info = rawFile.GetContainerInfo();

		for (uint16_t streamNumber = 6u; streamNumber <= 7u; ++streamNumber)
		{
			// Caller provides module stream number and symbol-region length (here: whole stream).
			const uint32_t symbolRegionSize = rawFile.GetStreamSize(streamNumber);
			const PDB::ModuleSymbolAnalysisResult analysis =
				PDB::AnalyzeModuleDataSymbols(rawFile, streamNumber, symbolRegionSize);

			for (const PDB::ModuleDataSymbol& symbol : analysis.dataSymbols)
			{
				tsv << input.id << '\t'
				    << FormatName(info.format) << '\t'
				    << info.blockSize << '\t'
				    << info.freeBlockMapIndex << '\t'
				    << info.blockCount << '\t'
				    << info.directorySize << '\t'
				    << symbol.moduleName << '\t'
				    << symbol.streamIndex << '\t'
				    << symbol.recordOffset << '\t'
				    << Hex32(symbol.recordOffset) << '\t'
				    << symbol.recordSize << '\t'
				    << symbol.variableName << '\t'
				    << Hex32(symbol.typeIndex) << '\t'
				    << symbol.typeIndex << '\t'
				    << symbol.section << '\t'
				    << HexOffset(symbol.sectionOffset) << '\t'
				    << symbol.sectionOffset << '\t'
				    << symbol.physicalRanges.size() << '\t'
				    << FormatRangesDecimal(symbol.physicalRanges) << '\t'
				    << FormatRangesHex(symbol.physicalRanges) << '\t'
				    << input.fileName << '\n';
				++totalRows;

				std::printf("%s stream=%u module=%s var=%s type=%s section=%u offset=%s ranges=%s\n",
					input.id.c_str(), symbol.streamIndex, symbol.moduleName.c_str(),
					symbol.variableName.c_str(), Hex32(symbol.typeIndex).c_str(),
					symbol.section, HexOffset(symbol.sectionOffset).c_str(),
					FormatRangesDecimal(symbol.physicalRanges).c_str());
			}
		}
	}

	tsv.flush();
	if (!tsv)
	{
		std::fprintf(stderr, "error: failed writing %s\n", outputPath.c_str());
		return 3;
	}

	std::printf("wrote %u rows to %s\n", totalRows, outputPath.c_str());
	return totalRows == 6u ? 0 : 4;
}
