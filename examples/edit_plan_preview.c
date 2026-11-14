/* edit_plan_preview -- sparse control-cage editing with cc_EditPlan.h
 *
 * This example builds a quad-grid control cage, creates a reusable edit
 * plan, and drags a single control point interactively-style: each drag
 * only re-evaluates the position rows that actually depend on the edited
 * vertex, instead of recomputing every subdivision level.
 *
 * Build (from the repository root):
 *     cc -std=c11 -O2 -I. examples/edit_plan_preview.c -lm -o edit_plan_preview
 */
#define CC_IMPLEMENTATION
#define CC_EDIT_IMPLEMENTATION
#include "cc_EditPlan.h" /* pulls in CatmullClark.h */

#include <stdio.h>
#include <stdlib.h>

static cc_Mesh *create_grid_cage(int32_t quadsPerSide, float spacing)
{
    const int32_t vertsPerSide = quadsPerSide + 1;
    const int32_t vertexCount = vertsPerSide * vertsPerSide;
    const int32_t faceCount = quadsPerSide * quadsPerSide;
    const int32_t halfedgeCount = 4 * faceCount;
    cc_Mesh *mesh = ccm_Create(vertexCount,
                               halfedgeCount,
                               halfedgeCount,
                               halfedgeCount, /* upper bound, fixed below */
                               faceCount);
    int32_t halfedgeID = 0;
    int32_t edgeID = 0;

    for (int32_t j = 0; j < vertsPerSide; ++j) {
        for (int32_t i = 0; i < vertsPerSide; ++i) {
            const int32_t vertexID = i + vertsPerSide * j;

            mesh->vertexPoints[vertexID].x = spacing * (float)i;
            mesh->vertexPoints[vertexID].y = spacing * (float)j;
            mesh->vertexPoints[vertexID].z = 0.0f;
            mesh->vertexToHalfedgeIDs[vertexID] = -1;
        }
    }
    for (int32_t j = 0; j < quadsPerSide; ++j) {
        for (int32_t i = 0; i < quadsPerSide; ++i) {
            const int32_t faceID = i + quadsPerSide * j;
            const int32_t v = i + vertsPerSide * j;

            mesh->faceToHalfedgeIDs[faceID] = halfedgeID;
            for (int32_t k = 0; k < 4; ++k, ++halfedgeID) {
                const int32_t corners[4] = {v, v + 1, v + 1 + vertsPerSide, v + vertsPerSide};
                cc_Halfedge *halfedge = &mesh->halfedges[halfedgeID];

                halfedge->vertexID = corners[k];
                halfedge->faceID = faceID;
                halfedge->nextID = mesh->faceToHalfedgeIDs[faceID] + (k + 1) % 4;
                halfedge->prevID = mesh->faceToHalfedgeIDs[faceID] + (k + 3) % 4;
                halfedge->twinID = -1;
                halfedge->edgeID = -1;
                halfedge->uvID = halfedgeID;
                mesh->vertexToHalfedgeIDs[corners[k]] = halfedgeID;
                mesh->uvs[halfedgeID].u = (float)(k == 1 || k == 2);
                mesh->uvs[halfedgeID].v = (float)(k >= 2);
            }
        }
    }
    for (halfedgeID = 0; halfedgeID < mesh->halfedgeCount; ++halfedgeID) {
        if (mesh->halfedges[halfedgeID].edgeID < 0) {
            const int32_t a = mesh->halfedges[halfedgeID].vertexID;
            const int32_t b = mesh->halfedges[mesh->halfedges[halfedgeID].nextID].vertexID;

            for (int32_t twinID = halfedgeID + 1; twinID < mesh->halfedgeCount; ++twinID) {
                if (mesh->halfedges[twinID].vertexID == b
                &&  mesh->halfedges[mesh->halfedges[twinID].nextID].vertexID == a) {
                    mesh->halfedges[halfedgeID].twinID = twinID;
                    mesh->halfedges[twinID].twinID = halfedgeID;
                    mesh->halfedges[halfedgeID].edgeID = edgeID;
                    mesh->halfedges[twinID].edgeID = edgeID;
                    break;
                }
            }
            if (mesh->halfedges[halfedgeID].edgeID < 0) {
                mesh->halfedges[halfedgeID].edgeID = edgeID; /* boundary */
            }
            mesh->edgeToHalfedgeIDs[edgeID] = halfedgeID;
            mesh->creases[edgeID].nextID = edgeID;
            mesh->creases[edgeID].prevID = edgeID;
            mesh->creases[edgeID].sharpness = 0.0f;
            ++edgeID;
        }
    }
    mesh->edgeCount = edgeID;

    return mesh;
}

int main(void)
{
    const int32_t quadsPerSide = 32;
    const int32_t vertsPerSide = quadsPerSide + 1;
    cc_Mesh *cage = create_grid_cage(quadsPerSide, 1.0f);
    cc_EditPlan *plan = ccs_EditPlanCreate(cage, 3);
    const int32_t dragID = 16 + 16 * vertsPerSide;

    if (!plan) {
        fprintf(stderr, "edit plan creation failed\n");
        return 1;
    }

    /* the source cage is no longer needed by the plan */
    ccm_Release(cage);

    /* simulate a few drag frames of one control point */
    for (int32_t frame = 0; frame < 5; ++frame) {
        const cc_VertexPoint position = {{16.0f, 16.0f, 0.25f * (float)(frame + 1)}};
        cc_EditStats stats;

        if (ccs_EditPlanUpdate(plan, &dragID, &position, 1, &stats) != 0) {
            fprintf(stderr, "edit plan update failed\n");
            return 1;
        }
        printf("frame %d:", (int)frame);
        for (int32_t depth = 0; depth < 3; ++depth) {
            printf("  [d%d] evaluated=%zu visited=%zu",
                   (int)(depth + 1),
                   stats.evaluated_rows[depth],
                   stats.visited_rows[depth]);
        }
        printf("\n");
    }

    /* read back the plan-owned subdivision result */
    {
        const cc_Subd *subd = ccs_EditPlanSubd(plan);
        const int32_t vertexCount = ccm_VertexCountAtDepth(subd->cage, 3);
        double checksum = 0.0;

        for (int32_t vertexID = 0; vertexID < vertexCount; ++vertexID) {
            const cc_VertexPoint point = ccs_VertexPoint(subd, vertexID, 3);

            checksum+= point.x + point.y + point.z;
        }
        printf("depth-3 checksum: %f (%d vertices)\n", checksum, (int)vertexCount);
    }

    ccs_EditPlanRelease(plan);

    return 0;
}
