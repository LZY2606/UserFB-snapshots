/*
 * Regression contract for SHF_COMPRESSED read support.
 *
 * The cases build real synthetic ELF32/ELF64 objects in both byte
 * orders through task_support/fixture.c and drive the normal
 * elf_getdata/elf_rawdata/elf_strptr/GElf read chain; no private
 * decompression entry point is used directly.
 */

#include <fcntl.h>
#include <gelf.h>
#include <libelf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../task_support/check.h"
#include "../task_support/fixture.h"

#define	ALL_SECTIONS_COMPRESSED	((1U << 2) | (1U << 3) | (1U << 4) | (1U << 5))

static struct task_fixture_spec
base_spec(int elfclass, int data, unsigned mask)
{
	struct task_fixture_spec spec;

	memset(&spec, 0, sizeof(spec));
	spec.elfclass = elfclass;
	spec.data = data;
	spec.compress_mask = mask;
	spec.name = "target";
	spec.addend = TASK_TARGET_ADDEND;
	spec.sym_value = TASK_TARGET_VALUE;
	spec.sym_size = TASK_TARGET_SIZE;
	spec.xindex = TASK_TARGET_SECTION;
	return (spec);
}

static Elf *
open_image(unsigned char *buf, size_t len)
{
	static int initialized;

	/* The shared driver does not call elf_version(); do it once. */
	if (!initialized) {
		if (elf_version(EV_CURRENT) == EV_NONE)
			return (NULL);
		initialized = 1;
	}
	return (elf_memory((char *)buf, len));
}

/* Walk every section and materialize both raw and logical descriptors. */
static int
materialize_all(Elf *e)
{
	for (size_t i = 1; i < TASK_FIXTURE_NSCN; ++i) {
		Elf_Scn *s = elf_getscn(e, i);
		TASK_CHECK(s != NULL);
		TASK_CHECK(elf_rawdata(s, NULL) != NULL);
		TASK_CHECK(elf_getdata(s, NULL) != NULL);
	}
	return (0);
}

/* ------------------------------------------------------------------ */
/* views group                                                        */
/* ------------------------------------------------------------------ */

