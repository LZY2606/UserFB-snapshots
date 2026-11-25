/* internal.h -- Internal header file for stack backtrace library.
   Copyright (C) 2012-2026 Free Software Foundation, Inc.
   Written by Ian Lance Taylor, Google.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

    (1) Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

    (2) Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in
    the documentation and/or other materials provided with the
    distribution.

    (3) The name of the author may not be used to
    endorse or promote products derived from this software without
    specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.  */

#ifndef BACKTRACE_INTERNAL_H
#define BACKTRACE_INTERNAL_H

/* We assume that <sys/types.h> and "backtrace.h" have already been
   included.  */

#ifndef GCC_VERSION
# define GCC_VERSION (__GNUC__ * 1000 + __GNUC_MINOR__)
#endif

#if (GCC_VERSION < 2007)
# define __attribute__(x)
#endif

#ifndef ATTRIBUTE_UNUSED
# define ATTRIBUTE_UNUSED __attribute__ ((__unused__))
#endif

#ifndef ATTRIBUTE_MALLOC
# if (GCC_VERSION >= 2096)
#  define ATTRIBUTE_MALLOC __attribute__ ((__malloc__))
# else
#  define ATTRIBUTE_MALLOC
# endif
#endif

#ifdef __has_attribute
# if __has_attribute(fallthrough)
#  define ATTRIBUTE_FALLTHROUGH __attribute__ ((fallthrough))
# endif
#endif
#ifndef ATTRIBUTE_FALLTHROUGH
# if (GCC_VERSION >= 7000)
#  define ATTRIBUTE_FALLTHROUGH __attribute__ ((__fallthrough__))
# else
#  define ATTRIBUTE_FALLTHROUGH
# endif
#endif

#ifndef HAVE_SYNC_FUNCTIONS

/* Define out the sync functions.  These should never be called if
   they are not available.  */

#define __sync_bool_compare_and_swap(A, B, C) (abort(), 1)
#define __sync_lock_test_and_set(A, B) (abort(), 0)
#define __sync_lock_release(A) abort()

#endif /* !defined (HAVE_SYNC_FUNCTIONS) */

#ifdef HAVE_ATOMIC_FUNCTIONS

/* We have the atomic builtin functions.  */

#define backtrace_atomic_load_pointer(p) \
    __atomic_load_n ((p), __ATOMIC_ACQUIRE)
#define backtrace_atomic_load_int(p) \
    __atomic_load_n ((p), __ATOMIC_ACQUIRE)
#define backtrace_atomic_store_pointer(p, v) \
    __atomic_store_n ((p), (v), __ATOMIC_RELEASE)
#define backtrace_atomic_store_size_t(p, v) \
    __atomic_store_n ((p), (v), __ATOMIC_RELEASE)
#define backtrace_atomic_store_int(p, v) \
    __atomic_store_n ((p), (v), __ATOMIC_RELEASE)

#else /* !defined (HAVE_ATOMIC_FUNCTIONS) */
#ifdef HAVE_SYNC_FUNCTIONS

/* We have the sync functions but not the atomic functions.  Define
   the atomic ones in terms of the sync ones.  */

extern void *backtrace_atomic_load_pointer (void *);
extern int backtrace_atomic_load_int (int *);
extern void backtrace_atomic_store_pointer (void *, void *);
extern void backtrace_atomic_store_size_t (size_t *, size_t);
extern void backtrace_atomic_store_int (int *, int);

#else /* !defined (HAVE_SYNC_FUNCTIONS) */

/* We have neither the sync nor the atomic functions.  These will
   never be called.  */

#define backtrace_atomic_load_pointer(p) (abort(), (void *) NULL)
#define backtrace_atomic_load_int(p) (abort(), 0)
#define backtrace_atomic_store_pointer(p, v) abort()
#define backtrace_atomic_store_size_t(p, v) abort()
#define backtrace_atomic_store_int(p, v) abort()

#endif /* !defined (HAVE_SYNC_FUNCTIONS) */
#endif /* !defined (HAVE_ATOMIC_FUNCTIONS) */

/* The type of the function that collects file/line information.  This
   is like backtrace_pcinfo.  */

typedef int (*fileline) (struct backtrace_state *state, uintptr_t pc,
			 backtrace_full_callback callback,
			 backtrace_error_callback error_callback, void *data);

/* The type of the function that collects symbol information.  This is
   like backtrace_syminfo.  */

typedef void (*syminfo) (struct backtrace_state *state, uintptr_t pc,
			 backtrace_syminfo_callback callback,
			 backtrace_error_callback error_callback, void *data);

/* What the backtrace state pointer points to.  */

