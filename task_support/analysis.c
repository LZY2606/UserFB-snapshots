/*
 * Offline symbol-reference analysis program.
 *
 * The two input images are produced by task_support/fixture.c's
 * task_fixture(): one ordinary ELF64 LE relocatable object and one whose
 * symbol table, string table, extended section index table and
 * relocation section are independently SHF_COMPRESSED.  Both are written
 * to the output directory, then read back through the public GElf
 * interface only -- no private entry points, no stored answers.
 *
 * For every relocation section the program follows:
 *   sh_info  -> the section holding the referenced location,
 *   sh_link  -> the symbol table,
 *   symtab's sh_link -> the string table,
 *   the SHT_SYMTAB_SHNDX section linked back to the symbol table -> the
 *   expanded definition section for SHN_XINDEX symbols.
 *
 * Output is a tab-separated table.  Logical record offsets and the
 * physical on-disk range of each section are recorded separately; for a
 * compressed section no per-record physical byte range exists.
 */

#include <fcntl.h>
#include <gelf.h>
#include <libelf.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "fixture.h"

struct ref_columns {
	size_t	rela_scn;
	int	record;
	size_t	target_scn;
	unsigned long long r_offset;
	unsigned r_type;
	long long addend;
	size_t	symtab_scn;
	unsigned sym_index;
	char	name[128];
	unsigned raw_shndx;
	unsigned def_scn;
	unsigned long long st_value;
	unsigned long long st_size;
};

static void
fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	exit(1);
}

static int
write_file(const char *path, const unsigned char *buf, size_t len)
{
	FILE *f = fopen(path, "wb");

	if (f == NULL)
		return (-1);
	if (fwrite(buf, 1, len, f) != len) {
		fclose(f);
		return (-1);
	}
	fclose(f);
	return (0);
}

static Elf *
open_read(const char *path, int *fdp)
{
	int fd = open(path, O_RDONLY);
	Elf *e;

	if (fd < 0)
		fail("cannot open %s\n", path);
	e = elf_begin(fd, ELF_C_READ, NULL);
	if (e == NULL)
		fail("elf_begin(%s): %s\n", path, elf_errmsg(-1));
	*fdp = fd;
	return (e);
}

static Elf_Scn *
find_shndx_companion(Elf *e, size_t symtab_ndx)
{
	Elf_Scn *s = NULL;

	while ((s = elf_nextscn(e, s)) != NULL) {
		GElf_Shdr sh;

		if (gelf_getshdr(s, &sh) == NULL)
			fail("gelf_getshdr: %s\n", elf_errmsg(-1));
		if (sh.sh_type == SHT_SYMTAB_SHNDX &&
		    sh.sh_link == symtab_ndx)
			return (s);
	}
	return (NULL);
}

static int
analyze_one(const char *path, const char *id, FILE *out,
    struct ref_columns *first)
{
	int fd;
	Elf *e;
	Elf_Scn *s = NULL;
	size_t nrow = 0;

	e = open_read(path, &fd);

	while ((s = elf_nextscn(e, s)) != NULL) {
		GElf_Shdr rsh;
		Elf_Data *rd;
		int ndx;

		if (gelf_getshdr(s, &rsh) == NULL)
			fail("gelf_getshdr: %s\n", elf_errmsg(-1));
		if (rsh.sh_type != SHT_RELA && rsh.sh_type != SHT_REL)
			continue;

		rd = elf_getdata(s, NULL);
		if (rd == NULL)
			fail("elf_getdata relocation section: %s\n",
			    elf_errmsg(-1));

		/* sh_link names the symbol table; sh_info the target. */
		Elf_Scn *symscn = elf_getscn(e, rsh.sh_link);
		Elf_Scn *target = elf_getscn(e, rsh.sh_info);
		GElf_Shdr symsh, tsh;
		Elf_Data *sd;
		Elf_Scn *xscn;
		Elf_Data *xd = NULL;

		if (symscn == NULL || target == NULL ||
		    gelf_getshdr(symscn, &symsh) == NULL ||
		    gelf_getshdr(target, &tsh) == NULL)
			fail("relocation link/info sections missing\n");
		sd = elf_getdata(symscn, NULL);
		if (sd == NULL)
			fail("elf_getdata symbol table: %s\n",
			    elf_errmsg(-1));

		xscn = find_shndx_companion(e, rsh.sh_link);
		if (xscn != NULL) {
			xd = elf_getdata(xscn, NULL);
			if (xd == NULL)
				fail("elf_getdata SHNDX table: %s\n",
				    elf_errmsg(-1));
		}

		for (ndx = 0; ; ndx++) {
			GElf_Rela rela;
			GElf_Sym sym;
			Elf32_Word xindex = 0;
			char *name;
			struct ref_columns col;

			if (rsh.sh_type == SHT_RELA) {
				if (gelf_getrela(rd, ndx, &rela) == NULL)
					break;
				col.r_offset = (unsigned long long)
				    rela.r_offset;
				col.sym_index = (unsigned)
				    ELF64_R_SYM(rela.r_info);
				col.r_type = (unsigned)
				    ELF64_R_TYPE(rela.r_info);
				col.addend = (long long)rela.r_addend;
			} else {
				GElf_Rel rel;

				if (gelf_getrel(rd, ndx, &rel) == NULL)
					break;
				col.r_offset = (unsigned long long)
				    rel.r_offset;
				col.sym_index = (unsigned)
				    ELF64_R_SYM(rel.r_info);
				col.r_type = (unsigned)
				    ELF64_R_TYPE(rel.r_info);
				col.addend = 0;
			}

			if (gelf_getsymshndx(sd, xd, (int)col.sym_index,
			    &sym, &xindex) == NULL)
				fail("gelf_getsymshndx: %s\n",
				    elf_errmsg(-1));

			name = elf_strptr(e, symsh.sh_link, sym.st_name);
			if (name == NULL)
				fail("elf_strptr: %s\n", elf_errmsg(-1));

			col.rela_scn = elf_ndxscn(s);
			col.record = ndx;
			col.target_scn = rsh.sh_info;
			col.symtab_scn = rsh.sh_link;
			snprintf(col.name, sizeof(col.name), "%s", name);
			col.raw_shndx = (unsigned)sym.st_shndx;
			col.def_scn = (sym.st_shndx == SHN_XINDEX) ?
			    (unsigned)xindex : (unsigned)sym.st_shndx;
			col.st_value = (unsigned long long)sym.st_value;
			col.st_size = (unsigned long long)sym.st_size;

			/*
			 * The logical record position is its offset inside
			 * the decompressed section content.  The physical
			 * columns describe the whole on-disk section range;
			 * for a compressed container the zlib stream has no
			 * byte-exact per-record mapping.
			 */
			size_t logical_record_off = (size_t)ndx *
			    rsh.sh_entsize;
			int is_compressed =
			    (rsh.sh_flags & SHF_COMPRESSED) != 0;

			fprintf(out,
			    "%s\t%zu\t%d\t%zu\t0x%llx\t%u\t%lld\t%zu\t%u\t"
			    "%s\t%u\t%u\t0x%llx\t%llu\t0x%zx\t0x%llx\t%llu\t"
			    "%s\n",
			    id, col.rela_scn, col.record, col.target_scn,
			    col.r_offset, col.r_type, col.addend,
			    col.symtab_scn, col.sym_index, col.name,
			    col.raw_shndx, col.def_scn, col.st_value,
			    col.st_size, logical_record_off,
			    (unsigned long long)rsh.sh_offset,
			    (unsigned long long)rsh.sh_size,
			    is_compressed ?
			    "container-only:no-per-record-byte-mapping" :
			    "exact:sh_offset+logical_record_off");

			if (first->name[0] == '\0')
				*first = col;
			nrow++;
		}
	}

