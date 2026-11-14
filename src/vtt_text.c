/**********************************************************************************************/
/* The MIT License                                                                            */
/*                                                                                            */
/* Copyright 2016-2017 Twitch Interactive, Inc. or its affiliates. All Rights Reserved.       */
/*                                                                                            */
/* Permission is hereby granted, free of charge, to any person obtaining a copy               */
/* of this software and associated documentation files (the "Software"), to deal              */
/* in the Software without restriction, including without limitation the rights               */
/* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell                  */
/* copies of the Software, and to permit persons to whom the Software is                      */
/* furnished to do so, subject to the following conditions:                                   */
/*                                                                                            */
/* The above copyright notice and this permission notice shall be included in                 */
/* all copies or substantial portions of the Software.                                        */
/*                                                                                            */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR                 */
/* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,                   */
/* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE                */
/* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER                     */
/* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,              */
/* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN                  */
/* THE SOFTWARE.                                                                              */
/**********************************************************************************************/

// Structured, editable WebVTT cue payload: token parser, serializer,
// token/timing editors and karaoke preview frame rendering.

#include "vtt.h"
#include "utf8.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VTT_TIME_BEFORE INT64_MIN

typedef struct {
    vtt_text_token_t* data;
    size_t count;
    size_t capacity;
} token_array_t;

static void token_array_init(token_array_t* arr)
{
    arr->data = NULL;
    arr->count = 0;
    arr->capacity = 0;
}

static int token_array_reserve(token_array_t* arr, size_t extra)
{
    if (arr->count + extra <= arr->capacity) {
        return 1;
    }

    size_t cap = arr->capacity ? arr->capacity : 8;

    while (cap < arr->count + extra) {
        if (cap > (SIZE_MAX / 2)) {
            return 0;
        }
        cap *= 2;
    }

    vtt_text_token_t* data = realloc(arr->data, cap * sizeof(vtt_text_token_t));

    if (!data) {
        return 0;
    }

    arr->data = data;
    arr->capacity = cap;
    return 1;
}

static char* dup_bytes(const char* data, size_t size)
{
    char* out = malloc(size + 1);

    if (!out) {
        return NULL;
    }

    memcpy(out, data, size);
    out[size] = '\0';
    return out;
}

static int token_add(token_array_t* arr, vtt_text_token_t tok)
{
    if (!token_array_reserve(arr, 1)) {
        return 0;
    }

    arr->data[arr->count++] = tok;
    return 1;
}

static int token_add_text(token_array_t* arr, const char* data, size_t size, int b, int i, int u)
{
    if (0 == size) {
        return 1;
    }

    char* text = dup_bytes(data, size);

    if (!text) {
        return 0;
    }

    vtt_text_token_t tok;
    memset(&tok, 0, sizeof(tok));
    tok.type = VTT_TOKEN_TEXT;
    tok.text = text;
    tok.bold = b ? 1 : 0;
    tok.italic = i ? 1 : 0;
    tok.underline = u ? 1 : 0;
    tok.time_ms = -1;

    if (!token_add(arr, tok)) {
        free(text);
        return 0;
    }

    return 1;
}

static int token_add_newline(token_array_t* arr)
{
    vtt_text_token_t tok;
    memset(&tok, 0, sizeof(tok));
    tok.type = VTT_TOKEN_NEWLINE;
    tok.time_ms = -1;
    return token_add(arr, tok);
}

static int token_add_time(token_array_t* arr, int64_t time_ms)
{
    vtt_text_token_t tok;
    memset(&tok, 0, sizeof(tok));
    tok.type = VTT_TOKEN_TIME;
    tok.time_ms = time_ms;
    return token_add(arr, tok);
}

// Unknown markup (voice spans, classes, ...) is preserved verbatim so that
// dump/reparse keeps the payload instead of silently dropping the tag.
static int token_add_raw(token_array_t* arr, const char* data, size_t size)
{
    char* text = dup_bytes(data, size);

    if (!text) {
        return 0;
    }

    vtt_text_token_t tok;
    memset(&tok, 0, sizeof(tok));
    tok.type = VTT_TOKEN_RAW;
    tok.text = text;
    tok.time_ms = -1;

    if (!token_add(arr, tok)) {
        free(text);
        return 0;
    }

    return 1;
}

static void token_array_free(token_array_t* arr)
{
    for (size_t i = 0; i < arr->count; ++i) {
        free(arr->data[i].text);
    }

    free(arr->data);
    token_array_init(arr);
}

