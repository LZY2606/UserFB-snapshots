#include <plutovg.h>
#include <stdio.h>
#include <stdint.h>

static int g_checks = 0;
static int g_failures = 0;

static uint32_t pixel(plutovg_surface_t* surface, int x, int y)
{
    return ((uint32_t*)(plutovg_surface_get_data(surface) + y * plutovg_surface_get_stride(surface)))[x];
}

static void note_failure(const char* scene, const char* what, const char* detail)
{
    if(g_failures == 0)
        printf("FIRST DIFFERENCE\n");
    printf("  FAIL [scene: %s] %s: %s\n", scene, what, detail);
    g_failures += 1;
}

static void check_bool(const char* scene, const char* what, int got, int expected)
{
    g_checks += 1;
    if(got != expected) {
        char detail[128];
        snprintf(detail, sizeof(detail), "got %d, expected %d", got, expected);
        note_failure(scene, what, detail);
    }
}

static void check_int(const char* scene, const char* what, int got, int expected)
{
    check_bool(scene, what, got, expected);
}

static void check_rect(const char* scene, const char* what, plutovg_rect_t got, float x, float y, float w, float h)
{
    g_checks += 1;
    if(got.x != x || got.y != y || got.w != w || got.h != h) {
        char detail[160];
        snprintf(detail, sizeof(detail), "got (%.2f, %.2f, %.2f, %.2f), expected (%.2f, %.2f, %.2f, %.2f)",
            got.x, got.y, got.w, got.h, x, y, w, h);
        note_failure(scene, what, detail);
    }
}

static void map_check_contains(const char* scene, plutovg_canvas_t* canvas, const plutovg_matrix_t* matrix,
    float ux, float uy, int expected)
{
    float dx, dy;
    plutovg_matrix_map(matrix, ux, uy, &dx, &dy);
    char what[96];
    snprintf(what, sizeof(what), "stroke_contains device(%.2f, %.2f) <- user(%.2f, %.2f)", dx, dy, ux, uy);
    check_bool(scene, what, plutovg_canvas_stroke_contains(canvas, dx, dy), expected);
}

/* S1: the canonical horizontal line from regress/baseline.c */
static void scene_horizontal_line(plutovg_surface_t* surface)
{
    const char* scene = "horizontal-line scale(3,1) width 4";
    plutovg_canvas_t* canvas = plutovg_canvas_create(surface);

    check_int(scene, "default stroke transform is LEGACY",
        plutovg_canvas_get_stroke_transform(canvas), PLUTOVG_STROKE_TRANSFORM_LEGACY);

    plutovg_canvas_set_rgb(canvas, 1, 0, 0);
    plutovg_canvas_set_line_width(canvas, 4);
    plutovg_canvas_scale(canvas, 3, 1);
    plutovg_canvas_move_to(canvas, 4, 20);
    plutovg_canvas_line_to(canvas, 16, 20);

    /* legacy reference rendering must stay (12, 15, 36, 10) */
    plutovg_rect_t legacy;
    plutovg_canvas_stroke_extents(canvas, &legacy);
    check_rect(scene, "legacy extents", legacy, 12, 15, 36, 10);

    plutovg_canvas_set_stroke_transform(canvas, PLUTOVG_STROKE_TRANSFORM_GEOMETRIC);
    check_int(scene, "getter returns GEOMETRIC after set",
        plutovg_canvas_get_stroke_transform(canvas), PLUTOVG_STROKE_TRANSFORM_GEOMETRIC);

    plutovg_rect_t geo;
    plutovg_canvas_stroke_extents(canvas, &geo);
    check_rect(scene, "geometric extents", geo, 12, 18, 36, 4);

    /* queries use device coordinates: cap ends, shaft and outside points */
    check_bool(scene, "contains shaft center (30,20)", plutovg_canvas_stroke_contains(canvas, 30, 20), 1);
    check_bool(scene, "contains above shaft (30,17)", plutovg_canvas_stroke_contains(canvas, 30, 17), 0);
    check_bool(scene, "contains below shaft (30,23)", plutovg_canvas_stroke_contains(canvas, 30, 23), 0);
    check_bool(scene, "contains before cap (11,20)", plutovg_canvas_stroke_contains(canvas, 11, 20), 0);
    check_bool(scene, "contains after cap (49,20)", plutovg_canvas_stroke_contains(canvas, 49, 20), 0);

    /* queries must not consume or rewrite the path */
    int elements_before = plutovg_path_get_elements(plutovg_canvas_get_path(canvas), NULL);
    float cx_before, cy_before;
    plutovg_canvas_get_current_point(canvas, &cx_before, &cy_before);
    plutovg_canvas_stroke_extents(canvas, &geo);
    plutovg_canvas_stroke_contains(canvas, 30, 20);
    int elements_after = plutovg_path_get_elements(plutovg_canvas_get_path(canvas), NULL);
    float cx_after, cy_after;
    plutovg_canvas_get_current_point(canvas, &cx_after, &cy_after);
    check_int(scene, "extents/contains preserve path element count", elements_after, elements_before);
    check_bool(scene, "extents/contains preserve current point",
        cx_after == cx_before && cy_after == cy_before, 1);

    /* real drawing through stroke_preserve, clip still applies (no clip here) */
    plutovg_canvas_stroke_preserve(canvas);
    check_int(scene, "stroke_preserve covers shaft pixel", (pixel(surface, 30, 20) >> 24) > 0, 1);
    check_int(scene, "stroke_preserve keeps path current point x", (int)cx_after, 16);

    plutovg_canvas_destroy(canvas);
}

