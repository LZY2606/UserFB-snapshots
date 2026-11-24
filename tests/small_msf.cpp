// Small MSF 2.00 container + module data-symbol location analysis tests.
//
// Registers three groups with task_support/driver.c:
//   directory : container detection/header validation, scattered directories, nil/empty
//               alignment, unsigned high page numbers, all Small page sizes/FPM positions.
//   streams   : Direct/Coalesced/ModuleSymbol reads, cross-page names, field-by-field
//               analysis of the three standard inputs, physical evidence reassembly.
//   ownership : move construction/assignment, self-move, pre-move streams, interleaved
//               images, and analysis results outliving temporary RawFiles/buffers.

#include "PDB_PCH.h"
#include "PDB.h"
#include "PDB_RawFile.h"
#include "PDB_DirectMSFStream.h"
#include "PDB_CoalescedMSFStream.h"
#include "PDB_ModuleSymbolStream.h"
#include "PDB_ModuleDataSymbolAnalysis.h"

#include "task_support/check.h"
#include "task_support/analysis_fixture.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using fixture::Bytes;

namespace
{
	// ---------------------------------------------------------------------
	// small helpers
	// ---------------------------------------------------------------------
	Bytes FullDirectRead(const PDB::RawFile& raw, uint32_t streamIndex)
	{
		PDB::DirectMSFStream direct = raw.CreateMSFStream<PDB::DirectMSFStream>(streamIndex);
		Bytes bytes(direct.GetSize());
		if (!bytes.empty())
		{
			direct.ReadAtOffset(bytes.data(), bytes.size(), 0u);
		}
		return bytes;
	}

	Bytes RebuildFromRanges(const Bytes& file, const std::vector<PDB::PhysicalFileRange>& ranges)
	{
		Bytes rebuilt;
		for (const PDB::PhysicalFileRange& range : ranges)
		{
			const size_t start = static_cast<size_t>(range.fileOffset);
			const size_t finish = start + range.size;
			if (finish > file.size())
			{
				rebuilt.clear();
				return rebuilt;
			}
			rebuilt.insert(rebuilt.end(), file.data() + start, file.data() + finish);
		}
		return rebuilt;
	}

	int CheckRangesEqual(const std::vector<PDB::PhysicalFileRange>& actual,
	                     const std::vector<PDB::PhysicalFileRange>& expected)
	{
		TASK_CHECK(actual.size() == expected.size());
		for (size_t i = 0; i < expected.size(); ++i)
		{
			TASK_CHECK(actual[i].fileOffset == expected[i].fileOffset);
			TASK_CHECK(actual[i].size == expected[i].size);
		}
		return 0;
	}

	int CheckStandardSymbol(const PDB::ModuleDataSymbol& symbol,
	                        const char* moduleName, uint32_t streamIndex,
	                        uint32_t typeIndex, uint16_t section, uint32_t sectionOffset,
	                        const std::vector<PDB::PhysicalFileRange>& expectedRanges)
	{
		TASK_CHECK(symbol.moduleName == moduleName);
		TASK_CHECK(symbol.variableName == "state");
		TASK_CHECK(symbol.streamIndex == streamIndex);
		TASK_CHECK(symbol.recordOffset == 1008u);
		TASK_CHECK(symbol.recordSize == 20u);
		TASK_CHECK(symbol.typeIndex == typeIndex);
		TASK_CHECK(symbol.section == section);
		TASK_CHECK(symbol.sectionOffset == sectionOffset);
		if (CheckRangesEqual(symbol.physicalRanges, expectedRanges))
		{
			return 1;
		}
		return 0;
	}

	// Reads the 20-byte variable record directly and compares it against both the source
	// stream bytes and the bytes rebuilt from the reported physical intervals.
	int CheckPhysicalEvidence(const Bytes& image, const Bytes& sourceStream,
	                          const PDB::RawFile& raw, const PDB::ModuleDataSymbol& symbol)
	{
		TASK_CHECK(symbol.physicalRanges.size() >= 1u);

		uint64_t totalMappedBytes = 0u;
		for (const PDB::PhysicalFileRange& range : symbol.physicalRanges)
		{
			TASK_CHECK(range.size > 0u);
			TASK_CHECK(range.fileOffset + range.size <= image.size());
			totalMappedBytes += range.size;
		}
		TASK_CHECK(totalMappedBytes == symbol.recordSize);

		const Bytes rebuilt = RebuildFromRanges(image, symbol.physicalRanges);
		TASK_CHECK(rebuilt.size() == symbol.recordSize);

		Bytes direct(symbol.recordSize);
		PDB::DirectMSFStream directStream = raw.CreateMSFStream<PDB::DirectMSFStream>(symbol.streamIndex);
		directStream.ReadAtOffset(direct.data(), symbol.recordSize, symbol.recordOffset);

		TASK_CHECK(rebuilt == direct);
		TASK_CHECK(sourceStream.size() >= symbol.recordOffset + symbol.recordSize);
		TASK_CHECK(std::memcmp(direct.data(), sourceStream.data() + symbol.recordOffset, symbol.recordSize) == 0);
		return 0;
	}