static int
view_class_encoding(int elfclass, int data)
{
	struct task_fixture_spec plain = base_spec(elfclass, data, 0);
	struct task_fixture_spec zspec = base_spec(elfclass, data,
	    ALL_SECTIONS_COMPRESSED);
	size_t plen = 0, zlen = 0;
	unsigned char *pbuf = task_fixture_build(&plain, &plen);
	unsigned char *zbuf = task_fixture_build(&zspec, &zlen);
	Elf *pe, *ze;

	TASK_CHECK(pbuf != NULL && zbuf != NULL);
	pe = open_image(pbuf, plen);
	ze = open_image(zbuf, zlen);

	for (size_t i = 1; i < TASK_FIXTURE_NSCN; ++i) {
		Elf_Scn *ps = elf_getscn(pe, i);
		Elf_Scn *zs = elf_getscn(ze, i);
		GElf_Shdr psh, zsh;
		Elf_Data *zr, *pd, *zd, *again;

		TASK_CHECK(ps != NULL && zs != NULL);
		TASK_CHECK(gelf_getshdr(ps, &psh) != NULL);
		TASK_CHECK(gelf_getshdr(zs, &zsh) != NULL);

		if (i >= 2) {
			/* Sections 2..5 are independently compressed. */
			TASK_CHECK((zsh.sh_flags & SHF_COMPRESSED) != 0);
			TASK_CHECK((psh.sh_flags & SHF_COMPRESSED) == 0);
		}

		/* Raw data: physical bytes, ELF_T_BYTE, stable cache. */
		zr = elf_rawdata(zs, NULL);
		TASK_CHECK(zr != NULL);
		TASK_CHECK(zr->d_type == ELF_T_BYTE);
		TASK_CHECK(zr->d_size == zsh.sh_size);
		TASK_CHECK(zr->d_off == 0);
		TASK_CHECK(zr->d_align == zsh.sh_addralign);
		/* elf_memory exposes the image itself as the raw file. */
		TASK_CHECK(zr->d_buf == (void *)(zbuf + zsh.sh_offset));
		again = elf_rawdata(zs, NULL);
		TASK_CHECK(again == zr);

		/* Logical data: translated by section type, like plain. */
		pd = elf_getdata(ps, NULL);
		zd = elf_getdata(zs, NULL);
		TASK_CHECK(pd != NULL && zd != NULL);
		TASK_CHECK(zd->d_type == pd->d_type);
		TASK_CHECK(zd->d_size == pd->d_size);
		TASK_CHECK(zd->d_off == 0);
		TASK_CHECK(zd->d_version == pd->d_version);

		/*
		 * d_align must come from ch_addralign inside the raw
		 * compression header, not from the on-disk sh_addralign.
		 */
		if (i >= 2) {
			size_t aoff = (elfclass == ELFCLASS32) ? 8 : 16;
			size_t aw = (elfclass == ELFCLASS32) ? 4 : 8;
			unsigned long long ch_align = 0;

			for (size_t k = 0; k < aw; k++) {
				unsigned char byte =
				    ((unsigned char *)zr->d_buf)[aoff + k];
				if (data == ELFDATA2MSB)
					ch_align = (ch_align << 8) | byte;
				else
					ch_align |= (unsigned long long)
					    byte << (8 * k);
			}
			TASK_CHECK(zd->d_align == ch_align);
		}
		if (zd->d_size > 0)
			TASK_CHECK(zd->d_buf != NULL);
		/* Raw and logical descriptors hold distinct buffers. */
		TASK_CHECK(zd->d_buf != zr->d_buf);
		again = elf_getdata(zs, NULL);
		TASK_CHECK(again == zd);

		/* Interleaved access never disturbs either descriptor. */
		TASK_CHECK(elf_rawdata(zs, NULL) == zr);
		TASK_CHECK(elf_getdata(zs, NULL) == zd);
		TASK_CHECK(elf_rawdata(zs, NULL) == zr);

		/* The logical chain holds exactly one descriptor. */
		TASK_CHECK(elf_getdata(zs, zd) == NULL);
		/* Re-fetching from NULL after iteration still hits cache. */
		TASK_CHECK(elf_getdata(zs, NULL) == zd);
		TASK_CHECK(elf_rawdata(zs, NULL) == zr);
	}

	elf_end(pe);
	elf_end(ze);
	free(pbuf);
	free(zbuf);
	return (0);
}

static int
views_32_le(void) { return (view_class_encoding(ELFCLASS32, ELFDATA2LSB)); }
static int
views_32_be(void) { return (view_class_encoding(ELFCLASS32, ELFDATA2MSB)); }
static int
views_64_le(void) { return (view_class_encoding(ELFCLASS64, ELFDATA2LSB)); }
static int
views_64_be(void) { return (view_class_encoding(ELFCLASS64, ELFDATA2MSB)); }

static int
views_shdr_and_rawfile_unchanged(void)
{
	struct task_fixture_spec spec = base_spec(ELFCLASS64, ELFDATA2LSB,
	    ALL_SECTIONS_COMPRESSED);
	size_t len = 0;
	unsigned char *buf = task_fixture_build(&spec, &len);
	unsigned char *snapshot;
	Elf *e;

	TASK_CHECK(buf != NULL);
	snapshot = malloc(len);
	TASK_CHECK(snapshot != NULL);
	memcpy(snapshot, buf, len);

	e = open_image(buf, len);
	TASK_CHECK(materialize_all(e) == 0);

	TASK_CHECK(elf_rawfile(e, NULL) == (char *)buf);
	TASK_CHECK(memcmp(buf, snapshot, len) == 0);

	for (size_t i = 1; i < TASK_FIXTURE_NSCN; ++i) {
		GElf_Shdr sh;
		TASK_CHECK(gelf_getshdr(elf_getscn(e, i), &sh) != NULL);
		if (i >= 2) {
			TASK_CHECK((sh.sh_flags & SHF_COMPRESSED) != 0);
			/* Physical offset/size remain the on-disk values. */
			TASK_CHECK(sh.sh_offset != 0);
		}
	}

	elf_end(e);
	free(snapshot);
	free(buf);
	return (0);
}

