//
// LICENCE:
//  The MIT License (MIT)
//
//  Copyright (c) 2016 Karim Naaji, karim.naaji@gmail.com
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy
//  of this software and associated documentation files (the "Software"), to deal
//  in the Software without restriction, including without limitation the rights
//  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//  copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in all
//  copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE
//
// REFERENCES:
//  http://matthias-mueller-fischer.ch/publications/tetraederCollision.pdf
//  http://fileadmin.cs.lth.se/cs/Personal/Tomas_Akenine-Moller/code/tribox2.txt
//
// HOWTO:
//  #define VOXELIZER_IMPLEMENTATION
//  #define VOXELIZER_DEBUG // Only if assertions need to be checked
//  #include "voxelizer.h"
//
// HISTORY:
//  - 0.11.0 (15-11-2026): Closed-box occupancy over the full integer lattice
//                         (candidate range inflated by half voxel + precision,
//                         exact zero-area degeneracy test, no epsilon cell
//                         merging) and surface-area weighted colors integrated
//                         over the clipped triangle/box intersection
//  - 0.10.0 (20-03-2017): Add vx_voxelize_snap_3d_grid to voxelize to 3d-textures
//  - 0.9.2  (03-01-2017): Fix triangle bounding bouxes bounds for bbox-triangle
//                         intersection test
//  - 0.9.1  (12-10-2016): Add vx_voxelize_pc to generate point cloud as a result
//                         of voxelization
//  - 0.9    (01-05-2016): Initial
//
// TODO:
//  - Triangle face merging
//

#ifndef VOXELIZER_H
#define VOXELIZER_H

// ------------------------------------------------------------------------------------------------
// VOXELIZER PUBLIC API
//

#ifndef VOXELIZER_HELPERS
#include <stdlib.h> // malloc, calloc, free
#endif

typedef struct vx_vertex {
    union {
        float v[3];
        struct {
            float x;
            float y;
            float z;
        };
        struct {
            float r;
            float g;
            float b;
        };
    };
} vx_vertex_t;

typedef vx_vertex_t vx_vec3_t;
typedef vx_vertex_t vx_color_t;

typedef struct vx_mesh {
    vx_vertex_t* vertices;          // Contiguous mesh vertices
    vx_color_t* colors;             // Contiguous vertices colors
    vx_vec3_t* normals;             // Contiguous mesh normals
    unsigned int* indices;          // Mesh indices
    unsigned int* normalindices;    // Mesh normal indices
    size_t nindices;                // The number of normal indices
    size_t nvertices;               // The number of vertices
    size_t nnormals;                // The number of normals
} vx_mesh_t;

typedef struct vx_point_cloud {
    vx_vertex_t* vertices;          // Contiguous point cloud vertices positions, each vertex corresponds
                                    // to the center of a voxel
    vx_color_t* colors;             // Contiguous point cloud vertices colors
    size_t nvertices;               // The number of vertices in the point cloud
} vx_point_cloud_t;

// vx_voxelize_pc: Voxelizes a triangle mesh to a point cloud
vx_point_cloud_t* vx_voxelize_pc(vx_mesh_t const* mesh, // The input mesh
                                 float voxelsizex,      // Voxel size on X-axis
                                 float voxelsizey,      // Voxel size on Y-axis
                                 float voxelsizez,      // Voxel size on Z-axis
                                 float precision);      // A precision factor that reduces "holes artifact
                                                        // usually a precision = voxelsize / 10. works ok

// vx_voxelize: Voxelizes a triangle mesh to a triangle mesh representing cubes
vx_mesh_t* vx_voxelize(vx_mesh_t const* mesh,       // The input mesh
        float voxelsizex,                           // Voxel size on X-axis
        float voxelsizey,                           // Voxel size on Y-axis
        float voxelsizez,                           // Voxel size on Z-axis
        float precision);                           // A precision factor that reduces "holes" artifact
                                                    // usually a precision = voxelsize / 10. works ok.

// vx_voxelize_snap_3d_grid: Voxelizes a triangle mesh to a 3d texture
// The texture data is aligned as RGBA8 and can be uploaded as a 3d texture with OpenGL like so:
// glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, width, height, depth, 0, GL_RGBA, GL_UNSIGNED_BYTE, texturedata);
unsigned int* vx_voxelize_snap_3dgrid(vx_mesh_t const* mesh, // The input mesh
        unsigned int width,                                  // The texture resolution on x-axis
        unsigned int height,                                 // The texture resolution on y-axis
        unsigned int depth);                                 // The texture resolution on z-axis


