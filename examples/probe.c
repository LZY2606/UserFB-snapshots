#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gl.h"
#include "probe.h"

/********************************************************************
 *                                                                  *
 *                     probe capture example                        *
 *                                                                  *
 ********************************************************************/

/**
 * probe.c
 * -------
 * renders a small scene three times into off-screen buffers:
 *
 *   unbound.ppm  - the old phong output with no probe field bound
 *   partial.ppm  - a probe field bound with no complete generation
 *                  yet, which must match unbound.ppm
 *   complete.ppm - the probe field bound with a complete generation,
 *                  which receives indirect light from the glowing
 *                  panel bouncing off the black wall geometry
 *
 * the receiver is a dark diffuse quad lit only by the glow of a
 * distant panel.  a black wall stands between two probes and the panel
 * on one side so the captured distance moments carry real occlusion.
 *
 * PPM has no alpha, so rgb_int's alpha channel is dropped on write.
 */

#define WIDTH  640
#define HEIGHT 480

struct scene_data {
    float *positions;
    int vertex_count;
    struct probe_triangle *triangles;
    int triangle_count;
};

static struct scene_data g_scene;

/********************************************************************
 *                                                                  *
 *                           scene setup                            *
 *                                                                  *
 ********************************************************************/

static void
add_quad(float *pos, struct probe_triangle *tri, int *nv, int *nt,
         const float a[3], const float b[3], const float c[3],
         const float d[3], const float radiance[3])
{
    int base = *nv;
    memcpy(pos + base * 3, a, 3 * sizeof(float));
    memcpy(pos + (base + 1) * 3, b, 3 * sizeof(float));
    memcpy(pos + (base + 2) * 3, c, 3 * sizeof(float));
    memcpy(pos + (base + 3) * 3, d, 3 * sizeof(float));

    tri[*nt].index[0] = base + 0;
    tri[*nt].index[1] = base + 1;
    tri[*nt].index[2] = base + 2;
    memcpy(tri[*nt].radiance, radiance, 3 * sizeof(float));
    (*nt)++;

    tri[*nt].index[0] = base + 0;
    tri[*nt].index[1] = base + 2;
    tri[*nt].index[2] = base + 3;
    memcpy(tri[*nt].radiance, radiance, 3 * sizeof(float));
    (*nt)++;

    *nv += 4;
}

static void
build_scene(void)
{
    int nv = 0, nt = 0;
    float panel[3] = {5.0f, 5.0f, 5.0f};
    float black[3] = {0, 0, 0};

    g_scene.positions = malloc(64 * 3 * sizeof(float));
    g_scene.triangles = malloc(64 * sizeof(struct probe_triangle));

    /* glowing panel on the far side of the probe volume */
    add_quad(g_scene.positions, g_scene.triangles, &nv, &nt,
             (float[3]){-9, -9, 10}, (float[3]){9, -9, 10},
             (float[3]){ 9,  9, 10}, (float[3]){-9, 9, 10}, panel);

    /* a black wall standing between the panel and the receiver: it
     * casts a soft captured shadow across the receiver's indirect
     * light while leaving the panel visible around its edges */
    add_quad(g_scene.positions, g_scene.triangles, &nv, &nt,
             (float[3]){-4, -4, 4}, (float[3]){4, -4, 4},
             (float[3]){ 4,  4, 4}, (float[3]){-4, 4, 4}, black);

    /* a side wall to make the indirect term directional */
    add_quad(g_scene.positions, g_scene.triangles, &nv, &nt,
             (float[3]){6, -6, -1}, (float[3]){6, -6, 9},
             (float[3]){6,  6, 9}, (float[3]){6,  6, -1}, black);

    g_scene.vertex_count = nv;
    g_scene.triangle_count = nt;
}

/********************************************************************
 *                                                                  *
 *                            rendering                             *
 *                                                                  *
 ********************************************************************/

static void
clear(uint32_t *colors, float *depths)
{
    for (int i = 0; i < WIDTH * HEIGHT; i++) {
        colors[i] = 0xff000000u;
        depths[i] = 1000.0f;
    }
}

static void
draw_receiver(struct probe_set *set, int with_probes)
{
    /* a receiving quad at z = 0 facing the camera and the panel; the
     * standard vertex shader wants x y z u v nx ny nz */
    float verts[] = {
        -7, -7, 0, 0, 0, 0, 0, 1,
         7, -7, 0, 0, 0, 0, 0, 1,
         7,  7, 0, 0, 0, 0, 0, 1,
        -7, -7, 0, 0, 0, 0, 0, 1,
         7,  7, 0, 0, 0, 0, 0, 1,
        -7,  7, 0, 0, 0, 0, 0, 1,
    };
    int idx[] = {0, 1, 2, 3, 4, 5};

    (void)set;
    gl_bind_vertices(verts, 6, 8);
    if (with_probes)
        gl_bind_probe_volume(probe_gl_volume(set, 0.001f));
    else
        gl_bind_probe_volume(NULL);
    gl_renderl(idx, 6, GL_TRIANGLE_LIST);
}

