/*
 * Write-side regression tests for fixed-width tar header generation.
 *
 * A recording write callback counts invocations and captures bytes:
 * invalid headers must fail before the callback fires even once, valid
 * headers are verified field by field against an independent checksum.
 */

#include "microtar.h"

#include <stdio.h>
#include <string.h>

static unsigned char out[8192];
static unsigned outlen;
static unsigned wcalls;
static int failures;

#define CHECK(cond) do {                                                  \
    if (!(cond)) {                                                        \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
      failures++;                                                         \
    }                                                                     \
  } while (0)

static int wr(mtar_t *t, const void *p, unsigned n) {
  (void) t;
  if (n > sizeof out - outlen) {
    return MTAR_EWRITEFAIL;
  }
  memcpy(out + outlen, p, n);
  outlen += n;
  wcalls++;
  return 0;
}

static int rd(mtar_t *t, void *p, unsigned n) {
  if (t->pos > outlen || n > outlen - t->pos) {
    return MTAR_EREADFAIL;
  }
  memcpy(p, out + t->pos, n);
  return 0;
}

static int sk(mtar_t *t, unsigned p) {
  (void) t;
  return p <= outlen ? 0 : MTAR_ESEEKFAIL;
}

static void reset(mtar_t *t) {
  memset(t, 0, sizeof *t);
  t->write = wr;
  t->read = rd;
  t->seek = sk;
  outlen = 0;
  wcalls = 0;
}

/* Independent checksum of a raw header block */
static unsigned sum512(const unsigned char *b) {
  unsigned s = 256;
  unsigned i;
  for (i = 0; i < 148; i++) {
    s += b[i];
  }
  for (i = 156; i < 512; i++) {
    s += b[i];
  }
  return s;
}

static int all_zero(const unsigned char *b, unsigned n) {
  unsigned i;
  for (i = 0; i < n; i++) {
    if (b[i]) {
      return 0;
    }
  }
  return 1;
}