static int
views_empty_logical_section(void)
{
	struct task_fixture_spec spec = base_spec(ELFCLASS64, ELFDATA2LSB,
	    ALL_SECTIONS_COMPRESSED);
	size_t len = 0;
	unsigned char *buf;
	Elf *e;
	Elf_Data *d, *raw;

	spec.tweak = TF_EMPTY_LOGICAL;
	spec.tweak_scn = TASK_SHNDX_SECTION;
	buf = task_fixture_build(&spec, &len);
	TASK_CHECK(buf != NULL);
	e = open_image(buf, len);

	d = elf_getdata(elf_getscn(e, TASK_SHNDX_SECTION), NULL);
	TASK_CHECK(d != NULL);
	TASK_CHECK(d->d_size == 0);
	TASK_CHECK(d->d_buf == NULL);
	TASK_CHECK(d->d_off == 0);
	TASK_CHECK(d->d_type == ELF_T_WORD);
	TASK_CHECK(d->d_align == 4);

	raw = elf_rawdata(elf_getscn(e, TASK_SHNDX_SECTION), NULL);
	TASK_CHECK(raw != NULL && raw->d_size > 0);
	TASK_CHECK(raw->d_type == ELF_T_BYTE);

	/* Other sections stay readable. */
	TASK_CHECK(elf_getdata(elf_getscn(e, TASK_SYMTAB_SECTION),
	    NULL) != NULL);
	TASK_CHECK(elf_strptr(e, TASK_STRTAB_SECTION, TASK_NAME_OFFSET)
	    != NULL);

	elf_end(e);
	free(buf);
	return (0);
}

static int
views_layout_flag(void)
{
	struct task_fixture_spec spec = base_spec(ELFCLASS64, ELFDATA2LSB,
	    ALL_SECTIONS_COMPRESSED);
	size_t len = 0;
	unsigned char *buf = task_fixture_build(&spec, &len);
	Elf *e;
	char *name;

	TASK_CHECK(buf != NULL);
	e = open_image(buf, len);
	TASK_CHECK(elf_flagelf(e, ELF_C_SET, ELF_F_LAYOUT) != 0);

	/* Logical offsets still resolve through the decompressed view. */
	name = elf_strptr(e, TASK_STRTAB_SECTION, TASK_NAME_OFFSET);
	TASK_CHECK(name != NULL && strcmp(name, "target") == 0);
	TASK_CHECK(elf_strptr(e, TASK_STRTAB_SECTION,
	    TASK_STRTAB_SIZE) == NULL);
	TASK_CHECK(elf_errno() == ELF_E_ARGUMENT);

	elf_end(e);
	free(buf);
	return (0);
}

/* ------------------------------------------------------------------ */
/* symbols group                                                      */
/* ------------------------------------------------------------------ */

static int
check_reference_columns(Elf *e, const char *want_name, long long want_addend,
    unsigned long long want_value, unsigned long long want_size)
{
	Elf_Scn *symscn = elf_getscn(e, TASK_SYMTAB_SECTION);
	Elf_Scn *strscn = elf_getscn(e, TASK_STRTAB_SECTION);
	Elf_Scn *xscn = elf_getscn(e, TASK_SHNDX_SECTION);
	Elf_Scn *relascn = elf_getscn(e, TASK_RELA_SECTION);
	Elf_Data *sd, *xd, *rd;
	GElf_Sym sym;
	GElf_Rela rela;
	Elf32_Word xindex = 0;
	char *name;

	TASK_CHECK(symscn && strscn && xscn && relascn);
	sd = elf_getdata(symscn, NULL);
	xd = elf_getdata(xscn, NULL);
	rd = elf_getdata(relascn, NULL);
	TASK_CHECK(sd && xd && rd);

	/* Symbol 1 through the same chain the analysis program uses. */
	TASK_CHECK(gelf_getsymshndx(sd, xd, TASK_TARGET_SYMBOL, &sym,
	    &xindex) != NULL);
	TASK_CHECK(sym.st_name == TASK_NAME_OFFSET);
	TASK_CHECK(ELF64_ST_BIND(sym.st_info) == STB_GLOBAL);
	TASK_CHECK(ELF64_ST_TYPE(sym.st_info) == STT_FUNC);
	TASK_CHECK(sym.st_shndx == SHN_XINDEX);
	TASK_CHECK(xindex == TASK_TARGET_SECTION);
	TASK_CHECK(sym.st_value == want_value);
	TASK_CHECK(sym.st_size == want_size);

	/* gelf_getsym without the companion must show the raw value. */
	TASK_CHECK(gelf_getsym(sd, TASK_TARGET_SYMBOL, &sym) != NULL);
	TASK_CHECK(sym.st_shndx == SHN_XINDEX);

	name = elf_strptr(e, TASK_STRTAB_SECTION, sym.st_name);
	TASK_CHECK(name != NULL && strcmp(name, want_name) == 0);

	/* Relocation 0: section 1 offset 8, symbol 1, type 2, addend. */
	TASK_CHECK(gelf_getrela(rd, 0, &rela) != NULL);
	TASK_CHECK(rela.r_offset == TASK_TARGET_OFFSET);
	TASK_CHECK(ELF64_R_SYM(rela.r_info) == TASK_TARGET_SYMBOL);
	TASK_CHECK(ELF64_R_TYPE(rela.r_info) == TASK_TARGET_RTYPE);
	TASK_CHECK((long long)rela.r_addend == want_addend);

	/* Record 1 is out of range for the single-record table. */
	TASK_CHECK(gelf_getrela(rd, 1, &rela) == NULL);
	(void)elf_errno();
	return (0);
}

