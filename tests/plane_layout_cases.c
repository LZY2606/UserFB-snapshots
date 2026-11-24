/* Direct per-plane stride output tests for libmpeg2. */

#include "task_support/check.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "mpeg2.h"
#include "mpeg2convert.h"
#include "task_support/fixture.h"
#include "interlaced_fixture.h"

#define MAX_PLANES 80
#define MAX_FRAMES 16
#define CHUNK 37
#define GUARD_BYTE 0xa7
#define FRONT_GUARD 64

typedef struct {
    uint8_t * raw[3];
    size_t raw_size[3];
    uint8_t * p[3];
    int stride[3];
    int width[3];
    int height[3];
    int tag;
} Plane;

typedef struct {
    int tag;
    unsigned temporal;
    uint32_t flags;
    int width, height;
    int chroma_width, chroma_height;
    int stride[3];
    uint8_t * data[3];
} Frame;

enum session_mode {
    MODE_PADDED,
    MODE_TIGHT_NEW,
    MODE_OLD,
    MODE_MIXED,
    MODE_AUTO,
    MODE_STRIDE
};

typedef struct {
    mpeg2dec_t * dec;
    const mpeg2_info_t * info;
    int mode;
    int reject_probes;
    int width, height, chroma_width, chroma_height;
    int set_stride;
    int plane_count;
    Plane * planes[MAX_PLANES];
    int registration_count;
    int picture_second_count;
    int frame_count;
    Frame frames[MAX_FRAMES];
    const void * last_picture;
} Session;

static uintptr_t align16_up (uintptr_t value)
{
    return (value + 15) & ~(uintptr_t) 15;
}

static Plane * plane_new (int tag, int padded,
			  int width, int height,
			  int chroma_width, int chroma_height)
{
    Plane * plane = (Plane *) calloc (1, sizeof (*plane));
    int widths[3] = { width, chroma_width, chroma_width };
    int heights[3] = { height, chroma_height, chroma_height };
    int j;

    if (!plane)
	return NULL;
    plane->tag = tag;

    for (j = 0; j < 3; j++) {
	size_t body;
	size_t total;
	uintptr_t aligned;

	plane->width[j] = widths[j];
	plane->height[j] = heights[j];
	if (padded) {
	    if (j == 0)
		plane->stride[j] = widths[j] + 32 + (tag * 11) % 48;
	    else if (j == 1)
		plane->stride[j] = widths[j] + 13 + (tag * 7) % 31;
	    else
		plane->stride[j] = widths[j] + 29 + (tag * 5) % 37;
	} else
	    plane->stride[j] = widths[j];
	if (padded && j == 2 && plane->stride[2] == plane->stride[1])
	    plane->stride[2] += 4;

	body = (size_t) plane->stride[j] * heights[j];
	total = FRONT_GUARD + 64 + body + 64;
	plane->raw_size[j] = total;
	plane->raw[j] = (uint8_t *) malloc (total);
	if (!plane->raw[j]) {
	    int k;
	    for (k = 0; k < j; k++)
		free (plane->raw[k]);
	    free (plane);
	    return NULL;
	}
	memset (plane->raw[j], GUARD_BYTE, total);
	aligned = align16_up ((uintptr_t) plane->raw[j] + FRONT_GUARD);
	plane->p[j] = (uint8_t *) aligned;
    }

    return plane;
}

static void plane_free (Plane * plane)
{
    int j;
    if (!plane)
	return;
    for (j = 0; j < 3; j++)
	free (plane->raw[j]);
    free (plane);
}

static int plane_guards_ok (const Plane * plane)
{
    int j;

    for (j = 0; j < 3; j++) {
	const uint8_t * start = plane->p[j];
	const uint8_t * end_used;
	size_t front = (size_t) (start - plane->raw[j]);
	size_t used = (size_t) plane->stride[j] * plane->height[j];
	size_t back;
	size_t k;
	int y, x;

	if (front >= plane->raw_size[j])
	    return 0;
	for (k = 0; k < front; k++)
	    if (plane->raw[j][k] != GUARD_BYTE)
		return 0;

	end_used = start + used;
	if ((size_t) (end_used - plane->raw[j]) > plane->raw_size[j])
	    return 0;
	back = plane->raw_size[j] - (size_t) (end_used - plane->raw[j]);
	for (k = 0; k < back; k++)
	    if (end_used[k] != GUARD_BYTE)
		return 0;

	for (y = 0; y < plane->height[j]; y++) {
	    const uint8_t * row = start + (size_t) y * plane->stride[j];
	    for (x = plane->width[j]; x < plane->stride[j]; x++)
		if (row[x] != GUARD_BYTE)
		    return 0;
	}
    }
    return 1;
}

