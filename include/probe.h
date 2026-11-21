#ifndef PROBE_H
#define PROBE_H

/********************************************************************
 *                                                                  *
 *                   world-space irradiance probes                  *
 *                                                                  *
 ********************************************************************/

/**
 * probe.h
 * -------
 * a grid of world-space irradiance probes captured with the existing
 * software rasterizer.  every probe renders the scene into the six
 * faces of a cube, keeps a floating point HDR radiance image plus a
 * directional distance-moment image per face, projects the radiance
 * onto nine real spherical-harmonic bands and stores the
 * lambert-convolved irradiance.
 *
 * a probe set is double buffered: capture work is committed through
 * probe_begin_update / probe_step, and nothing becomes visible to
 * probe_sample until every probe of the update owns all six faces and
 * its coefficients.  before that, probe_sample keeps reading the last
 * complete generation (or reports no data).
 *
 * scene triangles are two-sided, live in world space and carry a
 * non-negative linear RGB emitted radiance.  radiance is the light
 * leaving a surface along the ray that hit it, so a black surface
 * blocks light and a lit surface illuminates its surroundings.
 */

struct probe_config {
    float origin[3];   /* world position of probe (0, 0, 0) */
    float spacing;     /* positive distance between neighbouring probes */
    int nx, ny, nz;    /* probes per axis, each in [2, 16] */
    int face_size;     /* cube face resolution, in [8, 64] */
    float near_plane;  /* positive near distance along a face axis */
    float far_plane;   /* positive far distance along a face axis */
};

struct probe_triangle {
    int index[3];      /* indices into probe_scene.positions */
    float radiance[3]; /* non-negative linear RGB emitted radiance */
};

struct probe_scene {
    const float *positions;              /* 3 floats per vertex */
    int vertex_count;
    const struct probe_triangle *triangles;
    int triangle_count;
};

struct probe_set;

/* lifecycle ------------------------------------------------------ */

/* copies the configuration and the scene, returning null when either
 * is invalid.  the source arrays may be modified or freed afterwards. */
struct probe_set *probe_create(
    const struct probe_config *config,
    const struct probe_scene *scene);
void probe_destroy(struct probe_set *set);

/* capture scheduling --------------------------------------------- */

/* freezes a private snapshot of the scene together with the given
 * environment as a new pending generation, replacing any unfinished
 * update.  the last complete generation stays readable.  rays that
 * miss every surface report the environment radiance at the face's
 * far distance.  returns 0 when the environment is invalid. */
int probe_begin_update(
    struct probe_set *set,
    const float environment[3]); /* constant radiance, null = black */

/* completes at most face_budget whole cube faces of the pending
 * update; a budget of zero does no work.  returns the number of
 * generations published (0 or 1). */
int probe_step(struct probe_set *set, int face_budget);

/* discards unfinished capture work, keeping the last complete
 * generation. */
void probe_abort(struct probe_set *set);

/* queries --------------------------------------------------------- */

/* evaluates the last complete generation at p + bias * n.  bias must
 * be non-negative and n is a non-zero (unit) surface normal.  returns
 * 1 and writes irradiance when data is available, 0 otherwise. */
int probe_sample(
    const struct probe_set *set,
    const float p[3],
    const float n[3],
    float bias,
    float irradiance[3]);

/* reads back one face of one probe of the last complete generation,
 * probe index being ((z * ny + y) * nx + x) and face one of 0..5 in
 * the order +X, -X, +Y, -Y, +Z, -Z.  the returned array holds
 * face_size * face_size pixels of five floats each:
 *   emitted R, emitted G, emitted B, radial distance, distance squared
 * the pointer aliases set memory and stays valid until the next
 * published generation or probe_destroy.  returns 0 when no complete
 * generation exists. */
int probe_read_capture(
    const struct probe_set *set,
    int probe_index,
    int face,
    const float **texels,
    int *face_size);

/* reads the nine real SH irradiance coefficients of one probe for
 * each colour channel, stored channel-major: channel * 9 + band.
 * the basis is
 *   Y0  = 0.282095
 *   Y1  = 0.488603 y        Y2  = 0.488603 z
 *   Y3  = 0.488603 x
 *   Y4  = 1.092548 x y      Y5  = 1.092548 y z
 *   Y6  = 0.315392 (2z^2 - x^2 - y^2)
 *   Y7  = 1.092548 x z      Y8  = 0.546274 (x^2 - y^2)
 * band 0 is convolved by pi, band 1 by 2pi/3 and band 2 by pi/4.
 * returns 0 when no complete generation exists. */
int probe_read_coefficients(
    const struct probe_set *set,
    int probe_index,
    float irradiance[27]);

/* seam-aware bilinear lookup of the distance moments of one probe
 * along direction, used by probe_sample.  returns 0 without a complete
 * generation. */
int probe_read_distance_moment(
    const struct probe_set *set,
    int probe_index,
    const float direction[3],
    float *distance,
    float *distance_squared);

/* geometry helpers ----------------------------------------------- */

/* orientation of a cube face: forward, up and right = cross(forward,up) */
void probe_face_axes(int face, float forward[3], float up[3], float right[3]);

/* unit direction through the centre of texel (x, y) of a face with
 * resolution n, with u = 2(x + .5)/n - 1 and v = 1 - 2(y + .5)/n */
void probe_texel_direction(int face, int x, int y, int n, float direction[3]);

/* solid angle subtended by one texel, integrated over its four corners */
float probe_texel_solid_angle(int face, int x, int y, int n);

/********************************************************************
 *                                                                  *
 *                          phong binding                           *
 *                                                                  *
 ********************************************************************/

struct gl_probe_volume;

/* builds the nullable field consumed by the standard phong shader.
 * the field evaluates the last complete generation at world-space
 * positions offset by bias along the surface normal.  the returned
 * pointer aliases set memory and is freed by probe_destroy. */
struct gl_probe_volume *probe_gl_volume(struct probe_set *set, float bias);

/* binds a probe field, or null to restore the classic phong output */
void gl_bind_probe_volume(struct gl_probe_volume *volume);

#endif /* PROBE_H */
