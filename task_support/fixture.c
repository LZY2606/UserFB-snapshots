#include "fixture.h"
#include <libelf.h>
#include <stdlib.h>
#include <string.h>
#include "zlib.h"

static void
putv(unsigned char *p, unsigned long long v, size_t n, int big)
{
	for (size_t i = 0; i < n; ++i)
		p[big ? n - 1 - i : i] = (unsigned char)(v >> (8 * i));
}

struct build_scn {
	const unsigned char *logical;
	size_t	logical_len;
	unsigned char *phys;
	size_t	phys_len;
	unsigned int type;
	unsigned long long flags;
	unsigned int link, info, entsize;
	unsigned int align;
	size_t	position;
};

/*
 * Wrap 'src' into an ELF compression header plus a zlib stream.
 * 'hdr_size' selects the ELF32 (12) or ELF64 (24) header layout.
 */
static unsigned char *
compress_section(const unsigned char *src, size_t n, int elfclass, int big,
    unsigned int align, size_t *out)
{
	uLongf cap = compressBound(n ? n : 1);
	size_t hdr_size = elfclass == ELFCLASS64 ? 24 : 12;
	unsigned char *b = calloc(1, hdr_size + cap);
	if (b == NULL)
		return (NULL);

	putv(b + 0, 1, 4, big);			/* ch_type = ELFCOMPRESS_ZLIB */
	if (elfclass == ELFCLASS64) {
		putv(b + 4, 0, 4, big);		/* ch_reserved */
		putv(b + 8, n, 8, big);		/* ch_size */
		putv(b + 16, align, 8, big);	/* ch_addralign */
	} else {
		putv(b + 4, n, 4, big);		/* ch_size */
		putv(b + 8, align, 4, big);	/* ch_addralign */
	}

	if (compress2(b + hdr_size, &cap, src, (uLong)n, 9) != Z_OK) {
		free(b);
		return (NULL);
	}
	*out = hdr_size + cap;
	return (b);
}

static void
free_sections(struct build_scn *sc, size_t n)
{
	for (size_t i = 0; i < n; ++i)
		free(sc[i].phys);
}

