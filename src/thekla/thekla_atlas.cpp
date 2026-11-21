
#include "thekla_atlas.h"

#include <cfloat>
#include <cmath>

#include "nvmesh/halfedge/Edge.h"
#include "nvmesh/halfedge/Mesh.h"
#include "nvmesh/halfedge/Face.h"
#include "nvmesh/halfedge/Vertex.h"
#include "nvmesh/param/Atlas.h"
#include "nvmesh/param/InjectiveCharts.h"

#include "nvmath/Vector.inl"
#include "nvmath/ftoi.h"

#include "nvcore/Array.inl"


using namespace Thekla;
using namespace nv;


inline Atlas_Output_Mesh * set_error(Atlas_Error * error, Atlas_Error code) {
    if (error) *error = code;
    return NULL;
}



static void input_to_mesh(const Atlas_Input_Mesh * input, HalfEdge::Mesh * mesh, Atlas_Error * error) {

    Array<uint> canonicalMap;
    canonicalMap.reserve(input->vertex_count);

    for (int i = 0; i < input->vertex_count; i++) {
        const Atlas_Input_Vertex & input_vertex = input->vertex_array[i];
        const float * pos = input_vertex.position;
        const float * nor = input_vertex.normal;
        const float * tex = input_vertex.uv;

        HalfEdge::Vertex * vertex = mesh->addVertex(Vector3(pos[0], pos[1], pos[2]));
        vertex->nor.set(nor[0], nor[1], nor[2]);
        vertex->tex.set(tex[0], tex[1]);

        canonicalMap.append(input_vertex.first_colocal);
    }

    mesh->linkColocalsWithCanonicalMap(canonicalMap);


    const int face_count = input->face_count;

    int non_manifold_faces = 0;
    for (int i = 0; i < face_count; i++) {
        const Atlas_Input_Face & input_face = input->face_array[i];

        int v0 = input_face.vertex_index[0];
        int v1 = input_face.vertex_index[1];
        int v2 = input_face.vertex_index[2];

        HalfEdge::Face * face = mesh->addFace(v0, v1, v2);
        if (face != NULL) {
            face->material = input_face.material_index;
        }
        else {
            non_manifold_faces++;
        }
    }

    mesh->linkBoundary();

    if (non_manifold_faces != 0 && error != NULL) {
        *error = Atlas_Error_Invalid_Mesh_Non_Manifold;
    }
}

static Atlas_Output_Mesh * mesh_atlas_to_output(const HalfEdge::Mesh * mesh, const Atlas & atlas, Atlas_Error * error) {

    Atlas_Output_Mesh * output = new Atlas_Output_Mesh;

    const MeshCharts * charts = atlas.meshAt(0);

    // Allocate vertices.
    const int vertex_count = charts->vertexCount();
    output->vertex_count = vertex_count;
    output->vertex_array = new Atlas_Output_Vertex[vertex_count];

    int w = 0;
    int h = 0;

    // Output vertices.
    const int chart_count = charts->chartCount();
    for (int i = 0; i < chart_count; i++) {
        const Chart * chart = charts->chartAt(i);
        uint vertexOffset = charts->vertexCountBeforeChartAt(i);

        const uint chart_vertex_count = chart->vertexCount();
        for (uint v = 0; v < chart_vertex_count; v++) {
            Atlas_Output_Vertex & output_vertex = output->vertex_array[vertexOffset + v]; 

            uint original_vertex = chart->mapChartVertexToOriginalVertex(v);
            output_vertex.xref = original_vertex;

            Vector2 uv = chart->chartMesh()->vertexAt(v)->tex;
            output_vertex.uv[0] = uv.x;
            output_vertex.uv[1] = uv.y;
            w = max(w, ftoi_ceil(uv.x));
            h = max(h, ftoi_ceil(uv.y));
        }
    }

    const int face_count = mesh->faceCount();
    output->index_count = face_count * 3;
    output->index_array = new int[face_count * 3];

    // Set face indices.
    for (int f = 0; f < face_count; f++) {
        uint c = charts->faceChartAt(f);
        uint i = charts->faceIndexWithinChartAt(f);
        uint vertexOffset = charts->vertexCountBeforeChartAt(c);

        const Chart * chart = charts->chartAt(c);
        nvDebugCheck(chart->faceAt(i) == f);

        const HalfEdge::Face * face = chart->chartMesh()->faceAt(i);
        const HalfEdge::Edge * edge = face->edge;

        output->index_array[3*f+0] = vertexOffset + edge->vertex->id;
        output->index_array[3*f+1] = vertexOffset + edge->next->vertex->id;
        output->index_array[3*f+2] = vertexOffset + edge->next->next->vertex->id;
    }

    *error = Atlas_Error_Success;
    output->atlas_width = w;
    output->atlas_height = h;

    return output;
}


