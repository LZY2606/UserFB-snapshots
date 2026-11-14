// Regression tests for RVA-addressed resource reading.
//
// The synthetic images are non-executable PE files. They contain no code and
// are used only to exercise the resource directory reader.

#include "task_support/fixture.h"
#include "task_support/check.h"

#include "pelib/ResourceDirectory.h"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{

struct SectionSpec
{
	std::uint32_t virtualAddress;
	std::uint32_t rawOffset;
	std::uint32_t rawSize;
	std::uint32_t virtualSize;
};

// Places resources by RVA. Header bytes and each section's raw bytes form the
// only mapped ranges, which is what the reader under test must use.
class Image
{
	public:
		Image(bool pe64, std::uint32_t sizeOfHeaders, std::uint32_t sizeOfImage,
				std::uint32_t fileSize)
			: m_pe64(pe64),
			  m_sizeOfHeaders(sizeOfHeaders),
			  m_sizeOfImage(sizeOfImage),
			  m_resourceRva(0),
			  m_resourceSize(0),
			  m_data(fileSize, 0)
		{
		}

		void addSection(std::uint32_t virtualAddress, std::uint32_t rawOffset,
				std::uint32_t rawSize, std::uint32_t virtualSize)
		{
			m_sections.push_back({virtualAddress, rawOffset, rawSize, virtualSize});
		}

		void setResource(std::uint32_t rva, std::uint32_t size)
		{
			m_resourceRva = rva;
			m_resourceSize = size;
		}

		std::uint32_t fileOffsetFor(std::uint32_t rva) const
		{
			if (rva < m_sizeOfHeaders && rva < m_data.size())
			{
				return rva;
			}

			for (const auto& section : m_sections)
			{
				if (section.rawOffset >= m_data.size())
				{
					continue;
				}

				const std::uint32_t availableRawSize = std::min<std::uint32_t>(
						section.rawSize,
						static_cast<std::uint32_t>(m_data.size() - section.rawOffset));
				if (section.virtualAddress <= rva
						&& rva - section.virtualAddress < availableRawSize)
				{
					return section.rawOffset + (rva - section.virtualAddress);
				}
			}

			throw std::runtime_error("RVA is not file-backed");
		}

		void place(std::uint32_t rva, const std::string& bytes)
		{
			for (std::size_t i = 0; i < bytes.size(); ++i)
			{
				m_data.at(fileOffsetFor(rva + static_cast<std::uint32_t>(i))) =
						static_cast<unsigned char>(bytes[i]);
			}
		}

		void u16(std::uint32_t rva, std::uint16_t value)
		{
			m_data.at(fileOffsetFor(rva)) = static_cast<unsigned char>(value & 0xFF);
			m_data.at(fileOffsetFor(rva + 1)) =
					static_cast<unsigned char>((value >> 8) & 0xFF);
		}

		void u32(std::uint32_t rva, std::uint32_t value)
		{
			for (unsigned int i = 0; i < 4; ++i)
			{
				m_data.at(fileOffsetFor(rva + i)) =
						static_cast<unsigned char>((value >> (8 * i)) & 0xFF);
			}
		}

		// Writes a UTF-16 resource name: a length word followed by code units.
		void name(std::uint32_t rva, const std::string& text)
		{
			u16(rva, static_cast<std::uint16_t>(text.size()));
			for (std::size_t i = 0; i < text.size(); ++i)
			{
				u16(rva + 2 + static_cast<std::uint32_t>(i * 2),
						static_cast<std::uint16_t>(text[i]));
			}
		}

		void directory(std::uint32_t rva, std::uint16_t namedEntries,
				std::uint16_t idEntries)
		{
			u16(rva + 12, namedEntries);
			u16(rva + 14, idEntries);
		}

		void entry(std::uint32_t rva, std::uint32_t nameValue, std::uint32_t dataValue)
		{
			u32(rva, nameValue);
			u32(rva + 4, dataValue);
		}

		void descriptor(std::uint32_t rva, std::uint32_t offsetToData,
				std::uint32_t size, std::uint32_t codePage, std::uint32_t reserved)
		{
			u32(rva, offsetToData);
			u32(rva + 4, size);
			u32(rva + 8, codePage);
			u32(rva + 12, reserved);
		}

		const std::vector<unsigned char>& build()
		{
			writeHeaders();
			return m_data;
		}

