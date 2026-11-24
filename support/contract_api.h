#ifndef LOCATION_CONTRACT_API_H
#define LOCATION_CONTRACT_API_H
#include <stddef.h>
void location_add_case(const char *name, void (*fn)(void));
void location_expect(int condition, const char *message);
void register_location_cases(void);
#endif