static void session_init (Session * session, int mode)
{
    memset (session, 0, sizeof (*session));
    session->mode = mode;
    mpeg2_accel (0);
    session->dec = mpeg2_init ();
    if (session->dec)
	session->info = mpeg2_info (session->dec);
}

static void session_destroy (Session * session)
{
    int i, j;

    if (session->dec)
	mpeg2_close (session->dec);
    for (i = 0; i < session->plane_count; i++)
	plane_free (session->planes[i]);
    for (i = 0; i < session->frame_count; i++)
	for (j = 0; j < 3; j++)
	    free (session->frames[i].data[j]);
}

static int session_custom (const Session * session)
{
    return session->mode == MODE_PADDED ||
	   session->mode == MODE_TIGHT_NEW ||
	   session->mode == MODE_OLD ||
	   session->mode == MODE_MIXED;
}

static int register_plane (Session * session)
{
    int tag = session->plane_count + 1;
    int padded;
    int use_new;
    Plane * plane;
    uint8_t * buffers[3];
    int local_strides[3];
    int rc = 0;

    if (session->plane_count >= MAX_PLANES)
	return -1;

    padded = (session->mode == MODE_PADDED);
    if (session->mode == MODE_MIXED)
	padded = (tag & 1);
    plane = plane_new (tag, padded, session->width, session->height,
		       session->chroma_width, session->chroma_height);
    if (!plane)
	return -1;
    session->planes[session->plane_count++] = plane;

    buffers[0] = plane->p[0];
    buffers[1] = plane->p[1];
    buffers[2] = plane->p[2];
    local_strides[0] = plane->stride[0];
    local_strides[1] = plane->stride[1];
    local_strides[2] = plane->stride[2];

    use_new = (session->mode == MODE_PADDED ||
	       session->mode == MODE_TIGHT_NEW);
    if (session->mode == MODE_MIXED)
	use_new = (tag & 1);

    if (use_new) {
	rc = mpeg2_set_buf_strides (session->dec, buffers, local_strides,
				    plane);
	/* The values were copied; mutating our array must not matter. */
	local_strides[0] = -1;
	local_strides[1] = -2;
	local_strides[2] = -3;
    } else {
	mpeg2_set_buf (session->dec, buffers, plane);
    }
    session->registration_count++;
    return rc;
}

static int reject_probe (Session * session)
{
    Plane * probe;
    uint8_t * buffers[3];
    int strides[3];

    probe = plane_new (900 + session->registration_count, 1,
		       session->width, session->height,
		       session->chroma_width, session->chroma_height);
    TASK_CHECK (probe != NULL);
    buffers[0] = probe->p[0];
    buffers[1] = probe->p[1];
    buffers[2] = probe->p[2];
    strides[0] = probe->stride[0];
    strides[1] = probe->stride[1];
    strides[2] = probe->stride[2];

    TASK_CHECK (mpeg2_set_buf_strides (session->dec, NULL, strides, probe)
		== -1);
    buffers[0] = NULL;
    TASK_CHECK (mpeg2_set_buf_strides (session->dec, buffers, strides, probe)
		== -1);
    buffers[0] = probe->p[0];
    strides[0] = session->width - 1;
    TASK_CHECK (mpeg2_set_buf_strides (session->dec, buffers, strides, probe)
		== -1);
    strides[0] = probe->stride[0];
    strides[1] = session->chroma_width - 1;
    TASK_CHECK (mpeg2_set_buf_strides (session->dec, buffers, strides, probe)
		== -1);
    strides[1] = probe->stride[1];
    TASK_CHECK (mpeg2_set_buf_strides (session->dec, buffers, NULL, probe)
		== -1);

    plane_free (probe);
    return 0;
}