	// Fills stream 8 with the same pseudo-random 1100 bytes used by the original baseline.
	std::vector<Bytes> WindowStreams()
	{
		std::vector<Bytes> streams = fixture::analysis_streams(false);
		streams[8].resize(1100u);
		for (size_t i = 0; i < streams[8].size(); ++i)
		{
			streams[8][i] = static_cast<unsigned char>((i * 37u + 19u) & 255u);
		}
		return streams;
	}
}


// =====================================================================
// directory group
// =====================================================================

static int dir_container_info(void)
{
	const std::vector<Bytes> streams = fixture::analysis_streams(false);
	const Bytes smallImage = fixture::small_msf(streams);
	const Bytes msfImage = fixture::msf(streams);

	TASK_CHECK(PDB::ValidateFile(smallImage.data(), smallImage.size()) == PDB::ErrorCode::Success);
	TASK_CHECK(PDB::ValidateFile(msfImage.data(), msfImage.size()) == PDB::ErrorCode::Success);

	PDB::RawFile smallRaw = PDB::CreateRawFile(smallImage.data());
	const PDB::ContainerInfo smallInfo = smallRaw.GetContainerInfo();
	TASK_CHECK(smallInfo.format == PDB::ContainerFormat::SmallMSF2);
	TASK_CHECK(PDB::SmallMSF2 == PDB::ContainerFormat::SmallMSF2);
	TASK_CHECK(smallInfo.blockSize == 1024u);
	TASK_CHECK(smallInfo.freeBlockMapIndex == 1u);
	TASK_CHECK(smallInfo.blockCount == 48u);
	TASK_CHECK(smallInfo.directorySize == 84u);
	TASK_CHECK(smallRaw.GetStreamCount() == 9u);
	TASK_CHECK(smallRaw.GetSuperBlock() == nullptr);

	PDB::RawFile msfRaw = PDB::CreateRawFile(msfImage.data());
	const PDB::ContainerInfo msfInfo = msfRaw.GetContainerInfo();
	TASK_CHECK(msfInfo.format == PDB::ContainerFormat::MSF7);
	TASK_CHECK(PDB::MSF7 == PDB::ContainerFormat::MSF7);
	TASK_CHECK(msfInfo.blockSize == 512u);
	TASK_CHECK(msfInfo.freeBlockMapIndex == 1u);
	TASK_CHECK(msfInfo.blockCount == 17u);
	TASK_CHECK(msfInfo.directorySize == 64u);
	TASK_CHECK(msfRaw.GetStreamCount() == 9u);
	TASK_CHECK(msfRaw.GetSuperBlock() == static_cast<const PDB::SuperBlock*>(static_cast<const void*>(msfImage.data())));

	return 0;
}

static int dir_page_sizes_and_fpm(void)
{
	const unsigned pageSizes[3] = {1024u, 2048u, 4096u};
	const unsigned legalFpm[3][2] = {{1u, 9u}, {1u, 5u}, {1u, 2u}};
	const unsigned illegalFpm[3] = {2u, 2u, 5u};

	for (size_t p = 0; p < 3; ++p)
	{
		for (size_t f = 0; f < 2; ++f)
		{
			std::vector<Bytes> streams(3);
			streams[0] = Bytes {'p', 'a', 'g', 'e'};
			Bytes pattern(3000u);
			for (size_t i = 0; i < pattern.size(); ++i)
			{
				pattern[i] = static_cast<unsigned char>((i * 13u + p * 7u) & 255u);
			}
			streams[1] = pattern;
			streams[2] = Bytes {'t', 'a', 'i', 'l'};

			fixture::SmallMSFConfig config;
			config.pageSize = pageSizes[p];
			config.fpmPage = legalFpm[p][f];
			const Bytes image = fixture::small_msf_ex(streams, config);

			TASK_CHECK(PDB::ValidateFile(image.data(), image.size()) == PDB::ErrorCode::Success);
			PDB::RawFile raw = PDB::CreateRawFile(image.data());
			const PDB::ContainerInfo info = raw.GetContainerInfo();
			TASK_CHECK(info.format == PDB::ContainerFormat::SmallMSF2);
			TASK_CHECK(info.blockSize == pageSizes[p]);
			TASK_CHECK(info.freeBlockMapIndex == legalFpm[p][f]);
			TASK_CHECK(raw.GetSuperBlock() == nullptr);

			TASK_CHECK(FullDirectRead(raw, 0u) == streams[0]);
			TASK_CHECK(FullDirectRead(raw, 1u) == streams[1]);
			TASK_CHECK(FullDirectRead(raw, 2u) == streams[2]);

			PDB::CoalescedMSFStream coalesced = raw.CreateMSFStream<PDB::CoalescedMSFStream>(1u);
			TASK_CHECK(coalesced.GetSize() == pattern.size());
			TASK_CHECK(std::memcmp(coalesced.GetDataAtOffset<unsigned char>(0u), pattern.data(), pattern.size()) == 0);
		}

		// FPM positions that do not belong to this page size must be rejected.
		std::vector<Bytes> streams(1);
		streams[0] = Bytes {'x'};
		fixture::SmallMSFConfig badConfig;
		badConfig.pageSize = pageSizes[p];
		badConfig.fpmPage = illegalFpm[p];
		const Bytes badImage = fixture::small_msf_ex(streams, badConfig);
		TASK_CHECK(PDB::ValidateFile(badImage.data(), badImage.size()) != PDB::ErrorCode::Success);
	}
	return 0;
}

