/* Shared helpers for the LOAS/LATM regression tests. */
#include "latm_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t *latm_read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    long n;
    uint8_t *buf;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    if (n <= 0)
    {
        fclose(f);
        return NULL;
    }
    buf = (uint8_t *)malloc((size_t)n + 16);
    if (!buf)
    {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n)
    {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

int latm_load_aus(const char *path, latm_au_list *out)
{
    uint8_t *buf;
    size_t size, pos = 0;

    memset(out, 0, sizeof(*out));
    buf = latm_read_file(path, &size);
    if (!buf)
        return 1;

    while (pos < size)
    {
        size_t len, head;
        if (size - pos < 7 || buf[pos] != 0xFF || (buf[pos + 1] & 0xF6) != 0xF0)
        {
            free(buf);
            return 1;
        }
        len = ((size_t)(buf[pos + 3] & 3) << 11) |
              ((size_t)buf[pos + 4] << 3) | (buf[pos + 5] >> 5);
        head = (buf[pos + 1] & 1) ? 7 : 9;
        if (len < head || len > size - pos)
        {
            free(buf);
            return 1;
        }
        if (out->count >= LATM_MAX_AUS || len - head > LATM_MAX_AU_SIZE)
        {
            free(buf);
            return 1;
        }
        memcpy(out->au[out->count].data, buf + pos + head, len - head);
        out->au[out->count].len = len - head;
        out->count++;
        pos += len;
    }
    free(buf);
    return out->count > 0 ? 0 : 1;
}

/* MSB first bit writer */
typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t bits;
    int overflow;
} bitw;

static void bw_put(bitw *w, uint32_t value, int nbits)
{
    int i;
    for (i = nbits - 1; i >= 0; i--)
    {
        size_t byte = w->bits >> 3;
        if (byte >= w->cap)
        {
            w->overflow = 1;
            return;
        }
        if ((w->bits & 7) == 0)
            w->buf[byte] = 0;
        if ((value >> i) & 1)
            w->buf[byte] |= (uint8_t)(0x80 >> (w->bits & 7));
        w->bits++;
    }
}

static void bw_align(bitw *w)
{
    while (w->bits & 7)
        bw_put(w, 0, 1);
}

latm_mux_cfg latm_default_cfg(const uint8_t *asc, int asc_bits)
{
    latm_mux_cfg cfg;
    cfg.asc = asc;
    cfg.asc_bits = asc_bits;
    cfg.audio_mux_version = 0;
    cfg.all_same_time = 1;
    cfg.num_programs = 1;
    cfg.num_layers = 1;
    cfg.frame_length_type = 0;
    cfg.other_data_bits = 0;
    cfg.crc_present = 0;
    return cfg;
}

size_t latm_build_frame(uint8_t *out, size_t cap, const latm_mux_cfg *cfg,
                        int num_subframes,
                        const uint8_t **aus, const size_t *au_lens, int au_count)
{
    bitw w;
    size_t mux_len;
    int i;

    if (!out || !cfg || !aus || !au_lens)
        return 0;
    if (au_count <= 0 || au_count != num_subframes ||
        num_subframes < 1 || num_subframes > 64)
        return 0;
    if (cap < 3 + 1)
        return 0;

    /* write the AudioMuxElement after the 3 byte LOAS header */
    w.buf = out + 3;
    w.cap = cap - 3;
    w.bits = 0;
    w.overflow = 0;

    if (cfg->asc == NULL)
    {
        bw_put(&w, 1, 1); /* useSameStreamMux */
    } else {
        int j;
        bw_put(&w, 0, 1); /* useSameStreamMux */
        bw_put(&w, (uint32_t)cfg->audio_mux_version, 1);
        bw_put(&w, (uint32_t)cfg->all_same_time, 1);
        bw_put(&w, (uint32_t)(num_subframes - 1), 6);
        bw_put(&w, (uint32_t)(cfg->num_programs - 1), 4);
        bw_put(&w, (uint32_t)(cfg->num_layers - 1), 3);
        for (j = 0; j < cfg->asc_bits; j++)
            bw_put(&w, (cfg->asc[j >> 3] >> (7 - (j & 7))) & 1, 1);
        bw_put(&w, (uint32_t)cfg->frame_length_type, 3);
        bw_put(&w, 0, 8); /* latmBufferFullness */
        bw_put(&w, cfg->other_data_bits > 0 ? 1u : 0u, 1);
        if (cfg->other_data_bits > 0)
        {
            /* otherDataLenBits: big-endian bytes, escape bit on all
               but the last byte */
            int v = cfg->other_data_bits;
            uint8_t bytes[8];
            int nb = 0, j;
            do {
                bytes[nb++] = (uint8_t)(v & 0xFF);
                v >>= 8;
            } while (v > 0 && nb < 8);
            for (j = nb - 1; j >= 0; j--)
            {
                bw_put(&w, j > 0 ? 1u : 0u, 1);
                bw_put(&w, bytes[j], 8);
            }
        }
        bw_put(&w, (uint32_t)cfg->crc_present, 1);
        if (cfg->crc_present)
            bw_put(&w, 0, 8);
    }

    /* PayloadMux: per subframe the payloadLengthInfo, then the payload */
    for (i = 0; i < au_count; i++)
    {
        size_t rest = au_lens[i];
        size_t j;
        do {
            size_t v = rest > 255 ? 255 : rest;
            bw_put(&w, (uint32_t)v, 8);
            rest -= v;
        } while (rest > 0);
        for (j = 0; j < au_lens[i]; j++)
            bw_put(&w, aus[i][j], 8);
    }

    /* otherData, declared bit exact */
    for (i = 0; i < cfg->other_data_bits; i++)
        bw_put(&w, (uint32_t)(i & 1), 1);

    bw_align(&w);
    if (w.overflow)
        return 0;

    mux_len = (w.bits + 7) / 8;
    if (mux_len == 0 || mux_len > 0x1FFF)
        return 0;

    out[0] = 0x56;
    out[1] = (uint8_t)(0xE0 | ((mux_len >> 8) & 0x1F));
    out[2] = (uint8_t)(mux_len & 0xFF);

    return 3 + mux_len;
}
