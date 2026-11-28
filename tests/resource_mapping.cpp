// Regression tests for RVA-based resource directory reading: per-segment
// mapping, shared subdirectory references and repeated reads.
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "task_support/check.h"
#include "task_support/fixture.h"
#include "pelib/ResourceDirectory.h"

namespace
{
	struct SecDef
	{
		const char* name;
		unsigned va;
		unsigned raw;
		unsigned rawSize;
		unsigned vsize;
	};

	void writeDosHeader(std::vector<unsigned char>& b)
	{
		put(b, 0, 0x5a4d, 2); put(b, 2, 0x80, 2); put(b, 4, 1, 2); put(b, 8, 4, 2);
		put(b, 0x18, 0x40, 2); put(b, 0x3c, 0x80, 4);
	}

	void writeSections(std::vector<unsigned char>& b, size_t s, const std::vector<SecDef>& secs)
	{
		for (const auto& sec : secs)
		{
			for (size_t j = 0; sec.name[j]; ++j) b[s + j] = sec.name[j];
			put(b, s + 8, sec.vsize, 4);
			put(b, s + 12, sec.va, 4);
			put(b, s + 16, sec.rawSize, 4);
			put(b, s + 20, sec.raw, 4);
			put(b, s + 36, 0x40000040, 4);
			s += 40;
		}
	}

	std::vector<unsigned char> makePe32(const std::vector<SecDef>& secs, unsigned rsrcRva,
			size_t fileSize, unsigned sizeOfImage = 0x4000, unsigned sizeOfHeaders = 0x200)
	{
		std::vector<unsigned char> b(fileSize);
		writeDosHeader(b);
		put(b, 0x80, 0x4550, 4); put(b, 0x84, 0x14c, 2); put(b, 0x86, secs.size(), 2);
		put(b, 0x94, 224, 2); put(b, 0x96, 0x102, 2);
		size_t o = 0x98;
		put(b, o, 0x10b, 2); put(b, o + 28, 0x400000, 4); put(b, o + 32, 0x1000, 4); put(b, o + 36, 0x200, 4);
		put(b, o + 56, sizeOfImage, 4); put(b, o + 60, sizeOfHeaders, 4); put(b, o + 68, 3, 2);
		put(b, o + 72, 0x100000, 4); put(b, o + 76, 0x1000, 4); put(b, o + 80, 0x100000, 4); put(b, o + 84, 0x1000, 4);
		put(b, o + 92, 16, 4); put(b, o + 112, rsrcRva, 4); put(b, o + 116, 0x1000, 4);
		writeSections(b, o + 224, secs);
		return b;
	}

	std::vector<unsigned char> makePe64(const std::vector<SecDef>& secs, unsigned rsrcRva,
			size_t fileSize, unsigned sizeOfImage = 0x4000, unsigned sizeOfHeaders = 0x200)
	{
		std::vector<unsigned char> b(fileSize);
		writeDosHeader(b);
		put(b, 0x80, 0x4550, 4); put(b, 0x84, 0x8664, 2); put(b, 0x86, secs.size(), 2);
		put(b, 0x94, 240, 2); put(b, 0x96, 0x22, 2);
		size_t o = 0x98;
		put(b, o, 0x20b, 2); put(b, o + 24, 0x140000000ull, 8); put(b, o + 32, 0x1000, 4); put(b, o + 36, 0x200, 4);
		put(b, o + 56, sizeOfImage, 4); put(b, o + 60, sizeOfHeaders, 4); put(b, o + 68, 3, 2);
		put(b, o + 72, 0x100000, 8); put(b, o + 80, 0x1000, 8); put(b, o + 88, 0x100000, 8); put(b, o + 96, 0x1000, 8);
		put(b, o + 108, 16, 4); put(b, o + 128, rsrcRva, 4); put(b, o + 132, 0x1000, 4);
		writeSections(b, o + 240, secs);
		return b;
	}