static int dir_bad_page_size_rejected(void)
{
	const std::vector<Bytes> streams = fixture::analysis_streams(false);
	Bytes image = fixture::small_msf(streams);

	const uint32_t illegalSizes[] = {0u, 512u, 8192u, 2048u + 1u, 1023u};
	for (uint32_t illegal : illegalSizes)
	{
		Bytes patched = image;
		patched[44] = static_cast<unsigned char>(illegal & 0xffu);
		patched[45] = static_cast<unsigned char>((illegal >> 8u) & 0xffu);
		patched[46] = static_cast<unsigned char>((illegal >> 16u) & 0xffu);
		patched[47] = static_cast<unsigned char>((illegal >> 24u) & 0xffu);
		TASK_CHECK(PDB::ValidateFile(patched.data(), patched.size()) != PDB::ErrorCode::Success);
	}

	// the unpatched image remains valid
	TASK_CHECK(PDB::ValidateFile(image.data(), image.size()) == PDB::ErrorCode::Success);
	return 0;
}

static int dir_truncated_header_rejected(void)
{
	const std::vector<Bytes> streams = fixture::analysis_streams(false);
	const Bytes image = fixture::small_msf(streams);

	static const size_t truncations[] = {0u, 1u, 43u, 44u, 52u, 59u, 60u, 61u, 62u, 100u, 1024u, 40000u};
	for (size_t newSize : truncations)
	{
		Bytes truncated(image.begin(), image.begin() + static_cast<std::ptrdiff_t>(std::min(newSize, image.size())));
		TASK_CHECK(PDB::ValidateFile(truncated.data(), truncated.size()) != PDB::ErrorCode::Success);
	}
	TASK_CHECK(PDB::ValidateFile(nullptr, 0u) != PDB::ErrorCode::Success);
	return 0;
}

static int dir_scattered_directory(void)
{
	const unsigned streamCount = 260u;
	std::vector<Bytes> streams(streamCount);
	std::vector<bool> nil(streamCount, false);

	streams[0] = Bytes {'f', 'i', 'r', 's', 't'};
	Bytes middle(3000u);
	for (size_t i = 0; i < middle.size(); ++i)
	{
		middle[i] = static_cast<unsigned char>((i * 29u + 7u) & 255u);
	}
	streams[127] = middle; // its descriptor straddles the first directory page boundary
	Bytes crossing(1500u);
	for (size_t i = 0; i < crossing.size(); ++i)
	{
		crossing[i] = static_cast<unsigned char>((i * 17u + 3u) & 255u);
	}
	streams[255] = crossing; // its descriptor straddles the second directory page boundary
	streams[259] = Bytes {'l', 'a', 's', 't'};
	nil[50] = true;
	nil[200] = true;

	fixture::SmallMSFConfig config;
	config.nil = nil;
	const Bytes image = fixture::small_msf_ex(streams, config);

	PDB::ErrorCode errorCode = PDB::ValidateFile(image.data(), image.size());
	TASK_CHECK(errorCode == PDB::ErrorCode::Success);

	PDB::RawFile raw = PDB::CreateRawFile(image.data());
	TASK_CHECK(raw.GetStreamCount() == streamCount);
	TASK_CHECK(raw.GetContainerInfo().directorySize > 2048u);
	TASK_CHECK(raw.GetStreamSize(50u) == 0u);
	TASK_CHECK(raw.GetStreamSize(200u) == 0u);

	TASK_CHECK(FullDirectRead(raw, 0u) == streams[0]);
	TASK_CHECK(FullDirectRead(raw, 127u) == middle);
	TASK_CHECK(FullDirectRead(raw, 255u) == crossing);
	TASK_CHECK(FullDirectRead(raw, 259u) == streams[259]);

	PDB::CoalescedMSFStream coalescedMiddle = raw.CreateMSFStream<PDB::CoalescedMSFStream>(127u);
	TASK_CHECK(coalescedMiddle.GetSize() == middle.size());
	TASK_CHECK(std::memcmp(coalescedMiddle.GetDataAtOffset<unsigned char>(0u), middle.data(), middle.size()) == 0);
	return 0;
}