struct backtrace_state
{
  /* The name of the executable.  */
  const char *filename;
  /* Non-zero if threaded.  */
  int threaded;
  /* Non-zero if passing additional data.  */
  int moredata;
  /* The master lock for fileline_fn, fileline_data, syminfo_fn,
     syminfo_data, fileline_initialization_failed and everything the
     data pointers point to.  */
  void *lock;
  /* The function that returns file/line information.  */
  fileline fileline_fn;
  /* The data to pass to FILELINE_FN.  */
  void *fileline_data;
  /* The function that returns symbol information.  */
  syminfo syminfo_fn;
  /* The data to pass to SYMINFO_FN.  */
  void *syminfo_data;
  /* Whether initializing the file/line information failed.  */
  int fileline_initialization_failed;
  /* The lock for the freelist.  */
  int lock_alloc;
  /* The freelist when using mmap.  */
  struct backtrace_freelist_struct *freelist;
};

/* Open a file for reading.  Returns -1 on error.  If DOES_NOT_EXIST
   is not NULL, *DOES_NOT_EXIST will be set to 0 normally and set to 1
   if the file does not exist.  If the file does not exist and
   DOES_NOT_EXIST is not NULL, the function will return -1 and will
   not call ERROR_CALLBACK.  On other errors, or if DOES_NOT_EXIST is
   NULL, the function will call ERROR_CALLBACK before returning.  */
extern int backtrace_open (const char *filename,
			   backtrace_error_callback error_callback,
			   void *data,
			   int *does_not_exist);

/* A view of the contents of a file.  This supports mmap when
   available.  A view will remain in memory even after backtrace_close
   is called on the file descriptor from which the view was
   obtained.  */

struct backtrace_view
{
  /* The data that the caller requested.  */
  const void *data;
  /* The base of the view.  */
  void *base;
  /* The total length of the view.  */
  size_t len;
};

/* Create a view of SIZE bytes from DESCRIPTOR at OFFSET.  Store the
   result in *VIEW.  Returns 1 on success, 0 on error.  */
extern int backtrace_get_view (struct backtrace_state *state, int descriptor,
			       off_t offset, uint64_t size,
			       backtrace_error_callback error_callback,
			       void *data, struct backtrace_view *view);

/* Release a view created by backtrace_get_view.  */
extern void backtrace_release_view (struct backtrace_state *state,
				    struct backtrace_view *view,
				    backtrace_error_callback error_callback,
				    void *data);

/* Close a file opened by backtrace_open.  Returns 1 on success, 0 on
   error.  */

extern int backtrace_close (int descriptor,
			    backtrace_error_callback error_callback,
			    void *data);

/* Sort without using memory.  */

extern void backtrace_qsort (void *base, size_t count, size_t size,
			     int (*compar) (const void *, const void *));

/* Allocate memory.  This is like malloc.  If ERROR_CALLBACK is NULL,
   this does not report an error, it just returns NULL.  */

extern void *backtrace_alloc (struct backtrace_state *state, size_t size,
			      backtrace_error_callback error_callback,
			      void *data) ATTRIBUTE_MALLOC;

/* Free memory allocated by backtrace_alloc.  If ERROR_CALLBACK is
   NULL, this does not report an error.  */

extern void backtrace_free (struct backtrace_state *state, void *mem,
			    size_t size,
			    backtrace_error_callback error_callback,
			    void *data);

/* A growable vector of some struct.  This is used for more efficient
   allocation when we don't know the final size of some group of data
   that we want to represent as an array.  */

struct backtrace_vector
{
  /* The base of the vector.  */
  void *base;
  /* The number of bytes in the vector.  */
  size_t size;
  /* The number of bytes available at the current allocation.  */
  size_t alc;
};

/* Grow VEC by SIZE bytes.  Return a pointer to the newly allocated
   bytes.  Note that this may move the entire vector to a new memory
   location.  Returns NULL on failure.  */

extern void *backtrace_vector_grow (struct backtrace_state *state, size_t size,
				    backtrace_error_callback error_callback,
				    void *data,
				    struct backtrace_vector *vec);

/* Finish the current allocation on VEC.  Prepare to start a new
   allocation.  The finished allocation will never be freed.  Returns
   a pointer to the base of the finished entries, or NULL on
   failure.  */

extern void* backtrace_vector_finish (struct backtrace_state *state,
				      struct backtrace_vector *vec,
				      backtrace_error_callback error_callback,
				      void *data);

/* Release any extra space allocated for VEC.  This may change
   VEC->base.  Returns 1 on success, 0 on failure.  */

