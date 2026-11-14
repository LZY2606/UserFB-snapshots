/* Target tests for mlt_producer_set_cut_window: window, graph, and frames groups. */

#include "task_support/check.h"

#include <framework/mlt.h>

#include <stddef.h>

/* Shared profile (borrowed from the baseline scaffolding). */

static mlt_profile test_profile(void)
{
    static struct mlt_profile_s profile = {.frame_rate_num = 25,
                                           .frame_rate_den = 1,
                                           .width = 32,
                                           .height = 32,
                                           .progressive = 1,
                                           .sample_aspect_num = 1,
                                           .sample_aspect_den = 1,
                                           .display_aspect_num = 1,
                                           .display_aspect_den = 1,
                                           .colorspace = 709,
                                           .is_explicit = 1};
    return &profile;
}

static void ensure_factory(void)
{
    static int initialized = 0;
    if (!initialized) {
        mlt_pool_init();
        mlt_factory_init(NULL);
        initialized = 1;
    }
}

/* A producer implemented with a C callback that stamps each frame with the
 * source id and the source frame number. */

static int source_get_frame(mlt_producer producer, mlt_frame *frame, int index)
{
    (void) index;
    *frame = mlt_frame_init(MLT_PRODUCER_SERVICE(producer));
    mlt_properties frame_properties = MLT_FRAME_PROPERTIES(*frame);
    mlt_properties_set_int(frame_properties,
                           "source_id",
                           mlt_properties_get_int(MLT_PRODUCER_PROPERTIES(producer), "source_id"));
    mlt_properties_set_int(frame_properties, "source_frame", mlt_producer_position(producer));
    mlt_producer_prepare_next(producer);
    return *frame ? 0 : 1;
}

static mlt_producer make_source(int id, int length)
{
    mlt_producer producer = mlt_producer_new(test_profile());
    producer->get_frame = source_get_frame;
    mlt_properties_set_int(MLT_PRODUCER_PROPERTIES(producer), "source_id", id);
    mlt_properties_set_int(MLT_PRODUCER_PROPERTIES(producer), "length", length);
    mlt_producer_set_in_and_out(producer, 0, length - 1);
    return producer;
}

static void set_source_length(mlt_producer producer, int length)
{
    mlt_properties_set_int(MLT_PRODUCER_PROPERTIES(producer), "length", length);
    mlt_producer_set_in_and_out(producer, 0, length - 1);
}

/* Read helpers returning frame stamps through output parameters. */

static int grab_frame(mlt_producer producer, int *id, int *frame_number, int *is_test)
{
    mlt_frame frame = NULL;
    if (mlt_service_get_frame(MLT_PRODUCER_SERVICE(producer), &frame, 0) || !frame)
        return 1;
    mlt_properties frame_properties = MLT_FRAME_PROPERTIES(frame);
    if (id)
        *id = mlt_properties_get_int(frame_properties, "source_id");
    if (frame_number)
        *frame_number = mlt_properties_get_int(frame_properties, "source_frame");
    if (is_test)
        *is_test = mlt_frame_is_test_card(frame) && mlt_frame_is_test_audio(frame);
    mlt_frame_close(frame);
    return 0;
}

static int read_at(mlt_producer producer, mlt_position position, int *id, int *frame_number)
{
    mlt_producer_seek(producer, position);
    return grab_frame(producer, id, frame_number, NULL);
}

static int window_active(mlt_producer cut)
{
    return mlt_properties_get_int(MLT_PRODUCER_PROPERTIES(cut), "_cut_window");
}

/* window group */