static int dir_nil_empty_alignment(void)
{
	std::vector<Bytes> streams(6);
	streams[0] = Bytes {'n', 'o', 'r', 'm', 'a', 'l'};
	// stream 1 is nil, stream 2 stays default-constructed empty
	Bytes big(1500u);
	for (size_t i = 0; i < big.size(); ++i)
	{
		big[i] = static_cast<unsigned char>((i * 31u + 11u) & 255u);
	}
	streams[3] = big;
	// stream 4 is nil
	streams[5] = Bytes {'t', 'a', 'i', 'l'};

	std::vector<bool> nil(6, false);
	nil[1] = true;
	nil[4] = true;

	const Bytes image = fixture::small_msf(streams, nil);
	TASK_CHECK(PDB::ValidateFile(image.data(), image.size()) == PDB::ErrorCode::Success);

	PDB::RawFile raw = PDB::CreateRawFile(image.data());
	TASK_CHECK(raw.GetStreamCount() == 6u);
	TASK_CHECK(raw.GetStreamSize(1u) == 0u);
	TASK_CHECK(raw.GetStreamSize(2u) == 0u);
	TASK_CHECK(raw.GetStreamSize(4u) == 0u);

	// zero-length streams must construct without dereferencing a page number.
	PDB::DirectMSFStream nilDirect = raw.CreateMSFStream<PDB::DirectMSFStream>(1u);
	PDB::DirectMSFStream emptyDirect = raw.CreateMSFStream<PDB::DirectMSFStream>(2u);
	TASK_CHECK(nilDirect.GetSize() == 0u);
	TASK_CHECK(emptyDirect.GetSize() == 0u);
	unsigned char scratchBuffer[4] = {};
	nilDirect.ReadAtOffset(scratchBuffer, 0u, 0u);
	emptyDirect.ReadAtOffset(scratchBuffer, 0u, 0u);

	PDB::CoalescedMSFStream nilCoalesced = raw.CreateMSFStream<PDB::CoalescedMSFStream>(1u);
	PDB::CoalescedMSFStream emptyCoalesced = raw.CreateMSFStream<PDB::CoalescedMSFStream>(2u);
	TASK_CHECK(nilCoalesced.GetSize() == 0u);
	TASK_CHECK(emptyCoalesced.GetSize() == 0u);
	PDB::CoalescedMSFStream emptyWindow(emptyDirect, 0u, 0u);
	TASK_CHECK(emptyWindow.GetSize() == 0u);

	// later streams must not be misaligned by the nil/empty entries.
	TASK_CHECK(FullDirectRead(raw, 0u) == streams[0]);
	TASK_CHECK(FullDirectRead(raw, 3u) == big);
	TASK_CHECK(FullDirectRead(raw, 5u) == streams[5]);
	return 0;
}

static int dir_high_page_number(void)
{
	std::vector<Bytes> streams(3);
	streams[0] = Bytes {'h', 'i', 'g', 'h'};
	// stream 1 intentionally empty
	Bytes twoPages(1500u);
	for (size_t i = 0; i < twoPages.size(); ++i)
	{
		twoPages[i] = static_cast<unsigned char>((i * 7u + 1u) & 255u);
	}
	streams[2] = twoPages;

	fixture::SmallMSFConfig config;
	config.explicitPages.resize(3);
	config.explicitPages[0] = std::vector<unsigned> {0x8000u};
	config.explicitPages[2] = std::vector<unsigned> {0x8002u, 0x8005u};
	const Bytes image = fixture::small_msf_ex(streams, config);

	TASK_CHECK(PDB::ValidateFile(image.data(), image.size()) == PDB::ErrorCode::Success);
	PDB::RawFile raw = PDB::CreateRawFile(image.data());
	TASK_CHECK(raw.GetContainerInfo().blockCount == 0x8006u);

	TASK_CHECK(FullDirectRead(raw, 0u) == streams[0]);
	TASK_CHECK(FullDirectRead(raw, 2u) == twoPages);

	// logical [1000, 1500) splits inside page 0 and then into the disjoint second high page.
	const std::vector<PDB::PhysicalFileRange> ranges = raw.GetStreamPhysicalRanges(2u, 1000u, 500u);
	TASK_CHECK(ranges.size() == 2u);
	TASK_CHECK(ranges[0].fileOffset == static_cast<uint64_t>(0x8002u) * 1024u + 1000u);
	TASK_CHECK(ranges[0].size == 24u);
	TASK_CHECK(ranges[1].fileOffset == static_cast<uint64_t>(0x8005u) * 1024u);
	TASK_CHECK(ranges[1].size == 476u);

	// prove the high-bit page did not become a negative signed offset
	PDB::DirectMSFStream direct = raw.CreateMSFStream<PDB::DirectMSFStream>(2u);
	unsigned char piece[24] = {};
	direct.ReadAtOffset(piece, sizeof(piece), 1000u);
	TASK_CHECK(std::memcmp(piece, twoPages.data() + 1000u, sizeof(piece)) == 0);
	return 0;
}


// =====================================================================
// streams group
// =====================================================================

static int streams_three_input_fields(void)
{
	const std::vector<Bytes> standardStreams = fixture::analysis_streams(false);
	const Bytes msfImage = fixture::msf(standardStreams);
	const Bytes smallImage = fixture::small_msf(standardStreams);

	const std::vector<Bytes> changedStreams = fixture::analysis_streams(true);
	const Bytes movedImage = fixture::small_msf(changedStreams, std::vector<bool>(), 80u);

	struct InputExpectation
	{
		const Bytes* image;
		PDB::ContainerFormat format;
		uint32_t alphaOffset;
		std::vector<PDB::PhysicalFileRange> alphaRanges;
		std::vector<PDB::PhysicalFileRange> betaRanges;
	};

	const InputExpectation inputs[3] = {
		{
			&msfImage, PDB::ContainerFormat::MSF7, 0x40u,
			{ {4080ull, 16u}, {4608ull, 4u} },
			{ {7152ull, 16u}, {7680ull, 4u} }
		},
		{
			&smallImage, PDB::ContainerFormat::SmallMSF2, 0x40u,
			{ {41968ull, 16u}, {43008ull, 4u} },
			{ {46064ull, 16u}, {47104ull, 4u} }
		},
		{
			&movedImage, PDB::ContainerFormat::SmallMSF2, 0x60u,
			{ {82928ull, 16u}, {83968ull, 4u} },
			{ {87024ull, 16u}, {88064ull, 4u} }
		}
	};

	for (const InputExpectation& expected : inputs)
	{
		const Bytes& image = *expected.image;
		TASK_CHECK(PDB::ValidateFile(image.data(), image.size()) == PDB::ErrorCode::Success);
		PDB::RawFile raw = PDB::CreateRawFile(image.data());
		TASK_CHECK(raw.GetContainerInfo().format == expected.format);

		PDB::ModuleSymbolAnalysisResult alphaResult =
			PDB::AnalyzeModuleDataSymbols(raw, 6u, raw.GetStreamSize(6u));
		PDB::ModuleSymbolAnalysisResult betaResult =
			PDB::AnalyzeModuleDataSymbols(raw, 7u, raw.GetStreamSize(7u));
		TASK_CHECK(alphaResult.dataSymbols.size() == 1u);
		TASK_CHECK(betaResult.dataSymbols.size() == 1u);

		if (CheckStandardSymbol(alphaResult.dataSymbols[0], "alpha.obj", 6u, 0x1000u, 1u,
		                        expected.alphaOffset, expected.alphaRanges))
		{
			return 1;
		}
		if (CheckStandardSymbol(betaResult.dataSymbols[0], "beta.obj", 7u, 0x1001u, 2u, 0x88u,
		                        expected.betaRanges))
		{
			return 1;
		}
	}
	return 0;
}

