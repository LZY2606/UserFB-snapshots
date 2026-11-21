/* Regression tests for the independent CEA-608 CC1..CC4 decoder and its
   resumable checkpoint format. All CC data words are hand-built from the
   on-wire bit layout and carry their own odd parity so the tests exercise
   the format rather than the encoder helpers. */
#include "cc608_decoder.h"
#include "task_support/check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

////////////////////////////////////////////////////////////////////////////////
// Hand written CEA-608 words

static uint8_t parity7(unsigned value)
{
    unsigned v = value & 0x7F;
    unsigned ones = 0;

    for (unsigned i = 0; i < 7; ++i) {
        ones += (v >> i) & 1u;
    }

    return (uint8_t)(v | ((ones & 1u) ? 0x00 : 0x80));
}

static uint16_t word(unsigned high, unsigned low) { return (uint16_t)((parity7(high) << 8) | parity7(low)); }

/* Basic North American characters, two per word, no channel bit. */
static uint16_t chars(char a, char b) { return word((unsigned char)a, (unsigned char)b); }

/* Row code as decoded by eia608_row_map: bits 10:8 hold row bits 3:1 and the
   field relative bit 5 carries row bit 0. Returns a parity-stripped PAC. */
static unsigned pac_bits(int row_index, int chan)
{
    static const unsigned reverse_row_map[16] = { 2, 3, 4, 5, 10, 11, 12, 13, 14, 15, 0, 6, 7, 8, 9, 1 };
    unsigned row = reverse_row_map[row_index & 0x0F];
    return 0x1040u | (chan ? 0x0800u : 0u) | ((row << 7) & 0x0700u) | ((row << 5) & 0x0020u);
}

/* PAC row/column form: the 0x10 bit of the low byte marks column addressing. */
static uint16_t pac_col(int row_index, int col, int chan)
{
    unsigned x = 0x10u | ((unsigned)(col / 4) & 0x0Fu);
    unsigned pac = (pac_bits(row_index, chan) & ~0x1Fu) | ((x << 1) & 0x1Eu) | (pac_bits(row_index, chan) & 0x01u);
    return (uint16_t)((parity7(pac >> 8) << 8) | parity7(pac & 0x7F));
}

/* PAC row/style form: style and underline in the low byte. */
static uint16_t pac_style(int row_index, int style, int underline, int chan)
{
    unsigned x = ((unsigned)style << 1) & 0x0Eu;
    unsigned pac = (pac_bits(row_index, chan) & ~0x1Fu) | x | (underline ? 1u : 0u);
    return (uint16_t)((parity7(pac >> 8) << 8) | parity7(pac & 0x7F));
}

/* Mid-row style change: 0x11 0x20 plus style, channel in the high byte bit 3. */
static uint16_t midrow(int style, int underline, int chan) { return word(0x11 | (chan ? 0x08 : 0x00), 0x20 | ((style & 0x07) << 1) | (underline ? 1 : 0)); }

/* Control command: 0x14/0x1x high byte, channel in the high byte bit 3. */
static uint16_t control(unsigned high, unsigned low, int chan) { return word((high & 0x17) | (chan ? 0x08 : 0x00), low & 0x3F); }

/* Special North American character: 0x11 0x30 + index, channel in high byte. */
static uint16_t special(int index, int chan) { return word(0x11 | (chan ? 0x08 : 0x00), 0x30 + (index & 0x0F)); }

/* Extended Western European character: 0x12 0x20 + index, channel in high byte.
   The decoder backs up one cell before writing these, matching the legacy
   behaviour, so tests account for that. */
static uint16_t weu(int index, int chan) { return word(0x12 | (chan ? 0x08 : 0x00), 0x20 + (index & 0x1F)); }

/* XDS start: class in the high low-nibble, type in the low low-nibble. Both
   bytes must clear the 0x70 bits for eia608_is_xds. */
static uint16_t xds_start(int class_code, int type) { return word(class_code & 0x0F, type & 0x0F); }
static uint16_t xds_payload(unsigned a, unsigned b) { return word(a & 0x7F, b & 0x7F); }
static uint16_t xds_end(unsigned checksum) { return word(0x0F, checksum & 0x7F); }

static int parity_ok(uint16_t w) { return eia608_parity_varify(w) ? 1 : 0; }

////////////////////////////////////////////////////////////////////////////////
// Harness helpers

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

static void feed_field(cea708_t* cc, cea708_cc_type_t type, const uint16_t* words, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        cea708_add_cc_data(cc, 1, type, words[i]);
    }
}

static libcaption_stauts_t decode_words(cc608_decoder_t* decoder, double timestamp, cea708_cc_type_t type, const uint16_t* words, size_t count)
{
    cea708_t cc;
    cea708_init(&cc, timestamp);
    feed_field(&cc, type, words, count);
    return cc608_decoder_decode(decoder, &cc);
}

static int front_text(const cc608_decoder_t* decoder, cc608_service_t service, char* text)
{
    caption_frame_t frame;
    if (!cc608_decoder_get_frame(decoder, service, &frame)) {
        return 0;
    }

    caption_frame_to_text(&frame, text);
    return 1;
}

/* caption_frame_to_text always reads the front buffer, so extract hidden text
   by swapping buffers in a local copy. */
static int back_text(const cc608_decoder_t* decoder, cc608_service_t service, char* text)
{
    caption_frame_t frame;
    if (!cc608_decoder_get_frame(decoder, service, &frame)) {
        return 0;
    }

    caption_frame_buffer_t front = frame.front;
    frame.front = frame.back;
    frame.back = front;
    caption_frame_to_text(&frame, text);
    return 1;
}

typedef struct {
    caption_frame_buffer_t front;
    caption_frame_buffer_t back;
    int8_t row, col;
    int rup, sty, uln;
    double timestamp;
    uint16_t cc_data;
    int write_target; /* -1 none, 0 front, 1 back */
    int xds_state;
    int xds_class;
    int xds_type;
    uint32_t xds_size;
    uint8_t xds_checksum;
    uint8_t xds_content[32];
} service_snapshot_t;

static int snapshot_service(const cc608_decoder_t* decoder, cc608_service_t service, service_snapshot_t* snap)
{
    caption_frame_t frame;
    if (!cc608_decoder_get_frame(decoder, service, &frame)) {
        return 0;
    }

    snap->front = frame.front;
    snap->back = frame.back;
    snap->row = frame.state.row;
    snap->col = frame.state.col;
    snap->rup = frame.state.rup;
    snap->sty = frame.state.sty;
    snap->uln = frame.state.uln;
    snap->timestamp = frame.timestamp;
    snap->cc_data = frame.state.cc_data;
    snap->write_target = 0 == frame.write ? -1 : (frame.write == &frame.back ? 1 : 0);
    snap->xds_state = frame.xds.state;
    snap->xds_class = frame.xds.class_code;
    snap->xds_type = frame.xds.type;
    snap->xds_size = frame.xds.size;
    snap->xds_checksum = frame.xds.checksum;
    memcpy(snap->xds_content, frame.xds.content, sizeof(snap->xds_content));
    return 1;
}

static int snapshots_equal(const service_snapshot_t* a, const service_snapshot_t* b)
{
    return 0 == memcmp(&a->front, &b->front, sizeof(a->front)) && 0 == memcmp(&a->back, &b->back, sizeof(a->back)) && a->row == b->row && a->col == b->col && a->rup == b->rup && a->sty == b->sty && a->uln == b->uln && a->timestamp == b->timestamp && a->cc_data == b->cc_data && a->write_target == b->write_target && a->xds_state == b->xds_state && a->xds_class == b->xds_class && a->xds_type == b->xds_type && a->xds_size == b->xds_size && a->xds_checksum == b->xds_checksum && 0 == memcmp(a->xds_content, b->xds_content, sizeof(a->xds_content));
}

static int all_services_equal(const cc608_decoder_t* left, const cc608_decoder_t* right)
{
    for (int s = 0; s < CC608_SERVICE_COUNT; ++s) {
        service_snapshot_t a, b;
        if (!snapshot_service(left, (cc608_service_t)s, &a) || !snapshot_service(right, (cc608_service_t)s, &b) || !snapshots_equal(&a, &b)) {
            return 0;
        }
    }

    return 1;
}

