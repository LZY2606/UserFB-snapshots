// tests for ogt_mesh_from_paletted_voxels_atlas.
// registered with the task driver via task_cases/task_case_count. no main() and no
// implementation macro here - the implementation lives in task_support/library.cpp.
#include "task_support/check.h"
#include "src/ogt_voxel_meshify.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

//-----------------------------------------------------------------------------------------------------------------
// helpers
//-----------------------------------------------------------------------------------------------------------------

static ogt_mesh_rgba make_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    ogt_mesh_rgba c;
    c.r = r; c.g = g; c.b = b; c.a = a;
    return c;
}

// an allocator that tracks live allocations and can be told to fail the Nth allocation.
struct alloc_tracker {
    int live;
    int calls;
    int fail_after; // if >= 0, the allocation with call index == fail_after fails.
};

static void* tracked_alloc(size_t size, void* user_data) {
    alloc_tracker* t = (alloc_tracker*)user_data;
    if (t->fail_after >= 0 && t->calls >= t->fail_after)
        return NULL;
    t->calls++;
    void* p = malloc(size);
    if (p)
        t->live++;
    return p;
}

static void tracked_free(void* ptr, void* user_data) {
    if (ptr) {
        ((alloc_tracker*)user_data)->live--;
        free(ptr);
    }
}

static ogt_voxel_meshify_context make_tracker_ctx(alloc_tracker* t) {
    ogt_voxel_meshify_context ctx;
    ctx.alloc_func = tracked_alloc;
    ctx.free_func = tracked_free;
    ctx.alloc_free_user_data = t;
    return ctx;
}

static ogt_voxel_meshify_context make_default_ctx() {
    ogt_voxel_meshify_context ctx;
    ctx.alloc_func = NULL;
    ctx.free_func = NULL;
    ctx.alloc_free_user_data = NULL;
    return ctx;
}

// samples the atlas texel that contains the given uv coordinate (nearest, uv origin top-left).
static ogt_mesh_rgba sample_atlas(const ogt_mesh_atlas* atlas, float u, float v) {
    int tx = (int)floorf(u * (float)atlas->width);
    int ty = (int)floorf(v * (float)atlas->height);
    if (tx < 0) tx = 0;
    if (ty < 0) ty = 0;
    if (tx >= (int)atlas->width)  tx = (int)atlas->width - 1;
    if (ty >= (int)atlas->height) ty = (int)atlas->height - 1;
    return atlas->pixels[(uint32_t)ty * atlas->width + (uint32_t)tx];
}

// computes the barycentric weights of p within triangle abc. returns false if p is not
// on the triangle (within a small epsilon).
static bool point_in_triangle(const float p[3], const ogt_mesh_vec3& a, const ogt_mesh_vec3& b, const ogt_mesh_vec3& c, float* w0, float* w1, float* w2) {
    float v0[3] = { b.x - a.x, b.y - a.y, b.z - a.z };
    float v1[3] = { c.x - a.x, c.y - a.y, c.z - a.z };
    float v2[3] = { p[0] - a.x, p[1] - a.y, p[2] - a.z };
    float n[3] = {
        v0[1] * v1[2] - v0[2] * v1[1],
        v0[2] * v1[0] - v0[0] * v1[2],
        v0[0] * v1[1] - v0[1] * v1[0]
    };
    float len2 = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
    float dist = n[0] * v2[0] + n[1] * v2[1] + n[2] * v2[2];
    if (dist * dist > 1e-6f * len2)
        return false; // not on the plane of the triangle
    float d00 = v0[0] * v0[0] + v0[1] * v0[1] + v0[2] * v0[2];
    float d01 = v0[0] * v1[0] + v0[1] * v1[1] + v0[2] * v1[2];
    float d11 = v1[0] * v1[0] + v1[1] * v1[1] + v1[2] * v1[2];
    float d20 = v2[0] * v0[0] + v2[1] * v0[1] + v2[2] * v0[2];
    float d21 = v2[0] * v1[0] + v2[1] * v1[1] + v2[2] * v1[2];
    float denom = d00 * d11 - d01 * d01;
    if (denom <= 0.0f)
        return false;
    float v = (d11 * d20 - d01 * d21) / denom;
    float w = (d00 * d21 - d01 * d20) / denom;
    float u = 1.0f - v - w;
    const float eps = 1e-4f;
    if (u < -eps || v < -eps || w < -eps)
        return false;
    *w0 = u; *w1 = v; *w2 = w;
    return true;
}