static void capture_frame (Session * session)
{
    const mpeg2_info_t * info = session->info;
    const mpeg2_sequence_t * sequence = info->sequence;
    Frame * frame;
    Plane * plane = NULL;
    int strides[3];
    int widths[3], heights[3];
    int j;

    if (!info->display_fbuf || !info->display_picture || !sequence)
	return;
    if (session->frame_count >= MAX_FRAMES)
	return;

    if (session_custom (session))
	plane = (Plane *) info->display_fbuf->id;

    widths[0] = session->width;
    widths[1] = widths[2] = session->chroma_width;
    heights[0] = session->height;
    heights[1] = heights[2] = session->chroma_height;

    if (plane) {
	for (j = 0; j < 3; j++)
	    strides[j] = plane->stride[j];
    } else if (session->mode == MODE_STRIDE && session->set_stride > 0) {
	strides[0] = session->set_stride;
	strides[1] = strides[2] = session->set_stride >> 1;
    } else {
	strides[0] = session->width;
	strides[1] = strides[2] = session->chroma_width;
    }

    frame = &session->frames[session->frame_count++];
    memset (frame, 0, sizeof (*frame));
    frame->tag = plane ? plane->tag : 0;
    frame->temporal = info->display_picture->temporal_reference;
    frame->flags = info->display_picture->flags;
    frame->width = session->width;
    frame->height = session->height;
    frame->chroma_width = session->chroma_width;
    frame->chroma_height = session->chroma_height;
    for (j = 0; j < 3; j++) {
	int y;
	frame->stride[j] = widths[j];
	frame->data[j] = (uint8_t *) malloc ((size_t) widths[j] * heights[j]);
	if (!frame->data[j])
	    return;
	for (y = 0; y < heights[j]; y++)
	    memcpy (frame->data[j] + (size_t) y * widths[j],
		    info->display_fbuf->buf[j] +
		    (size_t) y * strides[j], widths[j]);
    }
}

static int run_stream (Session * session, const uint8_t * data, size_t size)
{
    size_t offset = 0;
    unsigned step;

    if (!session->dec)
	return -1;

    for (step = 0; step < 200000; step++) {
	mpeg2_state_t state = mpeg2_parse (session->dec);

	switch (state) {
	case STATE_BUFFER:
	    if (offset < size) {
		size_t amount = CHUNK;
		if (amount > size - offset)
		    amount = size - offset;
		mpeg2_buffer (session->dec, (uint8_t *) data + offset,
			      (uint8_t *) data + offset + amount);
		offset += amount;
	    } else
		return 0;
	    break;

	case STATE_SEQUENCE:
	    session->width = session->info->sequence->width;
	    session->height = session->info->sequence->height;
	    session->chroma_width =
		session->info->sequence->chroma_width;
	    session->chroma_height =
		session->info->sequence->chroma_height;
	    if (session->mode == MODE_STRIDE) {
		session->set_stride =
		    mpeg2_stride (session->dec, session->width + 32);
		if (session->set_stride < session->width)
		    return -2;
	    }
	    if (session_custom (session)) {
		int i;
		mpeg2_custom_fbuf (session->dec, 1);
		if (session->reject_probes)
		    TASK_CHECK (reject_probe (session) == 0);
		for (i = 0; i < 2; i++) {
		    if (i == 1 && session->reject_probes)
			TASK_CHECK (reject_probe (session) == 0);
		    if (register_plane (session))
			return -3;
		}
	    }
	    break;

	case STATE_PICTURE:
	    if (session_custom (session)) {
		Plane * good;
		if (session->reject_probes)
		    TASK_CHECK (reject_probe (session) == 0);
		if (register_plane (session))
		    return -4;
		good = session->planes[session->plane_count - 1];
		if (session->reject_probes) {
		    TASK_CHECK (reject_probe (session) == 0);
		    TASK_CHECK (session->info->current_fbuf != NULL &&
				session->info->current_fbuf->id == good);
		}
	    }
	    break;

	case STATE_PICTURE_2ND:
	    session->picture_second_count++;
	    break;

	case STATE_SLICE:
	case STATE_END:
	    capture_frame (session);
	    if (state == STATE_END)
		return 0;
	    break;

	case STATE_INVALID:
	case STATE_INVALID_END:
	    return -5;

	default:
	    break;
	}
    }
    return -6;
}