static void
setup_pipeline(uint32_t *colors, float *depths)
{
    float zero = 0;
    float one = 1;
    float ka = 0;
    float kd = 1;
    float ks = 0;
    float ambient[4] = {0, 0, 0, 0};
    float diffuse[4] = {1, 1, 1, 1};
    float specular[4] = {0, 0, 0, 0};
    float shininess = 8;

    gl_glight(GL_AMBIENT, &ka);
    gl_glight(GL_DIFFUSE, &kd);
    gl_glight(GL_SPECULAR, &ks);
    (void)zero;
    (void)one;
    for (int i = 0; i < GL_MAX_LIGHT_COUNT; i++)
        gl_light_disable((enum gl_light_slot)i);

    gl_material(GL_AMBIENT, ambient);
    gl_material(GL_DIFFUSE, diffuse);
    gl_material(GL_SPECULAR, specular);
    gl_material(GL_SHININESS, &shininess);
    gl_material(GL_BLEND, &zero);

    gl_bind_framebuffer(WIDTH, HEIGHT, colors, depths);
    gl_bind_std_vs();
    gl_bind_phong_fs();

    gl_matrix_mode(GL_PROJECTION_MATRIX);
    gl_load_identity();
    gl_perspective(1.1f, (float)WIDTH / (float)HEIGHT, 0.1f, 200.0f);
    gl_matrix_mode(GL_VIEW_MATRIX);
    gl_load_identity();
    gl_look_at(0, 0, 17, 0, 0, 0, 0, 1, 0);
    gl_matrix_mode(GL_MODEL_MATRIX);
    gl_load_identity();
}

static int
write_ppm(const char *path, const uint32_t *colors)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return 0;
    fprintf(f, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
    for (int i = 0; i < WIDTH * HEIGHT; i++) {
        uint8_t rgb[3];
        rgb[0] = (uint8_t)((colors[i] >> 16) & 0xff);   /* r */
        rgb[1] = (uint8_t)((colors[i] >> 8) & 0xff);    /* g */
        rgb[2] = (uint8_t)(colors[i] & 0xff);           /* b */
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return 1;
}

/********************************************************************
 *                                                                  *
 *                              main                                *
 *                                                                  *
 ********************************************************************/

int
main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : ".";
    char path[1024];
    uint32_t *colors = malloc(WIDTH * HEIGHT * sizeof(uint32_t));
    float *depths = malloc(WIDTH * HEIGHT * sizeof(float));
    struct probe_scene scene;
    struct probe_config cfg;
    struct probe_set *set;
    float env[3] = {0, 0, 0};

    build_scene();
    scene.positions = g_scene.positions;
    scene.vertex_count = g_scene.vertex_count;
    scene.triangles = g_scene.triangles;
    scene.triangle_count = g_scene.triangle_count;

    memset(&cfg, 0, sizeof(cfg));
    cfg.origin[0] = -1.5f;
    cfg.origin[1] = -1.5f;
    cfg.origin[2] = 0.0f;
    cfg.spacing = 1.0f;
    cfg.nx = cfg.ny = 4;
    cfg.nz = 8;
    cfg.face_size = 16;
    cfg.near_plane = 0.05f;
    cfg.far_plane = 30.0f;

    set = probe_create(&cfg, &scene);
    if (!set) {
        fprintf(stderr, "probe_create failed\n");
        return 1;
    }

    setup_pipeline(colors, depths);

    /* 1. classic output, nothing bound */
    gl_bind_probe_volume(NULL);
    clear(colors, depths);
    draw_receiver(set, 0);
    snprintf(path, sizeof(path), "%s/unbound.ppm", out);
    if (!write_ppm(path, colors))
        return 1;

    /* 2. a volume bound before any generation exists: no data, so the
     *    image must be identical to unbound */
    clear(colors, depths);
    draw_receiver(set, 1);
    snprintf(path, sizeof(path), "%s/partial.ppm", out);
    if (!write_ppm(path, colors))
        return 1;

    /* capture the scene, then draw the indirect image */
    if (!probe_begin_update(set, env)) {
        fprintf(stderr, "probe_begin_update failed\n");
        return 1;
    }
    while (probe_step(set, 16) == 0) { }

    clear(colors, depths);
    draw_receiver(set, 1);
    snprintf(path, sizeof(path), "%s/complete.ppm", out);
    if (!write_ppm(path, colors))
        return 1;

    /* a little proof for the console */
    {
        float E[3];
        float P[3] = {-3, 0, 0};
        float N[3] = {0, 0, 1};
        if (probe_sample(set, P, N, 0.001f, E))
            printf("indirect irradiance at receiver (-3, 0, 0): "
                   "%.4f %.4f %.4f\n", E[0], E[1], E[2]);
    }

    probe_destroy(set);
    free(colors);
    free(depths);
    free(g_scene.positions);
    free(g_scene.triangles);
    return 0;
}