static const int k_dirs[6][3] = {
    { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }
};

// independently enumerates every visible unit face of the voxel grid, finds the triangle
// containing the center of that face, interpolates uv within the triangle and checks that
// sampling the atlas there reproduces the original palette RGBA of the voxel. also checks
// the triangle normal matches the face direction.
static int check_unit_faces(const uint8_t* voxels, uint32_t size_x, uint32_t size_y, uint32_t size_z,
                            const ogt_mesh_rgba* palette, const ogt_mesh_atlas* atlas) {
    for (uint32_t z = 0; z < size_z; z++) {
        for (uint32_t y = 0; y < size_y; y++) {
            for (uint32_t x = 0; x < size_x; x++) {
                uint8_t index = voxels[x + y * size_x + z * size_x * size_y];
                if (!index)
                    continue;
                for (int d = 0; d < 6; d++) {
                    int nx = (int)x + k_dirs[d][0];
                    int ny = (int)y + k_dirs[d][1];
                    int nz = (int)z + k_dirs[d][2];
                    bool visible = (nx < 0 || ny < 0 || nz < 0 ||
                                    nx >= (int)size_x || ny >= (int)size_y || nz >= (int)size_z ||
                                    voxels[nx + ny * size_x + nz * size_x * size_y] == 0);
                    if (!visible)
                        continue;
                    float p[3] = {
                        (float)x + 0.5f + 0.5f * (float)k_dirs[d][0],
                        (float)y + 0.5f + 0.5f * (float)k_dirs[d][1],
                        (float)z + 0.5f + 0.5f * (float)k_dirs[d][2]
                    };
                    bool found = false;
                    for (uint32_t t = 0; t + 2 < atlas->index_count && !found; t += 3) {
                        const ogt_mesh_atlas_vertex* v0 = &atlas->vertices[atlas->indices[t + 0]];
                        const ogt_mesh_atlas_vertex* v1 = &atlas->vertices[atlas->indices[t + 1]];
                        const ogt_mesh_atlas_vertex* v2 = &atlas->vertices[atlas->indices[t + 2]];
                        float w0, w1, w2;
                        if (!point_in_triangle(p, v0->pos, v1->pos, v2->pos, &w0, &w1, &w2))
                            continue;
                        float u = w0 * v0->u + w1 * v1->u + w2 * v2->u;
                        float v = w0 * v0->v + w1 * v1->v + w2 * v2->v;
                        ogt_mesh_rgba sampled = sample_atlas(atlas, u, v);
                        TASK_CHECK(sampled.r == palette[index].r);
                        TASK_CHECK(sampled.g == palette[index].g);
                        TASK_CHECK(sampled.b == palette[index].b);
                        TASK_CHECK(sampled.a == palette[index].a);
                        TASK_CHECK(fabsf(v0->normal.x - (float)k_dirs[d][0]) < 1e-4f);
                        TASK_CHECK(fabsf(v0->normal.y - (float)k_dirs[d][1]) < 1e-4f);
                        TASK_CHECK(fabsf(v0->normal.z - (float)k_dirs[d][2]) < 1e-4f);
                        found = true;
                    }
                    TASK_CHECK(found);
                }
            }
        }
    }
    return 0;
}