	void writeString(std::vector<unsigned char>& b, size_t p, const char* s)
	{
		for (size_t i = 0; s[i]; ++i) b.at(p + i) = static_cast<unsigned char>(s[i]);
	}

	// Root (1-2 entries) -> type node -> name node -> leaf, same shape as the fixture.
	void writeNodeHeader(std::vector<unsigned char>& b, size_t p, unsigned idEntries)
	{
		put(b, p + 14, idEntries, 2);
	}

	void writeEntry(std::vector<unsigned char>& b, size_t p, unsigned name, unsigned offsetToData)
	{
		put(b, p, name, 4);
		put(b, p + 4, offsetToData, 4);
	}

	void writeDataEntry(std::vector<unsigned char>& b, size_t p, unsigned offsetToData, unsigned size, unsigned codePage = 0)
	{
		put(b, p, offsetToData, 4);
		put(b, p + 4, size, 4);
		put(b, p + 8, codePage, 4);
		put(b, p + 12, 0, 4);
	}

	// Directory structures straddle two VA-adjacent but file-discontiguous
	// sections; the section table is deliberately not sorted by VA; the
	// payload lives in a third section.
	std::vector<unsigned char> imageCrossSection()
	{
		auto b = makePe32({
				{".b", 0x201C, 0x800, 0x100, 0x100},
				{".c", 0x2200, 0xA00, 0x40, 0x40},
				{".a", 0x2000, 0x400, 0x1C, 0x100},
			}, 0x2000, 0xB00);
		writeNodeHeader(b, 0x400, 1);                       // root @ RVA 0x2000
		writeEntry(b, 0x410, 16, 0x80000018);               // -> type node @ RVA 0x2018
		put(b, 0x80A, 1, 2);                                // type node header straddles .a (file 0x418) and .b (file 0x800)
		writeEntry(b, 0x80C, 1, 0x80000030);                // -> name node @ RVA 0x2030
		writeNodeHeader(b, 0x814, 1);
		writeEntry(b, 0x824, 1033, 0x48);                   // -> data entry @ RVA 0x2048
		writeDataEntry(b, 0x82C, 0x2200, 8, 1200);
		writeString(b, 0xA00, "CROSSSEC");                  // payload @ RVA 0x2200
		return b;
	}

	// Payloads live before the resource root (earlier section) and inside the
	// SizeOfHeaders region where real file bytes exist.
	std::vector<unsigned char> imageScatteredPayload()
	{
		auto b = makePe32({
				{".rsrc", 0x2000, 0x400, 0x100, 0x100},
				{".early", 0x1000, 0x600, 0x40, 0x40},
			}, 0x2000, 0x700);
		writeNodeHeader(b, 0x400, 2);                       // root @ RVA 0x2000
		writeEntry(b, 0x410, 10, 0x80000020);               // -> type A @ RVA 0x2020
		writeEntry(b, 0x418, 11, 0x80000070);               // -> type B @ RVA 0x2070
		writeNodeHeader(b, 0x420, 1);
		writeEntry(b, 0x430, 1, 0x80000040);                // -> name A @ RVA 0x2040
		writeNodeHeader(b, 0x440, 1);
		writeEntry(b, 0x450, 1033, 0x60);                   // -> data entry A @ RVA 0x2060
		writeDataEntry(b, 0x460, 0x1000, 8, 1252);          // payload in .early, before the root
		writeNodeHeader(b, 0x470, 1);
		writeEntry(b, 0x480, 2, 0x80000090);                // -> name B @ RVA 0x2090
		writeNodeHeader(b, 0x490, 1);
		writeEntry(b, 0x4A0, 1033, 0xB0);                   // -> data entry B @ RVA 0x20B0
		writeDataEntry(b, 0x4B0, 0x48, 8, 1252);            // payload inside SizeOfHeaders
		writeString(b, 0x600, "EARLY001");
		writeString(b, 0x48, "HDRPAY01");
		return b;
	}

