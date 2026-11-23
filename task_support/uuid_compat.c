/* libuuid API adapter for the pinned MIT uuid4 source in deps/uuid4. */
#include <stdlib.h>
#include <string.h>
#include "uuid4.h"
#include "uuid/uuid.h"
void uuid_generate(uuid_t out) {
    static int initialized;
    static const char alphabet[]="0123456789abcdef";
    char text[UUID4_LEN]; unsigned i=0, n=0, value=0;
    if (!initialized) {
        if (uuid4_init()!=UUID4_ESUCCESS) abort();
        initialized=1;
    }
    uuid4_generate(text);
    while (text[i]) {
        const char *digit;
        if (text[i]=='-') { ++i; continue; }
        digit=strchr(alphabet,text[i++]);
        if (!digit) abort();
        value=(value<<4)|(unsigned)(digit-alphabet);
        if (++n%2==0) out[n/2-1]=(unsigned char)value;
    }
    if (n!=32) abort();
}
