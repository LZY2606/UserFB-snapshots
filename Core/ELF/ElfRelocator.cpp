#include "Core/ELF/ElfRelocator.h"

#include "Archs/Architecture.h"
#include "Commands/CAssemblerLabel.h"
#include "Core/Common.h"
#include "Core/Misc.h"
#include "Core/SymbolData.h"
#include "Util/CRC.h"
#include "Util/FileSystem.h"
#include "Util/Util.h"

#include <cstring>

struct ArFileHeader
{
	char fileName[16];
	char modifactionTime[12];
	char ownerId[6];
	char groupId[6];
	char fileMode[8];
	char fileSize[10];
	char magic[2];
};

struct ArFileEntry
{
	std::string name;
	ByteArray data;
};

std::vector<ArFileEntry> loadArArchive(const fs::path& inputName)
{
	ByteArray input = ByteArray::fromFile(inputName);
	std::vector<ArFileEntry> result;

	if (input.size() < 8 || memcmp(input.data(),"!<arch>\n",8) != 0)
	{
		if (input.size() < 4 || memcmp(input.data(),"\x7F""ELF",4) != 0)
			return result;

		ArFileEntry entry;
		entry.name = inputName.filename().u8string();
		entry.data = input;
		result.push_back(entry);
		return result;
	}

	size_t pos = 8;
	while (pos < input.size())
	{
		ArFileHeader* header = (ArFileHeader*) input.data(pos);
		pos += sizeof(ArFileHeader);
		
		// get file size
		int size = 0;
		for (int i = 0; i < 10; i++)
		{
			if (header->fileSize[i] == ' ')
				break;

			size = size*10;
			size += (header->fileSize[i]-'0');
		}

		// only ELF files are actually interesting
		if (memcmp(input.data(pos),"\x7F""ELF",4) == 0)
		{
			// get file name
			char fileName[17];
			fileName[16] = 0;
			for (int i = 0; i < 16; i++)
			{
				if (header->fileName[i] == ' ')
				{
					// remove trailing slashes of file names
					if (i > 0 && fileName[i-1] == '/')
						i--;
					fileName[i] = 0;
					break;
				}

				fileName[i] = header->fileName[i];
			}
		
			ArFileEntry entry;
			entry.name = fileName;
			entry.data = input.mid(pos,size);
			result.push_back(entry);
		}

		pos += size;
		if (pos % 2)
			pos++;
	}

	return result;
}

bool ElfRelocator::init(const fs::path& inputName)
{
	relocator = Architecture::current().getElfRelocator();
	if (relocator == nullptr)
	{
		Logger::printError(Logger::Error, "Object importing not supported for this architecture");
		return false;
	}

	auto inputFiles = loadArArchive(inputName);
	if (inputFiles.size() == 0)
	{
		Logger::printError(Logger::Error, "Could not load library");
		return false;
	}

	for (ArFileEntry& entry: inputFiles)
	{
		ElfRelocatorFile file;

		ElfFile* elf = new ElfFile();
		if (!elf->load(entry.data,false))
		{
			Logger::printError(Logger::Error, "Could not load object file %s",entry.name);
			return false;
		}

		if (elf->getType() != ET_REL)
		{
			Logger::printError(Logger::Error, "Unexpected ELF type %d in object file %s",elf->getType(),entry.name);
			return false;
		}

		if (elf->getMachine() != relocator->expectedMachine())
		{
			Logger::printError(Logger::Error, "Unexpected ELF machine %d in object file %s",elf->getMachine(),entry.name);
			return false;
		}

		if (elf->getEndianness() != Architecture::current().getEndianness())
		{
			Logger::printError(Logger::Error, "Incorrect endianness in object file %s",entry.name);
			return false;
		}

		if (elf->getSegmentCount() != 0)
		{
			Logger::printError(Logger::Error, "Unexpected segment count %d in object file %s",elf->getSegmentCount(),entry.name);
			return false;
		}


		// load all relevant sections of this file
		for (size_t s = 0; s < elf->getSegmentlessSectionCount(); s++)
		{
			ElfSection* sec = elf->getSegmentlessSection(s);
			if (!(sec->getFlags() & SHF_ALLOC))
				continue;

			if (sec->getType() == SHT_PROGBITS || sec->getType() == SHT_NOBITS || sec->getType() == SHT_INIT_ARRAY)
			{
				ElfRelocatorSection sectionEntry;
				sectionEntry.section = sec;
				sectionEntry.index = s;
				sectionEntry.relSection = nullptr;
				sectionEntry.label = nullptr;

				// search relocation section
				for (size_t k = 0; k < elf->getSegmentlessSectionCount(); k++)
				{
					ElfSection* relSection = elf->getSegmentlessSection(k);
					if (relSection->getType() != SHT_REL && relSection->getType() != SHT_RELA)
						continue;
					if (relSection->getInfo() != s)
						continue;

					// got it
					sectionEntry.relSection = relSection;
					break;
				}

				// keep track of constructor sections
				if (sec->getName() == ".ctors" || sec->getName() == ".init_array")
				{
					ElfRelocatorCtor ctor;
					ctor.symbolName = Global.symbolTable.getUniqueLabelName();
					ctor.size = sec->getSize();

					sectionEntry.label = Global.symbolTable.getLabel(ctor.symbolName,-1,-1);
					sectionEntry.label->setDefined(true);

					ctors.push_back(ctor);
				}

				file.sections.push_back(sectionEntry);
			}
		}

		// init exportable symbols
		for (int i = 0; i < elf->getSymbolCount(); i++)
		{
			Elf32_Sym symbol;
			elf->getSymbol(symbol, i);

			if ((ELF32_ST_BIND(symbol.st_info) == STB_GLOBAL || ELF32_ST_BIND(symbol.st_info) == STB_WEAK) && symbol.st_shndx != 0)
			{
				ElfRelocatorSymbol symEntry;
				symEntry.type = ELF32_ST_TYPE(symbol.st_info);
				symEntry.name = elf->getStrTableString(symbol.st_name);
				symEntry.relativeAddress = symbol.st_value;
				symEntry.section = symbol.st_shndx;
				symEntry.size = symbol.st_size;
				symEntry.label = nullptr;

				file.symbols.push_back(symEntry);
			}
		}

		file.elf = elf;
		file.name = entry.name;
		files.push_back(file);
	}

	return true;
}