	private:
		void writeAt(std::size_t offset, std::uint64_t value, unsigned int bytes)
		{
			for (unsigned int i = 0; i < bytes; ++i)
			{
				m_data.at(offset + i) = static_cast<unsigned char>((value >> (8 * i)) & 0xFF);
			}
		}

		void writeHeaders()
		{
			const std::uint32_t ntOffset = 0x80;

			writeAt(0, 0x5A4D, 2);
			writeAt(0x3C, ntOffset, 4);

			writeAt(ntOffset, 0x00004550, 4);

			const std::uint32_t fileHeader = ntOffset + 4;
			writeAt(fileHeader, m_pe64 ? 0x8664 : 0x014C, 2);
			writeAt(fileHeader + 2, static_cast<std::uint32_t>(m_sections.size()), 2);
			writeAt(fileHeader + 16, m_pe64 ? 240 : 224, 2);
			writeAt(fileHeader + 18, 0x0102, 2);

			const std::uint32_t optional = ntOffset + 4 + 20;
			if (m_pe64)
			{
				writeAt(optional, 0x20B, 2);
				writeAt(optional + 24, 0x140000000ULL, 8);
				writeAt(optional + 32, 0x1000, 4);
				writeAt(optional + 36, 0x200, 4);
				writeAt(optional + 56, m_sizeOfImage, 4);
				writeAt(optional + 60, m_sizeOfHeaders, 4);
				writeAt(optional + 68, 3, 2);
				writeAt(optional + 108, 16, 4);
				writeAt(optional + 112 + 2 * 8, m_resourceRva, 4);
				writeAt(optional + 112 + 2 * 8 + 4, m_resourceSize, 4);
			}
			else
			{
				writeAt(optional, 0x10B, 2);
				writeAt(optional + 28, 0x400000, 4);
				writeAt(optional + 32, 0x1000, 4);
				writeAt(optional + 36, 0x200, 4);
				writeAt(optional + 56, m_sizeOfImage, 4);
				writeAt(optional + 60, m_sizeOfHeaders, 4);
				writeAt(optional + 68, 3, 2);
				writeAt(optional + 92, 16, 4);
				writeAt(optional + 96 + 2 * 8, m_resourceRva, 4);
				writeAt(optional + 96 + 2 * 8 + 4, m_resourceSize, 4);
			}

			const std::uint32_t sectionBase =
					optional + (m_pe64 ? 240 : 224);
			for (std::size_t i = 0; i < m_sections.size(); ++i)
			{
				const std::uint32_t header = sectionBase + static_cast<std::uint32_t>(i * 40);
				const std::string name = "s" + std::to_string(i);
				for (std::size_t j = 0; j < name.size() && j < 8; ++j)
				{
					m_data.at(header + j) = static_cast<unsigned char>(name[j]);
				}
				writeAt(header + 8, m_sections[i].virtualSize, 4);
				writeAt(header + 12, m_sections[i].virtualAddress, 4);
				writeAt(header + 16, m_sections[i].rawSize, 4);
				writeAt(header + 20, m_sections[i].rawOffset, 4);
				writeAt(header + 36, 0x40000040, 4);
			}
		}

		bool m_pe64;
		std::uint32_t m_sizeOfHeaders;
		std::uint32_t m_sizeOfImage;
		std::uint32_t m_resourceRva;
		std::uint32_t m_resourceSize;
		std::vector<unsigned char> m_data;
		std::vector<SectionSpec> m_sections;
};

template <int bits>
PeLib::PeHeaderT<bits> readHeader(std::istream& in)
{
	PeLib::MzHeader mz;
	if (mz.read(in) != PeLib::ERROR_NONE)
	{
		throw std::runtime_error("DOS header read failed");
	}

	PeLib::PeHeaderT<bits> header;
	if (header.read(in, 0x80, mz) != PeLib::ERROR_NONE)
	{
		throw std::runtime_error("PE header read failed");
	}

	return header;
}

template <int bits>
int readResource(const std::vector<unsigned char>& image,
		PeLib::ResourceDirectoryT<bits>& directory)
{
	std::istringstream in(image_string(image));
	auto header = readHeader<bits>(in);
	return directory.read(in, header);
}

std::string asString(const std::vector<PeLib::byte>& data)
{
	return std::string(data.begin(), data.end());
}

bool occupies(const PeLib::ResourceDirectory& directory, unsigned int start,
		unsigned int end)
{
	const auto& ranges = directory.getOccupiedAddresses();
	return std::find(ranges.begin(), ranges.end(), std::make_pair(start, end))
			!= ranges.end();
}

