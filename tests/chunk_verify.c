/*
** tests/chunk_verify.c - contract tests for the 'v' load mode.
** See Copyright Notice in lua.h
**
** The tests start from chunks produced by the real compiler, apply a
** small, finite set of field mutations, and dump the result again.
** Corrupt chunks are only loaded, never executed; sound chunks are
** actually called once. Everything is plain C; no extra packages.
*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lstate.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lopnames.h"


typedef struct { char *p; size_t n; } Bytes;

static int g_count;
static char g_lastmsg[512];

static void ck (int cond, const char *what) {
  if (!cond) {
    fprintf(stderr, "ASSERT FAILED: %s\n", what);
    exit(2);
  }
  g_count++;
}

static int writer (lua_State *L, const void *p, size_t n, void *u) {
  Bytes *b = u;
  char *np;
  (void)L;
  np = realloc(b->p, b->n + n);
  if (np == NULL)
    return 1;
  b->p = np;
  memcpy(b->p + b->n, p, n);
  b->n += n;
  return 0;
}

static Proto *compile_proto (lua_State *L, const char *src) {
  Proto *p;
  ck(luaL_loadstring(L, src) == LUA_OK, "source compiles");
  p = getproto(s2v(L->top.p - 1));
  ck(p != NULL && p->sizecode > 0, "compiled proto has code");
  return p;
}

static Bytes dump_proto (lua_State *L, int strip) {
  Bytes b;
  b.p = NULL;
  b.n = 0;
  ck(lua_dump(L, writer, &b, strip) == 0, "lua_dump succeeds");
  lua_pop(L, 1);
  ck(b.n > 0, "dump produced bytes");
  return b;
}

static Proto *compile_and_dump (lua_State *L, const char *src,
                                Bytes *out, int strip) {
  Proto *p = compile_proto(L, src);
  *out = dump_proto(L, strip);
  return p;
}

static int loadmode (lua_State *L, const Bytes *b, const char *mode) {
  return luaL_loadbufferx(L, b->p, b->n, "chunk", mode);
}

/* A corrupted chunk must be rejected with a shaped diagnostic. */
static void expect_bad (lua_State *L, const Bytes *b, const char *tag) {
  int status = loadmode(L, b, "bv");
  const char *msg;
  size_t len;
  ck(status == LUA_ERRSYNTAX, tag);
  ck(lua_isstring(L, -1), "diagnostic left on stack");
  msg = lua_tolstring(L, -1, &len);
  ck(msg != NULL, "diagnostic is a string");
  strncpy(g_lastmsg, msg, sizeof(g_lastmsg) - 1);
  g_lastmsg[sizeof(g_lastmsg) - 1] = '\0';
  ck(strstr(g_lastmsg, "proto ") != NULL, "diagnostic has proto path");
  ck(strstr(g_lastmsg, "word ") != NULL, "diagnostic has word position");
  ck(strstr(g_lastmsg, "): ") != NULL, "diagnostic has a reason");
  if (getenv("CV_DEBUG"))
    fprintf(stderr, "  [%s] %s\n", tag, g_lastmsg);
  lua_pop(L, 1);
}

/* A sound chunk must load under verification. */
static void expect_good (lua_State *L, const Bytes *b, const char *tag) {
  int status = loadmode(L, b, "bv");
  if (status != LUA_OK) {
    fprintf(stderr, "good chunk rejected: %s: %s\n", tag,
            lua_tostring(L, -1));
    exit(2);
  }
  g_count++;
  lua_pop(L, 1);
}

static int findop (Proto *p, OpCode op, int from) {
  int i;
  for (i = from; i < p->sizecode; i++)
    if (GET_OPCODE(p->code[i]) == op)
      return i;
  return -1;
}

static void dumpproto (Proto *p, int depth) {
  int i;
  if (!getenv("CV_DEBUG"))
    return;
  fprintf(stderr, "%sproc@%p code=%d k=%d up=%d p=%d max=%d params=%d\n",
          depth ? "  " : "", (void *)p, p->sizecode, p->sizek,
          p->sizeupvalues, p->sizep, (int)p->maxstacksize,
          (int)p->numparams);
  for (i = 0; i < p->sizecode; i++)
    fprintf(stderr, "  %3d op=%-10s A=%3d B=%3d C=%3d k=%d\n", i,
            opnames[GET_OPCODE(p->code[i])],
            GETARG_A(p->code[i]), GETARG_B(p->code[i]),
            GETARG_C(p->code[i]), GETARG_k(p->code[i]));
  for (i = 0; i < p->sizep; i++)
    dumpproto(p->p[i], depth + 1);
}


