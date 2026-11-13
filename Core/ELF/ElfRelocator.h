#pragma once

#include "Core/ELF/ElfFile.h"
#include "Core/Types.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

struct ElfRelocatorCtor
{
	Identifier symbolName;
	size_t size;
};

// Description of an interworking veneer that an architecture relocator needs
// the driver to allocate space for. The entry address is filled in by the
// driver once the veneer area has been laid out.
struct RelocationVeneer
{
	uint32_t targetAddress = 0;		// final destination including the mode bit
	int callerState = 0;			// instruction state of the call site: 0 = ARM, 1 = Thumb
	int targetState = 0;			// instruction state of the final target
};

// A veneer that the driver has to place at the end of the current code area.
struct RelocationVeneerReservation
{
	RelocationVeneer veneer;
	int alignment;				// required entry alignment
};

enum class RelocationActionKind
{
	SetOpcode,			// patch a 32-bit value into the relocated code
	WriteVeneer,		// write finished veneer bytes into the allocated veneer area
};

struct RelocationAction
{
	// ordinary opcode patch, keeps the existing two-argument construction
	RelocationAction(int64_t offset, uint32_t newValue)
		: kind(RelocationActionKind::SetOpcode), offset(offset), newValue(newValue) {}

	static RelocationAction writeVeneer(int64_t entryAddress, std::vector<uint8_t> bytes)
	{
		RelocationAction action;
		action.kind = RelocationActionKind::WriteVeneer;
		action.offset = entryAddress;
		action.veneerBytes = std::move(bytes);
		return action;
	}

	RelocationAction() : kind(RelocationActionKind::SetOpcode), offset(0), newValue(0) {}

	RelocationActionKind kind;
	int64_t offset;
	uint32_t newValue;
	std::vector<uint8_t> veneerBytes;
};

class CAssemblerCommand;
class Identifier;
class Parser;

class IElfRelocator
{
public:
	virtual ~IElfRelocator() {};
	virtual int expectedMachine() const = 0;
	virtual bool isDummyRelocationType(int type) const { return false; }
	virtual bool relocateOpcode(int type, const RelocationData& data, std::vector<RelocationAction>& actions, std::vector<std::string>& errors) = 0;
	virtual bool finish(std::vector<RelocationAction>& actions, std::vector<std::string>& errors) { return true; }
	virtual void setSymbolAddress(RelocationData& data, int64_t symbolAddress, int symbolType) = 0;

	// called once before every relocation run so that architecture specific
	// state does not accumulate across the assembler's validation passes
	virtual void resetRelocationRun() { }

	// returns the deduplicated veneers required by the processed relocations,
	// in a deterministic placement order
	virtual std::vector<RelocationVeneerReservation> planVeneers() { return {}; }

	// exact size of a veneer at the given (already aligned) entry address
	virtual size_t getVeneerSize(const RelocationVeneer& veneer, int64_t entryAddress) const { return 0; }

	// entry addresses of the reservations returned by planVeneers(), same order
	virtual void assignVeneerAddresses(const std::vector<int64_t>& addresses) { }

	// emits the final branch patches and veneer contents now that all entry
	// addresses are known
	virtual bool emitVeneers(std::vector<RelocationAction>& actions, std::vector<std::string>& errors) { return true; }

	virtual std::unique_ptr<CAssemblerCommand> generateCtorStub(std::vector<ElfRelocatorCtor>& ctors);
};


class Label;
class SymbolData;

struct ElfRelocatorSection
{
	ElfSection* section;
	size_t index;
	ElfSection* relSection;
	std::shared_ptr<Label> label;
};

struct ElfRelocatorSymbol
{
	std::shared_ptr<Label> label;
	std::string name;
	int64_t relativeAddress;
	int64_t relocatedAddress;
	size_t section;
	size_t size;
	int type;
};

struct ElfRelocatorFile
{
	ElfFile* elf;
	std::vector<ElfRelocatorSection> sections;
	std::vector<ElfRelocatorSymbol> symbols;
	std::string name;
};

// section contents copied out of an input object while relocations are gathered
struct ElfLoadedSection
{
	int64_t baseAddress;
	ByteArray data;
	bool isNoBits;
};

class ElfRelocator
{
public:
	bool init(const fs::path& inputName);
	bool exportSymbols();
	void writeSymbols(SymbolData& symData) const;
	std::unique_ptr<CAssemblerCommand> generateCtor(const Identifier& ctorName);
	bool relocate(int64_t& memoryAddress);
	bool hasDataChanged() { return dataChanged; };
	const ByteArray& getData() const { return outputData; };
private:
	bool gatherFile(ElfRelocatorFile& file, int64_t& relocationAddress);
	void loadRelocation(Elf32_Rela& rela, bool addend, ByteArray& data, int offset, Endianness endianness);

	ByteArray outputData;
	std::unique_ptr<IElfRelocator> relocator;
	std::vector<ElfRelocatorFile> files;
	std::vector<ElfRelocatorCtor> ctors;
	std::vector<RelocationVeneerReservation> plannedVeneers;
	std::vector<int64_t> veneerAddresses;
	std::vector<RelocationAction> plannedActions;
	std::vector<ElfLoadedSection> loadedSections;
	bool dataChanged;
};