// Adds the deliberately unsorted section layout used by most images. The
// resource structures may cross the 0x3000 and 0x4000 virtual boundaries while
// the matching file offsets are unrelated.
void addMappingSections(Image& image)
{
	// Added out of virtual-address order on purpose. Raw ranges do not overlap
	// in the file even though their RVAs are adjacent.
	image.addSection(0x2000, 0x0800, 0x1000, 0x1000); // A: 0x2000-0x2FFF, raw 0x0800
	image.addSection(0x1400, 0x1800, 0x0400, 0x0400); // B: 0x1400-0x17FF, raw 0x1800
	image.addSection(0x4000, 0x3400, 0x0400, 0x0400); // G: 0x4000-0x43FF, raw 0x3400
	image.addSection(0x1000, 0x0400, 0x0400, 0x0400); // C: 0x1000-0x13FF, raw 0x0400
	image.addSection(0x1C00, 0x3000, 0x0400, 0x0400); // D: 0x1C00-0x1FFF, raw 0x3000
	image.addSection(0x3000, 0x1C00, 0x1000, 0x1000); // F: 0x3000-0x3FFF, raw 0x1C00
	image.addSection(0x1800, 0x2C00, 0x0400, 0x0400); // E: 0x1800-0x1BFF, raw 0x2C00
}

// Root at 0x2000 contains type 2. Type 2 has a named direct leaf, an ID
// resource with a language directory that crosses two boundaries, and an ID
// direct leaf whose descriptor and payload are in SizeOfHeaders.
std::vector<unsigned char> mappingImage(bool pe64)
{
	Image image(pe64, 0x0400, 0x4400, 0x3800);
	addMappingSections(image);
	// The resource root sits at 0x13F8 so every directory-relative offset is
	// positive while still crossing the 0x1400 C/B boundary.
	image.setResource(0x13F8, 0x3000);
	image.build();

	// Root directory header crosses 0x1400 (C -> B); its one entry is in B.
	image.directory(0x13F8, 0, 1);
	image.entry(0x1408, 2, 0x80000000u | 0x03F0);

	// Type directory header is in B; its entry array crosses 0x1800 (B -> E).
	image.directory(0x17E8, 1, 2);
	image.entry(0x17F8, 0x80000000u | 0x07E8, 0x0C00);
	image.entry(0x1800, 1, 0x80000000u | 0x0C10);
	image.entry(0x1808, 2, 0x00000D08);

	// UTF-16 name crosses 0x1C00 (E -> D).
	image.name(0x1BE0, "ABCDEFGHIJKLMNOPQRSTUVWX");

	// Named resource: descriptor crosses 0x2000 (D -> A), payload in section C
	// which precedes the resource directory.
	image.descriptor(0x1FF8, 0x1080, 8, 1252, 2);
	image.place(0x1080, "NAMED!!!");

	// Language directory header and entries are in A.
	image.directory(0x2008, 0, 2);
	image.entry(0x2018, 1033, 0x1C08);
	image.entry(0x2020, 1031, 0x1C20);

	// Language 1033: descriptor and payload fully in F / A.
	image.descriptor(0x3000, 0x2200, 8, 1200, 3);
	image.place(0x2200, "SECOND!!");

	// Language 1031: descriptor in F, payload crosses 0x4000 (F -> G).
	image.descriptor(0x3018, 0x3FFE, 8, 1200, 4);
	image.place(0x3FFE, "CROSSED!");

	// Resource 2 is a direct leaf in E whose payload is in SizeOfHeaders.
	image.descriptor(0x2100, 0x0300, 8, 65001, 7);
	image.place(0x0300, "HEADER!!");

	return image.build();
}

// A descriptor and its payload that only exist in the zero-filled tail of a
// section, plus a resource directory in the same section.
std::vector<unsigned char> zeroFillImage()
{
	Image image(false, 0x400, 0x3000, 0x1000);
	image.addSection(0x1000, 0x400, 0x400, 0x2000);
	image.setResource(0x1000, 0x200);
	image.build();

	image.directory(0x1000, 0, 1);
	image.entry(0x1010, 1, 0x80000000u | 0x20);
	image.directory(0x1020, 0, 1);
	image.entry(0x1030, 1033, 0x40);
	image.descriptor(0x1040, 0x1600, 4, 0, 0);

	return image.build();
}

