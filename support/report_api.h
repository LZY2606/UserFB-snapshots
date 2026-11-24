#ifndef REPORT_API_H
#define REPORT_API_H
#include <stdio.h>
/* Implement in root sample_report.c. Return zero on success, nonzero on error.
   It is invoked repeatedly in the SAME process. Do not exit the process. */
int sample_report(const char *elf_path, const char *pc_path, FILE *output);
#endif