static int service_field(cc608_service_t service) { return service >= cc608_cc3 ? 1 : 0; }
static int service_chan(cc608_service_t service) { return service == cc608_cc2 || service == cc608_cc4 ? 1 : 0; }
static cea708_cc_type_t service_type(cc608_service_t service) { return service_field(service) ? cc_type_ntsc_cc_field_2 : cc_type_ntsc_cc_field_1; }

/* Resume caption loading selects this service on its field (pop-on). */
static libcaption_stauts_t select_service(cc608_decoder_t* decoder, cc608_service_t service, double timestamp)
{
    uint16_t w = control(0x14, 0x20, service_chan(service));
    return decode_words(decoder, timestamp, service_type(service), &w, 1);
}

static libcaption_stauts_t load_service_text(cc608_decoder_t* decoder, cc608_service_t service, const char* text, double timestamp)
{
    uint16_t words[64];
    size_t n = 0;

    if (LIBCAPTION_ERROR == select_service(decoder, service, timestamp)) {
        return LIBCAPTION_ERROR;
    }

    for (const char* c = text; c[0] && n + 2 < ARRAY_COUNT(words); c += c[1] ? 2 : 1) {
        /* A lone final character still travels as one word with an empty
           second byte, which the legacy decoder accepts. */
        words[n++] = chars(c[0], c[1]);
    }

    return decode_words(decoder, timestamp, service_type(service), words, n);
}

static libcaption_stauts_t end_caption(cc608_decoder_t* decoder, cc608_service_t service, double timestamp)
{
    uint16_t w = control(0x14, 0x2F, service_chan(service));
    return decode_words(decoder, timestamp, service_type(service), &w, 1);
}

////////////////////////////////////////////////////////////////////////////////
// Routing group

static int test_routing_field_defaults(void)
{
    cc608_decoder_t* d = cc608_decoder_create();
    TASK_CHECK(d != 0);

    uint16_t rcl = control(0x14, 0x20, 0);
    uint16_t aa = chars('A', 'A');
    TASK_CHECK(parity_ok(rcl) && parity_ok(aa));
    TASK_CHECK(LIBCAPTION_OK == decode_words(d, 1.0, cc_type_ntsc_cc_field_1, &rcl, 1));
    TASK_CHECK(LIBCAPTION_OK == decode_words(d, 1.0, cc_type_ntsc_cc_field_2, &rcl, 1));
    TASK_CHECK(LIBCAPTION_OK == decode_words(d, 2.0, cc_type_ntsc_cc_field_1, &aa, 1));
    TASK_CHECK(LIBCAPTION_OK == decode_words(d, 2.0, cc_type_ntsc_cc_field_2, &aa, 1));

    caption_frame_t f1, f2, f3, f4;
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc1, &f1));
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc2, &f2));
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc3, &f3));
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc4, &f4));
    TASK_CHECK(0 == strcmp(f1.back.cell[14][0].data, "A"));
    TASK_CHECK(0 == strcmp(f3.back.cell[14][0].data, "A"));
    TASK_CHECK(0 == f2.back.cell[14][0].data[0]);
    TASK_CHECK(0 == f4.back.cell[14][0].data[0]);

    /* Pop-on text is hidden until EOC. */
    char text[4096];
    TASK_CHECK(front_text(d, cc608_cc1, text) && 0 == text[0]);
    TASK_CHECK(LIBCAPTION_READY == end_caption(d, cc608_cc1, 3.0));
    TASK_CHECK(front_text(d, cc608_cc1, text) && 0 == strcmp(text, "AA"));
    TASK_CHECK(front_text(d, cc608_cc3, text) && 0 == text[0]);

    cc608_decoder_free(d);
    return 0;
}

static int test_routing_channel_words_select_service(void)
{
    cc608_decoder_t* d = cc608_decoder_create();
    TASK_CHECK(d != 0);

    TASK_CHECK(LIBCAPTION_OK == load_service_text(d, cc608_cc1, "11", 1.0));
    TASK_CHECK(LIBCAPTION_OK == load_service_text(d, cc608_cc2, "22", 1.0));
    TASK_CHECK(LIBCAPTION_OK == load_service_text(d, cc608_cc3, "33", 1.0));
    TASK_CHECK(LIBCAPTION_OK == load_service_text(d, cc608_cc4, "44", 1.0));

    caption_frame_t f;
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc1, &f) && 0 == strcmp(f.back.cell[14][0].data, "1"));
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc2, &f) && 0 == strcmp(f.back.cell[14][0].data, "2"));
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc3, &f) && 0 == strcmp(f.back.cell[14][0].data, "3"));
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc4, &f) && 0 == strcmp(f.back.cell[14][0].data, "4"));

    /* A basic character inherits the most recent selection of its field. */
    uint16_t five = chars('5', '5');
    TASK_CHECK(LIBCAPTION_OK == decode_words(d, 2.0, cc_type_ntsc_cc_field_1, &five, 1));
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc2, &f) && 0 == strcmp(f.back.cell[14][2].data, "5"));
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc1, &f) && 0 == f.back.cell[14][2].data[0]);

    uint16_t six = chars('6', '6');
    TASK_CHECK(LIBCAPTION_OK == decode_words(d, 2.0, cc_type_ntsc_cc_field_2, &six, 1));
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc4, &f) && 0 == strcmp(f.back.cell[14][2].data, "6"));

    /* A PAC with a channel bit re-routes. */
    uint16_t p3 = pac_col(14, 0, 0);
    uint16_t seven = chars('7', '7');
    TASK_CHECK(parity_ok(p3));
    TASK_CHECK(LIBCAPTION_OK == decode_words(d, 3.0, cc_type_ntsc_cc_field_2, &p3, 1));
    TASK_CHECK(LIBCAPTION_OK == decode_words(d, 3.0, cc_type_ntsc_cc_field_2, &seven, 1));
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc3, &f) && 0 == strcmp(f.back.cell[14][0].data, "7"));

    /* A mid-row style change with a channel bit. */
    uint16_t m2 = midrow(eia608_style_green, 1, 1);
    uint16_t eight = chars('8', '8');
    TASK_CHECK(parity_ok(m2));
    TASK_CHECK(LIBCAPTION_OK == decode_words(d, 4.0, cc_type_ntsc_cc_field_1, &m2, 1));
    TASK_CHECK(LIBCAPTION_OK == decode_words(d, 4.0, cc_type_ntsc_cc_field_1, &eight, 1));
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc2, &f) && 0 == strcmp(f.back.cell[14][4].data, "8"));

    /* Special and extended characters carry their own channel bit. */
    uint16_t reg = special(0, 0); /* registered sign, selects CC1 */
    TASK_CHECK(parity_ok(reg));
    TASK_CHECK(LIBCAPTION_OK == decode_words(d, 5.0, cc_type_ntsc_cc_field_1, &reg, 1));
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc1, &f) && 0 == strcmp(f.back.cell[14][2].data, "\xC2\xAE"));

    /* Extended characters replace the previous cell, as in the legacy decoder,
       so on CC2 the cell at the current column minus one becomes the accent. */
    uint16_t acute = weu(0, 1);
    TASK_CHECK(parity_ok(acute));
    TASK_CHECK(LIBCAPTION_OK == decode_words(d, 5.0, cc_type_ntsc_cc_field_1, &acute, 1));
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc2, &f) && 0 == strcmp(f.back.cell[14][5].data, "\xC3\x81"));

    cc608_decoder_free(d);
    return 0;
}