// Two sections claim the same RVA range.
std::vector<unsigned char> ambiguousImage()
{
	Image image(false, 0x400, 0x3000, 0x1000);
	// The two sections overlap virtually at 0x1200-0x13FF while their raw
	// bytes are disjoint, and the section table is deliberately unsorted.
	image.addSection(0x1200, 0x600, 0x400, 0x400);
	image.addSection(0x1000, 0x200, 0x400, 0x400);
	// The root itself is inside the overlap, so the first read is ambiguous.
	image.setResource(0x1100, 0x0100);
	image.build();

	image.directory(0x1100, 0, 1);
	image.entry(0x1110, 1, 0x80000000u | 0x0120);
	image.directory(0x1120, 0, 1);
	image.entry(0x1130, 1033, 0x0140);
	image.descriptor(0x1140, 0x1200, 4, 0, 0);

	return image.build();
}

// Two sibling entries of type 2 refer to the same language directory. Its
// header is in A, its leaf descriptor crosses 0x2000, and its payload precedes
// the resource directory, so mapping and sharing occur together.
std::vector<unsigned char> aliasImage()
{
	Image image(false, 0x0400, 0x4400, 0x3800);
	addMappingSections(image);
	image.setResource(0x13F8, 0x3000);
	image.build();

	image.directory(0x13F8, 0, 1);
	image.entry(0x1408, 2, 0x80000000u | 0x03F0);

	// Two sibling entries point at the same language directory.
	image.directory(0x17E8, 0, 2);
	image.entry(0x17F8, 7, 0x80000000u | 0x0C10);
	image.entry(0x1800, 8, 0x80000000u | 0x0C10);

	// The shared directory header is in A; its descriptor crosses 0x2000 and
	// its payload precedes the resource directory.
	image.directory(0x2008, 0, 1);
	image.entry(0x2018, 1033, 0x0C00);
	image.descriptor(0x1FF8, 0x1080, 8, 1200, 1);
	image.place(0x1080, "SHARED!!");

	return image.build();
}

// The last entry of the innermost directory points back to that directory,
// i.e. to one of its own ancestors.
std::vector<unsigned char> cycleImage()
{
	Image image(false, 0x400, 0x3000, 0x1000);
	image.addSection(0x2000, 0x400, 0x800, 0x800);
	image.setResource(0x2000, 0x800);
	image.build();

	image.directory(0x2000, 0, 1);
	image.entry(0x2010, 2, 0x80000000u | 0x30);
	image.directory(0x2030, 0, 1);
	image.entry(0x2040, 5, 0x80000000u | 0x60);
	image.directory(0x2060, 0, 1);
	image.entry(0x2070, 1033, 0x80000000u | 0x60);

	return image.build();
}

// The innermost directory points at its grandparent (the type directory),
// which is a true ancestor cycle rather than a self reference.
std::vector<unsigned char> ancestorCycleImage()
{
	Image image(false, 0x0400, 0x3000, 0x1000);
	image.addSection(0x2000, 0x0400, 0x0800, 0x0800);
	image.setResource(0x2000, 0x0800);
	image.build();

	image.directory(0x2000, 0, 1);
	image.entry(0x2010, 2, 0x80000000u | 0x0030);
	image.directory(0x2030, 0, 1);
	image.entry(0x2040, 5, 0x80000000u | 0x0060);
	image.directory(0x2060, 0, 1);
	image.entry(0x2070, 1033, 0x80000000u | 0x0030);

	return image.build();
}

// The entry array is advertised beyond SizeOfImage; the loader check must set
// a loader error and produce an empty tree.
std::vector<unsigned char> overEndImage()
{
	Image image(false, 0x400, 0x5000, 0x1000);
	image.addSection(0x2000, 0x400, 0x800, 0x800);
	image.setResource(0x2000, 0x800);
	image.build();

	image.directory(0x2000, 0, 0x4000);

	return image.build();
}

// A small normal image used as the second image in the reread tests.
std::vector<unsigned char> secondImage()
{
	Image image(false, 0x400, 0x3000, 0x1000);
	image.addSection(0x2000, 0x400, 0x800, 0x800);
	image.setResource(0x2000, 0x800);
	image.build();

	image.directory(0x2000, 0, 1);
	image.entry(0x2010, 20, 0x80000000u | 0x30);
	image.directory(0x2030, 0, 1);
	image.entry(0x2040, 3, 0x50);
	image.descriptor(0x2050, 0x2400, 6, 1200, 0);
	image.place(0x2400, "SECOND");

	return image.build();
}