void Thekla::atlas_set_default_options(Atlas_Options * options) {
    if (options != NULL) {
        // These are the default values we use on The Witness.

        options->charter = Atlas_Charter_Default;
        options->charter_options.witness.proxy_fit_metric_weight = 2.0f;
        options->charter_options.witness.roundness_metric_weight = 0.01f;
        options->charter_options.witness.straightness_metric_weight = 6.0f;
        options->charter_options.witness.normal_seam_metric_weight = 4.0f;
        options->charter_options.witness.texture_seam_metric_weight = 0.5f;
        options->charter_options.witness.max_chart_area = FLT_MAX;
        options->charter_options.witness.max_boundary_length = FLT_MAX;

        options->mapper = Atlas_Mapper_Default;

        options->packer = Atlas_Packer_Default;
        options->packer_options.witness.packing_quality = 0;
        options->packer_options.witness.texels_per_unit = 8;
        options->packer_options.witness.block_align = true;
        options->packer_options.witness.conservative = false;
    }
}


Atlas_Output_Mesh * Thekla::atlas_generate(const Atlas_Input_Mesh * input, const Atlas_Options * options, Atlas_Error * error) {
    *error = Atlas_Error_Success;
    // Validate args.
    if (input == NULL || options == NULL || error == NULL) return set_error(error, Atlas_Error_Invalid_Args);

    // Validate options.
    if (options->charter != Atlas_Charter_Witness && options->charter != Atlas_Charter_Extract) {
        return set_error(error, Atlas_Error_Invalid_Options);
    }
    if (options->charter == Atlas_Charter_Witness) {
        // @@ Validate input options!
    }

    if (options->mapper != Atlas_Mapper_LSCM) {
        return set_error(error, Atlas_Error_Invalid_Options);
    }
    if (options->mapper == Atlas_Mapper_LSCM) {
        // No options.
    }

    if (options->packer != Atlas_Packer_Witness) {
        return set_error(error, Atlas_Error_Invalid_Options);
    }
    if (options->packer == Atlas_Packer_Witness) {
        // @@ Validate input options!
    }

    // Validate input mesh.
    for (int i = 0; i < input->face_count; i++) {
        int v0 = input->face_array[i].vertex_index[0];
        int v1 = input->face_array[i].vertex_index[1];
        int v2 = input->face_array[i].vertex_index[2];

        if (v0 < 0 || v0 >= input->vertex_count || 
            v1 < 0 || v1 >= input->vertex_count || 
            v2 < 0 || v2 >= input->vertex_count)
        {
            return set_error(error, Atlas_Error_Invalid_Mesh);
        }
    }


    // Build half edge mesh.
    AutoPtr<HalfEdge::Mesh> mesh(new HalfEdge::Mesh);

    input_to_mesh(input, mesh.ptr(), error);

    if (*error != Atlas_Error_Success) {
        return NULL;
    }

    Atlas atlas;

    // Charter.
    if (options->charter == Atlas_Charter_Extract) {
        // Let's only care about the chart topology if we have to run the LSCM mapper from scratch
        bool ensure_disk_charts = options->mapper_options.preserve_uvs == false || options->mapper_options.preserve_boundary == false;
        atlas.extractCharts(mesh.ptr(), ensure_disk_charts);
    }
    else if (options->charter == Atlas_Charter_Witness) {
        SegmentationSettings segmentation_settings;
        segmentation_settings.proxyFitMetricWeight = options->charter_options.witness.proxy_fit_metric_weight;
        segmentation_settings.roundnessMetricWeight = options->charter_options.witness.roundness_metric_weight;
        segmentation_settings.straightnessMetricWeight = options->charter_options.witness.straightness_metric_weight;
        segmentation_settings.normalSeamMetricWeight = options->charter_options.witness.normal_seam_metric_weight;
        segmentation_settings.textureSeamMetricWeight = options->charter_options.witness.texture_seam_metric_weight;
        segmentation_settings.maxChartArea = options->charter_options.witness.max_chart_area;
        segmentation_settings.maxBoundaryLength = options->charter_options.witness.max_boundary_length;

        Array<uint> uncharted_materials;
        atlas.computeCharts(mesh.ptr(), segmentation_settings, uncharted_materials);
    }

    // Mapper.

    if (options->mapper == Atlas_Mapper_LSCM) {
        atlas.parameterizeCharts(options->mapper_options.preserve_uvs, options->mapper_options.preserve_boundary);
    }

    // Packer.
    if (options->packer == Atlas_Packer_Witness) {
        int packing_quality = options->packer_options.witness.packing_quality;
        float texels_per_unit = options->packer_options.witness.texels_per_unit;
        bool block_align = options->packer_options.witness.block_align;
        bool conservative = options->packer_options.witness.conservative;

        /*float utilization =*/ atlas.packCharts(packing_quality, texels_per_unit, block_align, conservative);
    }


    // Build output mesh.
    return mesh_atlas_to_output(mesh.ptr(), atlas, error);
}


