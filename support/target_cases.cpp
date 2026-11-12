/* Target cases for the single-byte CD sector repair entry points:
 * cdrom_repair_sector() (cdrom.h) and chd_read_cd_repaired() (chd.h).
 *
 * Valid sectors are built with an independent, deliberately naive GF(2^8)
 * reference implementation below - not with ecc_generate() - so the library
 * is never both producing and judging every expectation. ecc_verify() is
 * only used afterwards to cross-check that the reference and the library
 * agree on what a valid sector is. */

#include <libchdr/chd.h>
#include <libchdr/cdrom.h>

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

/***************************************************************************
    INDEPENDENT NAIVE GF(2^8) REFERENCE
***************************************************************************/

/* GF(2^8) multiply, polynomial x^8+x^4+x^3+x^2+1 (0x11d), Russian-peasant */
static uint8_t ref_gf_mul(uint8_t a, uint8_t b)
{
	uint8_t r = 0;
	while (b != 0)
	{
		if (b & 1)
			r ^= a;
		a = (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1d : 0));
		b >>= 1;
	}
	return r;
}

/* parity of one P or Q row: v1 accumulates through xtime, v2 is the plain
 * XOR sum; the stored pair is (0xf4*(2*v1 ^ v2), v2 ^ that) */
static void ref_row_parity(const uint8_t *row, int n, uint8_t *p1, uint8_t *p2)
{
	uint8_t v1 = 0, v2 = 0;
	int i;
	for (i = 0; i < n; i++)
	{
		v1 = ref_gf_mul(2, (uint8_t)(v1 ^ row[i]));
		v2 ^= row[i];
	}
	*p1 = ref_gf_mul(0xf4, (uint8_t)(ref_gf_mul(2, v1) ^ v2));
	*p2 = (uint8_t)(v2 ^ *p1);
}

/* byte at data offset (sector+12 based); Mode 2 reads offsets 0..3 as zero */
static uint8_t ref_data_byte(const uint8_t *s, int off, int mode2)
{
	if (mode2 && off < 4)
		return 0;
	return s[12 + off];
}

/* P rows: offsets r + 86*k (k = 0..23); Q rows: (86*(q>>1) + (q&1) + 88*k)
 * mod 2236 (k = 0..42), computed over data plus the P parity just written */
static void ref_ecc(uint8_t *s, int mode2)
{
	uint8_t row[43];
	int r, q, k;
	for (r = 0; r < 86; r++)
	{
		for (k = 0; k < 24; k++)
			row[k] = ref_data_byte(s, r + 86 * k, mode2);
		ref_row_parity(row, 24, &s[2076 + r], &s[2076 + 86 + r]);
	}
	for (q = 0; q < 52; q++)
	{
		for (k = 0; k < 43; k++)
			row[k] = ref_data_byte(s, (86 * (q >> 1) + (q & 1) + 88 * k) % 2236, mode2);
		ref_row_parity(row, 43, &s[2248 + q], &s[2248 + 52 + q]);
	}
}

/* reflected CRC, polynomial 0xd8018001, initial value 0 */
static uint32_t ref_edc(const uint8_t *p, int n)
{
	uint32_t r = 0;
	int i, b;
	for (i = 0; i < n; i++)
	{
		r ^= p[i];
		for (b = 0; b < 8; b++)
			r = (r >> 1) ^ ((r & 1) ? 0xd8018001u : 0);
	}
	return r;
}

static void ref_put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void ref_sync(uint8_t *s)
{
	s[0] = 0x00;
	memset(s + 1, 0xff, 10);
	s[11] = 0x00;
}

/* a fully valid Mode 1 sector */
static void make_mode1(uint8_t *s, uint8_t seed)
{
	int i;
	memset(s, 0, 2352);
	ref_sync(s);
	s[12] = 0x00; s[13] = 0x02; s[14] = 0x10; /* MSF address */
	s[15] = 0x01;
	for (i = 16; i < 2064; i++)
		s[i] = (uint8_t)(i * 7 + seed);
	ref_put_le32(s + 2064, ref_edc(s, 2064)); /* EDC over 0..2063 */
	/* 2068..2075 reserved, zero */
	ref_ecc(s, 0);
}