static int
symbols_columns(int elfclass, int data, int compressed)
{
	struct task_fixture_spec spec = base_spec(elfclass, data,
	    compressed ? ALL_SECTIONS_COMPRESSED : 0);
	size_t len = 0;
	unsigned char *buf = task_fixture_build(&spec, &len);
	Elf *e;
	int rc;

	TASK_CHECK(buf != NULL);
	e = open_image(buf, len);
	rc = check_reference_columns(e, "target", TASK_TARGET_ADDEND,
	    TASK_TARGET_VALUE, TASK_TARGET_SIZE);
	elf_end(e);
	free(buf);
	return (rc);
}

static int
symbols_independent_compression(void)
{
	/* Only the symbol table and the relocation table are compressed;
	 * the string table and the SHNDX companion stay plain.  The
	 * sh_link/sh_info relationships still resolve the full chain. */
	struct task_fixture_spec spec = base_spec(ELFCLASS64, ELFDATA2LSB,
	    (1U << TASK_SYMTAB_SECTION) | (1U << TASK_RELA_SECTION));
	size_t len = 0;
	unsigned char *buf = task_fixture_build(&spec, &len);
	Elf *e;
	int rc;

	TASK_CHECK(buf != NULL);
	e = open_image(buf, len);
	TASK_CHECK(e != NULL);
	rc = check_reference_columns(e, "target", TASK_TARGET_ADDEND,
	    TASK_TARGET_VALUE, TASK_TARGET_SIZE);
	elf_end(e);
	free(buf);
	return (rc);
}

static int
symbols_columns_32_le_plain(void)
{ return (symbols_columns(ELFCLASS32, ELFDATA2LSB, 0)); }
static int
symbols_columns_32_le_zip(void)
{ return (symbols_columns(ELFCLASS32, ELFDATA2LSB, 1)); }
static int
symbols_columns_32_be_zip(void)
{ return (symbols_columns(ELFCLASS32, ELFDATA2MSB, 1)); }
static int
symbols_columns_64_le_plain(void)
{ return (symbols_columns(ELFCLASS64, ELFDATA2LSB, 0)); }
static int
symbols_columns_64_le_zip(void)
{ return (symbols_columns(ELFCLASS64, ELFDATA2LSB, 1)); }
static int
symbols_columns_64_be_zip(void)
{ return (symbols_columns(ELFCLASS64, ELFDATA2MSB, 1)); }