static int window_follow_shrink_grow(void)
{
    ensure_factory();
    mlt_producer source = make_source(1, 20);
    mlt_producer cut = mlt_producer_cut(source, 0, 19);
    mlt_producer static_cut = mlt_producer_cut(source, 5, 14);
    mlt_properties cut_properties = MLT_PRODUCER_PROPERTIES(cut);

    TASK_CHECK(mlt_producer_set_cut_window(cut, 5, 14) == 0);
    TASK_CHECK(mlt_producer_get_in(cut) == 5);
    TASK_CHECK(mlt_producer_get_out(cut) == 14);
    TASK_CHECK(mlt_producer_get_playtime(cut) == 10);
    TASK_CHECK(mlt_producer_get_length(cut) == 20);
    TASK_CHECK(mlt_properties_get_position(cut_properties, "_cut_window_in") == 5);
    TASK_CHECK(mlt_properties_get_position(cut_properties, "_cut_window_out") == 14);

    /* Shrink: the window is clipped, not rebased. */
    set_source_length(source, 8);
    TASK_CHECK(mlt_producer_get_in(cut) == 5);
    TASK_CHECK(mlt_producer_get_out(cut) == 7);
    TASK_CHECK(mlt_producer_get_playtime(cut) == 3);
    TASK_CHECK(mlt_producer_get_length(cut) == 8);

    /* Shrink below the window: empty but the request is retained. */
    set_source_length(source, 3);
    TASK_CHECK(mlt_producer_get_in(cut) == 0);
    TASK_CHECK(mlt_producer_get_out(cut) == -1);
    TASK_CHECK(mlt_producer_get_playtime(cut) == 0);
    TASK_CHECK(mlt_producer_get_length(cut) == 3);
    TASK_CHECK(mlt_properties_get_position(cut_properties, "_cut_window_in") == 5);
    TASK_CHECK(mlt_properties_get_position(cut_properties, "_cut_window_out") == 14);

    /* Recover: the original request applies again, not the last effective out. */
    set_source_length(source, 20);
    TASK_CHECK(mlt_producer_get_in(cut) == 5);
    TASK_CHECK(mlt_producer_get_out(cut) == 14);
    TASK_CHECK(mlt_producer_get_playtime(cut) == 10);
    TASK_CHECK(mlt_producer_get_length(cut) == 20);

    /* Open-ended window follows the end of the source. */
    TASK_CHECK(mlt_producer_set_cut_window(cut, 5, -1) == 0);
    TASK_CHECK(mlt_producer_get_out(cut) == 19);
    TASK_CHECK(mlt_producer_get_playtime(cut) == 15);
    set_source_length(source, 8);
    TASK_CHECK(mlt_producer_get_out(cut) == 7);
    set_source_length(source, 5);
    TASK_CHECK(mlt_producer_get_playtime(cut) == 0);
    TASK_CHECK(mlt_producer_get_out(cut) == -1);
    set_source_length(source, 20);
    TASK_CHECK(mlt_producer_get_in(cut) == 5);
    TASK_CHECK(mlt_producer_get_out(cut) == 19);

    /* A second call replaces the request. */
    TASK_CHECK(mlt_producer_set_cut_window(cut, 2, 4) == 0);
    TASK_CHECK(mlt_producer_get_in(cut) == 2);
    TASK_CHECK(mlt_producer_get_out(cut) == 4);
    TASK_CHECK(mlt_producer_get_playtime(cut) == 3);

    /* The static control cut never moved and the parent trim is untouched. */
    TASK_CHECK(mlt_producer_get_in(static_cut) == 5);
    TASK_CHECK(mlt_producer_get_out(static_cut) == 14);
    TASK_CHECK(mlt_producer_get_length(static_cut) == 20);
    TASK_CHECK(mlt_producer_get_in(source) == 0);
    TASK_CHECK(mlt_producer_get_out(source) == 19);

    mlt_producer_close(static_cut);
    mlt_producer_close(cut);
    mlt_producer_close(source);
    return 0;
}

static int window_validation(void)
{
    ensure_factory();
    mlt_producer source = make_source(2, 20);
    mlt_producer plain = mlt_producer_new(test_profile());

    /* Only existing, non-blank, non-mix cuts are supported. */
    TASK_CHECK(mlt_producer_set_cut_window(NULL, 0, 1) != 0);
    TASK_CHECK(mlt_producer_set_cut_window(plain, 0, 1) != 0);

    mlt_producer blank = mlt_producer_new(test_profile());
    mlt_properties_set(MLT_PRODUCER_PROPERTIES(blank), "resource", "blank");
    mlt_producer blank_cut = mlt_producer_cut(blank, 0, 5);
    TASK_CHECK(mlt_producer_is_blank(blank_cut));
    TASK_CHECK(mlt_producer_set_cut_window(blank_cut, 0, 1) != 0);

    /* Window bounds: in non-negative, out is -1 or >= in. */
    mlt_producer cut = mlt_producer_cut(source, 0, 19);
    TASK_CHECK(mlt_producer_set_cut_window(cut, -1, 5) != 0);
    TASK_CHECK(mlt_producer_set_cut_window(cut, 0, -2) != 0);
    TASK_CHECK(mlt_producer_set_cut_window(cut, 5, 3) != 0);
    TASK_CHECK(!window_active(cut));

    /* Valid enable, then invalid calls leave the request and state unchanged. */
    TASK_CHECK(mlt_producer_set_cut_window(cut, 5, 14) == 0);
    TASK_CHECK(mlt_producer_set_cut_window(cut, 9, 4) != 0);
    TASK_CHECK(mlt_producer_set_cut_window(cut, -3, -1) != 0);
    TASK_CHECK(mlt_producer_get_in(cut) == 5);
    TASK_CHECK(mlt_producer_get_out(cut) == 14);
    TASK_CHECK(mlt_properties_get_position(MLT_PRODUCER_PROPERTIES(cut), "_cut_window_in") == 5);
    TASK_CHECK(mlt_properties_get_position(MLT_PRODUCER_PROPERTIES(cut), "_cut_window_out") == 14);

    /* The enabled cut still follows after the rejected calls. */
    set_source_length(source, 10);
    TASK_CHECK(mlt_producer_get_out(cut) == 9);
    set_source_length(source, 20);
    TASK_CHECK(mlt_producer_get_out(cut) == 14);

    /* Single-frame and full-range windows are valid. */
    TASK_CHECK(mlt_producer_set_cut_window(cut, 7, 7) == 0);
    TASK_CHECK(mlt_producer_get_playtime(cut) == 1);
    TASK_CHECK(mlt_producer_set_cut_window(cut, 0, -1) == 0);
    TASK_CHECK(mlt_producer_get_out(cut) == 19);
    TASK_CHECK(mlt_producer_get_playtime(cut) == 20);

    /* A mix clip cannot follow a window. */
    mlt_playlist playlist = mlt_playlist_new(test_profile());
    mlt_producer clip_a = mlt_producer_cut(source, 0, 4);
    mlt_producer clip_b = mlt_producer_cut(source, 5, 9);
    TASK_CHECK(mlt_playlist_append(playlist, clip_a) == 0);
    TASK_CHECK(mlt_playlist_append(playlist, clip_b) == 0);
    TASK_CHECK(mlt_playlist_mix(playlist, 0, 2, NULL) == 0);
    mlt_producer mix_cut = mlt_playlist_get_clip(playlist, 1);
    TASK_CHECK(mix_cut != NULL);
    TASK_CHECK(mlt_producer_is_mix(mlt_producer_cut_parent(mix_cut)));
    TASK_CHECK(mlt_producer_set_cut_window(mix_cut, 0, 1) != 0);
    TASK_CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(playlist)) == 8);

    mlt_playlist_close(playlist);
    mlt_producer_close(clip_a);
    mlt_producer_close(clip_b);
    mlt_producer_close(cut);
    mlt_producer_close(blank_cut);
    mlt_producer_close(blank);
    mlt_producer_close(plain);
    mlt_producer_close(source);
    return 0;
}