static int test_routing_ignores_invalid_entries(void)
{
    cc608_decoder_t* d = cc608_decoder_create();
    TASK_CHECK(d != 0);
    TASK_CHECK(LIBCAPTION_OK == load_service_text(d, cc608_cc1, "AA", 1.0));
    TASK_CHECK(LIBCAPTION_OK == load_service_text(d, cc608_cc2, "BB", 1.0));
    TASK_CHECK(LIBCAPTION_OK == load_service_text(d, cc608_cc3, "CC", 1.0));
    TASK_CHECK(LIBCAPTION_OK == load_service_text(d, cc608_cc4, "DD", 1.0));

    cc608_decoder_t* before = cc608_decoder_create();
    TASK_CHECK(before != 0);
    size_t size = cc608_decoder_checkpoint_size(d);
    uint8_t* bytes = malloc(size);
    size_t got = 0;
    TASK_CHECK(bytes != 0);
    TASK_CHECK(cc608_decoder_checkpoint_export(d, bytes, size, &got));
    TASK_CHECK(cc608_decoder_checkpoint_restore(before, bytes, got));

    uint16_t routing_word = control(0x14, 0x20, 1);

    cea708_t invalid;
    cea708_init(&invalid, 2.0);
    cea708_add_cc_data(&invalid, 0, cc_type_ntsc_cc_field_1, routing_word);
    TASK_CHECK(LIBCAPTION_OK == cc608_decoder_decode(d, &invalid));
    TASK_CHECK(all_services_equal(d, before));

    cea708_t native;
    cea708_init(&native, 2.0);
    cea708_add_cc_data(&native, 1, cc_type_dtvcc_packet_data, routing_word);
    cea708_add_cc_data(&native, 1, cc_type_dtvcc_packet_start, routing_word);
    TASK_CHECK(LIBCAPTION_OK == cc608_decoder_decode(d, &native));
    TASK_CHECK(all_services_equal(d, before));

    cea708_t pad;
    cea708_init(&pad, 2.0);
    cea708_add_cc_data(&pad, 1, cc_type_ntsc_cc_field_1, word(0x00, 0x00));
    cea708_add_cc_data(&pad, 1, cc_type_ntsc_cc_field_2, word(0x00, 0x00));
    TASK_CHECK(LIBCAPTION_OK == cc608_decoder_decode(d, &pad));
    TASK_CHECK(all_services_equal(d, before));

    cea708_t other;
    cea708_init(&other, 2.0);
    other.user_identifier = DTG1;
    cea708_add_cc_data(&other, 1, cc_type_ntsc_cc_field_1, routing_word);
    TASK_CHECK(LIBCAPTION_OK == cc608_decoder_decode(d, &other));
    TASK_CHECK(all_services_equal(d, before));

    cea708_t off;
    cea708_init(&off, 2.0);
    off.user_data.process_cc_data_flag = 0;
    cea708_add_cc_data(&off, 1, cc_type_ntsc_cc_field_1, routing_word);
    TASK_CHECK(LIBCAPTION_OK == cc608_decoder_decode(d, &off));
    TASK_CHECK(all_services_equal(d, before));

    TASK_CHECK(LIBCAPTION_ERROR == cc608_decoder_decode(0, &invalid));
    TASK_CHECK(LIBCAPTION_ERROR == cc608_decoder_decode(d, 0));

    free(bytes);
    cc608_decoder_free(before);
    cc608_decoder_free(d);
    return 0;
}

static int test_routing_bad_parity_keeps_state(void)
{
    cc608_decoder_t* d = cc608_decoder_create();
    TASK_CHECK(d != 0);
    TASK_CHECK(LIBCAPTION_OK == load_service_text(d, cc608_cc1, "AB", 1.0));
    TASK_CHECK(LIBCAPTION_OK == load_service_text(d, cc608_cc2, "CD", 1.0));

    service_snapshot_t before;
    TASK_CHECK(snapshot_service(d, cc608_cc1, &before));

    /* One valid word then one bad parity word in the same object. */
    uint16_t words[2];
    words[0] = control(0x14, 0x20, 0);
    words[1] = (uint16_t)(chars('X', 'X') ^ 0x8000);
    TASK_CHECK(!parity_ok(words[1]));
    TASK_CHECK(LIBCAPTION_ERROR == decode_words(d, 2.0, cc_type_ntsc_cc_field_1, words, 2));

    service_snapshot_t after;
    TASK_CHECK(snapshot_service(d, cc608_cc1, &after));
    TASK_CHECK(0 == memcmp(&before.front, &after.front, sizeof(before.front)));
    TASK_CHECK(0 == memcmp(&before.back, &after.back, sizeof(before.back)));
    /* The earlier valid word was retained and became the recent word. */
    TASK_CHECK(words[0] == after.cc_data);

    /* A bad parity channel word must not re-route the field. */
    uint16_t bad_channel = (uint16_t)(control(0x14, 0x20, 1) ^ 0x8000);
    TASK_CHECK(!parity_ok(bad_channel));
    TASK_CHECK(LIBCAPTION_ERROR == decode_words(d, 3.0, cc_type_ntsc_cc_field_1, &bad_channel, 1));
    uint16_t plain = chars('E', 'E');
    TASK_CHECK(LIBCAPTION_OK == decode_words(d, 3.0, cc_type_ntsc_cc_field_1, &plain, 1));

    caption_frame_t f;
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc1, &f) && 0 == strcmp(f.back.cell[14][2].data, "E"));
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc2, &f) && 0 == f.back.cell[14][2].data[0]);

    cc608_decoder_free(d);
    return 0;
}

static int test_routing_repeat_control_per_service(void)
{
    cc608_decoder_t* d = cc608_decoder_create();
    TASK_CHECK(d != 0);
    TASK_CHECK(LIBCAPTION_OK == load_service_text(d, cc608_cc1, "AB", 1.0));
    TASK_CHECK(LIBCAPTION_OK == load_service_text(d, cc608_cc2, "CD", 1.0));

    /* Backspace is not idempotent: a second copy would erase another cell.
       The first call reports READY because the frame is in paint-on mode. */
    uint16_t bs1 = control(0x14, 0x21, 0); /* channel 1 of field 1: CC1 */
    TASK_CHECK(parity_ok(bs1));
    TASK_CHECK(LIBCAPTION_READY == decode_words(d, 2.0, cc_type_ntsc_cc_field_1, &bs1, 1));

    caption_frame_t f;
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc1, &f) && f.state.col == 1);

    /* Traffic aimed at the other service of the same field must not reset
       CC1's duplicate suppression. load_service_text on CC2 selects CC2 and
       ends with basic characters, so CC1 is untouched. */
    TASK_CHECK(LIBCAPTION_OK == load_service_text(d, cc608_cc2, "EF", 2.0));
    TASK_CHECK(LIBCAPTION_OK == load_service_text(d, cc608_cc3, "GH", 2.0));
    TASK_CHECK(LIBCAPTION_OK == load_service_text(d, cc608_cc4, "IJ", 2.0));

    /* CC1's most recent word is still bs1, so the repeat is suppressed and
       the status is OK rather than READY. */
    TASK_CHECK(LIBCAPTION_OK == decode_words(d, 3.0, cc_type_ntsc_cc_field_1, &bs1, 1));
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc1, &f) && f.state.col == 1);

    /* A third copy from a later object is also suppressed. */
    TASK_CHECK(LIBCAPTION_OK == decode_words(d, 4.0, cc_type_ntsc_cc_field_1, &bs1, 1));
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc1, &f) && f.state.col == 1);

    /* Ordinary text on the same service breaks repeat suppression: the same
       backspace now acts again and moves the cursor once more. */
    uint16_t more = chars('K', 'K');
    TASK_CHECK(LIBCAPTION_OK == decode_words(d, 4.0, cc_type_ntsc_cc_field_1, &more, 1));
    TASK_CHECK(LIBCAPTION_READY == decode_words(d, 5.0, cc_type_ntsc_cc_field_1, &bs1, 1));
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc1, &f) && f.state.col == 2);

    cc608_decoder_free(d);
    return 0;
}