	// The entry array starts at an RVA that no segment maps (VirtualSize of
	// .a would cover it, but zero fill is not content).
	std::vector<unsigned char> imageHole()
	{
		auto b = makePe32({
				{".a", 0x2000, 0x400, 0x10, 0x100},
				{".b", 0x2100, 0x800, 0x100, 0x100},
			}, 0x2000, 0x900);
		writeNodeHeader(b, 0x400, 1);
		writeEntry(b, 0x410, 16, 0x80000018);
		return b;
	}

	// Two sections map the same RVA to different file offsets.
	std::vector<unsigned char> imageAmbiguous()
	{
		auto b = makePe32({
				{".a", 0x2000, 0x400, 0x100, 0x100},
				{".dup", 0x2000, 0x800, 0x100, 0x100},
			}, 0x2000, 0x900);
		writeNodeHeader(b, 0x400, 1);
		writeEntry(b, 0x410, 16, 0x80000018);
		return b;
	}

	// PE32+ image with the payload in a different section than the directory.
	std::vector<unsigned char> imagePe64()
	{
		auto b = makePe64({
				{".rsrc", 0x2000, 0x400, 0x80, 0x80},
				{".data", 0x2080, 0x800, 0x40, 0x40},
			}, 0x2000, 0x900);
		writeNodeHeader(b, 0x400, 1);
		writeEntry(b, 0x410, 16, 0x80000018);
		writeNodeHeader(b, 0x418, 1);
		writeEntry(b, 0x428, 1, 0x80000030);
		writeNodeHeader(b, 0x430, 1);
		writeEntry(b, 0x440, 1033, 0x48);
		writeDataEntry(b, 0x448, 0x2080, 6);
		writeString(b, 0x800, "PE64OK");
		return b;
	}

	// Two sibling root entries reference the same subdirectory.
	std::vector<unsigned char> imageShared()
	{
		auto b = makePe32({
				{".rsrc", 0x2000, 0x400, 0x200, 0x200},
			}, 0x2000, 0x600);
		writeNodeHeader(b, 0x400, 2);
		writeEntry(b, 0x410, 16, 0x80000020);               // type 16 -> shared node @ RVA 0x2020
		writeEntry(b, 0x418, 2, 0x80000020);                // type 2  -> shared node @ RVA 0x2020
		writeNodeHeader(b, 0x420, 1);
		writeEntry(b, 0x430, 1, 0x80000038);                // -> name node @ RVA 0x2038
		writeNodeHeader(b, 0x438, 1);
		writeEntry(b, 0x448, 1033, 0x50);                   // -> data entry @ RVA 0x2050
		writeDataEntry(b, 0x450, 0x2100, 6);
		writeString(b, 0x500, "SHARED");
		return b;
	}

	// Shared subdirectory whose header straddles two sections, payload in a third.
	std::vector<unsigned char> imageSharedMapped()
	{
		auto b = makePe32({
				{".a", 0x2000, 0x400, 0x4C, 0x4C},
				{".b", 0x204C, 0x800, 0x40, 0x40},
				{".c", 0x3000, 0xA00, 0x20, 0x20},
			}, 0x2000, 0xB00);
		writeNodeHeader(b, 0x400, 2);
		writeEntry(b, 0x410, 5, 0x80000044);                // type 5 -> shared node @ RVA 0x2044
		writeEntry(b, 0x418, 7, 0x80000044);                // type 7 -> shared node @ RVA 0x2044
		put(b, 0x806, 1, 2);                                // shared header straddles .a (file 0x444) and .b (file 0x800)
		writeEntry(b, 0x808, 3, 0x8000005C);                // -> name node @ RVA 0x205C
		writeNodeHeader(b, 0x810, 1);
		writeEntry(b, 0x820, 1033, 0x74);                   // -> data entry @ RVA 0x2074
		writeDataEntry(b, 0x828, 0x3000, 8);
		writeString(b, 0xA00, "MAPALIAS");
		return b;
	}