static int window_exit_follow(void)
{
    ensure_factory();
    mlt_producer source = make_source(3, 20);
    mlt_producer cut = mlt_producer_cut(source, 0, 19);

    TASK_CHECK(mlt_producer_set_cut_window(cut, 2, 8) == 0);
    TASK_CHECK(window_active(cut));

    /* The legacy call exits the following and sets fixed points. */
    TASK_CHECK(mlt_producer_set_in_and_out(cut, 3, 9) == 0);
    TASK_CHECK(!window_active(cut));
    TASK_CHECK(mlt_producer_get_in(cut) == 3);
    TASK_CHECK(mlt_producer_get_out(cut) == 9);
    TASK_CHECK(mlt_producer_get_playtime(cut) == 7);

    /* The cut is static again: parent length changes no longer apply. */
    set_source_length(source, 10);
    TASK_CHECK(mlt_producer_get_out(cut) == 9);
    TASK_CHECK(mlt_producer_get_length(cut) == 20);
    set_source_length(source, 30);
    TASK_CHECK(mlt_producer_get_in(cut) == 3);
    TASK_CHECK(mlt_producer_get_out(cut) == 9);

    /* Following can be enabled again and tracks the current parent length. */
    TASK_CHECK(mlt_producer_set_cut_window(cut, 1, 2) == 0);
    TASK_CHECK(window_active(cut));
    TASK_CHECK(mlt_producer_get_length(cut) == 30);
    set_source_length(source, 2);
    TASK_CHECK(mlt_producer_get_in(cut) == 1);
    TASK_CHECK(mlt_producer_get_out(cut) == 1);
    TASK_CHECK(mlt_producer_get_playtime(cut) == 1);

    /* Replacing the request while enabled works. */
    TASK_CHECK(mlt_producer_set_cut_window(cut, 0, 0) == 0);
    TASK_CHECK(mlt_producer_get_in(cut) == 0);
    TASK_CHECK(mlt_producer_get_out(cut) == 0);

    mlt_producer_close(cut);
    mlt_producer_close(source);
    return 0;
}

/* graph group */

