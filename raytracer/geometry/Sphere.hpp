#pragma once

/**
   This file declares the Sphere class which represents a sphere defined by its
   center and radius.

   Courtesy Kevin Suffern.
*/

#include "Geometry.hpp"

#include "../utilities/Point3D.hpp"

class Sphere : public Geometry {
protected:
  Point3D c; // center.
  float r;   // radius.

public:
  // Constructors.
  Sphere(); // sphere at origin with radius 0.
  Sphere(const Point3D &center, float radius); // set center and radius,

  // Copy constructor and assignment operator.
  Sphere(const Sphere &object);
  Sphere &operator=(const Sphere &rhs);

  // Destructor.
  virtual ~Sphere() = default;

  // String representation.
  std::string to_string() const override;

  // Ray intersection. Set t and sinfo as per intersection with this object.
  virtual bool hit(const Ray &ray, float &t, ShadeInfo &s) const override;

  // Finite, double-sided segment intersection with an explicit open interval.
  bool hit_segment(const Point3D &O, const Point3D &Q,
                   double s_lo, double s_hi, double &s_hit) const override;

  // Center and radius.
  Point3D center() const;
  float radius() const;

  // Get bounding box.
  virtual BBox getBBox() const override;
};
