The two specimens are self-authored ELF64 little-endian EM_BPF ET_REL files.
They are input data, never programs to load or execute. Run the C++ writer to
produce them. The raw-record reader lists ELF records and all eight-byte slots;
it does not reconstruct instruction boundaries, calls, dependencies or paths.

Format references pinned to Linux v6.12:
- https://github.com/torvalds/linux/blob/v6.12/Documentation/bpf/llvm_reloc.rst
- https://github.com/torvalds/linux/blob/v6.12/Documentation/bpf/standardization/instruction-set.rst

For REL call records the encoded immediate carries the implicit addend in
instruction units, unlike a fully resolved local call. Map relocations use
the low immediate word; ordinary unrelocated wide constants remain constants.
The API requested by the user is a static provenance view, not a verifier or
a loader. Example output should distinguish object symbol identity from names.

New support code and specimen data are provided under the repository's MIT
license. There are no expected graphs or reference analyzer implementations here.