static int graph_playlist_repeat(void)
{
    ensure_factory();
    mlt_producer source = make_source(4, 20);
    mlt_playlist playlist = mlt_playlist_new(test_profile());
    mlt_producer playlist_producer = MLT_PLAYLIST_PRODUCER(playlist);
    mlt_playlist_clip_info info;

    /* Window enabled before the cut enters the playlist. */
    mlt_producer cut_a = mlt_producer_cut(source, 0, 19);
    TASK_CHECK(mlt_producer_set_cut_window(cut_a, 4, 9) == 0);
    TASK_CHECK(mlt_playlist_append(playlist, cut_a) == 0);
    TASK_CHECK(mlt_playlist_repeat_clip(playlist, 0, 3) == 0);
    TASK_CHECK(mlt_playlist_clip_length(playlist, 0) == 18);
    TASK_CHECK(mlt_producer_get_playtime(playlist_producer) == 18);

    /* Window enabled after the cut entered the playlist. */
    mlt_producer cut_b = mlt_producer_cut(source, 0, 4);
    TASK_CHECK(mlt_playlist_append(playlist, cut_b) == 0);
    TASK_CHECK(mlt_producer_get_playtime(playlist_producer) == 23);
    TASK_CHECK(mlt_producer_set_cut_window(cut_b, 1, 3) == 0);
    TASK_CHECK(mlt_playlist_clip_length(playlist, 1) == 3);
    TASK_CHECK(mlt_producer_get_playtime(playlist_producer) == 21);
    TASK_CHECK(mlt_playlist_clip_start(playlist, 1) == 18);

    TASK_CHECK(mlt_playlist_get_clip_info(playlist, &info, 0) == 0);
    TASK_CHECK(info.cut == cut_a);
    TASK_CHECK(info.producer == source);
    TASK_CHECK(info.frame_in == 4);
    TASK_CHECK(info.frame_out == 9);
    TASK_CHECK(info.frame_count == 18);
    TASK_CHECK(info.repeat == 3);
    TASK_CHECK(info.start == 0);

    /* Shrink the first window to empty: the entry keeps its slot and object. */
    set_source_length(source, 3);
    TASK_CHECK(mlt_playlist_clip_length(playlist, 0) == 0);
    TASK_CHECK(mlt_playlist_clip_length(playlist, 1) == 2);
    TASK_CHECK(mlt_producer_get_playtime(playlist_producer) == 2);
    TASK_CHECK(mlt_playlist_count(playlist) == 2);
    TASK_CHECK(mlt_playlist_get_clip(playlist, 0) == cut_a);
    TASK_CHECK(mlt_playlist_get_clip_info(playlist, &info, 0) == 0);
    TASK_CHECK(info.frame_count == 0);
    TASK_CHECK(info.start == 0);
    TASK_CHECK(mlt_playlist_clip_start(playlist, 1) == 0);

    /* Everything empty: entries remain and the total is zero. */
    set_source_length(source, 1);
    TASK_CHECK(mlt_producer_get_playtime(playlist_producer) == 0);
    TASK_CHECK(mlt_playlist_count(playlist) == 2);
    TASK_CHECK(mlt_playlist_get_clip(playlist, 1) == cut_b);

    /* Recovery restores the repeated lengths. */
    set_source_length(source, 20);
    TASK_CHECK(mlt_playlist_clip_length(playlist, 0) == 18);
    TASK_CHECK(mlt_playlist_clip_length(playlist, 1) == 3);
    TASK_CHECK(mlt_producer_get_playtime(playlist_producer) == 21);
    TASK_CHECK(mlt_playlist_clip_start(playlist, 1) == 18);

    mlt_playlist_close(playlist);
    mlt_producer_close(cut_a);
    mlt_producer_close(cut_b);
    mlt_producer_close(source);
    return 0;
}

/* Consistency observer for the nested graph: every producer-changed callback
 * on the outer playlist must see the cut window and the entry cache agree. */

static mlt_playlist observed_playlist;
static mlt_playlist observed_inner;
static mlt_producer observed_cut;
static int consistency_calls;
static int consistency_violations;

static void consistency_listener(mlt_properties owner, void *listener_data, mlt_event_data event_data)
{
    (void) owner;
    (void) listener_data;
    (void) event_data;
    mlt_position inner_length = mlt_producer_get_length(MLT_PLAYLIST_PRODUCER(observed_inner));
    mlt_position expected_out = inner_length - 1;
    if (expected_out > 4)
        expected_out = 4;
    if (inner_length < 2 || expected_out < 1)
        expected_out = -1;
    mlt_position expected_playtime = expected_out < 0 ? 0 : expected_out;
    ++consistency_calls;
    if (mlt_producer_get_out(observed_cut) != expected_out)
        ++consistency_violations;
    if (mlt_producer_get_playtime(observed_cut) != expected_playtime)
        ++consistency_violations;
    if (mlt_playlist_clip_length(observed_playlist, 0) != expected_playtime * 2)
        ++consistency_violations;
}

