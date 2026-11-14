/* offline.c -- Offline module state for the backtrace library.
   Copyright (C) 2026 Free Software Foundation, Inc.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

    (1) Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

    (2) Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

    (3) The name of the author may not be used to endorse or promote
    products derived from this software without specific prior written
    permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.  */

#include "config.h"

#include <string.h>
#include <sys/types.h>

#include "backtrace.h"
#include "internal.h"

/* The offline module table reads ELF files.  On a build for a
   different object file format there is no ELF reader, so this file
   provides nothing.  */

#if BACKTRACE_ELF_SIZE == 32 || BACKTRACE_ELF_SIZE == 64

/* An offline state describes a fixed table of ELF modules that live in
   a different address space from the process using the library.  The
   state does not unwind the stack of the tool, and does not look at
   /proc, the tool's own executable, loaded shared libraries, global
   debug directories, or external debug files.  It only reads the files
   registered with backtrace_offline_add_module.

   Each module maps the half-open runtime range [RUNTIME_LOW,
   RUNTIME_HIGH) to link time addresses by adding RUNTIME_LOW - LINK_LOW.
   A query first selects the single module that contains the runtime
   address, then translates that address into link coordinates for the
   ELF and DWARF readers, which were built with a zero base address.
   Every PC passed to a callback is converted back to runtime
   coordinates.  A query that does not fall in any module, or that falls
   in a module with no matching information, reports no information
   rather than consulting any other module or the tool process.

   Modules are registered and queried from a single thread, so no
   locking is needed.  */

struct backtrace_offline_module
{
  /* The next module in the sorted list.  */
  struct backtrace_offline_module *next;
  /* The runtime range, [runtime_low, runtime_high).  */
  uintptr_t runtime_low;
  uintptr_t runtime_high;
  /* The link address corresponding to runtime_low.  */
  uintptr_t link_low;
  /* The DWARF data for the module, or NULL if there is none.  */
  struct dwarf_data *dwarf_data;
  /* The symbol table for the module, or NULL if there is none.  */
  struct elf_syminfo_data *syminfo_data;
};

/* Return the module containing RUNTIME_PC, or NULL if there is none.
   The list is sorted by runtime_low and the ranges never overlap.  */

static struct backtrace_offline_module *
offline_find_module (struct backtrace_state *state, uintptr_t runtime_pc)
{
  struct backtrace_offline_module *module;

  for (module = state->offline_modules; module != NULL; module = module->next)
    {
      if (runtime_pc < module->runtime_low)
	return NULL;
      if (runtime_pc < module->runtime_high)
	return module;
    }

  return NULL;
}

/* Convert a runtime address in MODULE to a link time address.  The
   registration code has already verified that this cannot overflow.  */

static uintptr_t
offline_to_link (struct backtrace_offline_module *module, uintptr_t runtime)
{
  return module->link_low + (runtime - module->runtime_low);
}

/* Convert a link time address LVALUE in MODULE to a runtime address.
   Returns 1 on success, 0 if the address cannot be represented at
   runtime.  */

static int
offline_to_runtime (struct backtrace_offline_module *module, uintptr_t lvalue,
		    uintptr_t *runtime)
{
  uintptr_t offset;

  if (lvalue < module->link_low)
    return 0;

  offset = lvalue - module->link_low;
  if (offset > (uintptr_t) -1 - module->runtime_low)
    return 0;

  *runtime = module->runtime_low + offset;
  return 1;
}

/* Call a backtrace_full_callback.  FILENAME is always NULL, LINENO is
   0, and FUNCTION may be NULL.  DATA is wrapped for the MOREDATA
   layout when necessary.  */

static int
offline_call_full (struct backtrace_state *state,
		   backtrace_full_callback callback, void *data, uintptr_t pc,
		   const char *function)
{
  if (!state->moredata)
    return callback (data, pc, NULL, 0, function);
  else
    {
      struct backtrace_moredata md;

      memset (&md, 0, sizeof md);
      md.backtrace_version = BACKTRACE_MOREDATA_VERSION;
      md.backtrace_data = data;
      return callback ((void *) &md, pc, NULL, 0, function);
    }
}