// Allocates a mesh that can contain nvertices vertices, nindices indices
vx_mesh_t* vx_mesh_alloc(int nvertices, int nindices);

// Allocates a mesh that can contain nvertices vertices and colors, nindices indices
vx_mesh_t* vx_color_mesh_alloc(int nvertices, int nindices);

// Free a mesh allocated with vx_mesh_alloc, vx_color_mesh_alloc or after a call to vx_voxelize
void vx_mesh_free(vx_mesh_t* mesh);

// Free a point cloud allocated after a call of vx_voxelize_pc
void vx_point_cloud_free(vx_point_cloud_t* pointcloud);

// Voxelizer Helpers, define your own if needed
#ifndef VOXELIZER_HELPERS
#define VOXELIZER_HELPERS 1
#define VX_MIN(a, b) (a > b ? b : a)
#define VX_MAX(a, b) (a > b ? a : b)
#define VX_FINDMINMAX(x0, x1, x2, min, max) \
    min = max = x0;                         \
    if (x1 < min) min = x1;                 \
    if (x1 > max) max = x1;                 \
    if (x2 < min) min = x2;                 \
    if (x2 > max) max = x2;
#define VX_CLAMP(v, lo, hi) VX_MAX(lo, VX_MIN(hi, v))
#define VX_MALLOC(T, N) ((T*) malloc(N * sizeof(T)))
#define VX_FREE(T) free(T)
#define VX_CALLOC(T, N) ((T*) calloc(N * sizeof(T), 1))
#define VX_SWAP(T, A, B) { T tmp = B; B = A; A = tmp; }
#ifdef VOXELIZER_DEBUG
#define VX_ASSERT(STMT) if (!(STMT)) { *(int *)0 = 0; }
#else
#define VX_ASSERT(STMT)
#endif // VOXELIZER_DEBUG
#endif // VOXELIZER_HELPERS

//
// END VOXELIZER PUBLIC API
// ------------------------------------------------------------------------------------------------

#endif // VOXELIZER_H

#ifdef VOXELIZER_IMPLEMENTATION

#include <math.h>       // ceil, fabs & al.
#include <stdbool.h>    // hughh
#include <string.h>     // memcpy

#define VOXELIZER_NORMAL_INDICES_SIZE   (6)
#define VOXELIZER_INDICES_SIZE          (36)
#define VOXELIZER_HASH_TABLE_SIZE       (4096)

unsigned int vx_voxel_indices[VOXELIZER_INDICES_SIZE] = {
    0, 1, 2,
    0, 2, 3,
    3, 2, 6,
    3, 6, 7,
    0, 7, 4,
    0, 3, 7,
    4, 7, 5,
    7, 6, 5,
    0, 4, 5,
    0, 5, 1,
    1, 5, 6,
    1, 6, 2,
};

float vx_normals[18] = {
     0.0, -1.0,  0.0,
     0.0,  1.0,  0.0,
     1.0,  0.0,  0.0,
     0.0,  0.0,  1.0,
    -1.0,  0.0,  0.0,
     0.0,  0.0, -1.0,
};

unsigned int vx_normal_indices[VOXELIZER_NORMAL_INDICES_SIZE] = {
    3, 2, 1, 5, 4, 0,
};

typedef struct vx_aabb {
    vx_vertex_t min;
    vx_vertex_t max;
} vx_aabb_t;

typedef struct vx_edge {
    vx_vertex_t p1;
    vx_vertex_t p2;
} vx_edge_t;

typedef struct vx_triangle {
    union {
        vx_vertex_t vertices[3];
        struct {
            vx_vertex_t p1;
            vx_vertex_t p2;
            vx_vertex_t p3;
        };
    };
    vx_color_t colors[3];
} vx_triangle_t;

typedef struct vx_hash_table_node {
    struct vx_hash_table_node* next;
    struct vx_hash_table_node* prev;
    void* data;
} vx_hash_table_node_t;

typedef struct vx_hash_table {
    vx_hash_table_node_t** elements;
    size_t size;
} vx_hash_table_t;

// Fixed size per-voxel accumulation state. The integer lattice key (kx, ky, kz)
// identifies the cell exactly; two distinct cells are never merged.
typedef struct vx_voxel_data {
    vx_vec3_t position;     // Voxel center (kx*sx, ky*sy, kz*sz)
    vx_color_t color;       // Resolved on extraction from area/accum
    long long kx;
    long long ky;
    long long kz;
    double area;            // Total clipped surface area touching this cell
    double accum[3];        // Integral of the linear color fields over that area
} vx_voxel_data_t;