extern int backtrace_vector_release (struct backtrace_state *state,
				     struct backtrace_vector *vec,
				     backtrace_error_callback error_callback,
				     void *data);

/* Free the space managed by VEC.  This will reset VEC.  */

static inline void
backtrace_vector_free (struct backtrace_state *state,
		       struct backtrace_vector *vec,
		       backtrace_error_callback error_callback, void *data)
{
  vec->alc += vec->size;
  vec->size = 0;
  backtrace_vector_release (state, vec, error_callback, data);
}

/* Read initial debug data from a descriptor, and set the
   fileline_data, syminfo_fn, and syminfo_data fields of STATE.
   Return the fileln_fn field in *FILELN_FN--this is done this way so
   that the synchronization code is only implemented once.  This is
   called after the descriptor has first been opened.  It will close
   the descriptor if it is no longer needed.  Returns 1 on success, 0
   on error.  There will be multiple implementations of this function,
   for different file formats.  Each system will compile the
   appropriate one.  */

extern int backtrace_initialize (struct backtrace_state *state,
				 const char *filename,
				 int descriptor,
				 backtrace_error_callback error_callback,
				 void *data,
				 fileline *fileline_fn);

/* An enum for the DWARF sections we care about.  */

enum dwarf_section
{
  DEBUG_INFO,
  DEBUG_LINE,
  DEBUG_ABBREV,
  DEBUG_RANGES,
  DEBUG_STR,
  DEBUG_ADDR,
  DEBUG_STR_OFFSETS,
  DEBUG_LINE_STR,
  DEBUG_RNGLISTS,

  DEBUG_MAX
};

/* Data for the DWARF sections we care about.  */

struct dwarf_sections
{
  const unsigned char *data[DEBUG_MAX];
  size_t size[DEBUG_MAX];
};

/* DWARF data read from a file, used for .gnu_debugaltlink.  */

struct dwarf_data;

/* The load address mapping.  */

#if defined(__FDPIC__) && defined(HAVE_DL_ITERATE_PHDR) && (defined(HAVE_LINK_H) || defined(HAVE_SYS_LINK_H))

#ifdef HAVE_LINK_H
 #include <link.h>
#endif
#ifdef HAVE_SYS_LINK_H
 #include <sys/link.h>
#endif

#define libbacktrace_using_fdpic() (1)

struct libbacktrace_base_address
{
  struct elf32_fdpic_loadaddr m;
};

#define libbacktrace_add_base(pc, base) \
  ((uintptr_t) (__RELOC_POINTER ((pc), (base).m)))

#else /* not _FDPIC__ */

#define libbacktrace_using_fdpic() (0)

struct libbacktrace_base_address
{
  uintptr_t m;
};

#define libbacktrace_add_base(pc, base) ((pc) + (base).m)

#endif /* not _FDPIC__ */

/* Add file/line information for a DWARF module.  */

extern int backtrace_dwarf_add (struct backtrace_state *state,
				struct libbacktrace_base_address base_address,
				const struct dwarf_sections *dwarf_sections,
				int is_bigendian,
				struct dwarf_data *fileline_altlink,
				backtrace_error_callback error_callback,
				void *data, fileline *fileline_fn,
				struct dwarf_data **fileline_entry);

/* A data structure to pass to backtrace_syminfo_to_full.  */

struct backtrace_call_full
{
  backtrace_full_callback full_callback;
  backtrace_error_callback full_error_callback;
  void *full_data;
  int ret;
};

/* A backtrace_syminfo_callback that can call into a
   backtrace_full_callback, used when we have a symbol table but no
   debug info.  */

extern void backtrace_syminfo_to_full_callback (void *data, uintptr_t pc,
						const char *symname,
						uintptr_t symval,
						uintptr_t symsize);

/* An error callback that corresponds to
   backtrace_syminfo_to_full_callback.  */

extern void backtrace_syminfo_to_full_error_callback (void *, const char *,
						      int);

/* A test-only hook for elf_uncompress_zdebug.  */

extern int backtrace_uncompress_zdebug (struct backtrace_state *,
					const unsigned char *compressed,
					size_t compressed_size,
					backtrace_error_callback, void *data,
					unsigned char **uncompressed,
					size_t *uncompressed_size);

/* A test-only hook for elf_zstd_decompress.  */

extern int backtrace_uncompress_zstd (struct backtrace_state *,
				      const unsigned char *compressed,
				      size_t compressed_size,
				      backtrace_error_callback, void *data,
				      unsigned char *uncompressed,
				      size_t uncompressed_size);

/* A test-only hook for elf_uncompress_lzma.  */

