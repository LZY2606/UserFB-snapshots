/* Shared helpers for the LOAS/LATM regression tests: ADTS access unit
** extraction from the AAC fixture and a LOAS transport frame builder.
*/
#ifndef LATM_UTIL_H
#define LATM_UTIL_H

#include <stddef.h>
#include <stdint.h>

#define LATM_MAX_AUS 16
#define LATM_MAX_AU_SIZE 4096

typedef struct {
    uint8_t data[LATM_MAX_AU_SIZE];
    size_t len;
} latm_au;

typedef struct {
    latm_au au[LATM_MAX_AUS];
    int count;
} latm_au_list;

/* Read a whole file, returns malloc'ed buffer (NULL on error). */
uint8_t *latm_read_file(const char *path, size_t *size);

/* Split an ADTS AAC file into raw access units. Returns 0 on success. */
int latm_load_aus(const char *path, latm_au_list *out);

/* Configuration of a StreamMuxConfig to embed in a frame. */
typedef struct {
    const uint8_t *asc;     /* ASC bytes, NULL -> useSameStreamMux frame */
    int asc_bits;           /* number of valid ASC bits */
    int audio_mux_version;  /* supported: 0 */
    int all_same_time;      /* supported: 1 */
    int num_programs;       /* supported: 1 */
    int num_layers;         /* supported: 1 */
    int frame_length_type;  /* supported: 0 */
    int other_data_bits;    /* 0 -> no otherData */
    int crc_present;        /* 0 -> no crcCheckSum */
} latm_mux_cfg;

/* Default in-scope configuration carrying the given ASC. */
latm_mux_cfg latm_default_cfg(const uint8_t *asc, int asc_bits);

/* Build one complete LOAS frame (3 byte header + AudioMuxElement).
** au_count must equal num_subframes. Returns the frame length in bytes,
** or 0 if the output buffer is too small or the arguments are invalid.
*/
size_t latm_build_frame(uint8_t *out, size_t cap, const latm_mux_cfg *cfg,
                        int num_subframes,
                        const uint8_t **aus, const size_t *au_lens, int au_count);

#endif
