/*
The MIT License (MIT)

Copyright (c) 2013 Andrew Ruef

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

// Private definitions shared by the parser translation units. This
// header is not installed and not part of the public API.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <pe-parse/debug_records.h>
#include <pe-parse/nt-headers.h>
#include <pe-parse/parse.h>

namespace peparse {

struct section {
  std::string sectionName;
  std::uint64_t sectionBase;
  bounded_buffer *sectionData;
  image_section_header sec;
  // zero-based index in the original section table, kept because the
  // section vector is sorted by raw data pointer after parsing
  std::uint32_t sectionIndex = 0;
};

struct importent {
  VA addr;
  std::string symbolName;
  std::string moduleName;
};

struct exportent {
  VA addr;
  std::uint16_t ordinal;
  std::string symbolName;
  std::string moduleName;
  std::string forwardName;
};

struct reloc {
  VA shiftedAddr;
  reloc_type type;
};

struct debugent {
  std::uint32_t type;
  bounded_buffer *data;
};

#define SYMBOL_NAME_OFFSET(sn) (static_cast<std::uint32_t>(sn.data >> 32))
#define SYMBOL_TYPE_HI(x) (x.type >> 8)

union symbol_name {
  std::uint8_t shortName[NT_SHORT_NAME_LEN];
  std::uint32_t zeroes;
  std::uint64_t data;
};

struct aux_symbol_f1 {
  std::uint32_t tagIndex;
  std::uint32_t totalSize;
  std::uint32_t pointerToLineNumber;
  std::uint32_t pointerToNextFunction;
};

struct aux_symbol_f2 {
  std::uint16_t lineNumber;
  std::uint32_t pointerToNextFunction;
};

struct aux_symbol_f3 {
  std::uint32_t tagIndex;
  std::uint32_t characteristics;
};

struct aux_symbol_f4 {
  std::uint8_t filename[SYMTAB_RECORD_LEN];
  std::string strFilename;
};

struct aux_symbol_f5 {
  std::uint32_t length;
  std::uint16_t numberOfRelocations;
  std::uint16_t numberOfLineNumbers;
  std::uint32_t checkSum;
  std::uint16_t number;
  std::uint8_t selection;
};

struct symbol {
  std::string strName;
  symbol_name name;
  std::uint32_t value;
  std::int16_t sectionNumber;
  std::uint16_t type;
  std::uint8_t storageClass;
  std::uint8_t numberOfAuxSymbols;
  std::vector<aux_symbol_f1> aux_symbols_f1;
  std::vector<aux_symbol_f2> aux_symbols_f2;
  std::vector<aux_symbol_f3> aux_symbols_f3;
  std::vector<aux_symbol_f4> aux_symbols_f4;
  std::vector<aux_symbol_f5> aux_symbols_f5;
};

struct parsed_pe_internal {
  std::vector<section> secs;
  std::vector<resource> rsrcs;
  std::vector<importent> imports;
  std::vector<reloc> relocs;
  std::vector<exportent> exports;
  std::vector<symbol> symbols;
  std::vector<debugent> debugdirs;
  debug_records debugRecords;
};

// Decode the debug directory of a parsed PE into out, keeping every
// record (including zero-length markers and duplicates) and locating
// each payload through PointerToRawData and AddressOfRawData
// independently. Returns false and describes the failing entry in out
// when the directory or a payload cannot be read completely.
bool ComputeDebugRecords(parsed_pe *pe, debug_records &out);

} // namespace peparse
