// Copyright 2011-2022, Molecular Matters GmbH <office@molecular-matters.com>
// See LICENSE.txt for licensing details (2-clause BSD License: https://opensource.org/licenses/BSD-2-Clause)

#include "PDB_PCH.h"
#include "PDB_RawFile.h"
#include "PDB_Types.h"
#include "PDB_Util.h"
#include "PDB_DirectMSFStream.h"
#include "Foundation/PDB_PointerUtil.h"
#include "Foundation/PDB_Memory.h"
#include "Foundation/PDB_Assert.h"


namespace
{
	// Decodes an unsigned 16-bit little-endian value. Small MSF page numbers are unsigned:
	// a page with the high bit set must never be interpreted as a negative number.
	PDB_NO_DISCARD static inline uint32_t ReadU16LE(const PDB::Byte* p) PDB_NO_EXCEPT
	{
		return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8u);
	}

	PDB_NO_DISCARD static inline uint32_t ReadU32LE(const PDB::Byte* p) PDB_NO_EXCEPT
	{
		return static_cast<uint32_t>(p[0])
			| (static_cast<uint32_t>(p[1]) << 8u)
			| (static_cast<uint32_t>(p[2]) << 16u)
			| (static_cast<uint32_t>(p[3]) << 24u);
	}

	// Shared target for block lists of nil/empty streams, so no non-existent page number is ever dereferenced.
	static const uint32_t EMPTY_BLOCK_LIST = 0u;
}


// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
PDB::RawFile::RawFile(RawFile&& other) PDB_NO_EXCEPT
	: m_data(PDB_MOVE(other.m_data))
	, m_superBlock(PDB_MOVE(other.m_superBlock))
	, m_containerInfo(PDB_MOVE(other.m_containerInfo))
	, m_directoryStream(PDB_MOVE(other.m_directoryStream))
	, m_streamCount(PDB_MOVE(other.m_streamCount))
	, m_streamSizes(PDB_MOVE(other.m_streamSizes))
	, m_streamBlocks(PDB_MOVE(other.m_streamBlocks))
	, m_normalizedStreamSizes(PDB_MOVE(other.m_normalizedStreamSizes))
	, m_normalizedBlockData(PDB_MOVE(other.m_normalizedBlockData))
{
	// m_streamSizes and the inner pointers of m_streamBlocks may point into the normalized
	// vectors (or into m_directoryStream). Moving those members transfers the same heap
	// addresses, so the pointers stay valid.
	other.m_data = nullptr;
	other.m_superBlock = nullptr;
	other.m_containerInfo = ContainerInfo {};
	other.m_streamCount = 0u;
	other.m_streamSizes = nullptr;
	other.m_streamBlocks = nullptr;
}


// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
PDB::RawFile& PDB::RawFile::operator=(RawFile&& other) PDB_NO_EXCEPT
{
	if (this != &other)
	{
		// only the outer pointer array is owned directly; normalized block lists live in
		// m_normalizedBlockData and clean themselves up.
		PDB_DELETE_ARRAY(m_streamBlocks);

		m_data = PDB_MOVE(other.m_data);
		m_superBlock = PDB_MOVE(other.m_superBlock);
		m_containerInfo = PDB_MOVE(other.m_containerInfo);
		m_directoryStream = PDB_MOVE(other.m_directoryStream);
		m_streamCount = PDB_MOVE(other.m_streamCount);
		m_streamSizes = PDB_MOVE(other.m_streamSizes);
		m_streamBlocks = PDB_MOVE(other.m_streamBlocks);
		m_normalizedStreamSizes = PDB_MOVE(other.m_normalizedStreamSizes);
		m_normalizedBlockData = PDB_MOVE(other.m_normalizedBlockData);

		other.m_data = nullptr;
		other.m_superBlock = nullptr;
		other.m_containerInfo = ContainerInfo {};
		other.m_streamCount = 0u;
		other.m_streamSizes = nullptr;
		other.m_streamBlocks = nullptr;
	}

	return *this;
}


// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
PDB::RawFile::RawFile(const void* data) PDB_NO_EXCEPT
	: m_data(data)
	, m_superBlock(nullptr)
	, m_containerInfo()
	, m_directoryStream()
	, m_streamCount(0u)
	, m_streamSizes(nullptr)
	, m_streamBlocks(nullptr)
	, m_normalizedStreamSizes()
	, m_normalizedBlockData()
{
	PDB_ASSERT(data != nullptr, "RawFile needs valid data.");

	// ValidateFile has already established that enough bytes exist for the matching magic.
	if (std::memcmp(data, SmallMSFHeader::MAGIC, SmallMSFHeader::MagicSize) == 0)
	{
		InitializeSmallMSF2(data);
	}
	else
	{
		InitializeMSF7(data);
	}
}


// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void PDB::RawFile::InitializeMSF7(const void* data) PDB_NO_EXCEPT
{
	m_superBlock = Pointer::Offset<const SuperBlock*>(data, 0u);
	m_containerInfo.format = ContainerFormat::MSF7;
	m_containerInfo.blockSize = m_superBlock->blockSize;
	m_containerInfo.freeBlockMapIndex = m_superBlock->freeBlockMapIndex;
	m_containerInfo.blockCount = m_superBlock->blockCount;
	m_containerInfo.directorySize = m_superBlock->directorySize;

	// the SuperBlock stores an array of indices of blocks that make up the indices of directory blocks, which need to be stitched together to form the directory.
	// the blocks holding the indices of directory blocks are not necessarily contiguous, so they need to be coalesced first.
	const uint32_t directoryBlockCount = PDB::ConvertSizeToBlockCount(m_superBlock->directorySize, m_superBlock->blockSize);

	// the directory is made up of directoryBlockCount blocks, so we need that many indices to be read from the blocks that make up the indices
	CoalescedMSFStream directoryIndicesStream(data, m_superBlock->blockSize, m_superBlock->directoryBlockIndices, directoryBlockCount * sizeof(uint32_t));

	// these are the indices of blocks making up the directory stream, now guaranteed to be contiguous
	const uint32_t* directoryIndices = directoryIndicesStream.GetDataAtOffset<uint32_t>(0u);

	m_directoryStream = CoalescedMSFStream(data, m_superBlock->blockSize, directoryIndices, m_superBlock->directorySize);

	// https://llvm.org/docs/PDB/MsfFile.html#the-stream-directory
	// parse the directory from its contiguous version. the directory matches the following struct:
	//	struct StreamDirectory
	//	{
	//		uint32_t streamCount;
	//		uint32_t streamSizes[streamCount];
	//		uint32_t streamBlocks[streamCount][];
	//	};
	m_streamCount = *m_directoryStream.GetDataAtOffset<uint32_t>(0u);

	// we can assign pointers into the stream directly, since the RawFile keeps ownership of the directory stream
	m_streamSizes = m_directoryStream.GetDataAtOffset<uint32_t>(sizeof(uint32_t));
	const uint32_t* directoryStreamBlocks = m_directoryStream.GetDataAtOffset<uint32_t>(sizeof(uint32_t) + sizeof(uint32_t) * m_streamCount);

	// prepare indices for directly accessing individual streams
	m_streamBlocks = PDB_NEW_ARRAY(const uint32_t*, m_streamCount);

	const uint32_t* indicesForCurrentBlock = directoryStreamBlocks;
	for (uint32_t i = 0u; i < m_streamCount; ++i)
	{
		const uint32_t sizeInBytes = GetStreamSize(i);
		const uint32_t blockCount = ConvertSizeToBlockCount(sizeInBytes, m_superBlock->blockSize);
		m_streamBlocks[i] = indicesForCurrentBlock;

		indicesForCurrentBlock += blockCount;
	}
}


// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void PDB::RawFile::InitializeSmallMSF2(const void* data) PDB_NO_EXCEPT
{
	const SmallMSFHeader* header = Pointer::Offset<const SmallMSFHeader*>(data, 0u);

	const uint32_t blockSize = header->blockSize;
	const uint32_t directorySize = header->directorySize;

	m_containerInfo.format = ContainerFormat::SmallMSF2;
	m_containerInfo.blockSize = blockSize;
	m_containerInfo.freeBlockMapIndex = header->freeBlockMapIndex;
	m_containerInfo.blockCount = header->blockCount;
	m_containerInfo.directorySize = directorySize;

	// the fixed header is followed directly by unsigned 16-bit directory data page numbers.
	const uint32_t directoryBlockCount = ConvertSizeToBlockCount(directorySize, blockSize);
	std::vector<uint32_t> directoryBlockIndices(directoryBlockCount);
	for (uint32_t i = 0u; i < directoryBlockCount; ++i)
	{
		directoryBlockIndices[i] = header->directoryBlockIndices[i];
	}

	// stitch the potentially discontinuous directory pages into contiguous bytes.
	m_directoryStream = CoalescedMSFStream(data, blockSize, directoryBlockIndices.data(), directorySize);

	// Logical Small MSF directory:
	//	uint16_t streamCount;
	//	uint16_t reserved;
	//	struct { uint32_t byteSize; uint32_t obsoleteMemoryPointer; } descriptors[streamCount];
	//	uint16_t streamBlocks[streamCount][];
	// The obsolete pointer slots never identify file offsets and are ignored.
	const Byte* directory = m_directoryStream.GetDataAtOffset<Byte>(0u);

	const uint32_t streamCount = ReadU16LE(directory + 0u);
	m_streamCount = streamCount;

	m_normalizedStreamSizes.resize(streamCount);
	for (uint32_t i = 0u; i < streamCount; ++i)
	{
		const size_t descriptorOffset = 4u + static_cast<size_t>(i) * 8u;
		m_normalizedStreamSizes[i] = ReadU32LE(directory + descriptorOffset);
		// bytes [descriptorOffset + 4, descriptorOffset + 8) are the obsolete memory pointer.
	}
	m_streamSizes = m_normalizedStreamSizes.data();

	// nil (0xffffffff) and empty sizes both consume zero page numbers.
	std::vector<uint32_t> blockCounts(streamCount);
	uint32_t totalBlockCount = 0u;
	for (uint32_t i = 0u; i < streamCount; ++i)
	{
		const uint32_t sizeInBytes = GetStreamSize(i);
		blockCounts[i] = ConvertSizeToBlockCount(sizeInBytes, blockSize);
		totalBlockCount += blockCounts[i];
	}

	m_normalizedBlockData.resize(totalBlockCount);
	m_streamBlocks = PDB_NEW_ARRAY(const uint32_t*, streamCount);

	const size_t pageListStart = 4u + static_cast<size_t>(streamCount) * 8u;
	uint32_t nextStorageIndex = 0u;
	uint32_t nextDirectoryPage = 0u;
	for (uint32_t i = 0u; i < streamCount; ++i)
	{
		const uint32_t blockCount = blockCounts[i];
		if (blockCount == 0u)
		{
			// nil and empty streams must not hand out a pointer to a non-existent page number.
			m_streamBlocks[i] = &EMPTY_BLOCK_LIST;
			continue;
		}

		m_streamBlocks[i] = m_normalizedBlockData.data() + nextStorageIndex;
		for (uint32_t j = 0u; j < blockCount; ++j)
		{
			const size_t pageNumberOffset = pageListStart + static_cast<size_t>(nextDirectoryPage + j) * sizeof(uint16_t);
			m_normalizedBlockData[nextStorageIndex + j] = ReadU16LE(directory + pageNumberOffset);
		}

		nextStorageIndex += blockCount;
		nextDirectoryPage += blockCount;
	}
}


// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
PDB::RawFile::~RawFile(void) PDB_NO_EXCEPT
{
	PDB_DELETE_ARRAY(m_streamBlocks);
}


// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
PDB_NO_DISCARD std::vector<PDB::PhysicalFileRange> PDB::RawFile::GetStreamPhysicalRanges(uint32_t streamIndex, uint32_t offset, uint32_t size) const PDB_NO_EXCEPT
{
	PDB_ASSERT(streamIndex != PDB::NilStreamIndex, "Invalid stream index.");
	PDB_ASSERT(streamIndex < m_streamCount, "Invalid stream index.");

	std::vector<PhysicalFileRange> ranges;
	if (streamIndex >= m_streamCount)
	{
		return ranges;
	}

	const uint32_t blockSize = m_containerInfo.blockSize;
	const uint32_t streamSize = GetStreamSize(streamIndex);
	PDB_ASSERT(static_cast<uint64_t>(offset) + size <= streamSize, "Range exceeds stream size.");
	if (static_cast<uint64_t>(offset) + size > streamSize)
	{
		return ranges;
	}

	const uint32_t* const blockIndices = m_streamBlocks[streamIndex];
	uint32_t currentOffset = offset;
	uint32_t bytesLeft = size;

	while (bytesLeft != 0u)
	{
		const uint32_t logicalBlockIndex = currentOffset / blockSize;
		const uint32_t offsetWithinBlock = currentOffset - logicalBlockIndex * blockSize;
		const uint32_t bytesLeftInBlock = blockSize - offsetWithinBlock;
		const uint32_t chunkSize = (bytesLeft < bytesLeftInBlock) ? bytesLeft : bytesLeftInBlock;

		// block indices are normalized to unsigned uint32 for both container formats.
		const uint32_t physicalBlockIndex = blockIndices[logicalBlockIndex];
		const uint64_t fileOffset = static_cast<uint64_t>(physicalBlockIndex) * static_cast<uint64_t>(blockSize) + offsetWithinBlock;
		ranges.push_back(PhysicalFileRange { fileOffset, chunkSize });

		currentOffset += chunkSize;
		bytesLeft -= chunkSize;
	}

	return ranges;
}


// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
template <typename T>
PDB_NO_DISCARD T PDB::RawFile::CreateMSFStream(uint32_t streamIndex) const PDB_NO_EXCEPT
{
	PDB_ASSERT(streamIndex != PDB::NilStreamIndex, "Invalid stream index.");
	PDB_ASSERT(streamIndex < m_streamCount, "Invalid stream index.");

	return T(m_data, m_containerInfo.blockSize, m_streamBlocks[streamIndex], GetStreamSize(streamIndex));
}


// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
template <typename T>
PDB_NO_DISCARD T PDB::RawFile::CreateMSFStream(uint32_t streamIndex, uint32_t streamSize) const PDB_NO_EXCEPT
{
	PDB_ASSERT(streamIndex != PDB::NilStreamIndex, "Invalid stream index.");
	PDB_ASSERT(streamIndex < m_streamCount, "Invalid stream index.");
	PDB_ASSERT(streamSize <= GetStreamSize(streamIndex), "Invalid stream size.");

	return T(m_data, m_containerInfo.blockSize, m_streamBlocks[streamIndex], streamSize);
}


// explicit template instantiation
template PDB::CoalescedMSFStream PDB::RawFile::CreateMSFStream<PDB::CoalescedMSFStream>(uint32_t streamIndex) const PDB_NO_EXCEPT;
template PDB::DirectMSFStream PDB::RawFile::CreateMSFStream<PDB::DirectMSFStream>(uint32_t streamIndex) const PDB_NO_EXCEPT;

template PDB::CoalescedMSFStream PDB::RawFile::CreateMSFStream<PDB::CoalescedMSFStream>(uint32_t streamIndex, uint32_t streamSize) const PDB_NO_EXCEPT;
template PDB::DirectMSFStream PDB::RawFile::CreateMSFStream<PDB::DirectMSFStream>(uint32_t streamIndex, uint32_t streamSize) const PDB_NO_EXCEPT;
