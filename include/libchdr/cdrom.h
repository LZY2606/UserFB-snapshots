/* license:BSD-3-Clause
 * copyright-holders:Aaron Giles
***************************************************************************

    cdrom.h

    Generic MAME cd-rom implementation

***************************************************************************/

#pragma once

#ifndef __CDROM_H__
#define __CDROM_H__

#include <stdint.h>
#include "chd.h"
#include "chdconfig.h"
#include "macros.h"

#ifdef __cplusplus
extern "C" {
#endif

/***************************************************************************
    CONSTANTS
***************************************************************************/

/* tracks are padded to a multiple of this many frames */
#define CD_TRACK_PADDING   	(4)
#define CD_MAX_TRACKS           (99)    /* AFAIK the theoretical limit */
#define CD_MAX_SECTOR_DATA      (2352)
#define CD_MAX_SUBCODE_DATA     (96)

#define CD_FRAME_SIZE           (CD_MAX_SECTOR_DATA + CD_MAX_SUBCODE_DATA)
#define CD_FRAMES_PER_HUNK      (8)

#define CD_METADATA_WORDS       (1+(CD_MAX_TRACKS * 6))

enum
{
	CD_TRACK_MODE1 = 0,         /* mode 1 2048 bytes/sector */
	CD_TRACK_MODE1_RAW,         /* mode 1 2352 bytes/sector */
	CD_TRACK_MODE2,             /* mode 2 2336 bytes/sector */
	CD_TRACK_MODE2_FORM1,       /* mode 2 2048 bytes/sector */
	CD_TRACK_MODE2_FORM2,       /* mode 2 2324 bytes/sector */
	CD_TRACK_MODE2_FORM_MIX,    /* mode 2 2336 bytes/sector */
	CD_TRACK_MODE2_RAW,         /* mode 2 2352 bytes / sector */
	CD_TRACK_AUDIO,         /* redbook audio track 2352 bytes/sector (588 samples) */

	CD_TRACK_RAW_DONTCARE       /* special flag for cdrom_read_data: just return me whatever is there */
};

enum
{
	CD_SUB_NORMAL = 0,          /* "cooked" 96 bytes per sector */
	CD_SUB_RAW,                 /* raw uninterleaved 96 bytes per sector */
	CD_SUB_NONE                 /* no subcode data stored */
};

#define CD_FLAG_GDROM   0x00000001  /* disc is a GD-ROM, all tracks should be stored with GD-ROM metadata */
#define CD_FLAG_GDROMLE 0x00000002  /* legacy GD-ROM, with little-endian CDDA data */

/***************************************************************************
    FUNCTION PROTOTYPES
***************************************************************************/

#if WANT_RAW_DATA_SECTOR
/* ECC utilities */
int ecc_verify(const uint8_t *sector);
void ecc_generate(uint8_t *sector);
void ecc_clear(uint8_t *sector);

/* result of cdrom_repair_sector() */
enum
{
	CD_REPAIR_UNCHANGED = 0,    /* sector already passed every check; nothing touched */
	CD_REPAIR_CORRECTED,        /* exactly one byte was corrected; see corrected_offset */
	CD_REPAIR_UNCORRECTABLE,    /* no unique single-byte fix exists; sector left untouched */
	CD_REPAIR_SKIPPED           /* kind carries no repairable EDC/ECC; sector left untouched */
};

/* Attempt to repair a single damaged byte in a raw 2352-byte sector.
 *
 * `kind` is one of the CD_TRACK_* constants above. Only CD_TRACK_MODE1_RAW
 * and CD_TRACK_MODE2_FORM1 carry repairable EDC + P/Q ECC; every other kind
 * (audio, form 2, cooked modes, ...) returns CD_REPAIR_SKIPPED with the
 * sector byte-for-byte unchanged, no matter what its contents look like.
 *
 * A damaged byte is located from the P/Q syndromes and the error delta is
 * solved for arithmetically - no trial-and-error over offsets or values -
 * and the candidate is accepted only if the whole sector then passes every
 * check (EDC, P/Q ECC, and for Mode 2 Form 1 the duplicated subheader with
 * the Form 2 bit clear). Any single byte in offsets 16..2351 (user data,
 * EDC, reserved area, P or Q parity) can be repaired, with a delta of any
 * number of bits. The sync pattern, address and mode bytes (0..15) are
 * outside the repairable range. If no unique single-byte fix exists the
 * sector is returned unmodified and the result is CD_REPAIR_UNCORRECTABLE.
 *
 * `corrected_offset` may be NULL; when non-NULL it receives the 0-based
 * offset of the modified byte on CD_REPAIR_CORRECTED and -1 otherwise. */
int cdrom_repair_sector(uint8_t *sector, int kind, int *corrected_offset);
#endif

chd_error cd_codec_decompress(
	uint8_t *scratch,
	void *base_decompressor, chd_codec_interface_decompress base_decompress,
	void *subcode_decompressor, chd_codec_interface_decompress subcode_decompress,
	const uint8_t *src, uint32_t complen, uint8_t *dest, uint32_t destlen);


/***************************************************************************
    INLINE FUNCTIONS
***************************************************************************/

static CHDR_INLINE uint32_t msf_to_lba(uint32_t msf)
{
	return ( ((msf&0x00ff0000)>>16) * 60 * 75) + (((msf&0x0000ff00)>>8) * 75) + ((msf&0x000000ff)>>0);
}

static CHDR_INLINE uint32_t lba_to_msf(uint32_t lba)
{
	uint8_t m, s, f;

	m = lba / (60 * 75);
	lba -= m * (60 * 75);
	s = lba / 75;
	f = lba % 75;

	return ((m / 10) << 20) | ((m % 10) << 16) |
			((s / 10) << 12) | ((s % 10) <<  8) |
			((f / 10) <<  4) | ((f % 10) <<  0);
}

/**
 * segacd needs it like this.. investigate
 * Angelo also says PCE tracks often start playing at the
 * wrong address.. related?
 **/
static CHDR_INLINE uint32_t lba_to_msf_alt(int lba)
{
	uint32_t ret = 0;

	ret |= ((lba / (60 * 75))&0xff)<<16;
	ret |= (((lba / 75) % 60)&0xff)<<8;
	ret |= ((lba % 75)&0xff)<<0;

	return ret;
}

#ifdef __cplusplus
}
#endif

#endif  /* __CDROM_H__ */
