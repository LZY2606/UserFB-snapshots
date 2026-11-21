#pragma once

/**
   This file declares the Geometry class which is an abstract class from which
   other concrete geometric objects will inherit.

   Courtesy Kevin Suffern.
*/

#include <string>

class BBox;
class Material;
class Point3D;
class Ray;
class ShadeInfo;

class Geometry {
protected:
  Material *material_ptr; // this object's material.

public:
  // Constructors.
  Geometry(); // sets material_ptr to NULL.

  // Copy constructor and assignment operator.
  Geometry(const Geometry &object) = default;
  Geometry &operator=(const Geometry &rhs) = default;

  // Destructor.
  virtual ~Geometry() = default;

  // String representation.
  virtual std::string to_string() const = 0;

  // Get/set material.
  Material *get_material() const;
  void set_material(Material *mPtr);

  // Ray intersection. Set t and sinfo as per intersection with this object.
  virtual bool hit(const Ray &ray, float &t, ShadeInfo &sinfo) const = 0;

  // Finite segment intersection for visibility queries. The segment is
  // O + s (Q - O), s in (s_lo, s_hi). Double sided: any intersection in that
  // interval blocks. On a hit, s_hit receives the nearest blocking parameter.
  // Rendering epsilons (kEpsilon) never cull blockers inside the interval.
  virtual bool hit_segment(const Point3D &O, const Point3D &Q,
                           double s_lo, double s_hi, double &s_hit) const = 0;

  // Get bounding box.
  virtual BBox getBBox() const = 0;
};
