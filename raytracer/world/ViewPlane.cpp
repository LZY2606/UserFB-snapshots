#include "ViewPlane.hpp"

ViewPlane::ViewPlane()
    : top_left(-320, 240, 0),
      bottom_right(320, -240, 0),
      normal(0, 0, 1),
      hres(640),
      vres(480) {}

int ViewPlane::get_hres() const { return hres; }
void ViewPlane::set_hres(int h) { hres = h; }
int ViewPlane::get_vres() const { return vres; }
void ViewPlane::set_vres(int v) { vres = v; }