bool ElfRelocator::exportSymbols()
{
	bool error = false;

	for (ElfRelocatorFile& file: files)
	{
		for (ElfRelocatorSymbol& sym: file.symbols)
		{
			if (sym.label != nullptr)
				continue;

			std::string lowered = sym.name;
			std::transform(lowered.begin(), lowered.end(), lowered.begin(), ::tolower);

			sym.label = Global.symbolTable.getLabel(Identifier(lowered),-1,-1);
			if (sym.label == nullptr)
			{
				Logger::printError(Logger::Error, "Invalid label name \"%s\"",sym.name);
				error = true;
				continue;
			}

			if (sym.label->isDefined())
			{
				Logger::printError(Logger::Error, "Label \"%s\" already defined",sym.name);
				error = true;
				continue;
			}

			RelocationData data;
			data.symbolAddress = sym.relativeAddress;
			relocator->setSymbolAddress(data,sym.relativeAddress,sym.type);

			sym.relativeAddress = data.symbolAddress;
			sym.label->setInfo(data.targetSymbolInfo);
			sym.label->setIsData(sym.type == STT_OBJECT);
			sym.label->setUpdateInfo(false);

			sym.label->setValue(0);
			sym.label->setDefined(true);
			sym.label->setOriginalName(Identifier(sym.name));
		}
	}

	return !error;
}

std::unique_ptr<CAssemblerCommand> ElfRelocator::generateCtor(const Identifier& ctorName)
{
	std::unique_ptr<CAssemblerCommand> content = relocator->generateCtorStub(ctors);

	auto func = std::make_unique<CDirectiveFunction>(ctorName, ctorName);
	func->setContent(std::move(content));
	return func;
}

void ElfRelocator::loadRelocation(Elf32_Rela& rela, bool addend, ByteArray& data, int offset, Endianness endianness)
{
	rela.r_offset = data.getDoubleWord(offset + 0x00, endianness);
	rela.r_info   = data.getDoubleWord(offset + 0x04, endianness);
	if (addend)
		rela.r_addend = data.getDoubleWord(offset + 0x08, endianness);
	else
		rela.r_addend = 0;
}