static int streams_cross_page_records_and_windows(void)
{
	const std::vector<Bytes> sourceStreams = WindowStreams();
	const Bytes smallImage = fixture::small_msf(sourceStreams);
	const Bytes msfImage = fixture::msf(sourceStreams);

	for (int formatPass = 0; formatPass < 2; ++formatPass)
	{
		const Bytes& image = (formatPass == 0) ? smallImage : msfImage;
		TASK_CHECK(PDB::ValidateFile(image.data(), image.size()) == PDB::ErrorCode::Success);
		PDB::RawFile raw = PDB::CreateRawFile(image.data());

		// whole-stream direct + coalesced reads of stream 6, including the cross-page name
		PDB::DirectMSFStream direct6 = raw.CreateMSFStream<PDB::DirectMSFStream>(6u);
		TASK_CHECK(direct6.GetSize() == sourceStreams[6].size());
		TASK_CHECK(FullDirectRead(raw, 6u) == sourceStreams[6]);

		unsigned char nameBytes[6] = {};
		direct6.ReadAtOffset(nameBytes, 6u, 1022u);
		TASK_CHECK(std::memcmp(nameBytes, "state", 5u) == 0);
		TASK_CHECK(nameBytes[5] == 0);
		TASK_CHECK(std::memcmp(nameBytes, sourceStreams[6].data() + 1022u, 6u) == 0);

		unsigned char crossBoundary[16] = {};
		direct6.ReadAtOffset(crossBoundary, sizeof(crossBoundary), 1012u);
		TASK_CHECK(std::memcmp(crossBoundary, sourceStreams[6].data() + 1012u, sizeof(crossBoundary)) == 0);

		PDB::CoalescedMSFStream coalesced6 = raw.CreateMSFStream<PDB::CoalescedMSFStream>(6u);
		TASK_CHECK(coalesced6.GetSize() == sourceStreams[6].size());
		TASK_CHECK(std::memcmp(coalesced6.GetDataAtOffset<unsigned char>(0u), sourceStreams[6].data(), sourceStreams[6].size()) == 0);

		// unaligned sub-window crossing the page boundary inside stream 6 (ends at stream end 1028)
		PDB::CoalescedMSFStream crossWindow(direct6, 16u, 1012u);
		TASK_CHECK(crossWindow.GetSize() == 16u);
		TASK_CHECK(std::memcmp(crossWindow.GetDataAtOffset<unsigned char>(0u), sourceStreams[6].data() + 1012u, 16u) == 0);

		// baseline-style 1030-byte window at offset 29 on the 1100-byte stream 8
		PDB::DirectMSFStream direct8 = raw.CreateMSFStream<PDB::DirectMSFStream>(8u);
		Bytes windowBytes(1030u);
		direct8.ReadAtOffset(windowBytes.data(), windowBytes.size(), 29u);
		PDB::CoalescedMSFStream window(direct8, 1030u, 29u);
		TASK_CHECK(std::memcmp(window.GetDataAtOffset<unsigned char>(0u), windowBytes.data(), windowBytes.size()) == 0);
		TASK_CHECK(std::memcmp(window.GetDataAtOffset<unsigned char>(0u), sourceStreams[8].data() + 29u, 1030u) == 0);

		// ModuleSymbolStream enumeration and lookup
		PDB::ModuleSymbolStream symbols(raw, 6u, static_cast<uint32_t>(sourceStreams[6].size()));
		unsigned objNameCount = 0;
		unsigned uNamespaceCount = 0;
		unsigned gdataCount = 0;
		symbols.ForEachSymbol([&](const PDB::CodeView::DBI::Record* record)
		{
			if (record->header.kind == PDB::CodeView::DBI::SymbolRecordKind::S_OBJNAME) { ++objNameCount; }
			if (record->header.kind == PDB::CodeView::DBI::SymbolRecordKind::S_UNAMESPACE) { ++uNamespaceCount; }
			if (record->header.kind == PDB::CodeView::DBI::SymbolRecordKind::S_GDATA32) { ++gdataCount; }
		});
		TASK_CHECK(objNameCount == 1u);
		TASK_CHECK(uNamespaceCount == 1u);
		TASK_CHECK(gdataCount == 1u);
		TASK_CHECK(symbols.FindRecord(PDB::CodeView::DBI::SymbolRecordKind::S_OBJNAME) != nullptr);
		const PDB::CodeView::DBI::Record* gdataRecord =
			symbols.FindRecord(PDB::CodeView::DBI::SymbolRecordKind::S_GDATA32);
		TASK_CHECK(gdataRecord != nullptr);
		TASK_CHECK(gdataRecord->data.S_GDATA32.typeIndex == 0x1000u);
		TASK_CHECK(std::strcmp(gdataRecord->data.S_GDATA32.name, "state") == 0);
	}
	return 0;
}

