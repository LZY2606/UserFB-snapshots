/*
 * Stereo sample support — interface notes (documentation, not built).
 *
 * Frames vs. scalar points
 * ------------------------
 * A sample is a sequence of frames. A mono frame holds one scalar sample
 * point; a stereo frame holds two independent points, one per plane.
 *
 *   - xm_sample_t.length, loop_start and loop_end are always counts of
 *     frames, for both 8-bit and 16-bit data. A 16-bit length is never
 *     divided by two.
 *   - ctx->module.samples_data_length and the LIBXMIF header 0x10 count
 *     scalar points, so a stereo frame contributes two points.
 *   - Stereo sample data is laid out as two contiguous planes inside
 *     ctx->samples_data: the whole retained left plane first, then the
 *     whole retained right plane. Plane c of a sample therefore lives at
 *     index (sample.index + c * sample.length) and holds sample.length
 *     points.
 *
 * Planes and playback
 * -------------------
 * A module channel still owns a single note, a single sample phase and one
 * set of effect state. For a stereo sample the two planes share that phase:
 * interpolation, looping, the 9xx sample offset and retriggering all act at
 * the same frame position for the left and right plane, and one output frame
 * advances the phase exactly once. Stereo is never split into two tracker
 * channels and is never down-mixed to mono.
 *
 * The existing volume, global-volume and panning math is unchanged. The
 * left plane is multiplied by the existing left gain, the right plane by
 * the existing right gain. A mono sample keeps feeding its single plane to
 * both output sides, so a stereo sample whose two planes are identical
 * yields byte-identical PCM to the equivalent mono sample (there is no
 * extra centre-pan compensation).
 *
 * On a note change the ramping tail is stored per output plane, so switching
 * between stereo samples, back to mono, or to an empty sample never lets the
 * left sound replace the right tail.
 *
 * Public accessors
 * ----------------
 *   uint8_t xm_get_sample_channels(ctx, sample);
 *       1 for mono, 2 for stereo, 0 for an invalid sample number.
 *
 *   xm_sample_point_t*
 *   xm_get_sample_waveform_channel(ctx, sample, channel, &out_length);
 *       Returns the writeable plane (channel 0 = left, 1 = right); NULL on
 *       an invalid sample/channel, in which case *out_length is set to 0
 *       when out_length is non-NULL. out_length is a frame count and may be
 *       queried with a NULL length pointer. The pointer aliases the live
 *       sample storage — edits are audible on the next generated frame and
 *       no temporary plane is copied per call.
 *
 *   xm_get_sample_waveform(ctx, sample, &out_length) is unchanged in
 *       signature and keeps returning the mono data, or the left plane of a
 *       stereo sample, with out_length expressed in frames.
 *
 * LIBXMIF save format
 * -------------------
 * A module with only mono samples is still written as format version 0; its
 * bytes are unchanged apart from the corrected 16-bit S3M length handling.
 * As soon as at least one stereo sample is present the version byte (header
 * offset 1) becomes 1.
 *
 * In version 1:
 *   - header 0x10 holds the total number of scalar sample points;
 *   - sample-record flag byte 0x0C gains bit 1 to mark a stereo sample
 *     (bit 0 remains the ping-pong flag);
 *   - the record index points at the left plane, length/loop_length stay
 *     frame counts, and the right plane is written immediately after the
 *     retained left plane;
 *   - a mono record in a version 1 file is still interpreted as one plane.
 *
 * Version 0 files are always read as mono. xm_dump_context()/
 * xm_restore_context() preserve both planes and the half-tick playback
 * position within a build; the dump binary is not portable across builds.
 */