/* S2: sheared round caps (and a round join), cap geometry in user space */
static void scene_sheared_caps(plutovg_surface_t* surface)
{
    const char* scene = "sheared round cap/join";
    plutovg_matrix_t matrix = PLUTOVG_MAKE_MATRIX(1, 0, 0.6f, 1, 10, 30);

    plutovg_canvas_t* round = plutovg_canvas_create(surface);
    plutovg_canvas_set_matrix(round, &matrix);
    plutovg_canvas_set_line_width(round, 4);
    plutovg_canvas_set_line_cap(round, PLUTOVG_LINE_CAP_ROUND);
    plutovg_canvas_set_line_join(round, PLUTOVG_LINE_JOIN_ROUND);
    plutovg_canvas_set_stroke_transform(round, PLUTOVG_STROKE_TRANSFORM_GEOMETRIC);
    plutovg_canvas_move_to(round, 10, 10);
    plutovg_canvas_line_to(round, 40, 10);
    plutovg_canvas_line_to(round, 40, 25);

    plutovg_rect_t extents;
    plutovg_canvas_stroke_extents(round, &extents);
    check_rect(scene, "round cap/join extents", extents, 23, 38, 45, 19);

    map_check_contains(scene, round, &matrix, 8.5f, 10, 1);  /* inside the round cap */
    map_check_contains(scene, round, &matrix, 7, 10, 0);      /* outside the cap radius */
    map_check_contains(scene, round, &matrix, 25, 10, 1);     /* shaft */
    map_check_contains(scene, round, &matrix, 25, 13, 0);     /* beside the shaft */
    map_check_contains(scene, round, &matrix, 40, 17.5f, 1);  /* round join region */

    plutovg_canvas_set_rgb(round, 0, 0, 1);
    plutovg_canvas_stroke(round);
    float dx, dy;
    plutovg_matrix_map(&matrix, 25, 10, &dx, &dy);
    check_int(scene, "stroked shaft pixel covered", (pixel(surface, (int)dx, (int)dy) >> 24) > 0, 1);
    plutovg_canvas_destroy(round);

    /* a butt cap on the same geometry must not cover the round-cap point */
    plutovg_surface_t* other = plutovg_surface_create(120, 120);
    plutovg_canvas_t* butt = plutovg_canvas_create(other);
    plutovg_canvas_set_matrix(butt, &matrix);
    plutovg_canvas_set_line_width(butt, 4);
    plutovg_canvas_set_line_cap(butt, PLUTOVG_LINE_CAP_BUTT);
    plutovg_canvas_set_stroke_transform(butt, PLUTOVG_STROKE_TRANSFORM_GEOMETRIC);
    plutovg_canvas_move_to(butt, 10, 10);
    plutovg_canvas_line_to(butt, 40, 10);
    map_check_contains(scene, butt, &matrix, 8.5f, 10, 0);
    plutovg_canvas_destroy(butt);
    plutovg_surface_destroy(other);
}

