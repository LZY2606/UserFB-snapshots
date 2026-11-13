/* SPDX-License-Identifier: MIT */
#ifndef FIXTURE_H
#define FIXTURE_H
/* Writes tiny, linked ELF64 little-endian objects, never executable images.
   main has one CU and macro unit; sup has only macros/strings and no CU. */
int task_basic_fixtures(const char *directory);
#endif