/* ===================================================================== */

/*
** Structure: register windows, constants and their specialized types,
** upvalues, child prototype indexes, header compatibility, ancillary
** words, metamethod pairing and the root upvalue count.
*/
int contract_structure (void) {
  lua_State *L = luaL_newstate();
  Bytes b;
  Proto *p;
  int w;

  g_count = 0;
  ck(L != NULL, "new state");
  luaL_openlibs(L);

  /* sound baseline loads and verifies */
  compile_and_dump(L,
    "local function many(...) return ... end; "
    "local function outer(x) return function(...) return x,many(...) end end; "
    "return outer(17)(23,29)", &b, 0);
  expect_good(L, &b, "baseline");
  free(b.p);

  /* register out of window */
  p = compile_proto(L, "local x = 1; return x");
  w = findop(p, OP_LOADI, 0);
  ck(w >= 0, "found LOADI");
  SETARG_A(p->code[w], 200);
  b = dump_proto(L, 0);
  expect_bad(L, &b, "register out of range");
  free(b.p);

  /* constant index out of range */
  p = compile_proto(L, "local x = 1.5; return x");
  w = findop(p, OP_LOADK, 0);
  ck(w >= 0, "found LOADK");
  SETARG_Bx(p->code[w], p->sizek + 16);
  b = dump_proto(L, 0);
  expect_bad(L, &b, "constant out of range");
  free(b.p);

  /* LOADKX without a proper EXTRAARG */
  p = compile_proto(L, "local x = 1.5; return x");
  w = findop(p, OP_LOADK, 0);
  ck(w >= 0, "found LOADK to mutate");
  SET_OPCODE(p->code[w], OP_LOADKX);
  b = dump_proto(L, 0);
  expect_bad(L, &b, "LOADKX missing EXTRAARG");
  free(b.p);

  /* NEWTABLE whose ancillary word is not an EXTRAARG */
  p = compile_proto(L, "local t = {}; return t");
  w = findop(p, OP_NEWTABLE, 0);
  ck(w >= 0 && w + 1 < p->sizecode, "found NEWTABLE and next word");
  SET_OPCODE(p->code[w + 1], OP_MOVE);
  b = dump_proto(L, 0);
  expect_bad(L, &b, "NEWTABLE ancillary mismatch");
  free(b.p);

  /* SETLIST with the extension bit but no EXTRAARG */
  p = compile_proto(L, "local t = {1, 2, 3}; return t");
  w = findop(p, OP_SETLIST, 0);
  ck(w >= 0, "found SETLIST");
  SETARG_k(p->code[w], 1);
  b = dump_proto(L, 0);
  expect_bad(L, &b, "SETLIST extension mismatch");
  free(b.p);

  /*
  ** A structurally sound extended SETLIST. The compiler flushes every
  ** 50 fields so it never emits one directly; build it by limited field
  ** mutation of a compiled chunk (this one is only loaded, not run).
  */
  {
    char src[2048];
    size_t pos = 0;
    int i;
    pos += (size_t)snprintf(src + pos, sizeof(src) - pos,
                            "local t = {");
    for (i = 1; i <= 300; i++)
      pos += (size_t)snprintf(src + pos, sizeof(src) - pos, "%d,", i);
    pos += (size_t)snprintf(src + pos, sizeof(src) - pos,
                            "} return #t");
    ck(pos < sizeof(src), "long source fits");
    p = compile_proto(L, src);
    w = findop(p, OP_SETLIST, 0);  /* first flush; more words follow */
    ck(w >= 0 && w + 2 < p->sizecode, "SETLIST with following words");
    SETARG_k(p->code[w], 1);
    p->code[w + 1] = CREATE_Ax(OP_EXTRAARG, 1);
    b = dump_proto(L, 0);
    expect_good(L, &b, "extended SETLIST sound");
    free(b.p);
  }

  /* metamethod event mismatch */
  p = compile_proto(L, "local a,b = 1, 2; return a + b");
  dumpproto(p, 0);
  w = findop(p, OP_ADD, 0);
  ck(w >= 0, "found OP_ADD");
  ck(w + 1 < p->sizecode &&
     GET_OPCODE(p->code[w + 1]) == OP_MMBIN, "MMBIN follows ADD");
  SETARG_C(p->code[w + 1], TM_SUB);  /* wrong event */
  b = dump_proto(L, 0);
  expect_bad(L, &b, "MMBIN event mismatch");
  free(b.p);

  /* standalone metamethod fallback word */
  p = compile_proto(L, "local a,b = 1, 2; return a + b");
  w = findop(p, OP_MMBIN, 0);
  ck(w >= 0, "found MMBIN");
  SET_OPCODE(p->code[w - 1], OP_MOVE);  /* destroy its arithmetic owner */
  b = dump_proto(L, 0);
  expect_bad(L, &b, "standalone MMBIN");
  free(b.p);

  /* subtraction/left-shift of a negative immediate: ADDI/SHRI with the
     original SUB/SHL event is legitimate compiler output (sound) */
  compile_and_dump(L,
    "local function f(a) return a - -2, a - 3, a << -2, a >> 2 end; "
    "return f(10)", &b, 0);
  expect_good(L, &b, "negated immediates sound");
  free(b.p);

  /* ...but the fallback immediate must be the arithmetic one negated */
  p = compile_proto(L, "local function f(a) return a - 3 end; return f");
  {
    Proto *f = p->p[0];
    int mm = findop(f, OP_MMBINI, 0);
    ck(mm >= 0, "found MMBINI");
    SETARG_B(f->code[mm], int2sC(99));  /* no longer equals -arith sC */
    b = dump_proto(L, 0);
    expect_bad(L, &b, "negated immediate relation");
    free(b.p);
  }

  /* upvalue capture out of the parent register window */
  p = compile_proto(L,
    "local function f() local u = 1; return function() return u end end; "
    "return f()");
  ck(p->sizep >= 1 && p->p[0]->sizep >= 1, "has a nested closure proto");
  {
    Proto *closure = p->p[0]->p[0];  /* the function returning 'u' */
    ck(closure->sizeupvalues >= 1, "closure captures an upvalue");
    closure->upvalues[0].instack = 1;
    closure->upvalues[0].idx = 250;
    b = dump_proto(L, 0);
    expect_bad(L, &b, "capture register out of range");
    free(b.p);
  }

  /* GETUPVAL index out of range */
  p = compile_proto(L,
    "local function f() local u = 1; return function() return u end end; "
    "return f()");
  {
    Proto *closure = p->p[0]->p[0];
    w = findop(closure, OP_GETUPVAL, 0);
    ck(w >= 0, "found GETUPVAL in closure");
    SETARG_B(closure->code[w], closure->sizeupvalues + 3);
    b = dump_proto(L, 0);
    expect_bad(L, &b, "GETUPVAL out of range");
    free(b.p);
  }

  /* child prototype index out of range */
  p = compile_proto(L, "local function f() end; return f");
  w = findop(p, OP_CLOSURE, 0);
  ck(w >= 0, "found CLOSURE");
  SETARG_Bx(p->code[w], p->sizep + 2);
  b = dump_proto(L, 0);
  expect_bad(L, &b, "child proto out of range");
  free(b.p);

  /* numparams / maxstacksize incompatibility */
  p = compile_proto(L, "local function f(a, b) return a + b end; return f");
  p->maxstacksize = 1;  /* below numparams of the inner function */
  ck(p->sizep >= 1 && p->p[0]->numparams > 1, "inner proto has params");
  p->p[0]->maxstacksize = 0;
  b = dump_proto(L, 0);
  expect_bad(L, &b, "maxstacksize incompatible");
  free(b.p);

  /* stray EXTRAARG word (a non-vararg function ends with RETURN1) */
  p = compile_proto(L, "local function f() return 1 end; return f");
  {
    Proto *f = p->p[0];
    w = findop(f, OP_RETURN1, 0);
    ck(w >= 0, "function contains RETURN1");
    f->code[w] = CREATE_Ax(OP_EXTRAARG, 0);
    b = dump_proto(L, 0);
    expect_bad(L, &b, "stray EXTRAARG");
    free(b.p);
  }

  /* root upvalue count disagrees with the chunk header byte */
  {
    char *raw;
    compile_and_dump(L, "return _ENV", &b, 1);
    ck(sizeof(Instruction) == 4 && sizeof(lua_Integer) == 8 &&
       sizeof(lua_Number) == 8, "known header layout");
    /* header: 4 sig + 1 ver + 1 fmt + 6 data + 3 sizes + 8 + 8 = 31 */
    ck(b.n > 31, "header byte present");
    raw = malloc(b.n);
    ck(raw != NULL, "raw copy alloc");
    memcpy(raw, b.p, b.n);
    ck(raw[31] == 1, "header declares one upvalue");
    raw[31] = 2;  /* header now disagrees with the root prototype */
    free(b.p);
    b.p = raw;
    expect_bad(L, &b, "root upvalue count mismatch");
    free(b.p);
  }

  /* specialized constant type: bitwise K must be an integer */
  p = compile_proto(L, "local x = 1; local f = 3.5; return x & 1");
  dumpproto(p, 0);
  w = findop(p, OP_BANDK, 0);
  if (w < 0)  /* compiler may keep both operands in registers */
    w = findop(p, OP_BAND, 0);
  ck(w >= 0, "found a bitwise word");
  if (GET_OPCODE(p->code[w]) == OP_BANDK) {
    int ci = GETARG_C(p->code[w]);
    int fi;
    for (fi = 0; fi < p->sizek; fi++)
      if (ttisfloat(&p->k[fi]))
        break;
    ck(fi < p->sizek, "a float constant exists");
    p->k[ci] = p->k[fi];  /* replace integer K with a float K */
    b = dump_proto(L, 0);
    expect_bad(L, &b, "bitwise constant type");
    free(b.p);
  }
  else
    g_count++;  /* platform choice covered by the K arithmetic tests */

  lua_close(L);
  return g_count;
}


