/*
    libpe - the PE library

    Copyright (C) 2010 - 2017 libpe authors

    This file is part of libpe.

    libpe is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    libpe is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with libpe.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "libpe/exports.h"

#include "libpe/pe.h"
#include <stdlib.h>
#include <string.h>

// Returns the length (excluding the NUL terminator) of the string starting
// at `ptr`, requiring the NUL to be found within `max_len` bytes and inside
// the mapped file. Returns -1 if `ptr` is outside the mapping or no NUL is
// present in the allowed range. The input bytes are only read, never written.
static int64_t pe_exports_strnlen(const pe_ctx_t *ctx, const void *ptr, uint64_t max_len) {
	const uintptr_t start = (uintptr_t)ptr;
	const uintptr_t map_start = (uintptr_t)ctx->map_addr;
	const uintptr_t map_end = (uintptr_t)ctx->map_end;
	if (start < map_start || start > map_end)
		return -1;
	const uint64_t available = (uint64_t)(map_end - start);
	if (max_len > available)
		max_len = available;
	const void *nul = memchr(ptr, '\0', (size_t)max_len);
	if (nul == NULL)
		return -1;
	return (int64_t)((const char *)nul - (const char *)ptr);
}

// Duplicates the NUL-terminated string at `ptr`, which must be terminated
// within `max_len` bytes (see pe_exports_strnlen). Returns NULL if the
// string is not terminated in range or the allocation fails.
static char *pe_exports_strdup(const pe_ctx_t *ctx, const void *ptr, uint64_t max_len) {
	const int64_t len = pe_exports_strnlen(ctx, ptr, max_len);
	if (len < 0)
		return NULL;
	char *copy = malloc((size_t)len + 1);
	if (copy == NULL)
		return NULL;
	memcpy(copy, ptr, (size_t)len + 1);
	return copy;
}

static void pe_exports_free_functions(pe_exported_function_t *functions, uint32_t count) {
	if (functions == NULL)
		return;
	for (uint32_t i = 0; i < count; i++) {
		free(functions[i].name);
		free(functions[i].fwd_name);
	}
	free(functions);
}

pe_exports_t *pe_exports(pe_ctx_t *ctx) {
	if (ctx->cached_data.exports != NULL)
		return ctx->cached_data.exports;

	pe_exports_t *exports = ctx->cached_data.exports = calloc(1, sizeof(pe_exports_t));
	if (exports == NULL) {
		// TODO(jweyrich): Should we report an error? If yes, we need a redesign.
		return NULL;
	}

	exports->err = LIBPE_E_OK;

	const IMAGE_DATA_DIRECTORY *dir = pe_directory_by_entry(ctx, IMAGE_DIRECTORY_ENTRY_EXPORT);
	if (dir == NULL) {
		return exports;
	}

	const uint64_t va = dir->VirtualAddress;
	if (va == 0) {
		// NOTE: This file has no exported symbols.
		return exports;
	}

	// Half-open RVA range [va, va + Size) of the export directory. Both
	// operands are 32-bit, so the sum cannot overflow a uint64_t.
	const uint64_t dir_end_rva = va + dir->Size;

	uint64_t ofs = pe_rva2ofs(ctx, va);
	const IMAGE_EXPORT_DIRECTORY *exp = LIBPE_PTR_ADD(ctx->map_addr, ofs);
	if (!pe_can_read(ctx, exp, sizeof(IMAGE_EXPORT_DIRECTORY))) {
		exports->err = LIBPE_E_EXPORTS_CANT_READ_DIR;
		return exports;
	}

	//
	// The format of IMAGE_EXPORT_DIRECTORY can be seen in http://i.msdn.microsoft.com/dynimg/IC60608.gif
	//

	// We want to use `NumberOfFunctions` for looping as it's the total number of functions/symbols
	// exported by the module. On the other hand, `NumberOfNames` is the number of
	// functions/symbols exported by name only.

	const uint32_t functions_count = exp->NumberOfFunctions;
	const uint32_t names_count = exp->NumberOfNames;

	// Validate the full extent of every table whose size comes from the file
	// before allocating memory or dereferencing anything based on those
	// counts. The 64-bit products cannot overflow.
	const uint64_t offset_to_AddressOfFunctions = pe_rva2ofs(ctx, exp->AddressOfFunctions);
	const uint32_t *addr_of_functions = LIBPE_PTR_ADD(ctx->map_addr, offset_to_AddressOfFunctions);
	if (functions_count > 0 &&
		!pe_can_read(ctx, addr_of_functions, (uint64_t)functions_count * sizeof(uint32_t))) {
		exports->err = LIBPE_E_EXPORTS_CANT_READ_RVA;
		return exports;
	}

	const uint64_t offset_to_AddressOfNames = pe_rva2ofs(ctx, exp->AddressOfNames);
	const uint32_t *addr_of_names = LIBPE_PTR_ADD(ctx->map_addr, offset_to_AddressOfNames);
	if (names_count > 0 &&
		!pe_can_read(ctx, addr_of_names, (uint64_t)names_count * sizeof(uint32_t))) {
		exports->err = LIBPE_E_EXPORTS_CANT_READ_RVA;
		return exports;
	}

	const uint64_t offset_to_AddressOfNameOrdinals = pe_rva2ofs(ctx, exp->AddressOfNameOrdinals);
	const uint16_t *addr_of_name_ordinals = LIBPE_PTR_ADD(ctx->map_addr, offset_to_AddressOfNameOrdinals);
	if (names_count > 0 &&
		!pe_can_read(ctx, addr_of_name_ordinals, (uint64_t)names_count * sizeof(uint16_t))) {
		exports->err = LIBPE_E_EXPORTS_CANT_READ_RVA;
		return exports;
	}

	// The DLL name must be NUL-terminated before the end of the mapping.
	const uint64_t ofs_dll_name = pe_rva2ofs(ctx, exp->Name);
	const char *dll_name_ptr = LIBPE_PTR_ADD(ctx->map_addr, ofs_dll_name);
	char *dll_name = pe_exports_strdup(ctx, dll_name_ptr, UINT64_MAX);
	if (dll_name == NULL) {
		exports->err = LIBPE_E_EXPORTS_CANT_READ_RVA;
		return exports;
	}

	// The table extent checks above bound `functions_count` by the mapped
	// size, so this allocation is proportional to the actual input.
	pe_exported_function_t *functions = NULL;
	if (functions_count > 0) {
		functions = calloc(functions_count, sizeof(pe_exported_function_t));
		if (functions == NULL) {
			free(dll_name);
			exports->err = LIBPE_E_ALLOCATION_FAILURE;
			return exports;
		}
	}

	const uint32_t ordinal_base = exp->Base;

	//
	// Functions
	//

	// Functions table is indexed by unbiased ordinal number
	for (uint32_t i = 0; i < functions_count; i++) {
		if (ordinal_base > UINT32_MAX - i) {
			// `Base + i` would overflow the 32-bit ordinal.
			exports->err = LIBPE_E_EXPORTS_CANT_READ_RVA;
			goto failure;
		}
		functions[i].ordinal = ordinal_base + i;

		// Check whether the exported function is forwarded.
		// It's forwarded if its RVA is inside the half-open export
		// directory range [va, va + Size); an address exactly at the
		// end of the range is not a forwarder.
		const uint32_t entry_va = addr_of_functions[i];
		functions[i].address = entry_va;
		if (entry_va >= va && entry_va < dir_end_rva) {
			// When a symbol is forwarded, its RVA points to a string containing
			// the name of the DLL and symbol to which it is forwarded. The
			// string's NUL terminator must lie inside the directory range.
			const uint64_t fw_ofs = pe_rva2ofs(ctx, entry_va);
			const char *fw_ptr = LIBPE_PTR_ADD(ctx->map_addr, fw_ofs);
			functions[i].fwd_name = pe_exports_strdup(ctx, fw_ptr, dir_end_rva - entry_va);
			if (functions[i].fwd_name == NULL) {
				exports->err = LIBPE_E_EXPORTS_CANT_READ_RVA;
				goto failure;
			}
		}
	}

	//
	// Names
	//

	// Names table is indexed by hint (name index) number
	for (uint32_t i = 0; i < names_count; i++) {
		// In this NameOrdinals table is stored unbiased ordinal number
		const uint16_t ordinal = addr_of_name_ordinals[i];
		if (ordinal >= functions_count) {
			exports->err = LIBPE_E_EXPORTS_CANT_READ_RVA;
			goto failure;
		}

		const uint32_t entry_name_rva = addr_of_names[i];
		const uint64_t entry_name_ofs = pe_rva2ofs(ctx, entry_name_rva);
		const char *entry_name = LIBPE_PTR_ADD(ctx->map_addr, entry_name_ofs);

		// The name must be NUL-terminated before the end of the mapping.
		char *name_copy = pe_exports_strdup(ctx, entry_name, UINT64_MAX);
		if (name_copy == NULL) {
			exports->err = LIBPE_E_EXPORTS_CANT_READ_RVA;
			goto failure;
		}

		// If several names reference the same slot, the last one wins.
		free(functions[ordinal].name);
		functions[ordinal].name = name_copy;
		functions[ordinal].hint = i;
	}

	// Slots without a name keep an empty name.
	for (uint32_t i = 0; i < functions_count; i++) {
		if (functions[i].name == NULL) {
			functions[i].name = calloc(1, 1);
			if (functions[i].name == NULL) {
				exports->err = LIBPE_E_ALLOCATION_FAILURE;
				goto failure;
			}
		}
	}

	exports->name = dll_name;
	exports->functions = functions;
	exports->functions_count = functions_count;
	return exports;

failure:
	// Converge to an empty error result: no partial data may be mistaken
	// for a successful parse.
	pe_exports_free_functions(functions, functions_count);
	free(dll_name);
	return exports;
}

void pe_exports_dealloc(pe_exports_t *obj) {
	if (obj == NULL)
		return;

	pe_exports_free_functions(obj->functions, obj->functions_count);
	free(obj->name);
	free(obj);
}
