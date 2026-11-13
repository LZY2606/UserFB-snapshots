#include "Archs/ARM/ArmElfRelocator.h"

#include "Archs/ARM/Arm.h"
#include "Core/Common.h"
#include "Parser/Parser.h"
#include "Util/Util.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <tuple>

#include <tinyformat.h>

static inline int signExtend(int value, int bitsLength)
{
	return (value << (32-bitsLength)) >> (32-bitsLength);
}

static void putWord(std::vector<uint8_t>& bytes, uint32_t value)
{
	// little endian, matching the little-endian ARM ELF objects handled here
	bytes.push_back(value & 0xFF);
	bytes.push_back((value >> 8) & 0xFF);
	bytes.push_back((value >> 16) & 0xFF);
	bytes.push_back((value >> 24) & 0xFF);
}

static void putHalf(std::vector<uint8_t>& bytes, uint16_t value)
{
	bytes.push_back(value & 0xFF);
	bytes.push_back((value >> 8) & 0xFF);
}

// byte displacement of a Thumb long branch, relative to the unaligned Thumb
// PC value P+4. Using P+4 (not Align(P+4,4)) is what makes a 32 bit Thumb
// branch placed at a % 4 == 2 address resolve correctly.
static inline int64_t thumbBranchValue(int64_t opcodeOffset, int64_t destination)
{
	return destination - (opcodeOffset + 4);
}

// encode a Thumb T1 long branch (BL/BLX). All branches handled here are within
// the 23 bit displacement range, so J1=J2=1 always holds (it is precisely the
// sign extension of S) and the simple field split used by the assembler is
// exact for both word aligned and % 4 == 2 call sites.
static uint32_t encodeThumbLongBranch(uint32_t opcode, int64_t value, bool exchange)
{
	value >>= 1;

	uint16_t first = opcode & 0xFFFF;
	uint16_t second = (opcode >> 16) & 0xFFFF;
	first &= ~0x7FF;
	second &= ~0x7FF;
	second = exchange ? 0xE800 : 0xF800;
	first |= (value >> 11) & 0x7FF;
	second |= value & 0x7FF;
	return (uint32_t) first | ((uint32_t) second << 16);
}

bool ArmElfRelocator::isDummyRelocationType(int type) const
{
	// R_ARM_V4BX marks the position of a bx opcode, and does not
	// cause any actual relocations
	return type == R_ARM_V4BX;
}

int ArmElfRelocator::expectedMachine() const
{
	return EM_ARM;
}

/*
	S = symbol address
	T = 1 if symbol is a thumb mode function, 0 otherwise
	P = offset of opcode
	A = addend stored in the relocation instruction field
*/

bool ArmElfRelocator::inDirectRange(const BranchRequest& request, int64_t destination)
{
	if (request.callerState == 0)
	{
		// ARM B/BL: PC+8, 26 bit signed field in words
		if (destination % 4)
			return false;
		int64_t value = destination - (request.opcodeOffset + request.pcBias);
		return std::llabs(value) < 0x2000000;
	} else {
		// Thumb BL uses the unaligned P+4 PC value, including at % 4 == 2 sites
		if (destination % 2)
			return false;
		int64_t value = thumbBranchValue(request.opcodeOffset, destination);
		return std::llabs(value) < 0x400000;
	}
}