// A first image with a larger tree; the reread test checks it is replaced.
std::vector<unsigned char> firstImage()
{
	Image image(false, 0x400, 0x3000, 0x1000);
	image.addSection(0x1000, 0x200, 0x200, 0x200);
	image.addSection(0x2000, 0x400, 0x800, 0x800);
	image.setResource(0x2000, 0x800);
	image.build();

	image.directory(0x2000, 0, 1);
	image.entry(0x2010, 10, 0x80000000u | 0x30);
	image.directory(0x2030, 0, 2);
	image.entry(0x2040, 1, 0x50);
	image.entry(0x2048, 2, 0x70);
	image.descriptor(0x2050, 0x1040, 5, 1200, 0);
	image.descriptor(0x2070, 0x1048, 5, 1200, 0);
	image.place(0x1040, "FIRST");
	image.place(0x1048, "OTHER");

	return image.build();
}

int caseMappingCrossSections()
{
	const auto image = mappingImage(false);
	PeLib::ResourceDirectoryT<32> directory;
	TASK_CHECK(readResource(image, directory) == PeLib::ERROR_NONE);

	// Root header (0x13F8) crosses 0x1400.
	auto root = directory.getRoot();
	TASK_CHECK(root->getElementRva() == 0x13F8);
	TASK_CHECK(root->getNumberOfChildren() == 1);
	TASK_CHECK(root->getChild(0)->getOffsetToData() == (0x80000000u | 0x03F0));

	// Type header is in B and its entry array crosses 0x1800.
	auto typeNode = dynamic_cast<PeLib::ResourceNode*>(root->getChild(0)->getNode());
	TASK_CHECK(typeNode != nullptr);
	TASK_CHECK(typeNode->getElementRva() == 0x17E8);
	TASK_CHECK(typeNode->getNumberOfChildren() == 3);
	TASK_CHECK(typeNode->getChild(0)->getOffsetToData() == 0x0C00);
	TASK_CHECK(typeNode->getChild(1)->getOffsetToData() == (0x80000000u | 0x0C10));
	TASK_CHECK(typeNode->getChild(2)->getOffsetToData() == 0x00000D08);

	// The named resource is a direct leaf crossing 0x2000.
	auto namedLeaf = dynamic_cast<PeLib::ResourceLeaf*>(
			typeNode->getChild(0)->getNode());
	TASK_CHECK(namedLeaf != nullptr);
	TASK_CHECK(namedLeaf->getElementRva() == 0x1FF8);
	TASK_CHECK(namedLeaf->getOffsetToData() == 0x1080);
	TASK_CHECK(namedLeaf->getSize() == 8);
	TASK_CHECK(namedLeaf->getCodePage() == 1252);
	TASK_CHECK(namedLeaf->getReserved() == 2);
	TASK_CHECK(asString(namedLeaf->getData()) == "NAMED!!!");

	// Resource 1 goes through a language directory in A.
	auto languageNode = dynamic_cast<PeLib::ResourceNode*>(typeNode->getChild(1)->getNode());
	TASK_CHECK(languageNode != nullptr);
	TASK_CHECK(languageNode->getElementRva() == 0x2008);
	TASK_CHECK(languageNode->getNumberOfChildren() == 2);
	TASK_CHECK(languageNode->getChild(0)->getOffsetToData() == 0x1C08);
	TASK_CHECK(languageNode->getChild(1)->getOffsetToData() == 0x1C20);

	auto secondLeaf = dynamic_cast<PeLib::ResourceLeaf*>(
			languageNode->getChild(0)->getNode());
	TASK_CHECK(secondLeaf != nullptr);
	TASK_CHECK(secondLeaf->getElementRva() == 0x3000);
	TASK_CHECK(secondLeaf->getOffsetToData() == 0x2200);
	TASK_CHECK(asString(secondLeaf->getData()) == "SECOND!!");

	// Payload crosses 0x4000 while the descriptor stays in F.
	auto crossedLeaf = dynamic_cast<PeLib::ResourceLeaf*>(
			languageNode->getChild(1)->getNode());
	TASK_CHECK(crossedLeaf != nullptr);
	TASK_CHECK(crossedLeaf->getElementRva() == 0x3018);
	TASK_CHECK(crossedLeaf->getOffsetToData() == 0x3FFE);
	TASK_CHECK(crossedLeaf->getReserved() == 4);
	TASK_CHECK(asString(crossedLeaf->getData()) == "CROSSED!");

	// Resource 2 is a direct leaf whose payload is in the header region.
	auto headerLeaf = dynamic_cast<PeLib::ResourceLeaf*>(
			typeNode->getChild(2)->getNode());
	TASK_CHECK(headerLeaf != nullptr);
	TASK_CHECK(headerLeaf->getElementRva() == 0x2100);
	TASK_CHECK(headerLeaf->getOffsetToData() == 0x0300);
	TASK_CHECK(headerLeaf->getCodePage() == 65001);
	TASK_CHECK(headerLeaf->getReserved() == 7);
	TASK_CHECK(asString(headerLeaf->getData()) == "HEADER!!");

	// The UTF-16 name crosses 0x1C00.
	TASK_CHECK(typeNode->getChild(0)->getName()
			== "ABCDEFGHIJKLMNOPQRSTUVWX");

	std::vector<PeLib::byte> data;
	directory.getResourceData(2, 1, data);
	TASK_CHECK(asString(data) == "SECOND!!");
	directory.getResourceData(2, 2, data);
	TASK_CHECK(asString(data) == "HEADER!!");

	// Occupied ranges are RVAs and include every cross-boundary element.
	TASK_CHECK(occupies(directory, 0x13F8, 0x1407));   // root header
	TASK_CHECK(occupies(directory, 0x1408, 0x140F));   // root entry
	TASK_CHECK(occupies(directory, 0x17E8, 0x17F7));   // type header
	TASK_CHECK(occupies(directory, 0x17F8, 0x180F));   // type entries (B -> E)
	TASK_CHECK(occupies(directory, 0x1BE0, 0x1C11));   // name (E -> D)
	TASK_CHECK(occupies(directory, 0x1FF8, 0x2007));   // named descriptor
	TASK_CHECK(occupies(directory, 0x1080, 0x1087));   // named payload
	TASK_CHECK(occupies(directory, 0x3000, 0x300F));   // language 1033 descriptor
	TASK_CHECK(occupies(directory, 0x2200, 0x2207));   // language 1033 payload
	TASK_CHECK(occupies(directory, 0x3018, 0x3027));   // language 1031 descriptor
	TASK_CHECK(occupies(directory, 0x3FFE, 0x4005));   // payload crossing F -> G
	TASK_CHECK(occupies(directory, 0x0300, 0x0307));   // header payload

	// No occupied range is reported in file-offset space. The named
	// descriptor (RVA 0x1FF8) lives at raw 0x33F8 and raw 0x0800, and the
	// crossed payload (RVA 0x3FFE) at raw 0x2BFE and raw 0x3400.
	TASK_CHECK(!occupies(directory, 0x33F8, 0x3407));
	TASK_CHECK(!occupies(directory, 0x0800, 0x0807));
	TASK_CHECK(!occupies(directory, 0x2BFE, 0x2BFF));
	TASK_CHECK(!occupies(directory, 0x3400, 0x3405));

	for (const auto& range : directory.getOccupiedAddresses())
	{
		TASK_CHECK(range.second >= range.first);
		TASK_CHECK(range.first < 0x4400 && range.second < 0x4400);
	}

	return 0;
}

