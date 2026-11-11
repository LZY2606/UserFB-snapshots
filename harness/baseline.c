#include "GL/gl.h"
#include "zbuffer.h"
#include <assert.h>
#include <stdio.h>
static void triangle(void){glBegin(GL_TRIANGLES);glVertex3f(-.8f,-.8f,0);glVertex3f(.8f,-.8f,0);glVertex3f(0,.8f,0);glEnd();}
int main(void){
 ZBuffer *z=ZB_open(32,32,ZB_MODE_RGBA,0);assert(z);glInit(z);
 glViewport(0,0,32,32);glMatrixMode(GL_PROJECTION);glLoadIdentity();glMatrixMode(GL_MODELVIEW);glLoadIdentity();
 glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);glEnable(GL_DEPTH_TEST);glColor3f(1,0,0);triangle();
 int red=0,depth=0;for(int i=0;i<1024;i++){red+=(z->pbuf[i]&0xffffff)!=0;depth+=z->zbuf[i]!=0;}
 assert(red>100&&depth>100);GLint bits=-1;glGetIntegerv(GL_STENCIL_BITS,&bits);assert(bits==8);
 assert(z->sbuf!=0);
 glEnable(GL_STENCIL_TEST);GLint enabled=0;glGetIntegerv(GL_STENCIL_TEST,&enabled);assert(enabled==1);
 glDisable(GL_STENCIL_TEST);glGetIntegerv(GL_STENCIL_TEST,&enabled);assert(enabled==0);
 printf("TRIANGLE_PIXELS=%d DEPTH_PIXELS=%d STENCIL_BITS=%d\n",red,depth,bits);
 glClose();ZB_close(z);puts("TINYGL_TRIANGLE_BASELINE_OK");return 0;
}