static int graph_nested_tractor(void)
{
    ensure_factory();
    mlt_producer source = make_source(5, 20);

    /* Inner playlist holds a window cut of the source. */
    mlt_producer cut_a = mlt_producer_cut(source, 0, 19);
    TASK_CHECK(mlt_producer_set_cut_window(cut_a, 2, 7) == 0);
    mlt_playlist inner = mlt_playlist_new(test_profile());
    TASK_CHECK(mlt_playlist_append(inner, cut_a) == 0);
    TASK_CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(inner)) == 6);

    /* A window cut references the inner playlist. */
    mlt_producer cut_b = mlt_producer_cut(MLT_PLAYLIST_PRODUCER(inner), 0, 5);
    TASK_CHECK(mlt_producer_set_cut_window(cut_b, 1, 4) == 0);
    TASK_CHECK(mlt_producer_get_playtime(cut_b) == 4);

    /* Outer playlist with repeat sits on a multitrack track in a tractor. */
    mlt_playlist outer = mlt_playlist_new(test_profile());
    TASK_CHECK(mlt_playlist_append(outer, cut_b) == 0);
    TASK_CHECK(mlt_playlist_repeat_clip(outer, 0, 2) == 0);
    TASK_CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(outer)) == 8);

    mlt_tractor tractor = mlt_tractor_new();
    TASK_CHECK(mlt_tractor_set_track(tractor, MLT_PLAYLIST_PRODUCER(outer), 0) == 0);
    mlt_multitrack multitrack = mlt_tractor_multitrack(tractor);
    TASK_CHECK(mlt_producer_get_length(MLT_MULTITRACK_PRODUCER(multitrack)) == 8);
    TASK_CHECK(mlt_producer_get_length(MLT_TRACTOR_PRODUCER(tractor)) == 8);

    /* A window cut may also reference a plain tractor directly. */
    mlt_producer cut_t = mlt_producer_cut(MLT_TRACTOR_PRODUCER(tractor), 0, 3);
    TASK_CHECK(mlt_producer_set_cut_window(cut_t, 1, 2) == 0);
    TASK_CHECK(mlt_producer_get_playtime(cut_t) == 2);
    TASK_CHECK(mlt_producer_get_length(cut_t) == 8);

    /* Observe consistency of the outer playlist during the cascades. */
    observed_playlist = outer;
    observed_inner = inner;
    observed_cut = cut_b;
    consistency_calls = 0;
    consistency_violations = 0;
    mlt_events_listen(MLT_PLAYLIST_PROPERTIES(outer),
                      NULL,
                      "producer-changed",
                      (mlt_listener) consistency_listener);

    /* Shrink the deepest source: changes propagate through both playlists. */
    set_source_length(source, 5);
    TASK_CHECK(mlt_producer_get_playtime(cut_a) == 3);
    TASK_CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(inner)) == 3);
    TASK_CHECK(mlt_producer_get_out(cut_b) == 2);
    TASK_CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(outer)) == 4);
    TASK_CHECK(mlt_producer_get_length(MLT_MULTITRACK_PRODUCER(multitrack)) == 4);
    TASK_CHECK(mlt_producer_get_length(MLT_TRACTOR_PRODUCER(tractor)) == 4);
    TASK_CHECK(mlt_producer_get_playtime(cut_t) == 2);

    set_source_length(source, 3);
    TASK_CHECK(mlt_producer_get_playtime(cut_a) == 1);
    TASK_CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(inner)) == 1);
    TASK_CHECK(mlt_producer_get_playtime(cut_b) == 0);
    TASK_CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(outer)) == 0);
    TASK_CHECK(mlt_producer_get_length(MLT_MULTITRACK_PRODUCER(multitrack)) == 0);
    TASK_CHECK(mlt_producer_get_length(MLT_TRACTOR_PRODUCER(tractor)) == 0);
    TASK_CHECK(mlt_producer_get_playtime(cut_t) == 0);

    set_source_length(source, 1);
    TASK_CHECK(mlt_producer_get_playtime(cut_a) == 0);
    TASK_CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(inner)) == 0);
    TASK_CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(outer)) == 0);
    TASK_CHECK(mlt_producer_get_length(MLT_TRACTOR_PRODUCER(tractor)) == 0);

    /* Full recovery along the whole chain. */
    set_source_length(source, 20);
    TASK_CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(inner)) == 6);
    TASK_CHECK(mlt_producer_get_out(cut_b) == 4);
    TASK_CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(outer)) == 8);
    TASK_CHECK(mlt_producer_get_length(MLT_MULTITRACK_PRODUCER(multitrack)) == 8);
    TASK_CHECK(mlt_producer_get_length(MLT_TRACTOR_PRODUCER(tractor)) == 8);
    TASK_CHECK(mlt_producer_get_playtime(cut_t) == 2);
    TASK_CHECK(mlt_producer_get_length(cut_t) == 8);

    TASK_CHECK(consistency_calls > 0);
    TASK_CHECK(consistency_violations == 0);

    mlt_producer_close(cut_t);
    mlt_tractor_close(tractor);
    mlt_playlist_close(outer);
    mlt_playlist_close(inner);
    mlt_producer_close(cut_b);
    mlt_producer_close(cut_a);
    mlt_producer_close(source);
    return 0;
}

