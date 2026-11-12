/*
** lverify.h - structural verification of precompiled chunks
** See Copyright Notice in lua.h
*/

#ifndef lverify_h
#define lverify_h

#include "lobject.h"


/*
** Verify the internal structure of a loaded precompiled chunk.
** The check is read-only: it neither executes the bytecode nor
** modifies it. On failure it pushes a diagnostic on the stack and
** throws LUA_ERRSYNTAX. This is not a full sandbox: it validates the
** shape of the chunk, not the runtime behavior of its instructions.
*/
LUAI_FUNC void luaU_verify (lua_State *L, Proto *f, int chunknup);

#endif