static int same_frame_metadata (const Session * a, const Session * b, int i)
{
    const Frame * fa = &a->frames[i];
    const Frame * fb = &b->frames[i];
    return fa->temporal == fb->temporal && fa->flags == fb->flags &&
	   fa->width == fb->width && fa->height == fb->height &&
	   fa->chroma_width == fb->chroma_width &&
	   fa->chroma_height == fb->chroma_height;
}

static int case_layout_basic (void)
{
    Session padded, tight;
    int i, j, y, x;

    mpeg2_accel (0);
    session_init (&padded, MODE_PADDED);
    session_init (&tight, MODE_TIGHT_NEW);
    TASK_CHECK (padded.dec != NULL && tight.dec != NULL);
    TASK_CHECK (run_stream (&tight, task_video, sizeof (task_video)) == 0);
    TASK_CHECK (run_stream (&padded, task_video, sizeof (task_video)) == 0);

    TASK_CHECK (tight.frame_count == padded.frame_count);
    TASK_CHECK (padded.frame_count == 9);
    for (i = 0; i < padded.frame_count; i++) {
	static const unsigned temporal_order[9] = {
	    0, 1, 2, 3, 4, 5, 6, 7, 8
	};
	TASK_CHECK (padded.frames[i].temporal == temporal_order[i]);
	TASK_CHECK (same_frame_metadata (&padded, &tight, i));
	TASK_CHECK (padded.frames[i].tag == tight.frames[i].tag);
	for (j = 0; j < 3; j++) {
	    int w = padded.frames[i].stride[j];
	    int h = j ? padded.chroma_height : padded.height;
	    for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
		    TASK_CHECK (padded.frames[i].data[j][(size_t) y*w+x] ==
				tight.frames[i].data[j][(size_t) y*w+x]);
	}
    }
    for (i = 0; i < padded.plane_count; i++)
	TASK_CHECK (plane_guards_ok (padded.planes[i]));
    for (i = 0; i < tight.plane_count; i++)
	TASK_CHECK (plane_guards_ok (tight.planes[i]));

    session_destroy (&padded);
    session_destroy (&tight);
    return 0;
}

static int case_prediction_layouts (void)
{
    Session padded, tight;
    int i, p_count = 0, b_count = 0;

    mpeg2_accel (0);
    session_init (&padded, MODE_PADDED);
    session_init (&tight, MODE_AUTO);
    TASK_CHECK (padded.dec != NULL && tight.dec != NULL);
    TASK_CHECK (run_stream (&tight, task_video, sizeof (task_video)) == 0);
    TASK_CHECK (run_stream (&padded, task_video, sizeof (task_video)) == 0);
    TASK_CHECK (padded.frame_count == tight.frame_count);

    for (i = 0; i < padded.frame_count; i++) {
	uint32_t type = padded.frames[i].flags & PIC_MASK_CODING_TYPE;
	if (type == PIC_FLAG_CODING_TYPE_P)
	    p_count++;
	if (type == PIC_FLAG_CODING_TYPE_B)
	    b_count++;
	TASK_CHECK (same_frame_metadata (&padded, &tight, i));
    }
    TASK_CHECK (p_count >= 1 && b_count >= 1);

    /* Full pixel comparison is done by the layout case; here explicitly
       assert that every P/B output survived differing reference layouts. */
    for (i = 0; i < padded.frame_count; i++) {
	int j, y, x;
	uint32_t type = padded.frames[i].flags & PIC_MASK_CODING_TYPE;
	if (type != PIC_FLAG_CODING_TYPE_P &&
	    type != PIC_FLAG_CODING_TYPE_B)
	    continue;
	for (j = 0; j < 3; j++) {
	    int w = padded.frames[i].stride[j];
	    int h = j ? padded.chroma_height : padded.height;
	    for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
		    TASK_CHECK (padded.frames[i].data[j][(size_t) y*w+x] ==
				tight.frames[i].data[j][(size_t) y*w+x]);
	}
    }
    for (i = 0; i < padded.plane_count; i++)
	TASK_CHECK (plane_guards_ok (padded.planes[i]));

    session_destroy (&padded);
    session_destroy (&tight);
    return 0;
}