/* S3: closed hollow contour: interior fill is never a stroke hit */
static void scene_hollow_contour(plutovg_surface_t* surface)
{
    const char* scene = "closed hollow contour";
    plutovg_matrix_t matrix = PLUTOVG_MAKE_SCALE(2, 2);
    plutovg_canvas_t* canvas = plutovg_canvas_create(surface);
    plutovg_canvas_set_matrix(canvas, &matrix);
    plutovg_canvas_set_rgb(canvas, 0, 1, 0);
    plutovg_canvas_set_line_width(canvas, 2);
    plutovg_canvas_set_stroke_transform(canvas, PLUTOVG_STROKE_TRANSFORM_GEOMETRIC);

    plutovg_path_t* path = plutovg_path_create();
    plutovg_path_add_rect(path, 10, 10, 20, 20);
    plutovg_canvas_add_path(canvas, path);

    plutovg_rect_t extents;
    plutovg_canvas_stroke_extents(canvas, &extents);
    check_rect(scene, "hollow rect extents", extents, 18, 18, 44, 44);

    map_check_contains(scene, canvas, &matrix, 20, 20, 0); /* interior */
    map_check_contains(scene, canvas, &matrix, 15, 15, 0); /* interior corner region */
    map_check_contains(scene, canvas, &matrix, 10, 20, 1); /* left ring */
    map_check_contains(scene, canvas, &matrix, 20, 30, 1); /* bottom ring */

    plutovg_canvas_stroke(canvas); /* consumes the canvas path */
    check_int(scene, "ring pixel covered", (pixel(surface, 20, 40) >> 24) > 0, 1);
    check_int(scene, "interior pixel stays empty", (int)(pixel(surface, 40, 40) >> 24), 0);

    /* stroke_path chain on a fresh path, including an extents query on the
       standalone path before it is drawn */
    plutovg_path_t* stroke_path = plutovg_path_create();
    plutovg_path_add_rect(stroke_path, 5, 5, 8, 8);
    plutovg_canvas_new_path(canvas);
    plutovg_canvas_add_path(canvas, stroke_path);
    plutovg_rect_t path_extents;
    plutovg_canvas_stroke_extents(canvas, &path_extents);
    check_rect(scene, "stroke_path extents", path_extents, 8, 8, 20, 20);
    plutovg_canvas_stroke_path(canvas, stroke_path);
    check_int(scene, "stroke_path ring pixel covered", (pixel(surface, 10, 16) >> 24) > 0, 1);
    check_int(scene, "stroke_path interior pixel empty", (int)(pixel(surface, 14, 14) >> 24), 0);
    check_bool(scene, "stroke_path leaves an empty current path",
        plutovg_canvas_stroke_contains(canvas, 20, 20), 0);

    /* stroke_rect chain on a fresh path */
    plutovg_canvas_stroke_rect(canvas, 30, 30, 8, 8);
    map_check_contains(scene, canvas, &matrix, 34, 34, 0);
    plutovg_path_destroy(path);
    plutovg_path_destroy(stroke_path);
    plutovg_canvas_destroy(canvas);
}

/* S4: negative determinant transform flips the stroke together with the path */
static void scene_negative_determinant(plutovg_surface_t* surface)
{
    const char* scene = "negative determinant scale(2,-2)";
    plutovg_canvas_t* canvas = plutovg_canvas_create(surface);
    plutovg_canvas_scale(canvas, 2, -2);
    plutovg_canvas_translate(canvas, 0, -80);
    plutovg_canvas_set_line_width(canvas, 2);
    plutovg_canvas_set_stroke_transform(canvas, PLUTOVG_STROKE_TRANSFORM_GEOMETRIC);
    plutovg_canvas_circle(canvas, 20, 20, 10);

    plutovg_rect_t extents;
    plutovg_canvas_stroke_extents(canvas, &extents);
    check_rect(scene, "flipped circle extents", extents, 18, 98, 44, 44);

    plutovg_matrix_t matrix;
    plutovg_canvas_get_matrix(canvas, &matrix);
    map_check_contains(scene, canvas, &matrix, 20, 20, 0); /* hollow center */
    map_check_contains(scene, canvas, &matrix, 29, 20, 1); /* ring */
    map_check_contains(scene, canvas, &matrix, 20, 11, 1); /* ring after flip */

    plutovg_canvas_set_rgb(canvas, 1, 0, 1);
    plutovg_canvas_stroke(canvas);
    check_int(scene, "flipped ring pixel covered", (pixel(surface, 58, 120) >> 24) > 0, 1);
    check_int(scene, "flipped center pixel empty", (int)(pixel(surface, 40, 120) >> 24), 0);
    plutovg_canvas_destroy(canvas);
}

/* S5: vertical line under non-uniform scale stretches the thickness */
static void scene_vertical_line(plutovg_surface_t* surface)
{
    const char* scene = "vertical line scale(1,3)";
    plutovg_canvas_t* canvas = plutovg_canvas_create(surface);
    plutovg_canvas_scale(canvas, 1, 3);
    plutovg_canvas_set_line_width(canvas, 4);
    plutovg_canvas_set_stroke_transform(canvas, PLUTOVG_STROKE_TRANSFORM_GEOMETRIC);
    plutovg_canvas_move_to(canvas, 50, 10);
    plutovg_canvas_line_to(canvas, 50, 30);

    plutovg_rect_t extents;
    plutovg_canvas_stroke_extents(canvas, &extents);
    check_rect(scene, "vertical extents", extents, 48, 30, 4, 60);
    check_bool(scene, "contains stretched shaft (50,60)", plutovg_canvas_stroke_contains(canvas, 50, 60), 1);
    check_bool(scene, "contains beside shaft (45,60)", plutovg_canvas_stroke_contains(canvas, 45, 60), 0);
    plutovg_canvas_destroy(canvas);
}