static int test_routing_field2_xds_is_metadata(void)
{
    cc608_decoder_t* d = cc608_decoder_create();
    TASK_CHECK(d != 0);
    TASK_CHECK(LIBCAPTION_OK == load_service_text(d, cc608_cc3, "TV", 1.0));

    uint16_t xds[4];
    xds[0] = xds_start(0x01, 0x03);
    xds[1] = xds_payload(0x01, 0x02);
    xds[2] = xds_payload(0x03, 0x04);
    xds[3] = xds_end(0x2A);
    TASK_CHECK(parity_ok(xds[0]) && parity_ok(xds[1]) && parity_ok(xds[2]) && parity_ok(xds[3]));
    TASK_CHECK(LIBCAPTION_OK == decode_words(d, 2.0, cc_type_ntsc_cc_field_2, xds, ARRAY_COUNT(xds)));

    caption_frame_t f;
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc3, &f));
    char text[4096];
    TASK_CHECK(front_text(d, cc608_cc3, text) && 0 == text[0]);
    TASK_CHECK(back_text(d, cc608_cc3, text) && 0 == strcmp(text, "TV"));
    TASK_CHECK(0 == f.xds.state);
    TASK_CHECK(f.xds.size == 4);
    TASK_CHECK(f.xds.class_code == 0x01);
    TASK_CHECK(f.xds.type == 0x03);
    TASK_CHECK(f.xds.checksum == 0x2A);
    TASK_CHECK(f.xds.content[0] == 0x01 && f.xds.content[1] == 0x02);
    TASK_CHECK(f.xds.content[2] == 0x03 && f.xds.content[3] == 0x04);

    /* Routing still points at CC3: a basic character lands there. */
    uint16_t q = chars('Q', 'Q');
    TASK_CHECK(LIBCAPTION_OK == decode_words(d, 3.0, cc_type_ntsc_cc_field_2, &q, 1));
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc3, &f) && 0 == strcmp(f.back.cell[14][2].data, "Q"));

    /* XDS-looking data on field 1 is not XDS and must not touch CC1. */
    service_snapshot_t before, after;
    TASK_CHECK(snapshot_service(d, cc608_cc1, &before));
    TASK_CHECK(LIBCAPTION_OK == decode_words(d, 3.0, cc_type_ntsc_cc_field_1, xds, ARRAY_COUNT(xds)));
    TASK_CHECK(snapshot_service(d, cc608_cc1, &after));
    TASK_CHECK(snapshots_equal(&before, &after));

    cc608_decoder_free(d);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// Checkpoint group

typedef struct {
    cea708_cc_type_t type;
    double timestamp;
    uint16_t words[8];
    size_t count;
} batch_t;

/* Decode one script continuously, and a second time with an export/restore at
   the given word boundary. After the restore the two decoders are advanced in
   lockstep and their full four-service state is compared at every step, so a
   mismatch in characters, cursor, style, underline, mode, timestamp, XDS or
   repeat state is caught at the word where it first appears. */
static int replay_with_split(const batch_t* batches, size_t batch_count, size_t split)
{
    cc608_decoder_t* continuous = cc608_decoder_create();
    cc608_decoder_t* resumed = cc608_decoder_create();
    TASK_CHECK(continuous != 0 && resumed != 0);

    size_t item = 0;
    size_t total = 0;
    int restored = 0;

    for (size_t b = 0; b < batch_count; ++b) {
        for (size_t w = 0; w < batches[b].count; ++w) {
            /* Advance the continuous reference one word. */
            TASK_CHECK(LIBCAPTION_ERROR != decode_words(continuous, batches[b].timestamp, batches[b].type, &batches[b].words[w], 1));

            if (item < split) {
                TASK_CHECK(LIBCAPTION_ERROR != decode_words(resumed, batches[b].timestamp, batches[b].type, &batches[b].words[w], 1));
            }

            if (item + 1 == split) {
                /* Checkpoint the prefix and resume into a brand new instance. */
                size_t size = cc608_decoder_checkpoint_size(resumed);
                uint8_t* bytes = malloc(size);
                size_t got = 0;
                TASK_CHECK(bytes != 0);
                TASK_CHECK(cc608_decoder_checkpoint_export(resumed, bytes, size, &got));

                cc608_decoder_t* fresh = cc608_decoder_create();
                TASK_CHECK(fresh != 0);
                TASK_CHECK(cc608_decoder_checkpoint_restore(fresh, bytes, got));
                TASK_CHECK(all_services_equal(fresh, resumed));
                cc608_decoder_free(resumed);
                resumed = fresh;
                free(bytes);
                restored = 1;
            } else if (item >= split) {
                /* Suffix word after the resume point. */
                TASK_CHECK(LIBCAPTION_ERROR != decode_words(resumed, batches[b].timestamp, batches[b].type, &batches[b].words[w], 1));
            }

            if (restored) {
                TASK_CHECK(all_services_equal(continuous, resumed));
            }

            ++item;
            ++total;
        }
    }

    TASK_CHECK(item == total);
    TASK_CHECK(all_services_equal(continuous, resumed));

    cc608_decoder_free(continuous);
    cc608_decoder_free(resumed);
    return 0;
}

static int test_checkpoint_roundtrip_identity(void)
{
    cc608_decoder_t* d = cc608_decoder_create();
    TASK_CHECK(d != 0);
    TASK_CHECK(LIBCAPTION_OK == load_service_text(d, cc608_cc1, "HELLO", 1.0));
    TASK_CHECK(LIBCAPTION_OK == load_service_text(d, cc608_cc2, "WORLD", 1.5));
    TASK_CHECK(LIBCAPTION_OK == load_service_text(d, cc608_cc3, "THIRD", 2.0));
    TASK_CHECK(LIBCAPTION_OK == load_service_text(d, cc608_cc4, "FOUR", 2.5));
    TASK_CHECK(LIBCAPTION_READY == end_caption(d, cc608_cc1, 3.0));

    /* A timestamp with a long binary fraction must round trip bit exactly. */
    uint16_t bang = chars('!', '!');
    TASK_CHECK(LIBCAPTION_OK == decode_words(d, 1234.5678901234567, cc_type_ntsc_cc_field_1, &bang, 1));

    size_t size = cc608_decoder_checkpoint_size(d);
    TASK_CHECK(size > 0);
    uint8_t* bytes = malloc(size);
    TASK_CHECK(bytes != 0);
    size_t got = 0;
    TASK_CHECK(cc608_decoder_checkpoint_export(d, bytes, size, &got));
    TASK_CHECK(got == size);

    cc608_decoder_t* restored = cc608_decoder_create();
    TASK_CHECK(restored != 0);
    TASK_CHECK(cc608_decoder_checkpoint_restore(restored, bytes, got));
    TASK_CHECK(all_services_equal(d, restored));

    caption_frame_t a, b;
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc1, &a));
    TASK_CHECK(cc608_decoder_get_frame(restored, cc608_cc1, &b));
    TASK_CHECK(a.timestamp == b.timestamp);
    TASK_CHECK(a.timestamp == 1234.5678901234567);

    uint8_t* big = malloc(size + 16);
    TASK_CHECK(big != 0);
    TASK_CHECK(cc608_decoder_checkpoint_export(d, big, size + 16, &got));
    TASK_CHECK(got == size);
    TASK_CHECK(!cc608_decoder_checkpoint_export(d, big, size - 1, &got));
    TASK_CHECK(!cc608_decoder_checkpoint_export(0, big, size + 16, &got));
    TASK_CHECK(!cc608_decoder_checkpoint_export(d, 0, size, &got));
    TASK_CHECK(!cc608_decoder_checkpoint_export(d, big, size, 0));
    TASK_CHECK(0 == cc608_decoder_checkpoint_size(0));

    /* Self describing header: magic, version, byte order, field lengths. */
    TASK_CHECK(0 == memcmp(bytes, "C608", 4));
    TASK_CHECK(bytes[4] == 1);
    TASK_CHECK(bytes[5] == 1);
    TASK_CHECK(16 == (bytes[6] | (bytes[7] << 8)));
    TASK_CHECK(4 == (bytes[10] | (bytes[11] << 8)));
    TASK_CHECK((uint32_t)size == ((uint32_t)bytes[12] | ((uint32_t)bytes[13] << 8) | ((uint32_t)bytes[14] << 16) | ((uint32_t)bytes[15] << 24)));

    free(big);
    free(bytes);
    cc608_decoder_free(restored);
    cc608_decoder_free(d);
    return 0;
}

