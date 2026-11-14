#ifndef AAC_FIXTURE_H
#define AAC_FIXTURE_H
#include <stddef.h>
/* Small ordinary-spectrum AAC-LC material, 48kHz, 1024 samples.
   All elements are front elements. pair=0 means SCE, pair=1 means CPE.
   The order passed to fixture_packet is the order in the raw access unit. */
struct fixture_element { unsigned pair, tag; };
size_t fixture_asc(unsigned char *out, size_t capacity,
                   const struct fixture_element *elements, size_t count);
size_t fixture_packet(unsigned char *out, size_t capacity,
                      const struct fixture_element *elements, size_t count,
                      unsigned sequence, unsigned shape, unsigned gain);
#endif