int main(void) {
  mtar_t t;
  mtar_header_t h;
  mtar_header_t back;
  char nm[101];
  int err;

  /* Struct name without a terminator: error before any write */
  reset(&t);
  memset(&h, 0, sizeof h);
  memset(h.name, 'A', sizeof h.name);
  h.mode = 0644;
  err = mtar_write_header(&t, &h);
  CHECK(err < 0 && wcalls == 0 && outlen == 0);

  /* Struct linkname without a terminator: error before any write */
  reset(&t);
  memset(&h, 0, sizeof h);
  strcpy(h.name, "ok");
  memset(h.linkname, 'B', sizeof h.linkname);
  err = mtar_write_header(&t, &h);
  CHECK(err < 0 && wcalls == 0);

  /* 100-byte name through the convenience file/dir writers: rejected
   * before the write callback */
  memset(nm, 'a', 100);
  nm[100] = '\0';
  reset(&t);
  err = mtar_write_file_header(&t, nm, 0);
  CHECK(err < 0 && wcalls == 0);
  reset(&t);
  err = mtar_write_dir_header(&t, nm);
  CHECK(err < 0 && wcalls == 0);

  /* 99-byte name: the longest that fits, must succeed */
  nm[99] = '\0';
  reset(&t);
  err = mtar_write_file_header(&t, nm, 0);
  CHECK(err == MTAR_ESUCCESS && wcalls == 1 && outlen == 512);
  CHECK(!memcmp(out, nm, 99) && out[99] == '\0');

  /* Empty name is representable */
  reset(&t);
  err = mtar_write_file_header(&t, "", 0);
  CHECK(err == MTAR_ESUCCESS && wcalls == 1);

  /* mode just representable in the 8-byte field (7 octal digits) */
  reset(&t);
  memset(&h, 0, sizeof h);
  strcpy(h.name, "m");
  h.mode = 007777777;
  err = mtar_write_header(&t, &h);
  CHECK(err == MTAR_ESUCCESS && wcalls == 1);
  CHECK(!memcmp(out + 100, "7777777", 7) && out[107] == '\0');

  /* mode one past the field capacity: rejected, zero writes */
  reset(&t);
  h.mode = 0100000000;
  err = mtar_write_header(&t, &h);
  CHECK(err < 0 && wcalls == 0 && outlen == 0);

  /* owner one past the field capacity: rejected, zero writes */
  reset(&t);
  h.mode = 0644;
  h.owner = 0100000000;
  err = mtar_write_header(&t, &h);
  CHECK(err < 0 && wcalls == 0);

  /* size at the unsigned limit still fits the 12-byte field */
  reset(&t);
  h.owner = 0;
  h.size = 0xFFFFFFFFu;
  err = mtar_write_header(&t, &h);
  CHECK(err == MTAR_ESUCCESS && wcalls == 1);
  CHECK(!memcmp(out + 124, "37777777777", 11));

  /* A full valid file header, verified field by field */
  reset(&t);
  err = mtar_write_file_header(&t, "hello.txt", 3);
  CHECK(err == MTAR_ESUCCESS && wcalls == 1 && outlen == 512);
  CHECK(!memcmp(out, "hello.txt", 10));
  CHECK(all_zero(out + 10, 90));                 /* rest of name field */
  CHECK(!memcmp(out + 100, "664", 3) && out[103] == '\0');
  CHECK(!memcmp(out + 108, "0", 1) && out[109] == '\0');
  CHECK(all_zero(out + 116, 8));                 /* group unused */
  CHECK(!memcmp(out + 124, "3", 1) && out[125] == '\0');
  CHECK(!memcmp(out + 136, "0", 1) && out[137] == '\0');
  CHECK(out[156] == '0');                        /* regular file */
  CHECK(all_zero(out + 157, 100));               /* empty linkname */
  CHECK(all_zero(out + 257, 255));               /* padding */
  CHECK(out[154] == '\0' && out[155] == ' ');    /* checksum trailer */
  CHECK(sum512(out) == (unsigned) strtoul((char*) out + 148, NULL, 8));

  /* Directory header */
  reset(&t);
  err = mtar_write_dir_header(&t, "somedir");
  CHECK(err == MTAR_ESUCCESS && wcalls == 1 && outlen == 512);
  CHECK(!memcmp(out, "somedir", 8));
  CHECK(!memcmp(out + 100, "775", 3));
  CHECK(out[156] == '5');
  CHECK(sum512(out) == (unsigned) strtoul((char*) out + 148, NULL, 8));

  /* Round trip: written headers read back with the same contents */
  reset(&t);
  err = mtar_write_file_header(&t, "roundtrip.txt", 5);
  CHECK(err == MTAR_ESUCCESS);
  err = mtar_write_dir_header(&t, "dir");
  CHECK(err == MTAR_ESUCCESS);
  CHECK(outlen == 1024);
  t.pos = 0;
  err = mtar_read_header(&t, &back);
  CHECK(err == MTAR_ESUCCESS);
  CHECK(!strcmp(back.name, "roundtrip.txt") && back.size == 5);
  CHECK(back.type == MTAR_TREG && back.mode == 0664);
  CHECK(back.linkname[0] == '\0');
  err = mtar_seek(&t, 512);
  CHECK(err == MTAR_ESUCCESS);
  err = mtar_read_header(&t, &back);
  CHECK(err == MTAR_ESUCCESS);
  CHECK(!strcmp(back.name, "dir") && back.type == MTAR_TDIR);

  /* Round trip with a 99-byte name */
  memset(nm, 'z', 99);
  nm[99] = '\0';
  reset(&t);
  err = mtar_write_file_header(&t, nm, 0);
  CHECK(err == MTAR_ESUCCESS);
  t.pos = 0;
  err = mtar_read_header(&t, &back);
  CHECK(err == MTAR_ESUCCESS && !strcmp(back.name, nm));

  if (failures) {
    printf("header write tests: %d failure(s)\n", failures);
    return 1;
  }
  puts("header write tests PASS");
  return 0;
}