int caseMappingPe64()
{
	const auto image = mappingImage(true);
	PeLib::ResourceDirectoryT<64> directory;
	TASK_CHECK(readResource(image, directory) == PeLib::ERROR_NONE);

	auto root = directory.getRoot();
	TASK_CHECK(root->getNumberOfChildren() == 1);
	auto typeNode = dynamic_cast<PeLib::ResourceNode*>(root->getChild(0)->getNode());
	TASK_CHECK(typeNode != nullptr);
	TASK_CHECK(typeNode->getNumberOfChildren() == 3);

	auto languageNode = dynamic_cast<PeLib::ResourceNode*>(typeNode->getChild(1)->getNode());
	TASK_CHECK(languageNode != nullptr);
	TASK_CHECK(languageNode->getNumberOfChildren() == 2);

	auto leaf = dynamic_cast<PeLib::ResourceLeaf*>(languageNode->getChild(1)->getNode());
	TASK_CHECK(leaf != nullptr);
	TASK_CHECK(leaf->getElementRva() == 0x3018);
	TASK_CHECK(leaf->getOffsetToData() == 0x3FFE);
	TASK_CHECK(asString(leaf->getData()) == "CROSSED!");

	std::vector<PeLib::byte> data;
	directory.getResourceData(2, 2, data);
	TASK_CHECK(asString(data) == "HEADER!!");
	TASK_CHECK(occupies(directory, 0x3FFE, 0x4005));

	return 0;
}

