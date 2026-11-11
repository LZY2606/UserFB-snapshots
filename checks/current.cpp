#include <lunasvg.h>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
using lunasvg::Bitmap;
using lunasvg::Document;
static unsigned assertions = 0;
static void require(bool ok, const char* why) { ++assertions; if(!ok) throw std::runtime_error(why); }
static Bitmap render(const std::string& body) {
    auto doc = Document::loadFromData("<svg xmlns='http://www.w3.org/2000/svg' width='64' height='48'>" + body + "</svg>");
    require(bool(doc), "load failed");
    auto b = doc->renderToBitmap();
    require(!b.isNull() && b.width() == 64 && b.height() == 48, "bad bitmap");
    return b;
}
static uint32_t pixel(const Bitmap& b, int x, int y) {
    uint32_t value; std::memcpy(&value, b.data() + y * b.stride() + x * 4, 4); return value;
}
static int alpha(const Bitmap& b, int x, int y) { return pixel(b,x,y) >> 24; }
static void existing() {
    auto solid = render("<rect x='4' y='4' width='12' height='12' fill='red'/>");
    require(pixel(solid,8,8) == 0xffff0000u && pixel(solid,30,8) == 0, "solid");
    auto group = render("<g opacity='.5'><rect x='2' y='2' width='20' height='20' fill='red'/><rect x='12' y='2' width='20' height='20' fill='blue'/></g>");
    require(alpha(group,6,6) >= 127 && alpha(group,6,6) <= 128 && alpha(group,16,6) == alpha(group,6,6), "group isolation");
    require((pixel(group,16,6) & 0xffffffu) == unsigned(alpha(group,16,6)), "group color");
    auto clip = render("<defs><clipPath id='c'><rect x='8' y='8' width='16' height='16'/></clipPath></defs><g transform='translate(4 0)' clip-path='url(#c)'><rect width='48' height='32' fill='red'/></g>");
    require(alpha(clip,14,12) == 255 && alpha(clip,9,12) == 0 && alpha(clip,30,12) == 0, "clip transform");
    auto grad = render("<defs><linearGradient id='p'><stop offset='0' stop-color='red'/><stop offset='1' stop-color='blue'/></linearGradient></defs><rect width='64' height='48' fill='url(#p)'/>");
    require(((pixel(grad,8,20)>>16)&255) > 200 && (pixel(grad,56,20)&255) > 200, "gradient");
    auto use = render("<defs><rect id='r' width='8' height='8' fill='green'/></defs><use href='#r' x='20' y='12'/>");
    require(pixel(use,22,14) == 0xff008000u && pixel(use,2,2) == 0, "use clone");
    auto mask = render("<defs><mask id='m' maskUnits='userSpaceOnUse' x='0' y='0' width='64' height='48'><rect width='16' height='48' fill='white'/></mask></defs><rect width='64' height='48' fill='red' mask='url(#m)'/>");
    require(alpha(mask,8,20) == 255 && alpha(mask,30,20) == 0, "mask");
    std::cout << "LEGACY_RENDER_OK cases=6 assertions=" << assertions << '\n';
}
static void witness() {
    auto b = render("<defs><filter id='f' filterUnits='userSpaceOnUse' x='0' y='0' width='64' height='48'><feOffset dx='20' dy='0'/></filter></defs><rect x='4' y='8' width='12' height='12' fill='red' filter='url(#f)'/>");
    std::cout << "FILTER_START source_alpha=" << alpha(b,8,12) << " shifted_alpha=" << alpha(b,28,12) << '\n';
    require(alpha(b,8,12)==255 && alpha(b,28,12)==0, "starting witness changed");
    auto blur = render("<defs><filter id='f' filterUnits='userSpaceOnUse' x='0' y='0' width='64' height='48'><feGaussianBlur stdDeviation='2'/></filter></defs><rect x='16' y='16' width='16' height='16' fill='red' filter='url(#f)'/>");
    std::cout << "BLUR_START outside_alpha=" << alpha(blur,14,22) << " inside_alpha=" << alpha(blur,22,22) << '\n';
    require(alpha(blur,14,22)==0 && alpha(blur,22,22)==255, "starting blur witness changed");
    std::cout << "FILTER_BASELINE_OK\n";
}
int main(int argc, char** argv) {
    try { if(argc==2 && std::string(argv[1])=="--existing") existing();
          else if(argc==2 && std::string(argv[1])=="--witness") witness();
          else return 2;
    } catch(const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