vx_hash_table_t* vx__hash_table_alloc(size_t size)
{
    vx_hash_table_t* table = VX_MALLOC(vx_hash_table_t, 1);
    table->size = size;
    table->elements = VX_CALLOC(vx_hash_table_node_t*, size);

    return table;
}

void vx__hash_table_free(vx_hash_table_t* table)
{
    for (size_t i = 0; i < table->size; ++i) {
        vx_hash_table_node_t* node = table->elements[i];

        while (node) {
            vx_hash_table_node_t* next = node->next;
            VX_FREE(node->data);
            VX_FREE(node);
            node = next;
        }
    }

    VX_FREE(table->elements);
    VX_FREE(table);
}

void vx__hash_table_insert(vx_hash_table_t* table,
    size_t hash,
    void* data)
{
    vx_hash_table_node_t* node = VX_MALLOC(vx_hash_table_node_t, 1);

    node->prev = NULL;
    node->next = table->elements[hash];
    node->data = data;

    if (table->elements[hash]) {
        table->elements[hash]->prev = node;
    }

    table->elements[hash] = node;
}

vx_voxel_data_t* vx__hash_table_find_voxel(vx_hash_table_t* table,
    size_t hash,
    long long kx,
    long long ky,
    long long kz)
{
    vx_hash_table_node_t* node = table->elements[hash];

    while (node) {
        vx_voxel_data_t* data = (vx_voxel_data_t*) node->data;

        if (data->kx == kx && data->ky == ky && data->kz == kz) {
            return data;
        }

        node = node->next;
    }

    return NULL;
}

void vx_mesh_free(vx_mesh_t* mesh)
{
    VX_FREE(mesh->vertices);
    mesh->vertices = NULL;
    mesh->nvertices = 0;
    VX_FREE(mesh->indices);
    mesh->indices = NULL;
    VX_FREE(mesh->normalindices);
    mesh->normalindices = NULL;
    mesh->nindices = 0;
    VX_FREE(mesh->normals);
    mesh->normals = NULL;
    VX_FREE(mesh->colors);
    mesh->colors = NULL;
    VX_FREE(mesh);
}

void vx_point_cloud_free(vx_point_cloud_t* pc)
{
    VX_FREE(pc->vertices);
    pc->vertices = NULL;
    VX_FREE(pc->colors);
    pc->colors = NULL;
    pc->nvertices = 0;
    VX_FREE(pc);
}

vx_mesh_t* vx_mesh_alloc(int nvertices, int nindices)
{
    vx_mesh_t* mesh = VX_MALLOC(vx_mesh_t, 1);
    mesh->indices = VX_CALLOC(unsigned int, nindices);
    mesh->normalindices = VX_CALLOC(unsigned int, nindices);
    mesh->vertices = VX_CALLOC(vx_vertex_t, nvertices);
    mesh->normals = VX_CALLOC(vx_vec3_t, nvertices);
    mesh->colors = VX_CALLOC(vx_color_t, nvertices);
    mesh->nindices = nindices;
    mesh->nnormals = nvertices;
    mesh->nvertices = nvertices;
    return mesh;
}

vx_mesh_t* vx_color_mesh_alloc(int nvertices, int nindices)
{
    vx_mesh_t* mesh = vx_mesh_alloc(nvertices, nindices);
    if (!mesh->colors) {
        mesh->colors = VX_CALLOC(vx_color_t, nvertices);
    }
    return mesh;
}

void vx__vec3_multiply(vx_vec3_t* a, float v)
{
    a->x *= v;
    a->y *= v;
    a->z *= v;
}

