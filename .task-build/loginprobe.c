#include <unistd.h>
#include <stdio.h>
int main(){char*b=getlogin();printf("login=%s\n",b?b:"(null)");return 0;}
