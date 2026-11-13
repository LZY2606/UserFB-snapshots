The fixture is an independent normal-input DWARF4 memory loader. Its .debug_info,
.debug_abbrev, .debug_line and .debug_ranges are encoded directly in C++.
Two CUs have different file tables at file index 1. Concrete inline instances
share a ref_addr origin, while call and declaration locations differ. Address
ranges use initial CU base, base-address selection, holes and half-open bounds.

Run `bash task_support/build.sh` then `.task-build/normal_baseline` from the
source root. This exercises the original parser and original find_pc traversal;
it is not an implementation of the requested multi-candidate stack API.
The generated elf/to_string.cc and dwarf/to_string.cc are shipped source.
The build does not execute the upstream enum-print.py or upstream Makefiles.
