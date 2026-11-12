/*
 * k12_example.c - fingerprint a stream from standard input with KT128.
 *
 * The input is read in fixed-size chunks and fed straight to the
 * context, so the whole message is never assembled in memory. The first
 * 32 bytes of the KT128 XOF (with an empty customisation string) are
 * printed as hexadecimal.
 *
 * Build against the static library, for example:
 *
 *     cc -std=c99 -Wall -I.. k12_example.c ../libkeccak.a -o k12_example
 *
 * Run:
 *
 *     ./k12_example < some-record
 */
#include <stdio.h>
#include <stdlib.h>

#include "libkeccak.h"
#include "libkeccak/k12.h"

#define CHUNK 4096
#define FINGERPRINT_LEN 32

int
main(void)
{
	struct libkeccak_k12 *ctx;
	unsigned char chunk[CHUNK];
	unsigned char fingerprint[FINGERPRINT_LEN];
	char hex[2 * FINGERPRINT_LEN + 1];
	size_t got;

	ctx = libkeccak_k12_create();
	if (!ctx) {
		fputs("k12_example: allocation failure\n", stderr);
		return 1;
	}

	/* update() any number of times; chunk boundaries do not matter. */
	while ((got = fread(chunk, 1, sizeof(chunk), stdin)) > 0) {
		if (libkeccak_k12_update(ctx, chunk, got) != 0) {
			fputs("k12_example: update failure\n", stderr);
			libkeccak_k12_destroy(ctx);
			return 1;
		}
	}
	if (ferror(stdin)) {
		fputs("k12_example: read failure\n", stderr);
		libkeccak_k12_destroy(ctx);
		return 1;
	}

	/* Finish the input with an empty customisation string. */
	if (libkeccak_k12_final(ctx, NULL, 0) != 0) {
		fputs("k12_example: final failure\n", stderr);
		libkeccak_k12_destroy(ctx);
		return 1;
	}

	/* Read the fingerprint from the XOF stream. */
	if (libkeccak_k12_squeeze(ctx, fingerprint, sizeof(fingerprint)) != 0) {
		fputs("k12_example: squeeze failure\n", stderr);
		libkeccak_k12_destroy(ctx);
		return 1;
	}
	libkeccak_k12_destroy(ctx);

	libkeccak_behex_lower(hex, fingerprint, sizeof(fingerprint));
	printf("%s\n", hex);
	return 0;
}
