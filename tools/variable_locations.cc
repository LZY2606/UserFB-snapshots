// Copyright 2026 Google LLC
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google LLC nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// variable_locations: answer variable storage queries from optimized ELF
// debug info and emit a TSV report (see support/report-format.md).
//
// Usage: variable_locations queries.tsv
//
// Each input line is "sample<TAB>function<TAB>variable<TAB>pc" with no
// header. Queries are processed in input order and may repeat; results are
// recomputed for every line and never cached.

#include <stdint.h>
#include <stdio.h>

#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "common/dwarf/variable_location.h"

namespace {

using google_breakpad::VariableDiagnostic;
using google_breakpad::VariableLocationAnalyzer;
using google_breakpad::VariableLocationResult;
using google_breakpad::VariableLocationStatus;
using google_breakpad::VariableScopeRange;

struct Query {
  std::string sample;
  std::string function;
  std::string variable;
  uint64_t pc;
  std::string pc_text;
};

std::string Hex(uint64_t value) {
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "0x%llx",
           static_cast<unsigned long long>(value));
  return buffer;
}

std::string LowerHex(const std::vector<uint8_t>& bytes) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (uint8_t byte : bytes) {
    out.push_back(kDigits[byte >> 4]);
    out.push_back(kDigits[byte & 0x0f]);
  }
  return out;
}

std::string StatusText(VariableLocationStatus status) {
  switch (status) {
    case VariableLocationStatus::kDescribed:
      return "described";
    case VariableLocationStatus::kUnavailable:
      return "unavailable";
    case VariableLocationStatus::kOutOfScope:
      return "out_of_scope";
    case VariableLocationStatus::kNotFound:
      return "not_found";
  }
  return "not_found";
}

std::string ScopesText(const std::vector<VariableScopeRange>& scopes) {
  std::string out;
  for (size_t i = 0; i < scopes.size(); ++i) {
    if (i)
      out += ";";
    out += Hex(scopes[i].die_offset) + "@" + Hex(scopes[i].begin) + ".." +
           Hex(scopes[i].end);
  }
  return out;
}

std::string AttributeText(const VariableLocationResult& result) {
  if (!result.has_location_attribute)
    return "-";
  std::string value = result.location_is_buffer ? "-"
                                                : Hex(result.location_value);
  return Hex(static_cast<uint64_t>(result.location_form)) + ":" + value;
}

bool ParseQueryLine(const std::string& line, Query* query,
                    std::string* error) {
  std::vector<std::string> fields;
  std::string field;
  std::istringstream stream(line);
  while (std::getline(stream, field, '\t'))
    fields.push_back(field);
  if (fields.size() != 4) {
    *error = "expected four tab-separated fields";
    return false;
  }
  if (fields[3].size() < 3 || fields[3].compare(0, 2, "0x") != 0) {
    *error = "pc must be a hexadecimal address with 0x prefix";
    return false;
  }
  size_t consumed = 0;
  uint64_t pc = 0;
  try {
    pc = std::stoull(fields[3].substr(2), &consumed, 16);
  } catch (const std::exception&) {
    *error = "cannot parse pc";
    return false;
  }
  if (consumed != fields[3].size() - 2) {
    *error = "cannot parse pc";
    return false;
  }
  query->sample = fields[0];
  query->function = fields[1];
  query->variable = fields[2];
  query->pc = pc;
  query->pc_text = fields[3];
  return true;
}

void EmitRow(size_t query_number, const Query& query,
             const VariableLocationResult& result) {
  const bool not_found =
      result.status == VariableLocationStatus::kNotFound;
  const std::string missing = "-";

  std::string description =
      result.status == VariableLocationStatus::kDescribed ? result.description
                                                          : missing;
  std::string attribute = not_found ? missing : AttributeText(result);

  std::string entry = missing;
  std::string expression = missing;
  std::string bytes = missing;
  if (!not_found && result.has_entry) {
    entry = result.entry_section + "@" + Hex(result.entry_offset);
  }
  if (!not_found && result.has_expression) {
    expression = result.expression_section + "@" + Hex(result.expression_offset);
  }
  if (result.status == VariableLocationStatus::kDescribed) {
    bytes = LowerHex(result.expression_bytes);
  }

  std::string identity = not_found ? missing : Hex(result.cu_offset);
  std::string function_die = not_found ? missing : Hex(result.function_die);
  std::string variable_die = not_found ? missing : Hex(result.variable_die);
  std::string scopes = not_found ? missing : ScopesText(result.scopes);

  std::cout << query_number << '\t' << query.sample << '\t' << query.pc_text
            << '\t' << identity << '\t' << function_die << '\t' << variable_die
            << '\t' << scopes << '\t' << StatusText(result.status) << '\t'
            << description << '\t' << attribute << '\t' << entry << '\t'
            << expression << '\t' << bytes << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: variable_locations queries.tsv\n";
    return 2;
  }

  std::ifstream input(argv[1]);
  if (!input) {
    std::cerr << "cannot read query file: " << argv[1] << "\n";
    return 2;
  }

  std::map<std::string, std::unique_ptr<VariableLocationAnalyzer>> analyzers;

  std::cout << "query\tsample\tpc\tcu\tfunction_die\tvariable_die\tscopes"
               "\tstatus\tdescription\tattribute\tentry\texpression\tbytes\n";

  std::string line;
  size_t query_number = 0;
  while (std::getline(input, line)) {
    if (line.empty())
      continue;
    ++query_number;
    Query query;
    std::string error;
    if (!ParseQueryLine(line, &query, &error)) {
      std::cerr << "query " << query_number << ": " << error << "\n";
      return 1;
    }

    auto analyzer = analyzers.find(query.sample);
    if (analyzer == analyzers.end()) {
      std::unique_ptr<VariableLocationAnalyzer> fresh(
          new VariableLocationAnalyzer);
      VariableDiagnostic diagnostic;
      if (!fresh->Open(query.sample, &diagnostic)) {
        std::cerr << diagnostic.section << "@" << Hex(diagnostic.offset) << ": "
                  << diagnostic.message << "\n";
        return 1;
      }
      analyzer = analyzers.emplace(query.sample, std::move(fresh)).first;
    }

    std::vector<VariableLocationResult> results;
    VariableDiagnostic diagnostic;
    if (!analyzer->second->Query(query.function, query.variable, query.pc,
                                 &results, &diagnostic)) {
      std::cerr << diagnostic.section << "@" << Hex(diagnostic.offset) << ": "
                << diagnostic.message << "\n";
      return 1;
    }
    for (const VariableLocationResult& result : results)
      EmitRow(query_number, query, result);
  }

  return 0;
}