////////////////////////////////////////////////////////////////////////////////
// UTF-8 validation and decoding
static int utf8_validate(const char* data, size_t size)
{
    size_t i = 0;

    while (i < size) {
        unsigned char c = (unsigned char)data[i];
        size_t len;
        uint32_t cp;

        if (0 == c) {
            return 0;
        } else if (c < 0x80) {
            len = 1;
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            len = 2;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            cp = c & 0x07;
        } else {
            return 0;
        }

        if (i + len > size) {
            return 0;
        }

        for (size_t k = 1; k < len; ++k) {
            if (0x80 != ((unsigned char)data[i + k] & 0xC0)) {
                return 0;
            }
            cp = (cp << 6) | ((unsigned char)data[i + k] & 0x3F);
        }

        // overlong encodings, surrogates and values past U+10FFFF are invalid
        if ((2 == len && cp < 0x80) || (3 == len && cp < 0x800) || (4 == len && cp < 0x10000)
            || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return 0;
        }

        i += len;
    }

    return 1;
}

////////////////////////////////////////////////////////////////////////////////
// Dynamic byte buffer used by the serializer
typedef struct {
    char* data;
    size_t size;
    size_t capacity;
} byte_buf_t;

static void byte_buf_init(byte_buf_t* buf)
{
    buf->data = NULL;
    buf->size = 0;
    buf->capacity = 0;
}

static int byte_buf_reserve(byte_buf_t* buf, size_t extra)
{
    if (buf->size + extra + 1 <= buf->capacity) {
        return 1;
    }

    size_t cap = buf->capacity ? buf->capacity : 64;

    while (cap < buf->size + extra + 1) {
        if (cap > SIZE_MAX / 2) {
            return 0;
        }
        cap *= 2;
    }

    char* data = realloc(buf->data, cap);

    if (!data) {
        return 0;
    }

    buf->data = data;
    buf->capacity = cap;
    return 1;
}

static int byte_buf_append(byte_buf_t* buf, const char* data, size_t size)
{
    if (!byte_buf_reserve(buf, size)) {
        return 0;
    }

    memcpy(buf->data + buf->size, data, size);
    buf->size += size;
    buf->data[buf->size] = '\0';
    return 1;
}

static int byte_buf_puts(byte_buf_t* buf, const char* s)
{
    return byte_buf_append(buf, s, strlen(s));
}

static void byte_buf_free(byte_buf_t* buf)
{
    free(buf->data);
    byte_buf_init(buf);
}

////////////////////////////////////////////////////////////////////////////////
// Strict inline timestamp parsing: hh:mm:ss.mmm or mm:ss.mmm
static int two_digits(const char* p, long* out)
{
    if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') {
        return 0;
    }

    *out = (p[0] - '0') * 10 + (p[1] - '0');
    return 1;
}