// checks that every triangle is outward-facing: its geometric normal (from winding) must
// point along the vertex normal.
static int check_outward_triangles(const ogt_mesh_atlas* atlas) {
    for (uint32_t t = 0; t + 2 < atlas->index_count; t += 3) {
        const ogt_mesh_atlas_vertex* a = &atlas->vertices[atlas->indices[t + 0]];
        const ogt_mesh_atlas_vertex* b = &atlas->vertices[atlas->indices[t + 1]];
        const ogt_mesh_atlas_vertex* c = &atlas->vertices[atlas->indices[t + 2]];
        float ux = b->pos.x - a->pos.x, uy = b->pos.y - a->pos.y, uz = b->pos.z - a->pos.z;
        float vx = c->pos.x - a->pos.x, vy = c->pos.y - a->pos.y, vz = c->pos.z - a->pos.z;
        float nx = uy * vz - uz * vy;
        float ny = uz * vx - ux * vz;
        float nz = ux * vy - uy * vx;
        float dot = nx * a->normal.x + ny * a->normal.y + nz * a->normal.z;
        TASK_CHECK(dot > 0.0f);
    }
    return 0;
}

static ogt_mesh_rgba atlas_pixel(const ogt_mesh_atlas* atlas, uint32_t x, uint32_t y) {
    return atlas->pixels[y * atlas->width + x];
}

static int check_pixel(const ogt_mesh_atlas* atlas, uint32_t x, uint32_t y, ogt_mesh_rgba expected) {
    ogt_mesh_rgba p = atlas_pixel(atlas, x, y);
    TASK_CHECK(p.r == expected.r && p.g == expected.g && p.b == expected.b && p.a == expected.a);
    return 0;
}

//-----------------------------------------------------------------------------------------------------------------
// geometry group
//-----------------------------------------------------------------------------------------------------------------

// a 4x4x4 checkerboard of two colors must still merge into 6 rectangles (12 triangles)
// because merging no longer considers color.
static int case_checkerboard_block_low_triangle_count(void) {
    ogt_mesh_rgba palette[256];
    memset(palette, 0, sizeof(palette));
    palette[1] = make_rgba(10, 20, 30, 255);
    palette[2] = make_rgba(200, 100, 50, 255);

    uint8_t voxels[4 * 4 * 4];
    for (uint32_t z = 0; z < 4; z++)
        for (uint32_t y = 0; y < 4; y++)
            for (uint32_t x = 0; x < 4; x++)
                voxels[x + y * 4 + z * 16] = (uint8_t)(1 + ((x + y + z) & 1));

    alloc_tracker t = { 0, 0, -1 };
    ogt_voxel_meshify_context ctx = make_tracker_ctx(&t);
    ogt_mesh_atlas* atlas = ogt_mesh_from_paletted_voxels_atlas(&ctx, voxels, 4, 4, 4, palette, 64);
    TASK_CHECK(atlas != NULL);
    TASK_CHECK(atlas->vertex_count == 24);
    TASK_CHECK(atlas->index_count == 36);
    TASK_CHECK(atlas->width == 64);
    TASK_CHECK(atlas->height == 6);   // one 4x4 chart + 1 texel gutter on each side
    TASK_CHECK(atlas->chart_count == 1); // both checkerboard phases are rotation-equivalent
    TASK_CHECK(check_outward_triangles(atlas) == 0);
    TASK_CHECK(check_unit_faces(voxels, 4, 4, 4, palette, atlas) == 0);
    ogt_mesh_atlas_destroy(&ctx, atlas);
    TASK_CHECK(t.live == 0);
    return 0;
}

// a 2x1x1 row where the second color has alpha 0: the alpha-zero voxel is still solid,
// still occludes the shared face, and its (alpha-zero) color must appear on its outer faces.
static int case_alpha_zero_voxel_is_solid(void) {
    ogt_mesh_rgba palette[256];
    memset(palette, 0, sizeof(palette));
    palette[1] = make_rgba(1, 2, 3, 255);
    palette[2] = make_rgba(101, 102, 103, 0);

    uint8_t voxels[2] = { 1, 2 };
    ogt_voxel_meshify_context ctx = make_default_ctx();
    ogt_mesh_atlas* atlas = ogt_mesh_from_paletted_voxels_atlas(&ctx, voxels, 2, 1, 1, palette, 16);
    TASK_CHECK(atlas != NULL);
    TASK_CHECK(atlas->index_count == 36); // 6 rects, no internal face between the two voxels
    TASK_CHECK(atlas->vertex_count == 24);
    TASK_CHECK(atlas->chart_count == 3);  // one 1x2 chart for the sides, two 1x1 charts for the caps
    TASK_CHECK(check_outward_triangles(atlas) == 0);
    TASK_CHECK(check_unit_faces(voxels, 2, 1, 1, palette, atlas) == 0);
    ogt_mesh_atlas_destroy(&ctx, atlas);
    return 0;
}

