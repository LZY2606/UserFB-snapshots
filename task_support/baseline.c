#include <stdio.h>
#include <GL/gl.h>
#include <zbuffer.h>
int main(void){
 ZBuffer *z=ZB_open(32,32,ZB_MODE_RGBA,0); if(!z)return 2;
 glInit(z);glViewport(0,0,32,32);glEnable(GL_DEPTH_TEST);
 glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
 glColor3f(1,0,0);glBegin(GL_TRIANGLES);
 glVertex3f(-.8f,-.8f,-.2f);glVertex3f(.8f,-.8f,-.2f);glVertex3f(0,.8f,-.2f);glEnd();
 int count=0;for(int i=0;i<32*32;i++)if(z->zbuf[i])count++;
 printf("BASELINE_TRIANGLE covered=%d depth_bits=%zu\n",count,8*sizeof(*z->zbuf));
 glClose();ZB_close(z);return count>100?0:3;
}
