// Copyright 2011-2022, Molecular Matters GmbH <office@molecular-matters.com>
// See LICENSE.txt for licensing details (2-clause BSD License: https://opensource.org/licenses/BSD-2-Clause)

#pragma once

#include "Foundation/PDB_Macros.h"
#include "PDB_CoalescedMSFStream.h"

#include <cstdint>
#include <vector>


// https://llvm.org/docs/PDB/index.html
namespace PDB
{
	struct SuperBlock;


	// On-disk container generation.
	// MSF7 is the classic 7.00 format with 32-bit page numbers and a two-level directory block map.
	// SmallMSF2 is the old 2.00 format with unsigned 16-bit page numbers and a direct directory page list.
	enum class PDB_NO_DISCARD ContainerFormat : uint32_t
	{
		MSF7 = 0u,
		SmallMSF2 = 1u
	};

	// Unscoped aliases, allowing both PDB::ContainerFormat::MSF7 and PDB::MSF7 spellings.
	constexpr ContainerFormat MSF7 = ContainerFormat::MSF7;
	constexpr ContainerFormat SmallMSF2 = ContainerFormat::SmallMSF2;

	// Format-independent, read-only summary of the container header.
	struct PDB_NO_DISCARD ContainerInfo
	{
		ContainerFormat format = ContainerFormat::MSF7;
		uint32_t blockSize = 0u;
		uint32_t freeBlockMapIndex = 0u;
		uint32_t blockCount = 0u;
		uint32_t directorySize = 0u;
	};

	// One physically contiguous interval of bytes inside the original container file.
	// Intervals are always reported in stream logical order. A range that crosses a
	// discontinuous page boundary is split into several entries instead of being compressed.
	struct PDB_NO_DISCARD PhysicalFileRange
	{
		uint64_t fileOffset = 0u;
		uint32_t size = 0u;

		friend bool operator==(const PhysicalFileRange& lhs, const PhysicalFileRange& rhs) PDB_NO_EXCEPT
		{
			return lhs.fileOffset == rhs.fileOffset && lhs.size == rhs.size;
		}

		friend bool operator!=(const PhysicalFileRange& lhs, const PhysicalFileRange& rhs) PDB_NO_EXCEPT
		{
			return !(lhs == rhs);
		}
	};

	using PhysicalByteRange = PhysicalFileRange;


	class PDB_NO_DISCARD RawFile
	{
	public:
		RawFile(RawFile&& other) PDB_NO_EXCEPT;
		RawFile& operator=(RawFile&& other) PDB_NO_EXCEPT;

		explicit RawFile(const void* data) PDB_NO_EXCEPT;
		~RawFile(void) PDB_NO_EXCEPT;

		// Creates any type of MSF stream.
		template <typename T>
		PDB_NO_DISCARD T CreateMSFStream(uint32_t streamIndex) const PDB_NO_EXCEPT;

		// Creates any type of MSF stream with the given size.
		template <typename T>
		PDB_NO_DISCARD T CreateMSFStream(uint32_t streamIndex, uint32_t streamSize) const PDB_NO_EXCEPT;


		// Returns the SuperBlock for MSF7 containers.
		// Small MSF 2.00 containers do not have an MSF7 SuperBlock, so this returns nullptr for them;
		// use GetContainerInfo() for format-independent header fields.
		PDB_NO_DISCARD inline const SuperBlock* GetSuperBlock(void) const PDB_NO_EXCEPT
		{
			return m_superBlock;
		}

		// Returns the normalized, format-independent container header information.
		PDB_NO_DISCARD inline ContainerInfo GetContainerInfo(void) const PDB_NO_EXCEPT
		{
			return m_containerInfo;
		}

		// Returns the number of streams in the PDB file.
		PDB_NO_DISCARD inline uint32_t GetStreamCount(void) const PDB_NO_EXCEPT
		{
			return m_streamCount;
		}

		// Returns the size of the stream with the given index, taking into account nil page sizes.
		PDB_NO_DISCARD inline uint32_t GetStreamSize(uint32_t streamIndex) const PDB_NO_EXCEPT
		{
			const uint32_t streamSize = m_streamSizes[streamIndex];

			return (streamSize == NilPageSize) ? 0u : streamSize;
		}

		// Maps the logical range [offset, offset + size) of a stream to intervals inside the
		// original container file. The intervals are ordered like the stream's logical bytes and
		// never merge ranges across non-contiguous pages. Uses the normalized directory owned by
		// this RawFile; it does not parse another directory.
		PDB_NO_DISCARD std::vector<PhysicalFileRange> GetStreamPhysicalRanges(uint32_t streamIndex, uint32_t offset, uint32_t size) const PDB_NO_EXCEPT;

	private:
		// Parses the MSF7 two-level directory and assigns the normalized members.
		void InitializeMSF7(const void* data) PDB_NO_EXCEPT;

		// Parses the Small MSF 2.00 directory and normalizes its 16-bit metadata into uint32 arrays.
		void InitializeSmallMSF2(const void* data) PDB_NO_EXCEPT;

		const void* m_data;
		const SuperBlock* m_superBlock;
		ContainerInfo m_containerInfo;
		CoalescedMSFStream m_directoryStream;

		// stream directory
		uint32_t m_streamCount;
		const uint32_t* m_streamSizes;
		const uint32_t** m_streamBlocks;

		// Owned normalized storage for Small MSF 2.00 directories.
		// For MSF7, m_streamSizes and the block lists point into m_directoryStream instead.
		// Moving RawFile moves these vectors, preserving the addresses referenced by streams
		// created before the move.
		std::vector<uint32_t> m_normalizedStreamSizes;
		std::vector<uint32_t> m_normalizedBlockData;

		PDB_DISABLE_COPY(RawFile);
	};
}