// tiles that only differ by rotation or mirroring must share a single chart, and the uvs
// must still reconstruct the correct color for every unit face.
static int case_rotation_mirror_chart_reuse(void) {
    ogt_mesh_rgba palette[256];
    memset(palette, 0, sizeof(palette));
    palette[1] = make_rgba(10, 0, 0, 255);
    palette[2] = make_rgba(20, 0, 0, 255);
    palette[3] = make_rgba(30, 0, 0, 255);
    palette[4] = make_rgba(40, 0, 0, 255);

    ogt_voxel_meshify_context ctx = make_default_ctx();

    // 2x1x1 row of two colors: the 4 side faces produce 2x1 tiles in both orders ([1,2] and
    // [2,1]) which are mirror/rotation-equivalent, so they must share one chart.
    {
        uint8_t voxels[2] = { 1, 2 };
        ogt_mesh_atlas* atlas = ogt_mesh_from_paletted_voxels_atlas(&ctx, voxels, 2, 1, 1, palette, 16);
        TASK_CHECK(atlas != NULL);
        TASK_CHECK(atlas->chart_count == 3); // shared 1x2 side chart + two 1x1 cap charts
        TASK_CHECK(check_unit_faces(voxels, 2, 1, 1, palette, atlas) == 0);
        ogt_mesh_atlas_destroy(&ctx, atlas);
    }

    // 2x2x1 slab with 4 distinct colors: the front/back 2x2 tiles are mirror images and share
    // one chart; the 8 edge strips form 4 more unique charts.
    {
        uint8_t voxels[4] = { 1, 2, 3, 4 };
        ogt_mesh_atlas* atlas = ogt_mesh_from_paletted_voxels_atlas(&ctx, voxels, 2, 2, 1, palette, 16);
        TASK_CHECK(atlas != NULL);
        TASK_CHECK(atlas->index_count == 36); // 6 rects: front, back, and 4 edge strips
        TASK_CHECK(atlas->chart_count == 5);
        TASK_CHECK(check_outward_triangles(atlas) == 0);
        TASK_CHECK(check_unit_faces(voxels, 2, 2, 1, palette, atlas) == 0);
        ogt_mesh_atlas_destroy(&ctx, atlas);
    }
    return 0;
}

//-----------------------------------------------------------------------------------------------------------------
// texture group
//-----------------------------------------------------------------------------------------------------------------