static int parse_time_tag(const char* s, size_t len, int64_t* out_ms)
{
    long hh = 0, mm = 0, ss = 0, frac = 0;

    // mm:ss.mmm  (9 chars, colons at 2/5, dot at 5... positions 2 and 5, dot 5)
    if (9 == len && ':' == s[2] && '.' == s[5]) {
        if (!two_digits(s, &mm) || !two_digits(s + 3, &ss) || mm > 59 || ss > 59) {
            return 0;
        }

        for (size_t k = 6; k < 9; ++k) {
            if (s[k] < '0' || s[k] > '9') {
                return 0;
            }
        }

        frac = (s[6] - '0') * 100 + (s[7] - '0') * 10 + (s[8] - '0');
        *out_ms = ((mm * 60 + ss) * 1000) + frac;
        return 1;
    }

    // hh:mm:ss.mmm, hour field one or more digits; fixed 10-char tail ":mm:ss.mmm"
    if (len >= 11 && ':' == s[len - 10] && ':' == s[len - 7] && '.' == s[len - 4]) {
        size_t hour_len = len - 10;

        for (size_t k = 0; k < hour_len; ++k) {
            if (s[k] < '0' || s[k] > '9') {
                return 0;
            }
            hh = hh * 10 + (s[k] - '0');
        }

        if (!two_digits(s + len - 9, &mm) || !two_digits(s + len - 6, &ss) || mm > 59 || ss > 59) {
            return 0;
        }

        const char* f = s + len - 3;

        for (size_t k = 0; k < 3; ++k) {
            if (f[k] < '0' || f[k] > '9') {
                return 0;
            }
        }

        frac = (f[0] - '0') * 100 + (f[1] - '0') * 10 + (f[2] - '0');
        *out_ms = ((hh * 3600 + mm * 60 + ss) * 1000) + frac;
        return 1;
    }

    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// Payload parsing
typedef struct {
    const char* p;
    const char* end;
    int bold;
    int italic;
    int underline;
    int64_t prev_ms;
    int64_t cue_start_ms;
    int64_t cue_end_ms;
    token_array_t arr;
    int ok;
} cue_parser_t;

// Appends one decoded text segment, handling line breaks and the single
// layer of supported entities: &amp; &lt; &gt; &nbsp;
static int parse_text_segment(cue_parser_t* ps, const char* seg, size_t seg_size)
{
    byte_buf_t out;
    byte_buf_init(&out);

    for (size_t i = 0; i < seg_size;) {
        char c = seg[i];

        if ('\r' == c || '\n' == c) {
            if (!token_add_text(&ps->arr, out.data, out.size, ps->bold, ps->italic, ps->underline)
                || !token_add_newline(&ps->arr)) {
                byte_buf_free(&out);
                return 0;
            }

            byte_buf_free(&out);
            byte_buf_init(&out);

            if ('\r' == c && i + 1 < seg_size && '\n' == seg[i + 1]) {
                ++i;
            }

            ++i;
            continue;
        }

        if ('&' == c) {
            const char* semi = NULL;

            for (size_t k = i + 1; k < seg_size && k < i + 16; ++k) {
                if (';' == seg[k]) {
                    semi = seg + k;
                    break;
                }
            }

            const char* decoded = NULL;
            size_t decoded_size = 0;

            if (semi) {
                size_t name_len = (size_t)(semi - (seg + i + 1));

                if (3 == name_len && 0 == strncmp(seg + i + 1, "amp", 3)) {
                    decoded = "&";
                    decoded_size = 1;
                } else if (2 == name_len && 0 == strncmp(seg + i + 1, "lt", 2)) {
                    decoded = "<";
                    decoded_size = 1;
                } else if (2 == name_len && 0 == strncmp(seg + i + 1, "gt", 2)) {
                    decoded = ">";
                    decoded_size = 1;
                } else if (4 == name_len && 0 == strncmp(seg + i + 1, "nbsp", 4)) {
                    decoded = "\xC2\xA0";
                    decoded_size = 2;
                }
            }

            if (decoded) {
                if (!byte_buf_append(&out, decoded, decoded_size)) {
                    byte_buf_free(&out);
                    return 0;
                }
                i = (size_t)(semi - seg) + 1;
                continue;
            }
        }

        size_t char_len = utf8_char_length(seg + i);

        if (0 == char_len || i + char_len > seg_size) {
            byte_buf_free(&out);
            ps->ok = 0;
            return 0;
        }

        if (!byte_buf_append(&out, seg + i, char_len)) {
            byte_buf_free(&out);
            return 0;
        }

        i += char_len;
    }

    int rv = token_add_text(&ps->arr, out.data, out.size, ps->bold, ps->italic, ps->underline);
    byte_buf_free(&out);
    return rv;
}

static int parse_tag(cue_parser_t* ps, const char* tag, size_t tag_len)
{
    // tag/tag_len cover everything between '<' and '>', without the brackets
    if (0 == tag_len) {
        return 0;
    }

    // inline time tag: starts with a digit, strictly timestamp shaped
    if (tag[0] >= '0' && tag[0] <= '9') {
        int64_t ms;

        if (!parse_time_tag(tag, tag_len, &ms)) {
            return 0;
        }

        if (ms <= ps->cue_start_ms || ms >= ps->cue_end_ms || ms <= ps->prev_ms) {
            return 0;
        }

        ps->prev_ms = ms;
        return token_add_time(&ps->arr, ms);
    }

    int closing = 0;
    const char* name = tag;
    size_t name_len = tag_len;

    if ('/' == name[0]) {
        closing = 1;
        ++name;
        --name_len;
    }

    if (0 == name_len || !(('a' <= name[0] && name[0] <= 'z') || ('A' <= name[0] && name[0] <= 'Z'))) {
        return 0;
    }

    // b/i/u must be bare tags (<b>, </b>); attributes are not modeled
    if (1 == name_len) {
        char c = name[0];

        if ('b' == c || 'i' == c || 'u' == c) {
            int* depth;

            if ('b' == c) {
                depth = &ps->bold;
            } else if ('i' == c) {
                depth = &ps->italic;
            } else {
                depth = &ps->underline;
            }

            if (closing) {
                if (0 == *depth) {
                    return 0;
                }
                --(*depth);
            } else {
                ++(*depth);
            }

            return 1;
        }
    }

    // Anything else is kept verbatim (voice spans, class tags, ruby ...);
    // its semantics are not implemented but the bytes survive dump/reparse.
    return token_add_raw(&ps->arr, tag - 1, tag_len + 2);
}

static int cue_text_parse_internal(vtt_cue_text_t* ct, const char* payload, size_t payload_size,
    int64_t cue_start_ms, int64_t cue_end_ms)
{
    cue_parser_t ps;
    memset(&ps, 0, sizeof(ps));
    ps.p = payload;
    ps.end = payload + payload_size;
    ps.prev_ms = VTT_TIME_BEFORE;
    ps.cue_start_ms = cue_start_ms;
    ps.cue_end_ms = cue_end_ms;
    ps.ok = 1;
    token_array_init(&ps.arr);

    while (ps.p < ps.end && ps.ok) {
        const char* lt = memchr(ps.p, '<', (size_t)(ps.end - ps.p));

        if (!lt) {
            lt = ps.end;
        }

        if (lt > ps.p) {
            if (!parse_text_segment(&ps, ps.p, (size_t)(lt - ps.p))) {
                ps.ok = 0;
                break;
            }
        }

        if (lt == ps.end) {
            ps.p = lt;
            break;
        }

        const char* gt = memchr(lt + 1, '>', (size_t)(ps.end - lt - 1));

        if (!gt) {
            ps.ok = 0;
            break;
        }

        if (!parse_tag(&ps, lt + 1, (size_t)(gt - lt - 1))) {
            ps.ok = 0;
            break;
        }

        ps.p = gt + 1;
    }

    if (ps.ok && (0 != ps.bold || 0 != ps.italic || 0 != ps.underline)) {
        ps.ok = 0; // format tags must be closed
    }

    if (ps.ok && !utf8_validate(payload, payload_size)) {
        ps.ok = 0;
    }

    if (!ps.ok) {
        token_array_free(&ps.arr);
        ct->tokens = NULL;
        ct->count = 0;
        ct->capacity = 0;
        ct->valid = 0;
        return 0;
    }

    ct->tokens = ps.arr.data;
    ct->count = ps.arr.count;
    ct->capacity = ps.arr.capacity;
    ct->valid = 1;
    return 1;
}

////////////////////////////////////////////////////////////////////////////////
// Lifecycle hooks called from vtt.c. Parsing is lazy: vtt_block_new() runs
// before the cue timestamps are assigned, so attach on first access instead.
int vtt_cue_text_ensure(vtt_block_t* cue)
{
    if (!cue || VTT_CUE != cue->type) {
        return 0;
    }

    if (cue->cue_text) {
        return cue->cue_text->valid;
    }

    vtt_cue_text_t* ct = calloc(1, sizeof(vtt_cue_text_t));

    if (!ct) {
        return 0;
    }

    cue->cue_text = ct;
    const char* payload = vtt_block_data(cue);
    // vtt_cue_from_caption_frame() over-allocates (CAPTION_FRAME_TEXT_BYTES)
    // and stores a short NUL-terminated string, so never parse past the real
    // length; parsed blocks carry an exact text_size and are unaffected.
    size_t payload_size = strnlen(payload, cue->text_size);

    // The single trailing line break is the block terminator convention
    // (vtt_cue_from_caption_frame appends one too), not cue content.
    if (payload_size >= 2 && '\r' == payload[payload_size - 2] && '\n' == payload[payload_size - 1]) {
        payload_size -= 2;
    } else if (payload_size >= 1 && ('\r' == payload[payload_size - 1] || '\n' == payload[payload_size - 1])) {
        payload_size -= 1;
    }

    int64_t start_ms = llround(cue->timestamp * 1000.0);
    int64_t end_ms = llround((cue->timestamp + cue->duration) * 1000.0);

    if (end_ms <= start_ms) {
        // markers can only exist inside a positive-length cue
        end_ms = start_ms + 1;
    }

    cue_text_parse_internal(ct, payload, payload_size, start_ms, end_ms);
    return ct->valid;
}

void vtt_cue_text_detach(vtt_block_t* cue)
{
    if (!cue || !cue->cue_text) {
        return;
    }

    for (size_t i = 0; i < cue->cue_text->count; ++i) {
        free(cue->cue_text->tokens[i].text);
    }

    free(cue->cue_text->tokens);
    free(cue->cue_text);
    cue->cue_text = NULL;
}

////////////////////////////////////////////////////////////////////////////////
// Public token access API
size_t vtt_cue_token_count(vtt_block_t* cue)
{
    if (!vtt_cue_text_ensure(cue) || !cue->cue_text) {
        return 0;
    }

    return cue->cue_text->count;
}

const vtt_text_token_t* vtt_cue_token(vtt_block_t* cue, size_t index)
{
    if (!vtt_cue_text_ensure(cue) || !cue->cue_text || index >= cue->cue_text->count) {
        return NULL;
    }

    return &cue->cue_text->tokens[index];
}

libcaption_stauts_t vtt_cue_token_set_text(vtt_block_t* cue, size_t index, const utf8_char_t* text)
{
    if (!vtt_cue_text_ensure(cue) || !cue->cue_text || !text || index >= cue->cue_text->count) {
        return LIBCAPTION_ERROR;
    }

    vtt_text_token_t* tok = &cue->cue_text->tokens[index];

    if (VTT_TOKEN_TEXT != tok->type) {
        return LIBCAPTION_ERROR;
    }

    size_t size = strlen(text);

    if (!utf8_validate(text, size)) {
        return LIBCAPTION_ERROR;
    }

    char* copy = dup_bytes(text, size);

    if (!copy) {
        return LIBCAPTION_ERROR;
    }

    free(tok->text);
    tok->text = copy;
    return LIBCAPTION_OK;
}

libcaption_stauts_t vtt_cue_token_set_time(vtt_block_t* cue, size_t index, double absolute_time)
{
    if (!vtt_cue_text_ensure(cue) || !cue->cue_text || !isfinite(absolute_time)
        || index >= cue->cue_text->count) {
        return LIBCAPTION_ERROR;
    }

    vtt_text_token_t* tok = &cue->cue_text->tokens[index];

    if (VTT_TOKEN_TIME != tok->type) {
        return LIBCAPTION_ERROR;
    }

    int64_t start_ms = llround(cue->timestamp * 1000.0);
    int64_t end_ms = llround((cue->timestamp + cue->duration) * 1000.0);
    int64_t new_ms = llround(absolute_time * 1000.0);

    if (new_ms <= start_ms || new_ms >= end_ms) {
        return LIBCAPTION_ERROR;
    }

    // strict ordering against the neighboring time tags
    int64_t low = start_ms;
    int64_t high = end_ms;

    for (size_t i = 0; i < index; ++i) {
        if (VTT_TOKEN_TIME == cue->cue_text->tokens[i].type) {
            low = cue->cue_text->tokens[i].time_ms;
        }
    }

    for (size_t i = index + 1; i < cue->cue_text->count; ++i) {
        if (VTT_TOKEN_TIME == cue->cue_text->tokens[i].type) {
            high = cue->cue_text->tokens[i].time_ms;
            break;
        }
    }

    if (new_ms <= low || new_ms >= high) {
        return LIBCAPTION_ERROR;
    }

    tok->time_ms = new_ms;
    return LIBCAPTION_OK;
}

libcaption_stauts_t vtt_cue_retime(vtt_block_t* cue, double new_start, double new_duration)
{
    if (!cue || !isfinite(new_start) || !isfinite(new_duration) || !(new_duration > 0.0)) {
        return LIBCAPTION_ERROR;
    }

    // Parse so markers can move with the cue. A failed parse is fine here:
    // it yields no markers, and the cue's bounds can still be re-timed.
    vtt_cue_text_ensure(cue);

    int64_t old_start_ms = llround(cue->timestamp * 1000.0);
    int64_t old_end_ms = llround((cue->timestamp + cue->duration) * 1000.0);
    int64_t old_dur_ms = old_end_ms - old_start_ms;

    if (!(old_dur_ms > 0)) {
        return LIBCAPTION_ERROR;
    }

    int64_t new_start_ms = llround(new_start * 1000.0);
    int64_t new_end_ms = llround((new_start + new_duration) * 1000.0);
    int64_t new_dur_ms = new_end_ms - new_start_ms;

    if (!(new_dur_ms > 0)) {
        return LIBCAPTION_ERROR;
    }

    size_t count = cue->cue_text ? cue->cue_text->count : 0;
    int64_t* mapped = count ? malloc(count * sizeof(int64_t)) : NULL;

    if (count && !mapped) {
        return LIBCAPTION_ERROR;
    }

    int64_t prev = new_start_ms;

    for (size_t i = 0; i < count; ++i) {
        vtt_text_token_t* tok = &cue->cue_text->tokens[i];

        if (VTT_TOKEN_TIME != tok->type) {
            continue;
        }

        int64_t offset = tok->time_ms - old_start_ms;
        int64_t t = new_start_ms + llround(((double)offset * (double)new_dur_ms) / (double)old_dur_ms);

        if (t <= new_start_ms || t >= new_end_ms || t <= prev) {
            free(mapped);
            return LIBCAPTION_ERROR;
        }

        mapped[i] = t;
        prev = t;
    }

    // all checks passed, commit
    for (size_t i = 0; i < count; ++i) {
        if (VTT_TOKEN_TIME == cue->cue_text->tokens[i].type) {
            cue->cue_text->tokens[i].time_ms = mapped[i];
        }
    }

    free(mapped);
    cue->timestamp = new_start;
    cue->duration = new_duration;
    return LIBCAPTION_OK;
}

////////////////////////////////////////////////////////////////////////////////
// Serialization
static int serialize_escaped_text(byte_buf_t* buf, const char* text, size_t size)
{
    for (size_t i = 0; i < size;) {
        unsigned char c = (unsigned char)text[i];

        if ('&' == c) {
            if (!byte_buf_puts(buf, "&amp;")) {
                return 0;
            }
            ++i;
        } else if ('<' == c) {
            if (!byte_buf_puts(buf, "&lt;")) {
                return 0;
            }
            ++i;
        } else if ('>' == c) {
            if (!byte_buf_puts(buf, "&gt;")) {
                return 0;
            }
            ++i;
        } else if (0xC2 == c && i + 1 < size && 0xA0 == (unsigned char)text[i + 1]) {
            if (!byte_buf_puts(buf, "&nbsp;")) {
                return 0;
            }
            i += 2;
        } else {
            size_t char_len = utf8_char_length(text + i);

            if (0 == char_len || i + char_len > size) {
                return 0;
            }

            if (!byte_buf_append(buf, text + i, char_len)) {
                return 0;
            }

            i += char_len;
        }
    }

    return 1;
}

static int serialize_time(byte_buf_t* buf, int64_t ms)
{
    int64_t hh = ms / 3600000;
    int mm = (int)((ms / 60000) % 60);
    int ss = (int)((ms / 1000) % 60);
    int frac = (int)(ms % 1000);
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "<%02lld:%02d:%02d.%03d>", (long long)hh, mm, ss, frac);
    return byte_buf_puts(buf, tmp);
}