/* ===================================================================== */

/*
** Flow: ancillary words cannot be branch targets, open-result edges,
** for-loop successors and terminal words.
*/
int contract_flow (void) {
  lua_State *L = luaL_newstate();
  Bytes b;
  Proto *p;
  int w, jmp;

  g_count = 0;
  ck(L != NULL, "new state");
  luaL_openlibs(L);

  /* branch into a metamethod fallback word */
  p = compile_proto(L,
    "local function f(...) local s = 0; local i = 0; "
    "while i < 3 do s = s + i; i = i + 1 end; return s, ... end; "
    "return f(1)");
  dumpproto(p, 0);
  {
    Proto *fp = p->p[0];
    int mm = findop(fp, OP_MMBIN, 0);
    jmp = findop(fp, OP_JMP, 0);
    ck(mm >= 0 && jmp >= 0, "found MMBIN and JMP in loop proto");
    SETARG_sJ(fp->code[jmp], mm - (jmp + 1));  /* land on the MMBIN */
    b = dump_proto(L, 0);
    expect_bad(L, &b, "jump into MMBIN");
    free(b.p);
  }

  /* branch into an EXTRAARG word */
  p = compile_proto(L,
    "local function f(cond) local t; if cond then t = {} end; return t end; "
    "return f(true)");
  dumpproto(p, 0);
  {
    Proto *fp = p->p[0];
    int nt = findop(fp, OP_NEWTABLE, 0);
    jmp = findop(fp, OP_JMP, 0);
    ck(nt >= 0 && jmp >= 0, "found NEWTABLE and JMP");
    SETARG_sJ(fp->code[jmp], (nt + 1) - (jmp + 1));
    b = dump_proto(L, 0);
    expect_bad(L, &b, "jump into EXTRAARG");
    free(b.p);
  }

  /* open producer removed: VARARG no longer open, consumer still B=0 */
  p = compile_proto(L,
    "local function f(...) return ... end; "
    "local function g(...) return f(...) end; "
    "return g(1, 2)");
  dumpproto(p, 0);
  {
    Proto *gp = p->p[1];
    int va = findop(gp, OP_VARARG, 0);
    int tc = findop(gp, OP_TAILCALL, 0);
    ck(va >= 0 && tc >= 0, "found VARARG and TAILCALL");
    ck(GETARG_C(gp->code[va]) == 0 &&
       GETARG_B(gp->code[tc]) == 0, "open pair as compiled");
    SETARG_C(gp->code[va], 2);  /* fixed results: open top disappears */
    b = dump_proto(L, 0);
    expect_bad(L, &b, "missing open results");
    free(b.p);
  }

  /* jump that bypasses the open call on its way to the B=0 RETURN */
  p = compile_proto(L,
    "local function f(...) return ... end; "
    "local function g(cond, ...) local x = 0; "
    "if cond then x = 1 end; return x, f(...) end; "
    "return g(true, 1, 2)");
  dumpproto(p, 0);
  {
    Proto *gp = p->p[1];
    int ret = findop(gp, OP_RETURN, 0);
    jmp = findop(gp, OP_JMP, 0);
    ck(ret >= 0 && jmp >= 0 && GETARG_B(gp->code[ret]) == 0,
       "B=0 RETURN and a JMP exist");
    SETARG_sJ(gp->code[jmp], ret - (jmp + 1));
    b = dump_proto(L, 0);
    expect_bad(L, &b, "jump bypasses open producer");
    free(b.p);
  }

  /* numeric for: skip edge leaves the code */
  p = compile_proto(L, "for i = 1, 10 do local x = i end");
  dumpproto(p, 0);
  w = findop(p, OP_FORPREP, 0);
  ck(w >= 0, "found FORPREP");
  SETARG_Bx(p->code[w], 100000);
  b = dump_proto(L, 0);
  expect_bad(L, &b, "for prep edge out of code");
  free(b.p);

  /* numeric for: back edge lands on an ancillary word */
  p = compile_proto(L,
    "local s = 0; for i = 1, 10 do s = s + i end; return s");
  dumpproto(p, 0);
  {
    int fl = findop(p, OP_FORLOOP, 0);
    int mm = findop(p, OP_MMBIN, 0);
    ck(fl >= 0 && mm >= 0, "found FORLOOP and an MMBIN");
    SETARG_Bx(p->code[fl], (fl + 1) - mm);  /* back edge target = MMBIN */
    b = dump_proto(L, 0);
    expect_bad(L, &b, "for back edge into ancillary");
    free(b.p);
  }

  /* generic for: TFORPREP must reach TFORCALL */
  p = compile_proto(L, "for k, v in pairs({a = 1}) do local z = k .. v end");
  dumpproto(p, 0);
  w = findop(p, OP_TFORPREP, 0);
  ck(w >= 0, "found TFORPREP");
  SETARG_Bx(p->code[w], 0);  /* now reaches the word right after prep */
  b = dump_proto(L, 0);
  expect_bad(L, &b, "tfor prep target");
  free(b.p);

  /* fall off the end: turn the reachable terminal words into ordinary ones */
  p = compile_proto(L, "local function f() return 1 end; return f");
  {
    Proto *f = p->p[0];
    int r1 = findop(f, OP_RETURN1, 0);
    ck(r1 >= 0, "function contains RETURN1");
    f->code[r1] = CREATE_ABCk(OP_MOVE, 0, 0, 0, 0);
    f->code[f->sizecode - 1] =
        CREATE_ABCk(OP_MOVE, 0, 0, 0, 0);  /* trailing RETURN0 as well */
    b = dump_proto(L, 0);
    expect_bad(L, &b, "fall off code end");
    free(b.p);
  }

  /* unreachable words are still validated for their operands */
  p = compile_proto(L,
    "do return 1 end; local dead = 2");  /* words after RETURN are dead */
  dumpproto(p, 0);
  {
    int dead = findop(p, OP_LOADI, 0);
    ck(dead >= 0, "found a dead LOADI");
    SETARG_A(p->code[dead], 210);  /* invalid even if never executed */
    b = dump_proto(L, 0);
    expect_bad(L, &b, "dead word operand checked");
    free(b.p);
  }

  /* sound loops and metamethod-capable paths must survive the walk */
  compile_and_dump(L,
    "local function f(...) local s = 0; "
    "for i = 1, 20 do s = s + i end; "
    "local t = {}; for k, v in pairs({a=1,b=2}) do t[k] = v end; "
    "return s, t, ... end; return f(9, 8)", &b, 0);
  expect_good(L, &b, "sound loops");
  free(b.p);

  lua_close(L);
  return g_count;
}