static int graph_shared_parent(void)
{
    ensure_factory();
    mlt_producer source = make_source(6, 20);
    mlt_producer cut_a = mlt_producer_cut(source, 0, 19);
    mlt_producer cut_b = mlt_producer_cut(source, 0, 19);
    mlt_producer cut_static = mlt_producer_cut(source, 0, 4);
    TASK_CHECK(mlt_producer_set_cut_window(cut_a, 0, 9) == 0);
    TASK_CHECK(mlt_producer_set_cut_window(cut_b, 10, -1) == 0);

    mlt_playlist playlist = mlt_playlist_new(test_profile());
    TASK_CHECK(mlt_playlist_append(playlist, cut_a) == 0);
    TASK_CHECK(mlt_playlist_append(playlist, cut_b) == 0);
    TASK_CHECK(mlt_playlist_append(playlist, cut_static) == 0);
    TASK_CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(playlist)) == 25);

    /* Each cut follows its own window. */
    set_source_length(source, 15);
    TASK_CHECK(mlt_producer_get_playtime(cut_a) == 10);
    TASK_CHECK(mlt_producer_get_playtime(cut_b) == 5);
    TASK_CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(playlist)) == 20);

    set_source_length(source, 5);
    TASK_CHECK(mlt_producer_get_playtime(cut_a) == 5);
    TASK_CHECK(mlt_producer_get_playtime(cut_b) == 0);
    TASK_CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(playlist)) == 10);
    TASK_CHECK(mlt_playlist_count(playlist) == 3);
    TASK_CHECK(mlt_playlist_get_clip(playlist, 1) == cut_b);

    set_source_length(source, 20);
    TASK_CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(playlist)) == 25);

    /* The static cut and the parent trim are not modified by the windows. */
    TASK_CHECK(mlt_producer_get_in(cut_static) == 0);
    TASK_CHECK(mlt_producer_get_out(cut_static) == 4);
    TASK_CHECK(mlt_producer_get_in(source) == 0);
    TASK_CHECK(mlt_producer_get_out(source) == 19);

    /* Changing only the parent's in/out does not rebase the requests. */
    TASK_CHECK(mlt_producer_set_in_and_out(source, 2, 17) == 0);
    TASK_CHECK(mlt_producer_get_length(source) == 20);
    TASK_CHECK(mlt_producer_get_in(cut_a) == 0);
    TASK_CHECK(mlt_producer_get_out(cut_a) == 9);
    TASK_CHECK(mlt_producer_get_in(cut_b) == 10);
    TASK_CHECK(mlt_producer_get_out(cut_b) == 19);
    TASK_CHECK(mlt_producer_set_in_and_out(source, 0, 19) == 0);
    TASK_CHECK(mlt_producer_get_out(cut_b) == 19);

    mlt_playlist_close(playlist);
    mlt_producer_close(cut_a);
    mlt_producer_close(cut_b);
    mlt_producer_close(cut_static);
    mlt_producer_close(source);
    return 0;
}

static int graph_close_release(void)
{
    ensure_factory();
    mlt_producer source = make_source(7, 20);

    /* The playlist owns the only reference after we drop ours. */
    mlt_producer cut = mlt_producer_cut(source, 0, 19);
    TASK_CHECK(mlt_producer_set_cut_window(cut, 2, 8) == 0);
    mlt_playlist playlist = mlt_playlist_new(test_profile());
    TASK_CHECK(mlt_playlist_append(playlist, cut) == 0);
    mlt_producer_close(cut);
    set_source_length(source, 5);
    TASK_CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(playlist)) == 3);

    /* Closing the container closes the cut and releases its listener. */
    mlt_playlist_close(playlist);
    set_source_length(source, 20);
    set_source_length(source, 4);

    /* Closing a standalone cut releases its listener before source changes. */
    mlt_producer cut2 = mlt_producer_cut(source, 0, 3);
    TASK_CHECK(mlt_producer_set_cut_window(cut2, 1, 2) == 0);
    mlt_producer_close(cut2);
    set_source_length(source, 20);

    /* Removing an entry keeps our reference; closing it releases the listener. */
    mlt_producer cut3 = mlt_producer_cut(source, 0, 9);
    TASK_CHECK(mlt_producer_set_cut_window(cut3, 3, 6) == 0);
    mlt_playlist playlist2 = mlt_playlist_new(test_profile());
    TASK_CHECK(mlt_playlist_append(playlist2, cut3) == 0);
    TASK_CHECK(mlt_playlist_remove(playlist2, 0) == 0);
    set_source_length(source, 6);
    TASK_CHECK(mlt_producer_get_out(cut3) == 5);
    mlt_producer_close(cut3);
    set_source_length(source, 20);
    TASK_CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(playlist2)) == 0);

    mlt_playlist_close(playlist2);
    mlt_producer_close(source);
    return 0;
}

