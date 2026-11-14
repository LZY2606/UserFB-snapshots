/*
 * tests/patch_source.c
 *
 * Tiny literal replacement helper used by
 * tests/check_closed_loop_mutants.sh.  It edits one throw-away file in
 * the temporary source copy (never the original tree), replacing every
 * occurrence of a literal byte string and reporting how many were
 * replaced.  Kept in C so the mutation check needs only a system
 * compiler and the standard library, with no sed/awk/perl assumptions.
 *
 * Usage: patch_source FILE OLD NEW
 * Exit:  0 on success (at least one replacement), 1 otherwise.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *path, size_t *len_out)
{
	FILE *f = fopen(path, "rb");
	long n;
	char *buf;

	if (!f) return NULL;
	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
	n = ftell(f);
	if (n < 0) { fclose(f); return NULL; }
	if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
	buf = (char *)malloc((size_t)n + 1);
	if (!buf) { fclose(f); return NULL; }
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		fclose(f);
		return NULL;
	}
	buf[n] = '\0';
	fclose(f);
	*len_out = (size_t)n;
	return buf;
}

int main(int argc, char **argv)
{
	const char *path, *old, *new;
	size_t len, oldlen, newlen;
	char *src, *dst;
	size_t i, di, count = 0;
	FILE *f;

	if (argc != 4) {
		fprintf(stderr, "usage: %s FILE OLD NEW\n", argv[0]);
		return 1;
	}
	path = argv[1];
	old  = argv[2];
	new  = argv[3];
	oldlen = strlen(old);
	newlen = strlen(new);
	if (oldlen == 0) {
		fprintf(stderr, "patch_source: empty search string\n");
		return 1;
	}

	src = slurp(path, &len);
	if (!src) {
		fprintf(stderr, "patch_source: cannot read %s\n", path);
		return 1;
	}

	/* Replacements only ever shrink or grow by a small, known amount,
	 * so size the result conservatively. */
	dst = (char *)malloc(len * (newlen > oldlen ? newlen : oldlen)
			     / (oldlen ? oldlen : 1) + len + 1);
	if (!dst) {
		free(src);
		return 1;
	}

	di = 0;
	for (i = 0; i + oldlen <= len; ) {
		if (memcmp(src + i, old, oldlen) == 0) {
			memcpy(dst + di, new, newlen);
			di += newlen;
			i += oldlen;
			count++;
		} else {
			dst[di++] = src[i++];
		}
	}
	memcpy(dst + di, src + i, len - i);
	di += len - i;

	f = fopen(path, "wb");
	if (!f) {
		fprintf(stderr, "patch_source: cannot write %s\n", path);
		free(src);
		free(dst);
		return 1;
	}
	if (fwrite(dst, 1, di, f) != di) {
		fclose(f);
		free(src);
		free(dst);
		return 1;
	}
	fclose(f);

	free(src);
	free(dst);

	if (count == 0) {
		fprintf(stderr, "patch_source: pattern not found in %s\n", path);
		return 1;
	}
	printf("patched %lu occurrence(s) in %s\n",
	       (unsigned long)count, path);
	return 0;
}