void vx__dcross(double out[3], const double a[3], const double b[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

// Closed triangle/box overlap test (Akenine-Moller separating axis test),
// evaluated in double precision. A triangle touching the box on a single
// edge or point is still considered overlapping.
int vx__triangle_box_overlap_d(const double center[3],
    const double half[3],
    const double tri[3][3])
{
    double v[3][3];
    double e[3][3];

    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            v[i][j] = tri[i][j] - center[j];
        }
    }

    for (int i = 0; i < 3; ++i) {
        int n = (i + 1) % 3;
        for (int j = 0; j < 3; ++j) {
            e[i][j] = v[n][j] - v[i][j];
        }
    }

    // 9 separating axes: cross product of each edge with each coordinate axis
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            double a[3];
            double mn, mx, rad, p;
            int j1 = (j + 1) % 3;
            int j2 = (j + 2) % 3;

            a[j] = 0.0;
            a[j1] = e[i][j2];
            a[j2] = -e[i][j1];

            mn = mx = a[0] * v[0][0] + a[1] * v[0][1] + a[2] * v[0][2];
            for (int k = 1; k < 3; ++k) {
                p = a[0] * v[k][0] + a[1] * v[k][1] + a[2] * v[k][2];
                if (p < mn) mn = p;
                if (p > mx) mx = p;
            }

            rad = half[0] * fabs(a[0]) + half[1] * fabs(a[1]) + half[2] * fabs(a[2]);

            if (mn > rad || mx < -rad) {
                return 0;
            }
        }
    }

    // 3 separating axes: the coordinate axes
    for (int j = 0; j < 3; ++j) {
        double mn, mx;
        VX_FINDMINMAX(v[0][j], v[1][j], v[2][j], mn, mx);
        if (mn > half[j] || mx < -half[j]) {
            return 0;
        }
    }

    // 1 separating axis: the triangle plane normal
    {
        double n[3];
        double vmin[3], vmax[3];
        double d, dmin, dmax;

        vx__dcross(n, e[0], e[1]);
        d = -(n[0] * v[0][0] + n[1] * v[0][1] + n[2] * v[0][2]);

        for (int j = 0; j < 3; ++j) {
            if (n[j] > 0.0) {
                vmin[j] = -half[j];
                vmax[j] = half[j];
            } else {
                vmin[j] = half[j];
                vmax[j] = -half[j];
            }
        }

        dmin = n[0] * vmin[0] + n[1] * vmin[1] + n[2] * vmin[2] + d;
        dmax = n[0] * vmax[0] + n[1] * vmax[1] + n[2] * vmax[2] + d;

        if (dmin > 0.0) {
            return 0;
        }

        if (dmax >= 0.0) {
            return 1;
        }
    }

    return 0;
}

// Clips a triangle against the closed box and accumulates the area of the
// intersection polygon plus the integral of the linear color field over it.
// Clipped vertices inherit the linearly interpolated colors of the original
// triangle. A degenerate (edge/point only) intersection contributes zero area.
void vx__clip_accumulate(const double tri[3][3],
    const double tcol[3][3],
    const double center[3],
    const double half[3],
    double* out_area,
    double out_accum[3])
{
    double poly[12][3];
    double col[12][3];
    double area = 0.0;
    double accum[3] = {0.0, 0.0, 0.0};
    int n = 3;

    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            poly[i][j] = tri[i][j];
            col[i][j] = tcol[i][j];
        }
    }

    for (int axis = 0; axis < 3 && n > 0; ++axis) {
        for (int side = 0; side < 2 && n > 0; ++side) {
            double bound = side == 0 ? center[axis] - half[axis]
                                     : center[axis] + half[axis];
            double npoly[12][3];
            double ncol[12][3];
            int m = 0;

            for (int i = 0; i < n; ++i) {
                int j = (i + 1) % n;
                double di = poly[i][axis];
                double dj = poly[j][axis];
                int ini = side == 0 ? di >= bound : di <= bound;
                int inj = side == 0 ? dj >= bound : dj <= bound;

                if (ini) {
                    for (int c = 0; c < 3; ++c) {
                        npoly[m][c] = poly[i][c];
                        ncol[m][c] = col[i][c];
                    }
                    m++;
                }

                if (ini != inj) {
                    double t = (bound - di) / (dj - di);
                    for (int c = 0; c < 3; ++c) {
                        npoly[m][c] = poly[i][c] + t * (poly[j][c] - poly[i][c]);
                        ncol[m][c] = col[i][c] + t * (col[j][c] - col[i][c]);
                    }
                    m++;
                }
            }

            n = m;
            if (n > 0) {
                memcpy(poly, npoly, (size_t) n * 3 * sizeof(double));
                memcpy(col, ncol, (size_t) n * 3 * sizeof(double));
            }
        }
    }

    for (int i = 1; i + 1 < n; ++i) {
        double u[3], w[3], cr[3];
        double a;

        for (int c = 0; c < 3; ++c) {
            u[c] = poly[i][c] - poly[0][c];
            w[c] = poly[i + 1][c] - poly[0][c];
        }

        vx__dcross(cr, u, w);
        a = 0.5 * sqrt(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]);

        if (a == 0.0) {
            continue;
        }

        area += a;
        for (int c = 0; c < 3; ++c) {
            accum[c] += a * (col[0][c] + col[i][c] + col[i + 1][c]) / 3.0;
        }
    }

    *out_area = area;
    out_accum[0] = accum[0];
    out_accum[1] = accum[1];
    out_accum[2] = accum[2];
}