/* a fully valid Mode 2 Form 1 sector */
static void make_mode2f1(uint8_t *s, uint8_t seed)
{
	int i;
	memset(s, 0, 2352);
	ref_sync(s);
	s[12] = 0x00; s[13] = 0x02; s[14] = 0x11; /* MSF address */
	s[15] = 0x02;
	/* duplicated subheader, submode with the Form 2 bit clear */
	s[16] = s[20] = 0x00;
	s[17] = s[21] = 0x01;
	s[18] = s[22] = 0x08;
	s[19] = s[23] = 0x00;
	for (i = 24; i < 2072; i++)
		s[i] = (uint8_t)(i * 5 + seed);
	ref_put_le32(s + 2072, ref_edc(s + 16, 2056)); /* EDC over 16..2071 */
	ref_ecc(s, 1);
}

static uint8_t nonzero_delta(int off)
{
	uint8_t d = (uint8_t)(off * 167 + 91);
	return d ? d : (uint8_t)0x5a;
}

/* repair one sector and require a full restore */
static void check_repaired(const uint8_t *good, int kind, int off, uint8_t delta)
{
	uint8_t s[2352];
	int corrected = -2;
	int st;
	memcpy(s, good, 2352);
	s[off] ^= delta;
	st = cdrom_repair_sector(s, kind, &corrected);
	FB_CHECK(st == CD_REPAIR_CORRECTED);
	FB_CHECK(corrected == off);
	FB_CHECK(memcmp(s, good, 2352) == 0);
}

/***************************************************************************
    syndrome: located and solved from the P/Q syndromes
***************************************************************************/

FB_TEST("syndrome",mode1_every_offset)
{
	uint8_t good[2352];
	int off;
	make_mode1(good, 0x11);
	FB_CHECK(ecc_verify(good)); /* reference and library agree */
	for (off = 16; off < 2352; off++)
		check_repaired(good, CD_TRACK_MODE1_RAW, off, nonzero_delta(off));
}

FB_TEST("syndrome",mode2_every_offset)
{
	uint8_t good[2352];
	int off;
	make_mode2f1(good, 0x22);
	FB_CHECK(ecc_verify(good));
	for (off = 16; off < 2352; off++)
		check_repaired(good, CD_TRACK_MODE2_FORM1, off, nonzero_delta(off));
}

FB_TEST("syndrome",every_delta_spot_offsets)
{
	uint8_t good1[2352], good2[2352];
	static const int spots[] = { 16, 17, 1000, 2063, 2064, 2068, 2075, 2076, 2162, 2247, 2248, 2300, 2351 };
	unsigned i;
	int d;
	make_mode1(good1, 0x33);
	make_mode2f1(good2, 0x44);
	for (i = 0; i < sizeof(spots) / sizeof(spots[0]); i++)
		for (d = 1; d < 256; d++)
		{
			check_repaired(good1, CD_TRACK_MODE1_RAW, spots[i], (uint8_t)d);
			check_repaired(good2, CD_TRACK_MODE2_FORM1, spots[i], (uint8_t)d);
		}
}

/***************************************************************************
    sector_modes: per-kind behaviour of cdrom_repair_sector
***************************************************************************/

FB_TEST("sector_modes",unchanged_sectors)
{
	uint8_t good1[2352], good2[2352], s[2352];
	int corrected;
	make_mode1(good1, 0x55);
	make_mode2f1(good2, 0x66);

	corrected = 0;
	memcpy(s, good1, 2352);
	FB_CHECK(cdrom_repair_sector(s, CD_TRACK_MODE1_RAW, &corrected) == CD_REPAIR_UNCHANGED);
	FB_CHECK(corrected == -1);
	FB_CHECK(memcmp(s, good1, 2352) == 0);

	corrected = 0;
	memcpy(s, good2, 2352);
	FB_CHECK(cdrom_repair_sector(s, CD_TRACK_MODE2_FORM1, &corrected) == CD_REPAIR_UNCHANGED);
	FB_CHECK(corrected == -1);
	FB_CHECK(memcmp(s, good2, 2352) == 0);
}

