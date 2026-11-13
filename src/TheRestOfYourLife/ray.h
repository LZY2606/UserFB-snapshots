#ifndef RAY_H
#define RAY_H
//==============================================================================================
// Originally written in 2016 by Peter Shirley <ptrshrl@gmail.com>
//
// To the extent possible under law, the author(s) have dedicated all copyright and related and
// neighboring rights to this software to the public domain worldwide. This software is
// distributed without any warranty.
//
// You should have received a copy (see file COPYING.txt) of the CC0 Public Domain Dedication
// along with this software. If not, see <http://creativecommons.org/publicdomain/zero/1.0/>.
//==============================================================================================

#include "vec3.h"


class ray {
  private:
    point3 orig;
    vec3 dir;
    double tm;

  public:
    ray()
      : orig(0,0,0), dir(0,0,0), tm(0), has_differentials(false),
        do_dx(0,0,0), do_dy(0,0,0), dd_dx(0,0,0), dd_dy(0,0,0) {}

    ray(const point3& origin, const vec3& direction, double time)
      : orig(origin), dir(direction), tm(time), has_differentials(false),
        do_dx(0,0,0), do_dy(0,0,0), dd_dx(0,0,0), dd_dy(0,0,0) {}

    ray(const point3& origin, const vec3& direction)
      : ray(origin, direction, 0) {}

    const point3& origin() const  { return orig; }
    const vec3& direction() const { return dir; }

    double time() const { return tm; }

    point3 at(double t) const {
        return orig + t*dir;
    }

    // Ray differentials: derivatives of the origin and (possibly unnormalized)
    // direction with respect to the horizontal (x) and vertical (y) screen
    // coordinates. Off by default; ray time does not vary with screen position.
    bool has_differentials;
    vec3 do_dx;   // d(origin) / d(screen_x)
    vec3 do_dy;   // d(origin) / d(screen_y)
    vec3 dd_dx;   // d(direction) / d(screen_x)
    vec3 dd_dy;   // d(direction) / d(screen_y)

    void copy_differentials_from(const ray& src) {
        has_differentials = src.has_differentials;
        do_dx = src.do_dx;
        do_dy = src.do_dy;
        dd_dx = src.dd_dx;
        dd_dy = src.dd_dy;
    }

    void clear_differentials() {
        has_differentials = false;
        do_dx = vec3(0,0,0);
        do_dy = vec3(0,0,0);
        dd_dx = vec3(0,0,0);
        dd_dy = vec3(0,0,0);
    }
};


#endif