/* S6: dash lengths are defined in user space */
static void scene_dashes(plutovg_surface_t* surface)
{
    const char* scene = "user-space dashes scale(2,1)";
    plutovg_canvas_t* canvas = plutovg_canvas_create(surface);
    float dashes[] = {6, 4};
    plutovg_canvas_set_dash(canvas, 0, dashes, 2);
    plutovg_canvas_scale(canvas, 2, 1);
    plutovg_canvas_set_line_width(canvas, 2);
    plutovg_canvas_set_stroke_transform(canvas, PLUTOVG_STROKE_TRANSFORM_GEOMETRIC);
    plutovg_canvas_move_to(canvas, 5, 60);
    plutovg_canvas_line_to(canvas, 45, 60);

    plutovg_rect_t extents;
    plutovg_canvas_stroke_extents(canvas, &extents);
    check_rect(scene, "dashed extents", extents, 10, 59, 72, 2);
    check_bool(scene, "contains first dash (14,60)", plutovg_canvas_stroke_contains(canvas, 14, 60), 1);
    check_bool(scene, "contains user-space gap (28,60)", plutovg_canvas_stroke_contains(canvas, 28, 60), 0);
    check_bool(scene, "contains second dash (40,60)", plutovg_canvas_stroke_contains(canvas, 40, 60), 1);
    plutovg_canvas_destroy(canvas);
}

/* S7: zero width and empty paths keep empty coverage */
static void scene_empty_coverage(plutovg_surface_t* surface)
{
    const char* scene = "empty and zero-width";
    plutovg_canvas_t* canvas = plutovg_canvas_create(surface);
    plutovg_canvas_set_line_width(canvas, 4);
    plutovg_canvas_set_stroke_transform(canvas, PLUTOVG_STROKE_TRANSFORM_GEOMETRIC);

    plutovg_rect_t extents;
    plutovg_canvas_stroke_extents(canvas, &extents);
    check_rect(scene, "empty path extents", extents, 0, 0, 0, 0);
    check_bool(scene, "empty path contains", plutovg_canvas_stroke_contains(canvas, 10, 10), 0);

    plutovg_canvas_move_to(canvas, 10, 10);
    plutovg_canvas_line_to(canvas, 40, 10);
    plutovg_canvas_set_line_width(canvas, 0);
    plutovg_canvas_stroke_extents(canvas, &extents);
    check_rect(scene, "zero width extents", extents, 0, 0, 0, 0);
    check_bool(scene, "zero width contains", plutovg_canvas_stroke_contains(canvas, 25, 10), 0);

    /* stroke_text funnels through the same stroke chain; with no font face
       loaded it contributes no coverage and must not disturb the surface */
    plutovg_canvas_set_line_width(canvas, 4);
    float advance = plutovg_canvas_stroke_text(canvas, "x", -1, PLUTOVG_TEXT_ENCODING_UTF8, 5, 5);
    check_bool(scene, "stroke_text without font face has zero advance", advance == 0.f, 1);
    plutovg_canvas_stroke_extents(canvas, &extents);
    check_rect(scene, "stroke_text without font face stays empty", extents, 0, 0, 0, 0);
    plutovg_canvas_destroy(canvas);
}