// Each text run is wrapped independently: active spans are closed in
// reverse nesting order (u, i, b), then the run's spans reopen (b, i, u).
// This keeps the emitted markup strictly nested even across time tags.
static int serialize_sync_state(byte_buf_t* buf, int* active, int target_b, int target_i, int target_u)
{
    static const char* const close_tag[3] = { "</b>", "</i>", "</u>" };
    static const char* const open_tag[3] = { "<b>", "<i>", "<u>" };
    int target[3] = { target_b, target_i, target_u };

    for (int k = 2; k >= 0; --k) {
        if (active[k] && !byte_buf_puts(buf, close_tag[k])) {
            return 0;
        }
        active[k] = 0;
    }

    for (int k = 0; k < 3; ++k) {
        if (target[k]) {
            if (!byte_buf_puts(buf, open_tag[k])) {
                return 0;
            }
            active[k] = 1;
        }
    }

    return 1;
}

size_t vtt_cue_serialize(vtt_block_t* cue, utf8_char_t* buf, size_t buf_size)
{
    if (!cue || VTT_CUE != cue->type) {
        return (size_t)-1;
    }

    // Force parsing so a never-touched cue still serializes its text
    // rather than the block-terminated raw payload.
    int valid = vtt_cue_text_ensure(cue);

    // Unparseable payloads fall back to the original bytes, dropping the
    // single trailing line break that terminates the block.
    if (!valid) {
        const char* raw = vtt_block_data(cue);
        // cap at the real string length for over-allocated frame-created cues
        size_t size = strnlen(raw, cue->text_size);

        if (size >= 2 && '\r' == raw[size - 2] && '\n' == raw[size - 1]) {
            size -= 2;
        } else if (size >= 1 && ('\r' == raw[size - 1] || '\n' == raw[size - 1])) {
            size -= 1;
        }

        if (buf && size < buf_size) {
            memcpy(buf, raw, size);
            buf[size] = '\0';
            return size;
        }

        return buf ? (size_t)-1 : size;
    }

    byte_buf_t out;
    byte_buf_init(&out);
    int active[3] = { 0, 0, 0 }; // b, i, u
    int ok = 1;

    for (size_t i = 0; i < cue->cue_text->count && ok; ++i) {
        vtt_text_token_t* tok = &cue->cue_text->tokens[i];

        switch (tok->type) {
        case VTT_TOKEN_TEXT:
            ok = serialize_sync_state(&out, active, tok->bold, tok->italic, tok->underline)
                && serialize_escaped_text(&out, tok->text, strlen(tok->text));
            break;

        case VTT_TOKEN_NEWLINE:
            // close spans at the line boundary; they reopen on the next text
            ok = serialize_sync_state(&out, active, 0, 0, 0) && byte_buf_append(&out, "\n", 1);
            break;

        case VTT_TOKEN_TIME:
            // time tags sit between spans, not inside them
            ok = serialize_sync_state(&out, active, 0, 0, 0) && serialize_time(&out, tok->time_ms);
            break;

        default: // VTT_TOKEN_RAW, stored including its angle brackets
            ok = byte_buf_append(&out, tok->text, strlen(tok->text));
            break;
        }
    }

    if (ok) {
        ok = serialize_sync_state(&out, active, 0, 0, 0);
    }

    if (!ok) {
        byte_buf_free(&out);
        return (size_t)-1;
    }

    size_t needed = out.size;

    if (buf && needed < buf_size) {
        memcpy(buf, out.data, needed);
        buf[needed] = '\0';
    } else if (buf) {
        needed = (size_t)-1;
    }

    byte_buf_free(&out);
    return needed;
}

