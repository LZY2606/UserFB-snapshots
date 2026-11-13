#pragma once

#include "Core/ELF/ElfRelocator.h"

enum {
	R_ARM_ABS32 = 2,
	R_ARM_THM_CALL = 10,
	R_ARM_CALL = 28,
	R_ARM_JUMP24 = 29,
	R_ARM_TARGET1 = 38,
	R_ARM_V4BX = 40,
};


class ArmElfRelocator: public IElfRelocator
{
public:
	ArmElfRelocator(bool arm9): arm9(arm9) { }
	int expectedMachine() const override;
	bool isDummyRelocationType(int type) const override;

	bool relocateOpcode(int type, const RelocationData& data, std::vector<RelocationAction>& actions, std::vector<std::string>& errors) override;
	void setSymbolAddress(RelocationData& data, int64_t symbolAddress, int symbolType) override;

	void resetRelocationRun() override;
	std::vector<RelocationVeneerReservation> planVeneers() override;
	size_t getVeneerSize(const RelocationVeneer& veneer, int64_t entryAddress) const override;
	void assignVeneerAddresses(const std::vector<int64_t>& addresses) override;
	bool emitVeneers(std::vector<RelocationAction>& actions, std::vector<std::string>& errors) override;

	std::unique_ptr<CAssemblerCommand> generateCtorStub(std::vector<ElfRelocatorCtor>& ctors) override;
private:
	// a branch relocation that cannot always be encoded directly and may
	// have to be routed through an interworking veneer
	struct BranchRequest
	{
		int64_t opcodeOffset;	// virtual address of the branch instruction
		uint32_t opcode;		// original instruction contents
		int condition;			// ARM condition field (ARM call sites only)
		int callerState;		// 0 = ARM, 1 = Thumb
		bool withLink;			// R_ARM_CALL links, R_ARM_JUMP24 does not
		uint32_t targetAddress;	// true final destination (S+A+bias), including the Thumb mode bit
		int targetState;		// 0 = ARM, 1 = Thumb
		int32_t addend;			// addend decoded from the relocation instruction field
		int pcBias;				// hardware PC bias: 8 for ARM, 4 for Thumb
		size_t veneerIndex;		// index into the planned veneer list, or -1 if direct
	};

	static bool inDirectRange(const BranchRequest& request, int64_t destination);
	void encodeDirectBranch(const BranchRequest& request, int64_t destination,
		std::vector<RelocationAction>& actions, std::vector<std::string>& errors);
	std::vector<uint8_t> buildVeneer(const RelocationVeneer& veneer, int64_t entryAddress,
		std::vector<std::string>& errors) const;

	bool arm9;
	std::vector<BranchRequest> branches;
	std::vector<RelocationVeneer> uniqueVeneers;
	std::vector<int64_t> veneerEntryAddresses;
};