FB_TEST("sector_modes",mode2_header_and_subheader)
{
	uint8_t good[2352], s[2352];
	int corrected;
	make_mode2f1(good, 0x77);

	/* Mode 2 header address bytes (12..14) are not protected data: the ECC
	 * reads them as zero and the EDC starts at 16, so damage there is
	 * invisible and must simply come back unchanged */
	memcpy(s, good, 2352);
	s[12] ^= 0x01;
	s[13] ^= 0x20;
	s[14] ^= 0xff;
	corrected = 0;
	FB_CHECK(cdrom_repair_sector(s, CD_TRACK_MODE2_FORM1, &corrected) == CD_REPAIR_UNCHANGED);
	FB_CHECK(corrected == -1);

	/* subheader bytes (16..23) are protected: a damaged submode byte is
	 * located and repaired */
	check_repaired(good, CD_TRACK_MODE2_FORM1, 18, 0x24);
	check_repaired(good, CD_TRACK_MODE2_FORM1, 21, 0x81);

	/* the mode byte itself (15) is outside the repairable range */
	memcpy(s, good, 2352);
	s[15] = 0x01;
	corrected = 0;
	FB_CHECK(cdrom_repair_sector(s, CD_TRACK_MODE2_FORM1, &corrected) == CD_REPAIR_UNCORRECTABLE);
	FB_CHECK(corrected == -1);
	FB_CHECK(s[15] == 0x01);
}

FB_TEST("sector_modes",skipped_kinds_keep_bytes)
{
	uint8_t good[2352], s[2352], audio[2352];
	static const int kinds[] = {
		CD_TRACK_MODE1, CD_TRACK_MODE2, CD_TRACK_MODE2_FORM2,
		CD_TRACK_MODE2_FORM_MIX, CD_TRACK_MODE2_RAW, CD_TRACK_AUDIO,
		CD_TRACK_RAW_DONTCARE
	};
	unsigned i;
	int corrected;
	make_mode1(good, 0x88);

	for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
	{
		memcpy(s, good, 2352);
		s[500] ^= 0x5a; /* even a damaged sector is only skipped */
		corrected = 0;
		FB_CHECK(cdrom_repair_sector(s, kinds[i], &corrected) == CD_REPAIR_SKIPPED);
		FB_CHECK(corrected == -1);
		FB_CHECK(memcmp(s, good, 2352) != 0); /* the damage is still there */
	}

	/* an audio frame whose body happens to start with the sync pattern is
	 * still audio: it must not be treated as a data sector */
	for (i = 0; i < 2352; i++)
		audio[i] = (uint8_t)(i * 31 + 7);
	ref_sync(audio);
	corrected = 0;
	FB_CHECK(cdrom_repair_sector(audio, CD_TRACK_AUDIO, &corrected) == CD_REPAIR_SKIPPED);
	FB_CHECK(corrected == -1);
	FB_CHECK(audio[500] == (uint8_t)(500 * 31 + 7));
}

FB_TEST("sector_modes",offset_pointer_may_be_null)
{
	uint8_t good[2352], s[2352];
	make_mode1(good, 0x99);
	memcpy(s, good, 2352);
	s[1234] ^= 0xa5;
	FB_CHECK(cdrom_repair_sector(s, CD_TRACK_MODE1_RAW, NULL) == CD_REPAIR_CORRECTED);
	FB_CHECK(memcmp(s, good, 2352) == 0);
}

/***************************************************************************
    hunk: chd_read_cd_repaired over a real uncompressed CHDv5
***************************************************************************/

static void put_be32(std::vector<uint8_t> &v, size_t off, uint32_t x)
{
	v[off + 0] = (uint8_t)(x >> 24);
	v[off + 1] = (uint8_t)(x >> 16);
	v[off + 2] = (uint8_t)(x >> 8);
	v[off + 3] = (uint8_t)x;
}

static void put_be64(std::vector<uint8_t> &v, size_t off, uint64_t x)
{
	int i;
	for (i = 0; i < 8; i++)
		v[off + i] = (uint8_t)(x >> (56 - 8 * i));
}

/* an uncompressed CHDv5 image holding exactly one hunk */
static std::vector<uint8_t> build_chd_v5(const uint8_t *hunk, uint32_t hunkbytes, uint32_t unitbytes)
{
	std::vector<uint8_t> img(2 * (size_t)hunkbytes, 0);
	memcpy(&img[0], "MComprHD", 8);
	put_be32(img, 8, 124);              /* header length */
	put_be32(img, 12, 5);               /* version */
	/* 16..31 compressors: all zero = uncompressed */
	put_be64(img, 32, hunkbytes);       /* logicalbytes: one hunk */
	put_be64(img, 40, 124);             /* mapoffset */
	put_be64(img, 48, 0);               /* metaoffset */
	put_be32(img, 56, hunkbytes);
	put_be32(img, 60, unitbytes);
	put_be32(img, 124, 1);              /* map: hunk data at 1 * hunkbytes */
	memcpy(&img[hunkbytes], hunk, hunkbytes);
	return img;
}