static int case_compat_old_and_mixed (void)
{
    Session old_session, mixed_session, auto_session;
    int i;

    mpeg2_accel (0);
    session_init (&old_session, MODE_OLD);
    session_init (&mixed_session, MODE_MIXED);
    session_init (&auto_session, MODE_AUTO);
    TASK_CHECK (old_session.dec && mixed_session.dec && auto_session.dec);
    TASK_CHECK (run_stream (&auto_session, task_video,
			    sizeof (task_video)) == 0);
    TASK_CHECK (run_stream (&old_session, task_video,
			    sizeof (task_video)) == 0);
    TASK_CHECK (run_stream (&mixed_session, task_video,
			    sizeof (task_video)) == 0);
    TASK_CHECK (old_session.frame_count == 9);
    TASK_CHECK (mixed_session.frame_count == 9);

    for (i = 0; i < 9; i++) {
	int j, y, x;
	TASK_CHECK (same_frame_metadata (&old_session, &auto_session, i));
	TASK_CHECK (same_frame_metadata (&mixed_session, &auto_session, i));
	for (j = 0; j < 3; j++) {
	    int w = old_session.frames[i].stride[j];
	    int h = j ? old_session.chroma_height : old_session.height;
	    for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
		    size_t at = (size_t) y * w + x;
		    TASK_CHECK (old_session.frames[i].data[j][at] ==
				auto_session.frames[i].data[j][at]);
		    TASK_CHECK (mixed_session.frames[i].data[j][at] ==
				auto_session.frames[i].data[j][at]);
		}
	}
    }
    for (i = 0; i < old_session.plane_count; i++)
	TASK_CHECK (plane_guards_ok (old_session.planes[i]));
    for (i = 0; i < mixed_session.plane_count; i++)
	TASK_CHECK (plane_guards_ok (mixed_session.planes[i]));

    session_destroy (&old_session);
    session_destroy (&mixed_session);
    session_destroy (&auto_session);
    return 0;
}

static int case_compat_stride (void)
{
    Session stride_session, tight;
    int i;

    mpeg2_accel (0);
    session_init (&stride_session, MODE_STRIDE);
    session_init (&tight, MODE_TIGHT_NEW);
    TASK_CHECK (stride_session.dec && tight.dec);
    TASK_CHECK (run_stream (&stride_session, task_video,
			    sizeof (task_video)) == 0);
    TASK_CHECK (run_stream (&tight, task_video, sizeof (task_video)) == 0);
    TASK_CHECK (stride_session.frame_count == tight.frame_count);
    for (i = 0; i < stride_session.frame_count; i++)
	TASK_CHECK (same_frame_metadata (&stride_session, &tight, i));
    session_destroy (&stride_session);
    session_destroy (&tight);
    return 0;
}