// checks the gutter ring around a chart replicates the nearest edge texel (corners included),
// and that uv coordinates address the content region with unit-face centers on texel centers.
static int case_gutter_replicates_edge_texels(void) {
    ogt_mesh_rgba palette[256];
    memset(palette, 0, sizeof(palette));
    palette[1] = make_rgba(10, 1, 2, 3);
    palette[2] = make_rgba(20, 4, 5, 6);
    palette[3] = make_rgba(30, 7, 8, 9);
    palette[4] = make_rgba(40, 10, 11, 12);
    ogt_mesh_rgba c1 = palette[1], c2 = palette[2], c3 = palette[3], c4 = palette[4];

    ogt_voxel_meshify_context ctx = make_default_ctx();

    // 2x2x1 slab with 4 distinct colors. the canonical form of the 2x2 tile is the identity
    // [c1,c2;c3,c4]. the 4 strip charts (packed 3 wide) sort before the 2x2 chart, so with an
    // atlas width of 16 the 2x2 chart is packed at (12,0) with content at (13,1).
    uint8_t voxels[4] = { 1, 2, 3, 4 };
    ogt_mesh_atlas* atlas = ogt_mesh_from_paletted_voxels_atlas(&ctx, voxels, 2, 2, 1, palette, 16);
    TASK_CHECK(atlas != NULL);
    TASK_CHECK(atlas->width == 16);
    TASK_CHECK(atlas->height == 4);
    // content of the 2x2 chart
    TASK_CHECK(check_pixel(atlas, 13, 1, c1) == 0);
    TASK_CHECK(check_pixel(atlas, 14, 1, c2) == 0);
    TASK_CHECK(check_pixel(atlas, 13, 2, c3) == 0);
    TASK_CHECK(check_pixel(atlas, 14, 2, c4) == 0);
    // gutter: edges replicate the nearest edge texel
    TASK_CHECK(check_pixel(atlas, 13, 0, c1) == 0);
    TASK_CHECK(check_pixel(atlas, 14, 0, c2) == 0);
    TASK_CHECK(check_pixel(atlas, 13, 3, c3) == 0);
    TASK_CHECK(check_pixel(atlas, 14, 3, c4) == 0);
    TASK_CHECK(check_pixel(atlas, 12, 1, c1) == 0);
    TASK_CHECK(check_pixel(atlas, 12, 2, c3) == 0);
    TASK_CHECK(check_pixel(atlas, 15, 1, c2) == 0);
    TASK_CHECK(check_pixel(atlas, 15, 2, c4) == 0);
    // gutter: corners replicate the corner texel
    TASK_CHECK(check_pixel(atlas, 12, 0, c1) == 0);
    TASK_CHECK(check_pixel(atlas, 15, 0, c2) == 0);
    TASK_CHECK(check_pixel(atlas, 12, 3, c3) == 0);
    TASK_CHECK(check_pixel(atlas, 15, 3, c4) == 0);
    ogt_mesh_atlas_destroy(&ctx, atlas);

    // single voxel: one 1x1 chart packed at (0,0) of a 16-wide atlas -> height 3, the whole
    // 3x3 chart region replicates the single texel, everything else is zeroed.
    {
        uint8_t one[1] = { 1 };
        ogt_mesh_atlas* a2 = ogt_mesh_from_paletted_voxels_atlas(&ctx, one, 1, 1, 1, palette, 16);
        TASK_CHECK(a2 != NULL);
        TASK_CHECK(a2->width == 16 && a2->height == 3 && a2->chart_count == 1);
        for (uint32_t y = 0; y < 3; y++)
            for (uint32_t x = 0; x < 3; x++)
                TASK_CHECK(check_pixel(a2, x, y, c1) == 0);
        for (uint32_t y = 0; y < 3; y++)
            for (uint32_t x = 3; x < 16; x++)
                TASK_CHECK(check_pixel(a2, x, y, make_rgba(0, 0, 0, 0)) == 0);
        // every vertex uv must lie exactly on the content region boundary of the chart.
        for (uint32_t i = 0; i < a2->vertex_count; i++) {
            float u = a2->vertices[i].u;
            float v = a2->vertices[i].v;
            TASK_CHECK(fabsf(u - 1.0f / 16.0f) < 1e-5f || fabsf(u - 2.0f / 16.0f) < 1e-5f);
            TASK_CHECK(fabsf(v - 1.0f / 3.0f) < 1e-5f || fabsf(v - 2.0f / 3.0f) < 1e-5f);
        }
        ogt_mesh_atlas_destroy(&ctx, a2);
    }
    return 0;
}

// two different palette indices with identical RGBA must share the same chart.
static int case_same_rgba_different_index_shared(void) {
    ogt_mesh_rgba palette[256];
    memset(palette, 0, sizeof(palette));
    palette[1] = make_rgba(7, 8, 9, 200);
    palette[2] = make_rgba(7, 8, 9, 200);

    uint8_t voxels[3] = { 1, 0, 2 }; // two separated single voxels -> 12 one-texel tiles
    ogt_voxel_meshify_context ctx = make_default_ctx();
    ogt_mesh_atlas* atlas = ogt_mesh_from_paletted_voxels_atlas(&ctx, voxels, 3, 1, 1, palette, 16);
    TASK_CHECK(atlas != NULL);
    TASK_CHECK(atlas->index_count == 72);
    TASK_CHECK(atlas->chart_count == 1);
    TASK_CHECK(check_unit_faces(voxels, 3, 1, 1, palette, atlas) == 0);
    ogt_mesh_atlas_destroy(&ctx, atlas);
    return 0;
}

