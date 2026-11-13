// Interworking veneer tests for the ARM ELF relocator.
//
// The cases build minimal little-endian ARM ELF32 ET_REL objects in C++, run
// them through the real armips .importobj path, and then verify the generated
// branches and veneers with a restricted ARM/Thumb step interpreter. No host
// machine code is ever executed.
#include "support/cases.h"

#include "Archs/ARM/ArmElfRelocator.h"
#include "Core/Assembler.h"
#include "Core/ELF/ElfTypes.h"
#include "Util/ByteArray.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if !defined(_WIN32)
extern "C" int getpid(void) noexcept;
#endif

namespace
{
	namespace fs = std::filesystem;

	// ------------------------------------------------------------------
	// tiny little-endian writers
	// ------------------------------------------------------------------
	void put8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }

	void put16(std::vector<uint8_t>& b, uint16_t v)
	{
		b.push_back(v & 0xFF);
		b.push_back((v >> 8) & 0xFF);
	}

	void put32(std::vector<uint8_t>& b, uint32_t v)
	{
		b.push_back(v & 0xFF);
		b.push_back((v >> 8) & 0xFF);
		b.push_back((v >> 16) & 0xFF);
		b.push_back((v >> 24) & 0xFF);
	}

	uint32_t get32(const std::vector<uint8_t>& b, size_t off)
	{
		return uint32_t(b[off]) | (uint32_t(b[off + 1]) << 8)
			| (uint32_t(b[off + 2]) << 16) | (uint32_t(b[off + 3]) << 24);
	}

	uint16_t get16(const std::vector<uint8_t>& b, size_t off)
	{
		return uint16_t(b[off]) | uint16_t(b[off + 1] << 8);
	}

	// ------------------------------------------------------------------
	// minimal ARM ELF32 ET_REL builder
	// ------------------------------------------------------------------
	struct ElfSymbol
	{
		std::string name;
		uint32_t value;
		uint8_t type;
	};

	struct ElfReloc
	{
		uint32_t offset;
		uint32_t symbol;
		uint8_t type;
	};

	class ElfObject
	{
	public:
		void align(unsigned a)
		{
			while (text.size() % a)
				put8(text, 0);
		}

		size_t position() const { return text.size(); }

		void emitWord(uint32_t v) { put32(text, v); }
		void emitHalf(uint16_t v) { put16(text, v); }

		// reserve a symbol index; its value is assigned when its code is placed
		size_t addFunction(const std::string& name)
		{
			symbols.push_back(ElfSymbol { name, 0, STT_FUNC });
			return symbols.size() - 1;
		}

		size_t addObject(const std::string& name)
		{
			symbols.push_back(ElfSymbol { name, 0, STT_OBJECT });
			return symbols.size() - 1;
		}

		uint32_t place(size_t index, bool thumb)
		{
			uint32_t value = uint32_t(text.size()) | (thumb ? 1u : 0u);
			symbols[index].value = value;
			return value;
		}

		void setValue(size_t index, uint32_t value) { symbols[index].value = value; }

		// canonical GCC placeholders: the field addend compensates the PC bias,
		// so the resolved target is exactly the symbol address
		void armBl(size_t sym, unsigned cond = 0xE)
		{
			relocs.push_back(ElfReloc { uint32_t(text.size()), uint32_t(sym), R_ARM_CALL });
			emitWord((cond << 28) | 0x0BFFFFFE);
		}

		void armB(size_t sym, unsigned cond = 0xE)
		{
			relocs.push_back(ElfReloc { uint32_t(text.size()), uint32_t(sym), R_ARM_JUMP24 });
			emitWord((cond << 28) | 0x0AFFFFFE);
		}

		void thumbBl(size_t sym)
		{
			relocs.push_back(ElfReloc { uint32_t(text.size()), uint32_t(sym), R_ARM_THM_CALL });
			emitHalf(0xF7FF);
			emitHalf(0xFFFE);
		}

		void abs32(size_t sym)
		{
			relocs.push_back(ElfReloc { uint32_t(text.size()), uint32_t(sym), R_ARM_ABS32 });
			emitWord(0);
		}

		void target1(size_t sym)
		{
			relocs.push_back(ElfReloc { uint32_t(text.size()), uint32_t(sym), R_ARM_TARGET1 });
			emitWord(0);
		}

		void rawReloc(size_t sym, uint8_t type, uint32_t opcode)
		{
			relocs.push_back(ElfReloc { uint32_t(text.size()), uint32_t(sym), type });
			emitWord(opcode);
		}

		const std::vector<uint8_t>& build()
		{
			std::vector<uint8_t> strtab { 0 };
			std::vector<uint32_t> nameOff(symbols.size());
			for (size_t i = 0; i < symbols.size(); i++)
			{
				nameOff[i] = (uint32_t) strtab.size();
				strtab.insert(strtab.end(), symbols[i].name.begin(), symbols[i].name.end());
				strtab.push_back(0);
			}

			struct ShName { const char* name; uint32_t off; };
			std::vector<ShName> shNames = {
				{ ".text", 0 }, { ".rel.text", 0 }, { ".symtab", 0 },
				{ ".strtab", 0 }, { ".shstrtab", 0 },
			};
			std::vector<uint8_t> shstrtab { 0 };
			for (ShName& n: shNames)
			{
				n.off = (uint32_t) shstrtab.size();
				shstrtab.insert(shstrtab.end(), n.name, n.name + std::strlen(n.name));
				shstrtab.push_back(0);
			}

			std::vector<uint8_t> symtab;
			put32(symtab, 0); put32(symtab, 0); put32(symtab, 0); // null symbol
			put8(symtab, 0); put8(symtab, 0); put16(symtab, 0);
			for (size_t i = 0; i < symbols.size(); i++)
			{
				uint8_t info = uint8_t((STB_GLOBAL << 4) | (symbols[i].type & 0xF));
				put32(symtab, nameOff[i]);
				put32(symtab, symbols[i].value);
				put32(symtab, 0);
				put8(symtab, info);
				put8(symtab, 0);
				put16(symtab, 1); // section 1 = .text
			}

			std::vector<uint8_t> rel;
			for (const ElfReloc& r: relocs)
			{
				// symtab entry 0 is the reserved null symbol, so a builder symbol
				// index i lives at ELF symbol index i+1
				put32(rel, r.offset);
				put32(rel, ((r.symbol + 1) << 8) | r.type);
			}

			const unsigned textIdx = 1, relIdx = 2, symIdx = 3, strIdx = 4, shstrIdx = 5;

			std::vector<uint8_t> out;
			out.resize(52, 0);
			out[0] = 0x7F; out[1] = 'E'; out[2] = 'L'; out[3] = 'F';
			out[4] = ELFCLASS32; out[5] = ELFDATA2LSB; out[6] = EV_CURRENT;

			uint32_t off = 52;
			while (off % 4) off++;
			uint32_t textOff = off; off += (uint32_t) text.size();
			auto placeBlob = [&](const std::vector<uint8_t>& blob, unsigned alignment)
			{
				while (off % alignment) off++;
				uint32_t at = off;
				off += (uint32_t) blob.size();
				return at;
			};
			uint32_t relOff = placeBlob(rel, 4);
			uint32_t symOff = placeBlob(symtab, 4);
			uint32_t strOff = placeBlob(strtab, 1);
			uint32_t shstrOff = placeBlob(shstrtab, 1);
			while (off % 4) off++;
			uint32_t shoff = off;
			out.resize(shoff, 0);

			auto writeAt = [&](uint32_t at, const std::vector<uint8_t>& blob)
			{
				if (out.size() < at + blob.size())
					out.resize(at + blob.size(), 0);
				std::memcpy(out.data() + at, blob.data(), blob.size());
			};
			writeAt(textOff, text);
			writeAt(relOff, rel);
			writeAt(symOff, symtab);
			writeAt(strOff, strtab);
			writeAt(shstrOff, shstrtab);
			out.resize(shoff, 0);

			auto shdr = [&](uint32_t name, uint32_t type, uint32_t flags, uint32_t addr,
				uint32_t offset, uint32_t size, uint32_t link, uint32_t info,
				uint32_t alignment, uint32_t entsize)
			{
				put32(out, name); put32(out, type); put32(out, flags); put32(out, addr);
				put32(out, offset); put32(out, size); put32(out, link); put32(out, info);
				put32(out, alignment); put32(out, entsize);
			};
			shdr(0, 0, 0, 0, 0, 0, 0, 0, 0, 0); // NULL
			shdr(shNames[0].off, SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, 0, textOff,
				(uint32_t) text.size(), 0, 0, 4, 0);
			shdr(shNames[1].off, SHT_REL, 0, 0, relOff, (uint32_t) rel.size(),
				symIdx, textIdx, 4, 8);
			shdr(shNames[2].off, SHT_SYMTAB, 0, 0, symOff, (uint32_t) symtab.size(),
				strIdx, 1, 4, 16);
			shdr(shNames[3].off, SHT_STRTAB, 0, 0, strOff, (uint32_t) strtab.size(),
				0, 0, 1, 0);
			shdr(shNames[4].off, SHT_STRTAB, 0, 0, shstrOff, (uint32_t) shstrtab.size(),
				0, 0, 1, 0);

			auto h16 = [&](unsigned p, uint16_t v) { out[p] = v & 0xFF; out[p + 1] = v >> 8; };
			auto h32 = [&](unsigned p, uint32_t v)
			{
				out[p] = v & 0xFF; out[p + 1] = (v >> 8) & 0xFF;
				out[p + 2] = (v >> 16) & 0xFF; out[p + 3] = v >> 24;
			};
			h16(0x10, ET_REL);
			h16(0x12, EM_ARM);
			h32(0x14, EV_CURRENT);
			h32(0x1C, 0);
			h32(0x20, shoff);
			h16(0x28, 52);
			h16(0x2A, 0);
			h16(0x2C, 0);
			h16(0x2E, 40);
			h16(0x30, 6);
			h16(0x32, shstrIdx);

			blob = std::move(out);
			return blob;
		}

		std::vector<uint8_t> text;
		std::vector<ElfSymbol> symbols;
		std::vector<ElfReloc> relocs;
		std::vector<uint8_t> blob;
	};

	// ------------------------------------------------------------------
	// run an armips .importobj script in a scratch directory
	// ------------------------------------------------------------------
	struct ImportResult
	{
		bool ok;
		std::vector<std::string> errors;
		std::vector<uint8_t> output;
	};

	std::string scratchDir()
	{
		static unsigned counter = 0;
		const char* tmp = std::getenv("TMPDIR");
		std::string base = tmp ? tmp : "/tmp";
#if defined(_WIN32)
		int pid = 0;
#else
		int pid = getpid();
#endif
		std::ostringstream ss;
		ss << base << "/arm_interwork_" << pid << "_" << counter++;
		return ss.str();
	}

	void writeFile(const std::string& path, const std::vector<uint8_t>& data)
	{
		std::ofstream f(path, std::ios::binary | std::ios::trunc);
		f.write(reinterpret_cast<const char*>(data.data()), (std::streamsize) data.size());
	}

	void writeText(const std::string& path, const std::string& text)
	{
		std::ofstream f(path, std::ios::binary | std::ios::trunc);
		f.write(text.data(), (std::streamsize) text.size());
	}

	ImportResult runImport(const std::vector<uint8_t>& object, const std::string& archDirective,
		const std::string& tail = "")
	{
		std::string dir = scratchDir();
		fs::create_directories(dir);
		std::string objPath = dir + "/t.o";
		std::string asmPath = dir + "/t.asm";
		std::string outPath = dir + "/out.bin";
		writeFile(objPath, object);

		std::ostringstream asmText;
		asmText << ".create \"" << outPath << "\",0x08000000\n"
			<< archDirective << "\n"
			<< ".importobj \"" << objPath << "\"\n"
			<< tail
			<< ".close\n";
		writeText(asmPath, asmText.str());

		ArmipsArguments args;
		args.mode = ArmipsMode::FILE;
		args.inputFileName = fs::path(asmPath);
		args.silent = true;
		std::vector<std::string> errors;
		args.errorsResult = &errors;
		bool ok = runArmips(args);

		ImportResult result;
		result.ok = ok;
		result.errors = errors;
		ByteArray bin = ByteArray::fromFile(fs::path(outPath));
		result.output.assign(bin.data(), bin.data() + bin.size());

		std::error_code ec;
		fs::remove_all(dir, ec);
		return result;
	}

	constexpr uint32_t BASE = 0x08000000;
	constexpr uint16_t THUMB_BX_LR = 0x4770;
	constexpr uint32_t ARM_BX_LR = 0xE12FFF1E;

	// ------------------------------------------------------------------
	// restricted ARM/Thumb step interpreter (verification only)
	// ------------------------------------------------------------------
	struct Machine
	{
		const std::vector<uint8_t>& mem;
		uint32_t base;
		uint32_t r[16];
		bool thumb;
		bool nzcv[4]; // N Z C V
		unsigned steps;

		Machine(const std::vector<uint8_t>& memory, uint32_t imageBase, uint32_t pc, bool isThumb)
			: mem(memory), base(imageBase), thumb(isThumb), steps(0)
		{
			for (unsigned i = 0; i < 16; i++)
				r[i] = 0x40000000 + i * 0x111;
			r[15] = pc;
			for (bool& f: nzcv) f = false;
			nzcv[1] = true; // Z set so an EQ condition passes by default
		}

		uint32_t rd32(uint32_t addr) const { return get32(mem, addr - base); }
		uint16_t rd16(uint32_t addr) const { return get16(mem, addr - base); }

		static bool condPassed(unsigned cond, const bool flags[4])
		{
			switch (cond)
			{
			case 0: return flags[1];	// EQ
			case 1: return !flags[1];	// NE
			case 0xE: return true;		// AL
			default: throw std::runtime_error("interpreter: unsupported condition");
			}
		}

		// executes one instruction; state changes (including exchanges) are
		// reflected directly in r15/thumb
		void step()
		{
			if (++steps > 20)
				throw std::runtime_error("interpreter: did not reach target");
			if (thumb)
				stepThumb();
			else
				stepArm();
		}

		void stepArm()
		{
			uint32_t pc = r[15];
			uint32_t insn = rd32(pc);
			unsigned cond = insn >> 28;
			if (!condPassed(cond, nzcv))
			{
				r[15] = pc + 4;
				return;
			}

			if ((insn & 0x0E000000) == 0x0A000000) // B/BL/BLX imm24
			{
				bool link = (insn & (1 << 24)) != 0;
				int32_t imm24 = int32_t(insn << 8) >> 8;

				if ((insn & 0xFE000000) == 0xFA000000) // BLX -> Thumb
				{
					uint32_t h = (insn >> 24) & 1;
					if (link) r[14] = pc + 8;
					r[15] = (pc + 8 + (imm24 << 2) + (h << 1)) & ~3u;
					thumb = true;
					return;
				}

				if (link) r[14] = pc + 8;
				r[15] = pc + 8 + (imm24 << 2);
				return;
			}

			if ((insn & 0x0FFFFFF0) == 0x012FFF10) // BX Rn
			{
				unsigned rn = insn & 0xF;
				// reading PC gives the ARM PC value (pc + 8)
				uint32_t addr = rn == 15 ? pc + 8 : r[rn];
				r[15] = addr & ~1u;
				thumb = (addr & 1) != 0;
				return;
			}

			if ((insn & 0x0F7F0000) == 0x051F0000) // LDR Rt,[PC,#imm]
			{
				unsigned rt = (insn >> 12) & 0xF;
				bool up = (insn & (1 << 23)) != 0;
				uint32_t imm = insn & 0xFFF;
				uint32_t addr = (pc + 8) & ~3u;
				addr = up ? addr + imm : addr - imm;
				r[rt] = rd32(addr);
				r[15] = pc + 4;
				return;
			}

			throw std::runtime_error("interpreter: unknown ARM instruction");
		}

		void stepThumb()
		{
			uint32_t pc = r[15];
			uint16_t hw = rd16(pc);

			if ((hw & 0xF800) == 0xF000) // 32-bit BL/BLX (T1)
			{
				uint16_t hw2 = rd16(pc + 2);
				uint32_t s = (hw >> 10) & 1;
				uint32_t j1 = (hw2 >> 13) & 1;
				uint32_t j2 = (hw2 >> 11) & 1;
				uint32_t i1 = (~(j1 ^ s)) & 1;
				uint32_t i2 = (~(j2 ^ s)) & 1;
				int32_t imm32 = (s << 24) | (i1 << 23) | (i2 << 22)
					| ((hw & 0x3FF) << 12) | ((hw2 & 0x7FF) << 1);
				if (s) imm32 |= int32_t(0xFE000000); // sign extend bit 24
				// Thumb long branches read the unaligned PC value P+4
				uint32_t thumbPc = pc + 4;

				if ((hw2 & 0xD000) == 0xC000) // BLX -> ARM
				{
					r[14] = (pc + 4) | 1;
					r[15] = (thumbPc + imm32) & ~3u;
					thumb = false;
					return;
				}

				r[14] = (pc + 4) | 1; // Thumb return state bit retained
				r[15] = thumbPc + imm32;
				return;
			}

			if ((hw & 0xFF80) == 0x4700) // BX Rm (T1)
			{
				unsigned rm = ((hw & 0x78) >> 3) | (hw & 0x7);
				// BX PC reads Align(PC+4,4), which is what switches the Thumb
				// veneer into its word aligned ARM block
				uint32_t addr = rm == 15 ? ((pc + 4) & ~3u) : r[rm];
				r[15] = addr & ~1u;
				thumb = (addr & 1) != 0;
				return;
			}

			if (hw == 0xBF00) // NOP
			{
				r[15] = pc + 2;
				return;
			}

			if ((hw & 0xF800) == 0xE000) // unconditional B (T2)
			{
				int32_t off = (hw & 0x7FF) << 1;
				if (off & 0x1000) off |= int32_t(0xFFFFF000);
				r[15] = pc + 4 + off;
				return;
			}

			throw std::runtime_error("interpreter: unknown Thumb instruction");
		}

		void runUntilLeaves(uint32_t expectedTarget, bool expectedThumb)
		{
			uint32_t wanted = expectedTarget & ~1u;
			while (true)
			{
				step();
				if (r[15] == wanted && thumb == expectedThumb)
					return;
				if (r[15] < base || r[15] >= base + (uint32_t) mem.size())
					throw std::runtime_error("interpreter: left image unexpectedly");
			}
		}
	};

	// Thumb veneer entries begin with BX PC;NOP. Each Thumb veneer also embeds
	// one ARM LDR-literal, so ARM-entry veneers = LDR count minus Thumb entries.
	size_t countThumbVeneerEntries(const std::vector<uint8_t>& out)
	{
		size_t n = 0;
		for (size_t i = 0; i + 4 <= out.size(); i += 2)
			if (get16(out, i) == 0x4778 && get16(out, i + 2) == 0xBF00)
				n++;
		return n;
	}

	size_t countArmVeneerEntries(const std::vector<uint8_t>& out)
	{
		size_t ldrLiterals = 0;
		for (size_t i = 0; i + 4 <= out.size(); i += 4)
			if (get32(out, i) == 0xE59FC000)
				ldrLiterals++;
		return ldrLiterals - countThumbVeneerEntries(out);
	}

	void checkGeneralRegistersPreserved(const Machine& m)
	{
		for (unsigned i = 0; i < 12; i++)
			TASK_CHECK(m.r[i] == 0x40000000 + i * 0x111);
		TASK_CHECK(m.r[13] == 0x40000000 + 13 * 0x111); // SP untouched
	}

	// ==================================================================
	// cases
	// ==================================================================
	void caseArmToThumbGba()
	{
		ElfObject o;
		size_t caller = o.addFunction("arm_caller");
		size_t callee = o.addFunction("thumb_callee");

		o.place(caller, false);
		o.armBl(callee);              // ARM BL -> Thumb, needs a veneer on ARM7
		o.emitWord(ARM_BX_LR);
		o.align(2);
		uint32_t target = o.place(callee, true);
		o.emitHalf(THUMB_BX_LR);

		ImportResult r = runImport(o.build(), ".gba");
		TASK_CHECK(r.ok);
		TASK_CHECK(r.errors.empty());
		TASK_CHECK(countArmVeneerEntries(r.output) == 1);
		TASK_CHECK(countThumbVeneerEntries(r.output) == 0);

		Machine m(r.output, BASE, BASE, false);
		m.runUntilLeaves(BASE + (target & ~1u), true);
		checkGeneralRegistersPreserved(m);
		TASK_CHECK(m.r[14] == BASE + 8); // LR produced by the original call site
	}

	void caseThumbToArmGba()
	{
		ElfObject o;
		size_t caller = o.addFunction("thumb_caller");
		size_t callee = o.addFunction("arm_callee");

		o.place(caller, true);
		o.thumbBl(callee);            // Thumb BL -> ARM, needs a veneer on ARM7
		o.emitHalf(0xBF00);
		o.align(4);
		uint32_t target = o.place(callee, false);
		o.emitWord(ARM_BX_LR);

		ImportResult r = runImport(o.build(), ".gba");
		TASK_CHECK(r.ok);
		TASK_CHECK(r.errors.empty());
		TASK_CHECK(countThumbVeneerEntries(r.output) == 1);
		TASK_CHECK(countArmVeneerEntries(r.output) == 0);

		Machine m(r.output, BASE, BASE, true);
		m.runUntilLeaves(BASE + target, false);
		checkGeneralRegistersPreserved(m);
		TASK_CHECK(m.r[14] == ((BASE + 4) | 1)); // Thumb return state bit retained
	}

	void caseThumbHalfwordAlignedCallSite()
	{
		ElfObject o;
		size_t caller = o.addFunction("thumb_caller");
		size_t callee = o.addFunction("arm_callee");

		o.place(caller, true);
		o.emitHalf(0xBF00);           // offset 0
		o.thumbBl(callee);            // 32-bit BL at offset 2 (% 4 == 2)
		o.align(4);
		uint32_t target = o.place(callee, false);
		o.emitWord(ARM_BX_LR);

		ImportResult r = runImport(o.build(), ".gba");
		TASK_CHECK(r.ok);
		TASK_CHECK(r.errors.empty());
		TASK_CHECK(countThumbVeneerEntries(r.output) == 1);

		Machine m(r.output, BASE, BASE + 2, true);
		m.runUntilLeaves(BASE + target, false);
		TASK_CHECK(m.r[14] == ((BASE + 2 + 4) | 1));
	}

	void caseConditionalArmCall()
	{
		ElfObject o;
		size_t caller = o.addFunction("arm_caller");
		size_t callee = o.addFunction("thumb_callee");

		o.place(caller, false);
		o.armBl(callee, 0x0);         // BLEQ: condition stays on the original insn
		o.align(2);
		uint32_t target = o.place(callee, true);
		o.emitHalf(THUMB_BX_LR);

		ImportResult r = runImport(o.build(), ".gba");
		TASK_CHECK(r.ok);
		TASK_CHECK(countArmVeneerEntries(r.output) == 1);

		// condition true: the branch enters the veneer and reaches the target
		Machine taken(r.output, BASE, BASE, false);
		taken.runUntilLeaves(BASE + (target & ~1u), true);
		TASK_CHECK(taken.r[14] == BASE + 8);

		// condition false: the branch must not enter the veneer at all
		Machine skipped(r.output, BASE, BASE, false);
		skipped.nzcv[1] = false;
		uint32_t ipBefore = skipped.r[12];
		skipped.step();
		TASK_CHECK(skipped.r[15] == BASE + 4);
		TASK_CHECK(skipped.r[12] == ipBefore);
	}

	void caseTailJumpArmToThumb()
	{
		ElfObject o;
		size_t caller = o.addFunction("arm_caller");
		size_t callee = o.addFunction("thumb_callee");

		o.place(caller, false);
		o.armB(callee);               // R_ARM_JUMP24: no link, tail jump
		o.align(2);
		uint32_t target = o.place(callee, true);
		o.emitHalf(THUMB_BX_LR);

		ImportResult r = runImport(o.build(), ".gba");
		TASK_CHECK(r.ok);
		TASK_CHECK(countArmVeneerEntries(r.output) == 1);

		Machine m(r.output, BASE, BASE, false);
		uint32_t lrSentinel = m.r[14];
		m.runUntilLeaves(BASE + (target & ~1u), true);
		TASK_CHECK(m.r[14] == lrSentinel); // a tail jump never writes LR
	}

	void caseShareAndIsolate()
	{
		// two ARM call sites to the same Thumb target share one veneer
		ElfObject o;
		size_t caller = o.addFunction("arm_caller");
		size_t callee = o.addFunction("shared_thumb");

		o.place(caller, false);
		o.armBl(callee);
		o.armBl(callee);
		o.emitWord(ARM_BX_LR);
		o.align(2);
		uint32_t target = o.place(callee, true);
		o.emitHalf(THUMB_BX_LR);
		(void) target;

		ImportResult r = runImport(o.build(), ".gba");
		TASK_CHECK(r.ok);
		TASK_CHECK(countArmVeneerEntries(r.output) == 1);

		// the same numeric address, entered from different states, must produce
		// two isolated veneers even though the address value is identical
		ElfObject o2;
		size_t armCaller = o2.addFunction("arm_caller");
		size_t thumbCaller = o2.addFunction("thumb_caller");
		size_t thumbSym = o2.addFunction("fn_thumb");
		size_t armSym = o2.addFunction("fn_arm");

		o2.place(armCaller, false);
		o2.armBl(thumbSym);          // ARM state entry
		o2.align(2);
		o2.place(thumbCaller, true);
		o2.emitHalf(0xBF00);
		o2.thumbBl(armSym);          // Thumb state entry
		o2.align(4);
		uint32_t shared = uint32_t(o2.position());
		o2.setValue(thumbSym, shared | 1); // both resolve to address `shared`
		o2.setValue(armSym, shared);
		o2.emitWord(ARM_BX_LR);

		ImportResult r2 = runImport(o2.build(), ".gba");
		TASK_CHECK(r2.ok);
		TASK_CHECK(countArmVeneerEntries(r2.output) == 1);
		TASK_CHECK(countThumbVeneerEntries(r2.output) == 1);

		// behavioral isolation: each call site reaches `shared` in its own state
		Machine fromArm(r2.output, BASE, BASE, false);
		fromArm.runUntilLeaves(BASE + shared, true);
		Machine fromThumb(r2.output, BASE, BASE + 4, true);
		fromThumb.runUntilLeaves(BASE + shared, false);
	}

	void caseDirectEncodingUnchanged()
	{
		ElfObject o;
		size_t armCaller = o.addFunction("arm_caller");
		size_t thumbCaller = o.addFunction("thumb_caller");
		size_t armTgt = o.addFunction("arm_callee");
		size_t thumbTgt = o.addFunction("thumb_callee");

		o.place(armCaller, false);
		o.armBl(armTgt);              // ARM -> ARM, in range
		o.align(2);
		o.place(thumbCaller, true);
		o.emitHalf(0xBF00);
		o.thumbBl(thumbTgt);          // Thumb -> Thumb, in range
		o.align(4);
		uint32_t armTarget = o.place(armTgt, false);
		o.emitWord(ARM_BX_LR);
		o.place(thumbTgt, true);
		o.emitHalf(THUMB_BX_LR);

		ImportResult r = runImport(o.build(), ".gba");
		TASK_CHECK(r.ok);
		TASK_CHECK(countArmVeneerEntries(r.output) == 0);
		TASK_CHECK(countThumbVeneerEntries(r.output) == 0);
		TASK_CHECK(r.output.size() == o.text.size()); // no veneer area appended

		// the ARM BL still resolves directly to the ARM function
		uint32_t insn = get32(r.output, 0);
		int32_t imm24 = int32_t(insn << 8) >> 8;
		TASK_CHECK(BASE + 8 + (imm24 << 2) == BASE + armTarget);
	}

	void caseArm9DirectBlxRetained()
	{
		ElfObject o;
		size_t armCaller = o.addFunction("arm_caller");
		size_t thumbCaller = o.addFunction("thumb_caller");
		size_t thumbTgt = o.addFunction("thumb_callee");
		size_t armTgt = o.addFunction("arm_callee");

		o.place(armCaller, false);
		o.armBl(thumbTgt);            // ARM -> Thumb: ARM9 keeps a direct BLX
		o.align(2);
		o.place(thumbCaller, true);
		o.emitHalf(0xBF00);
		o.thumbBl(armTgt);            // Thumb -> ARM: ARM9 keeps a direct BLX
		o.align(4);
		o.place(armTgt, false);
		o.emitWord(ARM_BX_LR);
		o.place(thumbTgt, true);
		o.emitHalf(THUMB_BX_LR);

		ImportResult r = runImport(o.build(), ".nds");
		TASK_CHECK(r.ok);
		TASK_CHECK(countArmVeneerEntries(r.output) == 0);
		TASK_CHECK(countThumbVeneerEntries(r.output) == 0);

		TASK_CHECK((get32(r.output, 0) & 0xFE000000) == 0xFA000000); // ARM BLX
	}

	void caseThumbHalfwordAlignedDirectBl()
	{
		// a same-state Thumb BL placed at a % 4 == 2 address must still encode
		// directly (no veneer) using the unaligned P+4 PC value
		ElfObject o;
		size_t caller = o.addFunction("thumb_caller");
		size_t callee = o.addFunction("thumb_callee");

		o.place(caller, true);
		o.emitHalf(0xBF00);   // offset 0
		o.thumbBl(callee);    // BL at offset 2 (% 4 == 2)
		o.align(4);
		uint32_t target = o.place(callee, true);
		o.emitHalf(THUMB_BX_LR);

		ImportResult r = runImport(o.build(), ".gba");
		TASK_CHECK(r.ok);
		TASK_CHECK(countThumbVeneerEntries(r.output) == 0);
		TASK_CHECK(countArmVeneerEntries(r.output) == 0);
		TASK_CHECK(r.output.size() == o.text.size());

		// independent decoding with the assembler's P+4 displacement rule:
		// value = target - (2 + 4); halfwords split as (value>>1)
		int32_t value = int32_t(BASE + (target & ~1u)) - int32_t(BASE + 2 + 4);
		int32_t half = value >> 1;
		uint16_t expectFirst = 0xF000 | ((half >> 11) & 0x7FF);
		uint16_t expectSecond = 0xF800 | (half & 0x7FF);
		TASK_CHECK(get16(r.output, 2) == expectFirst);
		TASK_CHECK(get16(r.output, 4) == expectSecond);

		Machine m(r.output, BASE, BASE + 2, true);
		m.runUntilLeaves(BASE + (target & ~1u), true);
	}

	void caseAbs32KeepsModeBit()
	{
		ElfObject o;
		size_t thumbFn = o.addFunction("thumb_fn");
		size_t armFn = o.addFunction("arm_fn");

		o.abs32(thumbFn);
		o.target1(armFn);
		o.align(2);
		o.place(thumbFn, true);
		o.emitHalf(THUMB_BX_LR);
		o.align(4);
		o.place(armFn, false);
		o.emitWord(ARM_BX_LR);

		ImportResult r = runImport(o.build(), ".gba");
		TASK_CHECK(r.ok);
		TASK_CHECK((get32(r.output, 0) & 1) == 1); // Thumb mode bit preserved
		TASK_CHECK((get32(r.output, 4) & 1) == 0);
	}

	void caseMultipassStable()
	{
		ElfObject o;
		size_t caller = o.addFunction("arm_caller");
		size_t callee = o.addFunction("thumb_callee");

		o.place(caller, false);
		o.armBl(callee);
		o.align(2);
		o.place(callee, true);
		o.emitHalf(THUMB_BX_LR);

		// a marker emitted after the import must land at the final, stable size
		const std::string tail = "postimport:\n.word 0x12345678\n";
		ImportResult r1 = runImport(o.build(), ".gba", tail);
		ImportResult r2 = runImport(o.build(), ".gba", tail);
		ImportResult r3 = runImport(o.build(), ".gba", tail);
		TASK_CHECK(r1.ok && r2.ok && r3.ok);
		TASK_CHECK(r1.output == r2.output);
		TASK_CHECK(r2.output == r3.output);

		TASK_CHECK(get32(r1.output, r1.output.size() - 4) == 0x12345678);
		TASK_CHECK(r1.output.size() > o.text.size()); // veneer size included
	}

	void caseRelocatorVeneerCycleIdempotent()
	{
		ArmElfRelocator reloc(false);
		RelocationData d{};
		d.opcodeOffset = BASE;
		d.relocationBase = BASE + 0x100;
		d.opcode = 0xEBFFFFFE;
		d.targetSymbolType = STT_FUNC;
		d.targetSymbolInfo = 1; // Thumb target forces a veneer on ARM7

		size_t actionCount = 0;
		for (int pass = 0; pass < 3; pass++)
		{
			std::vector<RelocationAction> actions;
			std::vector<std::string> errors;
			reloc.resetRelocationRun();
			TASK_CHECK(reloc.relocateOpcode(R_ARM_CALL, d, actions, errors));
			TASK_CHECK(errors.empty());

			auto plan = reloc.planVeneers();
			TASK_CHECK(plan.size() == 1);
			reloc.assignVeneerAddresses(std::vector<int64_t> { BASE + 0x200 });

			actions.clear();
			TASK_CHECK(reloc.emitVeneers(actions, errors));
			TASK_CHECK(errors.empty());
			if (pass == 0)
				actionCount = actions.size();
			else
				TASK_CHECK(actions.size() == actionCount); // no growth across passes
		}
	}

	void caseErrors()
	{
		// branch relocation against a non-function symbol
		{
			ElfObject o;
			size_t dataSym = o.addObject("data_obj");
			o.armBl(dataSym);
			ImportResult r = runImport(o.build(), ".gba");
			TASK_CHECK(!r.ok);
			TASK_CHECK(!r.errors.empty());
		}

		// ARM function at a non-word-aligned address
		{
			ElfObject o;
			size_t caller = o.addFunction("arm_caller");
			size_t misaligned = o.addFunction("half_fn");
			o.place(caller, false);
			o.armBl(misaligned);
			o.setValue(misaligned, 2); // even, but % 4 == 2
			ImportResult r = runImport(o.build(), ".gba");
			TASK_CHECK(!r.ok);
			TASK_CHECK(!r.errors.empty());
		}

		// unsupported relocation type through the real import path
		{
			ElfObject o;
			size_t callee = o.addFunction("callee");
			// the type is rejected before the target is even examined
			o.rawReloc(callee, 250, 0xE1A00000);
			ImportResult r = runImport(o.build(), ".gba");
			TASK_CHECK(!r.ok);
			TASK_CHECK(!r.errors.empty());
		}

		// 32-bit address overflow, exercised at the relocator boundary
		{
			ArmElfRelocator reloc(false);
			std::vector<RelocationAction> actions;
			std::vector<std::string> errors;
			RelocationData d{};
			d.opcodeOffset = 0x1000;
			d.relocationBase = 0x100000000;
			d.opcode = 0xEBFFFFFE;
			d.targetSymbolType = STT_FUNC;
			TASK_CHECK(!reloc.relocateOpcode(R_ARM_CALL, d, actions, errors));
			TASK_CHECK(!errors.empty());
		}
	}
}

void register_cases(std::vector<OfflineCase>& cases)
{
	cases.push_back({ "arm_to_thumb_gba", caseArmToThumbGba });
	cases.push_back({ "thumb_to_arm_gba", caseThumbToArmGba });
	cases.push_back({ "thumb_halfword_aligned_call_site", caseThumbHalfwordAlignedCallSite });
	cases.push_back({ "thumb_halfword_aligned_direct_bl", caseThumbHalfwordAlignedDirectBl });
	cases.push_back({ "conditional_arm_call", caseConditionalArmCall });
	cases.push_back({ "tail_jump_arm_to_thumb", caseTailJumpArmToThumb });
	cases.push_back({ "share_and_isolate", caseShareAndIsolate });
	cases.push_back({ "direct_encoding_unchanged", caseDirectEncodingUnchanged });
	cases.push_back({ "arm9_direct_blx_retained", caseArm9DirectBlxRetained });
	cases.push_back({ "abs32_keeps_mode_bit", caseAbs32KeepsModeBit });
	cases.push_back({ "multipass_stable", caseMultipassStable });
	cases.push_back({ "relocator_veneer_cycle_idempotent", caseRelocatorVeneerCycleIdempotent });
	cases.push_back({ "error_cases", caseErrors });
}