static int test_checkpoint_rejects_bad_bytes(void)
{
    cc608_decoder_t* d = cc608_decoder_create();
    TASK_CHECK(d != 0);
    TASK_CHECK(LIBCAPTION_OK == load_service_text(d, cc608_cc1, "KEEP", 1.0));
    TASK_CHECK(LIBCAPTION_OK == load_service_text(d, cc608_cc2, "ME", 1.0));

    size_t size = cc608_decoder_checkpoint_size(d);
    uint8_t* good = malloc(size);
    uint8_t* copy = malloc(size);
    TASK_CHECK(good != 0 && copy != 0);
    size_t got = 0;
    TASK_CHECK(cc608_decoder_checkpoint_export(d, good, size, &got));

    cc608_decoder_t* victim = cc608_decoder_create();
    TASK_CHECK(victim != 0);
    TASK_CHECK(LIBCAPTION_OK == load_service_text(victim, cc608_cc4, "SAFE", 9.0));
    service_snapshot_t before[CC608_SERVICE_COUNT];

    for (int s = 0; s < CC608_SERVICE_COUNT; ++s) {
        TASK_CHECK(snapshot_service(victim, (cc608_service_t)s, &before[s]));
    }

    /* Unknown version. */
    memcpy(copy, good, size);
    copy[4] = 2;
    TASK_CHECK(!cc608_decoder_checkpoint_restore(victim, copy, size));

    /* Wrong byte order marker. */
    memcpy(copy, good, size);
    copy[5] = 2;
    TASK_CHECK(!cc608_decoder_checkpoint_restore(victim, copy, size));

    /* Wrong header size, record size and record count. */
    const size_t header_fields[] = { 6, 8, 10 };

    for (size_t i = 0; i < ARRAY_COUNT(header_fields); ++i) {
        memcpy(copy, good, size);
        copy[header_fields[i]] ^= 0x10;
        TASK_CHECK(!cc608_decoder_checkpoint_restore(victim, copy, size));
    }

    /* Declared total length mismatch. */
    memcpy(copy, good, size);
    copy[12] ^= 0x10;
    TASK_CHECK(!cc608_decoder_checkpoint_restore(victim, copy, size));

    /* Truncated input and extra trailing bytes. */
    TASK_CHECK(!cc608_decoder_checkpoint_restore(victim, good, size - 1));
    TASK_CHECK(!cc608_decoder_checkpoint_restore(victim, good, 0));
    TASK_CHECK(!cc608_decoder_checkpoint_restore(victim, good, size + 1));

    /* Corruption anywhere is caught by the trailing checksum. */
    const size_t probes[] = { 0, 5, 11, 40, 100, size / 2, size - 8 };

    for (size_t i = 0; i < ARRAY_COUNT(probes); ++i) {
        memcpy(copy, good, size);
        copy[probes[i]] ^= 0x5A;
        TASK_CHECK(!cc608_decoder_checkpoint_restore(victim, copy, size));
    }

    TASK_CHECK(!cc608_decoder_checkpoint_restore(0, good, size));
    TASK_CHECK(!cc608_decoder_checkpoint_restore(victim, 0, size));

    for (int s = 0; s < CC608_SERVICE_COUNT; ++s) {
        service_snapshot_t after;
        TASK_CHECK(snapshot_service(victim, (cc608_service_t)s, &after));
        TASK_CHECK(snapshots_equal(&before[s], &after));
    }

    free(copy);
    free(good);
    cc608_decoder_free(victim);
    cc608_decoder_free(d);
    return 0;
}

/* FNV-1a over every checkpoint byte except the trailing checksum. */
static void fix_checksum(uint8_t* bytes, size_t size)
{
    uint32_t hash = 2166136261u;

    for (size_t b = 0; b < size - 4; ++b) {
        hash ^= bytes[b];
        hash *= 16777619u;
    }

    bytes[size - 4] = (uint8_t)(hash & 0xFF);
    bytes[size - 3] = (uint8_t)((hash >> 8) & 0xFF);
    bytes[size - 2] = (uint8_t)((hash >> 16) & 0xFF);
    bytes[size - 1] = (uint8_t)((hash >> 24) & 0xFF);
}

static int test_checkpoint_illegal_state_fields(void)
{
    /* Rewrite individual state bytes and repair the checksum so parsing reaches
       the semantic validation instead of rejecting the bytes as corrupt. The
       offsets below follow the documented header and record layouts, and the
       header itself supplies the record size used to reach later services. */
    cc608_decoder_t* d = cc608_decoder_create();
    TASK_CHECK(d != 0);
    TASK_CHECK(LIBCAPTION_OK == load_service_text(d, cc608_cc1, "CHK", 1.0));

    size_t size = cc608_decoder_checkpoint_size(d);
    uint8_t* header = malloc(size);
    size_t exported = 0;
    TASK_CHECK(size > 16 && header != 0 && cc608_decoder_checkpoint_export(d, header, size, &exported));

    const size_t header_size = (size_t)(header[6] | (header[7] << 8));
    const size_t record_size = (size_t)(header[8] | (header[9] << 8));
    TASK_CHECK(header_size == 16 && record_size > 0);
    free(header);

    enum {
        off_flags = 1,
        off_status = 2,
        off_row = 3,
        off_col = 4,
        off_sty = 5,
        off_uln = 6,
        off_rup = 7,
        off_xds_state = 8,
        off_xds_size = 11,
        off_timestamp = 17,
        off_front_cell_reserve = 60, /* first front cell's second byte */
        routing_tail = 2
    };

    struct probe {
        size_t offset;
        uint8_t value;
    };

    const struct probe probes[] = {
        { header_size + off_flags, 0x80 }, /* unknown flag bit */
        { header_size + off_status, 0x09 }, /* status out of enum */
        { header_size + off_row, 0x7F }, /* row past the screen */
        { header_size + off_col, 0x80 }, /* column out of range */
        { header_size + 2 * record_size + off_sty, 0x0F }, /* style overflow */
        { header_size + off_uln, 0x05 }, /* underline is one bit */
        { header_size + off_rup, 0x09 }, /* roll-up out of range */
        { header_size + off_xds_state, 0x07 }, /* XDS state out of range */
        { header_size + off_xds_size, 0x40 }, /* payload larger than 32 */
        { header_size + off_front_cell_reserve, 0x01 }, /* non-zero cell reserve byte */
        { size - 4 - routing_tail, 0x02 }, /* service index out of range */
    };

    for (size_t i = 0; i < ARRAY_COUNT(probes); ++i) {
        uint8_t* probe = malloc(size);
        TASK_CHECK(probe != 0);
        TASK_CHECK(cc608_decoder_checkpoint_export(d, probe, size, &exported));
        probe[probes[i].offset] = probes[i].value;
        fix_checksum(probe, size);

        cc608_decoder_t* target = cc608_decoder_create();
        TASK_CHECK(target != 0);
        TASK_CHECK(LIBCAPTION_OK == load_service_text(target, cc608_cc2, "STAY", 4.0));
        service_snapshot_t before, after;
        TASK_CHECK(snapshot_service(target, cc608_cc2, &before));
        TASK_CHECK(!cc608_decoder_checkpoint_restore(target, probe, size));
        TASK_CHECK(snapshot_service(target, cc608_cc2, &after));
        TASK_CHECK(snapshots_equal(&before, &after));
        cc608_decoder_free(target);
        free(probe);
    }

    /* Explicit NaN timestamp in service 0: little endian 0x7FF8000000000000. */
    {
        uint8_t* probe = malloc(size);
        static const uint8_t nan_bits[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x7F };
        TASK_CHECK(probe != 0);
        TASK_CHECK(cc608_decoder_checkpoint_export(d, probe, size, &exported));
        memcpy(&probe[header_size + off_timestamp], nan_bits, sizeof(nan_bits));
        fix_checksum(probe, size);

        cc608_decoder_t* target = cc608_decoder_create();
        TASK_CHECK(target != 0);
        TASK_CHECK(!cc608_decoder_checkpoint_restore(target, probe, size));
        cc608_decoder_free(target);
        free(probe);
    }

    cc608_decoder_free(d);
    return 0;
}