static int
symbols_plain_and_compressed_match(void)
{
	struct task_fixture_spec plain = base_spec(ELFCLASS64, ELFDATA2LSB, 0);
	struct task_fixture_spec zspec = base_spec(ELFCLASS64, ELFDATA2LSB,
	    ALL_SECTIONS_COMPRESSED);
	size_t plen = 0, zlen = 0;
	unsigned char *pbuf = task_fixture_build(&plain, &plen);
	unsigned char *zbuf = task_fixture_build(&zspec, &zlen);
	Elf *pe = open_image(pbuf, plen);
	Elf *ze = open_image(zbuf, zlen);
	Elf_Data *pd, *zd;
	GElf_Sym psym, zsym;
	GElf_Rela prela, zrela;
	Elf32_Word px = 0, zx = 0;

	TASK_CHECK(pbuf && zbuf);

	pd = elf_getdata(elf_getscn(pe, TASK_SYMTAB_SECTION), NULL);
	zd = elf_getdata(elf_getscn(ze, TASK_SYMTAB_SECTION), NULL);
	TASK_CHECK(gelf_getsymshndx(pd,
	    elf_getdata(elf_getscn(pe, TASK_SHNDX_SECTION), NULL), 1,
	    &psym, &px) != NULL);
	TASK_CHECK(gelf_getsymshndx(zd,
	    elf_getdata(elf_getscn(ze, TASK_SHNDX_SECTION), NULL), 1,
	    &zsym, &zx) != NULL);
	TASK_CHECK(memcmp(&psym, &zsym, sizeof(psym)) == 0);
	TASK_CHECK(px == zx && px == TASK_TARGET_SECTION);

	TASK_CHECK(gelf_getrela(
	    elf_getdata(elf_getscn(pe, TASK_RELA_SECTION), NULL), 0,
	    &prela) != NULL);
	TASK_CHECK(gelf_getrela(
	    elf_getdata(elf_getscn(ze, TASK_RELA_SECTION), NULL), 0,
	    &zrela) != NULL);
	TASK_CHECK(memcmp(&prela, &zrela, sizeof(prela)) == 0);

	elf_end(pe);
	elf_end(ze);
	free(pbuf);
	free(zbuf);
	return (0);
}

static int
symbols_name_variant_tracks_input(void)
{
	struct task_fixture_spec spec = base_spec(ELFCLASS64, ELFDATA2LSB,
	    ALL_SECTIONS_COMPRESSED);
	size_t len = 0;
	unsigned char *buf;
	Elf *e;
	GElf_Sym sym;
	Elf32_Word x = 0;
	char *name;

	spec.name = "renamed_target";
	buf = task_fixture_build(&spec, &len);
	TASK_CHECK(buf != NULL);
	e = open_image(buf, len);

	TASK_CHECK(gelf_getsymshndx(
	    elf_getdata(elf_getscn(e, TASK_SYMTAB_SECTION), NULL),
	    elf_getdata(elf_getscn(e, TASK_SHNDX_SECTION), NULL), 1,
	    &sym, &x) != NULL);
	name = elf_strptr(e, TASK_STRTAB_SECTION, sym.st_name);
	TASK_CHECK(name != NULL);
	TASK_CHECK(strcmp(name, "renamed_target") == 0);
	TASK_CHECK(strcmp(name, "target") != 0);
	/* The numeric columns do not move when only the name changes. */
	TASK_CHECK(sym.st_value == TASK_TARGET_VALUE);
	TASK_CHECK(sym.st_size == TASK_TARGET_SIZE);

	elf_end(e);
	free(buf);
	return (0);
}

static int
symbols_addend_variant_tracks_input(void)
{
	struct task_fixture_spec spec = base_spec(ELFCLASS64, ELFDATA2MSB,
	    ALL_SECTIONS_COMPRESSED);
	size_t len = 0;
	unsigned char *buf;
	Elf *e;
	GElf_Rela rela;

	spec.addend = -77;
	buf = task_fixture_build(&spec, &len);
	TASK_CHECK(buf != NULL);
	e = open_image(buf, len);

	TASK_CHECK(gelf_getrela(
	    elf_getdata(elf_getscn(e, TASK_RELA_SECTION), NULL), 0,
	    &rela) != NULL);
	TASK_CHECK((long long)rela.r_addend == -77);
	TASK_CHECK((long long)rela.r_addend != TASK_TARGET_ADDEND);
	TASK_CHECK(ELF64_R_TYPE(rela.r_info) == TASK_TARGET_RTYPE);

	elf_end(e);
	free(buf);
	return (0);
}