static int streams_physical_reassembly(void)
{
	const std::vector<Bytes> standardStreams = fixture::analysis_streams(false);
	const std::vector<Bytes> changedStreams = fixture::analysis_streams(true);

	struct EvidenceInput
	{
		Bytes image;
		std::vector<Bytes> sourceStreams;
	};

	EvidenceInput inputs[3];
	inputs[0].sourceStreams = standardStreams;
	inputs[0].image = fixture::msf(standardStreams);
	inputs[1].sourceStreams = standardStreams;
	inputs[1].image = fixture::small_msf(standardStreams);
	inputs[2].sourceStreams = changedStreams;
	inputs[2].image = fixture::small_msf(changedStreams, std::vector<bool>(), 80u);

	for (const EvidenceInput& input : inputs)
	{
		PDB::RawFile raw = PDB::CreateRawFile(input.image.data());
		for (uint16_t streamIndex = 6u; streamIndex <= 7u; ++streamIndex)
		{
			PDB::ModuleSymbolAnalysisResult analysis =
				PDB::AnalyzeModuleDataSymbols(raw, streamIndex, raw.GetStreamSize(streamIndex));
			TASK_CHECK(analysis.dataSymbols.size() == 1u);
			if (CheckPhysicalEvidence(input.image, input.sourceStreams[streamIndex], raw, analysis.dataSymbols[0]))
			{
				return 1;
			}
		}
	}
	return 0;
}

static int streams_physical_pages_change_but_payload_same(void)
{
	const std::vector<Bytes> streams = fixture::analysis_streams(false);
	const Bytes page40 = fixture::small_msf(streams);
	const Bytes page80 = fixture::small_msf(fixture::analysis_streams(true), std::vector<bool>(), 80u);

	PDB::RawFile raw40 = PDB::CreateRawFile(page40.data());
	PDB::RawFile raw80 = PDB::CreateRawFile(page80.data());

	const auto beta40 = PDB::AnalyzeModuleDataSymbols(raw40, 7u, raw40.GetStreamSize(7u));
	const auto beta80 = PDB::AnalyzeModuleDataSymbols(raw80, 7u, raw80.GetStreamSize(7u));
	TASK_CHECK(beta40.dataSymbols.size() == 1u);
	TASK_CHECK(beta80.dataSymbols.size() == 1u);

	const PDB::ModuleDataSymbol& a = beta40.dataSymbols[0];
	const PDB::ModuleDataSymbol& b = beta80.dataSymbols[0];

	// beta's payload is identical in both containers ...
	TASK_CHECK(a.moduleName == b.moduleName);
	TASK_CHECK(a.variableName == b.variableName);
	TASK_CHECK(a.typeIndex == b.typeIndex);
	TASK_CHECK(a.section == b.section);
	TASK_CHECK(a.sectionOffset == b.sectionOffset);
	TASK_CHECK(a.recordOffset == b.recordOffset);

	// ... but its physical evidence moved with firstDataPage 40 -> 80.
	TASK_CHECK(a.physicalRanges != b.physicalRanges);
	if (CheckRangesEqual(a.physicalRanges, { {46064ull, 16u}, {47104ull, 4u} })) { return 1; }
	if (CheckRangesEqual(b.physicalRanges, { {87024ull, 16u}, {88064ull, 4u} })) { return 1; }

	// alpha's recovered section offset is the one field the third input actually changes.
	const auto alpha80 = PDB::AnalyzeModuleDataSymbols(raw80, 6u, raw80.GetStreamSize(6u));
	TASK_CHECK(alpha80.dataSymbols.size() == 1u);
	TASK_CHECK(alpha80.dataSymbols[0].sectionOffset == 0x60u);
	return 0;
}


// =====================================================================
// ownership group
// =====================================================================

static int own_move_construction_small(void)
{
	const Bytes image = fixture::small_msf(fixture::analysis_streams(false));

	PDB::DirectMSFStream survivingDirect;
	PDB::ModuleSymbolAnalysisResult survivingAnalysis;
	std::unique_ptr<PDB::RawFile> destination;

	{
		PDB::RawFile source = PDB::CreateRawFile(image.data());
		survivingDirect = source.CreateMSFStream<PDB::DirectMSFStream>(6u);
		survivingAnalysis = PDB::AnalyzeModuleDataSymbols(source, 6u, source.GetStreamSize(6u));
		destination.reset(new PDB::RawFile(std::move(source)));
	}
	// source has now been destroyed; destination and the original input stay alive.

	TASK_CHECK(destination->GetContainerInfo().format == PDB::ContainerFormat::SmallMSF2);
	TASK_CHECK(destination->GetStreamCount() == 9u);
	TASK_CHECK(survivingDirect.GetSize() == 1028u);

	unsigned char name[6] = {};
	survivingDirect.ReadAtOffset(name, 6u, 1022u);
	TASK_CHECK(std::memcmp(name, "state", 5u) == 0);

	TASK_CHECK(survivingAnalysis.dataSymbols.size() == 1u);
	TASK_CHECK(survivingAnalysis.dataSymbols[0].moduleName == "alpha.obj");
	TASK_CHECK(survivingAnalysis.dataSymbols[0].physicalRanges.size() == 2u);

	// streams created from the destination itself keep working too
	TASK_CHECK(FullDirectRead(*destination, 7u) == fixture::analysis_streams(false)[7]);
	return 0;
}