	elf_end(e);
	close(fd);
	return ((int)nrow);
}

static int
columns_match(const struct ref_columns *a, const struct ref_columns *b)
{
	return (a->rela_scn == b->rela_scn &&
	    a->record == b->record &&
	    a->target_scn == b->target_scn &&
	    a->r_offset == b->r_offset &&
	    a->r_type == b->r_type &&
	    a->addend == b->addend &&
	    a->symtab_scn == b->symtab_scn &&
	    a->sym_index == b->sym_index &&
	    strcmp(a->name, b->name) == 0 &&
	    a->raw_shndx == b->raw_shndx &&
	    a->def_scn == b->def_scn &&
	    a->st_value == b->st_value &&
	    a->st_size == b->st_size);
}

int
main(int argc, char **argv)
{
	const char *outdir = argc > 1 ? argv[1] : "analysis";
	char plain_path[512], zip_path[512], tsv_path[512];
	size_t plain_len = 0, zip_len = 0;
	unsigned char *plain_buf, *zip_buf;
	FILE *out;
	struct ref_columns plain_col, zip_col;

	if (elf_version(EV_CURRENT) == EV_NONE)
		fail("elf_version failed\n");

	plain_buf = task_fixture(0, &plain_len);
	zip_buf = task_fixture(1, &zip_len);
	if (plain_buf == NULL || zip_buf == NULL)
		fail("fixture construction failed\n");

	snprintf(plain_path, sizeof(plain_path), "%s/plain.elf", outdir);
	snprintf(zip_path, sizeof(zip_path), "%s/compressed.elf", outdir);
	snprintf(tsv_path, sizeof(tsv_path), "%s/references.tsv", outdir);

	(void) mkdir(outdir, 0755);

	if (write_file(plain_path, plain_buf, plain_len) != 0 ||
	    write_file(zip_path, zip_buf, zip_len) != 0)
		fail("could not write input ELF files under %s\n", outdir);

	out = fopen(tsv_path, "wb");
	if (out == NULL)
		fail("cannot open %s\n", tsv_path);

	fprintf(out,
	    "input_id\trela_scn\trela_record\ttarget_scn\tr_offset\t"
	    "r_type\tr_addend\tsymtab_scn\tsym_index\tsym_name\t"
	    "raw_st_shndx\tdef_scn\tst_value\tst_size\t"
	    "logical_record_off\tphysical_scn_off\tphysical_scn_size\t"
	    "physical_mapping\n");

	memset(&plain_col, 0, sizeof(plain_col));
	memset(&zip_col, 0, sizeof(zip_col));

	int nplain = analyze_one(plain_path, "plain", out, &plain_col);
	int nzip = analyze_one(zip_path, "compressed", out, &zip_col);
	fclose(out);

	if (nplain != nzip || nplain == 0)
		fail("reference row count differs: plain=%d compressed=%d\n",
		    nplain, nzip);
	if (!columns_match(&plain_col, &zip_col))
		fail("symbol reference columns differ between inputs\n");

	printf("wrote %s (%d reference rows per input)\n", tsv_path,
	    nplain);
	printf("inputs: %s (%zu bytes), %s (%zu bytes)\n",
	    plain_path, plain_len, zip_path, zip_len);
	printf("reference columns of plain and compressed inputs match\n");

	free(plain_buf);
	free(zip_buf);
	return (0);
}
