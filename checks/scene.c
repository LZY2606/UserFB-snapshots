#include "zbuffer.h"
#include <stdio.h>
#include <stdlib.h>
int uv_repro(unsigned* varied,unsigned* flat){ZBuffer* z=ZB_open(32,32,ZB_MODE_RGBA,0);if(!z)return 2;glInit(z);glViewport(0,0,32,32);glDisable(GL_CULL_FACE);glDisable(GL_DEPTH_TEST);glEnable(GL_TEXTURE_2D);glColor3f(1,1,1);
 unsigned char* tex=malloc(256*256*3);for(int y=0;y<256;y++)for(int x=0;x<256;x++){tex[(y*256+x)*3]=x;tex[(y*256+x)*3+1]=y;tex[(y*256+x)*3+2]=3;}
 GLuint t;glGenTextures(1,&t);glBindTexture(GL_TEXTURE_2D,t);glTexImage2D(GL_TEXTURE_2D,0,3,256,256,0,GL_RGB,GL_UNSIGNED_BYTE,tex);
 glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
 glBegin(GL_TRIANGLES);glTexCoord2f(0,0);glVertex3f(-.75f,-.75f,-.8f);glTexCoord2f(1,0);glVertex3f(.75f,-.75f,.8f);glTexCoord2f(0,1);glVertex3f(-.75f,.75f,-.8f);glEnd();
 unsigned a=((PIXEL*)z->pbuf)[16*32+12];
 glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
 glBegin(GL_TRIANGLES);glTexCoord2f(0,0);glVertex3f(-.75f,-.75f,0);glTexCoord2f(1,0);glVertex3f(.75f,-.75f,0);glTexCoord2f(0,1);glVertex3f(-.75f,.75f,0);glEnd();
 unsigned b=((PIXEL*)z->pbuf)[16*32+12];
 *varied=a;*flat=b;glClose();ZB_close(z);free(tex);return 0;}