/* frames group */

static int frames_cut_window(void)
{
    ensure_factory();
    mlt_producer source = make_source(8, 20);
    mlt_producer cut = mlt_producer_cut(source, 0, 19);
    mlt_producer static_cut = mlt_producer_cut(source, 5, 14);
    int id = 0, number = -1;

    TASK_CHECK(mlt_producer_set_cut_window(cut, 5, 14) == 0);
    TASK_CHECK(read_at(cut, 0, &id, &number) == 0);
    TASK_CHECK(id == 8 && number == 5);
    TASK_CHECK(read_at(cut, 9, &id, &number) == 0);
    TASK_CHECK(id == 8 && number == 14);

    /* The static control cut reads the same source frames. */
    TASK_CHECK(read_at(static_cut, 0, &id, &number) == 0);
    TASK_CHECK(id == 8 && number == 5);

    /* Shrink: frames beyond the new end are gone. */
    set_source_length(source, 8);
    TASK_CHECK(read_at(cut, 0, &id, &number) == 0);
    TASK_CHECK(number == 5);
    TASK_CHECK(read_at(cut, 2, &id, &number) == 0);
    TASK_CHECK(number == 7);

    /* Empty: a blank test frame is produced and the parent is not consumed. */
    set_source_length(source, 3);
    TASK_CHECK(mlt_producer_seek(source, 2) == 0);
    mlt_position parent_position = mlt_producer_position(source);
    mlt_frame frame = NULL;
    TASK_CHECK(mlt_service_get_frame(MLT_PRODUCER_SERVICE(cut), &frame, 0) == 0);
    TASK_CHECK(frame != NULL);
    TASK_CHECK(mlt_frame_is_test_card(frame));
    TASK_CHECK(mlt_frame_is_test_audio(frame));
    TASK_CHECK(mlt_properties_get(MLT_FRAME_PROPERTIES(frame), "source_id") == NULL);
    TASK_CHECK(mlt_properties_get(MLT_FRAME_PROPERTIES(frame), "source_frame") == NULL);
    mlt_frame_close(frame);
    TASK_CHECK(mlt_producer_position(source) == parent_position);

    /* Recovery: the requested window produces its frames again. */
    set_source_length(source, 20);
    TASK_CHECK(read_at(cut, 0, &id, &number) == 0);
    TASK_CHECK(number == 5);
    TASK_CHECK(read_at(cut, 9, &id, &number) == 0);
    TASK_CHECK(id == 8 && number == 14);

    mlt_producer_close(static_cut);
    mlt_producer_close(cut);
    mlt_producer_close(source);
    return 0;
}

static int frames_playlist_sequence(void)
{
    ensure_factory();
    mlt_producer source = make_source(9, 20);
    mlt_producer cut_a = mlt_producer_cut(source, 0, 19);
    mlt_producer cut_b = mlt_producer_cut(source, 0, 19);
    TASK_CHECK(mlt_producer_set_cut_window(cut_a, 2, 3) == 0);
    TASK_CHECK(mlt_producer_set_cut_window(cut_b, 8, 9) == 0);

    mlt_playlist playlist = mlt_playlist_new(test_profile());
    mlt_producer playlist_producer = MLT_PLAYLIST_PRODUCER(playlist);
    TASK_CHECK(mlt_playlist_append(playlist, cut_a) == 0);
    TASK_CHECK(mlt_playlist_append(playlist, cut_b) == 0);
    TASK_CHECK(mlt_playlist_repeat_clip(playlist, 1, 2) == 0);
    TASK_CHECK(mlt_producer_get_playtime(playlist_producer) == 6);
    int id = 0, number = -1;

    /* Sequential play: 2,3 then 8,9 repeated. */
    TASK_CHECK(mlt_producer_seek(playlist_producer, 0) == 0);
    int expected[] = {2, 3, 8, 9, 8, 9};
    for (int i = 0; i < 6; i++) {
        TASK_CHECK(grab_frame(playlist_producer, &id, &number, NULL) == 0);
        TASK_CHECK(id == 9 && number == expected[i]);
    }

    /* Random access seeks land on the same frames. */
    TASK_CHECK(read_at(playlist_producer, 5, &id, &number) == 0 && number == 9);
    TASK_CHECK(read_at(playlist_producer, 2, &id, &number) == 0 && number == 8);
    TASK_CHECK(read_at(playlist_producer, 0, &id, &number) == 0 && number == 2);
    TASK_CHECK(read_at(playlist_producer, 3, &id, &number) == 0 && number == 9);

    /* Position the play head, then empty the second window underneath it. */
    TASK_CHECK(mlt_producer_seek(playlist_producer, 5) == 0);
    set_source_length(source, 4);
    TASK_CHECK(mlt_producer_get_playtime(playlist_producer) == 2);

    /* Pause at the end selects the last clip that still has frames. */
    TASK_CHECK(grab_frame(playlist_producer, &id, &number, NULL) == 0);
    TASK_CHECK(id == 9 && number == 3);

    /* Zero length entries are skipped during ordered and seeking playback. */
    TASK_CHECK(read_at(playlist_producer, 0, &id, &number) == 0 && number == 2);
    TASK_CHECK(read_at(playlist_producer, 1, &id, &number) == 0 && number == 3);

    /* All entries empty: a blank frame is produced, no division by zero. */
    set_source_length(source, 2);
    TASK_CHECK(mlt_producer_get_playtime(playlist_producer) == 0);
    int is_test = 0;
    TASK_CHECK(grab_frame(playlist_producer, &id, &number, &is_test) == 0);
    TASK_CHECK(is_test);

    /* Recovery yields fresh frames, not a replay of stale ones. */
    set_source_length(source, 20);
    TASK_CHECK(mlt_producer_get_playtime(playlist_producer) == 6);
    TASK_CHECK(read_at(playlist_producer, 0, &id, &number) == 0 && number == 2);
    TASK_CHECK(read_at(playlist_producer, 5, &id, &number) == 0 && number == 9);

    mlt_playlist_close(playlist);
    mlt_producer_close(cut_a);
    mlt_producer_close(cut_b);
    mlt_producer_close(source);
    return 0;
}