////////////////////////////////////////////////////////////////////////////////
// Karaoke preview rendering
//
// All visible text is flattened into one UTF-8 stream (a real '\n' marks each
// hard line break) with a parallel per-codepoint attribute array. The frame
// then runs the exact same layout loop as caption_frame_from_text() /
// utf8_wrap_length(), so every glyph lands on the identical 32x15 cell it
// would occupy in a plain-text frame; future text therefore never changes
// wrapping and time tags occupy no cell.
typedef struct {
    eia608_style_t style;
    int underline;
    int64_t active_ms;
} glyph_attr_t;

typedef struct {
    byte_buf_t text;
    glyph_attr_t* attrs;
    size_t count;
    size_t capacity;
} flat_text_t;

static int flat_reserve(flat_text_t* flat, size_t extra)
{
    if (flat->count + extra <= flat->capacity) {
        return 1;
    }

    size_t cap = flat->capacity ? flat->capacity : 32;

    while (cap < flat->count + extra) {
        cap *= 2;
    }

    glyph_attr_t* attrs = realloc(flat->attrs, cap * sizeof(glyph_attr_t));

    if (!attrs) {
        return 0;
    }

    flat->attrs = attrs;
    flat->capacity = cap;
    return 1;
}

static int flat_push_char(flat_text_t* flat, const char* ch, size_t char_len, eia608_style_t style,
    int underline, int64_t active_ms)
{
    if (!flat_reserve(flat, 1) || !byte_buf_append(&flat->text, ch, char_len)) {
        return 0;
    }

    glyph_attr_t* a = &flat->attrs[flat->count++];
    a->style = style;
    a->underline = underline;
    a->active_ms = active_ms;
    return 1;
}