/* S8: clipped gradient stroke: unclipped queries, clipped drawing, stable paint sampling */
static void scene_clipped_gradient(plutovg_surface_t* surface)
{
    const char* scene = "clipped gradient stroke";
    plutovg_gradient_stop_t stops[2] = {
        {0, PLUTOVG_MAKE_COLOR(1, 0, 0, 1)},
        {1, PLUTOVG_MAKE_COLOR(0, 0, 1, 1)}
    };

    plutovg_canvas_t* canvas = plutovg_canvas_create(surface);
    plutovg_canvas_rect(canvas, 10, 10, 30, 40);
    plutovg_canvas_clip(canvas);
    plutovg_canvas_set_linear_gradient(canvas, 0, 0, 20, 0, PLUTOVG_SPREAD_METHOD_PAD, stops, 2, NULL);
    plutovg_canvas_scale(canvas, 2, 1);
    plutovg_canvas_set_line_width(canvas, 6);
    plutovg_canvas_set_stroke_transform(canvas, PLUTOVG_STROKE_TRANSFORM_GEOMETRIC);
    plutovg_canvas_move_to(canvas, 5, 20);
    plutovg_canvas_line_to(canvas, 35, 20);

    plutovg_rect_t extents;
    plutovg_canvas_stroke_extents(canvas, &extents);
    check_rect(scene, "extents ignore clip", extents, 10, 17, 60, 6);
    check_bool(scene, "contains ignores clip (45,20)", plutovg_canvas_stroke_contains(canvas, 45, 20), 1);
    check_bool(scene, "contains inside clip (15,20)", plutovg_canvas_stroke_contains(canvas, 15, 20), 1);

    plutovg_canvas_stroke(canvas);
    check_int(scene, "clip removes pixel (5,20)", (int)(pixel(surface, 5, 20) >> 24), 0);
    check_int(scene, "clip keeps pixel (15,20)", (pixel(surface, 15, 20) >> 24), 255);
    check_int(scene, "clip keeps pixel (36,20)", (pixel(surface, 36, 20) >> 24), 255);
    check_int(scene, "clip removes pixel (45,20)", (int)(pixel(surface, 45, 20) >> 24), 0);
    uint32_t left = pixel(surface, 15, 20);
    uint32_t right = pixel(surface, 36, 20);
    check_bool(scene, "gradient runs red to blue", ((left >> 16) & 0xff) > (left & 0xff) &&
        ((right & 0xff) > ((right >> 16) & 0xff)), 1);
    plutovg_canvas_destroy(canvas);

    /* paint sampling must be identical for legacy and geometric strokes on shared coverage */
    plutovg_surface_t* s_legacy = plutovg_surface_create(80, 80);
    plutovg_surface_t* s_geo = plutovg_surface_create(80, 80);
    plutovg_canvas_t* c_legacy = plutovg_canvas_create(s_legacy);
    plutovg_canvas_t* c_geo = plutovg_canvas_create(s_geo);
    plutovg_canvas_set_linear_gradient(c_legacy, 0, 0, 40, 0, PLUTOVG_SPREAD_METHOD_PAD, stops, 2, NULL);
    plutovg_canvas_set_linear_gradient(c_geo, 0, 0, 40, 0, PLUTOVG_SPREAD_METHOD_PAD, stops, 2, NULL);
    plutovg_canvas_scale(c_legacy, 2, 1);
    plutovg_canvas_scale(c_geo, 2, 1);
    plutovg_canvas_set_line_width(c_legacy, 6);
    plutovg_canvas_set_line_width(c_geo, 6);
    plutovg_canvas_set_stroke_transform(c_geo, PLUTOVG_STROKE_TRANSFORM_GEOMETRIC);
    plutovg_canvas_move_to(c_legacy, 5, 40);
    plutovg_canvas_line_to(c_legacy, 35, 40);
    plutovg_canvas_move_to(c_geo, 5, 40);
    plutovg_canvas_line_to(c_geo, 35, 40);
    plutovg_canvas_stroke(c_legacy);
    plutovg_canvas_stroke(c_geo);
    int mismatches = 0, shared = 0;
    for(int y = 34; y < 47; y++) {
        for(int x = 4; x < 76; x++) {
            uint32_t a = pixel(s_legacy, x, y);
            uint32_t b = pixel(s_geo, x, y);
            if((a >> 24) == 255 && (b >> 24) == 255) {
                shared++;
                if((a & 0xffffff) != (b & 0xffffff))
                    mismatches++;
            }
        }
    }
    check_bool(scene, "gradient colors identical on shared coverage", shared > 0 && mismatches == 0, 1);
    plutovg_canvas_destroy(c_legacy);
    plutovg_canvas_destroy(c_geo);
    plutovg_surface_destroy(s_legacy);
    plutovg_surface_destroy(s_geo);

    /* tiled textures must keep sampling through the canvas/paint matrices unchanged */
    plutovg_surface_t* tex = plutovg_surface_create(8, 8);
    unsigned char* tex_data = plutovg_surface_get_data(tex);
    int tex_stride = plutovg_surface_get_stride(tex);
    for(int y = 0; y < 8; y++)
        for(int x = 0; x < 8; x++)
            ((uint32_t*)(tex_data + y * tex_stride))[x] = ((x / 2 + y / 2) % 2) ? 0xffff00ff : 0xff00ffff;

    plutovg_surface_t* t_legacy = plutovg_surface_create(80, 80);
    plutovg_surface_t* t_geo = plutovg_surface_create(80, 80);
    plutovg_canvas_t* q_legacy = plutovg_canvas_create(t_legacy);
    plutovg_canvas_t* q_geo = plutovg_canvas_create(t_geo);
    plutovg_canvas_set_texture(q_legacy, tex, PLUTOVG_TEXTURE_TYPE_TILED, 1, NULL);
    plutovg_canvas_set_texture(q_geo, tex, PLUTOVG_TEXTURE_TYPE_TILED, 1, NULL);
    plutovg_canvas_scale(q_legacy, 2, 1);
    plutovg_canvas_scale(q_geo, 2, 1);
    plutovg_canvas_set_line_width(q_legacy, 6);
    plutovg_canvas_set_line_width(q_geo, 6);
    plutovg_canvas_set_stroke_transform(q_geo, PLUTOVG_STROKE_TRANSFORM_GEOMETRIC);
    plutovg_canvas_move_to(q_legacy, 5, 40);
    plutovg_canvas_line_to(q_legacy, 35, 40);
    plutovg_canvas_stroke(q_legacy);
    plutovg_canvas_move_to(q_geo, 5, 40);
    plutovg_canvas_line_to(q_geo, 35, 40);
    plutovg_canvas_stroke(q_geo);
    int tex_mismatches = 0, tex_shared = 0;
    for(int y = 34; y < 47; y++) {
        for(int x = 4; x < 76; x++) {
            uint32_t a = pixel(t_legacy, x, y);
            uint32_t b = pixel(t_geo, x, y);
            if((a >> 24) == 255 && (b >> 24) == 255) {
                tex_shared++;
                if((a & 0xffffff) != (b & 0xffffff))
                    tex_mismatches++;
            }
        }
    }
    check_bool(scene, "texture colors identical on shared coverage", tex_shared > 0 && tex_mismatches == 0, 1);
    plutovg_canvas_destroy(q_legacy);
    plutovg_canvas_destroy(q_geo);
    plutovg_surface_destroy(t_legacy);
    plutovg_surface_destroy(t_geo);
    plutovg_surface_destroy(tex);
}

