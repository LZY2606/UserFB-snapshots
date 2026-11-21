#include <math.h>
#include <iostream>
#include <stdio.h>

#include "vector.h"
#include "ray.h"
#include "material.h"
#include "objects.h"
//#include "../lib/fastbvh/BVH.h"


ObjectIntersection::ObjectIntersection(bool hit_, double u_, Vec n_, Material m_,
                                       long slot_, long face_)
{
	hit=hit_, u=u_, n=n_, m=m_;
	slot=slot_;
	face=face_;
}


Sphere::Sphere( Vec p_, double r_, Material m_ ) {
	m_p=p_, m_r=r_, m_m=m_;
}

double Sphere::get_radius() { return m_r; }
Material Sphere::get_material() { return m_m; }

// Check if ray intersects with sphere. Returns ObjectIntersection data structure
ObjectIntersection Sphere::get_intersection(const Ray &ray) {
	// Solve t^2*d.d + 2*t*(o-p).d + (o-p).(o-p)-R^2 = 0
	bool hit = false;
	double distance = 0;
	Vec n = Vec();

	Vec op = m_p-ray.origin;
	double t, eps=1e-4, b=op.dot(ray.direction), det=b*b-op.dot(op)+m_r*m_r;
	if (det<0) return ObjectIntersection(hit, distance, n, m_m); 
	else det=sqrt(det);
	distance = (t=b-det)>eps ? t : ((t=b+det)>eps ? t : 0);
	if (distance != 0) hit = true, 
		n = ((ray.origin + ray.direction * distance) - m_p).norm();

	return ObjectIntersection(hit, distance, n, m_m);
}