void vx__aabb_init(vx_aabb_t* aabb)
{
    aabb->max.x = aabb->max.y = aabb->max.z = -INFINITY;
    aabb->min.x = aabb->min.y = aabb->min.z = INFINITY;
}

vx_aabb_t vx__triangle_aabb(vx_triangle_t* triangle)
{
    vx_aabb_t aabb;

    vx__aabb_init(&aabb);

    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            aabb.max.v[i] = VX_MAX(aabb.max.v[i], triangle->vertices[j].v[i]);
            aabb.min.v[i] = VX_MIN(aabb.min.v[i], triangle->vertices[j].v[i]);
        }
    }

    return aabb;
}

vx_aabb_t vx__aabb_merge(vx_aabb_t* a, vx_aabb_t* b)
{
    vx_aabb_t merge;

    merge.min.x = VX_MIN(a->min.x, b->min.x);
    merge.min.y = VX_MIN(a->min.y, b->min.y);
    merge.min.z = VX_MIN(a->min.z, b->min.z);

    merge.max.x = VX_MAX(a->max.x, b->max.x);
    merge.max.y = VX_MAX(a->max.y, b->max.y);
    merge.max.z = VX_MAX(a->max.z, b->max.z);

    return merge;
}

size_t vx__voxel_hash(long long kx,
    long long ky,
    long long kz,
    size_t n)
{
    unsigned long long a = (unsigned long long) kx * 73856093ull;
    unsigned long long b = (unsigned long long) ky * 19349663ull;
    unsigned long long c = (unsigned long long) kz * 83492791ull;

    return (size_t) ((a ^ b ^ c) % n);
}

void vx__add_voxel(vx_mesh_t* mesh,
    vx_vertex_t* pos,
    vx_color_t color,
    float* vertices)
{
    for (size_t i = 0; i < 8; ++i) {
        size_t index = i+mesh->nvertices;

        mesh->vertices[index].x = vertices[i*3+0] + pos->x;
        mesh->vertices[index].y = vertices[i*3+1] + pos->y;
        mesh->vertices[index].z = vertices[i*3+2] + pos->z;

        if (mesh->colors) {
            mesh->colors[index].r = color.r;
            mesh->colors[index].g = color.g;
            mesh->colors[index].b = color.b;
        }
    }

    int j = -1;
    for (size_t i = 0; i < VOXELIZER_INDICES_SIZE; ++i) {
        if (i % 6 == 0) {
            j++;
        }
        mesh->normalindices[i+mesh->nindices] = vx_normal_indices[j];
    }

    for (size_t i = 0; i < VOXELIZER_INDICES_SIZE; ++i) {
        mesh->indices[i+mesh->nindices] = vx_voxel_indices[i] + mesh->nvertices;
    }

    mesh->nindices += VOXELIZER_INDICES_SIZE;
    mesh->nvertices += 8;
}