static int test_checkpoint_four_services_interleaved(void)
{
    batch_t batches[12];
    size_t n = 0;

    batches[n].type = cc_type_ntsc_cc_field_1;
    batches[n].timestamp = 1.0;
    batches[n].words[0] = control(0x14, 0x20, 0);
    batches[n].words[1] = chars('A', 'A');
    batches[n].count = 2;
    ++n;

    batches[n].type = cc_type_ntsc_cc_field_1;
    batches[n].timestamp = 1.0;
    batches[n].words[0] = control(0x14, 0x20, 1);
    batches[n].words[1] = chars('B', 'B');
    batches[n].count = 2;
    ++n;

    batches[n].type = cc_type_ntsc_cc_field_2;
    batches[n].timestamp = 1.0;
    batches[n].words[0] = control(0x14, 0x20, 0);
    batches[n].words[1] = chars('C', 'C');
    batches[n].count = 2;
    ++n;

    batches[n].type = cc_type_ntsc_cc_field_2;
    batches[n].timestamp = 1.0;
    batches[n].words[0] = control(0x14, 0x20, 1);
    batches[n].words[1] = chars('D', 'D');
    batches[n].count = 2;
    ++n;

    batches[n].type = cc_type_ntsc_cc_field_1;
    batches[n].timestamp = 2.0;
    batches[n].words[0] = midrow(eia608_style_blue, 0, 0);
    batches[n].words[1] = chars('E', 'E');
    batches[n].count = 2;
    ++n;

    batches[n].type = cc_type_ntsc_cc_field_2;
    batches[n].timestamp = 2.0;
    batches[n].words[0] = xds_start(0x01, 0x01);
    batches[n].words[1] = xds_payload(0x05, 0x06);
    batches[n].count = 2;
    ++n;

    batches[n].type = cc_type_ntsc_cc_field_2;
    batches[n].timestamp = 2.0;
    batches[n].words[0] = xds_payload(0x07, 0x08);
    batches[n].words[1] = xds_end(0x11);
    batches[n].count = 2;
    ++n;

    batches[n].type = cc_type_ntsc_cc_field_1;
    batches[n].timestamp = 3.0;
    batches[n].words[0] = control(0x14, 0x2F, 0);
    batches[n].words[1] = control(0x14, 0x2F, 0);
    batches[n].count = 2;
    ++n;

    batches[n].type = cc_type_ntsc_cc_field_1;
    batches[n].timestamp = 3.0;
    batches[n].words[0] = control(0x14, 0x2F, 1);
    batches[n].words[1] = chars('F', 'F');
    batches[n].count = 2;
    ++n;

    batches[n].type = cc_type_ntsc_cc_field_2;
    batches[n].timestamp = 4.0;
    batches[n].words[0] = pac_style(5, eia608_style_cyan, 1, 0);
    batches[n].words[1] = chars('G', 'G');
    batches[n].count = 2;
    ++n;

    batches[n].type = cc_type_ntsc_cc_field_2;
    batches[n].timestamp = 4.0;
    batches[n].words[0] = control(0x14, 0x2F, 0);
    batches[n].words[1] = control(0x14, 0x2F, 1);
    batches[n].count = 2;
    ++n;

    batches[n].type = cc_type_ntsc_cc_field_1;
    batches[n].timestamp = 5.0;
    batches[n].words[0] = chars('H', 'H');
    batches[n].count = 1;
    ++n;

    size_t total = 0;

    for (size_t i = 0; i < n; ++i) {
        total += batches[i].count;
    }

    /* Split at every word boundary, including the position between the two
       copies of the field 1 EOC and around the XDS packet. */
    for (size_t split = 0; split <= total; ++split) {
        TASK_CHECK(0 == replay_with_split(batches, n, split));
    }

    return 0;
}

static int test_checkpoint_preserves_write_target_and_back_text(void)
{
    cc608_decoder_t* d = cc608_decoder_create();
    TASK_CHECK(d != 0);
    TASK_CHECK(LIBCAPTION_OK == load_service_text(d, cc608_cc1, "HIDDEN", 1.0));

    caption_frame_t f;
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc1, &f));
    TASK_CHECK(f.write == &f.back);

    size_t size = cc608_decoder_checkpoint_size(d);
    uint8_t* bytes = malloc(size);
    size_t got = 0;
    TASK_CHECK(bytes != 0 && cc608_decoder_checkpoint_export(d, bytes, size, &got));

    cc608_decoder_t* restored = cc608_decoder_create();
    TASK_CHECK(restored != 0 && cc608_decoder_checkpoint_restore(restored, bytes, got));

    caption_frame_t r;
    TASK_CHECK(cc608_decoder_get_frame(restored, cc608_cc1, &r));
    TASK_CHECK(r.write == &r.back);
    char text[4096];
    TASK_CHECK(back_text(restored, cc608_cc1, text) && 0 == strcmp(text, "HIDDEN"));
    TASK_CHECK(front_text(restored, cc608_cc1, text) && 0 == text[0]);

    /* Finish with the EOC that was never sent before the split. */
    TASK_CHECK(LIBCAPTION_READY == end_caption(restored, cc608_cc1, 2.0));
    TASK_CHECK(front_text(restored, cc608_cc1, text) && 0 == strcmp(text, "HIDDEN"));

    free(bytes);
    cc608_decoder_free(restored);
    cc608_decoder_free(d);
    return 0;
}

static int test_checkpoint_eoc_split_across_restore(void)
{
    cc608_decoder_t* d = cc608_decoder_create();
    TASK_CHECK(d != 0);
    TASK_CHECK(LIBCAPTION_OK == load_service_text(d, cc608_cc1, "EOC", 1.0));

    /* First EOC before the split. */
    uint16_t eoc1 = control(0x14, 0x2F, 0);
    TASK_CHECK(LIBCAPTION_READY == decode_words(d, 2.0, cc_type_ntsc_cc_field_1, &eoc1, 1));

    size_t size = cc608_decoder_checkpoint_size(d);
    uint8_t* bytes = malloc(size);
    size_t got = 0;
    TASK_CHECK(bytes != 0 && cc608_decoder_checkpoint_export(d, bytes, size, &got));

    cc608_decoder_t* restored = cc608_decoder_create();
    TASK_CHECK(restored != 0 && cc608_decoder_checkpoint_restore(restored, bytes, got));

    /* The duplicate suppression state must survive the checkpoint, so the
       second copy of the same EOC is ignored rather than replaying the back
       buffer a second time. */
    caption_frame_t before, after;
    TASK_CHECK(cc608_decoder_get_frame(restored, cc608_cc1, &before));
    TASK_CHECK(LIBCAPTION_OK == decode_words(restored, 2.0, cc_type_ntsc_cc_field_1, &eoc1, 1));
    TASK_CHECK(cc608_decoder_get_frame(restored, cc608_cc1, &after));
    TASK_CHECK(before.state.cc_data == after.state.cc_data);
    TASK_CHECK(0 == memcmp(&before.front, &after.front, sizeof(before.front)));
    TASK_CHECK(0 == memcmp(&before.back, &after.back, sizeof(after.back)));

    char text[4096];
    TASK_CHECK(front_text(restored, cc608_cc1, text) && 0 == strcmp(text, "EOC"));

    /* Ordinary text on the same service breaks suppression; the next EOC then
       publishes the new hidden line. */
    uint16_t rcl = control(0x14, 0x20, 0);
    TASK_CHECK(LIBCAPTION_OK == decode_words(restored, 3.0, cc_type_ntsc_cc_field_1, &rcl, 1));
    uint16_t fresh = chars('N', 'O');
    TASK_CHECK(LIBCAPTION_OK == decode_words(restored, 3.0, cc_type_ntsc_cc_field_1, &fresh, 1));
    TASK_CHECK(LIBCAPTION_READY == decode_words(restored, 3.0, cc_type_ntsc_cc_field_1, &eoc1, 1));
    TASK_CHECK(front_text(restored, cc608_cc1, text) && 0 == strcmp(text, "NO"));

    free(bytes);
    cc608_decoder_free(restored);
    cc608_decoder_free(d);
    return 0;
}