extern int backtrace_uncompress_lzma (struct backtrace_state *,
				      const unsigned char *compressed,
				      size_t compressed_size,
				      backtrace_error_callback, void *data,
				      unsigned char **uncompressed,
				      size_t *uncompressed_size);

/* Source evidence.  These structures reconstruct the provenance of an
   inlined call chain directly from the parsed DWARF data.  Every byte
   span is recorded while the .debug_info/.debug_line/.debug_str
   sections are being parsed; the offsets are section relative offsets,
   not ELF file offsets.  All pointers are borrowed from the
   dwarf_sections buffers passed to backtrace_dwarf_add and from the
   backtrace_state allocator, so they stay valid for the lifetime of
   the backtrace_state.  The state is never freed by this library;
   callers that own the section buffers (the sample driver keeps them
   for the whole process) must keep them alive as long as any evidence
   structure is in use.  */

/* Kind of a parsed attribute operand.  */

enum dwarf_evidence_encoding
{
  /* No operand bytes (DW_FORM_flag_present).  */
  DW_EVIDENCE_ENCODING_NONE,
  /* An address-sized unsigned value (DW_FORM_addr).  */
  DW_EVIDENCE_ENCODING_UINT,
  /* An inline DW_FORM_string, pointing into .debug_info.  */
  DW_EVIDENCE_ENCODING_INLINE_STRING,
  /* A DW_FORM_strp value, pointing into .debug_str.  */
  DW_EVIDENCE_ENCODING_STRP
};

/* One attribute operand on a concrete or abstract DIE.  */

struct dwarf_evidence_attr
{
  /* DW_AT_* attribute name.  */
  unsigned int attr;
  /* DW_FORM_* form.  */
  unsigned int form;
  /* Section-relative .debug_info offset of the first operand byte.  */
  size_t offset;
  /* Number of operand bytes; zero for DW_FORM_flag_present.  */
  size_t length;
  /* How raw_value/string should be interpreted.  */
  enum dwarf_evidence_encoding encoding;
  /* The decoded scalar value (also used for DW_FORM_strp offsets).  */
  uint64_t raw_value;
  /* For string forms, the NUL-terminated bytes.  */
  const unsigned char *string;
  /* Length of STRING including the terminating NUL, or 0.  */
  size_t string_length;
};

/* One hop along a DW_AT_abstract_origin or DW_AT_specification
   reference.  */

struct dwarf_evidence_ref
{
  /* DW_AT_abstract_origin or DW_AT_specification.  */
  unsigned int attr;
  /* Form of the reference operand.  */
  unsigned int form;
  /* .debug_info offset and length of the reference operand.  */
  size_t attr_offset;
  size_t attr_length;
  /* Section-relative offset of the DIE holding the reference.  */
  size_t from_die;
  /* Section-relative offset of the referenced DIE.  */
  size_t to_die;
  /* Section-relative start of the CU containing FROM_DIE.  */
  size_t from_cu;
  /* Section-relative start of the CU containing TO_DIE.  */
  size_t to_cu;
};

/* A DIE that participates in a provenance chain, together with the
   attributes the evidence reconstruction inspected.  */

struct dwarf_evidence_die
{
  /* DW_TAG_* tag of the DIE.  */
  unsigned int tag;
  /* Section-relative .debug_info offset of the DIE (first abbrev code
     byte).  */
  size_t die_offset;
  /* Section-relative start/end of the containing CU.  */
  size_t cu_offset;
  size_t cu_end;
  /* DW_AT_stmt_list raw value of the containing CU.  */
  uint64_t stmt_list;
  /* The attributes of this DIE in their on-disk order.  */
  const struct dwarf_evidence_attr *attrs;
  size_t attrs_count;
};

/* The provenance of one resolved declaration attribute.  */

struct dwarf_evidence_source
{
  /* One of the DW_EVIDENCE_KEY_* keys below.  */
  unsigned int key;
  /* The chain of DIEs consulted, starting with the concrete DIE.  */
  const struct dwarf_evidence_die *dies;
  size_t dies_count;
  /* The reference hops actually followed, in traversal order.  */
  const struct dwarf_evidence_ref *refs;
  size_t refs_count;
  /* The DIE that supplied the value, i.e. dies[provider_index].  */
  size_t provider_index;
  /* The supplying attribute on that DIE.  */
  struct dwarf_evidence_attr provider_attr;
  /* Non-zero if the attribute was present; a value of zero is still
     present and must be reported.  */
  int present;
};

/* Keys for the declaration/call attributes reconstructed per frame.  */