static int own_move_construction_msf7(void)
{
	const Bytes image = fixture::msf(fixture::analysis_streams(false));

	PDB::DirectMSFStream survivingDirect;
	std::unique_ptr<PDB::RawFile> destination;
	{
		PDB::RawFile source = PDB::CreateRawFile(image.data());
		survivingDirect = source.CreateMSFStream<PDB::DirectMSFStream>(7u);
		destination.reset(new PDB::RawFile(std::move(source)));
	}

	TASK_CHECK(destination->GetContainerInfo().format == PDB::ContainerFormat::MSF7);
	TASK_CHECK(destination->GetSuperBlock() == static_cast<const PDB::SuperBlock*>(static_cast<const void*>(image.data())));
	TASK_CHECK(survivingDirect.GetSize() == fixture::analysis_streams(false)[7].size());

	Bytes got(survivingDirect.GetSize());
	survivingDirect.ReadAtOffset(got.data(), got.size(), 0u);
	TASK_CHECK(got == fixture::analysis_streams(false)[7]);
	return 0;
}

static int own_cross_format_move_assignment(void)
{
	const Bytes msfImage = fixture::msf(fixture::analysis_streams(false));
	const Bytes smallImage = fixture::small_msf(fixture::analysis_streams(false));

	// MSF7 destination is move-assigned a Small MSF source. A stream created from the source
	// before the assignment must keep working after the temporary source is destroyed.
	PDB::RawFile destination = PDB::CreateRawFile(msfImage.data());
	TASK_CHECK(destination.GetContainerInfo().format == PDB::ContainerFormat::MSF7);
	PDB::DirectMSFStream survivingAssignedStream;
	{
		PDB::RawFile source = PDB::CreateRawFile(smallImage.data());
		survivingAssignedStream = source.CreateMSFStream<PDB::DirectMSFStream>(7u);
		destination = std::move(source);
	}
	TASK_CHECK(destination.GetContainerInfo().format == PDB::ContainerFormat::SmallMSF2);
	TASK_CHECK(destination.GetSuperBlock() == nullptr);
	TASK_CHECK(destination.GetContainerInfo().blockCount == 48u);
	TASK_CHECK(FullDirectRead(destination, 6u) == fixture::analysis_streams(false)[6]);
	TASK_CHECK(survivingAssignedStream.GetSize() == fixture::analysis_streams(false)[7].size());
	{
		Bytes movedStreamBytes(survivingAssignedStream.GetSize());
		survivingAssignedStream.ReadAtOffset(movedStreamBytes.data(), movedStreamBytes.size(), 0u);
		TASK_CHECK(movedStreamBytes == fixture::analysis_streams(false)[7]);
	}
	{
		const auto analysis = PDB::AnalyzeModuleDataSymbols(destination, 6u, destination.GetStreamSize(6u));
		TASK_CHECK(analysis.dataSymbols.size() == 1u);
		TASK_CHECK(analysis.dataSymbols[0].typeIndex == 0x1000u);
	}

	// Small destination is move-assigned an MSF7 source.
	PDB::RawFile reverseDestination = PDB::CreateRawFile(smallImage.data());
	TASK_CHECK(reverseDestination.GetContainerInfo().format == PDB::ContainerFormat::SmallMSF2);
	{
		PDB::RawFile source = PDB::CreateRawFile(msfImage.data());
		reverseDestination = std::move(source);
	}
	TASK_CHECK(reverseDestination.GetContainerInfo().format == PDB::ContainerFormat::MSF7);
	TASK_CHECK(reverseDestination.GetSuperBlock() == static_cast<const PDB::SuperBlock*>(static_cast<const void*>(msfImage.data())));
	TASK_CHECK(reverseDestination.GetContainerInfo().blockCount == 17u);
	TASK_CHECK(FullDirectRead(reverseDestination, 7u) == fixture::analysis_streams(false)[7]);
	return 0;
}

static int own_self_move_assignment(void)
{
	const Bytes image = fixture::small_msf(fixture::analysis_streams(false));
	PDB::RawFile raw = PDB::CreateRawFile(image.data());

	PDB::RawFile& alias = raw;
	raw = std::move(alias); // must be a no-op self-move

	TASK_CHECK(raw.GetContainerInfo().format == PDB::ContainerFormat::SmallMSF2);
	TASK_CHECK(raw.GetContainerInfo().blockCount == 48u);
	TASK_CHECK(raw.GetStreamCount() == 9u);
	TASK_CHECK(FullDirectRead(raw, 6u) == fixture::analysis_streams(false)[6]);

	const auto analysis = PDB::AnalyzeModuleDataSymbols(raw, 6u, raw.GetStreamSize(6u));
	TASK_CHECK(analysis.dataSymbols.size() == 1u);
	TASK_CHECK(analysis.dataSymbols[0].variableName == "state");
	return 0;
}

