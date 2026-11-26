/*
 * edit_plan_drag.c -- interactive-style sparse Catmull-Clark cage editing.
 *
 * Builds a small planar quad cage, subdivides it through an cc_EditPlan and
 * applies a few absolute control-point drags. Each drag updates only the
 * subdivision rows the change can reach; the printed cc_EditStats show the
 * sparse workload versus the size of the fully subdivided surface.
 *
 * This example has no external dependency beyond CatmullClark.h / cc_EditPlan.h.
 */
/*
 * Single-translation-unit build: defining CC_IMPLEMENTATION before the edit
 * plan header pulls in the upstream implementation exactly once.
 */
#define CC_IMPLEMENTATION
#define CC_EDIT_IMPLEMENTATION
#include "cc_EditPlan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Build an nx * ny quad grid in the z = 0 plane. */
static cc_Mesh *build_grid(int nx, int ny)
{
    const int vertexCount = (nx + 1) * (ny + 1);
    const int faceCount = nx * ny;
    const int halfedgeCount = 4 * faceCount;
    const int edgeCount = 2 * nx * ny + nx + ny;
    cc_Mesh *mesh = ccm_Create(vertexCount, halfedgeCount, halfedgeCount,
                               edgeCount, faceCount);
    if (mesh == NULL) {
        return NULL;
    }

    for (int y = 0; y <= ny; ++y) {
        for (int x = 0; x <= nx; ++x) {
            const int v = y * (nx + 1) + x;
            mesh->vertexPoints[v] = (cc_VertexPoint){(float)x, (float)y, 0.0f};
            mesh->vertexToHalfedgeIDs[v] = -1;
        }
    }

    for (int f = 0; f < faceCount; ++f) {
        const int x = f % nx;
        const int y = f / nx;
        const int base = 4 * f;
        const int loop[4] = {
            y * (nx + 1) + x,
            y * (nx + 1) + x + 1,
            (y + 1) * (nx + 1) + x + 1,
            (y + 1) * (nx + 1) + x
        };
        mesh->faceToHalfedgeIDs[f] = base;
        for (int j = 0; j < 4; ++j) {
            cc_Halfedge *he = &mesh->halfedges[base + j];
            he->vertexID = loop[j];
            he->faceID = f;
            he->nextID = base + (j + 1) % 4;
            he->prevID = base + (j + 3) % 4;
            he->twinID = -1;
            he->edgeID = -1;
            he->uvID = base + j;
            mesh->vertexToHalfedgeIDs[loop[j]] = base + j;
            mesh->uvs[base + j] = (cc_VertexUv){(float)(j & 1),
                                                (float)((j >> 1) & 1)};
        }
    }

    int edgeCountAssigned = 0;
    for (int h = 0; h < halfedgeCount; ++h) {
        if (mesh->halfedges[h].edgeID >= 0) {
            continue;
        }
        const int a = mesh->halfedges[h].vertexID;
        const int b =
            mesh->halfedges[mesh->halfedges[h].nextID].vertexID;
        for (int t = h + 1; t < halfedgeCount; ++t) {
            const int c = mesh->halfedges[t].vertexID;
            const int d =
                mesh->halfedges[mesh->halfedges[t].nextID].vertexID;
            if (c == b && d == a) {
                mesh->halfedges[h].twinID = t;
                mesh->halfedges[t].twinID = h;
                mesh->halfedges[h].edgeID = edgeCountAssigned;
                mesh->halfedges[t].edgeID = edgeCountAssigned;
                break;
            }
        }
        mesh->edgeToHalfedgeIDs[edgeCountAssigned] = h;
        mesh->creases[edgeCountAssigned].nextID = edgeCountAssigned;
        mesh->creases[edgeCountAssigned].prevID = edgeCountAssigned;
        mesh->creases[edgeCountAssigned].sharpness = 0.0f;
        ++edgeCountAssigned;
    }

    return mesh;
}

int main(void)
{
    const int depth = 3;
    cc_Mesh *cage = build_grid(16, 16);
    if (cage == NULL) {
        fprintf(stderr, "failed to build cage\n");
        return 1;
    }

    cc_EditPlan *plan = ccs_EditPlanCreate(cage, depth);
    if (plan == NULL) {
        fprintf(stderr, "failed to create edit plan\n");
        ccm_Release(cage);
        return 1;
    }

    /* The plan owns its own cage copy; the source can go away immediately. */
    const int totalRows = ccs_CumulativeVertexCountAtDepth(cage, depth);
    ccm_Release(cage);

    const cc_Subd *subd = ccs_EditPlanSubd(plan);
    printf("subdivided surface: %d position rows over %d levels\n",
           totalRows, depth);

    /* Drag a single interior control point along z. */
    const int32_t dragId = 8 * 17 + 8;
    cc_VertexPoint dragPos = subd->cage->vertexPoints[dragId];
    dragPos.z += 2.0f;

    cc_EditStats stats;
    if (ccs_EditPlanUpdate(plan, &dragId, &dragPos, 1, &stats) != 0) {
        fprintf(stderr, "edit plan update failed\n");
        ccs_EditPlanRelease(plan);
        return 1;
    }

    printf("single-point drag workload (rows evaluated at each output depth):\n");
    for (int d = 0; d < depth; ++d) {
        printf("  depth %d: evaluated=%4zu  visited=%4zu  (of %d rows)\n",
               d + 1, stats.evaluated_rows[d], stats.visited_rows[d],
               ccs_CumulativeVertexCountAtDepth(subd->cage, d + 1));
    }

    /* Repeating the same absolute position performs no geometry work. */
    memset(&stats, 0, sizeof(stats));
    ccs_EditPlanUpdate(plan, &dragId, &dragPos, 1, &stats);
    printf("identical re-application evaluated rows at depth 1: %zu\n",
           stats.evaluated_rows[0]);

    ccs_EditPlanRelease(plan);
    return 0;
}