// checks shelf packing: charts are packed left-to-right from the top-left, wrapping to a new
// row when they don't fit, and the atlas height is exactly the used height (not padded).
static int case_shelf_packing_rows(void) {
    ogt_mesh_rgba palette[256];
    memset(palette, 0, sizeof(palette));
    palette[1] = make_rgba(10, 0, 0, 255);
    palette[2] = make_rgba(20, 0, 0, 255);
    palette[3] = make_rgba(30, 0, 0, 255);

    // three separated single voxels -> three 1x1 charts (packed 3x3 each), sorted by color.
    uint8_t voxels[5] = { 1, 0, 2, 0, 3 };
    ogt_voxel_meshify_context ctx = make_default_ctx();
    ogt_mesh_atlas* atlas = ogt_mesh_from_paletted_voxels_atlas(&ctx, voxels, 5, 1, 1, palette, 7);
    TASK_CHECK(atlas != NULL);
    TASK_CHECK(atlas->width == 7);
    TASK_CHECK(atlas->chart_count == 3);
    // two charts fit per row (3+3 <= 7), the third wraps to the next row: height 3+3 = 6.
    TASK_CHECK(atlas->height == 6);
    // chart contents: c1 at (1,1), c2 at (4,1), c3 at (1,4)
    TASK_CHECK(check_pixel(atlas, 1, 1, palette[1]) == 0);
    TASK_CHECK(check_pixel(atlas, 4, 1, palette[2]) == 0);
    TASK_CHECK(check_pixel(atlas, 1, 4, palette[3]) == 0);
    // the unused column on the first row (x=6) must be zeroed.
    TASK_CHECK(check_pixel(atlas, 6, 0, make_rgba(0, 0, 0, 0)) == 0);
    TASK_CHECK(check_pixel(atlas, 6, 1, make_rgba(0, 0, 0, 0)) == 0);
    TASK_CHECK(check_pixel(atlas, 6, 2, make_rgba(0, 0, 0, 0)) == 0);
    ogt_mesh_atlas_destroy(&ctx, atlas);
    return 0;
}

//-----------------------------------------------------------------------------------------------------------------
// ownership group
//-----------------------------------------------------------------------------------------------------------------

static int case_invalid_arguments_return_null(void) {
    ogt_mesh_rgba palette[256];
    memset(palette, 0, sizeof(palette));
    palette[1] = make_rgba(1, 2, 3, 4);
    uint8_t voxels[1] = { 1 };
    ogt_voxel_meshify_context ctx = make_default_ctx();

    TASK_CHECK(ogt_mesh_from_paletted_voxels_atlas(NULL, voxels, 1, 1, 1, palette, 8) == NULL);
    TASK_CHECK(ogt_mesh_from_paletted_voxels_atlas(&ctx, NULL, 1, 1, 1, palette, 8) == NULL);
    TASK_CHECK(ogt_mesh_from_paletted_voxels_atlas(&ctx, voxels, 1, 1, 1, NULL, 8) == NULL);
    // sizes must be within [1,64]
    TASK_CHECK(ogt_mesh_from_paletted_voxels_atlas(&ctx, voxels, 0, 1, 1, palette, 8) == NULL);
    TASK_CHECK(ogt_mesh_from_paletted_voxels_atlas(&ctx, voxels, 1, 0, 1, palette, 8) == NULL);
    TASK_CHECK(ogt_mesh_from_paletted_voxels_atlas(&ctx, voxels, 1, 1, 0, palette, 8) == NULL);
    TASK_CHECK(ogt_mesh_from_paletted_voxels_atlas(&ctx, voxels, 65, 1, 1, palette, 8) == NULL);
    TASK_CHECK(ogt_mesh_from_paletted_voxels_atlas(&ctx, voxels, 1, 65, 1, palette, 8) == NULL);
    TASK_CHECK(ogt_mesh_from_paletted_voxels_atlas(&ctx, voxels, 1, 1, 65, palette, 8) == NULL);
    // atlas_width must be within [3,512]
    TASK_CHECK(ogt_mesh_from_paletted_voxels_atlas(&ctx, voxels, 1, 1, 1, palette, 2) == NULL);
    TASK_CHECK(ogt_mesh_from_paletted_voxels_atlas(&ctx, voxels, 1, 1, 1, palette, 513) == NULL);
    return 0;
}

