// Regression tests for PackChartsPreservingUv: fixed UV meshes keep their input pixel
// coordinates on a given page while the remaining charts pack around them.
#include "source/xatlas/xatlas.h"
#include "source/xatlas/xatlas_c.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdint.h>
#include <vector>

namespace {

int g_checks = 0;
bool g_failed = false;

#define CHECK(cond) do { \
		++g_checks; \
		if (!(cond)) { \
			fprintf(stderr, "CHECK failed (%s:%d): %s\n", __FILE__, __LINE__, #cond); \
			g_failed = true; \
			return; \
		} \
	} while (0)

struct FVec2 {
	float x, y;
};

float crossz(FVec2 a, FVec2 b, FVec2 c)
{
	return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// Strict interior overlap test: clip triangle a by triangle b, check the remaining area.
bool trianglesOverlap(FVec2 a[3], FVec2 b[3])
{
	if (crossz(b[0], b[1], b[2]) < 0.0f) {
		FVec2 t = b[1];
		b[1] = b[2];
		b[2] = t;
	}
	FVec2 poly[8];
	int n = 3;
	for (int i = 0; i < 3; i++)
		poly[i] = a[i];
	for (int e = 0; e < 3 && n > 0; e++) {
		const FVec2 p0 = b[e], p1 = b[(e + 1) % 3];
		FVec2 out[8];
		int m = 0;
		for (int i = 0; i < n; i++) {
			const FVec2 s = poly[i], t = poly[(i + 1) % n];
			const float ds = crossz(p0, p1, s), dt = crossz(p0, p1, t);
			const bool ins = ds >= -1e-4f, int_ = dt >= -1e-4f;
			if (ins)
				out[m++] = s;
			if (ins != int_) {
				const float k = ds / (ds - dt);
				FVec2 r = { s.x + k * (t.x - s.x), s.y + k * (t.y - s.y) };
				out[m++] = r;
			}
		}
		n = m;
		for (int i = 0; i < n; i++)
			poly[i] = out[i];
	}
	float area = 0.0f;
	for (int i = 0; i < n; i++) {
		const FVec2 s = poly[i], t = poly[(i + 1) % n];
		area += s.x * t.y - s.y * t.x;
	}
	return fabsf(area) * 0.5f > 1e-3f;
}

bool pointInTriangle(float px, float py, const FVec2 t[3])
{
	FVec2 p = { px, py };
	const float d0 = crossz(t[0], t[1], p);
	const float d1 = crossz(t[1], t[2], p);
	const float d2 = crossz(t[2], t[0], p);
	const bool neg = d0 < -1e-4f || d1 < -1e-4f || d2 < -1e-4f;
	const bool pos = d0 > 1e-4f || d1 > 1e-4f || d2 > 1e-4f;
	return !(neg && pos);
}

struct UvMeshData {
	std::vector<float> uv; // vertexCount * 2
	std::vector<uint32_t> idx;
};

UvMeshData makeQuad(float x0, float y0, float x1, float y1)
{
	UvMeshData m;
	m.uv = { x0, y0, x1, y0, x1, y1, x0, y1 };
	m.idx = { 0, 1, 2, 0, 2, 3 };
	return m;
}

UvMeshData makeTriangle(float x0, float y0, float x1, float y1, float x2, float y2)
{
	UvMeshData m;
	m.uv = { x0, y0, x1, y1, x2, y2 };
	m.idx = { 0, 1, 2 };
	return m;
}

// A square frame filling [outer0, outer1]^2 except the inner hole (inner0, inner1)^2.
UvMeshData makeRing(float outer0, float outer1, float inner0, float inner1)
{
	UvMeshData m;
	m.uv = {
		outer0, outer0, outer1, outer0, outer1, outer1, outer0, outer1, // 0..3 outer
		inner0, inner0, inner1, inner0, inner1, inner1, inner0, inner1 // 4..7 inner
	};
	m.idx = {
		0, 1, 5, 0, 5, 4, // bottom
		1, 2, 6, 1, 6, 5, // right
		2, 3, 7, 2, 7, 6, // top
		3, 0, 4, 3, 4, 7 // left
	};
	return m;
}

void addUvMeshChecked(xatlas::Atlas *atlas, const UvMeshData &mesh)
{
	xatlas::UvMeshDecl decl;
	decl.vertexUvData = mesh.uv.data();
	decl.vertexCount = (uint32_t)mesh.uv.size() / 2;
	decl.vertexStride = sizeof(float) * 2;
	decl.indexData = mesh.idx.data();
	decl.indexCount = (uint32_t)mesh.idx.size();
	decl.indexFormat = xatlas::IndexFormat::UInt32;
	CHECK(xatlas::AddUvMesh(atlas, decl) == xatlas::AddMeshError::Success);
}

xatlas::PackOptions makePackOptions(uint32_t resolution, float texelsPerUnit, uint32_t padding, bool bilinear)
{
	xatlas::PackOptions options;
	options.resolution = resolution;
	options.texelsPerUnit = texelsPerUnit;
	options.padding = padding;
	options.bilinear = bilinear;
	options.createImage = true;
	options.rotateCharts = false;
	options.rotateChartsToAxis = false;
	options.bruteForce = true;
	return options;
}

uint32_t globalChartIndex(const xatlas::Atlas *atlas, uint32_t mesh, uint32_t chart)
{
	uint32_t index = chart;
	for (uint32_t m = 0; m < mesh; m++)
		index += atlas->meshes[m].chartCount;
	return index;
}

void getOutputTriangle(const xatlas::Atlas *atlas, uint32_t mesh, uint32_t face, FVec2 tri[3])
{
	const xatlas::Mesh &m = atlas->meshes[mesh];
	for (uint32_t j = 0; j < 3; j++) {
		const xatlas::Vertex &v = m.vertexArray[m.indexArray[face * 3 + j]];
		tri[j].x = v.uv[0];
		tri[j].y = v.uv[1];
	}
}

// Every vertex of a fixed mesh must keep its exact input UV (matched through xref) on the given page.
void checkFixedMeshPreserved(const xatlas::Atlas *atlas, uint32_t meshIndex, const UvMeshData &input, uint32_t page)
{
	CHECK(meshIndex < atlas->meshCount);
	const xatlas::Mesh &mesh = atlas->meshes[meshIndex];
	CHECK(mesh.vertexCount == (uint32_t)input.uv.size() / 2);
	CHECK(mesh.indexCount == (uint32_t)input.idx.size());
	for (uint32_t i = 0; i < mesh.indexCount; i++)
		CHECK(mesh.indexArray[i] == input.idx[i]);
	for (uint32_t v = 0; v < mesh.vertexCount; v++) {
		const xatlas::Vertex &vertex = mesh.vertexArray[v];
		CHECK(vertex.xref == v);
		CHECK(vertex.uv[0] == input.uv[vertex.xref * 2 + 0]);
		CHECK(vertex.uv[1] == input.uv[vertex.xref * 2 + 1]);
		CHECK(vertex.atlasIndex == (int32_t)page);
		CHECK(vertex.chartIndex >= 0);
	}
	for (uint32_t c = 0; c < mesh.chartCount; c++)
		CHECK(mesh.chartArray[c].atlasIndex == page);
	// Per-face check: input face geometry (through chart faceArray and xref) matches the output.
	for (uint32_t c = 0; c < mesh.chartCount; c++) {
		const xatlas::Chart &chart = mesh.chartArray[c];
		for (uint32_t f = 0; f < chart.faceCount; f++) {
			const uint32_t face = chart.faceArray[f];
			CHECK(face * 3 + 2 < input.idx.size());
			for (uint32_t j = 0; j < 3; j++) {
				const uint32_t inputVertex = input.idx[face * 3 + j];
				const xatlas::Vertex &vertex = mesh.vertexArray[mesh.indexArray[face * 3 + j]];
				CHECK(vertex.xref == inputVertex);
				CHECK(vertex.uv[0] == input.uv[inputVertex * 2 + 0]);
				CHECK(vertex.uv[1] == input.uv[inputVertex * 2 + 1]);
			}
		}
	}
}

// Independent geometric check: triangles of different charts on the same page must not overlap.
void checkNoChartOverlap(const xatlas::Atlas *atlas)
{
	struct Tri { uint32_t page, chart; FVec2 v[3]; };
	std::vector<Tri> tris;
	for (uint32_t m = 0; m < atlas->meshCount; m++) {
		const xatlas::Mesh &mesh = atlas->meshes[m];
		for (uint32_t c = 0; c < mesh.chartCount; c++) {
			const xatlas::Chart &chart = mesh.chartArray[c];
			for (uint32_t f = 0; f < chart.faceCount; f++) {
				Tri tri;
				tri.page = chart.atlasIndex;
				tri.chart = globalChartIndex(atlas, m, c);
				getOutputTriangle(atlas, m, chart.faceArray[f], tri.v);
				tris.push_back(tri);
			}
		}
	}
	for (size_t i = 0; i < tris.size(); i++) {
		for (size_t j = i + 1; j < tris.size(); j++) {
			if (tris[i].page != tris[j].page || tris[i].chart == tris[j].chart)
				continue;
			FVec2 a[3] = { tris[i].v[0], tris[i].v[1], tris[i].v[2] };
			FVec2 b[3] = { tris[j].v[0], tris[j].v[1], tris[j].v[2] };
			CHECK(!trianglesOverlap(a, b));
		}
	}
}

// Independent pixel ownership check: pixels covered by a chart's output triangles must belong to
// that chart in Atlas.image, and no two charts may cover the same pixel.
void checkPixelOwnership(const xatlas::Atlas *atlas)
{
	CHECK(atlas->image != nullptr);
	const uint32_t w = atlas->width, h = atlas->height;
	CHECK(w > 0 && h > 0);
	std::vector<int> owner((size_t)atlas->atlasCount * w * h, -1);
	for (uint32_t m = 0; m < atlas->meshCount; m++) {
		const xatlas::Mesh &mesh = atlas->meshes[m];
		for (uint32_t c = 0; c < mesh.chartCount; c++) {
			const xatlas::Chart &chart = mesh.chartArray[c];
			const uint32_t page = chart.atlasIndex;
			CHECK(page < atlas->atlasCount);
			const uint32_t globalChart = globalChartIndex(atlas, m, c);
			for (uint32_t f = 0; f < chart.faceCount; f++) {
				FVec2 tri[3];
				getOutputTriangle(atlas, m, chart.faceArray[f], tri);
				const float minX = fminf(tri[0].x, fminf(tri[1].x, tri[2].x));
				const float minY = fminf(tri[0].y, fminf(tri[1].y, tri[2].y));
				const float maxX = fmaxf(tri[0].x, fmaxf(tri[1].x, tri[2].x));
				const float maxY = fmaxf(tri[0].y, fmaxf(tri[1].y, tri[2].y));
				const int x0 = (int)floorf(minX), x1 = (int)ceilf(maxX);
				const int y0 = (int)floorf(minY), y1 = (int)ceilf(maxY);
				for (int y = y0; y <= y1; y++) {
					for (int x = x0; x <= x1; x++) {
						if (!pointInTriangle(x + 0.5f, y + 0.5f, tri))
							continue;
						CHECK(x >= 0 && x < (int)w && y >= 0 && y < (int)h);
						const size_t pixel = (size_t)page * w * h + (size_t)y * w + (size_t)x;
						const uint32_t data = atlas->image[pixel];
						CHECK((data & xatlas::kImageHasChartIndexBit) != 0);
						CHECK((data & xatlas::kImageChartIndexMask) == globalChart);
						CHECK((data & (xatlas::kImageIsBilinearBit | xatlas::kImageIsPaddingBit)) == 0);
						CHECK(owner[pixel] == -1 || owner[pixel] == (int)globalChart);
						owner[pixel] = (int)globalChart;
					}
				}
			}
		}
	}
}

// Deep copy of the public Atlas output, used to verify that a failed call preserves everything.
struct Snapshot {
	uint32_t meshCount = 0, chartCount = 0, atlasCount = 0, width = 0, height = 0;
	float texelsPerUnit = 0.0f;
	std::vector<float> utilization;
	std::vector<uint32_t> image;
	struct MeshData {
		std::vector<xatlas::Vertex> vertices;
		std::vector<uint32_t> indices;
		std::vector<xatlas::Chart> charts;
		std::vector<std::vector<uint32_t>> faces;
	};
	std::vector<MeshData> meshes;

	void capture(const xatlas::Atlas *atlas)
	{
		meshCount = atlas->meshCount;
		chartCount = atlas->chartCount;
		atlasCount = atlas->atlasCount;
		width = atlas->width;
		height = atlas->height;
		texelsPerUnit = atlas->texelsPerUnit;
		if (atlas->utilization)
			utilization.assign(atlas->utilization, atlas->utilization + atlas->atlasCount);
		if (atlas->image)
			image.assign(atlas->image, atlas->image + (size_t)atlas->atlasCount * width * height);
		meshes.resize(atlas->meshCount);
		for (uint32_t m = 0; m < atlas->meshCount; m++) {
			const xatlas::Mesh &mesh = atlas->meshes[m];
			MeshData &data = meshes[m];
			data.vertices.assign(mesh.vertexArray, mesh.vertexArray + mesh.vertexCount);
			data.indices.assign(mesh.indexArray, mesh.indexArray + mesh.indexCount);
			data.charts.assign(mesh.chartArray, mesh.chartArray + mesh.chartCount);
			data.faces.resize(mesh.chartCount);
			for (uint32_t c = 0; c < mesh.chartCount; c++)
				data.faces[c].assign(mesh.chartArray[c].faceArray, mesh.chartArray[c].faceArray + mesh.chartArray[c].faceCount);
		}
	}

	void verify(const xatlas::Atlas *atlas) const
	{
		CHECK(atlas->meshCount == meshCount);
		CHECK(atlas->chartCount == chartCount);
		CHECK(atlas->atlasCount == atlasCount);
		CHECK(atlas->width == width);
		CHECK(atlas->height == height);
		CHECK(atlas->texelsPerUnit == texelsPerUnit);
		if (!utilization.empty()) {
			CHECK(atlas->utilization != nullptr);
			for (size_t i = 0; i < utilization.size(); i++)
				CHECK(atlas->utilization[i] == utilization[i]);
		}
		if (!image.empty()) {
			CHECK(atlas->image != nullptr);
			for (size_t i = 0; i < image.size(); i++)
				CHECK(atlas->image[i] == image[i]);
		}
		for (uint32_t m = 0; m < meshCount; m++) {
			const xatlas::Mesh &mesh = atlas->meshes[m];
			const MeshData &data = meshes[m];
			CHECK(mesh.vertexCount == (uint32_t)data.vertices.size());
			CHECK(mesh.indexCount == (uint32_t)data.indices.size());
			CHECK(mesh.chartCount == (uint32_t)data.charts.size());
			for (uint32_t v = 0; v < mesh.vertexCount; v++) {
				CHECK(mesh.vertexArray[v].atlasIndex == data.vertices[v].atlasIndex);
				CHECK(mesh.vertexArray[v].chartIndex == data.vertices[v].chartIndex);
				CHECK(mesh.vertexArray[v].uv[0] == data.vertices[v].uv[0]);
				CHECK(mesh.vertexArray[v].uv[1] == data.vertices[v].uv[1]);
				CHECK(mesh.vertexArray[v].xref == data.vertices[v].xref);
			}
			for (uint32_t i = 0; i < mesh.indexCount; i++)
				CHECK(mesh.indexArray[i] == data.indices[i]);
			for (uint32_t c = 0; c < mesh.chartCount; c++) {
				CHECK(mesh.chartArray[c].atlasIndex == data.charts[c].atlasIndex);
				CHECK(mesh.chartArray[c].faceCount == data.charts[c].faceCount);
				CHECK(mesh.chartArray[c].material == data.charts[c].material);
				for (uint32_t f = 0; f < mesh.chartArray[c].faceCount; f++)
					CHECK(mesh.chartArray[c].faceArray[f] == data.faces[c][f]);
			}
		}
	}
};

void destroyAtlas(xatlas::Atlas *atlas)
{
	if (!g_failed)
		xatlas::Destroy(atlas);
}

} // namespace

static void testFixedBasic()
{
	// Fractional input UVs stay bit-exact on the requested page, free chart packs around them.
	UvMeshData fixed = makeQuad(10.25f, 8.25f, 14.25f, 12.25f);
	UvMeshData freeQuad = makeQuad(0.0f, 0.0f, 4.0f, 4.0f);
	xatlas::Atlas *atlas = xatlas::Create();
	addUvMeshChecked(atlas, fixed);
	addUvMeshChecked(atlas, freeQuad);
	xatlas::ComputeCharts(atlas);
	xatlas::PackOptions options = makePackOptions(64, 1.0f, 1, true);
	xatlas::FixedUvMesh fixedMesh = { 0, 0 };
	CHECK(xatlas::PackChartsPreservingUv(atlas, &fixedMesh, 1, options));
	CHECK(atlas->atlasCount == 1);
	CHECK(atlas->meshCount == 2);
	CHECK(atlas->chartCount == 2);
	CHECK(atlas->width == 64 && atlas->height == 64);
	checkFixedMeshPreserved(atlas, 0, fixed, 0);
	// Free chart is on the same page, inside the page, and doesn't touch the fixed chart.
	const xatlas::Mesh &freeMesh = atlas->meshes[1];
	for (uint32_t v = 0; v < freeMesh.vertexCount; v++) {
		CHECK(freeMesh.vertexArray[v].atlasIndex == 0);
		CHECK(freeMesh.vertexArray[v].uv[0] >= 0.0f && freeMesh.vertexArray[v].uv[0] <= 64.0f);
		CHECK(freeMesh.vertexArray[v].uv[1] >= 0.0f && freeMesh.vertexArray[v].uv[1] <= 64.0f);
	}
	checkNoChartOverlap(atlas);
	checkPixelOwnership(atlas);
	CHECK(atlas->utilization[0] > 0.0f);
	destroyAtlas(atlas);
}

static void testFixedPaddingBilinear()
{
	// Padding and bilinear toggles don't affect fixed UV preservation.
	const uint32_t paddings[] = { 0, 0, 1, 2, 3 };
	const bool bilinears[] = { false, true, true, true, false };
	for (int i = 0; i < 5; i++) {
		UvMeshData fixed = makeQuad(8.25f, 10.5f, 24.75f, 30.25f);
		UvMeshData freeQuad = makeQuad(0.0f, 0.0f, 4.0f, 4.0f);
		xatlas::Atlas *atlas = xatlas::Create();
		addUvMeshChecked(atlas, fixed);
		addUvMeshChecked(atlas, freeQuad);
		xatlas::ComputeCharts(atlas);
		xatlas::PackOptions options = makePackOptions(64, 1.0f, paddings[i], bilinears[i]);
		xatlas::FixedUvMesh fixedMesh = { 0, 0 };
		CHECK(xatlas::PackChartsPreservingUv(atlas, &fixedMesh, 1, options));
		checkFixedMeshPreserved(atlas, 0, fixed, 0);
		checkNoChartOverlap(atlas);
		checkPixelOwnership(atlas);
		destroyAtlas(atlas);
		if (g_failed)
			return;
	}
}

static void testFixedPageHoles()
{
	// A fixed page of 2 creates pages 0 and 1 as empty holes.
	UvMeshData fixed = makeQuad(4.5f, 4.5f, 12.5f, 12.5f);
	UvMeshData freeQuad = makeQuad(0.0f, 0.0f, 4.0f, 4.0f);
	xatlas::Atlas *atlas = xatlas::Create();
	addUvMeshChecked(atlas, fixed);
	addUvMeshChecked(atlas, freeQuad);
	xatlas::ComputeCharts(atlas);
	xatlas::PackOptions options = makePackOptions(64, 1.0f, 1, true);
	xatlas::FixedUvMesh fixedMesh = { 0, 2 };
	CHECK(xatlas::PackChartsPreservingUv(atlas, &fixedMesh, 1, options));
	CHECK(atlas->atlasCount == 3);
	checkFixedMeshPreserved(atlas, 0, fixed, 2);
	CHECK(atlas->utilization[0] > 0.0f); // free quad
	CHECK(atlas->utilization[1] == 0.0f); // empty hole page
	CHECK(atlas->utilization[2] > 0.0f); // fixed quad
	for (uint32_t v = 0; v < atlas->meshes[1].vertexCount; v++)
		CHECK(atlas->meshes[1].vertexArray[v].atlasIndex == 0);
	checkNoChartOverlap(atlas);
	checkPixelOwnership(atlas);
	destroyAtlas(atlas);
}

static void testFixedBoundingBoxOverlap()
{
	// Two fixed islands on the same page: bounding boxes intersect, real pixels don't.
	UvMeshData tri1 = makeTriangle(2.0f, 2.0f, 30.0f, 2.0f, 2.0f, 30.0f);
	UvMeshData tri2 = makeTriangle(40.0f, 40.0f, 8.0f, 40.0f, 40.0f, 8.0f);
	xatlas::Atlas *atlas = xatlas::Create();
	addUvMeshChecked(atlas, tri1);
	addUvMeshChecked(atlas, tri2);
	xatlas::ComputeCharts(atlas);
	xatlas::PackOptions options = makePackOptions(64, 1.0f, 1, true);
	xatlas::FixedUvMesh fixedMeshes[2] = { { 0, 0 }, { 1, 0 } };
	CHECK(xatlas::PackChartsPreservingUv(atlas, fixedMeshes, 2, options));
	CHECK(atlas->atlasCount == 1);
	checkFixedMeshPreserved(atlas, 0, tri1, 0);
	checkFixedMeshPreserved(atlas, 1, tri2, 0);
	checkNoChartOverlap(atlas);
	checkPixelOwnership(atlas);
	destroyAtlas(atlas);
}

static void testFixedFailures()
{
	// Invalid mesh index.
	{
		UvMeshData quad = makeQuad(8.0f, 8.0f, 16.0f, 16.0f);
		xatlas::Atlas *atlas = xatlas::Create();
		addUvMeshChecked(atlas, quad);
		xatlas::ComputeCharts(atlas);
		xatlas::PackOptions options = makePackOptions(64, 1.0f, 1, true);
		xatlas::FixedUvMesh fixedMesh = { 3, 0 };
		CHECK(!xatlas::PackChartsPreservingUv(atlas, &fixedMesh, 1, options));
		CHECK(atlas->meshCount == 0); // Never packed: no output at all.
		destroyAtlas(atlas);
		if (g_failed)
			return;
	}
	// Duplicate selection.
	{
		UvMeshData quad = makeQuad(8.0f, 8.0f, 16.0f, 16.0f);
		xatlas::Atlas *atlas = xatlas::Create();
		addUvMeshChecked(atlas, quad);
		xatlas::ComputeCharts(atlas);
		xatlas::PackOptions options = makePackOptions(64, 1.0f, 1, true);
		xatlas::FixedUvMesh fixedMeshes[2] = { { 0, 0 }, { 0, 1 } };
		CHECK(!xatlas::PackChartsPreservingUv(atlas, fixedMeshes, 2, options));
		destroyAtlas(atlas);
		if (g_failed)
			return;
	}
	// Non-finite fixed UV.
	{
		UvMeshData quad = makeQuad(10.0f, 10.0f, 20.0f, 20.0f);
		quad.uv[2] = INFINITY;
		xatlas::Atlas *atlas = xatlas::Create();
		addUvMeshChecked(atlas, quad);
		xatlas::ComputeCharts(atlas);
		xatlas::PackOptions options = makePackOptions(64, 1.0f, 1, true);
		xatlas::FixedUvMesh fixedMesh = { 0, 0 };
		CHECK(!xatlas::PackChartsPreservingUv(atlas, &fixedMesh, 1, options));
		destroyAtlas(atlas);
		if (g_failed)
			return;
	}
	// Fixed chart too close to the page edge: padding expansion leaves the page.
	{
		UvMeshData quad = makeQuad(62.0f, 62.0f, 64.0f, 64.0f);
		xatlas::Atlas *atlas = xatlas::Create();
		addUvMeshChecked(atlas, quad);
		xatlas::ComputeCharts(atlas);
		xatlas::PackOptions options = makePackOptions(64, 1.0f, 2, true);
		xatlas::FixedUvMesh fixedMesh = { 0, 0 };
		CHECK(!xatlas::PackChartsPreservingUv(atlas, &fixedMesh, 1, options));
		destroyAtlas(atlas);
		if (g_failed)
			return;
	}
	// The same chart fits with no padding and no bilinear expansion.
	{
		UvMeshData quad = makeQuad(62.0f, 62.0f, 63.5f, 63.5f);
		xatlas::Atlas *atlas = xatlas::Create();
		addUvMeshChecked(atlas, quad);
		xatlas::ComputeCharts(atlas);
		xatlas::PackOptions options = makePackOptions(64, 1.0f, 0, false);
		xatlas::FixedUvMesh fixedMesh = { 0, 0 };
		CHECK(xatlas::PackChartsPreservingUv(atlas, &fixedMesh, 1, options));
		checkFixedMeshPreserved(atlas, 0, quad, 0);
		destroyAtlas(atlas);
		if (g_failed)
			return;
	}
	// Two fixed charts covering the same pixels on the same page.
	{
		UvMeshData quad1 = makeQuad(8.0f, 8.0f, 24.0f, 24.0f);
		UvMeshData quad2 = makeQuad(16.0f, 16.0f, 32.0f, 32.0f);
		xatlas::Atlas *atlas = xatlas::Create();
		addUvMeshChecked(atlas, quad1);
		addUvMeshChecked(atlas, quad2);
		xatlas::ComputeCharts(atlas);
		xatlas::PackOptions options = makePackOptions(64, 1.0f, 1, true);
		xatlas::FixedUvMesh fixedMeshes[2] = { { 0, 0 }, { 1, 0 } };
		CHECK(!xatlas::PackChartsPreservingUv(atlas, fixedMeshes, 2, options));
		destroyAtlas(atlas);
		if (g_failed)
			return;
	}
	// resolution and texelsPerUnit must be positive.
	{
		UvMeshData quad = makeQuad(8.0f, 8.0f, 16.0f, 16.0f);
		xatlas::Atlas *atlas = xatlas::Create();
		addUvMeshChecked(atlas, quad);
		xatlas::ComputeCharts(atlas);
		xatlas::FixedUvMesh fixedMesh = { 0, 0 };
		xatlas::PackOptions options = makePackOptions(0, 1.0f, 1, true);
		CHECK(!xatlas::PackChartsPreservingUv(atlas, &fixedMesh, 1, options));
		options = makePackOptions(64, 0.0f, 1, true);
		CHECK(!xatlas::PackChartsPreservingUv(atlas, &fixedMesh, 1, options));
		destroyAtlas(atlas);
		if (g_failed)
			return;
	}
	// ComputeCharts must be called first.
	{
		UvMeshData quad = makeQuad(8.0f, 8.0f, 16.0f, 16.0f);
		xatlas::Atlas *atlas = xatlas::Create();
		addUvMeshChecked(atlas, quad);
		xatlas::PackOptions options = makePackOptions(64, 1.0f, 1, true);
		xatlas::FixedUvMesh fixedMesh = { 0, 0 };
		CHECK(!xatlas::PackChartsPreservingUv(atlas, &fixedMesh, 1, options));
		destroyAtlas(atlas);
		if (g_failed)
			return;
	}
	// Meshes added with AddMesh are not supported.
	{
		const float positions[] = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
		xatlas::Atlas *atlas = xatlas::Create();
		xatlas::MeshDecl decl;
		decl.vertexPositionData = positions;
		decl.vertexCount = 3;
		decl.vertexPositionStride = sizeof(float) * 3;
		CHECK(xatlas::AddMesh(atlas, decl) == xatlas::AddMeshError::Success);
		xatlas::ComputeCharts(atlas);
		xatlas::PackOptions options = makePackOptions(64, 1.0f, 1, true);
		xatlas::FixedUvMesh fixedMesh = { 0, 0 };
		CHECK(!xatlas::PackChartsPreservingUv(atlas, &fixedMesh, 1, options));
		destroyAtlas(atlas);
		if (g_failed)
			return;
	}
	// fixedUvMeshCount == 0 behaves exactly like PackCharts.
	{
		UvMeshData quad1 = makeQuad(10.25f, 8.25f, 14.25f, 12.25f);
		UvMeshData quad2 = makeQuad(0.0f, 0.0f, 4.0f, 4.0f);
		xatlas::PackOptions options = makePackOptions(64, 1.0f, 1, true);
		xatlas::Atlas *atlas1 = xatlas::Create();
		addUvMeshChecked(atlas1, quad1);
		addUvMeshChecked(atlas1, quad2);
		xatlas::ComputeCharts(atlas1);
		CHECK(xatlas::PackChartsPreservingUv(atlas1, nullptr, 0, options));
		xatlas::Atlas *atlas2 = xatlas::Create();
		addUvMeshChecked(atlas2, quad1);
		addUvMeshChecked(atlas2, quad2);
		xatlas::ComputeCharts(atlas2);
		xatlas::PackCharts(atlas2, options);
		CHECK(atlas1->meshCount == atlas2->meshCount);
		CHECK(atlas1->chartCount == atlas2->chartCount);
		CHECK(atlas1->atlasCount == atlas2->atlasCount);
		for (uint32_t m = 0; m < atlas1->meshCount; m++) {
			const xatlas::Mesh &mesh1 = atlas1->meshes[m];
			const xatlas::Mesh &mesh2 = atlas2->meshes[m];
			CHECK(mesh1.vertexCount == mesh2.vertexCount);
			for (uint32_t v = 0; v < mesh1.vertexCount; v++) {
				CHECK(mesh1.vertexArray[v].uv[0] == mesh2.vertexArray[v].uv[0]);
				CHECK(mesh1.vertexArray[v].uv[1] == mesh2.vertexArray[v].uv[1]);
				CHECK(mesh1.vertexArray[v].atlasIndex == mesh2.vertexArray[v].atlasIndex);
			}
		}
		destroyAtlas(atlas1);
		destroyAtlas(atlas2);
		if (g_failed)
			return;
	}
}

static void testFixedFailurePreservesOutput()
{
	// A failing call must not touch the previous output, and a later valid call must succeed.
	UvMeshData good = makeQuad(8.0f, 8.0f, 16.0f, 16.0f);
	UvMeshData outOfPage = makeQuad(100.0f, 100.0f, 108.0f, 108.0f);
	xatlas::Atlas *atlas = xatlas::Create();
	addUvMeshChecked(atlas, good);
	addUvMeshChecked(atlas, outOfPage);
	xatlas::ComputeCharts(atlas);
	xatlas::PackOptions options = makePackOptions(64, 1.0f, 1, true);
	xatlas::PackCharts(atlas, options);
	CHECK(atlas->meshCount == 2);
	Snapshot snapshot;
	snapshot.capture(atlas);
	// Out-of-page fixed UV: fails, output preserved.
	xatlas::FixedUvMesh badFixed = { 1, 0 };
	CHECK(!xatlas::PackChartsPreservingUv(atlas, &badFixed, 1, options));
	snapshot.verify(atlas);
	// Invalid mesh index: fails, output preserved.
	xatlas::FixedUvMesh invalidFixed = { 9, 0 };
	CHECK(!xatlas::PackChartsPreservingUv(atlas, &invalidFixed, 1, options));
	snapshot.verify(atlas);
	// Valid retry succeeds.
	xatlas::FixedUvMesh goodFixed = { 0, 0 };
	CHECK(xatlas::PackChartsPreservingUv(atlas, &goodFixed, 1, options));
	checkFixedMeshPreserved(atlas, 0, good, 0);
	checkNoChartOverlap(atlas);
	checkPixelOwnership(atlas);
	destroyAtlas(atlas);
}

static void testIslandsRing()
{
	// A fixed ring fills the page border; free islands must land inside the big inner hole.
	UvMeshData ring = makeRing(0.0f, 64.0f, 16.0f, 48.0f);
	xatlas::Atlas *atlas = xatlas::Create();
	addUvMeshChecked(atlas, ring);
	std::vector<UvMeshData> quads;
	for (int i = 0; i < 4; i++)
		quads.push_back(makeQuad(0.0f, 0.0f, 8.0f, 8.0f));
	for (size_t i = 0; i < quads.size(); i++)
		addUvMeshChecked(atlas, quads[i]);
	xatlas::ComputeCharts(atlas);
	xatlas::PackOptions options = makePackOptions(64, 1.0f, 0, false);
	xatlas::FixedUvMesh fixedMesh = { 0, 0 };
	CHECK(xatlas::PackChartsPreservingUv(atlas, &fixedMesh, 1, options));
	CHECK(atlas->atlasCount == 1); // Everything fits on the fixed page: the islands found the hole.
	CHECK(atlas->chartCount == 5);
	checkFixedMeshPreserved(atlas, 0, ring, 0);
	// Every free island vertex must be inside the hole.
	for (uint32_t m = 1; m < atlas->meshCount; m++) {
		for (uint32_t v = 0; v < atlas->meshes[m].vertexCount; v++) {
			const xatlas::Vertex &vertex = atlas->meshes[m].vertexArray[v];
			CHECK(vertex.atlasIndex == 0);
			CHECK(vertex.uv[0] >= 16.0f && vertex.uv[0] <= 48.0f);
			CHECK(vertex.uv[1] >= 16.0f && vertex.uv[1] <= 48.0f);
		}
	}
	checkNoChartOverlap(atlas);
	checkPixelOwnership(atlas);
	CHECK(atlas->utilization[0] > 0.5f); // The ring covers most of the page.
	destroyAtlas(atlas);
}

static void testIslandsRingPadding()
{
	// Same ring scenario with padding and bilinear expansion enabled.
	UvMeshData ring = makeRing(3.0f, 61.0f, 16.0f, 48.0f);
	UvMeshData quad = makeQuad(0.0f, 0.0f, 8.0f, 8.0f);
	xatlas::Atlas *atlas = xatlas::Create();
	addUvMeshChecked(atlas, ring);
	addUvMeshChecked(atlas, quad);
	xatlas::ComputeCharts(atlas);
	xatlas::PackOptions options = makePackOptions(64, 1.0f, 1, true);
	xatlas::FixedUvMesh fixedMesh = { 0, 0 };
	CHECK(xatlas::PackChartsPreservingUv(atlas, &fixedMesh, 1, options));
	CHECK(atlas->atlasCount == 1);
	checkFixedMeshPreserved(atlas, 0, ring, 0);
	for (uint32_t v = 0; v < atlas->meshes[1].vertexCount; v++) {
		const xatlas::Vertex &vertex = atlas->meshes[1].vertexArray[v];
		CHECK(vertex.atlasIndex == 0);
		CHECK(vertex.uv[0] >= 16.0f && vertex.uv[0] <= 48.0f);
		CHECK(vertex.uv[1] >= 16.0f && vertex.uv[1] <= 48.0f);
	}
	checkNoChartOverlap(atlas);
	checkPixelOwnership(atlas);
	destroyAtlas(atlas);
}

static void testIslandsMultiPage()
{
	// Two meshes fixed to different pages. Identical UVs on different pages are allowed.
	UvMeshData fixed1 = makeQuad(4.0f, 4.0f, 20.0f, 20.0f);
	UvMeshData fixed2 = makeQuad(4.0f, 4.0f, 20.0f, 20.0f);
	UvMeshData freeQuad = makeQuad(0.0f, 0.0f, 8.0f, 8.0f);
	xatlas::Atlas *atlas = xatlas::Create();
	addUvMeshChecked(atlas, fixed1);
	addUvMeshChecked(atlas, fixed2);
	addUvMeshChecked(atlas, freeQuad);
	xatlas::ComputeCharts(atlas);
	xatlas::PackOptions options = makePackOptions(64, 1.0f, 1, true);
	xatlas::FixedUvMesh fixedMeshes[2] = { { 0, 2 }, { 1, 0 } };
	CHECK(xatlas::PackChartsPreservingUv(atlas, fixedMeshes, 2, options));
	CHECK(atlas->atlasCount == 3);
	checkFixedMeshPreserved(atlas, 0, fixed1, 2);
	checkFixedMeshPreserved(atlas, 1, fixed2, 0);
	CHECK(atlas->utilization[0] > 0.0f);
	CHECK(atlas->utilization[1] == 0.0f);
	CHECK(atlas->utilization[2] > 0.0f);
	for (uint32_t v = 0; v < atlas->meshes[2].vertexCount; v++)
		CHECK(atlas->meshes[2].vertexArray[v].atlasIndex == 0);
	checkNoChartOverlap(atlas);
	checkPixelOwnership(atlas);
	destroyAtlas(atlas);
}

static void testRepack()
{
	// Re-packing with different options must not turn the previous output into the new input.
	UvMeshData fixed = makeQuad(8.5f, 8.5f, 24.5f, 24.5f);
	UvMeshData freeQuad = makeQuad(0.0f, 0.0f, 4.0f, 4.0f);
	xatlas::Atlas *atlas = xatlas::Create();
	addUvMeshChecked(atlas, fixed);
	addUvMeshChecked(atlas, freeQuad);
	xatlas::ComputeCharts(atlas);
	xatlas::FixedUvMesh fixedMesh = { 0, 0 };
	xatlas::PackOptions options1 = makePackOptions(64, 1.0f, 1, true);
	CHECK(xatlas::PackChartsPreservingUv(atlas, &fixedMesh, 1, options1));
	checkFixedMeshPreserved(atlas, 0, fixed, 0);
	// Measure the free chart extents.
	float minX = 1e30f, maxX = -1e30f;
	for (uint32_t v = 0; v < atlas->meshes[1].vertexCount; v++) {
		minX = fminf(minX, atlas->meshes[1].vertexArray[v].uv[0]);
		maxX = fmaxf(maxX, atlas->meshes[1].vertexArray[v].uv[0]);
	}
	const float extent1 = maxX - minX;
	CHECK(extent1 > 3.0f && extent1 < 5.0f);
	std::vector<xatlas::Vertex> firstVertices(atlas->meshes[1].vertexArray, atlas->meshes[1].vertexArray + atlas->meshes[1].vertexCount);
	// Repack with a different scale and rotation allowed: fixed UVs keep the input values.
	xatlas::PackOptions options2 = makePackOptions(64, 4.0f, 1, true);
	options2.rotateCharts = true;
	options2.rotateChartsToAxis = true;
	CHECK(xatlas::PackChartsPreservingUv(atlas, &fixedMesh, 1, options2));
	checkFixedMeshPreserved(atlas, 0, fixed, 0);
	minX = 1e30f;
	maxX = -1e30f;
	for (uint32_t v = 0; v < atlas->meshes[1].vertexCount; v++) {
		minX = fminf(minX, atlas->meshes[1].vertexArray[v].uv[0]);
		maxX = fmaxf(maxX, atlas->meshes[1].vertexArray[v].uv[0]);
	}
	CHECK(maxX - minX > 12.0f); // The free chart was actually rescaled.
	checkNoChartOverlap(atlas);
	// Repack with the original options: same result as the first pack, fixed UVs still exact.
	CHECK(xatlas::PackChartsPreservingUv(atlas, &fixedMesh, 1, options1));
	checkFixedMeshPreserved(atlas, 0, fixed, 0);
	CHECK(atlas->meshes[1].vertexCount == (uint32_t)firstVertices.size());
	for (uint32_t v = 0; v < atlas->meshes[1].vertexCount; v++) {
		CHECK(atlas->meshes[1].vertexArray[v].uv[0] == firstVertices[v].uv[0]);
		CHECK(atlas->meshes[1].vertexArray[v].uv[1] == firstVertices[v].uv[1]);
	}
	checkNoChartOverlap(atlas);
	checkPixelOwnership(atlas);
	destroyAtlas(atlas);
}

static void testCApi()
{
	// The C API exposes the same entry point with matching results.
	UvMeshData fixed = makeQuad(10.25f, 8.25f, 14.25f, 12.25f);
	UvMeshData freeQuad = makeQuad(0.0f, 0.0f, 4.0f, 4.0f);
	// C++ run.
	xatlas::Atlas *cppAtlas = xatlas::Create();
	addUvMeshChecked(cppAtlas, fixed);
	addUvMeshChecked(cppAtlas, freeQuad);
	xatlas::ComputeCharts(cppAtlas);
	xatlas::PackOptions cppOptions = makePackOptions(64, 1.0f, 1, true);
	xatlas::FixedUvMesh cppFixed = { 0, 1 };
	CHECK(xatlas::PackChartsPreservingUv(cppAtlas, &cppFixed, 1, cppOptions));
	CHECK(cppAtlas->atlasCount == 2);
	// C run.
	xatlasAtlas *cAtlas = xatlasCreate();
	CHECK(cAtlas != nullptr);
	xatlasUvMeshDecl cDecl;
	xatlasUvMeshDeclInit(&cDecl);
	cDecl.vertexUvData = fixed.uv.data();
	cDecl.vertexCount = (uint32_t)fixed.uv.size() / 2;
	cDecl.vertexStride = sizeof(float) * 2;
	cDecl.indexData = fixed.idx.data();
	cDecl.indexCount = (uint32_t)fixed.idx.size();
	cDecl.indexFormat = XATLAS_INDEX_FORMAT_UINT32;
	CHECK(xatlasAddUvMesh(cAtlas, &cDecl) == XATLAS_ADD_MESH_ERROR_SUCCESS);
	cDecl.vertexUvData = freeQuad.uv.data();
	cDecl.vertexCount = (uint32_t)freeQuad.uv.size() / 2;
	cDecl.indexData = freeQuad.idx.data();
	cDecl.indexCount = (uint32_t)freeQuad.idx.size();
	CHECK(xatlasAddUvMesh(cAtlas, &cDecl) == XATLAS_ADD_MESH_ERROR_SUCCESS);
	xatlasComputeCharts(cAtlas, nullptr);
	xatlasPackOptions cOptions;
	xatlasPackOptionsInit(&cOptions);
	cOptions.resolution = 64;
	cOptions.texelsPerUnit = 1.0f;
	cOptions.padding = 1;
	cOptions.bilinear = true;
	cOptions.createImage = true;
	cOptions.rotateCharts = false;
	cOptions.rotateChartsToAxis = false;
	cOptions.bruteForce = true;
	xatlasFixedUvMesh cFixed;
	cFixed.meshIndex = 0;
	cFixed.atlasIndex = 1;
	CHECK(xatlasPackChartsPreservingUv(cAtlas, &cFixed, 1, &cOptions));
	// Same geometry from both APIs.
	CHECK(cAtlas->meshCount == cppAtlas->meshCount);
	CHECK(cAtlas->chartCount == cppAtlas->chartCount);
	CHECK(cAtlas->atlasCount == cppAtlas->atlasCount);
	CHECK(cAtlas->width == cppAtlas->width && cAtlas->height == cppAtlas->height);
	for (uint32_t m = 0; m < cAtlas->meshCount; m++) {
		const xatlasMesh &cMesh = cAtlas->meshes[m];
		const xatlas::Mesh &cppMesh = cppAtlas->meshes[m];
		CHECK(cMesh.vertexCount == cppMesh.vertexCount);
		CHECK(cMesh.indexCount == cppMesh.indexCount);
		CHECK(cMesh.chartCount == cppMesh.chartCount);
		for (uint32_t v = 0; v < cMesh.vertexCount; v++) {
			CHECK(cMesh.vertexArray[v].uv[0] == cppMesh.vertexArray[v].uv[0]);
			CHECK(cMesh.vertexArray[v].uv[1] == cppMesh.vertexArray[v].uv[1]);
			CHECK(cMesh.vertexArray[v].atlasIndex == cppMesh.vertexArray[v].atlasIndex);
			CHECK(cMesh.vertexArray[v].chartIndex == cppMesh.vertexArray[v].chartIndex);
			CHECK(cMesh.vertexArray[v].xref == cppMesh.vertexArray[v].xref);
		}
		for (uint32_t i = 0; i < cMesh.indexCount; i++)
			CHECK(cMesh.indexArray[i] == cppMesh.indexArray[i]);
		for (uint32_t c = 0; c < cMesh.chartCount; c++) {
			CHECK(cMesh.chartArray[c].atlasIndex == cppMesh.chartArray[c].atlasIndex);
			CHECK(cMesh.chartArray[c].faceCount == cppMesh.chartArray[c].faceCount);
		}
	}
	// Fixed mesh kept its input UVs on page 1.
	for (uint32_t v = 0; v < cAtlas->meshes[0].vertexCount; v++) {
		const xatlasVertex *vertex = &cAtlas->meshes[0].vertexArray[v];
		CHECK(vertex->uv[0] == fixed.uv[vertex->xref * 2 + 0]);
		CHECK(vertex->uv[1] == fixed.uv[vertex->xref * 2 + 1]);
		CHECK(vertex->atlasIndex == 1);
	}
	// Invalid requests fail through the C API too.
	cFixed.meshIndex = 9;
	CHECK(!xatlasPackChartsPreservingUv(cAtlas, &cFixed, 1, &cOptions));
	CHECK(cAtlas->meshCount == cppAtlas->meshCount); // Output preserved.
	// count == 0 packs normally.
	CHECK(xatlasPackChartsPreservingUv(cAtlas, nullptr, 0, &cOptions));
	CHECK(cAtlas->chartCount == 2);
	xatlasDestroy(cAtlas);
	destroyAtlas(cppAtlas);
}

int contract_fixed(void)
{
	g_checks = 0;
	g_failed = false;
	testFixedBasic();
	if (!g_failed) testFixedPaddingBilinear();
	if (!g_failed) testFixedPageHoles();
	if (!g_failed) testFixedBoundingBoxOverlap();
	if (!g_failed) testFixedFailures();
	if (!g_failed) testFixedFailurePreservesOutput();
	return g_failed ? -g_checks : g_checks;
}

int contract_islands(void)
{
	g_checks = 0;
	g_failed = false;
	testIslandsRing();
	if (!g_failed) testIslandsRingPadding();
	if (!g_failed) testIslandsMultiPage();
	return g_failed ? -g_checks : g_checks;
}

int contract_repack(void)
{
	g_checks = 0;
	g_failed = false;
	testRepack();
	return g_failed ? -g_checks : g_checks;
}

int contract_c_api(void)
{
	g_checks = 0;
	g_failed = false;
	testCApi();
	return g_failed ? -g_checks : g_checks;
}
