#pragma once
using btf_case_fn = void (*)();
void btf_add_case(const char* group, btf_case_fn function);
void btf_expect(bool condition, const char* explanation);
void register_btf_compaction_cases();