/* S9: save/restore nesting and the internal freed-state reuse */
static void scene_save_restore(plutovg_surface_t* surface)
{
    const char* scene = "save/restore";
    plutovg_canvas_t* canvas = plutovg_canvas_create(surface);
    plutovg_canvas_set_line_width(canvas, 4);
    plutovg_canvas_scale(canvas, 3, 1);
    plutovg_canvas_move_to(canvas, 4, 20);
    plutovg_canvas_line_to(canvas, 16, 20);

    check_int(scene, "initial mode LEGACY", plutovg_canvas_get_stroke_transform(canvas), PLUTOVG_STROKE_TRANSFORM_LEGACY);
    plutovg_canvas_save(canvas);
    plutovg_canvas_set_stroke_transform(canvas, PLUTOVG_STROKE_TRANSFORM_GEOMETRIC);
    plutovg_canvas_save(canvas);
    check_int(scene, "nested mode GEOMETRIC", plutovg_canvas_get_stroke_transform(canvas), PLUTOVG_STROKE_TRANSFORM_GEOMETRIC);
    plutovg_rect_t geo;
    plutovg_canvas_stroke_extents(canvas, &geo);
    check_rect(scene, "nested geometric extents", geo, 12, 18, 36, 4);
    plutovg_canvas_restore(canvas);
    check_int(scene, "mode after inner restore", plutovg_canvas_get_stroke_transform(canvas), PLUTOVG_STROKE_TRANSFORM_GEOMETRIC);
    plutovg_canvas_restore(canvas);
    check_int(scene, "mode after outer restore", plutovg_canvas_get_stroke_transform(canvas), PLUTOVG_STROKE_TRANSFORM_LEGACY);

    plutovg_rect_t legacy;
    plutovg_canvas_stroke_extents(canvas, &legacy);
    check_rect(scene, "legacy rendering reproducible after restore", legacy, 12, 15, 36, 10);

    /* recycle the freed state object several times */
    for(int i = 0; i < 3; i++) {
        plutovg_canvas_save(canvas);
        check_int(scene, "reused state keeps LEGACY", plutovg_canvas_get_stroke_transform(canvas), PLUTOVG_STROKE_TRANSFORM_LEGACY);
        plutovg_canvas_set_stroke_transform(canvas, PLUTOVG_STROKE_TRANSFORM_GEOMETRIC);
        plutovg_canvas_restore(canvas);
        check_int(scene, "mode back to LEGACY after recycled restore", plutovg_canvas_get_stroke_transform(canvas), PLUTOVG_STROKE_TRANSFORM_LEGACY);
    }
    plutovg_canvas_destroy(canvas);
}

