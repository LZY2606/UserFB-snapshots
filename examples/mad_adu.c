/*
 * libmad - ADU pack/decode example
 *
 * Usage:
 *   mad_adu pack   <input.mp3>  <output.adu>
 *   mad_adu decode <input.adu>  <output.pcm>
 *
 * pack reads a tag-free MPEG-1/2 Layer III file and writes one record per
 * ADU: a big-endian 32-bit byte length followed by the ADU bytes.
 *
 * decode reads those records and writes interleaved 16-bit signed
 * little-endian PCM (one frame's samples per ADU, channel order as
 * decoded; mono input produces one sample per position).
 *
 * MAD samples are signed Q28 fixed-point values in [-1, 1).  Conversion
 * to 16 bits adds 2^(28-16) for rounding, clips to [-1, 1) and shifts
 * right by 28+1-16 = 13 bits; no dithering or noise shaping is applied.
 */

# include <stdio.h>
# include <stdlib.h>
# include <string.h>

# include "mad.h"

# define CHUNK 1024

struct record_out {
  FILE *fp;
  int failed;
};

static
int emit_record(void *user, unsigned char const *adu, unsigned long length)
{
  struct record_out *out = user;
  unsigned char hdr[4];

  if (length > 0xffffffffUL) {
    out->failed = 1;
    return -1;
  }

  hdr[0] = (unsigned char) (length >> 24);
  hdr[1] = (unsigned char) (length >> 16);
  hdr[2] = (unsigned char) (length >> 8);
  hdr[3] = (unsigned char) (length);

  if (fwrite(hdr, 1, 4, out->fp) != 4 ||
      fwrite(adu, 1, length, out->fp) != length) {
    out->failed = 1;
    return -1;
  }

  return 0;
}

static
int do_pack(char const *in_path, char const *out_path)
{
  FILE *in, *out;
  struct mad_adu_packer *packer;
  struct record_out rec;
  unsigned char chunk[CHUNK];
  size_t n;
  int rc = 1;

  in = fopen(in_path, "rb");
  if (in == 0) {
    perror(in_path);
    return 1;
  }

  out = fopen(out_path, "wb");
  if (out == 0) {
    perror(out_path);
    fclose(in);
    return 1;
  }

  rec.fp = out;
  rec.failed = 0;

  packer = mad_adu_packer_new(emit_record, &rec);
  if (packer == 0) {
    fprintf(stderr, "mad_adu_packer_new: out of memory\n");
    goto out_files;
  }

  while ((n = fread(chunk, 1, sizeof chunk, in)) > 0) {
    if (mad_adu_packer_feed(packer, chunk, (unsigned long) n) == -1) {
      fprintf(stderr, "pack: feed failed at byte %ld\n",
	      (long) ftell(in));
      goto out_packer;
    }
  }

  if (ferror(in)) {
    perror(in_path);
    goto out_packer;
  }

  if (mad_adu_packer_finish(packer) == -1 || rec.failed) {
    fprintf(stderr, "pack: finish failed (truncated or invalid stream)\n");
    goto out_packer;
  }

  rc = 0;

 out_packer:
  mad_adu_packer_free(packer);
 out_files:
  if (fclose(out) != 0 && rc == 0) {
    perror(out_path);
    rc = 1;
  }
  fclose(in);
  return rc;
}

static
signed int scale_pcm(mad_fixed_t sample)
{
  /* round (Q28 -> 16 bit) */
  sample += (1L << (MAD_F_FRACBITS - 16));

  /* clip to [-1, 1) */
  if (sample >= MAD_F_ONE)
    sample = MAD_F_ONE - 1;
  else if (sample < -MAD_F_ONE)
    sample = -MAD_F_ONE;

  /* quantize */
  return sample >> (MAD_F_FRACBITS + 1 - 16);
}

static
int write_le16(FILE *fp, signed int value)
{
  unsigned char b[2];

  b[0] = (unsigned char) (value & 0xff);
  b[1] = (unsigned char) ((value >> 8) & 0xff);

  return fwrite(b, 1, 2, fp) == 2;
}

static
int do_decode(char const *in_path, char const *out_path)
{
  FILE *in, *out;
  struct mad_stream stream;
  struct mad_frame frame;
  struct mad_synth synth;
  unsigned char hdr[4];
  unsigned char *adu = 0;
  unsigned long adu_cap = 0;
  int rc = 1;

  in = fopen(in_path, "rb");
  if (in == 0) {
    perror(in_path);
    return 1;
  }

  out = fopen(out_path, "wb");
  if (out == 0) {
    perror(out_path);
    fclose(in);
    return 1;
  }

  mad_stream_init(&stream);
  mad_frame_init(&frame);
  mad_synth_init(&synth);

  while (fread(hdr, 1, 4, in) == 4) {
    unsigned long length;
    unsigned int ch, i;

    length = ((unsigned long) hdr[0] << 24) |
	     ((unsigned long) hdr[1] << 16) |
	     ((unsigned long) hdr[2] << 8) |
	     (unsigned long) hdr[3];

    if (length > adu_cap) {
      unsigned char *next = realloc(adu, length ? length : 1);
      if (next == 0) {
	fprintf(stderr, "decode: out of memory\n");
	goto done;
      }
      adu = next;
      adu_cap = length;
    }

    if (fread(adu, 1, length, in) != length) {
      fprintf(stderr, "decode: truncated record\n");
      goto done;
    }

    if (mad_frame_decode_adu(&frame, &stream, adu, length) == -1) {
      fprintf(stderr, "decode: bad ADU: %s\n",
	      mad_stream_errorstr(&stream));
      goto done;
    }

    mad_synth_frame(&synth, &frame);

    for (i = 0; i < synth.pcm.length; ++i) {
      for (ch = 0; ch < synth.pcm.channels; ++ch) {
	if (!write_le16(out, scale_pcm(synth.pcm.samples[ch][i]))) {
	  perror(out_path);
	  goto done;
	}
      }
    }
  }

  if (ferror(in)) {
    perror(in_path);
    goto done;
  }
  if (!feof(in)) {
    fprintf(stderr, "decode: truncated length prefix\n");
    goto done;
  }

  rc = 0;

 done:
  free(adu);
  mad_synth_finish(&synth);
  mad_frame_finish(&frame);
  mad_stream_finish(&stream);
  if (fclose(out) != 0 && rc == 0) {
    perror(out_path);
    rc = 1;
  }
  fclose(in);
  return rc;
}

int main(int argc, char *argv[])
{
  if (argc != 4) {
    fprintf(stderr,
	    "usage: %s pack <input.mp3> <output.adu>\n"
	    "       %s decode <input.adu> <output.pcm>\n",
	    argv[0], argv[0]);
    return 2;
  }

  if (strcmp(argv[1], "pack") == 0)
    return do_pack(argv[2], argv[3]);

  if (strcmp(argv[1], "decode") == 0)
    return do_decode(argv[2], argv[3]);

  fprintf(stderr, "unknown subcommand: %s\n", argv[1]);
  return 2;
}