void Thekla::atlas_free(Atlas_Output_Mesh * output) {
    if (output != NULL) {
        delete [] output->vertex_array;
        delete [] output->index_array;
        delete output;
    }
}


static Atlas_Output_Mesh * injective_mesh_to_output(
    const HalfEdge::Mesh * mesh,
    const Atlas & atlas,
    Atlas_Error * error)
{
    Atlas_Output_Mesh * output = new Atlas_Output_Mesh;

    const MeshCharts * charts = atlas.meshAt(0);

    // Allocate vertices.
    const int vertex_count = charts->vertexCount();
    output->vertex_count = vertex_count;
    output->vertex_array = new Atlas_Output_Vertex[vertex_count];

    int w = 0;
    int h = 0;

    // Output vertices.
    const int chart_count = charts->chartCount();
    for (int i = 0; i < chart_count; i++) {
        const Chart * chart = charts->chartAt(i);
        uint vertexOffset = charts->vertexCountBeforeChartAt(i);

        const uint chart_vertex_count = chart->vertexCount();
        for (uint v = 0; v < chart_vertex_count; v++) {
            Atlas_Output_Vertex & output_vertex = output->vertex_array[vertexOffset + v];

            uint original_vertex = chart->mapChartVertexToOriginalVertex(v);
            output_vertex.xref = original_vertex;

            Vector2 uv = chart->chartMesh()->vertexAt(v)->tex;
            output_vertex.uv[0] = uv.x;
            output_vertex.uv[1] = uv.y;
            w = max(w, ftoi_ceil(uv.x));
            h = max(h, ftoi_ceil(uv.y));
        }
    }

    const int face_count = mesh->faceCount();
    output->index_count = face_count * 3;
    output->index_array = new int[face_count * 3];

    // Set face indices, restoring the original face and corner order.
    for (int f = 0; f < face_count; f++) {
        uint c = charts->faceChartAt(f);
        uint i = charts->faceIndexWithinChartAt(f);
        uint vertexOffset = charts->vertexCountBeforeChartAt(c);

        const Chart * chart = charts->chartAt(c);
        nvDebugCheck(chart->faceAt(i) == f);

        const HalfEdge::Face * face = chart->chartMesh()->faceAt(i);
        const HalfEdge::Edge * edge = face->edge;

        output->index_array[3*f+0] = vertexOffset + edge->vertex->id;
        output->index_array[3*f+1] = vertexOffset + edge->next->vertex->id;
        output->index_array[3*f+2] = vertexOffset + edge->next->next->vertex->id;
    }

    output->atlas_width = w;
    output->atlas_height = h;

    // Every output UV must be finite and inside the atlas rectangle.
    for (int i = 0; i < vertex_count; i++) {
        const float u = output->vertex_array[i].uv[0];
        const float v = output->vertex_array[i].uv[1];
        if (!isfinite(u) || !isfinite(v) || u < 0.0f || v < 0.0f ||
            u > float(w) || v > float(h))
        {
            atlas_free(output);
            *error = Atlas_Error_Uninjectable;
            return NULL;
        }
    }

    *error = Atlas_Error_Success;
    return output;
}

