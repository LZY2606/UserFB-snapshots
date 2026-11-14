#pragma once
#include <sstream>
#include <stdexcept>
#include "Camera.hpp"
#include "Scene.hpp"
#include "Image.hpp"
#include "Random.hpp"
#include "RayTracer.hpp"
namespace author_fixture {
inline void input(std::istringstream& s) {s.exceptions(std::ios::failbit|std::ios::badbit|std::ios::eofbit);}
inline const char* camera_text(){return "(0 0 0) (0 0 1) 45 ";}
inline const char* diffuse_text(){return "(1 1 1) (1 1 1)\n(-100 -100 3) (0 100 3) (100 -100 3) (0.5 0.4 0.3) (0 0 0)\n";}
inline const char* mixed_text(){return "(1 1 1) (1 1 1)\n(-100 -100 3) (0 100 3) (0 -100 3) (0.5 0.4 0.3) (0 0 0)\n";}
inline const char* emitter_text(){return "(0 0 0) (0 0 0)\n(-100 -100 3) (0 100 3) (100 -100 3) (0 0 0) (2 3 4)\n";}
}