static int frames_playlist_loop(void)
{
    ensure_factory();
    mlt_producer source = make_source(10, 20);
    mlt_producer cut_c = mlt_producer_cut(source, 0, 19);
    mlt_producer cut_d = mlt_producer_cut(source, 0, 19);
    TASK_CHECK(mlt_producer_set_cut_window(cut_c, 8, 9) == 0);
    TASK_CHECK(mlt_producer_set_cut_window(cut_d, 2, 3) == 0);

    mlt_playlist playlist = mlt_playlist_new(test_profile());
    mlt_producer playlist_producer = MLT_PLAYLIST_PRODUCER(playlist);
    mlt_properties_set(MLT_PLAYLIST_PROPERTIES(playlist), "eof", "loop");
    TASK_CHECK(mlt_playlist_append(playlist, cut_c) == 0);
    TASK_CHECK(mlt_playlist_append(playlist, cut_d) == 0);
    TASK_CHECK(mlt_producer_get_playtime(playlist_producer) == 4);
    int id = 0, number = -1, is_test = 0;

    TASK_CHECK(read_at(playlist_producer, 0, &id, &number) == 0 && number == 8);
    TASK_CHECK(read_at(playlist_producer, 3, &id, &number) == 0 && number == 3);
    /* Loop wraps the play head back into the first clip. */
    TASK_CHECK(read_at(playlist_producer, 4, &id, &number) == 0 && number == 8);

    /* Park the play head, then empty the first entry underneath it. */
    TASK_CHECK(mlt_producer_seek(playlist_producer, 3) == 0);
    set_source_length(source, 4);
    TASK_CHECK(mlt_producer_get_playtime(playlist_producer) == 2);

    /* Loop selects the first clip that still has frames. */
    TASK_CHECK(grab_frame(playlist_producer, &id, &number, NULL) == 0);
    TASK_CHECK(id == 10 && number == 2);

    /* All entries empty: a blank frame is produced, no division by zero. */
    set_source_length(source, 2);
    TASK_CHECK(mlt_producer_get_playtime(playlist_producer) == 0);
    TASK_CHECK(grab_frame(playlist_producer, &id, &number, &is_test) == 0);
    TASK_CHECK(is_test);

    /* Recovery yields fresh frames again. */
    set_source_length(source, 20);
    TASK_CHECK(mlt_producer_get_playtime(playlist_producer) == 4);
    TASK_CHECK(read_at(playlist_producer, 0, &id, &number) == 0 && number == 8);
    TASK_CHECK(read_at(playlist_producer, 2, &id, &number) == 0 && number == 2);

    mlt_playlist_close(playlist);
    mlt_producer_close(cut_c);
    mlt_producer_close(cut_d);
    mlt_producer_close(source);
    return 0;
}

const struct task_case task_cases[] = {{"window", "follow_shrink_grow", window_follow_shrink_grow},
                                       {"window", "validation", window_validation},
                                       {"window", "exit_follow", window_exit_follow},
                                       {"graph", "playlist_repeat", graph_playlist_repeat},
                                       {"graph", "nested_tractor", graph_nested_tractor},
                                       {"graph", "shared_parent", graph_shared_parent},
                                       {"graph", "close_release", graph_close_release},
                                       {"frames", "cut_window", frames_cut_window},
                                       {"frames", "playlist_sequence", frames_playlist_sequence},
                                       {"frames", "playlist_loop", frames_playlist_loop}};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