/* Call a backtrace_syminfo_callback, wrapping DATA for the MOREDATA
   layout when necessary.  NAME may be NULL to report that no symbol
   was found.  */

static void
offline_call_syminfo (struct backtrace_state *state,
		      backtrace_syminfo_callback callback, void *data,
		      uintptr_t pc, const char *name, uintptr_t value,
		      uintptr_t size)
{
  if (!state->moredata)
    callback (data, pc, name, value, size);
  else
    {
      struct backtrace_moredata md;

      memset (&md, 0, sizeof md);
      md.backtrace_version = BACKTRACE_MOREDATA_VERSION;
      md.backtrace_data = data;
      callback ((void *) &md, pc, name, value, size);
    }
}

/* Find file/line information for an offline state.  This has the type
   of a fileline function.  */

static int
offline_fileline (struct backtrace_state *state, uintptr_t runtime_pc,
		  backtrace_full_callback callback,
		  backtrace_error_callback error_callback, void *data)
{
  struct backtrace_offline_module *module;
  uintptr_t link_pc;
  int found;
  int ret;

  module = offline_find_module (state, runtime_pc);
  if (module == NULL)
    return offline_call_full (state, callback, data, runtime_pc, NULL);

  /* If the module has DWARF information, look the address up in that
     module only.  */
  if (module->dwarf_data != NULL)
    {
      link_pc = offline_to_link (module, runtime_pc);
      found = 0;
      ret = backtrace_dwarf_lookup_pc (state, module->dwarf_data, link_pc,
				       runtime_pc, callback, error_callback,
				       data, &found);
      if (ret != 0)
	return ret;
      if (found)
	return 0;
    }

  /* There is no DWARF information that covers this address.  As with
     the normal ELF handling, use the symbol table of this same module
     to at least report a function name.  Never borrow information
     from another module.  */
  if (module->syminfo_data != NULL)
    {
      const char *name;
      uintptr_t value;
      size_t size;

      link_pc = offline_to_link (module, runtime_pc);
      if (backtrace_elf_syminfo_lookup (module->syminfo_data, link_pc, &name,
					&value, &size))
	/* The callback must receive the runtime PC and the symbol name,
	   with no file or line information.  */
	return offline_call_full (state, callback, data, runtime_pc, name);
    }

  return offline_call_full (state, callback, data, runtime_pc, NULL);
}

/* Find symbol information for an offline state.  This has the type of
   a syminfo function.  */

static void
offline_syminfo (struct backtrace_state *state, uintptr_t runtime_pc,
		 backtrace_syminfo_callback callback,
		 backtrace_error_callback error_callback, void *data)
{
  struct backtrace_offline_module *module;
  uintptr_t link_pc;
  const char *name;
  uintptr_t value;
  size_t size;
  uintptr_t runtime_value;

  module = offline_find_module (state, runtime_pc);
  if (module == NULL
      || module->syminfo_data == NULL)
    {
      offline_call_syminfo (state, callback, data, runtime_pc, NULL, 0, 0);
      return;
    }

  link_pc = offline_to_link (module, runtime_pc);
  if (!backtrace_elf_syminfo_lookup (module->syminfo_data, link_pc, &name,
				    &value, &size))
    {
      offline_call_syminfo (state, callback, data, runtime_pc, NULL, 0, 0);
      return;
    }

  /* The symbol value is a link time address.  Report it in runtime
     coordinates while keeping its size.  */
  if (!offline_to_runtime (module, value, &runtime_value))
    {
      error_callback (data, "offline symbol address out of range", 0);
      return;
    }

  offline_call_syminfo (state, callback, data, runtime_pc, name, runtime_value,
			size);
}

/* Create an offline state.  This is like backtrace_create_state, but
   the state does not refer to the current process at all.  Modules
   must be added with backtrace_offline_add_module.  */