static chd_file *open_image(const std::vector<uint8_t> &img, FILE **outf)
{
	FILE *f = tmpfile();
	chd_file *c = NULL;
	FB_CHECK(f != NULL);
	FB_CHECK(fwrite(&img[0], 1, img.size(), f) == img.size());
	fflush(f);
	FB_CHECK(chd_open_file(f, CHD_OPEN_READ, NULL, &c) == CHDERR_NONE);
	*outf = f;
	return c;
}

FB_TEST("hunk",read_cd_repaired)
{
	uint8_t good0[2352], audio[2352], good2[2352], good3[2352];
	std::vector<uint8_t> hunk(4 * 2448);
	uint8_t kinds[4] = { CD_TRACK_MODE1_RAW, CD_TRACK_AUDIO, CD_TRACK_MODE2_FORM1, CD_TRACK_MODE1_RAW };
	int statuses[4];
	std::vector<uint8_t> img, plain;
	FILE *f;
	chd_file *c;
	unsigned i;
	chd_error err;

	make_mode1(good0, 0x01);
	for (i = 0; i < 2352; i++)
		audio[i] = (uint8_t)(i * 13 + i / 9 + 3);
	ref_sync(audio); /* audio frame masquerading as a data sector */
	make_mode2f1(good2, 0x02);
	make_mode1(good3, 0x03);

	/* frame 0: one damaged data byte; frame 1: audio, untouched;
	 * frame 2: one damaged Q parity byte; frame 3: two damaged bytes */
	memcpy(&hunk[0 * 2448], good0, 2352);
	hunk[0 * 2448 + 500] ^= 0x77;
	memcpy(&hunk[1 * 2448], audio, 2352);
	memcpy(&hunk[2 * 2448], good2, 2352);
	hunk[2 * 2448 + 2351] ^= 0x81;
	memcpy(&hunk[3 * 2448], good3, 2352);
	hunk[3 * 2448 + 40] ^= 0x01;
	hunk[3 * 2448 + 2000] ^= 0x02;
	for (i = 0; i < 4; i++)
	{
		unsigned j;
		for (j = 2352; j < 2448; j++)
			hunk[i * 2448 + j] = (uint8_t)(i * 7 + j);
	}

	img = build_chd_v5(&hunk[0], 4 * 2448, 2448);
	c = open_image(img, &f);

	plain.resize(4 * 2448);
	err = chd_read_cd_repaired(c, 0, &plain[0], kinds, 4, statuses);
	FB_CHECK(err == CHDERR_NONE);
	FB_CHECK(statuses[0] == CD_REPAIR_CORRECTED);
	FB_CHECK(statuses[1] == CD_REPAIR_SKIPPED);
	FB_CHECK(statuses[2] == CD_REPAIR_CORRECTED);
	FB_CHECK(statuses[3] == CD_REPAIR_UNCORRECTABLE);

	/* repaired slots carry the restored sectors; the unrepairable slot and
	 * the audio slot carry their original bytes */
	FB_CHECK(memcmp(&plain[0 * 2448], good0, 2352) == 0);
	FB_CHECK(memcmp(&plain[1 * 2448], audio, 2352) == 0);
	FB_CHECK(memcmp(&plain[2 * 2448], good2, 2352) == 0);
	FB_CHECK(memcmp(&plain[3 * 2448], &hunk[3 * 2448], 2352) == 0);
	/* subchannel areas are never touched */
	for (i = 0; i < 4; i++)
		FB_CHECK(memcmp(&plain[i * 2448 + 2352], &hunk[i * 2448 + 2352], 96) == 0);

	/* the repair is not written back: a plain chd_read still returns the
	 * original damaged hunk, byte for byte */
	{
		std::vector<uint8_t> again(4 * 2448);
		FB_CHECK(chd_read(c, 0, &again[0]) == CHDERR_NONE);
		FB_CHECK(memcmp(&again[0], &hunk[0], 4 * 2448) == 0);
	}

	chd_close(c);
	fclose(f);
}