static int case_reject_rules (void)
{
    mpeg2dec_t * dec;
    const mpeg2_info_t * info;
    Plane * plane;
    uint8_t * buffers[3];
    const int good_strides[3] = { 80, 44, 48 };
    mpeg2_state_t state;
    size_t offset = 0;
    int i;

    mpeg2_accel (0);
    dec = mpeg2_init ();
    TASK_CHECK (dec != NULL);

    /* Wrong state before a sequence header. */
    mpeg2_custom_fbuf (dec, 1);
    plane = plane_new (1, 1, 64, 48, 32, 24);
    TASK_CHECK (plane != NULL);
    buffers[0] = plane->p[0];
    buffers[1] = plane->p[1];
    buffers[2] = plane->p[2];
    TASK_CHECK (mpeg2_set_buf_strides (dec, buffers, good_strides, plane)
		== -1);
    plane_free (plane);

    /* Reach STATE_SEQUENCE. */
    info = mpeg2_info (dec);
    for (i = 0; i < 100000; i++) {
	state = mpeg2_parse (dec);
	if (state == STATE_BUFFER) {
	    size_t n = CHUNK;
	    if (n > sizeof (task_video) - offset)
		n = sizeof (task_video) - offset;
	    mpeg2_buffer (dec, (uint8_t *) task_video + offset,
			  (uint8_t *) task_video + offset + n);
	    offset += n;
	} else if (state == STATE_SEQUENCE)
	    break;
	else if (state == STATE_INVALID || state == STATE_INVALID_END) {
	    TASK_CHECK (0);
	    break;
	}
    }
    TASK_CHECK (state == STATE_SEQUENCE);

    /* Not in custom-buffer mode. */
    mpeg2_custom_fbuf (dec, 0);
    plane = plane_new (2, 1, info->sequence->width, info->sequence->height,
		       info->sequence->chroma_width,
		       info->sequence->chroma_height);
    TASK_CHECK (plane != NULL);
    buffers[0] = plane->p[0];
    buffers[1] = plane->p[1];
    buffers[2] = plane->p[2];
    TASK_CHECK (mpeg2_set_buf_strides (dec, buffers, plane->stride, plane)
		== -1);
    plane_free (plane);

    /* Colour conversion is also rejected. */
    TASK_CHECK (mpeg2_convert (dec, mpeg2convert_rgb32, NULL) == 0);
    mpeg2_custom_fbuf (dec, 1);
    plane = plane_new (3, 1, info->sequence->width, info->sequence->height,
		       info->sequence->chroma_width,
		       info->sequence->chroma_height);
    TASK_CHECK (plane != NULL);
    buffers[0] = plane->p[0];
    buffers[1] = plane->p[1];
    buffers[2] = plane->p[2];
    TASK_CHECK (mpeg2_set_buf_strides (dec, buffers, plane->stride, plane)
		== -1);
    plane_free (plane);
    mpeg2_close (dec);

    /* Null planes and short strides leave a usable custom decoder. */
    {
	Session session;
	int run_rc;
	mpeg2_accel (0);
	session_init (&session, MODE_PADDED);
	TASK_CHECK (session.dec != NULL);
	session.reject_probes = 1;
	run_rc = run_stream (&session, task_video, sizeof (task_video));
	TASK_CHECK (run_rc == 0);
	TASK_CHECK (session.frame_count == 9);
	session_destroy (&session);
    }

    return 0;
}

typedef struct {
    uint8_t * data;
    size_t size;
    size_t cap;
    unsigned bit;
} BitWriter;

static void bw_init (BitWriter * bw, uint8_t * data, size_t cap)
{
    bw->data = data;
    bw->size = 0;
    bw->cap = cap;
    bw->bit = 0;
    if (cap)
	data[0] = 0;
}

static void bw_bits (BitWriter * bw, uint32_t value, int n)
{
    int i;

    for (i = n - 1; i >= 0; i--) {
	if (bw->bit == 0) {
	    if (bw->size >= bw->cap)
		return;
	    bw->data[bw->size] = 0;
	}
	if ((value >> i) & 1u)
	    bw->data[bw->size] |= (uint8_t) (1u << (7 - bw->bit));
	bw->bit++;
	if (bw->bit == 8) {
	    bw->bit = 0;
	    bw->size++;
	}
    }
}

static void bw_align (BitWriter * bw)
{
    if (bw->bit) {
	bw->bit = 0;
	bw->size++;
    }
}

static void bw_bytes (BitWriter * bw, const uint8_t * bytes, size_t n)
{
    bw_align (bw);
    if (bw->size + n <= bw->cap) {
	memcpy (bw->data + bw->size, bytes, n);
	bw->size += n;
    }
}

static void field_picture_header (BitWriter * bw, int temporal, int p_type)
{
    static const uint8_t start[4] = { 0x00, 0x00, 0x01, 0x00 };
    uint8_t header[5] = { 0x00, 0xd7, 0xff, 0xfb, 0x80 };
    uint16_t packed;

    if (p_type == 1) {
	header[0] = 0x00;
	header[1] = 0x0f;
	header[2] = 0xff;
	header[3] = 0xf8;
	header[4] = 0x00;
    }

    packed = (uint16_t) (((temporal & 0x3ff) << 6) | ((p_type & 7) << 3));
    header[0] = (uint8_t) (packed >> 8);
    header[1] = (uint8_t) ((packed & 0xf8) | (header[1] & 0x07));
    bw_bytes (bw, start, sizeof (start));
    bw_bytes (bw, header, sizeof (header));
}