enum dwarf_evidence_key
{
  DW_EVIDENCE_KEY_NAME = 0,
  DW_EVIDENCE_KEY_DECL_FILE,
  DW_EVIDENCE_KEY_DECL_LINE,
  DW_EVIDENCE_KEY_DECL_COLUMN,
  DW_EVIDENCE_KEY_CALL_FILE,
  DW_EVIDENCE_KEY_CALL_LINE,
  DW_EVIDENCE_KEY_CALL_COLUMN,
  DW_EVIDENCE_KEY_LOW_PC,
  DW_EVIDENCE_KEY_HIGH_PC,
  DW_EVIDENCE_KEY_COUNT
};

/* One frame of an inlined call chain.  Depth 0 is the physical
   (concrete outer) function; each inlined instance adds one.  The
   sources array is indexed by enum dwarf_evidence_key.  A source with
   present == 0 means the attribute did not exist anywhere on the
   permitted chain.  */

/* Evidence for one DWARF4 line-program file-name entry.  All offsets
   are section-relative .debug_line offsets, except comp_dir_* which
   are section-relative .debug_info offsets of the CU's
   DW_AT_comp_dir operand.  A directory index of zero means the entry
   is relative to the compilation directory; the directory spans are
   then unused.  */

struct dwarf_evidence_file
{
  /* The CU whose line table and file table supply this entry.  */
  size_t cu_offset;
  size_t cu_end;
  /* Raw DW_AT_stmt_list value of that CU.  */
  uint64_t stmt_list;
  /* Original DW_AT_decl_file / DW_AT_call_file value (1-based).  */
  uint64_t file_index;
  /* .debug_line span of the whole file entry: from the first file
     name byte through the trailing size ULEB.  */
  size_t file_entry_offset;
  size_t file_entry_length;
  /* .debug_line span of the referenced directory entry including its
     NUL.  Zero length when the directory index is zero.  */
  size_t directory_entry_offset;
  size_t directory_entry_length;
  /* .debug_info span of the CU's DW_AT_comp_dir operand.  */
  size_t compdir_attr_offset;
  size_t compdir_attr_length;
  /* Form and decoded NUL-terminated bytes of DW_AT_comp_dir, so the
     report can place the string evidence in .debug_info (inline
     string) or .debug_str (strp).  */
  unsigned int compdir_form;
  const unsigned char *compdir_string;
  size_t compdir_string_length;
  /* For DW_FORM_strp, the raw .debug_str offset operand.  */
  uint64_t compdir_raw_value;
  /* The fully resolved path (directory joined with comp_dir when the
     directory entry is relative), borrowed from the state allocator.  */
  const char *resolved_path;
  /* The directory index as encoded.  */
  uint64_t directory_index;
};

struct dwarf_evidence_frame
{
  /* Depth of this frame; 0 for the physical function.  */
  unsigned int depth;
  /* The concrete DIE backing this frame.  */
  struct dwarf_evidence_die concrete;
  /* The half-open address range of this frame that contains the
     queried PC.  */
  uintptr_t low_pc;
  uintptr_t high_pc;
  /* Resolved provenance for every tracked attribute.  */
  struct dwarf_evidence_source sources[DW_EVIDENCE_KEY_COUNT];
  /* File-table evidence for the resolved declaration and call files.
     NULL when the corresponding file attribute is absent.  Borrowed
     from the state allocator.  */
  const struct dwarf_evidence_file *decl_file_evidence;
  const struct dwarf_evidence_file *call_file_evidence;
};

/* The complete evidence for one PC query.  */

struct dwarf_evidence
{
  /* Frames ordered from the innermost inlined instance down to the
     physical function.  frames[0] is innermost; the physical function
     is the last entry.  */
  const struct dwarf_evidence_frame *frames;
  size_t frames_count;
};

/* Reconstruct the source evidence for PC using the existing DWARF
   address and inlined-function selection (single chain, half-open
   ranges, no overlap enumeration).  STATE must be the non-threaded
   state that ENTRY was added to.  On success *EVIDENCE points to
   memory owned by STATE (via backtrace_alloc) and remains valid for
   the lifetime of the state; it must not be freed by the caller.  The
   per-PC result is cached on ENTRY, so distinct objects (distinct
   dwarf_data) never share evidence.  Returns 1 on success, 0 on
   error.  A PC with no match succeeds with frames_count == 0.  */

extern int backtrace_dwarf_evidence (struct backtrace_state *state,
				     struct dwarf_data *entry, uintptr_t pc,
				     const struct dwarf_evidence **evidence,
				     backtrace_error_callback error_callback,
				     void *data);

/* Current expected backtrace_moredata version.  */
#define BACKTRACE_MOREDATA_VERSION (4)

#endif
