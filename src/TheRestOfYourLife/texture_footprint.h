#ifndef TEXTURE_FOOTPRINT_H
#define TEXTURE_FOOTPRINT_H
//==============================================================================================
// Texture footprint carried along a differentiated light path.
//==============================================================================================

#include <cmath>


class texture_footprint {
  public:
    texture_footprint()
      : valid(false), dudx(0), dvdx(0), dudy(0), dvdy(0) {}

    bool valid;
    double dudx;
    double dvdx;
    double dudy;
    double dvdy;

    void reset() {
        valid = false;
        dudx = dvdx = dudy = dvdy = 0.0;
    }

    bool all_finite() const {
        return std::isfinite(dudx) && std::isfinite(dvdx)
            && std::isfinite(dudy) && std::isfinite(dvdy);
    }
};


#endif