int caseMappingRejectsZeroFill()
{
	const auto image = zeroFillImage();
	PeLib::ResourceDirectoryT<32> directory;
	TASK_CHECK(readResource(image, directory) == PeLib::ERROR_INVALID_FILE);
	return 0;
}

int caseMappingRejectsAmbiguousMapping()
{
	const auto image = ambiguousImage();
	PeLib::ResourceDirectoryT<32> directory;
	TASK_CHECK(readResource(image, directory) == PeLib::ERROR_INVALID_FILE);
	return 0;
}

int caseAliasesSharedDirectory()
{
	const auto image = aliasImage();
	PeLib::ResourceDirectoryT<32> directory;
	TASK_CHECK(readResource(image, directory) == PeLib::ERROR_NONE);

	// Mapping and sharing occur together: the shared directory's header is in
	// A, the descriptor crosses 0x2000, and its payload is before the root.
	auto root = directory.getRoot();
	TASK_CHECK(root->getNumberOfChildren() == 1);
	auto typeNode = dynamic_cast<PeLib::ResourceNode*>(root->getChild(0)->getNode());
	TASK_CHECK(typeNode != nullptr);
	TASK_CHECK(typeNode->getNumberOfChildren() == 2);

	// Both sibling entries get their own copy of the shared directory.
	auto firstPath = dynamic_cast<PeLib::ResourceNode*>(typeNode->getChild(0)->getNode());
	auto secondPath = dynamic_cast<PeLib::ResourceNode*>(typeNode->getChild(1)->getNode());
	TASK_CHECK(firstPath != nullptr);
	TASK_CHECK(secondPath != nullptr);
	TASK_CHECK(firstPath != secondPath);
	TASK_CHECK(firstPath->getElementRva() == 0x2008);
	TASK_CHECK(secondPath->getElementRva() == 0x2008);
	TASK_CHECK(firstPath->getNumberOfChildren() == 1);
	TASK_CHECK(secondPath->getNumberOfChildren() == 1);

	auto firstLeaf = dynamic_cast<PeLib::ResourceLeaf*>(firstPath->getChild(0)->getNode());
	auto secondLeaf = dynamic_cast<PeLib::ResourceLeaf*>(secondPath->getChild(0)->getNode());
	TASK_CHECK(firstLeaf != nullptr);
	TASK_CHECK(secondLeaf != nullptr);
	TASK_CHECK(firstLeaf != secondLeaf);
	TASK_CHECK(firstLeaf->getElementRva() == 0x1FF8);
	TASK_CHECK(secondLeaf->getElementRva() == 0x1FF8);
	TASK_CHECK(firstLeaf->getOffsetToData() == 0x1080);
	TASK_CHECK(asString(firstLeaf->getData()) == "SHARED!!");
	TASK_CHECK(asString(secondLeaf->getData()) == "SHARED!!");

	std::vector<PeLib::byte> data;
	directory.getResourceData(2, 7, data);
	TASK_CHECK(asString(data) == "SHARED!!");
	directory.getResourceData(2, 8, data);
	TASK_CHECK(asString(data) == "SHARED!!");

	// Every visit occupies the same RVAs; sharing does not merge the nodes.
	TASK_CHECK(occupies(directory, 0x2008, 0x2017));
	TASK_CHECK(occupies(directory, 0x1FF8, 0x2007));
	TASK_CHECK(occupies(directory, 0x1080, 0x1087));

	std::size_t descriptorVisits = 0;
	for (const auto& range : directory.getOccupiedAddresses())
	{
		if (range == std::make_pair(0x1FF8u, 0x2007u))
		{
			++descriptorVisits;
		}
	}
	TASK_CHECK(descriptorVisits == 2);

	// A deep copy owns its own nodes; destroying it leaves the other path intact.
	{
		PeLib::ResourceChild copy = *typeNode->getChild(0);
		auto copyPath = dynamic_cast<PeLib::ResourceNode*>(copy.getNode());
		TASK_CHECK(copyPath != nullptr);
		TASK_CHECK(copyPath != firstPath);
		auto copyLeaf = dynamic_cast<PeLib::ResourceLeaf*>(copyPath->getChild(0)->getNode());
		TASK_CHECK(copyLeaf != nullptr);
		TASK_CHECK(asString(copyLeaf->getData()) == "SHARED!!");
	}

	TASK_CHECK(firstPath->getNumberOfChildren() == 1);
	TASK_CHECK(asString(firstLeaf->getData()) == "SHARED!!");
	TASK_CHECK(secondPath->getNumberOfChildren() == 1);
	TASK_CHECK(asString(secondLeaf->getData()) == "SHARED!!");

	return 0;
}

