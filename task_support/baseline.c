#include "GL/gl.h"
#include "zbuffer.h"
#include <stdio.h>
static void triangle(float z){glBegin(GL_TRIANGLES);glVertex3f(-1.4f,-.8f,z);glVertex3f(.8f,-.8f,z);glVertex3f(0,.8f,z);glEnd();}
int main(void){ZBuffer*z=ZB_open(48,32,ZB_MODE_RGBA,0);if(!z)return 2;glInit(z);glViewport(0,0,48,32);glMatrixMode(GL_PROJECTION);glLoadIdentity();glMatrixMode(GL_MODELVIEW);glLoadIdentity();glDisable(GL_LIGHTING);glDisable(GL_CULL_FACE);glEnable(GL_DEPTH_TEST);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);glColor3f(1,0,0);triangle(-.4f);glColor3f(0,1,0);triangle(.4f);PIXEL p=z->pbuf[16*48+24];int red=GET_RED(p),green=GET_GREEN(p),ok=red>200&&green<10&&z->zbuf[16*48+24]>0;printf("SEED_CLIPPED_TRIANGLE center_rgb=%d,%d,%d z=%u\n",red,green,GET_BLUE(p),z->zbuf[16*48+24]);
 glNewList(1,GL_COMPILE);glColor3f(0,0,1);triangle(-.7f);glEndList();ok&=z->pbuf[16*48+24]==p;glCallList(1);p=z->pbuf[16*48+24];printf("SEED_LIST_REPLAY center_blue=%d\n",GET_BLUE(p));ok&=GET_BLUE(p)>200;
 glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);glColor3f(1,1,1);glBegin(GL_LINE_STRIP);glVertex3f(-1.4f,-.6f,0);glVertex3f(.3f,.6f,0);glVertex3f(.8f,-.5f,0);glEnd();int n=0;for(int i=0;i<48*32;i++)if(GET_RED(z->pbuf[i]))n++;printf("SEED_CLIPPED_STRIP pixels=%d\n",n);ok&=n>10;glClose();ZB_close(z);if(!ok)return 3;puts("TINYGL_BASELINE_OK");return 0;}