// a valid all-empty grid returns a freeable object with the requested width, height 1 and
// zero counts.
static int case_empty_grid_returns_freeable_object(void) {
    ogt_mesh_rgba palette[256];
    memset(palette, 0, sizeof(palette));
    palette[1] = make_rgba(1, 2, 3, 4);
    uint8_t voxels[4 * 4 * 4];
    memset(voxels, 0, sizeof(voxels));

    alloc_tracker t = { 0, 0, -1 };
    ogt_voxel_meshify_context ctx = make_tracker_ctx(&t);
    ogt_mesh_atlas* atlas = ogt_mesh_from_paletted_voxels_atlas(&ctx, voxels, 4, 4, 4, palette, 8);
    TASK_CHECK(atlas != NULL);
    TASK_CHECK(atlas->width == 8);
    TASK_CHECK(atlas->height == 1);
    TASK_CHECK(atlas->vertex_count == 0);
    TASK_CHECK(atlas->index_count == 0);
    TASK_CHECK(atlas->chart_count == 0);
    for (uint32_t i = 0; i < 8; i++)
        TASK_CHECK(check_pixel(atlas, i, 0, make_rgba(0, 0, 0, 0)) == 0);
    ogt_mesh_atlas_destroy(&ctx, atlas);
    TASK_CHECK(t.live == 0);
    TASK_CHECK(t.calls > 0);
    return 0;
}

// when any allocation fails, the call must return NULL and release everything it allocated;
// the inputs must be left unchanged.
static int case_allocation_failure_cleans_up(void) {
    ogt_mesh_rgba palette[256];
    memset(palette, 0, sizeof(palette));
    palette[1] = make_rgba(10, 20, 30, 255);
    palette[2] = make_rgba(200, 100, 50, 255);
    ogt_mesh_rgba palette_copy[256];
    memcpy(palette_copy, palette, sizeof(palette));

    uint8_t voxels[4 * 4 * 4];
    for (uint32_t z = 0; z < 4; z++)
        for (uint32_t y = 0; y < 4; y++)
            for (uint32_t x = 0; x < 4; x++)
                voxels[x + y * 4 + z * 16] = (uint8_t)(1 + ((x + y + z) & 1));
    uint8_t voxels_copy[4 * 4 * 4];
    memcpy(voxels_copy, voxels, sizeof(voxels));

    bool got_success = false;
    for (int fail_after = 0; fail_after < 64; fail_after++) {
        alloc_tracker t = { 0, 0, fail_after };
        ogt_voxel_meshify_context ctx = make_tracker_ctx(&t);
        ogt_mesh_atlas* atlas = ogt_mesh_from_paletted_voxels_atlas(&ctx, voxels, 4, 4, 4, palette, 64);
        if (!atlas) {
            TASK_CHECK(t.live == 0); // everything allocated by this call was released
        }
        else {
            ogt_mesh_atlas_destroy(&ctx, atlas);
            TASK_CHECK(t.live == 0);
            got_success = true;
        }
        // inputs must be unchanged
        TASK_CHECK(memcmp(voxels, voxels_copy, sizeof(voxels)) == 0);
        TASK_CHECK(memcmp(palette, palette_copy, sizeof(palette)) == 0);
        if (got_success)
            break;
    }
    TASK_CHECK(got_success);
    return 0;
}