struct backtrace_state *
backtrace_create_offline_state (int flags,
				backtrace_error_callback error_callback,
				void *data)
{
  struct backtrace_state *state;

  state = backtrace_create_state_common (NULL, flags, 1, error_callback,
					 data);
  if (state == NULL)
    return NULL;

  state->fileline_fn = offline_fileline;
  state->syminfo_fn = offline_syminfo;

  return state;
}

/* Add a module to an offline state.  See backtrace.h for the
   interface.  */

int
backtrace_offline_add_module (struct backtrace_state *state,
			      const char *filename,
			      uintptr_t runtime_low, uintptr_t runtime_high,
			      uintptr_t link_low,
			      backtrace_error_callback error_callback,
			      void *data)
{
  struct backtrace_offline_module *module;
  struct backtrace_offline_module *existing;
  struct backtrace_offline_module **pp;
  struct elf_offline_info info;
  uintptr_t range;
  int descriptor;

  if (state == NULL || filename == NULL || !state->offline)
    {
      error_callback (data, "backtrace_offline_add_module: invalid state", 0);
      return 0;
    }

  /* Reject an empty range.  The runtime range is half-open, so
     runtime_high must be strictly greater than runtime_low.  */
  if (runtime_high <= runtime_low)
    {
      error_callback (data, "backtrace_offline_add_module: empty module range",
		      0);
      return 0;
    }

  /* Reject any address translation that cannot be represented.  The
     largest link address we can produce is link_low + range - 1.  */
  range = runtime_high - runtime_low;
  if (link_low > (uintptr_t) -1 - (range - 1))
    {
      error_callback (data,
		      "backtrace_offline_add_module: address translation "
		      "overflows", 0);
      return 0;
    }

  /* Reject overlapping ranges.  Adjacent ranges are permitted.  */
  for (existing = state->offline_modules; existing != NULL;
       existing = existing->next)
    {
      if (runtime_low < existing->runtime_high
	  && existing->runtime_low < runtime_high)
	{
	  error_callback (data,
			  "backtrace_offline_add_module: overlapping "
			  "module range", 0);
	  return 0;
	}
    }

  descriptor = backtrace_open (filename, error_callback, data, NULL);
  if (descriptor < 0)
    return 0;

  memset (&info, 0, sizeof info);
  if (!backtrace_elf_add_offline (state, filename, descriptor, error_callback,
				  data, &info))
    return 0;

  module = ((struct backtrace_offline_module *)
	    backtrace_alloc (state, sizeof *module, error_callback, data));
  if (module == NULL)
    return 0;

  module->next = NULL;
  module->runtime_low = runtime_low;
  module->runtime_high = runtime_high;
  module->link_low = link_low;
  module->dwarf_data = info.dwarf_data;
  module->syminfo_data = info.syminfo_data;

  /* Keep the list sorted by runtime_low.  */
  pp = &state->offline_modules;
  while (*pp != NULL && (*pp)->runtime_low < runtime_low)
    pp = &(*pp)->next;
  module->next = *pp;
  *pp = module;

  return 1;
}

#else /* BACKTRACE_ELF_SIZE != 32 && != 64 */

/* Without an ELF reader there are no offline modules.  The entry
   points still exist so that programs can link, but creating a state
   always fails.  */

struct backtrace_state *
backtrace_create_offline_state (int flags ATTRIBUTE_UNUSED,
				backtrace_error_callback error_callback,
				void *data)
{
  error_callback (data, "backtrace_create_offline_state: "
		  "ELF is not supported in this build", 0);
  return NULL;
}

int
backtrace_offline_add_module (struct backtrace_state *state
			      ATTRIBUTE_UNUSED,
			      const char *filename ATTRIBUTE_UNUSED,
			      uintptr_t runtime_low ATTRIBUTE_UNUSED,
			      uintptr_t runtime_high ATTRIBUTE_UNUSED,
			      uintptr_t link_low ATTRIBUTE_UNUSED,
			      backtrace_error_callback error_callback,
			      void *data)
{
  error_callback (data, "backtrace_offline_add_module: "
		  "ELF is not supported in this build", 0);
  return 0;
}

#endif /* BACKTRACE_ELF_SIZE != 32 && != 64 */