static int test_checkpoint_eoc_per_service_split(void)
{
    /* Each of the four services receives its own end of caption twice, and the
       repeats are separated by checkpoint/restore boundaries so every field's
       duplicate suppression must be restored independently. */
    const cc608_service_t services[4] = { cc608_cc1, cc608_cc2, cc608_cc3, cc608_cc4 };
    const char* lines[4] = { "AA", "BB", "CC", "DD" };
    const double base[4] = { 1.0, 2.0, 3.0, 4.0 };

    cc608_decoder_t* continuous = cc608_decoder_create();
    cc608_decoder_t* resumed = cc608_decoder_create();
    TASK_CHECK(continuous != 0 && resumed != 0);

    /* Populate all four services with hidden text. */
    for (int s = 0; s < 4; ++s) {
        TASK_CHECK(LIBCAPTION_OK == load_service_text(continuous, services[s], lines[s], base[s]));
        TASK_CHECK(LIBCAPTION_OK == load_service_text(resumed, services[s], lines[s], base[s]));
    }

    for (int s = 0; s < 4; ++s) {
        uint16_t eoc = control(0x14, 0x2F, service_chan(services[s]));
        double t = 10.0 + s;

        /* First copy on both decoders. */
        TASK_CHECK(LIBCAPTION_READY == decode_words(continuous, t, service_type(services[s]), &eoc, 1));
        TASK_CHECK(LIBCAPTION_READY == decode_words(resumed, t, service_type(services[s]), &eoc, 1));

        /* Checkpoint between the two copies. */
        size_t size = cc608_decoder_checkpoint_size(resumed);
        uint8_t* bytes = malloc(size);
        size_t got = 0;
        TASK_CHECK(bytes != 0 && cc608_decoder_checkpoint_export(resumed, bytes, size, &got));

        cc608_decoder_t* fresh = cc608_decoder_create();
        TASK_CHECK(fresh != 0 && cc608_decoder_checkpoint_restore(fresh, bytes, got));
        TASK_CHECK(all_services_equal(fresh, resumed));
        cc608_decoder_free(resumed);
        resumed = fresh;
        free(bytes);

        /* Second copy must be suppressed on both. */
        TASK_CHECK(LIBCAPTION_OK == decode_words(continuous, t, service_type(services[s]), &eoc, 1));
        TASK_CHECK(LIBCAPTION_OK == decode_words(resumed, t, service_type(services[s]), &eoc, 1));
        TASK_CHECK(all_services_equal(continuous, resumed));
    }

    /* Each service displays its own line exactly once. */
    for (int s = 0; s < 4; ++s) {
        char text[4096];
        TASK_CHECK(front_text(continuous, services[s], text) && 0 == strcmp(text, lines[s]));
        TASK_CHECK(front_text(resumed, services[s], text) && 0 == strcmp(text, lines[s]));
    }

    cc608_decoder_free(continuous);
    cc608_decoder_free(resumed);
    return 0;
}

static int test_checkpoint_xds_mid_payload(void)
{
    cc608_decoder_t* d = cc608_decoder_create();
    TASK_CHECK(d != 0);
    TASK_CHECK(LIBCAPTION_OK == load_service_text(d, cc608_cc3, "NO", 1.0));

    /* Start plus one payload word before the split. */
    uint16_t head[2];
    head[0] = xds_start(0x02, 0x04);
    head[1] = xds_payload(0x09, 0x0A);
    TASK_CHECK(LIBCAPTION_OK == decode_words(d, 2.0, cc_type_ntsc_cc_field_2, head, ARRAY_COUNT(head)));

    caption_frame_t mid;
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc3, &mid));
    TASK_CHECK(1 == mid.xds.state);
    TASK_CHECK(mid.xds.size == 2);

    size_t size = cc608_decoder_checkpoint_size(d);
    uint8_t* bytes = malloc(size);
    size_t got = 0;
    TASK_CHECK(bytes != 0 && cc608_decoder_checkpoint_export(d, bytes, size, &got));

    cc608_decoder_t* restored = cc608_decoder_create();
    TASK_CHECK(restored != 0 && cc608_decoder_checkpoint_restore(restored, bytes, got));

    /* Continue the packet after the restore. */
    uint16_t tail[2];
    tail[0] = xds_payload(0x0B, 0x0C);
    tail[1] = xds_end(0x33);
    TASK_CHECK(LIBCAPTION_OK == decode_words(restored, 2.0, cc_type_ntsc_cc_field_2, tail, ARRAY_COUNT(tail)));

    caption_frame_t done;
    TASK_CHECK(cc608_decoder_get_frame(restored, cc608_cc3, &done));
    TASK_CHECK(0 == done.xds.state);
    TASK_CHECK(done.xds.size == 4);
    TASK_CHECK(done.xds.class_code == 0x02 && done.xds.type == 0x04);
    TASK_CHECK(done.xds.checksum == 0x33);
    TASK_CHECK(done.xds.content[0] == 0x09 && done.xds.content[1] == 0x0A);
    TASK_CHECK(done.xds.content[2] == 0x0B && done.xds.content[3] == 0x0C);

    /* The XDS bytes still did not reach the caption buffers. */
    char text[4096];
    TASK_CHECK(back_text(restored, cc608_cc3, text) && 0 == strcmp(text, "NO"));
    TASK_CHECK(front_text(restored, cc608_cc3, text) && 0 == text[0]);

    free(bytes);
    cc608_decoder_free(restored);
    cc608_decoder_free(d);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// Compatibility group

static int test_compatibility_legacy_single_frame(void)
{
    caption_frame_t frame;
    caption_frame_init(&frame);
    cea708_t cc;
    cea708_init(&cc, 3.0);
    cea708_add_cc_data(&cc, 1, cc_type_ntsc_cc_field_1, eia608_control_command(eia608_control_resume_caption_loading, 0));
    cea708_add_cc_data(&cc, 1, cc_type_ntsc_cc_field_1, eia608_from_utf8_2("A", "A"));
    cea708_add_cc_data(&cc, 1, cc_type_ntsc_cc_field_1, eia608_control_command(eia608_control_resume_caption_loading, 1));
    cea708_add_cc_data(&cc, 1, cc_type_ntsc_cc_field_1, eia608_from_utf8_2("B", "B"));
    cea708_add_cc_data(&cc, 1, cc_type_ntsc_cc_field_1, eia608_control_command(eia608_control_end_of_caption, 1));
    TASK_CHECK(LIBCAPTION_READY == cea708_to_caption_frame(&frame, &cc));

    char text[4096];
    caption_frame_to_text(&frame, text);
    TASK_CHECK(0 == strcmp(text, "AABB"));

    caption_frame_t one;
    caption_frame_init(&one);
    TASK_CHECK(LIBCAPTION_ERROR == caption_frame_decode(&one, 0x0000, 1.0));
    TASK_CHECK(LIBCAPTION_OK == caption_frame_decode(&one, 0x8080, 1.0));
    TASK_CHECK(LIBCAPTION_OK == caption_frame_decode(&one, eia608_control_command(eia608_control_resume_caption_loading, 0), 1.0));
    /* Pop-on writes into the hidden buffer, so a text word is only OK. */
    TASK_CHECK(LIBCAPTION_OK == caption_frame_decode(&one, eia608_from_utf8_2("A", "A"), 1.0));
    /* Paint-on makes text READY immediately. */
    TASK_CHECK(LIBCAPTION_OK == caption_frame_decode(&one, eia608_control_command(eia608_control_resume_direct_captioning, 0), 1.0));
    TASK_CHECK(LIBCAPTION_READY == caption_frame_decode(&one, eia608_from_utf8_2("B", "B"), 1.0));
    return 0;
}

static int test_compatibility_ga94_roundtrip(void)
{
    cea708_t cc;
    cea708_init(&cc, 4.0);
    cea708_add_cc_data(&cc, 1, cc_type_ntsc_cc_field_1, control(0x14, 0x20, 0));
    cea708_add_cc_data(&cc, 1, cc_type_ntsc_cc_field_1, chars('O', 'K'));
    cea708_add_cc_data(&cc, 1, cc_type_ntsc_cc_field_2, control(0x14, 0x20, 0));
    cea708_add_cc_data(&cc, 1, cc_type_ntsc_cc_field_2, chars('N', 'O'));

    uint8_t data[512];
    int size = cea708_render(&cc, data, sizeof(data));
    TASK_CHECK(size > 0);

    cea708_t wire;
    cea708_init(&wire, 4.0);
    TASK_CHECK(LIBCAPTION_ERROR != cea708_parse_h264(data, (size_t)size, &wire));

    cc608_decoder_t* d = cc608_decoder_create();
    TASK_CHECK(d != 0);
    TASK_CHECK(LIBCAPTION_OK == cc608_decoder_decode(d, &wire));

    caption_frame_t f;
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc1, &f) && 0 == strcmp(f.back.cell[14][0].data, "O"));
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc3, &f) && 0 == strcmp(f.back.cell[14][0].data, "N"));
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc2, &f) && 0 == f.back.cell[14][0].data[0]);

    /* The legacy path still merges both channels of field 1 into one frame,
       and keeps pop-on text in the hidden buffer until an EOC. */
    caption_frame_t legacy;
    caption_frame_init(&legacy);
    TASK_CHECK(LIBCAPTION_ERROR != cea708_to_caption_frame(&legacy, &wire));
    char text[4096];
    TASK_CHECK(legacy.write == &legacy.back);
    TASK_CHECK(0 == strcmp(legacy.back.cell[14][0].data, "O") && 0 == strcmp(legacy.back.cell[14][1].data, "K"));
    TASK_CHECK(LIBCAPTION_READY == caption_frame_decode(&legacy, eia608_control_command(eia608_control_end_of_caption, 0), 4.0));
    caption_frame_to_text(&legacy, text);
    TASK_CHECK(0 == strcmp(text, "OK"));

    cc608_decoder_free(d);
    return 0;
}

