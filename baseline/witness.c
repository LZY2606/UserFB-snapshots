#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "lstate.h"
#include "lopcodes.h"
typedef struct { char *p; size_t n; } Bytes;
static int writer(lua_State *L,const void *p,size_t n,void *u) {
  Bytes *b=u; (void)L; b->p=realloc(b->p,b->n+n); assert(b->p);
  memcpy(b->p+b->n,p,n); b->n+=n; return 0;
}
int main(void) {
 lua_State *L=luaL_newstate(); Bytes b={0}; assert(L);
 const char *src="local function many(...) return ... end; local function outer(x) return function(...) return x,many(...) end end; return outer(17)(23,29)";
 assert(luaL_loadstring(L,src)==LUA_OK); assert(lua_dump(L,writer,&b,1)==0);
 lua_pop(L,1); assert(luaL_loadbufferx(L,b.p,b.n,"valid","b")==LUA_OK);
 assert(lua_pcall(L,0,LUA_MULTRET,0)==LUA_OK);
 assert(lua_gettop(L)==3 && lua_tointeger(L,1)==17 && lua_tointeger(L,2)==23 && lua_tointeger(L,3)==29);
 free(b.p); b.p=NULL; b.n=0; lua_settop(L,0);
 assert(luaL_loadstring(L,"return 7")==LUA_OK);
 /* A well-framed local dump with one invalid control-flow word. Never execute it. */
 Proto *p=getproto(s2v(L->top.p-1));
 p->code[0]=CREATE_sJ(OP_JMP,100000,0);
 assert(lua_dump(L,writer,&b,1)==0); lua_pop(L,1);
 int status=luaL_loadbufferx(L,b.p,b.n,"never-executed","bv");
 assert(status==LUA_OK || status==LUA_ERRSYNTAX);
 printf("BASELINE_LOAD_VALID=17,23,29\nLEGACY_BV_STATUS=%d\n",status);
 free(b.p); lua_close(L); return 0;
}
