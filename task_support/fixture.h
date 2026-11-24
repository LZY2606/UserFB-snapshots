#ifndef TASK_FIXTURE_H
#define TASK_FIXTURE_H
#include <stddef.h>
/* Builds one valid synthetic ELF64 little-endian ET_REL. Caller owns bytes. */
unsigned char *task_fixture(int compressed, size_t *length);

/*
 * General purpose synthetic object builder used by the compressed-section
 * regression and by hand-column comparisons.  Builds an ET_REL with six
 * sections: 0=null, 1=.text PROGBITS (96 bytes), 2=SHT_SYMTAB,
 * 3=SHT_STRTAB (256 bytes, the symbol name lives at offset 192),
 * 4=SHT_SYMTAB_SHNDX and 5=SHT_RELA targeting section 1.  Symbol 1 is
 * a global function whose raw st_shndx is SHN_XINDEX; section 4 carries
 * the companion section index.  Caller owns the returned bytes.
 */
struct task_fixture_spec {
	int	elfclass;	/* ELFCLASS32 or ELFCLASS64 */
	int	data;		/* ELFDATA2LSB or ELFDATA2MSB */
	int	compress_mask;	/* bit (1<<section index) set => compressed */
	const char *name;	/* symbol 1 name, installed at strtab[192] */
	long long addend;	/* relocation record 0 addend */
	unsigned long long sym_value;
	unsigned long long sym_size;
	unsigned int xindex;	/* companion definition section for symbol 1 */
	int	tweak;		/* TF_* corruption selector */
	int	tweak_scn;	/* section index (1..5) the tweak applies to */
};

enum {
	TF_NONE = 0,
	TF_BAD_HDR_TYPE,	/* unknown ch_type -> ELF_E_UNIMPL */
	TF_BAD_RESERVED,	/* nonzero ELF64 reserved word -> ELF_E_DATA */
	TF_BAD_ALIGN,		/* ch_addralign not 0/1/power of two -> DATA */
	TF_TRUNC_HEADER,	/* physical sh_size shorter than the header */
	TF_CORRUPT_STREAM,	/* flipped byte inside the zlib stream */
	TF_TRAILING_BYTE,	/* byte after the end of the zlib stream */
	TF_SIZE_MISMATCH,	/* ch_size disagrees with the stream output */
	TF_BAD_MULTIPLE,	/* logical size is not a whole record */
	TF_EMPTY_LOGICAL,	/* ch_size == 0 with a valid empty stream */
	TF_FLAG_ALLOC,		/* SHF_COMPRESSED combined with SHF_ALLOC */
	TF_FLAG_NOBITS,		/* SHF_COMPRESSED combined with SHT_NOBITS */
	TF_NUM
};

unsigned char *task_fixture_build(const struct task_fixture_spec *spec,
    size_t *length);

/* Number of sections (including the null section) in every built object. */
#define	TASK_FIXTURE_NSCN	6

/* Hand-listed column values shared by the built reference objects. */
#define	TASK_NAME_OFFSET	192U
#define	TASK_STRTAB_SIZE	256U
#define	TASK_TARGET_SECTION	1U
#define	TASK_SYMTAB_SECTION	2U
#define	TASK_STRTAB_SECTION	3U
#define	TASK_SHNDX_SECTION	4U
#define	TASK_RELA_SECTION	5U
#define	TASK_TARGET_SYMBOL	1U
#define	TASK_TARGET_VALUE	0x40ULL
#define	TASK_TARGET_SIZE	8ULL
#define	TASK_TARGET_OFFSET	8ULL
#define	TASK_TARGET_RTYPE	2U
#define	TASK_TARGET_ADDEND	(-4LL)
#endif