	// The type node points back to an ancestor (target 0 = root) or to itself.
	std::vector<unsigned char> imageCycle(unsigned target)
	{
		auto b = makePe32({
				{".rsrc", 0x2000, 0x400, 0x100, 0x100},
			}, 0x2000, 0x500);
		writeNodeHeader(b, 0x400, 1);
		writeEntry(b, 0x410, 16, 0x80000018);               // -> type node @ RVA 0x2018
		writeNodeHeader(b, 0x418, 1);
		writeEntry(b, 0x428, 1, 0x80000000 | target);       // -> back to an ancestor
		return b;
	}

	// Two leaves with identical payload content behind different paths.
	std::vector<unsigned char> imageIdenticalLeaves()
	{
		auto b = makePe32({
				{".rsrc", 0x2000, 0x400, 0x300, 0x300},
			}, 0x2000, 0x700);
		writeNodeHeader(b, 0x400, 1);
		writeEntry(b, 0x410, 16, 0x80000018);
		writeNodeHeader(b, 0x418, 2);                       // type node with two names
		writeEntry(b, 0x428, 1, 0x80000038);                // -> name A @ RVA 0x2038
		writeEntry(b, 0x430, 2, 0x80000050);                // -> name B @ RVA 0x2050
		writeNodeHeader(b, 0x438, 1);
		writeEntry(b, 0x448, 1033, 0x70);                   // -> data entry A @ RVA 0x2070
		writeNodeHeader(b, 0x450, 1);
		writeEntry(b, 0x460, 1033, 0x80);                   // -> data entry B @ RVA 0x2080
		writeDataEntry(b, 0x470, 0x2100, 8);
		writeDataEntry(b, 0x480, 0x2200, 8);
		writeString(b, 0x500, "SAMESAME");
		writeString(b, 0x600, "SAMESAME");
		return b;
	}

	// Second image for repeated reads: two types, different data.
	std::vector<unsigned char> imageSecond()
	{
		auto b = makePe32({
				{".rsrc", 0x2000, 0x400, 0x300, 0x300},
			}, 0x2000, 0x700);
		writeNodeHeader(b, 0x400, 2);
		writeEntry(b, 0x410, 5, 0x80000020);                // -> type A @ RVA 0x2020
		writeEntry(b, 0x418, 6, 0x80000070);                // -> type B @ RVA 0x2070
		writeNodeHeader(b, 0x420, 1);
		writeEntry(b, 0x430, 9, 0x80000040);                // -> name A @ RVA 0x2040
		writeNodeHeader(b, 0x440, 1);
		writeEntry(b, 0x450, 1033, 0x60);                   // -> data entry A @ RVA 0x2060
		writeDataEntry(b, 0x460, 0x2100, 6);
		writeNodeHeader(b, 0x470, 1);
		writeEntry(b, 0x480, 8, 0x80000090);                // -> name B @ RVA 0x2090
		writeNodeHeader(b, 0x490, 1);
		writeEntry(b, 0x4A0, 1033, 0xB0);                   // -> data entry B @ RVA 0x20B0
		writeDataEntry(b, 0x4B0, 0x2200, 6);
		writeString(b, 0x500, "SECOND");
		writeString(b, 0x600, "THIRD!");
		return b;
	}

	// Root declares more entries than fit into SizeOfImage.
	std::vector<unsigned char> imageLoaderError()
	{
		auto b = makePe32({
				{".rsrc", 0x2000, 0x400, 0x100, 0x100},
			}, 0x2000, 0x500, 0x2001);
		writeNodeHeader(b, 0x400, 1);
		writeEntry(b, 0x410, 16, 0x80000018);
		return b;
	}

	PeLib::PeHeaderT<64> readHeader64(std::istream& in)
	{
		PeLib::MzHeader mz;
		if (mz.read(in) != PeLib::ERROR_NONE) throw std::runtime_error("DOS read");
		PeLib::PeHeaderT<64> h;
		if (h.read(in, 0x80, mz) != PeLib::ERROR_NONE) throw std::runtime_error("PE read");
		return h;
	}

