#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
int main(){int types[]={SOCK_DGRAM};for(int i=0;i<1;i++){int s=socket(AF_INET,types[i],0);struct sockaddr_in a;memset(&a,0,sizeof a);a.sin_family=AF_INET;a.sin_port=htons(0);a.sin_addr.s_addr=htonl(0);int r=bind(s,(struct sockaddr*)&a,sizeof a);printf("any-bind=%d errno=%d %s\n",r,errno,strerror(errno));}return 0;}