bool ArmElfRelocator::relocateOpcode(int type, const RelocationData& data, std::vector<RelocationAction>& actions, std::vector<std::string>& errors)
{
	int t = (data.targetSymbolType == STT_FUNC && data.targetSymbolInfo != 0) ? 1 : 0;
	int64_t s = data.relocationBase;

	uint32_t opcode = data.opcode;
	switch (type)
	{
	case R_ARM_ABS32:		// (S + A) | T
	case R_ARM_TARGET1:
		opcode = (int) (opcode + data.relocationBase) | t;
		actions.emplace_back(data.opcodeOffset, opcode);
		return true;
	case R_ARM_CALL:
	case R_ARM_JUMP24:
	case R_ARM_THM_CALL:
		break;
	default:
		errors.emplace_back(tfm::format("Unknown ARM relocation type %d",type));
		return false;
	}

	if (data.targetSymbolType != STT_FUNC)
	{
		errors.emplace_back(tfm::format("ARM branch relocation %d target is not a function",type));
		return false;
	}

	BranchRequest request;
	request.opcodeOffset = data.opcodeOffset;
	request.opcode = opcode;
	request.veneerIndex = (size_t)-1;
	request.targetState = t;
	request.condition = 0xE;

	if (type == R_ARM_THM_CALL)
	{
		// Thumb BL/BLX pair: addend is split across both halfwords,
		// PC bias is +4 (do not mix with the ARM PC+8 below)
		unsigned short first = opcode & 0xFFFF;
		unsigned short second = (opcode >> 16) & 0xFFFF;
		int opField = ((first & 0x7FF) << 11) | (second & 0x7FF);
		request.addend = signExtend(opField << 1,23);
		request.callerState = 1;
		request.pcBias = 4;
		request.withLink = true;
	} else {
		// ARM B/BL: 26 bit signed word offset, PC bias is +8
		request.condition = (opcode >> 28) & 0xF;
		int opField = (opcode & 0xFFFFFF) << 2;
		request.addend = signExtend(opField,26);
		request.callerState = 0;
		request.pcBias = 8;
		request.withLink = (type == R_ARM_CALL);
	}

	// The field stores a raw PC-relative displacement: hardware would compute
	// P + bias + A. Recover the true destination S + A + bias instead of
	// treating the field (or S+A) as a literal target address.
	int64_t trueTarget = s + request.addend + request.pcBias;
	if (trueTarget < 0 || trueTarget > 0xFFFFFFFF)
	{
		errors.emplace_back(tfm::format("Branch target %08X overflows the 32 bit address space",
			(unsigned int)(trueTarget & 0xFFFFFFFF)));
		return false;
	}

	if (t == 1)
	{
		if (trueTarget % 2)
		{
			errors.emplace_back("Branch target must be halfword aligned");
			return false;
		}
	} else {
		if (trueTarget % 4)
		{
			errors.emplace_back("Branch target must be word aligned");
			return false;
		}
	}

	// the mode bit stays part of the constants handed to the veneers
	request.targetAddress = (uint32_t) trueTarget | (uint32_t) t;

	// same state and within the original branch range: no veneer is possible,
	// keep the legacy direct encoding and emit it immediately
	if (request.callerState == request.targetState &&
		inDirectRange(request, request.targetAddress & ~1u))
	{
		encodeDirectBranch(request, request.targetAddress & ~1u, actions, errors);
		return errors.empty();
	}

	branches.push_back(request);
	return true;
}

std::vector<RelocationVeneerReservation> ArmElfRelocator::planVeneers()
{
	uniqueVeneers.clear();

	struct VeneerKey
	{
		int callerState;
		uint32_t targetAddress;
		int targetState;

		bool operator<(const VeneerKey& other) const
		{
			return std::tie(callerState,targetAddress,targetState)
				< std::tie(other.callerState,other.targetAddress,other.targetState);
		}
	};

	std::map<VeneerKey,size_t> veneerMap;

	for (BranchRequest& request: branches)
	{
		bool needsVeneer = false;

		// magnitude of the hardware displacement the original instruction
		// could encode, ignoring the cross-state alignment restrictions.
		// pcBias is 8 for ARM and 4 (the unaligned Thumb PC) for Thumb.
		int64_t pcBase = request.opcodeOffset + request.pcBias;
		int64_t displacement = (request.targetAddress & ~1u) - pcBase;
		bool withinRange = request.callerState == 0
			? std::llabs(displacement) < 0x2000000
			: std::llabs(displacement) < 0x400000;

		if (request.callerState != request.targetState)
		{
			// interworking: keep the direct BLX encodings ARM9 already supports.
			// ARM conditional calls and link-free tail jumps can never use BLX,
			// and the ARMv4T ARM7 has no BLX at all; even an eligible BLX still
			// needs a veneer when the target is out of its branch range
			bool directBlx = false;
			if (request.callerState == 0)
				directBlx = arm9 && request.withLink && request.condition == 0xE;
			else
				directBlx = arm9;	// Thumb BLX is always unconditional

			needsVeneer = !(directBlx && withinRange);
		} else {
			// same state: the in-range cases were already encoded eagerly, so
			// anything left here has an insufficient displacement
			needsVeneer = true;
		}

		if (!needsVeneer)
			continue;

		VeneerKey key { request.callerState, request.targetAddress, request.targetState };
		auto it = veneerMap.find(key);
		if (it == veneerMap.end())
		{
			RelocationVeneer veneer;
			veneer.targetAddress = request.targetAddress;
			veneer.callerState = request.callerState;
			veneer.targetState = request.targetState;

			it = veneerMap.emplace(key,uniqueVeneers.size()).first;
			uniqueVeneers.push_back(veneer);
		}

		request.veneerIndex = it->second;
	}

	std::vector<RelocationVeneerReservation> result;
	for (const RelocationVeneer& veneer: uniqueVeneers)
	{
		RelocationVeneerReservation reservation;
		reservation.veneer = veneer;
		// both entry kinds are word aligned: ARM veneers naturally, and Thumb
		// veneers because BX PC needs a word aligned target. A Thumb call site
		// at a % 4 == 2 address still reaches a word aligned entry via BL.
		reservation.alignment = 4;
		result.push_back(reservation);
	}

	return result;
}