static int flat_push_newline(flat_text_t* flat)
{
    if (!flat_reserve(flat, 1) || !byte_buf_append(&flat->text, "\n", 1)) {
        return 0;
    }

    glyph_attr_t* a = &flat->attrs[flat->count++];
    a->style = eia608_style_white;
    a->underline = 0;
    a->active_ms = VTT_TIME_BEFORE;
    return 1;
}

static void flat_free(flat_text_t* flat)
{
    byte_buf_free(&flat->text);
    free(flat->attrs);
    memset(flat, 0, sizeof(*flat));
}

// Unsupported markup (RAW) is skipped: it never occupies a caption cell.
static flat_text_t* build_flat(vtt_block_t* cue)
{
    flat_text_t* flat = calloc(1, sizeof(flat_text_t));

    if (!flat) {
        return NULL;
    }

    byte_buf_init(&flat->text);
    int64_t active_ms = VTT_TIME_BEFORE;

    for (size_t i = 0; i < cue->cue_text->count; ++i) {
        vtt_text_token_t* tok = &cue->cue_text->tokens[i];

        if (VTT_TOKEN_TIME == tok->type) {
            active_ms = tok->time_ms;
            continue;
        }

        if (VTT_TOKEN_NEWLINE == tok->type) {
            if (!flat_push_newline(flat)) {
                flat_free(flat);
                free(flat);
                return NULL;
            }
            continue;
        }

        if (VTT_TOKEN_TEXT != tok->type) {
            continue;
        }

        eia608_style_t style = tok->italic ? eia608_style_italics : eia608_style_white;
        const char* p = tok->text;

        while (*p) {
            size_t len = utf8_char_length(p);

            if (0 == len) {
                flat_free(flat);
                free(flat);
                return NULL;
            }

            if (!flat_push_char(flat, p, len, style, tok->underline, active_ms)) {
                flat_free(flat);
                free(flat);
                return NULL;
            }

            p += len;
        }
    }

    return flat;
}