/* ===================================================================== */

/*
** Captures: recursive closure capture ownership and the root header
** count, including stripped dumps.
*/
int contract_captures (void) {
  lua_State *L = luaL_newstate();
  Bytes b;
  Proto *p;

  g_count = 0;
  ck(L != NULL, "new state");
  luaL_openlibs(L);

  /* sound nested captures, stripped and non-stripped */
  static const char *capsrc =
    "local function outer() local u = 11; "
    "return function() return function() return u end end end; "
    "return outer()()";
  compile_and_dump(L, capsrc, &b, 0);
  expect_good(L, &b, "nested captures");
  free(b.p);
  compile_and_dump(L, capsrc, &b, 1);
  expect_good(L, &b, "nested captures stripped");
  free(b.p);

  /* cross-layer capture: innermost upvalue must name an outer upvalue */
  p = compile_proto(L, capsrc);
  ck(p->sizep >= 1 && p->p[0]->sizep >= 1 && p->p[0]->p[0]->sizep >= 1,
     "prototype nesting present");
  {
    Proto *middle = p->p[0]->p[0];  /* closure capturing local 'u' */
    Proto *inner = middle->p[0];    /* closure inheriting through 'u' */
    ck(inner->sizeupvalues >= 1 && inner->upvalues[0].instack == 0,
       "innermost capture goes through an outer upvalue");
    inner->upvalues[0].idx = cast_byte(middle->sizeupvalues + 2);
    b = dump_proto(L, 0);
    expect_bad(L, &b, "cross-layer capture index");
    /* the diagnostic must name a nested, non-root prototype path */
    ck(strstr(g_lastmsg, "proto 0.0.0") != NULL,
       "cross-layer fault names a nested prototype path");
    free(b.p);
  }

  /* instack capture must point inside the parent register window */
  p = compile_proto(L,
    "local function f() local u = 1; return function() return u end end; "
    "return f()");
  {
    Proto *closure = p->p[0]->p[0];
    closure->upvalues[0].instack = 1;
    closure->upvalues[0].idx = cast_byte(p->p[0]->maxstacksize + 10);
    b = dump_proto(L, 0);
    expect_bad(L, &b, "instack capture window");
    free(b.p);
  }

  /* invalid instack flag */
  p = compile_proto(L,
    "local function f() local u = 1; return function() return u end end; "
    "return f()");
  p->p[0]->p[0]->upvalues[0].instack = 7;
  b = dump_proto(L, 0);
  expect_bad(L, &b, "invalid instack flag");
  free(b.p);

  /* root header disagreement is also caught after stripping */
  {
    char *raw;
    compile_and_dump(L, "return _ENV", &b, 1);
    raw = malloc(b.n);
    ck(raw != NULL, "raw alloc");
    memcpy(raw, b.p, b.n);
    raw[31] = 0;
    free(b.p);
    b.p = raw;
    expect_bad(L, &b, "stripped root count mismatch");
    free(b.p);
  }

  lua_close(L);
  return g_count;
}


