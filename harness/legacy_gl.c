/* Exercises unchanged upstream glfontstash.h with a driver-free GL recorder. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define FONTSTASH_IMPLEMENTATION
#include "fontstash.h"
#include "gl_spy.h"
#define GLFONTSTASH_IMPLEMENTATION
#include "glfontstash.h"
int main(void) {
    FONScontext *s=glfonsCreate(128,128,FONS_ZERO_TOPLEFT);int f;GLuint initial;
    assert(s);f=fonsAddFont(s,"jp","example/DroidSansJapanese.ttf");assert(f>=0);
    fonsSetFont(s,f);fonsSetSize(s,24);fonsSetColor(s,glfonsRGBA(1,2,3,200));
    fonsDrawText(s,3,40,"日本",NULL);
    assert(spy.creates==1 && spy.draw_count==1 && spy.draws[0].nverts==12);initial=spy.draws[0].texture;
    assert(fonsExpandAtlas(s,256,128));fonsDrawText(s,3,40,"日本",NULL);
    assert(spy.draw_count==2 && spy.draws[1].nverts==12 && spy.draws[1].texture!=initial);
    assert(!spy.textures[initial].alive);
    assert(fonsResetAtlas(s,128,128));fonsDrawText(s,3,40,"日本",NULL);
    assert(spy.creates==3 && spy.deletes==2 && spy.draw_count==3);
    glfonsDelete(s);assert(spy.deletes==3);
    puts("LEGACY_GL2_OK creates=3 deletes=3 draws=3 vertices=36");return 0;
}