static int
symbols_strptr_logical_bounds(void)
{
	struct task_fixture_spec spec = base_spec(ELFCLASS64, ELFDATA2LSB,
	    ALL_SECTIONS_COMPRESSED);
	size_t len = 0;
	unsigned char *buf = task_fixture_build(&spec, &len);
	Elf *e;
	GElf_Shdr sh;
	char *p;

	TASK_CHECK(buf != NULL);
	e = open_image(buf, len);
	TASK_CHECK(gelf_getshdr(elf_getscn(e, TASK_STRTAB_SECTION),
	    &sh) != NULL);
	/* The high name offset lies beyond the physical disk length. */
	TASK_CHECK(sh.sh_size < TASK_NAME_OFFSET + 8);

	p = elf_strptr(e, TASK_STRTAB_SECTION, TASK_NAME_OFFSET);
	TASK_CHECK(p != NULL && strcmp(p, "target") == 0);
	p = elf_strptr(e, TASK_STRTAB_SECTION, TASK_STRTAB_SIZE - 1);
	TASK_CHECK(p != NULL && *p == '\0');

	/* Exactly the logical length is already out of bounds. */
	TASK_CHECK(elf_strptr(e, TASK_STRTAB_SECTION,
	    TASK_STRTAB_SIZE) == NULL);
	TASK_CHECK(elf_errno() == ELF_E_ARGUMENT);
	TASK_CHECK(elf_strptr(e, TASK_STRTAB_SECTION,
	    TASK_STRTAB_SIZE + 10) == NULL);
	TASK_CHECK(elf_errno() == ELF_E_ARGUMENT);

	elf_end(e);
	free(buf);
	return (0);
}

/* ------------------------------------------------------------------ */
/* failures group                                                     */
/* ------------------------------------------------------------------ */

static Elf *
open_tweaked(int tweak, int scn, struct task_fixture_spec *out)
{
	struct task_fixture_spec spec = base_spec(ELFCLASS64, ELFDATA2LSB,
	    ALL_SECTIONS_COMPRESSED);
	size_t len = 0;
	unsigned char *buf;

	spec.tweak = tweak;
	spec.tweak_scn = scn;
	buf = task_fixture_build(&spec, &len);
	if (buf == NULL)
		return (NULL);
	*out = spec;
	return (open_image(buf, len));
}

static int
expect_getdata_error(int tweak, int scn, int want)
{
	struct task_fixture_spec spec;
	Elf *e = open_tweaked(tweak, scn, &spec);
	Elf_Data *d, *raw;

	TASK_CHECK(e != NULL);

	/* Raw-first ordering: the physical view works before the failure. */
	raw = elf_rawdata(elf_getscn(e, scn), NULL);
	TASK_CHECK(raw != NULL);
	TASK_CHECK(raw->d_type == ELF_T_BYTE);

	d = elf_getdata(elf_getscn(e, scn), NULL);
	TASK_CHECK(d == NULL);
	TASK_CHECK(elf_errno() == want);

	/* A second attempt fails the same way and caches nothing. */
	d = elf_getdata(elf_getscn(e, scn), NULL);
	TASK_CHECK(d == NULL);
	TASK_CHECK(elf_errno() == want);

	/* The cached raw view of the same section remains available. */
	TASK_CHECK(elf_rawdata(elf_getscn(e, scn), NULL) == raw);

	elf_end(e);
	return (0);
}

static int
failures_bad_header_type(void)
{ return (expect_getdata_error(TF_BAD_HDR_TYPE, TASK_SYMTAB_SECTION,
    ELF_E_UNIMPL)); }

static int
failures_bad_reserved(void)
{ return (expect_getdata_error(TF_BAD_RESERVED, TASK_SYMTAB_SECTION,
    ELF_E_DATA)); }

static int
failures_bad_alignment(void)
{ return (expect_getdata_error(TF_BAD_ALIGN, TASK_SYMTAB_SECTION,
    ELF_E_DATA)); }

static int
failures_truncated_header(void)
{ return (expect_getdata_error(TF_TRUNC_HEADER, TASK_RELA_SECTION,
    ELF_E_DATA)); }

