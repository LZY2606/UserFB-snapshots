/*
 * libmad - MPEG audio decoder library
 *
 * ADU (Audio Data Unit) packetization, per RFC 5219 section 4.1.
 *
 * An ADU is the frame header, the optional CRC word and the side
 * information of one physical MPEG-1/2 Layer III frame (kept byte for
 * byte identical to the input frame), followed by that frame's main
 * data segment measured on the continuous main-data slot axis formed by
 * concatenating every physical frame's main-data slot (the bytes left
 * after header, optional CRC and side information).
 */

# ifndef LIBMAD_ADU_H
# define LIBMAD_ADU_H

# ifdef __cplusplus
extern "C" {
# endif

/*
 * An ADU packer consumes an arbitrary sequence of byte fragments of a
 * continuous MPEG-1/2 Layer III stream (without ID3/APE tags, without
 * free-format frames and without MPEG-2.5) and emits one complete ADU
 * per physical frame through the emit callback.  Memory usage is bounded
 * independently of stream length.
 */
struct mad_adu_packer;

/*
 * NAME:	mad_adu_packer_new()
 * DESCRIPTION:	create an ADU packer
 *
 * The emit callback receives one complete ADU at a time; the buffer is
 * only valid for the duration of the callback.  A non-zero return value
 * from the callback permanently terminates the instance.
 *
 * Returns 0 when memory could not be allocated.
 */
struct mad_adu_packer *mad_adu_packer_new(
    int (*emit)(void *, unsigned char const *, unsigned long),
    void *user);

/*
 * NAME:	mad_adu_packer_feed()
 * DESCRIPTION:	hand an arbitrary-length byte fragment to the packer
 *
 * A zero-length fragment does not advance the stream.  Returns 0 on
 * success and -1 on any parse, truncation, allocation or callback
 * failure; after a failure the instance is permanently stopped and no
 * further callbacks are made.
 */
int mad_adu_packer_feed(struct mad_adu_packer *,
			unsigned char const *, unsigned long);

/*
 * NAME:	mad_adu_packer_finish()
 * DESCRIPTION:	flush the final, previously undeliverable ADU
 *
 * An empty stream finishes successfully without any callback.  A
 * truncated partial frame makes finish fail.  Calling finish more than
 * once is harmless: the last ADU is delivered at most once and the
 * repeated call returns the same status.  After a successful finish no
 * new data is accepted.
 */
int mad_adu_packer_finish(struct mad_adu_packer *);

/*
 * NAME:	mad_adu_packer_free()
 * DESCRIPTION:	destroy a packer and release all associated memory
 */
void mad_adu_packer_free(struct mad_adu_packer *);

# ifdef __cplusplus
}
# endif

# endif
