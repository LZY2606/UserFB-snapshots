#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
int main(){int s=socket(AF_INET,SOCK_DGRAM,0);struct sockaddr_in a;memset(&a,0,sizeof a);a.sin_family=AF_INET;a.sin_port=htons(9);a.sin_addr.s_addr=htonl(0x7f000001);int r=sendto(s,"x",1,0,(struct sockaddr*)&a,sizeof a);printf("sendto=%d errno=%d %s\n",r,errno,strerror(errno));return 0;}