unsigned char *
task_fixture_build(const struct task_fixture_spec *spec, size_t *length)
{
	const int c32 = spec->elfclass == ELFCLASS32;
	const int big = spec->data == ELFDATA2MSB;
	const size_t symsz = c32 ? 16 : 24;
	const size_t relasz = c32 ? 12 : 24;
	const size_t ehsize = c32 ? 52 : 64;
	const size_t shentsize = c32 ? 40 : 64;
	unsigned char text[96];
	unsigned char sym[48];
	unsigned char str[TASK_STRTAB_SIZE];
	unsigned char idx[8];
	unsigned char rela[24];
	unsigned char *b = NULL;
	struct build_scn sc[TASK_FIXTURE_NSCN];
	size_t next, shoff;

	memset(text, 0, sizeof(text));
	memset(sym, 0, sizeof(sym));
	memset(str, 0, sizeof(str));
	memset(idx, 0, sizeof(idx));
	memset(rela, 0, sizeof(rela));
	memset(sc, 0, sizeof(sc));

	/* Symbol 1: global function, st_shndx = SHN_XINDEX. Symbol 0 is null. */
	{
		unsigned char *s1 = sym + symsz;

		if (c32) {
			putv(s1 + 0, TASK_NAME_OFFSET, 4, big);
			putv(s1 + 4, spec->sym_value, 4, big);
			putv(s1 + 8, spec->sym_size, 4, big);
			s1[12] = 0x12;		/* STB_GLOBAL | STT_FUNC */
			s1[13] = 0;
			putv(s1 + 14, SHN_XINDEX, 2, big);
		} else {
			putv(s1 + 0, TASK_NAME_OFFSET, 4, big);
			s1[4] = 0x12;
			s1[5] = 0;
			putv(s1 + 6, SHN_XINDEX, 2, big);
			putv(s1 + 8, spec->sym_value, 8, big);
			putv(s1 + 16, spec->sym_size, 8, big);
		}
	}

	/* Name at the high string-table offset 192. */
	{
		size_t namelen = strlen(spec->name);
		if (namelen + 1 > TASK_STRTAB_SIZE - TASK_NAME_OFFSET)
			return (NULL);
		memcpy(str + TASK_NAME_OFFSET, spec->name, namelen + 1);
	}

	/* SHT_SYMTAB_SHNDX companion: entry 1 names the defining section. */
	putv(idx + 0, 0, 4, big);
	putv(idx + 4, spec->xindex, 4, big);

	/* Relocation 0 against symbol 1, type 2, signed addend. */
	if (c32) {
		putv(rela + 0, TASK_TARGET_OFFSET, 4, big);
		putv(rela + 4, ELF32_R_INFO(TASK_TARGET_SYMBOL,
		    TASK_TARGET_RTYPE), 4, big);
		putv(rela + 8, (unsigned long long)(uint32_t)spec->addend, 4,
		    big);
	} else {
		putv(rela + 0, TASK_TARGET_OFFSET, 8, big);
		putv(rela + 8, ELF64_R_INFO(TASK_TARGET_SYMBOL,
		    TASK_TARGET_RTYPE), 8, big);
		putv(rela + 16, (unsigned long long)spec->addend, 8, big);
	}

	sc[1].logical = text;	sc[1].logical_len = sizeof(text);
	sc[2].logical = sym;	sc[2].logical_len = 2 * symsz;
	sc[3].logical = str;	sc[3].logical_len = sizeof(str);
	sc[4].logical = idx;	sc[4].logical_len = sizeof(idx);
	sc[5].logical = rela;	sc[5].logical_len = relasz;

	sc[1].type = SHT_PROGBITS;
	sc[1].flags = SHF_ALLOC | SHF_EXECINSTR;
	sc[1].align = 16;
	sc[2].type = SHT_SYMTAB;	sc[2].link = 3; sc[2].info = 1;
	sc[2].entsize = (unsigned int)symsz; sc[2].align = c32 ? 4 : 8;
	sc[3].type = SHT_STRTAB;	sc[3].align = 1;
	sc[4].type = SHT_SYMTAB_SHNDX; sc[4].link = 2;
	sc[4].entsize = 4; sc[4].align = 4;
	sc[5].type = SHT_RELA; sc[5].link = 2; sc[5].info = 1;
	sc[5].entsize = (unsigned int)relasz; sc[5].align = c32 ? 4 : 8;

	for (size_t i = 1; i < TASK_FIXTURE_NSCN; ++i) {
		size_t logical_len = sc[i].logical_len;
		const unsigned char *logical = sc[i].logical;

		if (spec->tweak == TF_EMPTY_LOGICAL &&
		    (int)i == spec->tweak_scn) {
			logical = (const unsigned char *)"";
			logical_len = 0;
		} else if (spec->tweak == TF_BAD_MULTIPLE &&
		    (int)i == spec->tweak_scn && logical_len > 1) {
			/* Drop one byte so the record count is not whole. */
			logical_len -= 1;
		}

		if ((spec->compress_mask & (1U << i)) == 0) {
			sc[i].phys = malloc(logical_len ? logical_len : 1);
			if (sc[i].phys == NULL)
				goto fail;
			memcpy(sc[i].phys, logical, logical_len);
			sc[i].phys_len = logical_len;
		} else {
			sc[i].phys = compress_section(logical, logical_len,
			    spec->elfclass, big, sc[i].align, &sc[i].phys_len);
			if (sc[i].phys == NULL)
				goto fail;
		}
	}

	/* Corruption tweaks, applied to the chosen section's physical bytes. */
	if (spec->tweak != TF_NONE && spec->tweak_scn >= 1 &&
	    spec->tweak_scn < TASK_FIXTURE_NSCN) {
		struct build_scn *t = &sc[spec->tweak_scn];
		size_t hdr = c32 ? 12 : 24;

		switch (spec->tweak) {
		case TF_BAD_HDR_TYPE:
			putv(t->phys + 0, 2, 4, big);
			break;
		case TF_BAD_RESERVED:
			if (!c32)
				putv(t->phys + 4, 0x11223344, 4, big);
			break;
		case TF_BAD_ALIGN:
			putv(t->phys + (c32 ? 8 : 16), 3, c32 ? 4 : 8, big);
			break;
		case TF_TRUNC_HEADER:
			t->phys_len = hdr / 2;
			break;
		case TF_CORRUPT_STREAM:
			if (t->phys_len > hdr + 2)
				t->phys[hdr + 2] ^= 0xFF;
			break;
		case TF_TRAILING_BYTE: {
			unsigned char *nb = realloc(t->phys, t->phys_len + 1);
			if (nb == NULL)
				goto fail;
			nb[t->phys_len] = 0x5A;
			t->phys = nb;
			t->phys_len += 1;
			break;
		}
		case TF_SIZE_MISMATCH:
			putv(t->phys + (c32 ? 4 : 8),
			    sc[spec->tweak_scn].logical_len + 1,
			    c32 ? 4 : 8, big);
			break;
		case TF_FLAG_ALLOC:
			t->flags = SHF_COMPRESSED | SHF_ALLOC;
			break;
		case TF_FLAG_NOBITS:
			t->type = SHT_NOBITS;
			t->flags = SHF_COMPRESSED;
			break;
		default:
			break;
		}
	}

	/* Assemble the image: ELF header, section header table, data. */
	shoff = ehsize;
	next = shoff + TASK_FIXTURE_NSCN * shentsize;
	for (size_t i = 1; i < TASK_FIXTURE_NSCN; ++i) {
		next = (next + 15) & ~(size_t)15;
		sc[i].position = next;
		next += sc[i].phys_len;
	}

	b = calloc(1, next);
	if (b == NULL)
		goto fail;

	memcpy(b, "\177ELF", 4);
	b[EI_CLASS] = (unsigned char)spec->elfclass;
	b[EI_DATA] = (unsigned char)spec->data;
	b[EI_VERSION] = EV_CURRENT;
	if (c32) {
		putv(b + 16, ET_REL, 2, big);
		putv(b + 18, EM_386, 2, big);
		putv(b + 20, EV_CURRENT, 4, big);
		putv(b + 24, 0, 4, big);		/* e_entry */
		putv(b + 28, 0, 4, big);		/* e_phoff */
		putv(b + 32, shoff, 4, big);		/* e_shoff */
		putv(b + 36, 0, 4, big);		/* e_flags */
		putv(b + 40, 52, 2, big);		/* e_ehsize */
		putv(b + 42, 0, 2, big);		/* e_phentsize */
		putv(b + 44, 0, 2, big);		/* e_phnum */
		putv(b + 46, 40, 2, big);		/* e_shentsize */
		putv(b + 48, TASK_FIXTURE_NSCN, 2, big);
		putv(b + 50, 0, 2, big);		/* e_shstrndx */
	} else {
		putv(b + 16, ET_REL, 2, big);
		putv(b + 18, EM_X86_64, 2, big);
		putv(b + 20, EV_CURRENT, 4, big);
		putv(b + 24, 0, 8, big);		/* e_entry */
		putv(b + 32, 0, 8, big);		/* e_phoff */
		putv(b + 40, shoff, 8, big);		/* e_shoff */
		putv(b + 48, 0, 4, big);		/* e_flags */
		putv(b + 52, 64, 2, big);		/* e_ehsize */
		putv(b + 54, 0, 2, big);
		putv(b + 56, 0, 2, big);
		putv(b + 58, 64, 2, big);		/* e_shentsize */
		putv(b + 60, TASK_FIXTURE_NSCN, 2, big);
		putv(b + 62, 0, 2, big);
	}

	for (size_t i = 1; i < TASK_FIXTURE_NSCN; ++i) {
		unsigned long long flags;
		unsigned int sh_align;
		unsigned char *h = b + shoff + i * shentsize;

		flags = sc[i].flags;
		if (spec->tweak == TF_FLAG_NOBITS &&
		    (int)i == spec->tweak_scn)
			flags = SHF_COMPRESSED;
		else if (spec->tweak == TF_FLAG_ALLOC &&
		    (int)i == spec->tweak_scn)
			flags = SHF_COMPRESSED | SHF_ALLOC;
		else if (spec->compress_mask & (1U << i))
			flags |= SHF_COMPRESSED;

		/*
		 * The on-disk sh_addralign is not rewritten by the
		 * library; the logical descriptor takes its alignment
		 * from ch_addralign.
		 */
		sh_align = (spec->compress_mask & (1U << i)) ?
		    (c32 ? 4 : 8) : sc[i].align;

		if (c32) {
			putv(h + 0, 0, 4, big);		/* sh_name */
			putv(h + 4, sc[i].type, 4, big);
			putv(h + 8, flags, 4, big);
			putv(h + 12, 0, 4, big);	/* sh_addr */
			putv(h + 16, sc[i].position, 4, big);
			putv(h + 20, sc[i].phys_len, 4, big);
			putv(h + 24, sc[i].link, 4, big);
			putv(h + 28, sc[i].info, 4, big);
			putv(h + 32, sh_align, 4, big);
			putv(h + 36, sc[i].entsize, 4, big);
		} else {
			putv(h + 0, 0, 4, big);
			putv(h + 4, sc[i].type, 4, big);
			putv(h + 8, flags, 8, big);
			putv(h + 16, 0, 8, big);	/* sh_addr */
			putv(h + 24, sc[i].position, 8, big);
			putv(h + 32, sc[i].phys_len, 8, big);
			putv(h + 40, sc[i].link, 4, big);
			putv(h + 44, sc[i].info, 4, big);
			putv(h + 48, sh_align, 8, big);
			putv(h + 56, sc[i].entsize, 8, big);
		}

		memcpy(b + sc[i].position, sc[i].phys, sc[i].phys_len);
	}

	free_sections(sc, TASK_FIXTURE_NSCN);
	*length = next;
	return (b);

fail:
	free_sections(sc, TASK_FIXTURE_NSCN);
	free(b);
	return (NULL);
}

unsigned char *
task_fixture(int compressed, size_t *length)
{
	struct task_fixture_spec spec;

	memset(&spec, 0, sizeof(spec));
	spec.elfclass = ELFCLASS64;
	spec.data = ELFDATA2LSB;
	spec.compress_mask = compressed ?
	    ((1U << 2) | (1U << 3) | (1U << 4) | (1U << 5)) : 0;
	spec.name = "target";
	spec.addend = TASK_TARGET_ADDEND;
	spec.sym_value = TASK_TARGET_VALUE;
	spec.sym_size = TASK_TARGET_SIZE;
	spec.xindex = TASK_TARGET_SECTION;
	return (task_fixture_build(&spec, length));
}