static int
failures_corrupt_stream(void)
{ return (expect_getdata_error(TF_CORRUPT_STREAM, TASK_SYMTAB_SECTION,
    ELF_E_DATA)); }

static int
failures_trailing_byte(void)
{ return (expect_getdata_error(TF_TRAILING_BYTE, TASK_RELA_SECTION,
    ELF_E_DATA)); }

static int
failures_size_mismatch(void)
{ return (expect_getdata_error(TF_SIZE_MISMATCH, TASK_STRTAB_SECTION,
    ELF_E_DATA)); }

static int
failures_record_multiple(void)
{ return (expect_getdata_error(TF_BAD_MULTIPLE, TASK_SYMTAB_SECTION,
    ELF_E_DATA)); }

static int
failures_compressed_with_alloc(void)
{ return (expect_getdata_error(TF_FLAG_ALLOC, TASK_SYMTAB_SECTION,
    ELF_E_DATA)); }

static int
failures_compressed_nobits(void)
{ return (expect_getdata_error(TF_FLAG_NOBITS, TASK_SYMTAB_SECTION,
    ELF_E_DATA)); }

static int
failures_other_sections_survive(void)
{
	struct task_fixture_spec spec;
	Elf *e = open_tweaked(TF_CORRUPT_STREAM, TASK_SYMTAB_SECTION,
	    &spec);

	/* The bad section fails ... */
	TASK_CHECK(elf_getdata(elf_getscn(e, TASK_SYMTAB_SECTION),
	    NULL) == NULL);
	TASK_CHECK(elf_errno() == ELF_E_DATA);

	/* ... while unrelated valid sections keep reading normally. */
	TASK_CHECK(elf_getdata(elf_getscn(e, TASK_TARGET_SECTION),
	    NULL) != NULL);
	TASK_CHECK(elf_getdata(elf_getscn(e, TASK_STRTAB_SECTION),
	    NULL) != NULL);
	TASK_CHECK(elf_getdata(elf_getscn(e, TASK_RELA_SECTION),
	    NULL) != NULL);
	TASK_CHECK(elf_strptr(e, TASK_STRTAB_SECTION,
	    TASK_NAME_OFFSET) != NULL);
	TASK_CHECK(elf_rawdata(elf_getscn(e, TASK_SYMTAB_SECTION),
	    NULL) != NULL);

	elf_end(e);
	return (0);
}

static int
failures_rdwr_materialization_unimpl(void)
{
	struct task_fixture_spec spec = base_spec(ELFCLASS64, ELFDATA2LSB,
	    ALL_SECTIONS_COMPRESSED);
	size_t len = 0;
	unsigned char *buf = task_fixture_build(&spec, &len);
	const char *tmpdir = getenv("TMPDIR");
	char path[256];
	int fd;
	Elf *e;
	Elf_Data *d;

	TASK_CHECK(buf != NULL);
	if (tmpdir == NULL || tmpdir[0] == '\0')
		tmpdir = "/tmp";
	snprintf(path, sizeof(path), "%s/task-elf-XXXXXX", tmpdir);
	fd = mkstemp(path);
	TASK_CHECK(fd >= 0);
	TASK_CHECK(write(fd, buf, len) == (ssize_t)len);
	TASK_CHECK(lseek(fd, 0, SEEK_SET) == 0);

	e = elf_begin(fd, ELF_C_RDWR, NULL);
	TASK_CHECK(e != NULL);

	/* Compressed materialization is read-only. */
	d = elf_getdata(elf_getscn(e, TASK_SYMTAB_SECTION), NULL);
	TASK_CHECK(d == NULL);
	TASK_CHECK(elf_errno() == ELF_E_UNIMPL);

	/* So is any write-back of an object with a compressed section. */
	TASK_CHECK(elf_update(e, ELF_C_NULL) == (off_t)-1);
	TASK_CHECK(elf_errno() == ELF_E_UNIMPL);

	/* Ordinary sections retain their existing read behavior. */
	TASK_CHECK(elf_getdata(elf_getscn(e, TASK_TARGET_SECTION),
	    NULL) != NULL);

	elf_end(e);
	close(fd);
	unlink(path);
	free(buf);
	return (0);
}

