#pragma once

/**
   This file declares the Triangle class which represents a triangle defined by
   its 3 vertices.

   Courtesy Kevin Suffern.
*/

#include "Geometry.hpp"

#include "../utilities/Point3D.hpp"

class Triangle : public Geometry {
protected:
  Point3D v0, v1, v2; // the vertices. they must not be colinear.

public:
  // Constructors. Passed vertices are assumed to be ordered for orientation,
  Triangle(); // triangle with vertices at origin.
  Triangle(const Point3D &, const Point3D &, const Point3D &); // set vertices.

  // Copy constructor and assignment operator.
  Triangle(const Triangle &object);
  Triangle &operator=(const Triangle &rhs);

  // Destructor.
  virtual ~Triangle() = default;

  // String representation.
  std::string to_string() const override;

  // Ray intersection. Set t and sinfo as per intersection with this object.
  virtual bool hit(const Ray &ray, float &t, ShadeInfo &s) const override;

  // Finite, double-sided segment intersection with an explicit open interval.
  bool hit_segment(const Point3D &O, const Point3D &Q,
                   double s_lo, double s_hi, double &s_hit) const override;

  // Vertices (ordered for orientation). i must be 0, 1 or 2.
  const Point3D &vertex(int i) const;

  // Get bounding box.
  virtual BBox getBBox() const override;
};