vx_hash_table_t* vx__voxelize(vx_mesh_t const* m,
    vx_vertex_t vs,
    vx_vertex_t hvs,
    float precision,
    size_t* nvoxels)
{
    vx_hash_table_t* table = NULL;
    double half[3];
    double s[3];

    table = vx__hash_table_alloc(VOXELIZER_HASH_TABLE_SIZE);

    // Intersection box half widths: half a voxel extended by the precision
    // length on every axis
    half[0] = (double) hvs.x + (double) precision;
    half[1] = (double) hvs.y + (double) precision;
    half[2] = (double) hvs.z + (double) precision;

    s[0] = (double) vs.x;
    s[1] = (double) vs.y;
    s[2] = (double) vs.z;

    // Non-positive voxel sizes describe no valid lattice
    if (!(s[0] > 0.0) || !(s[1] > 0.0) || !(s[2] > 0.0)) {
        return table;
    }

    for (size_t i = 0; i < m->nindices; i += 3) {
        unsigned int i1, i2, i3;
        double tri[3][3];
        double tcol[3][3];
        double e1[3], e2[3], n[3];
        double tmin[3], tmax[3];
        long long kmin[3], kmax[3];

        VX_ASSERT(m->indices[i+0] < m->nvertices);
        VX_ASSERT(m->indices[i+1] < m->nvertices);
        VX_ASSERT(m->indices[i+2] < m->nvertices);

        i1 = m->indices[i+0];
        i2 = m->indices[i+1];
        i3 = m->indices[i+2];

        for (int j = 0; j < 3; ++j) {
            tri[0][j] = (double) m->vertices[i1].v[j];
            tri[1][j] = (double) m->vertices[i2].v[j];
            tri[2][j] = (double) m->vertices[i3].v[j];
        }

        if (m->colors) {
            for (int j = 0; j < 3; ++j) {
                tcol[0][j] = (double) m->colors[i1].v[j];
                tcol[1][j] = (double) m->colors[i2].v[j];
                tcol[2][j] = (double) m->colors[i3].v[j];
            }
        }

        // Skip truly degenerate (collinear or repeated vertices, zero area)
        // triangles; any positive area triangle is kept, however small
        for (int j = 0; j < 3; ++j) {
            e1[j] = tri[1][j] - tri[0][j];
            e2[j] = tri[2][j] - tri[0][j];
        }
        vx__dcross(n, e1, e2);

        if (n[0] == 0.0 && n[1] == 0.0 && n[2] == 0.0) {
            continue;
        }

        // Candidate cells: every lattice cell whose closed intersection box
        // may share a point with the triangle
        for (int j = 0; j < 3; ++j) {
            tmin[j] = VX_MIN(tri[0][j], VX_MIN(tri[1][j], tri[2][j]));
            tmax[j] = VX_MAX(tri[0][j], VX_MAX(tri[1][j], tri[2][j]));
            kmin[j] = (long long) floor((tmin[j] - half[j]) / s[j]);
            kmax[j] = (long long) ceil((tmax[j] + half[j]) / s[j]);
        }

        for (long long kx = kmin[0]; kx <= kmax[0]; ++kx) {
            for (long long ky = kmin[1]; ky <= kmax[1]; ++ky) {
                for (long long kz = kmin[2]; kz <= kmax[2]; ++kz) {
                    double center[3];
                    double area = 0.0;
                    double accum[3] = {0.0, 0.0, 0.0};
                    vx_voxel_data_t* nodedata;
                    size_t hash;

                    center[0] = (double) kx * s[0];
                    center[1] = (double) ky * s[1];
                    center[2] = (double) kz * s[2];

                    if (!vx__triangle_box_overlap_d(center, half, tri)) {
                        continue;
                    }

                    if (m->colors != NULL) {
                        vx__clip_accumulate(tri, tcol, center, half, &area, accum);
                    }

                    hash = vx__voxel_hash(kx, ky, kz, table->size);
                    nodedata = vx__hash_table_find_voxel(table, hash, kx, ky, kz);

                    if (nodedata) {
                        // Repeated key: accumulate into the existing cell
                        nodedata->area += area;
                        nodedata->accum[0] += accum[0];
                        nodedata->accum[1] += accum[1];
                        nodedata->accum[2] += accum[2];
                    } else {
                        nodedata = VX_MALLOC(vx_voxel_data_t, 1);

                        nodedata->kx = kx;
                        nodedata->ky = ky;
                        nodedata->kz = kz;

                        nodedata->position.x = (float) center[0];
                        nodedata->position.y = (float) center[1];
                        nodedata->position.z = (float) center[2];

                        nodedata->color.r = 0.0f;
                        nodedata->color.g = 0.0f;
                        nodedata->color.b = 0.0f;

                        nodedata->area = area;
                        nodedata->accum[0] = accum[0];
                        nodedata->accum[1] = accum[1];
                        nodedata->accum[2] = accum[2];

                        vx__hash_table_insert(table, hash, nodedata);
                        (*nvoxels)++;
                    }
                }
            }
        }
    }

    return table;
}

vx_color_t vx__voxel_final_color(vx_voxel_data_t* voxeldata)
{
    vx_color_t color;

    color.r = 0.0f;
    color.g = 0.0f;
    color.b = 0.0f;

    // Cells only touched on an edge or point have zero accumulated area and
    // resolve to black; no clamping is applied to out of [0, 1] colors
    if (voxeldata->area > 0.0) {
        color.r = (float) (voxeldata->accum[0] / voxeldata->area);
        color.g = (float) (voxeldata->accum[1] / voxeldata->area);
        color.b = (float) (voxeldata->accum[2] / voxeldata->area);
    }

    return color;
}