	const PeLib::ResourceNode* asNode(const PeLib::ResourceChild* child)
	{
		return child ? dynamic_cast<const PeLib::ResourceNode*>(child->getNode()) : nullptr;
	}

	const PeLib::ResourceLeaf* asLeaf(const PeLib::ResourceChild* child)
	{
		return child ? dynamic_cast<const PeLib::ResourceLeaf*>(child->getNode()) : nullptr;
	}

	// Walks root -> type -> name -> leaf along the given child indices.
	const PeLib::ResourceLeaf* leafAt(const PeLib::ResourceNode* root, unsigned typeIndex, unsigned nameIndex)
	{
		auto typeNode = asNode(root->getChild(typeIndex));
		if (!typeNode) return nullptr;
		auto nameNode = asNode(typeNode->getChild(nameIndex));
		if (!nameNode) return nullptr;
		return asLeaf(nameNode->getChild(0));
	}

	std::string leafData(const PeLib::ResourceLeaf* leaf)
	{
		if (!leaf) return std::string();
		auto data = leaf->getData();
		return std::string(data.begin(), data.end());
	}

	bool hasRange(const PeLib::ResourceDirectory& dir, unsigned start, unsigned end)
	{
		for (const auto& range : dir.getOccupiedAddresses())
		{
			if (range.first == start && range.second == end) return true;
		}
		return false;
	}
}

// ---------------------------------------------------------------- Mapping

static int case_mapping_cross_section()
{
	auto b = imageCrossSection();
	std::istringstream in(image_string(b));
	auto h = read_header(in);
	PeLib::ResourceDirectoryT<32> d;
	TASK_CHECK(d.read(in, h) == PeLib::ERROR_NONE);
	auto root = d.getRoot();
	TASK_CHECK(root->getNumberOfChildren() == 1);
	auto leaf = leafAt(root, 0, 0);
	TASK_CHECK(leaf != nullptr);
	TASK_CHECK(leafData(leaf) == "CROSSSEC");
	TASK_CHECK(leaf->getElementRva() == 0x2048);
	TASK_CHECK(leaf->getOffsetToData() == 0x2200);
	TASK_CHECK(leaf->getSize() == 8);
	TASK_CHECK(leaf->getCodePage() == 1200);
	TASK_CHECK(leaf->getReserved() == 0);
	std::vector<PeLib::byte> data;
	d.getResourceData(16, 1, data);
	TASK_CHECK(std::string(data.begin(), data.end()) == "CROSSSEC");
	TASK_CHECK(d.getOccupiedAddresses().size() == 8);
	TASK_CHECK(hasRange(d, 0x2000, 0x200F));
	TASK_CHECK(hasRange(d, 0x2018, 0x2027));
	TASK_CHECK(hasRange(d, 0x2048, 0x2057));
	TASK_CHECK(hasRange(d, 0x2200, 0x2207));
	return 0;
}

static int case_mapping_scattered_payload()
{
	auto b = imageScatteredPayload();
	std::istringstream in(image_string(b));
	auto h = read_header(in);
	PeLib::ResourceDirectoryT<32> d;
	TASK_CHECK(d.read(in, h) == PeLib::ERROR_NONE);
	auto root = d.getRoot();
	TASK_CHECK(root->getNumberOfChildren() == 2);
	TASK_CHECK(leafData(leafAt(root, 0, 0)) == "EARLY001");
	TASK_CHECK(leafData(leafAt(root, 1, 0)) == "HDRPAY01");
	TASK_CHECK(leafAt(root, 0, 0)->getElementRva() == 0x2060);
	TASK_CHECK(leafAt(root, 1, 0)->getElementRva() == 0x20B0);
	std::vector<PeLib::byte> data;
	d.getResourceData(10, 1, data);
	TASK_CHECK(std::string(data.begin(), data.end()) == "EARLY001");
	d.getResourceData(11, 2, data);
	TASK_CHECK(std::string(data.begin(), data.end()) == "HDRPAY01");
	TASK_CHECK(hasRange(d, 0x1000, 0x1007));
	TASK_CHECK(hasRange(d, 0x48, 0x4F));
	return 0;
}