size_t ArmElfRelocator::getVeneerSize(const RelocationVeneer& veneer, int64_t entryAddress) const
{
	(void) entryAddress;	// entries are always word aligned
	if (veneer.callerState == 0)
		return 12;	// LDR r12,[PC] :: BX r12 :: .word target

	// BX PC (Thumb) :: NOP :: LDR r12,[PC] :: BX r12 :: .word target
	return 16;
}

void ArmElfRelocator::assignVeneerAddresses(const std::vector<int64_t>& addresses)
{
	veneerEntryAddresses = addresses;
}

void ArmElfRelocator::resetRelocationRun()
{
	branches.clear();
	uniqueVeneers.clear();
	veneerEntryAddresses.clear();
}

void ArmElfRelocator::encodeDirectBranch(const BranchRequest& request, int64_t destination,
	std::vector<RelocationAction>& actions, std::vector<std::string>& errors)
{
	uint32_t opcode = request.opcode;

	if (request.callerState == 0)
	{
		// hardware displacement, accounting for the ARM PC+8 bias
		int64_t value = destination - (request.opcodeOffset + request.pcBias);
		if (std::llabs(value) >= 0x2000000)
		{
			errors.emplace_back(tfm::format("Branch target %08X out of range",(unsigned int)destination));
			return;
		}

		if (request.targetState == 1)
		{
			// ARM -> Thumb direct BLX, ARM9 only and unconditional BL
			if (destination % 2)
			{
				errors.emplace_back("Branch target must be halfword aligned");
				return;
			}

			opcode = 0xFA000000;
			if (value & 2)
				opcode |= (1 << 24);
		} else {
			if (destination % 4)
			{
				errors.emplace_back("Branch target must be word aligned");
				return;
			}

			// keep the original condition and link fields; only the offset changes
			opcode &= ~0xFFFFFF;
		}

		opcode |= (value >> 2) & 0xFFFFFF;
	} else {
		// displacement relative to the unaligned Thumb PC value P+4
		int64_t imm32 = thumbBranchValue(request.opcodeOffset, destination);
		if (destination % 2 || std::llabs(imm32) >= 0x400000)
		{
			errors.emplace_back(tfm::format("Branch target %08X out of range",(unsigned int)destination));
			return;
		}

		if (request.targetState == 0 && destination % 4)
		{
			errors.emplace_back("Branch target must be word aligned");
			return;
		}

		// BLX exchanges to ARM, BL stays in Thumb; the P+4 displacement split
		// is exact for % 4 == 2 call sites as well as word aligned ones
		opcode = encodeThumbLongBranch(opcode, imm32, request.targetState == 0);
	}

	actions.emplace_back(request.opcodeOffset, opcode);
}

std::vector<uint8_t> ArmElfRelocator::buildVeneer(const RelocationVeneer& veneer, int64_t entryAddress,
	std::vector<std::string>& errors) const
{
	(void) errors;
	std::vector<uint8_t> bytes;

	if (veneer.callerState == 0)
	{
		// ARM entry, word aligned. LR is never touched, so a caller BL keeps
		// its own return address and a tail jump stays a tail jump. BX r12
		// selects the target state from the mode bit of the literal.
		putWord(bytes,0xE59FC000);	// LDR r12,[PC,#0]  -> literal at entry+8
		putWord(bytes,0xE12FFF1C);	// BX r12
		putWord(bytes,veneer.targetAddress);
		return bytes;
	}

	// Thumb entry, word aligned. ARMv4T has no high-register literal LDR, so
	// switch into an ARM block with BX PC (valid ARMv4T) before loading the
	// absolute target. Only r12/ip is clobbered; LR, SP, flags and the stack
	// are untouched.
	putHalf(bytes,0x4778);			// BX PC -> ARM block at entry+4
	putHalf(bytes,0xBF00);			// padding, never executed
	putWord(bytes,0xE59FC000);		// LDR r12,[PC,#0] -> literal at entry+12
	putWord(bytes,0xE12FFF1C);		// BX r12
	putWord(bytes,veneer.targetAddress);
	return bytes;
}