Mesh::Mesh(Vec p_, const char* file_path, Material m_) {

	m_p=p_, m_m=m_;
	m_slot=-1;

    std::string mtlbasepath;
    std::string inputfile = file_path;
    unsigned long pos = inputfile.find_last_of("/");
    mtlbasepath = inputfile.substr(0, pos+1);

    printf("Loading %s...\n", file_path);
    // Attempt to load mesh
	std::string err = tinyobj::LoadObj(m_shapes, m_materials, inputfile.c_str(), mtlbasepath.c_str());

	if (!err.empty()) {
		std::cerr << err << std::endl;
		exit(1);
	}
	printf(" - Generating k-d tree...\n\n");

    long shapes_size, indices_size, materials_size;
    shapes_size = m_shapes.size();
    materials_size = m_materials.size();

    // Load materials/textures from obj
    // TODO: Only texture is loaded at the moment, need to implement material types and colours
    for (int i=0; i<materials_size; i++) {
        std::string texture_path = "";

        if (!m_materials[i].diffuse_texname.empty()){
            if (m_materials[i].diffuse_texname[0] == '/') texture_path = m_materials[i].diffuse_texname;
            texture_path = mtlbasepath + m_materials[i].diffuse_texname;
            materials.push_back( Material(DIFF, Vec(1,1,1), Vec(), texture_path.c_str()) );
        }
        else {
            materials.push_back( Material(DIFF, Vec(1,1,1), Vec()) );
        }

    }

    // Load triangles from obj. The running face counter is the original OBJ
    // face order; duplicate faces remain distinct because each gets its own
    // Triangle with that order index. Zero area triangles are skipped.
    long face_order = 0;
    for (int i = 0; i < shapes_size; i++) {
        indices_size = m_shapes[i].mesh.indices.size() / 3;
        for (size_t f = 0; f < indices_size; f++, face_order++) {

            // Triangle vertex coordinates
            Vec v0_ = Vec(
                    m_shapes[i].mesh.positions[ m_shapes[i].mesh.indices[3*f] * 3     ],
                    m_shapes[i].mesh.positions[ m_shapes[i].mesh.indices[3*f] * 3 + 1 ],
                    m_shapes[i].mesh.positions[ m_shapes[i].mesh.indices[3*f] * 3 + 2 ]
            ) + m_p;

            Vec v1_ = Vec(
                    m_shapes[i].mesh.positions[ m_shapes[i].mesh.indices[3*f + 1] * 3     ],
                    m_shapes[i].mesh.positions[ m_shapes[i].mesh.indices[3*f + 1] * 3 + 1 ],
                    m_shapes[i].mesh.positions[ m_shapes[i].mesh.indices[3*f + 1] * 3 + 2 ]
            ) + m_p;

            Vec v2_ = Vec(
                    m_shapes[i].mesh.positions[ m_shapes[i].mesh.indices[3*f + 2] * 3     ],
                    m_shapes[i].mesh.positions[ m_shapes[i].mesh.indices[3*f + 2] * 3 + 1 ],
                    m_shapes[i].mesh.positions[ m_shapes[i].mesh.indices[3*f + 2] * 3 + 2 ]
            ) + m_p;

            Vec t0_, t1_, t2_;

            //Attempt to load triangle texture coordinates
            if (m_shapes[i].mesh.indices[3 * f + 2] * 2 + 1 < m_shapes[i].mesh.texcoords.size()) {
                t0_ = Vec(
                        m_shapes[i].mesh.texcoords[m_shapes[i].mesh.indices[3 * f] * 2],
                        m_shapes[i].mesh.texcoords[m_shapes[i].mesh.indices[3 * f] * 2 + 1],
                        0
                );

                t1_ = Vec(
                        m_shapes[i].mesh.texcoords[m_shapes[i].mesh.indices[3 * f + 1] * 2],
                        m_shapes[i].mesh.texcoords[m_shapes[i].mesh.indices[3 * f + 1] * 2 + 1],
                        0
                );

                t2_ = Vec(
                        m_shapes[i].mesh.texcoords[m_shapes[i].mesh.indices[3 * f + 2] * 2],
                        m_shapes[i].mesh.texcoords[m_shapes[i].mesh.indices[3 * f + 2] * 2 + 1],
                        0
                );
            }
            else {
                t0_=Vec();
                t1_=Vec();
                t2_=Vec();
            }

            Material *mp;
            if (f < m_shapes[i].mesh.material_ids.size() &&
                m_shapes[i].mesh.material_ids[ f ] >= 0 &&
                m_shapes[i].mesh.material_ids[ f ] < (int)materials.size())
                mp = &materials[ m_shapes[i].mesh.material_ids[ f ] ];
            else
                mp = &m_m;

            Triangle *tri = new Triangle(v0_, v1_, v2_, t0_, t1_, t2_, mp, m_slot, face_order);
            if (!tri->valid()) {
                // Finite, zero area triangles contribute nothing.
                delete tri;
                continue;
            }
            tris.push_back(tri);
        }
    }

    // Clean up
    m_shapes.clear();
    m_materials.clear();
    node = KDNode().build(tris, 0);
    printf("\n");
	//bvh = BVH(&tris);
}

// Check if ray intersects with mesh. Returns ObjectIntersection data structure
ObjectIntersection Mesh::get_intersection(const Ray &ray) {
    double tmin = INFINITY;
    Vec normal = Vec();
    Triangle *tri = node->nearest(node, ray, 1e-12, tmin, normal);
    if (tri == NULL) return ObjectIntersection();

    // Preserve the original material (including IDEAL_MIRROR and EMIT) and
    // carry the real face identity instead of flattening everything to DIFF.
    Material m = tri->m != NULL ? *tri->m : m_m;
    return ObjectIntersection(true, tmin, normal, m, tri->slot, tri->face);
}

void Mesh::set_slot(long slot) {
    m_slot = slot;
    for (size_t i = 0; i < tris.size(); ++i) {
        tris[i]->slot = slot;
    }
}

long Mesh::get_slot() const { return m_slot; }

KDNode* Mesh::get_kd_root() const { return node; }

const std::vector<Triangle*>& Mesh::get_triangles() const { return tris; }