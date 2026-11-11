#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include "src/examples/Texture.h"
#include "src/renderer/PixelData.h"
int main() {
    SDL_Surface* image=SDL_CreateRGBSurface(0,16,16,32,0xff0000,0xff00,0xff,0);
    assert(image);
    for(int y=0;y<16;y++) for(int x=0;x<16;x++)
        static_cast<Uint32*>(image->pixels)[y*16+x]=(y%2 ? 0xffffff : 0);
    swr::Texture t(image,8); SDL_FreeSurface(image);
    Uint32 a=0,b=0,flat=0;
    t.sample(.37f,.16f,8.f/16,0,0,1.f/16,a);
    const float q=std::sqrt(.5f);
    t.sample(.37f,.16f,8.f*q/16,q/16,-8.f*q/16,q/16,b);
    t.sample(.37f,.16f,0,0,0,0,flat);
    assert(a<=0xffffff && b<=0xffffff && flat<=0xffffff);
    assert(a!=b);
    std::cout << "baseline basis-dependent RGB: " << (a&255) << " " << (b&255) << "; zero-footprint=" << (flat&255) << "\n";
}
