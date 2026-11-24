/* Allocation accounting for the codec only; callers' fixture storage is separate. */
#include <stddef.h>
#include <stdlib.h>
typedef union Header { size_t size; long double align_number; void* align_pointer; } Header;
static size_t current_bytes, peak_bytes;
void* lodepng_malloc(size_t n) {
  Header* h;
  if(n > (size_t)-1 - sizeof(Header)) return 0;
  h = (Header*)malloc(sizeof(Header) + n);
  if(!h) return 0;
  h->size = n; current_bytes += n;
  if(current_bytes > peak_bytes) peak_bytes = current_bytes;
  return h + 1;
}
void lodepng_free(void* p) {
  if(p) { Header* h = (Header*)p - 1; current_bytes -= h->size; free(h); }
}
void* lodepng_realloc(void* p, size_t n) {
  Header* h;
  size_t before;
  if(!p) return lodepng_malloc(n);
  if(!n) { lodepng_free(p); return 0; }
  if(n > (size_t)-1 - sizeof(Header)) return 0;
  h = (Header*)p - 1; before = h->size;
  h = (Header*)realloc(h, sizeof(Header) + n);
  if(!h) return 0;
  h->size = n; current_bytes = current_bytes - before + n;
  if(current_bytes > peak_bytes) peak_bytes = current_bytes;
  return h + 1;
}
size_t unit_alloc_current(void) { return current_bytes; }
size_t unit_alloc_peak(void) { return peak_bytes; }
void unit_alloc_reset_peak(void) { peak_bytes = current_bytes; }