// Writes the frame at preview_ms; INT64_MAX renders every cell as NONE.
static libcaption_stauts_t render_frame(vtt_block_t* cue, caption_frame_t* frame, int64_t preview_ms,
    double frame_timestamp)
{
    flat_text_t* flat = build_flat(cue);

    if (!flat) {
        return LIBCAPTION_ERROR;
    }

    caption_frame_init(frame);
    frame->write = &frame->back;

    const char* data = flat->text.data;
    ssize_t size = (ssize_t)flat->text.size;
    size_t cp_index = 0;

    // Same control flow as caption_frame_from_text(); only the cell payload
    // and phase come from the structured attributes instead of raw bytes.
    for (size_t r = 0; (*data) && size && r < SCREEN_ROWS;) {
        while (size && utf8_char_whitespace(data)) {
            size_t s = utf8_char_length(data);
            data += s, size -= (ssize_t)s, ++cp_index;
        }

        utf8_size_t char_count = utf8_wrap_length(data, SCREEN_COLS);

        for (size_t c = 0; c < char_count; ++c) {
            size_t char_length = utf8_char_length(data);

            // utf8_wrap_length() can run past the real text because the NUL
            // terminator counts as whitespace; caption_frame_write_char()
            // simply no-ops on such a zero-length char, so stop here to avoid
            // touching phantom cells or reading attributes past the glyphs.
            if (0 == char_length || cp_index >= flat->count) {
                break;
            }

            caption_frame_cell_t* cell = &frame->back.cell[r][c];
            size_t len = utf8_char_copy((utf8_char_t*)cell->data, data);
            cell->data[len] = '\0';
            cell->sty = flat->attrs[cp_index].style;
            cell->uln = flat->attrs[cp_index].underline ? 1 : 0;

            if (INT64_MAX == preview_ms) {
                // legacy full-text conversion is not a karaoke preview
                frame->back.phase[r][c] = caption_phase_none;
            } else {
                // every legal Unicode codepoint occupies one cell, including
                // codepoints that have no EIA-608 encoding
                int64_t active = flat->attrs[cp_index].active_ms;
                frame->back.phase[r][c]
                    = (VTT_TIME_BEFORE == active || active <= preview_ms) ? caption_phase_past
                                                                          : caption_phase_future;
            }

            data += char_length, size -= (ssize_t)char_length, ++cp_index;
        }

        r += char_count ? 1 : 0;
    }

    caption_frame_end(frame);
    frame->timestamp = frame_timestamp;
    flat_free(flat);
    free(flat);
    return LIBCAPTION_OK;
}