vx_mesh_t* vx_voxelize(vx_mesh_t const* m,
    float voxelsizex,
    float voxelsizey,
    float voxelsizez,
    float precision)
{
    vx_mesh_t* outmesh = NULL;
    vx_hash_table_t* table = NULL;
    size_t voxels = 0;

    vx_vertex_t vs = {{{voxelsizex, voxelsizey, voxelsizez}}};
    vx_vertex_t hvs = vs;

    vx__vec3_multiply(&hvs, 0.5f);

    table = vx__voxelize(m, vs, hvs, precision, &voxels);

    outmesh = VX_MALLOC(vx_mesh_t, 1);
    size_t nvertices = voxels * 8;
    size_t nindices = voxels * VOXELIZER_INDICES_SIZE;
    outmesh->nnormals = VOXELIZER_NORMAL_INDICES_SIZE;
    outmesh->vertices = VX_CALLOC(vx_vertex_t, nvertices);
    outmesh->normals = VX_CALLOC(vx_vec3_t, 6);
    outmesh->colors = m->colors != NULL ? VX_CALLOC(vx_color_t, nvertices) : NULL;
    outmesh->indices = VX_CALLOC(unsigned int, nindices);
    outmesh->normalindices = VX_CALLOC(unsigned int, nindices);
    outmesh->nindices = 0;
    outmesh->nvertices = 0;

    memcpy(outmesh->normals, vx_normals, 18 * sizeof(float));

    float vertices[24] = {
        -hvs.x,  hvs.y,  hvs.z,
        -hvs.x, -hvs.y,  hvs.z,
         hvs.x, -hvs.y,  hvs.z,
         hvs.x,  hvs.y,  hvs.z,
        -hvs.x,  hvs.y, -hvs.z,
        -hvs.x, -hvs.y, -hvs.z,
         hvs.x, -hvs.y, -hvs.z,
         hvs.x,  hvs.y, -hvs.z,
    };

    for (size_t i = 0; i < table->size; ++i) {
        if (table->elements[i] != NULL) {
            vx_hash_table_node_t* node = table->elements[i];
            vx_voxel_data_t* voxeldata;

            while (node) {
                voxeldata = (vx_voxel_data_t*) node->data;
                vx__add_voxel(outmesh, &voxeldata->position,
                        vx__voxel_final_color(voxeldata), vertices);
                node = node->next;
            }
        }
    }

    vx__hash_table_free(table);

    return outmesh;
}

vx_point_cloud_t* vx_voxelize_pc(vx_mesh_t const* mesh,
    float voxelsizex,
    float voxelsizey,
    float voxelsizez,
    float precision)
{
    vx_point_cloud_t* pc = NULL;
    vx_hash_table_t* table = NULL;
    size_t voxels = 0;

    vx_vec3_t vs = {{{voxelsizex, voxelsizey, voxelsizez}}};
    vx_vec3_t hvs = vs;

    vx__vec3_multiply(&hvs, 0.5f);

    table = vx__voxelize(mesh, vs, hvs, precision, &voxels);

    pc = VX_MALLOC(vx_point_cloud_t, 1);
    pc->vertices = VX_MALLOC(vx_vec3_t, voxels);
    pc->colors = mesh->colors != NULL ? VX_MALLOC(vx_color_t, voxels) : NULL;
    pc->nvertices = 0;

    for (size_t i = 0; i < table->size; ++i) {
        if (!table->elements[i]) { continue; }

        vx_hash_table_node_t* node = table->elements[i];
        vx_voxel_data_t* voxeldata;

        while (node) {
            voxeldata = (vx_voxel_data_t*) node->data;
            if (pc->colors) { pc->colors[pc->nvertices] = vx__voxel_final_color(voxeldata); }
            pc->vertices[pc->nvertices++] = voxeldata->position;

            node = node->next;
        }
    }

    vx__hash_table_free(table);
    return pc;
}

unsigned int vx__rgbaf32_to_abgr8888(float rgba[4])
{
    unsigned int color =
       (((unsigned int)(255.0f * rgba[3]) & 0xff) << 24) |
       (((unsigned int)(255.0f * rgba[2]) & 0xff) << 16) |
       (((unsigned int)(255.0f * rgba[1]) & 0xff) <<  8) |
       (((unsigned int)(255.0f * rgba[0]) & 0xff) <<  0);
    return color;
}

void vx__abgr8888_to_rgbaf32(unsigned int abgr8888,
    float (*rgbaf32)[4])
{
    (*rgbaf32)[0] = ((abgr8888 >>  0) & 0xff) / 255.0f;
    (*rgbaf32)[1] = ((abgr8888 >>  8) & 0xff) / 255.0f;
    (*rgbaf32)[2] = ((abgr8888 >> 16) & 0xff) / 255.0f;
    (*rgbaf32)[3] = ((abgr8888 >> 24) & 0xff) / 255.0f;
}