/* S10: miter join geometry and a transformed cubic curve */
static void scene_joins_and_curves(plutovg_surface_t* surface)
{
    const char* scene = "miter join and cubic curve";
    plutovg_canvas_t* canvas = plutovg_canvas_create(surface);
    plutovg_canvas_set_line_width(canvas, 4);
    plutovg_canvas_set_line_join(canvas, PLUTOVG_LINE_JOIN_MITER);
    plutovg_canvas_scale(canvas, 3, 1);
    plutovg_canvas_move_to(canvas, 5, 20);
    plutovg_canvas_line_to(canvas, 15, 20);
    plutovg_canvas_line_to(canvas, 15, 35);

    plutovg_rect_t legacy;
    plutovg_canvas_stroke_extents(canvas, &legacy);
    check_rect(scene, "legacy miter extents", legacy, 15, 15, 35, 20);
    plutovg_canvas_set_stroke_transform(canvas, PLUTOVG_STROKE_TRANSFORM_GEOMETRIC);
    plutovg_rect_t geo;
    plutovg_canvas_stroke_extents(canvas, &geo);
    check_rect(scene, "geometric miter extents", geo, 15, 18, 36, 17);
    plutovg_canvas_destroy(canvas);

    plutovg_surface_t* other = plutovg_surface_create(120, 120);
    plutovg_canvas_t* curve = plutovg_canvas_create(other);
    plutovg_matrix_t matrix = PLUTOVG_MAKE_MATRIX(-2, 0, 0.5f, 2, 90, 10);
    plutovg_canvas_set_matrix(curve, &matrix);
    plutovg_canvas_set_line_width(curve, 3);
    plutovg_canvas_set_line_cap(curve, PLUTOVG_LINE_CAP_ROUND);
    plutovg_canvas_set_stroke_transform(curve, PLUTOVG_STROKE_TRANSFORM_GEOMETRIC);
    plutovg_canvas_move_to(curve, 5, 5);
    plutovg_canvas_cubic_to(curve, 10, 25, 25, 25, 30, 5);

    plutovg_rect_t extents;
    plutovg_canvas_stroke_extents(curve, &extents);
    check_rect(scene, "cubic under shear/flip extents", extents, 29, 17, 57, 36);
    plutovg_canvas_set_rgb(curve, 0, 0.5f, 0.5f);
    plutovg_canvas_stroke(curve);
    int covered = 0;
    for(int y = (int)extents.y; y < extents.y + extents.h && !covered; y++)
        for(int x = (int)extents.x; x < extents.x + extents.w; x++)
            if(pixel(other, x, y) >> 24) { covered = 1; break; }
    check_int(scene, "cubic stroke produced coverage", covered, 1);
    plutovg_canvas_destroy(curve);
    plutovg_surface_destroy(other);
}

