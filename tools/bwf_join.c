/*
 * bwf_join.c - command line entry point.
 *
 * Usage: bwf_join OUTPUT INPUT...
 *
 * All validation (format, metadata references, overlap byte-exactness,
 * RIFF size limits) happens before OUTPUT is created.  See bwf.h for the
 * module layout and README.md for the supported metadata boundary.
 */
#include "bwf.h"

#include <stdio.h>

int main(int argc, char **argv)
{
    char err[BWF_ERR_SIZE];

    if (bwf_join_run(argc, argv, err, sizeof(err)) != 0) {
        fprintf(stderr, "bwf_join: %s\n", err);
        return 1;
    }
    return 0;
}
