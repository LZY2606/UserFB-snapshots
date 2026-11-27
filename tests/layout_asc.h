#ifndef LAYOUT_ASC_H
#define LAYOUT_ASC_H
#include <stddef.h>
#include "aac_fixture.h"
/* Flexible AudioSpecificConfig writer for the layout update regression.
   Describes a GASpecificConfig with an explicit program_config_element;
   when chan_cfg is nonzero the PCE is omitted entirely. */
struct layout_asc_spec {
    unsigned profile;        /* ASC audioObjectType */
    unsigned sf_index;       /* ASC samplingFrequencyIndex */
    unsigned chan_cfg;       /* ASC channelConfiguration */
    unsigned frame_len_flag; /* GASpecificConfig frameLengthFlag */
    unsigned pce_profile;    /* PCE object_type (2 bit profile) */
    unsigned pce_sf_index;   /* PCE sampling_frequency_index */
    struct fixture_element front[16]; unsigned n_front;
    struct fixture_element side[16];  unsigned n_side;
    struct fixture_element back[16];  unsigned n_back;
    unsigned lfe[4];                  unsigned n_lfe;
    unsigned n_assoc;
    unsigned n_cc;
    unsigned mono_mix, stereo_mix, matrix_mix;
};
/* AAC-LC, 48 kHz, channelConfiguration 0, PCE with front SCE0 + CPE0. */
void layout_asc_defaults(struct layout_asc_spec *sp);
size_t layout_asc_write(unsigned char *out, size_t capacity,
                        const struct layout_asc_spec *sp);
#endif
