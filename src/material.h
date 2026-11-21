#ifndef MATERIAL_H
#define MATERIAL_H

#include "../lib/rand48/erand48.h"
#include "vector.h"
#include "ray.h"
#include "texture.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// IDEAL_MIRROR is appended so the original DIFF/SPEC/EMIT values keep
// their numeric identities and their old Monte Carlo behaviour.
enum MaterialType { DIFF, SPEC, EMIT, IDEAL_MIRROR };


class Material {

private:
	MaterialType m_type;
	Vec m_colour;
	Vec m_emission;
    Texture m_texture;


public:
	Material( MaterialType t=DIFF, Vec c=Vec(1,1,1), Vec e=Vec(0,0,0), Texture tex=Texture() );
	MaterialType get_type() const;
	Vec get_colour() const;
	Vec get_colour_at(double u, double v) const;
	Vec get_emission() const;
	bool has_texture() const;
	// Perfect, roughness-free reflection used by IDEAL_MIRROR.
	Ray ideal_reflected_ray(const Ray &r, const Vec &p, const Vec &n) const;
	Ray get_reflected_ray( const Ray &r, Vec &p, const Vec &n, unsigned short *Xi ) const;

};


#endif // MATERIAL_H