FB_TEST("hunk",parameter_checks)
{
	uint8_t good[2352];
	std::vector<uint8_t> hunk(2 * 2448, 0);
	uint8_t kinds2[2] = { CD_TRACK_MODE1_RAW, CD_TRACK_MODE1_RAW };
	uint8_t kinds4[4] = { CD_TRACK_MODE1_RAW, CD_TRACK_MODE1_RAW, CD_TRACK_MODE1_RAW, CD_TRACK_MODE1_RAW };
	int statuses[4];
	std::vector<uint8_t> img, buf(2 * 2448);
	FILE *f, *f2, *f3;
	chd_file *c, *c2, *c3;

	make_mode1(good, 0x0a);
	memcpy(&hunk[0], good, 2352);
	memcpy(&hunk[2448], good, 2352);
	img = build_chd_v5(&hunk[0], 2 * 2448, 2448);
	c = open_image(img, &f);

	/* count must equal the number of physical frame slots */
	FB_CHECK(chd_read_cd_repaired(c, 0, &buf[0], kinds4, 4, statuses) == CHDERR_INVALID_PARAMETER);
	FB_CHECK(chd_read_cd_repaired(c, 0, &buf[0], kinds2, 1, statuses) == CHDERR_INVALID_PARAMETER);
	/* NULL arguments */
	FB_CHECK(chd_read_cd_repaired(c, 0, NULL, kinds2, 2, statuses) == CHDERR_INVALID_PARAMETER);
	FB_CHECK(chd_read_cd_repaired(c, 0, &buf[0], NULL, 2, statuses) == CHDERR_INVALID_PARAMETER);
	FB_CHECK(chd_read_cd_repaired(c, 0, &buf[0], kinds2, 2, NULL) == CHDERR_INVALID_PARAMETER);
	FB_CHECK(chd_read_cd_repaired(NULL, 0, &buf[0], kinds2, 2, statuses) == CHDERR_INVALID_PARAMETER);
	/* out-of-range hunk comes back from the underlying read path */
	FB_CHECK(chd_read_cd_repaired(c, 1, &buf[0], kinds2, 2, statuses) == CHDERR_HUNK_OUT_OF_RANGE);
	/* a well-formed call on the same image succeeds */
	FB_CHECK(chd_read_cd_repaired(c, 0, &buf[0], kinds2, 2, statuses) == CHDERR_NONE);
	FB_CHECK(statuses[0] == CD_REPAIR_UNCHANGED && statuses[1] == CD_REPAIR_UNCHANGED);
	chd_close(c);
	fclose(f);

	/* unitbytes other than 2448 is rejected */
	{
		std::vector<uint8_t> bad = build_chd_v5(&hunk[0], 2352, 2352);
		std::vector<uint8_t> bbuf(2352);
		c2 = open_image(bad, &f2);
		FB_CHECK(chd_read_cd_repaired(c2, 0, &bbuf[0], kinds2, 1, statuses) == CHDERR_INVALID_PARAMETER);
		chd_close(c2);
		fclose(f2);
	}

	/* a hunk size that is not a multiple of 2448 is rejected */
	{
		std::vector<uint8_t> bad = build_chd_v5(&hunk[0], 2500, 2448);
		std::vector<uint8_t> bbuf(2500);
		c3 = open_image(bad, &f3);
		FB_CHECK(chd_read_cd_repaired(c3, 0, &bbuf[0], kinds2, 1, statuses) == CHDERR_INVALID_PARAMETER);
		chd_close(c3);
		fclose(f3);
	}
}

/***************************************************************************
    uncorrectable: material with no unique single-byte fix
***************************************************************************/

