// Copyright 2011-2022, Molecular Matters GmbH <office@molecular-matters.com>
// See LICENSE.txt for licensing details (2-clause BSD License: https://opensource.org/licenses/BSD-2-Clause)

#include "PDB_PCH.h"
#include "PDB.h"
#include "PDB_Types.h"
#include "PDB_Util.h"
#include "PDB_RawFile.h"
#include "Foundation/PDB_PointerUtil.h"


namespace
{
	// Decodes an unsigned 16-bit little-endian value.
	static inline uint32_t ValidateReadU16LE(const void* p) PDB_NO_EXCEPT
	{
		const unsigned char* b = static_cast<const unsigned char*>(p);
		return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8u);
	}

	// Decodes an unsigned 32-bit little-endian value.
	static inline uint32_t ValidateReadU32LE(const void* p) PDB_NO_EXCEPT
	{
		const unsigned char* b = static_cast<const unsigned char*>(p);
		return static_cast<uint32_t>(b[0])
			| (static_cast<uint32_t>(b[1]) << 8u)
			| (static_cast<uint32_t>(b[2]) << 16u)
			| (static_cast<uint32_t>(b[3]) << 24u);
	}

	// Validates a Small MSF 2.00 container. See task_support/FORMAT.md.
	PDB_NO_DISCARD PDB::ErrorCode ValidateSmallMSF2(const void* data, size_t size) PDB_NO_EXCEPT
	{
		// fixed header is 60 bytes (44-byte magic + page size/FPM/count/directory size/obsolete slot)
		if (size < sizeof(PDB::SmallMSFHeader))
		{
			return PDB::ErrorCode::InvalidDataSize;
		}

		const PDB::SmallMSFHeader* header = static_cast<const PDB::SmallMSFHeader*>(data);
		if (memcmp(header->fileMagic, PDB::SmallMSFHeader::MAGIC, PDB::SmallMSFHeader::MagicSize) != 0)
		{
			return PDB::ErrorCode::InvalidSuperBlock;
		}

		const uint32_t blockSize = header->blockSize;

		// Small MSF 2.00 only supports these three page sizes.
		if (blockSize != 1024u && blockSize != 2048u && blockSize != 4096u)
		{
			return PDB::ErrorCode::InvalidSuperBlock;
		}

		const uint32_t freeBlockMapIndex = header->freeBlockMapIndex;

		// each page size has its own legal FPM positions:
		// 1024 -> (1, 9), 2048 -> (1, 5), 4096 -> (1, 2)
		const bool validFreeBlockMap =
			(blockSize == 1024u && (freeBlockMapIndex == 1u || freeBlockMapIndex == 9u)) ||
			(blockSize == 2048u && (freeBlockMapIndex == 1u || freeBlockMapIndex == 5u)) ||
			(blockSize == 4096u && (freeBlockMapIndex == 1u || freeBlockMapIndex == 2u));
		if (!validFreeBlockMap)
		{
			return PDB::ErrorCode::InvalidFreeBlockMap;
		}

		const uint32_t blockCount = header->blockCount;
		if (blockCount == 0u)
		{
			return PDB::ErrorCode::InvalidSuperBlock;
		}

		// blockCount * blockSize is the expected size of the container on disk
		const uint64_t expectedFileSize = static_cast<uint64_t>(blockCount) * static_cast<uint64_t>(blockSize);
		if (size < expectedFileSize)
		{
			return PDB::ErrorCode::InvalidDataSize;
		}

		const uint32_t directorySize = header->directorySize;
		if (directorySize < 4u)
		{
			return PDB::ErrorCode::InvalidDataSize;
		}

		const uint32_t directoryBlockCount = PDB::ConvertSizeToBlockCount(directorySize, blockSize);

		// the direct directory page-number list follows the 60-byte fixed header
		const uint64_t pageListBytes = static_cast<uint64_t>(directoryBlockCount) * sizeof(uint16_t);
		if (static_cast<uint64_t>(sizeof(PDB::SmallMSFHeader)) + pageListBytes > size)
		{
			return PDB::ErrorCode::InvalidDataSize;
		}

		// every directory page must be a known page of the file
		for (uint32_t i = 0u; i < directoryBlockCount; ++i)
		{
			const uint32_t directoryPageIndex = header->directoryBlockIndices[i];
			if (directoryPageIndex >= blockCount)
			{
				return PDB::ErrorCode::InvalidDataSize;
			}
		}

		// structurally validate the coalesced directory so RawFile parsing cannot run out of bounds.
		// directory pages are discontinuous, so coalesce the directory page bytes into a local
		// buffer and validate the sequential descriptor/page-list structure against directorySize.
		std::vector<unsigned char> directory(static_cast<size_t>(directorySize), 0);
		for (uint32_t i = 0u; i < directoryBlockCount; ++i)
		{
			const uint32_t pageIndex = header->directoryBlockIndices[i];
			const size_t sourceOffset = static_cast<size_t>(pageIndex) * blockSize;
			const size_t targetOffset = static_cast<size_t>(i) * blockSize;
			const size_t bytesRemaining = static_cast<size_t>(directorySize) - targetOffset;
			const size_t copied = (bytesRemaining < blockSize) ? bytesRemaining : blockSize;
			std::memcpy(directory.data() + targetOffset, static_cast<const unsigned char*>(data) + sourceOffset, copied);
		}

		const uint32_t streamCount = ValidateReadU16LE(directory.data() + 0u);
		const uint64_t descriptorBytes = 4ull + static_cast<uint64_t>(streamCount) * 8ull;
		if (descriptorBytes > directorySize)
		{
			return PDB::ErrorCode::InvalidDataSize;
		}

		uint64_t requiredDirectoryBytes = descriptorBytes;
		for (uint32_t i = 0u; i < streamCount; ++i)
		{
			const uint32_t byteSize = ValidateReadU32LE(directory.data() + 4u + static_cast<size_t>(i) * 8u);
			if (byteSize == PDB::NilPageSize || byteSize == 0u)
			{
				continue;
			}

			const uint32_t pageCount = PDB::ConvertSizeToBlockCount(byteSize, blockSize);
			requiredDirectoryBytes += static_cast<uint64_t>(pageCount) * sizeof(uint16_t);
			if (requiredDirectoryBytes > directorySize)
			{
				return PDB::ErrorCode::InvalidDataSize;
			}
		}

		// validate every stream page number as well
		uint64_t pageListOffset = descriptorBytes;
		for (uint32_t i = 0u; i < streamCount; ++i)
		{
			const uint32_t byteSize = ValidateReadU32LE(directory.data() + 4u + static_cast<size_t>(i) * 8u);
			if (byteSize == PDB::NilPageSize || byteSize == 0u)
			{
				continue;
			}

			const uint32_t pageCount = PDB::ConvertSizeToBlockCount(byteSize, blockSize);
			for (uint32_t j = 0u; j < pageCount; ++j)
			{
				const uint32_t pageIndex = ValidateReadU16LE(directory.data() + static_cast<size_t>(pageListOffset));
				pageListOffset += sizeof(uint16_t);
				if (pageIndex >= blockCount)
				{
					return PDB::ErrorCode::InvalidDataSize;
				}
			}
		}

		return PDB::ErrorCode::Success;
	}
}


// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
PDB_NO_DISCARD PDB::ErrorCode PDB::ValidateFile(const void* data, size_t size) PDB_NO_EXCEPT
{
	if (data == nullptr || size == 0u)
	{
		return ErrorCode::InvalidDataSize;
	}

	// Small MSF 2.00 has a distinct 44-byte magic. Only inspect it when enough bytes for the
	// MSF7 super block are missing but the small magic could still be present, or generally when
	// the first bytes match. The MSF7 path below retains its original rules.
	if (size >= PDB::SmallMSFHeader::MagicSize &&
		memcmp(data, PDB::SmallMSFHeader::MAGIC, PDB::SmallMSFHeader::MagicSize) == 0)
	{
		return ValidateSmallMSF2(data, size);
	}

	// validate whether there is enough size for the super block
	if (size < sizeof(SuperBlock))
	{
		return ErrorCode::InvalidDataSize;
	}
	// validate the super block
	const SuperBlock* superBlock = Pointer::Offset<const SuperBlock*>(data, 0u);
	{
		// validate header magic
		if (memcmp(superBlock->fileMagic, SuperBlock::MAGIC, sizeof(SuperBlock::MAGIC)) != 0)
		{
			return ErrorCode::InvalidSuperBlock;
		}

		// validate whether enough size is provided for the PDB file
		// blockCount * blockSize is the size of the PDB file on disk
		if (size < superBlock->blockCount * superBlock->blockSize)
		{
			return ErrorCode::InvalidDataSize;
		}

		// validate free block map.
		// the free block map should always reside at either index 1 or 2.
		if (superBlock->freeBlockMapIndex != 1u && superBlock->freeBlockMapIndex != 2u)
		{
			return ErrorCode::InvalidFreeBlockMap;
		}
	}

	return ErrorCode::Success;
}


// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
PDB_NO_DISCARD PDB::RawFile PDB::CreateRawFile(const void* data) PDB_NO_EXCEPT
{
	return RawFile(data);
}