static void field_picture_extension (BitWriter * bw, int top_field)
{
    static const uint8_t start[4] = { 0x00, 0x00, 0x01, 0xb5 };

    bw_bytes (bw, start, sizeof (start));
    bw_bits (bw, 0x8, 4);		/* picture coding extension */
    bw_bits (bw, 1, 4);		/* four f_code nibbles: 1 */
    bw_bits (bw, 1, 4);
    bw_bits (bw, 1, 4);
    bw_bits (bw, 1, 4);
    bw_bits (bw, 0, 2);		/* intra DC precision */
    bw_bits (bw, top_field ? 1 : 2, 2);
    bw_bits (bw, 0, 1);		/* top field first */
    bw_bits (bw, 0, 1);		/* frame prediction / frame DCT only */
    bw_bits (bw, 0, 1);		/* concealment motion vectors */
    bw_bits (bw, 0, 1);		/* qscale type */
    bw_bits (bw, 0, 1);		/* intra VLC format */
    bw_bits (bw, 0, 1);		/* alternate scan */
    bw_bits (bw, 0, 1);		/* repeat first field */
    bw_bits (bw, 1, 1);		/* chroma 4:2:0 type */
    bw_bits (bw, 0, 1);		/* progressive frame */
    bw_bits (bw, 0, 1);		/* no composite display flag */
    bw_align (bw);
}

static void field_slice (BitWriter * bw, int intra)
{
    static const uint8_t start[4] = { 0x00, 0x00, 0x01, 0x01 };
    int block;

    bw_bytes (bw, start, sizeof (start));
    bw_bits (bw, 8, 5);		/* quantiser scale code */
    bw_bits (bw, 0, 1);		/* no extra slice information */
    bw_bits (bw, 1, 2);		/* first macroblock address increment */

    if (intra) {
	bw_bits (bw, 1, 1);	/* MB type: Intra, no quantiser change */
	for (block = 0; block < 4; block++) {
	    bw_bits (bw, 4, 3);	/* luma DC size 0: 100 */
	    bw_bits (bw, 2, 2);	/* end of block: 10 */
	}
	for (block = 0; block < 2; block++) {
	    bw_bits (bw, 0, 2);	/* chroma DC size 0: 00 */
	    bw_bits (bw, 2, 2);	/* end of block: 10 */
	}
    } else {
	bw_bits (bw, 1, 3);	/* P MB type: forward MC only ("001") */
	bw_bits (bw, 1, 2);	/* field motion_type = "01" */
	bw_bits (bw, 0, 1);	/* select reference field 0 */
	bw_bits (bw, 1, 1);	/* zero horizontal motion delta */
	bw_bits (bw, 1, 1);	/* zero vertical motion delta */
    }
    bw_align (bw);
}

/*
 * Construct a tiny 16x32, 4:2:0 MPEG-2 elementary stream consisting of
 * field-coded picture pairs.  The sequence extension clears
 * progressive_sequence; each picture coding extension sets
 * picture_structure to TOP_FIELD or BOTTOM_FIELD.  Slices contain one
 * 16x16 macroblock: I fields use address increment one, MB type Intra and
 * DC-only/end-of-block codes; later P fields use field motion type with
 * field select zero and both motion-delta codes set to zero.  Thus the
 * second field exercises the library's second-field reference setup while
 * comparisons remain exact.  This is deliberately generated here in C; no
 * host encoder or external media is needed at test time.
 */
