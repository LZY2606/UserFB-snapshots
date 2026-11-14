#ifndef LAYOUT_REF_H
#define LAYOUT_REF_H
#include <stddef.h>
#include "aac_fixture.h"
#include "neaacdec.h"

/* Flexible AudioSpecificConfig builder for the layout update tests.
   Defaults (after asc_spec_init) match the fixture: AAC-LC, 48kHz,
   channelConfiguration == 0 with an explicit PCE, 1024 sample frames. */
struct asc_spec {
    unsigned chconf;        /* channelConfiguration */
    unsigned sf_index;      /* ASC samplingFrequencyIndex */
    unsigned framelen_flag; /* GASpecificConfig frameLengthFlag */
    unsigned ext_flag;      /* GASpecificConfig extensionFlag */
    unsigned pce_profile;   /* PCE object_type (1 == LC) */
    unsigned pce_sf;        /* PCE sampling_frequency_index */
    unsigned mono_mix, stereo_mix, matrix_mix;
    unsigned nfront, nside, nback;
    struct fixture_element front[16], side[16], back[16];
    unsigned nlfe, lfe_tag[4];
    unsigned nassoc, assoc_tag[8];
    unsigned ncc, cc_tag[16];
};

void asc_spec_init(struct asc_spec *s);
size_t layout_asc(unsigned char *out, size_t cap, const struct asc_spec *s);
/* raw access unit that starts with an in-stream PCE (to be ignored by
   the decoder) followed by the given elements */
size_t layout_packet_pce(unsigned char *out, size_t cap,
                         const struct asc_spec *pce_spec,
                         const struct fixture_element *e, unsigned count,
                         unsigned seq, unsigned shape, unsigned gain);
unsigned spec_channels(const struct asc_spec *s);
/* channel slot of an identity in the layout, -1 when absent */
int spec_slot(const struct asc_spec *s, unsigned pair, unsigned tag);

/* open a float-output decoder from a ready ASC */
NeAACDecHandle layout_open(const unsigned char *asc, unsigned long n);

#endif