static int test_compatibility_get_frame_isolated(void)
{
    cc608_decoder_t* d = cc608_decoder_create();
    TASK_CHECK(d != 0);
    TASK_CHECK(LIBCAPTION_OK == load_service_text(d, cc608_cc1, "COPY", 1.0));
    TASK_CHECK(LIBCAPTION_READY == end_caption(d, cc608_cc1, 1.0));

    caption_frame_t snapshot;
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc1, &snapshot));
    /* After EOC the write target remains the hidden buffer, matching the
       legacy decoder, and the visible text is on the front buffer. */
    TASK_CHECK(snapshot.write == &snapshot.back);
    TASK_CHECK(0 == strcmp(snapshot.front.cell[14][0].data, "C"));

    snapshot.front.cell[14][0].data[0] = 'X';

    char text[4096];
    TASK_CHECK(front_text(d, cc608_cc1, text) && 0 == strcmp(text, "COPY"));
    TASK_CHECK(0 == strcmp(snapshot.front.cell[14][0].data, "X"));

    /* Later input, reset and free must not change the copy. The new word goes
       into the hidden back buffer, leaving the visible line alone. */
    uint16_t more = chars('Z', 'Z');
    TASK_CHECK(LIBCAPTION_OK == decode_words(d, 2.0, cc_type_ntsc_cc_field_1, &more, 1));
    TASK_CHECK(front_text(d, cc608_cc1, text) && 0 == strcmp(text, "COPY"));
    TASK_CHECK(back_text(d, cc608_cc1, text) && 0 == strcmp(text, "ZZ"));
    TASK_CHECK(0 == strcmp(snapshot.front.cell[14][0].data, "X"));

    caption_frame_t snapshot2;
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc1, &snapshot2));
    cc608_decoder_reset(d);
    TASK_CHECK(front_text(d, cc608_cc1, text) && 0 == text[0]);
    TASK_CHECK(0 == strcmp(snapshot2.front.cell[14][0].data, "C"));
    TASK_CHECK(snapshot2.write == &snapshot2.back);

    cc608_decoder_free(d);
    TASK_CHECK(0 == strcmp(snapshot2.front.cell[14][0].data, "C"));

    /* A snapshot of an unused service has no write target. */
    cc608_decoder_t* fresh = cc608_decoder_create();
    TASK_CHECK(fresh != 0);
    caption_frame_t empty;
    TASK_CHECK(cc608_decoder_get_frame(fresh, cc608_cc2, &empty));
    TASK_CHECK(0 == empty.write);
    TASK_CHECK(!cc608_decoder_get_frame(fresh, (cc608_service_t)4, &empty));
    TASK_CHECK(!cc608_decoder_get_frame(fresh, cc608_cc1, 0));
    TASK_CHECK(!cc608_decoder_get_frame(0, cc608_cc1, &empty));
    cc608_decoder_free(fresh);
    return 0;
}

static int test_compatibility_after_bad_snapshot_decodes(void)
{
    cc608_decoder_t* d = cc608_decoder_create();
    TASK_CHECK(d != 0);
    TASK_CHECK(LIBCAPTION_OK == load_service_text(d, cc608_cc1, "GOOD", 1.0));

    size_t size = cc608_decoder_checkpoint_size(d);
    uint8_t* bytes = malloc(size);
    size_t got = 0;
    TASK_CHECK(bytes != 0 && cc608_decoder_checkpoint_export(d, bytes, size, &got));
    bytes[size / 2] ^= 0xFF;
    TASK_CHECK(!cc608_decoder_checkpoint_restore(d, bytes, got));

    uint16_t more = chars('!', '!');
    TASK_CHECK(LIBCAPTION_OK == decode_words(d, 2.0, cc_type_ntsc_cc_field_1, &more, 1));
    TASK_CHECK(LIBCAPTION_READY == end_caption(d, cc608_cc1, 2.0));

    char text[4096];
    TASK_CHECK(front_text(d, cc608_cc1, text) && 0 == strcmp(text, "GOOD!!"));

    free(bytes);
    cc608_decoder_free(d);
    return 0;
}

static int test_compatibility_reset_and_reuse(void)
{
    cc608_decoder_t* d = cc608_decoder_create();
    TASK_CHECK(d != 0);
    TASK_CHECK(LIBCAPTION_OK == load_service_text(d, cc608_cc2, "TWO", 1.0));
    TASK_CHECK(LIBCAPTION_OK == load_service_text(d, cc608_cc4, "FOUR", 1.0));

    cc608_decoder_reset(d);

    /* After reset each field is back on its first service. */
    uint16_t rcl = control(0x14, 0x20, 0);
    uint16_t aa = chars('A', 'A');
    TASK_CHECK(LIBCAPTION_OK == decode_words(d, 1.0, cc_type_ntsc_cc_field_1, &rcl, 1));
    TASK_CHECK(LIBCAPTION_OK == decode_words(d, 1.0, cc_type_ntsc_cc_field_1, &aa, 1));
    TASK_CHECK(LIBCAPTION_OK == decode_words(d, 1.0, cc_type_ntsc_cc_field_2, &rcl, 1));
    TASK_CHECK(LIBCAPTION_OK == decode_words(d, 1.0, cc_type_ntsc_cc_field_2, &aa, 1));

    caption_frame_t f;
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc1, &f) && 0 == strcmp(f.back.cell[14][0].data, "A"));
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc3, &f) && 0 == strcmp(f.back.cell[14][0].data, "A"));
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc2, &f) && 0 == f.back.cell[14][0].data[0]);
    TASK_CHECK(cc608_decoder_get_frame(d, cc608_cc4, &f) && 0 == f.back.cell[14][0].data[0]);

    cc608_decoder_free(d);
    cc608_decoder_free(0);
    cc608_decoder_reset(0);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// Case registry

const struct task_case task_cases[] = {
    { "routing", "field_defaults", test_routing_field_defaults },
    { "routing", "channel_words_select_service", test_routing_channel_words_select_service },
    { "routing", "ignores_invalid_entries", test_routing_ignores_invalid_entries },
    { "routing", "bad_parity_keeps_state", test_routing_bad_parity_keeps_state },
    { "routing", "repeat_control_per_service", test_routing_repeat_control_per_service },
    { "routing", "field2_xds_is_metadata", test_routing_field2_xds_is_metadata },
    { "checkpoint", "roundtrip_identity", test_checkpoint_roundtrip_identity },
    { "checkpoint", "rejects_bad_bytes", test_checkpoint_rejects_bad_bytes },
    { "checkpoint", "illegal_state_fields", test_checkpoint_illegal_state_fields },
    { "checkpoint", "four_services_interleaved", test_checkpoint_four_services_interleaved },
    { "checkpoint", "preserves_write_target_and_back_text", test_checkpoint_preserves_write_target_and_back_text },
    { "checkpoint", "eoc_split_across_restore", test_checkpoint_eoc_split_across_restore },
    { "checkpoint", "eoc_per_service_split", test_checkpoint_eoc_per_service_split },
    { "checkpoint", "xds_mid_payload", test_checkpoint_xds_mid_payload },
    { "compatibility", "legacy_single_frame", test_compatibility_legacy_single_frame },
    { "compatibility", "ga94_roundtrip", test_compatibility_ga94_roundtrip },
    { "compatibility", "get_frame_isolated", test_compatibility_get_frame_isolated },
    { "compatibility", "after_bad_snapshot_decodes", test_compatibility_after_bad_snapshot_decodes },
    { "compatibility", "reset_and_reuse", test_compatibility_reset_and_reuse },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