libcaption_stauts_t vtt_cue_to_caption_frame_at(vtt_block_t* cue, double absolute_time, caption_frame_t* frame)
{
    if (!cue || !frame || !vtt_cue_text_ensure(cue) || !cue->cue_text || !isfinite(absolute_time)) {
        return LIBCAPTION_ERROR;
    }

    int64_t start_ms = llround(cue->timestamp * 1000.0);
    int64_t end_ms = llround((cue->timestamp + cue->duration) * 1000.0);
    int64_t at_ms = llround(absolute_time * 1000.0);

    caption_frame_init(frame);
    frame->timestamp = absolute_time;

    // outside the half-open cue interval [start, end): empty screen, all NONE
    if (at_ms < start_ms || at_ms >= end_ms) {
        return LIBCAPTION_OK;
    }

    return render_frame(cue, frame, at_ms, absolute_time);
}

// Final-state rendering used by the legacy vtt_cue_to_caption_frame()
libcaption_stauts_t vtt_cue_render_final(vtt_block_t* cue, caption_frame_t* frame)
{
    if (!cue || !frame || !vtt_cue_text_ensure(cue) || !cue->cue_text) {
        return LIBCAPTION_ERROR;
    }

    return render_frame(cue, frame, INT64_MAX, cue->timestamp + cue->duration);
}