static int * copy_uint_array(const nv::Array<uint> & src)
{
    int * dst = new int[src.count()];
    for (uint i = 0; i < src.count(); i++) {
        dst[i] = int(src[i]);
    }
    return dst;
}

static Atlas_Injective_Report * copy_injective_report(const nv::InjectiveReport & source)
{
    Atlas_Injective_Report * report = new Atlas_Injective_Report;

    report->chart_count = int(source.chart_faces.count());
    report->node_count = int(source.node_faces.count());
    report->split_count = int(source.splits.count());

    report->face_chart = copy_uint_array(source.face_chart);

    report->chart_face_count = new int[report->chart_count];
    report->chart_face_array = new int * [report->chart_count];
    report->chart_parent_node = new int[report->chart_count];
    for (int c = 0; c < report->chart_count; c++) {
        report->chart_face_count[c] = int(source.chart_faces[c].count());
        report->chart_face_array[c] = copy_uint_array(source.chart_faces[c]);
        report->chart_parent_node[c] =
            (source.chart_parent_node[c] == ~0u) ? -1 : int(source.chart_parent_node[c]);
    }

    report->node_face_count = new int[report->node_count];
    report->node_face_array = new int * [report->node_count];
    report->node_parent = new int[report->node_count];
    for (int n = 0; n < report->node_count; n++) {
        report->node_face_count[n] = int(source.node_faces[n].count());
        report->node_face_array[n] = copy_uint_array(source.node_faces[n]);
        report->node_parent[n] =
            (source.node_parent[n] == ~0u) ? -1 : int(source.node_parent[n]);
    }

    report->split_array = new Atlas_Injective_Split[report->split_count > 0 ? report->split_count : 1];
    for (int s = 0; s < report->split_count; s++) {
        const nv::InjectiveSplitRecord & src = source.splits[s];
        Atlas_Injective_Split & dst = report->split_array[s];

        dst.parent_chart = int(src.parent_node);
        dst.child_chart[0] = int(src.child_node[0]);
        dst.child_chart[1] = int(src.child_node[1]);
        dst.witness_face[0] = int(src.witness_face[0]);
        dst.witness_face[1] = int(src.witness_face[1]);
        dst.cut_edge_face[0] = int(src.cut_edge[0]);
        dst.cut_edge_face[1] = int(src.cut_edge[1]);

        dst.parent_face_count = int(src.parent_faces.count());
        dst.parent_face_array = copy_uint_array(src.parent_faces);

        for (int g = 0; g < 2; g++) {
            dst.child_face_count[g] = int(src.child_faces[g].count());
            dst.child_face_array[g] = copy_uint_array(src.child_faces[g]);
        }
    }

    report->narrow_phase_count = (long long)source.narrow_phase_count;

    return report;
}