bool ArmElfRelocator::emitVeneers(std::vector<RelocationAction>& actions, std::vector<std::string>& errors)
{
	bool ok = true;

	for (const BranchRequest& request: branches)
	{
		if (request.veneerIndex == (size_t)-1)
		{
			encodeDirectBranch(request, request.targetAddress & ~1u, actions, errors);
			continue;
		}

		int64_t veneerAddress = veneerEntryAddresses[request.veneerIndex];
		if (!inDirectRange(request, veneerAddress))
		{
			errors.emplace_back(tfm::format("Interworking veneer for target %08X is out of range",
				request.targetAddress));
			ok = false;
			continue;
		}

		if (request.callerState == 0)
		{
			// branch into the ARM veneer, preserving the original condition
			// (a failed conditional branch never enters the veneer) and link bit
			int64_t value = veneerAddress - (request.opcodeOffset + request.pcBias);
			uint32_t opcode = request.opcode & 0xFF000000;
			opcode |= (value >> 2) & 0xFFFFFF;
			actions.emplace_back(request.opcodeOffset, opcode);
		} else {
			// Thumb BL into the Thumb veneer; the link register still holds the
			// return address of the original call site. The displacement uses
			// the unaligned P+4 PC, so % 4 == 2 call sites work as well.
			int64_t imm32 = thumbBranchValue(request.opcodeOffset, veneerAddress);
			uint32_t opcode = encodeThumbLongBranch(request.opcode, imm32, false);
			actions.emplace_back(request.opcodeOffset, opcode);
		}
	}

	for (size_t i = 0; i < uniqueVeneers.size(); i++)
	{
		std::vector<uint8_t> bytes = buildVeneer(uniqueVeneers[i], veneerEntryAddresses[i], errors);
		actions.push_back(RelocationAction::writeVeneer(veneerEntryAddresses[i], std::move(bytes)));
	}

	return ok && errors.empty();
}

void ArmElfRelocator::setSymbolAddress(RelocationData& data, int64_t symbolAddress, int symbolType)
{
	if (symbolType == STT_FUNC)
	{
		data.targetSymbolInfo = symbolAddress & 1;
		symbolAddress &= ~1;
	}

	data.symbolAddress = symbolAddress;
	data.targetSymbolType = symbolType;
}

const char* ctorTemplate = R"(
	push	r4-r7,r14
	ldr	r4,=%ctorTable%
	ldr	r5,=%ctorTable%+%ctorTableSize%
	%outerLoopLabel%:
	ldr	r6,[r4]
	ldr	r7,[r4,4]
	add	r4,8
	%innerLoopLabel%:
	ldr	r0,[r6]
	add	r6,4
	.if %simpleMode%
		blx	r0
	.else
		bl	%stubName%
	.endif
	cmp	r6,r7
	blt	%innerLooplabel%
	cmp	r4,r5
	blt	%outerLoopLabel%
	.if %simpleMode%
		pop	r4-r7,r15
	.else
		pop	r4-r7
		pop	r0
		%stubName%:
		bx	r0
	.endif
	.pool
	%ctorTable%:
	.word %ctorContent%"
)";

std::unique_ptr<CAssemblerCommand> ArmElfRelocator::generateCtorStub(std::vector<ElfRelocatorCtor>& ctors)
{
	Parser parser;
	if (ctors.size() != 0)
	{
		bool simpleMode = arm9 == false && Arm.GetThumbMode();

		// create constructor table
		std::string table;
		for (size_t i = 0; i < ctors.size(); i++)
		{
			if (i != 0)
				table += ',';
			table += tfm::format("%s,%s+0x%08X",ctors[i].symbolName,ctors[i].symbolName,ctors[i].size);
		}

		return parser.parseTemplate(ctorTemplate,{
			{ "%ctorTable%",		Global.symbolTable.getUniqueLabelName().string() },
			{ "%ctorTableSize%",	tfm::format("%d",ctors.size()*8) },
			{ "%simpleMode%",		simpleMode ? "1" : "0" },
			{ "%outerLoopLabel%",	Global.symbolTable.getUniqueLabelName().string() },
			{ "%innerLoopLabel%",	Global.symbolTable.getUniqueLabelName().string() },
			{ "%stubName%",		Global.symbolTable.getUniqueLabelName().string() },
			{ "%ctorContent%",		table },
		});
	} else {
		return parser.parseTemplate("bx r14");
	}
}