// repeated calls with the same input must produce identical fields, pixels and indices.
static int case_repeat_calls_deterministic(void) {
    ogt_mesh_rgba palette[256];
    memset(palette, 0, sizeof(palette));
    palette[1] = make_rgba(10, 20, 30, 255);
    palette[2] = make_rgba(200, 100, 50, 255);
    palette[3] = make_rgba(1, 2, 3, 4);

    uint8_t voxels[4 * 4 * 4];
    for (uint32_t i = 0; i < sizeof(voxels); i++)
        voxels[i] = (uint8_t)((i * 7 + 3) % 4);

    ogt_voxel_meshify_context ctx = make_default_ctx();
    ogt_mesh_atlas* a = ogt_mesh_from_paletted_voxels_atlas(&ctx, voxels, 4, 4, 4, palette, 32);
    ogt_mesh_atlas* b = ogt_mesh_from_paletted_voxels_atlas(&ctx, voxels, 4, 4, 4, palette, 32);
    TASK_CHECK(a != NULL && b != NULL);
    TASK_CHECK(a->vertex_count == b->vertex_count);
    TASK_CHECK(a->index_count == b->index_count);
    TASK_CHECK(a->width == b->width);
    TASK_CHECK(a->height == b->height);
    TASK_CHECK(a->chart_count == b->chart_count);
    TASK_CHECK(memcmp(a->vertices, b->vertices, a->vertex_count * sizeof(ogt_mesh_atlas_vertex)) == 0);
    TASK_CHECK(memcmp(a->indices, b->indices, a->index_count * sizeof(uint32_t)) == 0);
    TASK_CHECK(memcmp(a->pixels, b->pixels, (size_t)a->width * a->height * sizeof(ogt_mesh_rgba)) == 0);
    ogt_mesh_atlas_destroy(&ctx, a);
    ogt_mesh_atlas_destroy(&ctx, b);
    return 0;
}

// a chart whose canonical width + 2 (gutter) exceeds atlas_width must fail with NULL.
static int case_oversized_chart_returns_null(void) {
    ogt_mesh_rgba palette[256];
    memset(palette, 0, sizeof(palette));
    palette[1] = make_rgba(10, 20, 30, 255);

    // a solid 2x2x1 slab produces 2x2 tiles, which need 4 texels of width with gutter.
    uint8_t voxels[4] = { 1, 1, 1, 1 };

    alloc_tracker t = { 0, 0, -1 };
    ogt_voxel_meshify_context ctx = make_tracker_ctx(&t);
    ogt_mesh_atlas* atlas = ogt_mesh_from_paletted_voxels_atlas(&ctx, voxels, 2, 2, 1, palette, 3);
    TASK_CHECK(atlas == NULL);
    TASK_CHECK(t.live == 0);

    atlas = ogt_mesh_from_paletted_voxels_atlas(&ctx, voxels, 2, 2, 1, palette, 4);
    TASK_CHECK(atlas != NULL);
    TASK_CHECK(atlas->width == 4);
    ogt_mesh_atlas_destroy(&ctx, atlas);
    TASK_CHECK(t.live == 0);
    return 0;
}

//-----------------------------------------------------------------------------------------------------------------
// registration
//-----------------------------------------------------------------------------------------------------------------

const struct task_case task_cases[] = {
    { "geometry",  "checkerboard_block_low_triangle_count", case_checkerboard_block_low_triangle_count },
    { "geometry",  "alpha_zero_voxel_is_solid",             case_alpha_zero_voxel_is_solid },
    { "geometry",  "rotation_mirror_chart_reuse",           case_rotation_mirror_chart_reuse },
    { "texture",   "gutter_replicates_edge_texels",         case_gutter_replicates_edge_texels },
    { "texture",   "same_rgba_different_index_shared",      case_same_rgba_different_index_shared },
    { "texture",   "shelf_packing_rows",                    case_shelf_packing_rows },
    { "ownership", "invalid_arguments_return_null",         case_invalid_arguments_return_null },
    { "ownership", "empty_grid_returns_freeable_object",    case_empty_grid_returns_freeable_object },
    { "ownership", "allocation_failure_cleans_up",          case_allocation_failure_cleans_up },
    { "ownership", "repeat_calls_deterministic",            case_repeat_calls_deterministic },
    { "ownership", "oversized_chart_returns_null",          case_oversized_chart_returns_null },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