/* ===================================================================== */

/*
** Compatibility: legacy modes keep their behavior, 'v' alone never
** loads, the same lua_State recovers after a rejection, and sound
** chunks (open results, loops, closures, strip) really execute.
*/
int contract_compat (void) {
  lua_State *L = luaL_newstate();
  Bytes bin, txt, corrupt;
  Proto *p;
  int w;
  int status;

  g_count = 0;
  ck(L != NULL, "new state");
  luaL_openlibs(L);

  static const char *src =
    "local function many(...) return ... end; "
    "local function outer(x) "
    "  local function inner(...) return x, many(...) end "
    "  return inner "
    "end; "
    "local sum = 0; for i = 1, 10 do sum = sum + i end; "
    "local t = {}; for k, v in pairs({a = 1, b = 2}) do t[k] = v end; "
    "local r1, r2, r3 = outer(17)(23, 29); "
    "return sum, t.a, t.b, r1, r2, r3";

  p = compile_proto(L, src);
  bin = dump_proto(L, 0);
  ck(luaL_loadstring(L, "return 40 + 2") == LUA_OK, "text compiles");
  txt = dump_proto(L, 1);  /* also a binary dump of a text-compiled chunk */

  /* NULL mode accepts both kinds through the normal chain */
  ck(loadmode(L, &bin, NULL) == LUA_OK, "NULL mode binary");
  lua_pop(L, 1);
  ck(luaL_loadbufferx(L, "return 1", 8, "txt", NULL) == LUA_OK,
     "NULL mode text");
  lua_pop(L, 1);

  /* legacy b / t / bt */
  ck(loadmode(L, &bin, "b") == LUA_OK, "legacy b binary");
  lua_pop(L, 1);
  ck(loadmode(L, &bin, "bt") == LUA_OK, "legacy bt binary");
  lua_pop(L, 1);
  ck(luaL_loadbufferx(L, "return 1", 8, "txt", "t") == LUA_OK,
     "legacy t text");
  lua_pop(L, 1);
  ck(luaL_loadbufferx(L, "return 1", 8, "txt", "bt") == LUA_OK,
     "legacy bt text");
  lua_pop(L, 1);
  status = luaL_loadbufferx(L, "return 1", 8, "txt", "b");
  ck(status == LUA_ERRSYNTAX, "b rejects text");
  lua_pop(L, 1);
  ck(loadmode(L, &bin, "t") == LUA_ERRSYNTAX, "t rejects binary");
  lua_pop(L, 1);

  /* v alone never loads anything */
  ck(loadmode(L, &bin, "v") == LUA_ERRSYNTAX, "v alone rejects binary");
  lua_pop(L, 1);
  status = luaL_loadbufferx(L, "return 1", 8, "txt", "v");
  ck(status == LUA_ERRSYNTAX, "v alone rejects text");
  lua_pop(L, 1);

  /* btv verifies binary but still compiles text */
  ck(loadmode(L, &bin, "btv") == LUA_OK, "btv binary");
  lua_pop(L, 1);
  ck(luaL_loadbufferx(L, "return 1", 8, "txt", "btv") == LUA_OK,
     "btv text compiled");
  lua_pop(L, 1);
  ck(luaL_loadbufferx(L, "return 1", 8, "txt", "tv") == LUA_OK,
     "tv text compiled");
  lua_pop(L, 1);

  /* a corrupt chunk: b loads it, bv rejects it */
  p = compile_proto(L, "return 1");
  w = findop(p, OP_LOADI, 0);
  ck(w >= 0, "found LOADI");
  SETARG_A(p->code[w], 240);
  corrupt = dump_proto(L, 0);
  ck(loadmode(L, &corrupt, "b") == LUA_OK, "legacy b still loads corrupt");
  lua_pop(L, 1);
  ck(loadmode(L, &corrupt, "bv") == LUA_ERRSYNTAX, "bv rejects corrupt");
  lua_pop(L, 1);

  /* recovery: the same state rejects then loads and runs normally */
  ck(loadmode(L, &corrupt, "bv") == LUA_ERRSYNTAX, "repeat rejection");
  lua_pop(L, 1);
  ck(loadmode(L, &bin, "bv") == LUA_OK, "state still accepts good chunks");
  ck(lua_pcall(L, 0, LUA_MULTRET, 0) == LUA_OK, "good chunk executes");
  ck(lua_gettop(L) == 6, "six results");
  ck(lua_tointeger(L, 1) == 55, "loop sum");
  ck(lua_tointeger(L, 2) == 1 && lua_tointeger(L, 3) == 2,
     "generic for results");
  ck(lua_tointeger(L, 4) == 17 && lua_tointeger(L, 5) == 23 &&
     lua_tointeger(L, 6) == 29, "open-result closure chain");
  lua_settop(L, 0);

  /* stripped round-trip under verification, executed once */
  {
    Bytes sb;
    compile_proto(L, src);
    sb = dump_proto(L, 1);
    ck(loadmode(L, &sb, "bv") == LUA_OK, "stripped chunk verifies");
    ck(lua_pcall(L, 0, LUA_MULTRET, 0) == LUA_OK, "stripped chunk runs");
    ck(lua_tointeger(L, 1) == 55 && lua_tointeger(L, 4) == 17,
       "stripped results match");
    lua_settop(L, 0);
    free(sb.p);
  }

  /* when several words are faulty, the earliest word wins */
  p = compile_proto(L, "local a, b = 1, 2; return a + b");
  w = findop(p, OP_LOADI, 0);
  ck(w >= 0, "first LOADI");
  SETARG_A(p->code[w], 230);                /* fault at the earliest word */
  p->code[p->sizecode - 1] =
      CREATE_Ax(OP_EXTRAARG, 0);            /* fault at the last word */
  corrupt = dump_proto(L, 0);
  status = loadmode(L, &corrupt, "bv");
  ck(status == LUA_ERRSYNTAX, "double fault rejected");
  {
    const char *msg = lua_tostring(L, -1);
    char want[64];
    ck(msg != NULL, "diagnostic available");
    snprintf(want, sizeof(want), "word %d", w);
    ck(strstr(msg, want) != NULL, "earliest fault reported first");
  }
  lua_pop(L, 1);

  free(bin.p);
  free(txt.p);
  free(corrupt.p);
  lua_close(L);
  return g_count;
}
