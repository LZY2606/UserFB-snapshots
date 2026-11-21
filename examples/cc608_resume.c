/* Demonstrates the independent CC1..CC4 decoder and its resumable checkpoint.
 *
 * The program decodes an interleaved field 1 / field 2 script on one decoder,
 * exports the checkpoint, creates a fresh decoder, restores the checkpoint and
 * feeds the remaining words. The final text of all four services is printed
 * from both decoders so they can be compared.
 */
#include "caption/cc608_decoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_service(const cc608_decoder_t* decoder, cc608_service_t service, const char* tag)
{
    caption_frame_t frame;
    char text[CAPTION_FRAME_TEXT_BYTES];
    cc608_decoder_get_frame(decoder, service, &frame);
    caption_frame_to_text(&frame, text);
    printf("%s CC%d: '%s'\n", tag, (int)service + 1, text);
}

static int send(cc608_decoder_t* decoder, cea708_t* cc, cea708_cc_type_t type, uint16_t word_)
{
    cea708_init(cc, cc->timestamp);
    cea708_add_cc_data(cc, 1, type, word_);
    return cc608_decoder_decode(decoder, cc);
}

int main(void)
{
    cc608_decoder_t* live = cc608_decoder_create();
    cc608_decoder_t* resumed = cc608_decoder_create();

    if (!live || !resumed) {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }

    cea708_t cc;
    cea708_init(&cc, 10.0);

    /* Field 1 carries CC1 and CC2; field 2 carries CC3 and CC4. A control
       word's channel bit selects the service of its field, and basic
       characters then follow that selection. The helpers add odd parity. */
    struct step {
        cea708_cc_type_t type;
        uint16_t word;
    };

    struct step first[9];
    first[0].type = cc_type_ntsc_cc_field_1;
    first[0].word = eia608_control_command(eia608_control_resume_caption_loading, 0);
    first[1].type = cc_type_ntsc_cc_field_1;
    first[1].word = eia608_from_utf8_2("A", "B"); /* hidden on CC1 */
    first[2].type = cc_type_ntsc_cc_field_1;
    first[2].word = eia608_control_command(eia608_control_end_of_caption, 0);
    first[3].type = cc_type_ntsc_cc_field_1;
    first[3].word = eia608_control_command(eia608_control_resume_caption_loading, 1);
    first[4].type = cc_type_ntsc_cc_field_1;
    first[4].word = eia608_from_utf8_2("C", "D"); /* hidden on CC2 */
    first[5].type = cc_type_ntsc_cc_field_2;
    first[5].word = eia608_control_command(eia608_control_resume_caption_loading, 0);
    first[6].type = cc_type_ntsc_cc_field_2;
    first[6].word = eia608_from_utf8_2("E", "F"); /* hidden on CC3 */
    first[7].type = cc_type_ntsc_cc_field_2;
    first[7].word = eia608_control_command(eia608_control_resume_caption_loading, 1);
    first[8].type = cc_type_ntsc_cc_field_2;
    first[8].word = eia608_from_utf8_2("G", "H"); /* hidden on CC4 */

    for (size_t i = 0; i < sizeof(first) / sizeof(first[0]); ++i) {
        cc.timestamp = 10.0 + (double)i;
        send(live, &cc, first[i].type, first[i].word);
    }

    /* Save while CC2, CC3 and CC4 still have un-published back buffers. */
    size_t size = cc608_decoder_checkpoint_size(live);
    uint8_t* bytes = malloc(size);
    size_t written = 0;

    if (!bytes || !cc608_decoder_checkpoint_export(live, bytes, size, &written)) {
        fprintf(stderr, "checkpoint export failed\n");
        return 1;
    }

    printf("checkpoint: %zu bytes\n", written);

    if (!cc608_decoder_checkpoint_restore(resumed, bytes, written)) {
        fprintf(stderr, "checkpoint restore failed\n");
        return 1;
    }

    /* Continue on the restored instance: publish each hidden line. */
    struct step rest[3];
    rest[0].type = cc_type_ntsc_cc_field_1;
    rest[0].word = eia608_control_command(eia608_control_end_of_caption, 1);
    rest[1].type = cc_type_ntsc_cc_field_2;
    rest[1].word = eia608_control_command(eia608_control_end_of_caption, 0);
    rest[2].type = cc_type_ntsc_cc_field_2;
    rest[2].word = eia608_control_command(eia608_control_end_of_caption, 1);

    for (size_t i = 0; i < sizeof(rest) / sizeof(rest[0]); ++i) {
        cc.timestamp = 20.0 + (double)i;
        send(resumed, &cc, rest[i].type, rest[i].word);
    }

    /* Compare against a continuous run of the whole script. */
    cc608_decoder_t* continuous = cc608_decoder_create();

    if (!continuous) {
        return 1;
    }

    for (size_t i = 0; i < sizeof(first) / sizeof(first[0]); ++i) {
        cc.timestamp = 10.0 + (double)i;
        send(continuous, &cc, first[i].type, first[i].word);
    }

    for (size_t i = 0; i < sizeof(rest) / sizeof(rest[0]); ++i) {
        cc.timestamp = 20.0 + (double)i;
        send(continuous, &cc, rest[i].type, rest[i].word);
    }

    for (int service = 0; service < CC608_SERVICE_COUNT; ++service) {
        print_service(continuous, (cc608_service_t)service, "continuous");
        print_service(resumed, (cc608_service_t)service, "resumed   ");
    }

    free(bytes);
    cc608_decoder_free(continuous);
    cc608_decoder_free(resumed);
    cc608_decoder_free(live);
    return 0;
}