bool ElfRelocator::gatherFile(ElfRelocatorFile& file, int64_t& relocationAddress)
{
	ElfFile* elf = file.elf;

	// calculate address for each section
	std::map<int64_t,int64_t> relocationOffsets;
	for (ElfRelocatorSection& entry: file.sections)
	{
		ElfSection* section = entry.section;
		size_t index = entry.index;
		int size = section->getSize();

		while (relocationAddress % section->getAlignment())
			relocationAddress++;

		if (entry.label != nullptr)
			entry.label->setValue(relocationAddress);

		relocationOffsets[index] = relocationAddress;

		ElfLoadedSection loaded;
		loaded.baseAddress = relocationAddress;
		loaded.isNoBits = section->getType() == SHT_NOBITS;
		loaded.data = loaded.isNoBits ? ByteArray() : section->getData();
		loadedSections.push_back(std::move(loaded));

		relocationAddress += size;
	}

	bool error = false;
	for (ElfRelocatorSection& entry: file.sections)
	{
		ElfSection* relSection = entry.relSection;
		if (relSection == nullptr)
			continue;

		// NOBITS sections occupy no file space, so relocations against them
		// cannot be applied
		if (entry.section->getType() == SHT_NOBITS)
			continue;

		size_t index = entry.index;
		int64_t sectionBase = relocationOffsets[index];
		bool isRela = relSection->getType() == SHT_RELA;
		int structSize = isRela ? sizeof(Elf32_Rela) : sizeof(Elf32_Rel);

		for (unsigned int relOffset = 0; relOffset < relSection->getSize(); relOffset += structSize)
		{
			Elf32_Rela rela;
			loadRelocation(rela, isRela, relSection->getData(), relOffset, elf->getEndianness());
			int pos = rela.r_offset;

			if (relocator->isDummyRelocationType(rela.getType()))
				continue;

			int symNum = rela.getSymbolNum();
			if (symNum <= 0)
			{
				Logger::queueError(Logger::Warning, "Invalid symbol num %06X",symNum);
				error = true;
				continue;
			}

			Elf32_Sym sym;
			elf->getSymbol(sym, symNum);

			RelocationData relData;
			relData.opcode = entry.section->getData().getDoubleWord(pos, elf->getEndianness());
			relData.opcodeOffset = pos+sectionBase;
			relData.addend = rela.r_addend;
			relocator->setSymbolAddress(relData,sym.st_value,sym.st_info & 0xF);

			// externs?
			if (sym.st_shndx == 0)
			{
				if (sym.st_name == 0)
				{
					Logger::queueError(Logger::Error, "Symbol without a name");
					error = true;
					continue;
				}

				std::string symName = toLowercase(elf->getStrTableString(sym.st_name));

				std::shared_ptr<Label> label = Global.symbolTable.getLabel(Identifier(symName),-1,-1);
				if (label == nullptr)
				{
					Logger::queueError(Logger::Error, "Invalid external symbol %s",symName);
					error = true;
					continue;
				}
				if (!label->isDefined())
				{
					Logger::queueError(Logger::Error, "Undefined external symbol %s in file %s",symName,file.name);
					error = true;
					continue;
				}

				relData.relocationBase = (unsigned int) label->getValue();
				relData.targetSymbolType = label->isData() ? STT_OBJECT : STT_FUNC;
				relData.targetSymbolInfo = label->getInfo();
			} else {
				relData.relocationBase = relocationOffsets[sym.st_shndx]+relData.symbolAddress;
			}

			std::vector<std::string> errors;
			if (!relocator->relocateOpcode(rela.getType(), relData, plannedActions, errors))
			{
				for (const std::string& error : errors)
				{
					Logger::queueError(Logger::Error, error);
				}
				error = true;
				continue;
			}
		}

		// finish any dangling relocations (e.g. MIPS HI16 entries)
		std::vector<std::string> errors;
		if (!relocator->finish(plannedActions, errors))
		{
			for (const std::string& error : errors)
			{
				Logger::queueError(Logger::Error, error);
			}
			error = true;
		}
	}

	// now update symbols
	for (ElfRelocatorSymbol& sym: file.symbols)
	{
		int64_t oldAddress = sym.relocatedAddress;

		switch (sym.section)
		{
		case SHN_ABS:		// address does not change
			sym.relocatedAddress = sym.relativeAddress;
			break;
		case SHN_COMMON:	// needs to be allocated. relativeAddress gives alignment constraint
			while (relocationAddress % sym.relativeAddress)
				relocationAddress++;

			sym.relocatedAddress = relocationAddress;
			relocationAddress += sym.size;
			break;
		default:			// normal relocated symbol
			sym.relocatedAddress = sym.relativeAddress+relocationOffsets[sym.section];
			break;
		}

		if (sym.label != nullptr)
			sym.label->setValue(sym.relocatedAddress);

		if (oldAddress != sym.relocatedAddress)
			dataChanged = true;
	}

	return !error;
}