unsigned int vx__mix(unsigned int abgr88880,
    unsigned int abgr88881)
{
    float rgba0[4];
    float rgba1[4];
    float out[4];

    vx__abgr8888_to_rgbaf32(abgr88880, &rgba0);
    vx__abgr8888_to_rgbaf32(abgr88881, &rgba1);

    for (int i = 0; i < 4; ++i) {
        out[i] = rgba0[i] * 0.5f + rgba1[i] * 0.5f;
    }

    return vx__rgbaf32_to_abgr8888(out);
}

unsigned int* vx_voxelize_snap_3dgrid(vx_mesh_t const* m,
    unsigned int width,
    unsigned int height,
    unsigned int depth)
{
    vx_aabb_t* aabb = NULL;
    vx_aabb_t* meshaabb = NULL;
    float ax, ay, az;

    VX_ASSERT(m->colors);

    for (size_t i = 0; i < m->nindices; i += 3) {
        vx_triangle_t triangle;
        unsigned int i1, i2, i3;

        VX_ASSERT(m->indices[i+0] < m->nvertices);
        VX_ASSERT(m->indices[i+1] < m->nvertices);
        VX_ASSERT(m->indices[i+2] < m->nvertices);

        i1 = m->indices[i+0];
        i2 = m->indices[i+1];
        i3 = m->indices[i+2];

        triangle.p1 = m->vertices[i1];
        triangle.p2 = m->vertices[i2];
        triangle.p3 = m->vertices[i3];

        if (!meshaabb) {
            meshaabb = VX_MALLOC(vx_aabb_t, 1);
            *meshaabb = vx__triangle_aabb(&triangle);
        } else {
            vx_aabb_t naabb = vx__triangle_aabb(&triangle);
            *meshaabb = vx__aabb_merge(meshaabb, &naabb);
        }
    }

    float resx = (meshaabb->max.x - meshaabb->min.x) / width;
    float resy = (meshaabb->max.y - meshaabb->min.y) / height;
    float resz = (meshaabb->max.z - meshaabb->min.z) / depth;

    vx_point_cloud_t* pc = vx_voxelize_pc(m, resx, resy, resz, 0.0);

    aabb = VX_MALLOC(vx_aabb_t, 1);

    vx__aabb_init(aabb);

    for (size_t i = 0; i < pc->nvertices; i++) {
        for (size_t j = 0; j < 3; j++) {
            aabb->max.v[j] = VX_MAX(aabb->max.v[j], pc->vertices[i].v[j]);
            aabb->min.v[j] = VX_MIN(aabb->min.v[j], pc->vertices[i].v[j]);
        }
    }

    ax = aabb->max.x - aabb->min.x;
    ay = aabb->max.y - aabb->min.y;
    az = aabb->max.z - aabb->min.z;

    unsigned int* data = VX_CALLOC(unsigned int, width * height * depth);

    for (size_t i = 0; i < pc->nvertices; ++i) {
        float rgba[4] = {pc->colors[i].r, pc->colors[i].g, pc->colors[i].b, 1.0};
        unsigned int color;
        float ox, oy, oz;
        int ix, iy, iz;
        unsigned int index;

        ox = pc->vertices[i].x + fabs(aabb->min.x);
        oy = pc->vertices[i].y + fabs(aabb->min.y);
        oz = pc->vertices[i].z + fabs(aabb->min.z);

        VX_ASSERT(ox >= 0.f);
        VX_ASSERT(oy >= 0.f);
        VX_ASSERT(oz >= 0.f);

        ix = (ax == 0.0) ? 0 : (ox / ax) * (width - 1);
        iy = (ay == 0.0) ? 0 : (oy / ay) * (height - 1);
        iz = (az == 0.0) ? 0 : (oz / az) * (depth - 1);


        VX_ASSERT(ix >= 0);
        VX_ASSERT(iy >= 0);
        VX_ASSERT(iz >= 0);

        VX_ASSERT(ix + iy * width + iz * (width * height) < width * height * depth);

        color = vx__rgbaf32_to_abgr8888(rgba);
        index = ix + iy * width + iz * (width * height);

        if (data[index] != 0) {
            data[index] = vx__mix(color, data[index]);
        } else {
            data[index] = color;
        }
    }

    VX_FREE(aabb);
    VX_FREE(meshaabb);
    vx_point_cloud_free(pc);

    return data;
}

#undef VOXELIZER_INDICES_SIZE
#undef VOXELIZER_HASH_TABLE_SIZE

#endif // VX_VOXELIZER_IMPLEMENTATION