FB_TEST("uncorrectable",two_byte_damage)
{
	uint8_t good[2352], s[2352], before[2352];
	int corrected;
	make_mode1(good, 0xbb);

	/* two damaged data bytes */
	memcpy(s, good, 2352);
	s[100] ^= 0x10;
	s[2000] ^= 0x20;
	memcpy(before, s, 2352);
	corrected = 0;
	FB_CHECK(cdrom_repair_sector(s, CD_TRACK_MODE1_RAW, &corrected) == CD_REPAIR_UNCORRECTABLE);
	FB_CHECK(corrected == -1);
	FB_CHECK(memcmp(s, before, 2352) == 0);

	/* two damaged bytes sharing one P row */
	memcpy(s, good, 2352);
	s[16] ^= 0x01;
	s[16 + 86] ^= 0x02;
	memcpy(before, s, 2352);
	FB_CHECK(cdrom_repair_sector(s, CD_TRACK_MODE1_RAW, &corrected) == CD_REPAIR_UNCORRECTABLE);
	FB_CHECK(memcmp(s, before, 2352) == 0);

	/* both Q parity bytes of one row */
	memcpy(s, good, 2352);
	s[2248 + 10] ^= 0x40;
	s[2248 + 52 + 10] ^= 0x80;
	memcpy(before, s, 2352);
	FB_CHECK(cdrom_repair_sector(s, CD_TRACK_MODE1_RAW, &corrected) == CD_REPAIR_UNCORRECTABLE);
	FB_CHECK(memcmp(s, before, 2352) == 0);

	/* one data byte plus one P parity byte */
	make_mode2f1(good, 0xcc);
	memcpy(s, good, 2352);
	s[300] ^= 0x08;
	s[2100] ^= 0x04;
	memcpy(before, s, 2352);
	FB_CHECK(cdrom_repair_sector(s, CD_TRACK_MODE2_FORM1, &corrected) == CD_REPAIR_UNCORRECTABLE);
	FB_CHECK(memcmp(s, before, 2352) == 0);
}

FB_TEST("uncorrectable",header_bytes_out_of_range)
{
	uint8_t good[2352], s[2352], before[2352];
	int corrected;
	make_mode1(good, 0xdd);

	/* sync pattern damage: EDC sees it, but it is not repairable */
	memcpy(s, good, 2352);
	s[5] ^= 0xff;
	memcpy(before, s, 2352);
	corrected = 0;
	FB_CHECK(cdrom_repair_sector(s, CD_TRACK_MODE1_RAW, &corrected) == CD_REPAIR_UNCORRECTABLE);
	FB_CHECK(corrected == -1);
	FB_CHECK(memcmp(s, before, 2352) == 0);

	/* Mode 1 address bytes are protected by EDC/ECC but sit at offsets
	 * 12..14, outside the repairable range */
	memcpy(s, good, 2352);
	s[13] ^= 0x01;
	memcpy(before, s, 2352);
	FB_CHECK(cdrom_repair_sector(s, CD_TRACK_MODE1_RAW, &corrected) == CD_REPAIR_UNCORRECTABLE);
	FB_CHECK(memcmp(s, before, 2352) == 0);

	/* the mode byte */
	memcpy(s, good, 2352);
	s[15] = 0x02;
	memcpy(before, s, 2352);
	FB_CHECK(cdrom_repair_sector(s, CD_TRACK_MODE1_RAW, &corrected) == CD_REPAIR_UNCORRECTABLE);
	FB_CHECK(memcmp(s, before, 2352) == 0);
}

FB_TEST("uncorrectable",garbage_sectors)
{
	uint8_t s[2352], before[2352];
	unsigned i;
	int corrected;

	/* correct sync and mode byte, garbage everywhere else: the syndromes
	 * point at nothing consistent */
	for (i = 12; i < 2352; i++)
		s[i] = (uint8_t)(i * 109 + 41);
	ref_sync(s);
	s[15] = 0x01;
	memcpy(before, s, 2352);
	corrected = 0;
	FB_CHECK(cdrom_repair_sector(s, CD_TRACK_MODE1_RAW, &corrected) == CD_REPAIR_UNCORRECTABLE);
	FB_CHECK(corrected == -1);
	FB_CHECK(memcmp(s, before, 2352) == 0);

	/* a Mode 2 sector whose subheader copies disagree cannot be passed off
	 * as repaired by regenerating anything: only a genuine single-byte fix
	 * is accepted, and this one needs two */
	make_mode2f1(s, 0xee);
	s[18] ^= 0x04; /* one copy of the submode byte only... */
	s[19] ^= 0x01; /* ...plus a second damaged byte */
	memcpy(before, s, 2352);
	FB_CHECK(cdrom_repair_sector(s, CD_TRACK_MODE2_FORM1, &corrected) == CD_REPAIR_UNCORRECTABLE);
	FB_CHECK(memcmp(s, before, 2352) == 0);
}