int caseAliasesCycleRejected()
{
	const auto image = cycleImage();
	PeLib::ResourceDirectoryT<32> directory;
	TASK_CHECK(readResource(image, directory) == PeLib::ERROR_INVALID_FILE);
	return 0;
}

int caseAliasesAncestorCycleRejected()
{
	const auto image = ancestorCycleImage();
	PeLib::ResourceDirectoryT<32> directory;
	TASK_CHECK(readResource(image, directory) == PeLib::ERROR_INVALID_FILE);
	return 0;
}

int caseRereadReplacesTree()
{
	PeLib::ResourceDirectoryT<32> directory;
	TASK_CHECK(readResource(firstImage(), directory) == PeLib::ERROR_NONE);

	auto firstRoot = directory.getRoot();
	TASK_CHECK(firstRoot->getNumberOfChildren() == 1);
	TASK_CHECK(firstRoot->getChild(0)->getOffsetToName() == 10);
	TASK_CHECK(directory.getNumberOfResources(10) == 2);
	TASK_CHECK(occupies(directory, 0x1040, 0x1044));

	// Read a second, different image into the same object.
	TASK_CHECK(readResource(secondImage(), directory) == PeLib::ERROR_NONE);

	PeLib::ResourceDirectoryT<32> fresh;
	TASK_CHECK(readResource(secondImage(), fresh) == PeLib::ERROR_NONE);

	auto root = directory.getRoot();
	TASK_CHECK(root->getNumberOfChildren() == 1);
	TASK_CHECK(root->getChild(0)->getOffsetToName() == 20);
	TASK_CHECK(directory.getNumberOfResources(20) == 1);
	TASK_CHECK(directory.getNumberOfResources(10) == 0xFFFFFFFF);
	TASK_CHECK(directory.loaderError() == PeLib::LDR_ERROR_NONE);
	TASK_CHECK(directory.getOccupiedAddresses() == fresh.getOccupiedAddresses());
	TASK_CHECK(!occupies(directory, 0x1040, 0x1044));
	TASK_CHECK(occupies(directory, 0x2400, 0x2405));

	std::vector<PeLib::byte> data;
	directory.getResourceData(20, 3, data);
	TASK_CHECK(asString(data) == "SECOND");

	return 0;
}

int caseRereadClearsLoaderError()
{
	PeLib::ResourceDirectoryT<32> directory;
	TASK_CHECK(readResource(overEndImage(), directory) == PeLib::ERROR_NONE);
	TASK_CHECK(directory.loaderError() == PeLib::LDR_ERROR_RSRC_OVER_END_OF_IMAGE);
	TASK_CHECK(directory.getRoot()->getNumberOfChildren() == 0);
	TASK_CHECK(occupies(directory, 0x2000, 0x200F));

	TASK_CHECK(readResource(mappingImage(false), directory) == PeLib::ERROR_NONE);

	PeLib::ResourceDirectoryT<32> fresh;
	TASK_CHECK(readResource(mappingImage(false), fresh) == PeLib::ERROR_NONE);

	TASK_CHECK(directory.loaderError() == PeLib::LDR_ERROR_NONE);
	TASK_CHECK(directory.getRoot()->getNumberOfChildren() == 1);
	TASK_CHECK(directory.getOccupiedAddresses() == fresh.getOccupiedAddresses());
	TASK_CHECK(occupies(directory, 0x3FFE, 0x4005));

	std::vector<PeLib::byte> data;
	directory.getResourceData(2, 1, data);
	TASK_CHECK(asString(data) == "SECOND!!");

	return 0;
}

} // namespace

extern const task_case task_cases[];
extern const size_t task_case_count;

const task_case task_cases[] = {
	{"Mapping", "cross_sections", caseMappingCrossSections},
	{"Mapping", "pe64", caseMappingPe64},
	{"Mapping", "zero_fill_rejected", caseMappingRejectsZeroFill},
	{"Mapping", "ambiguous_mapping_rejected", caseMappingRejectsAmbiguousMapping},
	{"Aliases", "shared_directory", caseAliasesSharedDirectory},
	{"Aliases", "cycle_rejected", caseAliasesCycleRejected},
	{"Aliases", "ancestor_cycle_rejected", caseAliasesAncestorCycleRejected},
	{"Reread", "replaces_tree", caseRereadReplacesTree},
	{"Reread", "clears_loader_error", caseRereadClearsLoaderError},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