static int case_mapping_hole_invalid()
{
	auto b = imageHole();
	std::istringstream in(image_string(b));
	auto h = read_header(in);
	PeLib::ResourceDirectoryT<32> d;
	TASK_CHECK(d.read(in, h) == PeLib::ERROR_INVALID_FILE);
	return 0;
}

static int case_mapping_ambiguous_invalid()
{
	auto b = imageAmbiguous();
	std::istringstream in(image_string(b));
	auto h = read_header(in);
	PeLib::ResourceDirectoryT<32> d;
	TASK_CHECK(d.read(in, h) == PeLib::ERROR_INVALID_FILE);
	return 0;
}

static int case_mapping_pe64()
{
	auto b = imagePe64();
	std::istringstream in(image_string(b));
	auto h = readHeader64(in);
	PeLib::ResourceDirectoryT<64> d;
	TASK_CHECK(d.read(in, h) == PeLib::ERROR_NONE);
	auto root = d.getRoot();
	TASK_CHECK(root->getNumberOfChildren() == 1);
	auto leaf = leafAt(root, 0, 0);
	TASK_CHECK(leaf != nullptr);
	TASK_CHECK(leafData(leaf) == "PE64OK");
	TASK_CHECK(leaf->getElementRva() == 0x2048);
	TASK_CHECK(leaf->getOffsetToData() == 0x2080);
	std::vector<PeLib::byte> data;
	d.getResourceData(16, 1, data);
	TASK_CHECK(std::string(data.begin(), data.end()) == "PE64OK");
	TASK_CHECK(hasRange(d, 0x2080, 0x2085));
	return 0;
}

// ---------------------------------------------------------------- Aliases

static int case_aliases_shared_subdirectory()
{
	auto b = imageShared();
	std::istringstream in(image_string(b));
	auto h = read_header(in);
	PeLib::ResourceDirectoryT<32> d;
	TASK_CHECK(d.read(in, h) == PeLib::ERROR_NONE);
	auto root = d.getRoot();
	TASK_CHECK(root->getNumberOfChildren() == 2);
	// Both references produce their own independently owned nodes.
	TASK_CHECK(root->getChild(0)->getNode() != root->getChild(1)->getNode());
	auto leafA = leafAt(root, 0, 0);
	auto leafB = leafAt(root, 1, 0);
	TASK_CHECK(leafA != nullptr && leafB != nullptr);
	TASK_CHECK(leafA != leafB);
	TASK_CHECK(leafData(leafA) == "SHARED");
	TASK_CHECK(leafData(leafB) == "SHARED");
	TASK_CHECK(leafA->getElementRva() == 0x2050);
	TASK_CHECK(leafB->getElementRva() == 0x2050);
	std::vector<PeLib::byte> data;
	d.getResourceData(16, 1, data);
	TASK_CHECK(std::string(data.begin(), data.end()) == "SHARED");
	d.getResourceData(2, 1, data);
	TASK_CHECK(std::string(data.begin(), data.end()) == "SHARED");
	// The shared subdirectory is read once per reference.
	TASK_CHECK(d.getOccupiedAddresses().size() == 14);
	return 0;
}