static size_t build_field_stream (uint8_t * data, size_t cap)
{
    static const uint8_t sequence_header[12] = {
	0x00, 0x00, 0x01, 0xb3, 0x01, 0x00, 0x20, 0x23,
	0xff, 0xff, 0xe0, 0x18
    };
    static const uint8_t sequence_extension[10] = {
	0x00, 0x00, 0x01, 0xb5, 0x14, 0x82, 0x00, 0x01,
	0x00, 0x00
    };
    static const uint8_t sequence_end[4] = { 0x00, 0x00, 0x01, 0xb7 };
    BitWriter bw;
    int pair;

    bw_init (&bw, data, cap);
    bw_bytes (&bw, sequence_header, sizeof (sequence_header));
    bw_bytes (&bw, sequence_extension, sizeof (sequence_extension));

    for (pair = 0; pair < 3; pair++) {
	int picture_type = (pair == 0) ? 1 : 2;
	int field;
	for (field = 0; field < 2; field++) {
	    field_picture_header (&bw, pair, picture_type);
	    field_picture_extension (&bw, field == 0);
	    field_slice (&bw, picture_type == 1);
	}
    }

    bw_bytes (&bw, sequence_end, sizeof (sequence_end));
    return bw.size;
}

static int case_fields (void)
{
    uint8_t stream[4096];
    size_t stream_size = build_field_stream (stream, sizeof (stream));
    Session padded, tight;
    int i, j, y, x;

    TASK_CHECK (stream_size > 64 && stream_size < sizeof (stream));
    mpeg2_accel (0);
    session_init (&padded, MODE_PADDED);
    session_init (&tight, MODE_TIGHT_NEW);
    TASK_CHECK (padded.dec != NULL && tight.dec != NULL);
    TASK_CHECK (run_stream (&tight, stream, stream_size) == 0);
    TASK_CHECK (run_stream (&padded, stream, stream_size) == 0);

    TASK_CHECK (tight.width == 16 && tight.height == 32);
    TASK_CHECK (tight.chroma_width == 8 && tight.chroma_height == 16);
    TASK_CHECK (tight.picture_second_count >= 3);
    TASK_CHECK (padded.picture_second_count ==
		tight.picture_second_count);
    TASK_CHECK (padded.frame_count == tight.frame_count);
    TASK_CHECK (padded.frame_count >= 2);

    for (i = 0; i < padded.frame_count; i++) {
	TASK_CHECK (same_frame_metadata (&padded, &tight, i));
	for (j = 0; j < 3; j++) {
	    int w = padded.frames[i].stride[j];
	    int h = j ? padded.chroma_height : padded.height;
	    for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
		    TASK_CHECK (padded.frames[i].data[j][(size_t) y*w+x] ==
				tight.frames[i].data[j][(size_t) y*w+x]);
	}
    }
    for (i = 0; i < padded.plane_count; i++)
	TASK_CHECK (plane_guards_ok (padded.planes[i]));

    session_destroy (&padded);
    session_destroy (&tight);

    /* Fixed interlaced frame pictures exercise frame-based field MC. */
    session_init (&padded, MODE_PADDED);
    session_init (&tight, MODE_TIGHT_NEW);
    TASK_CHECK (padded.dec != NULL && tight.dec != NULL);
    TASK_CHECK (run_stream (&tight, interlaced_video,
			    interlaced_video_len) == 0);
    TASK_CHECK (run_stream (&padded, interlaced_video,
			    interlaced_video_len) == 0);
    TASK_CHECK (padded.frame_count == tight.frame_count);
    TASK_CHECK (padded.frame_count >= 2);
    for (i = 0; i < padded.frame_count; i++) {
	TASK_CHECK (same_frame_metadata (&padded, &tight, i));
	for (j = 0; j < 3; j++) {
	    int w = padded.frames[i].stride[j];
	    int h = j ? padded.chroma_height : padded.height;
	    for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
		    TASK_CHECK (padded.frames[i].data[j][(size_t) y*w+x] ==
				tight.frames[i].data[j][(size_t) y*w+x]);
	}
    }
    for (i = 0; i < padded.plane_count; i++)
	TASK_CHECK (plane_guards_ok (padded.planes[i]));
    session_destroy (&padded);
    session_destroy (&tight);
    return 0;
}

const struct task_case task_cases[] = {
    { "layout", "padded_vs_tight", case_layout_basic },
    { "layout", "reject_rules", case_reject_rules },
    { "prediction", "differing_reference_layouts", case_prediction_layouts },
    { "fields", "field_pairs", case_fields },
    { "compat", "old_and_mixed", case_compat_old_and_mixed },
    { "compat", "mpeg2_stride", case_compat_stride }
};

const size_t task_case_count =
    sizeof (task_cases) / sizeof (task_cases[0]);