/* one small PNG showing all four required motifs */
static void write_png(const char* filename)
{
    const int size = 160;
    plutovg_surface_t* surface = plutovg_surface_create(size, size);
    plutovg_color_t white = PLUTOVG_MAKE_COLOR(1, 1, 1, 1);
    plutovg_surface_clear(surface, &white);
    plutovg_canvas_t* canvas = plutovg_canvas_create(surface);
    plutovg_canvas_set_stroke_transform(canvas, PLUTOVG_STROKE_TRANSFORM_GEOMETRIC);

    /* quadrant 1: geometric horizontal line (thin) above the legacy one (thick) */
    plutovg_canvas_save(canvas);
    plutovg_canvas_set_rgb(canvas, 1, 0, 0);
    plutovg_canvas_set_line_width(canvas, 4);
    plutovg_canvas_scale(canvas, 3, 1);
    plutovg_canvas_move_to(canvas, 4, 10);
    plutovg_canvas_line_to(canvas, 16, 10);
    plutovg_canvas_stroke(canvas);
    plutovg_canvas_set_stroke_transform(canvas, PLUTOVG_STROKE_TRANSFORM_LEGACY);
    plutovg_canvas_move_to(canvas, 4, 20);
    plutovg_canvas_line_to(canvas, 16, 20);
    plutovg_canvas_stroke(canvas);
    plutovg_canvas_restore(canvas);

    /* quadrant 2: sheared round cap and round join */
    plutovg_canvas_save(canvas);
    plutovg_matrix_t shear = PLUTOVG_MAKE_MATRIX(1, 0, 0.6f, 1, 85, 15);
    plutovg_canvas_set_matrix(canvas, &shear);
    plutovg_canvas_set_rgb(canvas, 0, 0, 1);
    plutovg_canvas_set_line_width(canvas, 4);
    plutovg_canvas_set_line_cap(canvas, PLUTOVG_LINE_CAP_ROUND);
    plutovg_canvas_set_line_join(canvas, PLUTOVG_LINE_JOIN_ROUND);
    plutovg_canvas_move_to(canvas, 5, 10);
    plutovg_canvas_line_to(canvas, 28, 10);
    plutovg_canvas_line_to(canvas, 28, 26);
    plutovg_canvas_stroke(canvas);
    plutovg_canvas_restore(canvas);

    /* quadrant 3: closed hollow contour */
    plutovg_canvas_save(canvas);
    plutovg_canvas_translate(canvas, 8, 88);
    plutovg_canvas_scale(canvas, 2, 2);
    plutovg_canvas_set_rgb(canvas, 0, 0.6f, 0);
    plutovg_canvas_set_line_width(canvas, 2);
    plutovg_canvas_circle(canvas, 12, 12, 8);
    plutovg_canvas_stroke(canvas);
    plutovg_canvas_restore(canvas);

    /* quadrant 4: clipped gradient stroke, clip box outlined in gray */
    plutovg_canvas_save(canvas);
    plutovg_gradient_stop_t stops[2] = {
        {0, PLUTOVG_MAKE_COLOR(1, 0, 1, 1)},
        {1, PLUTOVG_MAKE_COLOR(0, 1, 1, 1)}
    };
    plutovg_canvas_set_rgb(canvas, 0.5f, 0.5f, 0.5f);
    plutovg_canvas_set_line_width(canvas, 1);
    plutovg_canvas_stroke_rect(canvas, 88, 92, 64, 60);
    plutovg_canvas_rect(canvas, 88, 92, 64, 60);
    plutovg_canvas_clip(canvas);
    plutovg_canvas_translate(canvas, 80, 80);
    plutovg_canvas_scale(canvas, 2, 1);
    plutovg_canvas_set_linear_gradient(canvas, 0, 0, 20, 0, PLUTOVG_SPREAD_METHOD_PAD, stops, 2, NULL);
    plutovg_canvas_set_line_width(canvas, 6);
    plutovg_canvas_move_to(canvas, 2, 15);
    plutovg_canvas_line_to(canvas, 38, 15);
    plutovg_canvas_stroke(canvas);
    plutovg_canvas_restore(canvas);

    int ok = plutovg_surface_write_to_png(surface, filename);
    plutovg_canvas_destroy(canvas);
    plutovg_surface_destroy(surface);
    if(!ok) {
        note_failure("png", "write_to_png", filename);
    }
}

int main(int argc, char** argv)
{
    /* dedicated surfaces per scenario keep the numeric assertions independent */
    plutovg_surface_t* s1 = plutovg_surface_create(64, 48);
    plutovg_surface_t* s2 = plutovg_surface_create(120, 120);
    plutovg_surface_t* s3 = plutovg_surface_create(120, 120);
    plutovg_surface_t* s4 = plutovg_surface_create(120, 160);
    plutovg_surface_t* s5 = plutovg_surface_create(120, 120);
    plutovg_surface_t* s6 = plutovg_surface_create(120, 120);
    plutovg_surface_t* s7 = plutovg_surface_create(120, 120);
    plutovg_surface_t* s8 = plutovg_surface_create(80, 80);
    plutovg_surface_t* s9 = plutovg_surface_create(64, 48);
    plutovg_surface_t* s10 = plutovg_surface_create(120, 120);

    scene_horizontal_line(s1);
    scene_sheared_caps(s2);
    scene_hollow_contour(s3);
    scene_negative_determinant(s4);
    scene_vertical_line(s5);
    scene_dashes(s6);
    scene_empty_coverage(s7);
    scene_clipped_gradient(s8);
    scene_save_restore(s9);
    scene_joins_and_curves(s10);

    const char* png = argc > 1 ? argv[1] : "geometric_stroke.png";
    write_png(png);

    plutovg_surface_destroy(s1);
    plutovg_surface_destroy(s2);
    plutovg_surface_destroy(s3);
    plutovg_surface_destroy(s4);
    plutovg_surface_destroy(s5);
    plutovg_surface_destroy(s6);
    plutovg_surface_destroy(s7);
    plutovg_surface_destroy(s8);
    plutovg_surface_destroy(s9);
    plutovg_surface_destroy(s10);

    printf("geometric stroke: %d checks, %d failures\n", g_checks, g_failures);
    if(g_failures == 0)
        printf("PNG written to %s\n", png);
    return g_failures ? 1 : 0;
}