static int case_aliases_shared_and_mapped()
{
	auto b = imageSharedMapped();
	std::istringstream in(image_string(b));
	auto h = read_header(in);
	PeLib::ResourceDirectoryT<32> d;
	TASK_CHECK(d.read(in, h) == PeLib::ERROR_NONE);
	auto root = d.getRoot();
	TASK_CHECK(root->getNumberOfChildren() == 2);
	TASK_CHECK(root->getChild(0)->getNode() != root->getChild(1)->getNode());
	auto leafA = leafAt(root, 0, 0);
	auto leafB = leafAt(root, 1, 0);
	TASK_CHECK(leafA != nullptr && leafB != nullptr);
	TASK_CHECK(leafA != leafB);
	TASK_CHECK(leafData(leafA) == "MAPALIAS");
	TASK_CHECK(leafData(leafB) == "MAPALIAS");
	std::vector<PeLib::byte> data;
	d.getResourceData(5, 3, data);
	TASK_CHECK(std::string(data.begin(), data.end()) == "MAPALIAS");
	d.getResourceData(7, 3, data);
	TASK_CHECK(std::string(data.begin(), data.end()) == "MAPALIAS");
	TASK_CHECK(hasRange(d, 0x2044, 0x2053));
	TASK_CHECK(hasRange(d, 0x3000, 0x3007));
	return 0;
}

static int case_aliases_cycle_invalid()
{
	{
		auto b = imageCycle(0);      // entry points back to the root
		std::istringstream in(image_string(b));
		auto h = read_header(in);
		PeLib::ResourceDirectoryT<32> d;
		TASK_CHECK(d.read(in, h) == PeLib::ERROR_INVALID_FILE);
	}
	{
		auto b = imageCycle(0x18);   // entry points to the node itself
		std::istringstream in(image_string(b));
		auto h = read_header(in);
		PeLib::ResourceDirectoryT<32> d;
		TASK_CHECK(d.read(in, h) == PeLib::ERROR_INVALID_FILE);
	}
	return 0;
}

static int case_aliases_identical_leaves_not_merged()
{
	auto b = imageIdenticalLeaves();
	std::istringstream in(image_string(b));
	auto h = read_header(in);
	PeLib::ResourceDirectoryT<32> d;
	TASK_CHECK(d.read(in, h) == PeLib::ERROR_NONE);
	auto root = d.getRoot();
	TASK_CHECK(root->getNumberOfChildren() == 1);
	auto typeNode = asNode(root->getChild(0));
	TASK_CHECK(typeNode != nullptr);
	TASK_CHECK(typeNode->getNumberOfChildren() == 2);
	auto leafA = leafAt(root, 0, 0);
	auto leafB = leafAt(root, 0, 1);
	TASK_CHECK(leafA != nullptr && leafB != nullptr);
	TASK_CHECK(leafA != leafB);
	TASK_CHECK(leafData(leafA) == "SAMESAME");
	TASK_CHECK(leafData(leafB) == "SAMESAME");
	TASK_CHECK(leafA->getElementRva() == 0x2070);
	TASK_CHECK(leafB->getElementRva() == 0x2080);
	std::vector<PeLib::byte> data;
	d.getResourceData(16, 1, data);
	TASK_CHECK(std::string(data.begin(), data.end()) == "SAMESAME");
	d.getResourceData(16, 2, data);
	TASK_CHECK(std::string(data.begin(), data.end()) == "SAMESAME");
	return 0;
}

// ---------------------------------------------------------------- Reread