static int own_interleaved_two_images(void)
{
	const std::vector<Bytes> sourceStreams = WindowStreams();
	const Bytes small40 = fixture::small_msf(sourceStreams);
	const Bytes small80 = fixture::small_msf(fixture::analysis_streams(true), std::vector<bool>(), 80u);
	const Bytes msfImage = fixture::msf(sourceStreams);

	PDB::RawFile raw40 = PDB::CreateRawFile(small40.data());
	PDB::RawFile raw80 = PDB::CreateRawFile(small80.data());
	PDB::RawFile rawMsf = PDB::CreateRawFile(msfImage.data());

	const std::vector<Bytes> changedStreams = fixture::analysis_streams(true);

	for (unsigned iteration = 0; iteration < 64u; ++iteration)
	{
		TASK_CHECK(FullDirectRead(raw40, 6u) == sourceStreams[6]);
		TASK_CHECK(FullDirectRead(raw80, 6u) == changedStreams[6]);
		TASK_CHECK(FullDirectRead(rawMsf, 6u) == sourceStreams[6]);

		TASK_CHECK(FullDirectRead(raw40, 7u) == sourceStreams[7]);
		TASK_CHECK(FullDirectRead(raw80, 7u) == changedStreams[7]);
		TASK_CHECK(FullDirectRead(rawMsf, 7u) == sourceStreams[7]);

		TASK_CHECK(FullDirectRead(raw40, 8u) == sourceStreams[8]);
		TASK_CHECK(FullDirectRead(rawMsf, 8u) == sourceStreams[8]);

		const auto a40 = PDB::AnalyzeModuleDataSymbols(raw40, 6u, raw40.GetStreamSize(6u));
		const auto a80 = PDB::AnalyzeModuleDataSymbols(raw80, 6u, raw80.GetStreamSize(6u));
		TASK_CHECK(a40.dataSymbols.size() == 1u);
		TASK_CHECK(a80.dataSymbols.size() == 1u);
		TASK_CHECK(a40.dataSymbols[0].sectionOffset == 0x40u);
		TASK_CHECK(a80.dataSymbols[0].sectionOffset == 0x60u);
		TASK_CHECK(a40.dataSymbols[0].physicalRanges != a80.dataSymbols[0].physicalRanges);
	}
	return 0;
}

static int own_analysis_outlives_temporaries(void)
{
	PDB::ModuleSymbolAnalysisResult detached;
	{
		const std::vector<Bytes> temporaryStreams = fixture::analysis_streams(false);
		Bytes temporaryImage = fixture::small_msf(temporaryStreams);
		PDB::RawFile temporaryRaw = PDB::CreateRawFile(temporaryImage.data());
		detached = PDB::AnalyzeModuleDataSymbols(temporaryRaw, 6u, temporaryRaw.GetStreamSize(6u));
		// RawFile and image are destroyed here; the result must own its strings/values.
	}

	TASK_CHECK(detached.dataSymbols.size() == 1u);
	const PDB::ModuleDataSymbol& symbol = detached.dataSymbols[0];
	TASK_CHECK(symbol.moduleName == "alpha.obj");
	TASK_CHECK(symbol.variableName == "state");
	TASK_CHECK(symbol.typeIndex == 0x1000u);
	TASK_CHECK(symbol.section == 1u);
	TASK_CHECK(symbol.sectionOffset == 0x40u);
	TASK_CHECK(symbol.streamIndex == 6u);
	TASK_CHECK(symbol.recordOffset == 1008u);
	TASK_CHECK(symbol.physicalRanges.size() == 2u);
	TASK_CHECK(symbol.physicalRanges[0].fileOffset == 41968ull);
	return 0;
}


// =====================================================================
// registration consumed by task_support/driver.c
// =====================================================================

const task_case task_cases[] = {
	{"directory", "container_info", dir_container_info},
	{"directory", "page_sizes_and_fpm", dir_page_sizes_and_fpm},
	{"directory", "bad_page_size_rejected", dir_bad_page_size_rejected},
	{"directory", "truncated_header_rejected", dir_truncated_header_rejected},
	{"directory", "scattered_directory", dir_scattered_directory},
	{"directory", "nil_empty_alignment", dir_nil_empty_alignment},
	{"directory", "high_page_number", dir_high_page_number},

	{"streams", "three_input_fields", streams_three_input_fields},
	{"streams", "cross_page_records_and_windows", streams_cross_page_records_and_windows},
	{"streams", "physical_reassembly", streams_physical_reassembly},
	{"streams", "physical_pages_change_but_payload_same", streams_physical_pages_change_but_payload_same},

	{"ownership", "move_construction_small", own_move_construction_small},
	{"ownership", "move_construction_msf7", own_move_construction_msf7},
	{"ownership", "cross_format_move_assignment", own_cross_format_move_assignment},
	{"ownership", "self_move_assignment", own_self_move_assignment},
	{"ownership", "interleaved_two_images", own_interleaved_two_images},
	{"ownership", "analysis_outlives_temporaries", own_analysis_outlives_temporaries}
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