static int
failures_plain_sections_keep_rdwr_behavior(void)
{
	struct task_fixture_spec spec = base_spec(ELFCLASS64, ELFDATA2LSB, 0);
	size_t len = 0;
	unsigned char *buf = task_fixture_build(&spec, &len);
	const char *tmpdir = getenv("TMPDIR");
	char path[256];
	int fd;
	Elf *e;

	TASK_CHECK(buf != NULL);
	if (tmpdir == NULL || tmpdir[0] == '\0')
		tmpdir = "/tmp";
	snprintf(path, sizeof(path), "%s/task-plain-XXXXXX", tmpdir);
	fd = mkstemp(path);
	TASK_CHECK(fd >= 0);
	TASK_CHECK(write(fd, buf, len) == (ssize_t)len);
	TASK_CHECK(lseek(fd, 0, SEEK_SET) == 0);

	e = elf_begin(fd, ELF_C_RDWR, NULL);
	TASK_CHECK(e != NULL);

	/* No SHF_COMPRESSED section exists, so ordinary behavior is kept. */
	TASK_CHECK(elf_getdata(elf_getscn(e, TASK_SYMTAB_SECTION),
	    NULL) != NULL);
	TASK_CHECK(elf_update(e, ELF_C_NULL) != (off_t)-1);

	elf_end(e);
	close(fd);
	unlink(path);
	free(buf);
	return (0);
}

#define	CASE(G, S, F)	{ .group = G, .name = S, .run = F }

const struct task_case task_cases[] = {
	CASE("views", "elf32_little_endian", views_32_le),
	CASE("views", "elf32_big_endian", views_32_be),
	CASE("views", "elf64_little_endian", views_64_le),
	CASE("views", "elf64_big_endian", views_64_be),
	CASE("views", "shdr_rawfile_unchanged",
	    views_shdr_and_rawfile_unchanged),
	CASE("views", "empty_logical_section", views_empty_logical_section),
	CASE("views", "elf_f_layout_strptr", views_layout_flag),

	CASE("symbols", "columns_elf32_le_plain",
	    symbols_columns_32_le_plain),
	CASE("symbols", "columns_elf32_le_compressed",
	    symbols_columns_32_le_zip),
	CASE("symbols", "columns_elf32_be_compressed",
	    symbols_columns_32_be_zip),
	CASE("symbols", "columns_elf64_le_plain",
	    symbols_columns_64_le_plain),
	CASE("symbols", "columns_elf64_le_compressed",
	    symbols_columns_64_le_zip),
	CASE("symbols", "columns_elf64_be_compressed",
	    symbols_columns_64_be_zip),
	CASE("symbols", "plain_compressed_columns_match",
	    symbols_plain_and_compressed_match),
	CASE("symbols", "independently_compressed_sections",
	    symbols_independent_compression),
	CASE("symbols", "name_variant_tracks_input",
	    symbols_name_variant_tracks_input),
	CASE("symbols", "addend_variant_tracks_input",
	    symbols_addend_variant_tracks_input),
	CASE("symbols", "strptr_logical_bounds",
	    symbols_strptr_logical_bounds),

	CASE("failures", "bad_compression_type", failures_bad_header_type),
	CASE("failures", "bad_reserved_word", failures_bad_reserved),
	CASE("failures", "bad_alignment", failures_bad_alignment),
	CASE("failures", "truncated_header", failures_truncated_header),
	CASE("failures", "corrupt_stream", failures_corrupt_stream),
	CASE("failures", "trailing_byte", failures_trailing_byte),
	CASE("failures", "size_mismatch", failures_size_mismatch),
	CASE("failures", "record_size_not_multiple",
	    failures_record_multiple),
	CASE("failures", "compressed_with_shf_alloc",
	    failures_compressed_with_alloc),
	CASE("failures", "compressed_nobits", failures_compressed_nobits),
	CASE("failures", "other_sections_survive",
	    failures_other_sections_survive),
	CASE("failures", "rdwr_materialization_unimpl",
	    failures_rdwr_materialization_unimpl),
	CASE("failures", "plain_sections_keep_rdwr_behavior",
	    failures_plain_sections_keep_rdwr_behavior),
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