static int case_reread_replaces_state()
{
	PeLib::ResourceDirectoryT<32> d;
	{
		auto b = resource_image();
		std::istringstream in(image_string(b));
		auto h = read_header(in);
		TASK_CHECK(d.read(in, h) == PeLib::ERROR_NONE);
		TASK_CHECK(d.getRoot()->getNumberOfChildren() == 1);
		std::vector<PeLib::byte> data;
		d.getResourceData(16, 1, data);
		TASK_CHECK(std::string(data.begin(), data.end()) == "RSRC");
		TASK_CHECK(d.getOccupiedAddresses().size() == 8);
	}
	{
		auto b = imageSecond();
		std::istringstream in(image_string(b));
		auto h = read_header(in);
		TASK_CHECK(d.read(in, h) == PeLib::ERROR_NONE);
		// Only the second tree survives; nothing from the first read adds up.
		TASK_CHECK(d.getRoot()->getNumberOfChildren() == 2);
		TASK_CHECK(d.resourceTypeIdToIndex(16) == -1);
		TASK_CHECK(d.resourceTypeIdToIndex(5) == 0);
		TASK_CHECK(d.resourceTypeIdToIndex(6) == 1);
		std::vector<PeLib::byte> data;
		d.getResourceData(5, 9, data);
		TASK_CHECK(std::string(data.begin(), data.end()) == "SECOND");
		d.getResourceData(6, 8, data);
		TASK_CHECK(std::string(data.begin(), data.end()) == "THIRD!");
		TASK_CHECK(d.getOccupiedAddresses().size() == 14);
		TASK_CHECK(d.loaderError() == PeLib::LDR_ERROR_NONE);
	}
	return 0;
}

static int case_reread_loader_error_reset()
{
	PeLib::ResourceDirectoryT<32> d;
	{
		auto b = imageLoaderError();
		std::istringstream in(image_string(b));
		auto h = read_header(in);
		TASK_CHECK(d.read(in, h) == PeLib::ERROR_NONE);
		TASK_CHECK(d.loaderError() == PeLib::LDR_ERROR_RSRC_OVER_END_OF_IMAGE);
	}
	{
		auto b = resource_image();
		std::istringstream in(image_string(b));
		auto h = read_header(in);
		TASK_CHECK(d.read(in, h) == PeLib::ERROR_NONE);
		TASK_CHECK(d.loaderError() == PeLib::LDR_ERROR_NONE);
		std::vector<PeLib::byte> data;
		d.getResourceData(16, 1, data);
		TASK_CHECK(std::string(data.begin(), data.end()) == "RSRC");
		TASK_CHECK(d.getOccupiedAddresses().size() == 8);
	}
	return 0;
}

static int case_reread_shared_paths_independent()
{
	auto b = imageShared();
	std::istringstream in(image_string(b));
	auto h = read_header(in);
	PeLib::ResourceDirectoryT<32> d;
	TASK_CHECK(d.read(in, h) == PeLib::ERROR_NONE);
	TASK_CHECK(d.getRoot()->getNumberOfChildren() == 2);
	// Destroying one of the two paths into the shared subdirectory must not
	// affect the other path: each reference owns its tree nodes.
	TASK_CHECK(d.removeResourceTypeByIndex(0) == PeLib::ERROR_NONE);
	TASK_CHECK(d.getRoot()->getNumberOfChildren() == 1);
	std::vector<PeLib::byte> data;
	d.getResourceData(2, 1, data);
	TASK_CHECK(std::string(data.begin(), data.end()) == "SHARED");
	auto leaf = leafAt(d.getRoot(), 0, 0);
	TASK_CHECK(leaf != nullptr);
	TASK_CHECK(leafData(leaf) == "SHARED");
	return 0;
}

const task_case task_cases[] = {
	{"Mapping", "cross_section_structures", case_mapping_cross_section},
	{"Mapping", "scattered_payload", case_mapping_scattered_payload},
	{"Mapping", "hole_is_invalid", case_mapping_hole_invalid},
	{"Mapping", "ambiguous_mapping_is_invalid", case_mapping_ambiguous_invalid},
	{"Mapping", "pe64_sections", case_mapping_pe64},
	{"Aliases", "shared_subdirectory", case_aliases_shared_subdirectory},
	{"Aliases", "shared_and_mapped", case_aliases_shared_and_mapped},
	{"Aliases", "cycle_is_invalid", case_aliases_cycle_invalid},
	{"Aliases", "identical_leaves_not_merged", case_aliases_identical_leaves_not_merged},
	{"Reread", "replaces_state", case_reread_replaces_state},
	{"Reread", "loader_error_reset", case_reread_loader_error_reset},
	{"Reread", "shared_paths_independent", case_reread_shared_paths_independent},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