bool ElfRelocator::relocate(int64_t& memoryAddress)
{
	int oldCrc = getCrc32(outputData.data(),outputData.size());
	outputData.clear();
	dataChanged = false;
	plannedActions.clear();
	loadedSections.clear();
	relocator->resetRelocationRun();

	bool error = false;
	int64_t start = memoryAddress;

	for (ElfRelocatorFile& file: files)
	{
		if (!gatherFile(file,memoryAddress))
			error = true;
	}

	// lay out the interworking veneers at the end of the import code area
	plannedVeneers = relocator->planVeneers();
	veneerAddresses.clear();
	bool veneerLayoutError = false;
	for (const RelocationVeneerReservation& reservation: plannedVeneers)
	{
		while (memoryAddress % reservation.alignment)
			memoryAddress++;

		if (memoryAddress > 0xFFFFFFFF)
		{
			Logger::queueError(Logger::Error, "Interworking veneer overflows the 32 bit address space");
			error = true;
			veneerLayoutError = true;
			break;
		}

		veneerAddresses.push_back(memoryAddress);
		memoryAddress += relocator->getVeneerSize(reservation.veneer, memoryAddress);
	}
	relocator->assignVeneerAddresses(veneerAddresses);

	// the relocator can now emit the final branch patches and veneer contents.
	// skip it when the layout itself failed, otherwise the relocator could index
	// past the partially assigned veneer address list
	if (!veneerLayoutError)
	{
		std::vector<std::string> veneerErrors;
		if (!relocator->emitVeneers(plannedActions, veneerErrors))
		{
			for (const std::string& error : veneerErrors)
			{
				Logger::queueError(Logger::Error, error);
			}
			error = true;
		}
	}

	size_t dataStart = outputData.size();
	outputData.reserveBytes((size_t)(memoryAddress-start));

	// apply the relocation actions. Ordinary patches go back into the section
	// they belong to, veneer contents go into the generated veneer area.
	for (const RelocationAction& action: plannedActions)
	{
		if (action.kind == RelocationActionKind::WriteVeneer)
		{
			if (!action.veneerBytes.empty())
				memcpy(outputData.data(dataStart+(size_t)(action.offset-start)),action.veneerBytes.data(),action.veneerBytes.size());
			continue;
		}

		bool applied = false;
		for (ElfLoadedSection& loaded: loadedSections)
		{
			if (loaded.isNoBits)
				continue;

			int64_t sectionOffset = action.offset - loaded.baseAddress;
			if (sectionOffset < 0 || (uint64_t) sectionOffset+4 > loaded.data.size())
				continue;

			loaded.data.replaceDoubleWord((size_t) sectionOffset, action.newValue,
				Architecture::current().getEndianness());
			applied = true;
			break;
		}

		if (!applied)
		{
			Logger::queueError(Logger::Error, "Relocation action does not belong to any section");
			error = true;
		}
	}

	// copy the (possibly patched) sections into the output
	for (const ElfLoadedSection& loaded: loadedSections)
	{
		if (loaded.isNoBits)
			continue;	// reserveBytes initialized the data to 0 already

		size_t arrayStart = (size_t) (dataStart+loaded.baseAddress-start);
		memcpy(outputData.data(arrayStart),loaded.data.data(),loaded.data.size());
	}

	int newCrc = getCrc32(outputData.data(),outputData.size());
	if (oldCrc != newCrc)
		dataChanged = true;

	memoryAddress -= start;
	return !error;
}

void ElfRelocator::writeSymbols(SymbolData& symData) const
{
	for (const ElfRelocatorFile& file: files)
	{
		for (const ElfRelocatorSymbol& sym: file.symbols)
		{
			symData.addLabel(sym.relocatedAddress, sym.name);

			switch (sym.type)
			{
			case STT_OBJECT:
				symData.addData(sym.relocatedAddress,sym.size,SymbolData::Data8);
				break;
			case STT_FUNC:
				symData.startFunction(sym.relocatedAddress);
				symData.endFunction(sym.relocatedAddress+sym.size);
				break;
			}
		}
	}
}

std::unique_ptr<CAssemblerCommand> IElfRelocator::generateCtorStub(std::vector<ElfRelocatorCtor> &ctors)
{
	return nullptr;
}