Atlas_Output_Mesh * Thekla::atlas_generate_injective(
    const Atlas_Input_Mesh * input,
    const Atlas_Options * options,
    int max_charts,
    Atlas_Injective_Report ** report_out,
    Atlas_Error * error)
{
    if (error == NULL) return NULL;
    *error = Atlas_Error_Success;

    // Validate args.
    if (input == NULL || options == NULL || report_out == NULL || max_charts <= 0) {
        return set_error(error, Atlas_Error_Invalid_Args);
    }
    *report_out = NULL;

    // This entry point only supports Extract + preserved UVs and boundaries.
    if (options->charter != Atlas_Charter_Extract ||
        options->mapper != Atlas_Mapper_LSCM ||
        options->packer != Atlas_Packer_Witness ||
        !options->mapper_options.preserve_uvs ||
        !options->mapper_options.preserve_boundary)
    {
        return set_error(error, Atlas_Error_Invalid_Options);
    }

    if (input->vertex_count <= 0 || input->face_count <= 0 ||
        input->vertex_array == NULL || input->face_array == NULL)
    {
        return set_error(error, Atlas_Error_Invalid_Mesh);
    }

    // Validate finite geometry, normals and UVs, and face index ranges.
    for (int i = 0; i < input->vertex_count; i++) {
        const Atlas_Input_Vertex & v = input->vertex_array[i];
        for (int k = 0; k < 3; k++) {
            if (!isfinite(v.position[k]) || !isfinite(v.normal[k])) {
                return set_error(error, Atlas_Error_Invalid_Mesh);
            }
        }
        if (!isfinite(v.uv[0]) || !isfinite(v.uv[1]) ||
            v.first_colocal < 0 || v.first_colocal >= input->vertex_count)
        {
            return set_error(error, Atlas_Error_Invalid_Mesh);
        }
    }
    for (int i = 0; i < input->face_count; i++) {
        const int v0 = input->face_array[i].vertex_index[0];
        const int v1 = input->face_array[i].vertex_index[1];
        const int v2 = input->face_array[i].vertex_index[2];
        if (v0 < 0 || v0 >= input->vertex_count ||
            v1 < 0 || v1 >= input->vertex_count ||
            v2 < 0 || v2 >= input->vertex_count ||
            v0 == v1 || v1 == v2 || v0 == v2)
        {
            return set_error(error, Atlas_Error_Invalid_Mesh);
        }
    }

    // Build the half edge mesh from an independent copy; the input is never modified.
    AutoPtr<HalfEdge::Mesh> mesh(new HalfEdge::Mesh);
    Atlas_Error build_error = Atlas_Error_Success;
    input_to_mesh(input, mesh.ptr(), &build_error);
    if (build_error != Atlas_Error_Success) {
        return set_error(error, build_error);
    }

    nv::InjectiveOptions injective_options;
    injective_options.packing_quality = options->packer_options.witness.packing_quality;
    injective_options.texels_per_unit = options->packer_options.witness.texels_per_unit;
    injective_options.block_align = options->packer_options.witness.block_align;

    nv::Atlas * atlas = NULL;
    nv::InjectiveReport report;

    const nv::InjectiveStatus status = nv::generateInjectiveAtlas(
        mesh.ptr(), (uint)max_charts, injective_options, atlas, report);

    if (status != nv::InjectiveStatus_Ok) {
        if (atlas != NULL) delete atlas;
        if (status == nv::InjectiveStatus_Budget_Exceeded) {
            return set_error(error, Atlas_Error_Budget_Exceeded);
        }
        if (status == nv::InjectiveStatus_Invalid_Mesh) {
            return set_error(error, Atlas_Error_Invalid_Mesh);
        }
        return set_error(error, Atlas_Error_Uninjectable);
    }

    AutoPtr<nv::Atlas> owned_atlas(atlas);

    Atlas_Output_Mesh * output = injective_mesh_to_output(mesh.ptr(), *atlas, error);
    if (output == NULL) {
        return NULL;
    }

    *report_out = copy_injective_report(report);
    return output;
}

void Thekla::atlas_injective_report_free(Atlas_Injective_Report * report)
{
    if (report == NULL) return;

    delete [] report->face_chart;

    if (report->chart_face_array != NULL) {
        for (int c = 0; c < report->chart_count; c++) {
            delete [] report->chart_face_array[c];
        }
    }
    delete [] report->chart_face_count;
    delete [] report->chart_face_array;
    delete [] report->chart_parent_node;

    if (report->node_face_array != NULL) {
        for (int n = 0; n < report->node_count; n++) {
            delete [] report->node_face_array[n];
        }
    }
    delete [] report->node_face_count;
    delete [] report->node_face_array;
    delete [] report->node_parent;

    if (report->split_array != NULL) {
        for (int s = 0; s < report->split_count; s++) {
            delete [] report->split_array[s].parent_face_array;
            delete [] report->split_array[s].child_face_array[0];
            delete [] report->split_array[s].child_face_array[1];
        }
    }
    delete [] report->split_array;

    delete report;
}


typedef void Atlas_Debug_Output(const char * output);

void Thekla::atlas_set_debug_output(Atlas_Debug_Output * output) {
    // @@ 
}
