#ifndef __RAY_TRACER_H__
#define __RAY_TRACER_H__

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <memory>
#include <iostream>
#include <fstream>
#include "Vector.h"
#include "Camera.h"
#include "TetTransport.hpp"

class Ray;
class Color;
class Intersection;
class Object;
class Light;
class Material;
class NormalMap;
class BSP;

class RayTracer {
public:
   int width;
   int height;
   int maxReflections;
   int superSamples; // Square root of number of samples to use for each pixel.
   Camera camera;
   double imageScale;
   int depthComplexity;
   double dispersion;
   unsigned long long raysCast;
   Material* startingMaterial;
   BSP* bsp;

   std::vector<Object*> objects;
   std::vector<Light*> lights;
   std::map<std::string, Material*> materials;

   // Optional tetrahedral volume.  When unset, rendering behavior is
   // identical to the surface-only tracer.
   std::shared_ptr<tetvol::TetPose> volumePose;

   RayTracer(int, int, int, int, int);

   ~RayTracer();

   void addObject(Object* object) {
      objects.push_back(object);
   }

   void addLight(Light* light) {
      lights.push_back(light);
   }

   // Attaches (or clears, with nullptr) an independently posed tetrahedral
   // volume.  The tracer shares ownership of the pose; the pose's own caches
   // remain independent of any other pose of the same resource.  Defined out
   // of line in TetTransport.cpp so a build without that translation unit
   // still links as long as no volume is attached.
   void setVolume(std::shared_ptr<tetvol::TetPose> pose);

   // Public entry point that runs the original recursive castRay.  Used by
   // the headless example and the test suite.
   Color sampleRay(const Ray& ray);

   // Volume transmittance (no emission) along a raw finite segment.  Used to
   // attenuate point-light diffuse/specular contributions and shadow rays.
   tetvol::RGB volumeTransmittance(const Vector& origin, const Vector& dir,
                                   double t0, double t1) const {
      if (!volumePose || !integrateRayHook_) {
         return tetvol::RGB(1.0, 1.0, 1.0);
      }
      // Shadow/light segments carry transmittance only, never volume glow.
      return integrateRayHook_(origin, dir, t0, t1).transmittance;
   }

   void traceRays(std::string);
   void readScene(std::istream&);
   void readModel(std::string, int size, Vector translate, Material* material);

   Color castRay(const Ray&);

private:
   Color castRayForPixel(int, int);
   Color castRayAtPoint(const Vector&);
   bool isInShadow(const Ray&, double);
   Intersection getClosestIntersection(const Ray&);
   Color performLighting(const Intersection&);
   Color getAmbientLighting(const Intersection&, const Color&);
   Color getDiffuseAndSpecularLighting(const Intersection&, const Color&);
   Color getSpecularLighting(const Intersection&, Light*);
   Color getReflectiveRefractiveLighting(const Intersection&);
   double getReflectance(const Vector&, const Vector&, double, double);
   Vector refractVector(const Vector&, const Vector&, double, double);
   Vector reflectVector(Vector, Vector);
   Material* readMaterial(std::istream&);
   NormalMap* readNormalMap(std::istream&);
   void addMaterial(std::istream&);

   // Indirection through std::function keeps the tet transport symbols out
   // of builds that never attach a volume.  Installed by setVolume; the pose
   // is captured by the lambda so the signature stays pose-free.
   std::function<tetvol::TetRayTransport(
      const Vector&, const Vector&, double, double)>
      integrateRayHook_;
};

#endif
