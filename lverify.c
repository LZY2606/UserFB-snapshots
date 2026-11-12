/*
** lverify.c - structural verification of precompiled chunks
** See Copyright Notice in lua.h
**
** This module checks the internal structure of an already loaded
** binary chunk. It is strictly read-only: it never executes the
** bytecode and never modifies it. It is deliberately not a complete
** sandbox: validating the shape of instructions, constants and
** control flow cannot rule out every unsafe runtime behavior.
*/

#define lverify_c
#define LUA_CORE

#include "lprefix.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lobject.h"
#include "lopcodes.h"
#include "ltm.h"
#include "lverify.h"


/*
** Per-word flags. A "boundary" word is the first word of an actual
** instruction; words consumed as EXTRAARG arguments or as metamethod
** fallbacks sit inside another instruction and can never be reached
** by control flow.
*/
#define VF_BOUNDARY	1
#define VF_EXTRA	2   /* EXTRAARG belonging to previous word */
#define VF_MM		4   /* MMBIN/MMBINI/MMBINK fallback word */

#define IS_ARITHREG(op)	((op) >= OP_ADD && (op) <= OP_SHR)
#define IS_ARITHK(op)	((op) >= OP_ADDK && (op) <= OP_BXORK)


typedef struct Verifier {
  lua_State *L;
  Proto *root;
  char *path;       /* current prototype path, root is "0" */
  lu_byte *flags;   /* per-word flags for the current prototype */
  int n;            /* number of words in the current prototype */
} Verifier;


static l_noret verror (Verifier *v, int word, const char *fmt, ...) {
  lua_State *L = v->L;
  va_list argp;
  char msg[256];
  va_start(argp, fmt);
  vsnprintf(msg, sizeof(msg), fmt, argp);
  va_end(argp);
  luaO_pushfstring(L,
      "bad precompiled chunk (proto %s word %d): %s",
      (v->path != NULL) ? v->path : "0", word, msg);
  free(v->flags);
  free(v->path);
  luaD_throw(L, LUA_ERRSYNTAX);
}


/*
** Push or pop a child index on the prototype path. The path of the
** root prototype is "0"; its first child is "0.0" and so on.
*/
static void pushpath (Verifier *v, int child) {
  size_t len = strlen(v->path);
  char extra[32];
  snprintf(extra, sizeof(extra), ".%d", child);
  v->path = cast(char *, realloc(v->path, len + strlen(extra) + 1));
  if (v->path == NULL)
    luaD_throw(v->L, LUA_ERRMEM);
  memcpy(v->path + len, extra, strlen(extra) + 1);
}

static void poppath (Verifier *v) {
  char *dot = strrchr(v->path, '.');
  if (dot != NULL)
    *dot = '\0';
}


static void checkreg (Verifier *v, int word, int r, const char *what) {
  if (r < 0 || r >= v->root->maxstacksize)
    verror(v, word, "register %s=%d out of range (maxstacksize=%d)",
               what, r, v->root->maxstacksize);
}

static void checkregrange (Verifier *v, int word, int first, int last) {
  if (first < 0 || last < first || last >= v->root->maxstacksize)
    verror(v, word,
           "register range R[%d]..R[%d] out of range (maxstacksize=%d)",
           first, last, v->root->maxstacksize);
}

static void checkk (Verifier *v, int word, int idx, const char *what) {
  if (idx < 0 || idx >= v->root->sizek)
    verror(v, word, "constant %s=%d out of range (sizek=%d)",
               what, idx, v->root->sizek);
}

static void checkup (Verifier *v, int word, int idx) {
  if (idx < 0 || idx >= v->root->sizeupvalues)
    verror(v, word, "upvalue index %d out of range (sizeupvalues=%d)",
               idx, v->root->sizeupvalues);
}

static void checkproto (Verifier *v, int word, int idx) {
  if (idx < 0 || idx >= v->root->sizep)
    verror(v, word, "child prototype index %d out of range (sizep=%d)",
               idx, v->root->sizep);
}

/*
** Check an RK() operand: either a register or a constant index.
*/
static void checkRK (Verifier *v, int word, Instruction i, int arg) {
  if (TESTARG_k(i))
    checkk(v, word, arg, "RK");
  else
    checkreg(v, word, arg, "RK");
}

static void checkKstr (Verifier *v, int word, int idx) {
  TValue *o;
  checkk(v, word, idx, "K");
  o = &v->root->k[idx];
  if (!ttisstring(o) || !ttisshrstring(o))
    verror(v, word, "constant K[%d] must be a short string", idx);
}

static void checkKtype (Verifier *v, int word, int idx,
                        int wantinteger, const char *opname) {
  TValue *o;
  checkk(v, word, idx, "K");
  o = &v->root->k[idx];
  if (wantinteger) {
    if (!ttisinteger(o))
      verror(v, word, "%s constant K[%d] must be an integer", opname, idx);
  }
  else {
    if (!ttisnumber(o))
      verror(v, word, "%s constant K[%d] must be a number", opname, idx);
  }
}


/*
** Expected metamethod event for an arithmetic opcode. Register and
** constant variants share the same order (ORDER OP / ORDER TM).
*/
static int eventForReg (OpCode op) {
  return TM_ADD + (op - OP_ADD);
}

static int eventForK (OpCode op) {
  return TM_ADD + (op - OP_ADDK);
}


/*
** Check that a successor stays inside the code and lands on a
** boundary word.
*/
static void checkedgeto (Verifier *v, int from, int target) {
  if (target < 0 || target >= v->n)
    verror(v, from, "control flow edge to word %d falls outside the code "
                    "(code size=%d)", target, v->n);
  if (!(v->flags[target] & VF_BOUNDARY))
    verror(v, from, "control flow edge lands in the middle of the "
                    "multi-word instruction at word %d", target);
}

/*
** The word following a test must be the JMP that performs the actual
** branch.
*/
static void checknextjmp (Verifier *v, int word) {
  if (word + 1 >= v->n)
    verror(v, word, "conditional test is missing its following JMP");
  if (!(v->flags[word + 1] & VF_BOUNDARY) ||
      GET_OPCODE(v->root->code[word + 1]) != OP_JMP)
    verror(v, word, "word following conditional test must be a JMP");
}

/* Both edges of a conditional test: skip the JMP, or take the JMP. */
static void checktestedges (Verifier *v, int word) {
  int jmp = word + 1;
  checknextjmp(v, word);
  checkedgeto(v, word, jmp + 1);   /* test failed: skip the JMP */
  checkedgeto(v, word, jmp + 1 + GETARG_sJ(v->root->code[jmp]));
}

static int isbitwiseK (OpCode op) {
  return op == OP_BANDK || op == OP_BORK || op == OP_BXORK;
}


/*
** Every arithmetic word the compiler emits is paired with the
** metamethod fallback in the very next word; the arithmetic word
** falls through to it when the fast track fails and skips it when it
** succeeds.
*/
static void checkcompanion (Verifier *v, int i) {
  Proto *p = v->root;
  OpCode op = GET_OPCODE(p->code[i]);
  OpCode want;
  if (i + 1 >= p->sizecode)
    verror(v, i, "arithmetic word is missing its metamethod fallback");
  want = GET_OPCODE(p->code[i + 1]);
  if (op >= OP_ADD && op <= OP_SHR) {
    if (want != OP_MMBIN)
      verror(v, i, "register arithmetic word must be followed by MMBIN");
  }
  else if (op == OP_ADDI || op == OP_SHRI || op == OP_SHLI) {
    if (want != OP_MMBINI)
      verror(v, i, "immediate arithmetic word must be followed by MMBINI");
  }
  else {
    if (want != OP_MMBINK)
      verror(v, i, "constant arithmetic word must be followed by MMBINK");
  }
}


/*
** Validate a metamethod fallback word (MMBIN/MMBINI/MMBINK), which is
** an ancillary word of the arithmetic word immediately before it.
*/
static void checkmm (Verifier *v, int i) {
  Proto *p = v->root;
  Instruction inst = p->code[i];
  OpCode op = GET_OPCODE(inst);
  OpCode prev;
  int a = GETARG_A(inst);
  int b = GETARG_B(inst);
  int c = GETARG_C(inst);
  int k = GETARG_k(inst);
  int event = -1;
  int negated = 0;  /* compiler rewrote a subtraction/left shift */

  if (i == 0 || !(v->flags[i - 1] & VF_BOUNDARY))
    verror(v, i, "standalone metamethod fallback word");
  prev = GET_OPCODE(p->code[i - 1]);
  if (c < TM_ADD || c > TM_SHR)
    verror(v, i, "metamethod event %d is outside the arithmetic events", c);
  checkreg(v, i, a, "A");

  switch (op) {
    case OP_MMBIN:
      if (k != 0)
        verror(v, i, "MMBIN must have its k bit clear");
      if (!IS_ARITHREG(prev))
        verror(v, i, "MMBIN must follow a register arithmetic word");
      checkreg(v, i, b, "B");
      if (b != GETARG_C(p->code[i - 1]))
        verror(v, i, "MMBIN B=%d does not match the arithmetic word C=%d",
                   b, GETARG_C(p->code[i - 1]));
      event = eventForReg(prev);
      break;
    case OP_MMBINI:
      if (prev == OP_ADDI) {
        if (c == TM_ADD) event = TM_ADD;  /* plain addition */
        else if (c == TM_SUB) {           /* (r1 + -I) for subtraction */
          event = TM_SUB;
          negated = 1;
        }
        else
          verror(v, i, "MMBINI after ADDI must use event ADD or SUB");
      }
      else if (prev == OP_SHRI) {
        if (c == TM_SHR) event = TM_SHR;  /* plain right shift */
        else if (c == TM_SHL) {           /* (r1 >> -I) for left shift */
          event = TM_SHL;
          negated = 1;
        }
        else
          verror(v, i, "MMBINI after SHRI must use event SHR or SHL");
      }
      else if (prev == OP_SHLI) {
        event = TM_SHL;
      }
      else
        verror(v, i, "MMBINI must follow ADDI, SHRI or SHLI");
      break;
    case OP_MMBINK:
      if (!IS_ARITHK(prev))
        verror(v, i, "MMBINK must follow a constant arithmetic word");
      checkKtype(v, i, b, isbitwiseK(prev), "MMBINK");
      if (b != GETARG_C(p->code[i - 1]))
        verror(v, i, "MMBINK B=%d does not match the arithmetic word C=%d",
                   b, GETARG_C(p->code[i - 1]));
      event = eventForK(prev);
      break;
    default:
      verror(v, i, "internal error in metamethod validation");
  }
  /*
  ** The fallback works over the first operand, which the compiler
  ** places both in the arithmetic word's B and in the MM word's A.
  */
  if (a != GETARG_B(p->code[i - 1]))
    verror(v, i, "fallback A=%d does not match the arithmetic word B=%d",
               a, GETARG_B(p->code[i - 1]));
  if (event != c)
    verror(v, i, "metamethod event %d does not match the arithmetic "
                 "word (event %d)", c, event);
  /* a negated immediate must appear inverted in the fallback word */
  if (negated &&
      GETARG_sB(inst) != -sC2int(GETARG_C(p->code[i - 1])))
    verror(v, i, "negated immediate does not match the fallback operand");
}


/*
** Structural checks for one boundary word. Every word is examined,
** including words that no control-flow edge can reach.
*/
static void checkword (Verifier *v, int i) {
  Proto *p = v->root;
  Instruction inst = p->code[i];
  OpCode op = GET_OPCODE(inst);
  int a = GETARG_A(inst);
  int b = GETARG_B(inst);
  int c = GETARG_C(inst);
  int k = GETARG_k(inst);

  if (op >= NUM_OPCODES)
    verror(v, i, "invalid opcode %d", (int)op);

  switch (op) {
    case OP_MOVE:
      checkreg(v, i, a, "A"); checkreg(v, i, b, "B");
      break;
    case OP_LOADI: case OP_LOADF:
      checkreg(v, i, a, "A");
      break;
    case OP_LOADK:
      checkreg(v, i, a, "A");
      checkk(v, i, GETARG_Bx(inst), "Bx");
      break;
    case OP_LOADKX: {
      checkreg(v, i, a, "A");
      if (i + 1 >= p->sizecode || !(v->flags[i + 1] & VF_EXTRA))
        verror(v, i, "LOADKX must be followed by an EXTRAARG word");
      checkk(v, i, GETARG_Ax(p->code[i + 1]), "Ax");
      break;
    }
    case OP_LOADFALSE: case OP_LOADTRUE:
      checkreg(v, i, a, "A");
      break;
    case OP_LFALSESKIP:
      checkreg(v, i, a, "A");
      if (i + 1 >= v->n || !(v->flags[i + 1] & VF_BOUNDARY))
        verror(v, i, "LFALSESKIP must skip a real following instruction");
      checkedgeto(v, i, i + 2);  /* the skipped word is never entered */
      break;
    case OP_LOADNIL:
      checkregrange(v, i, a, a + b);
      break;
    case OP_GETUPVAL:
      checkreg(v, i, a, "A"); checkup(v, i, b);
      break;
    case OP_SETUPVAL:
      checkreg(v, i, a, "A"); checkup(v, i, b);
      break;
    case OP_GETTABUP:
      checkreg(v, i, a, "A"); checkup(v, i, b); checkKstr(v, i, c);
      break;
    case OP_GETTABLE:
      checkreg(v, i, a, "A"); checkreg(v, i, b, "B");
      checkreg(v, i, c, "C");
      break;
    case OP_GETI:
      checkreg(v, i, a, "A"); checkreg(v, i, b, "B");
      break;
    case OP_GETFIELD:
      checkreg(v, i, a, "A"); checkreg(v, i, b, "B");
      checkKstr(v, i, c);
      break;
    case OP_SETTABUP:
      checkup(v, i, a); checkKstr(v, i, b); checkRK(v, i, inst, c);
      break;
    case OP_SETTABLE:
      checkreg(v, i, a, "A"); checkreg(v, i, b, "B");
      checkRK(v, i, inst, c);
      break;
    case OP_SETI:
      checkreg(v, i, a, "A"); checkRK(v, i, inst, c);
      break;
    case OP_SETFIELD:
      checkreg(v, i, a, "A"); checkKstr(v, i, b);
      checkRK(v, i, inst, c);
      break;
    case OP_NEWTABLE:
      checkreg(v, i, a, "A");
      /* the compiler always emits an EXTRAARG word (Ax may be zero) */
      if (i + 1 >= p->sizecode || !(v->flags[i + 1] & VF_EXTRA))
        verror(v, i, "NEWTABLE must be followed by an EXTRAARG word");
      break;
    case OP_SELF:
      checkreg(v, i, a, "A"); checkregrange(v, i, a, a + 1);
      checkreg(v, i, b, "B"); checkRK(v, i, inst, c);
      break;
    case OP_ADDI: case OP_SHRI: case OP_SHLI:
      checkreg(v, i, a, "A"); checkreg(v, i, b, "B");
      checkcompanion(v, i);
      break;
    case OP_ADDK: case OP_SUBK: case OP_MULK: case OP_MODK:
    case OP_POWK: case OP_DIVK: case OP_IDIVK:
      checkreg(v, i, a, "A"); checkreg(v, i, b, "B");
      checkKtype(v, i, c, 0, "arithmetic");
      checkcompanion(v, i);
      break;
    case OP_BANDK: case OP_BORK: case OP_BXORK:
      checkreg(v, i, a, "A"); checkreg(v, i, b, "B");
      checkKtype(v, i, c, 1, "bitwise");
      checkcompanion(v, i);
      break;
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_MOD:
    case OP_POW: case OP_DIV: case OP_IDIV:
    case OP_BAND: case OP_BOR: case OP_BXOR:
    case OP_SHL: case OP_SHR:
      checkreg(v, i, a, "A"); checkreg(v, i, b, "B");
      checkreg(v, i, c, "C");
      checkcompanion(v, i);
      break;
    case OP_MMBIN: case OP_MMBINI: case OP_MMBINK:
      checkmm(v, i);  /* also reached for malformed standalone words */
      break;
    case OP_UNM: case OP_BNOT: case OP_NOT: case OP_LEN:
      checkreg(v, i, a, "A"); checkreg(v, i, b, "B");
      break;
    case OP_CONCAT:
      if (b < 1)
        verror(v, i, "CONCAT needs at least one operand");
      checkregrange(v, i, a, a + b - 1);
      break;
    case OP_CLOSE: case OP_TBC:
      checkreg(v, i, a, "A");
      break;
    case OP_JMP:
      checkedgeto(v, i, i + 1 + GETARG_sJ(inst));
      break;
    case OP_EQ: case OP_LT: case OP_LE:
      checkreg(v, i, a, "A"); checkreg(v, i, b, "B");
      checktestedges(v, i);
      break;
    case OP_EQK:
      checkreg(v, i, a, "A"); checkk(v, i, b, "B");
      checktestedges(v, i);
      break;
    case OP_EQI: case OP_LTI: case OP_LEI:
    case OP_GTI: case OP_GEI:
      checkreg(v, i, a, "A");
      checktestedges(v, i);
      break;
    case OP_TEST:
      checkreg(v, i, a, "A");
      checktestedges(v, i);
      break;
    case OP_TESTSET:
      checkreg(v, i, a, "A"); checkreg(v, i, b, "B");
      checktestedges(v, i);
      break;
    case OP_CALL:
      checkreg(v, i, a, "A");
      if (b != 0) checkregrange(v, i, a, a + b - 1);
      if (c >= 2) checkregrange(v, i, a, a + c - 2);
      checkedgeto(v, i, i + 1);
      break;
    case OP_TAILCALL:
      checkreg(v, i, a, "A");
      if (b != 0) checkregrange(v, i, a, a + b - 1);
      if (p->is_vararg) {
        if (c != p->numparams + 1)
          verror(v, i, "vararg TAILCALL C must be numparams+1");
      }
      else if (c != 0)
        verror(v, i, "non-vararg TAILCALL C must be zero");
      break;  /* terminal word */
    case OP_RETURN:
      /* B=1 returns zero values, so R[A] is never read and A may
         equal maxstacksize; only fixed results bound the window */
      if (b >= 2) checkregrange(v, i, a, a + b - 2);
      if (p->is_vararg) {
        if (c != p->numparams + 1)
          verror(v, i, "vararg RETURN C must be numparams+1");
      }
      else if (c != 0)
        verror(v, i, "non-vararg RETURN C must be zero");
      break;  /* terminal word */
    case OP_RETURN0:
      break;  /* terminal word */
    case OP_RETURN1:
      checkreg(v, i, a, "A");
      break;  /* terminal word */
    case OP_FORPREP:
      checkregrange(v, i, a, a + 3);
      checkedgeto(v, i, i + 1);                          /* enter loop */
      checkedgeto(v, i, i + 2 + GETARG_Bx(inst));       /* skip loop */
      break;
    case OP_FORLOOP:
      checkregrange(v, i, a, a + 3);
      checkedgeto(v, i, i + 1);                         /* loop ended */
      checkedgeto(v, i, i + 1 - GETARG_Bx(inst));       /* back edge */
      break;
    case OP_TFORPREP: {
      int t = i + 1 + GETARG_Bx(inst);
      checkregrange(v, i, a, a + 3);
      checkedgeto(v, i, t);
      if (GET_OPCODE(p->code[t]) != OP_TFORCALL ||
          GETARG_A(p->code[t]) != a)
        verror(v, i, "TFORPREP must reach TFORCALL on the same A");
      break;
    }
    case OP_TFORCALL:
      if (c < 1)
        verror(v, i, "TFORCALL needs at least one result");
      checkregrange(v, i, a, a + 3 + c);
      if (i + 1 >= v->n || !(v->flags[i + 1] & VF_BOUNDARY) ||
          GET_OPCODE(p->code[i + 1]) != OP_TFORLOOP ||
          GETARG_A(p->code[i + 1]) != a)
        verror(v, i, "TFORCALL must be followed by TFORLOOP on the same A");
      checkedgeto(v, i, i + 1);
      break;
    case OP_TFORLOOP:
      checkregrange(v, i, a, a + 4);
      checkedgeto(v, i, i + 1);
      checkedgeto(v, i, i + 1 - GETARG_Bx(inst));
      break;
    case OP_SETLIST:
      checkreg(v, i, a, "A");
      if (b != 0) checkregrange(v, i, a + 1, a + b);
      if (k != 0) {
        if (i + 1 >= p->sizecode || !(v->flags[i + 1] & VF_EXTRA))
          verror(v, i, "SETLIST with extension bit needs an EXTRAARG word");
      }
      checkedgeto(v, i, i + 1 + (k != 0));
      break;
    case OP_CLOSURE: {
      int child = GETARG_Bx(inst);
      int j;
      checkreg(v, i, a, "A");
      checkproto(v, i, child);
      /* recursive upvalue capture checks against this prototype */
      for (j = 0; j < p->p[child]->sizeupvalues; j++) {
        Upvaldesc *uv = &p->p[child]->upvalues[j];
        if (uv->instack != 0 && uv->instack != 1)
          verror(v, i, "child %d upvalue %d has invalid instack flag",
                     child, j);
        if (uv->instack) {
          if (uv->idx >= p->maxstacksize)
            verror(v, i, "child %d upvalue %d captures register %d out of "
                         "range (maxstacksize=%d)",
                         child, j, uv->idx, p->maxstacksize);
        }
        else {
          if (uv->idx >= p->sizeupvalues)
            verror(v, i, "child %d upvalue %d captures parent upvalue %d "
                         "out of range (sizeupvalues=%d)",
                         child, j, uv->idx, p->sizeupvalues);
        }
      }
      checkedgeto(v, i, i + 1);
      break;
    }
    case OP_VARARG:
      checkreg(v, i, a, "A");
      if (c >= 2) checkregrange(v, i, a, a + c - 2);
      checkedgeto(v, i, i + 1);
      break;
    case OP_VARARGPREP:
      if (i != 0)
        verror(v, i, "VARARGPREP can only be the first word");
      if (!p->is_vararg)
        verror(v, i, "VARARGPREP in a non-vararg prototype");
      if (a != p->numparams)
        verror(v, i, "VARARGPREP A=%d differs from numparams=%d",
                   a, p->numparams);
      checkedgeto(v, i, i + 1);
      break;
    case OP_EXTRAARG:
      verror(v, i, "stray EXTRAARG word");
      break;
  }
}


/*
** Open-result ("top") data flow, see lopcodes.h and lvm.c.
*/
static int openProducer (Instruction i) {
  OpCode op = GET_OPCODE(i);
  if (op == OP_CALL && GETARG_C(i) == 0)
    return 1;
  if (op == OP_VARARG && GETARG_C(i) == 0)
    return 1;
  if (op == OP_TAILCALL)  /* always sets top, see isOT */
    return 1;
  return 0;
}

static int openConsumer (Instruction i) {
  OpCode op = GET_OPCODE(i);
  if (op != OP_CALL && op != OP_TAILCALL &&
      op != OP_RETURN && op != OP_SETLIST)
    return 0;
  return GETARG_B(i) == 0;
}

static int isterminal (OpCode op) {
  return op == OP_TAILCALL || op == OP_RETURN ||
         op == OP_RETURN0 || op == OP_RETURN1;
}


/*
** Build the successor list of a boundary word. Returns the count and
** fills 'targets' (at most 2 targets).
*/
static int successors (Verifier *v, int i, int targets[2]) {
  Instruction inst = v->root->code[i];
  OpCode op = GET_OPCODE(inst);
  int n = 0;
  switch (op) {
    case OP_JMP:
      targets[n++] = i + 1 + GETARG_sJ(inst);
      break;
    case OP_EQ: case OP_LT: case OP_LE:
    case OP_EQK: case OP_EQI: case OP_LTI: case OP_LEI:
    case OP_GTI: case OP_GEI:
    case OP_TEST: case OP_TESTSET: {
      int jmp = i + 1;  /* test failed skips the JMP, else takes it */
      targets[n++] = jmp + 1;
      targets[n++] = jmp + 1 + GETARG_sJ(v->root->code[jmp]);
      break;
    }
    case OP_LFALSESKIP:
      targets[n++] = i + 2;  /* next word is always skipped */
      break;
    case OP_ADDI: case OP_SHRI: case OP_SHLI:
    case OP_ADDK: case OP_SUBK: case OP_MULK: case OP_MODK:
    case OP_POWK: case OP_DIVK: case OP_IDIVK:
    case OP_BANDK: case OP_BORK: case OP_BXORK:
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_MOD:
    case OP_POW: case OP_DIV: case OP_IDIV:
    case OP_BAND: case OP_BOR: case OP_BXOR:
    case OP_SHL: case OP_SHR:
      /* both runtime paths continue after the MMBIN* companion word */
      targets[n++] = i + 2;
      break;
    case OP_FORPREP:
      targets[n++] = i + 1;
      targets[n++] = i + 2 + GETARG_Bx(inst);
      break;
    case OP_FORLOOP:
      targets[n++] = i + 1;
      targets[n++] = i + 1 - GETARG_Bx(inst);
      break;
    case OP_TFORPREP:
      targets[n++] = i + 1 + GETARG_Bx(inst);
      break;
    case OP_TFORCALL:
      targets[n++] = i + 1;
      break;
    case OP_TFORLOOP:
      targets[n++] = i + 1;
      targets[n++] = i + 1 - GETARG_Bx(inst);
      break;
    case OP_LOADKX: case OP_NEWTABLE:
      targets[n++] = i + 2;  /* the next word is an EXTRAARG */
      break;
    case OP_SETLIST:
      targets[n++] = i + 1 + (GETARG_k(inst) != 0);
      break;
    default:
      if (!isterminal(op))
        targets[n++] = i + 1;
      break;
  }
  return n;
}


/*
** Walk the reachable control-flow graph, tracking whether an open
** result is available on each edge. Every ordinary instruction
** invalidates 'top' (see lvm.c), so a B=0 consumer may only be
** entered when every incoming edge carries an open result; a jump
** that bypasses the producer cannot borrow the state of another path.
*/
static void checkflow (Verifier *v) {
  int n = v->n;
  enum { ST_UNSEEN = 2, ST_CLOSED = 0, ST_OPEN = 1 };
  lu_byte *state = cast(lu_byte *, malloc(n));
  int *stack = cast(int *, malloc(sizeof(int) * (n > 0 ? n : 1)));
  int sp = 0;
  int minword = n;
  char reason[160];
  int t;

  if (state == NULL || stack == NULL) {
    free(state); free(stack);
    luaD_throw(v->L, LUA_ERRMEM);
  }
  memset(state, ST_UNSEEN, n);
  reason[0] = '\0';

  #define FLOW_FAIL(word, text) \
    do { if ((word) < minword) { minword = (word); \
         snprintf(reason, sizeof(reason), "%s", (text)); } } while (0)

  state[0] = ST_CLOSED;
  stack[sp++] = 0;
  while (sp > 0) {
    int i = stack[--sp];
    Instruction inst = v->root->code[i];
    int open = state[i];
    int targets[2];
    int nt, j;
    if (openConsumer(inst) && !open)
      FLOW_FAIL(i, "instruction needs open results but no reachable edge "
                   "carries them");
    if (openProducer(inst))
      open = ST_OPEN;
    else if (!openConsumer(inst))
      open = ST_CLOSED;  /* ordinary words reset top */
    nt = successors(v, i, targets);
    for (j = 0; j < nt; j++) {
      t = targets[j];
      if (t < 0 || t >= n || !(v->flags[t] & VF_BOUNDARY))
        FLOW_FAIL(i, "control flow edge leaves the code or enters an "
                     "ancillary word");
      else if (state[t] == ST_UNSEEN) {
        state[t] = cast_byte(open);
        stack[sp++] = t;
      }
      else if (state[t] != open) {
        /* conflicting edges: top is available only when all edges open */
        state[t] = ST_CLOSED;
        stack[sp++] = t;  /* reprocess with the conservative state */
      }
    }
  }
  #undef FLOW_FAIL

  free(state); free(stack);
  if (minword < n)
    verror(v, minword, "%s", reason);
}


/*
** Mark multi-word instructions: their ancillary words are not
** boundaries and cannot be branch targets.
*/
static void buildflags (Verifier *v) {
  Proto *p = v->root;
  int i;
  memset(v->flags, 0, p->sizecode);
  for (i = 0; i < p->sizecode; i++) {
    Instruction inst = p->code[i];
    OpCode op = GET_OPCODE(inst);
    if (op != OP_EXTRAARG)
      v->flags[i] |= VF_BOUNDARY;
    if (op == OP_LOADKX || op == OP_NEWTABLE) {
      if (i + 1 < p->sizecode &&
          GET_OPCODE(p->code[i + 1]) == OP_EXTRAARG)
        v->flags[i + 1] |= VF_EXTRA;
    }
    else if (op == OP_SETLIST && GETARG_k(inst)) {
      if (i + 1 < p->sizecode &&
          GET_OPCODE(p->code[i + 1]) == OP_EXTRAARG)
        v->flags[i + 1] |= VF_EXTRA;
    }
    else if (op == OP_MMBIN || op == OP_MMBINI || op == OP_MMBINK) {
      v->flags[i] &= cast_byte(~VF_BOUNDARY);
      v->flags[i] |= VF_MM;
    }
  }
  /* an EXTRAARG with no owner is a stray word and must be reported */
  for (i = 0; i < p->sizecode; i++) {
    if (GET_OPCODE(p->code[i]) == OP_EXTRAARG &&
        !(v->flags[i] & VF_EXTRA))
      v->flags[i] |= VF_BOUNDARY;
  }
}


/*
** Header-level consistency checks.
*/
static void checkheader (Verifier *v, Proto *p, int rootnup) {
  if (p->sizecode <= 0)
    verror(v, 0, "prototype has no code");
  if (p->maxstacksize == 0 || p->maxstacksize > MAXARG_A)
    verror(v, 0, "maxstacksize=%d is not within 1..%d",
               (int)p->maxstacksize, MAXARG_A);
  if (p->numparams > p->maxstacksize)
    verror(v, 0, "numparams=%d does not fit maxstacksize=%d",
               (int)p->numparams, (int)p->maxstacksize);
  if (p->is_vararg != 0 && p->is_vararg != 1)
    verror(v, 0, "invalid is_vararg flag %d", (int)p->is_vararg);
  if (p->is_vararg) {
    if (GET_OPCODE(p->code[0]) != OP_VARARGPREP)
      verror(v, 0, "vararg prototype must start with VARARGPREP");
  }
  else if (GET_OPCODE(p->code[0]) == OP_VARARGPREP)
    verror(v, 0, "VARARGPREP in a non-vararg prototype");
  if (rootnup >= 0 && p->sizeupvalues != rootnup)
    verror(v, 0, "root prototype declares %d upvalues but the chunk "
                 "header declares %d", p->sizeupvalues, rootnup);
}


static void verifyproto (Verifier *v, Proto *p, int rootnup) {
  int i;
  v->root = p;
  v->n = p->sizecode;
  v->flags = cast(lu_byte *, malloc(p->sizecode));
  if (v->flags == NULL)
    luaD_throw(v->L, LUA_ERRMEM);

  checkheader(v, p, rootnup);
  buildflags(v);

  /*
  ** Single pass in word order: ancillary metamethod words and boundary
  ** words are interleaved as they appear, so the earliest faulty word
  ** wins. Owned EXTRAARG words carry VF_EXTRA and are skipped here.
  */
  for (i = 0; i < p->sizecode; i++) {
    if (v->flags[i] & VF_MM)
      checkmm(v, i);
    else if (v->flags[i] & VF_BOUNDARY)
      checkword(v, i);
  }

  checkflow(v);

  free(v->flags);
  v->flags = NULL;

  /* children are visited in preorder once the parent is sound */
  for (i = 0; i < p->sizep; i++) {
    pushpath(v, i);
    verifyproto(v, p->p[i], -1);
    poppath(v);
  }
}


void luaU_verify (lua_State *L, Proto *f, int chunknup) {
  Verifier v;
  v.L = L;
  v.flags = NULL;
  v.path = cast(char *, malloc(2));
  if (v.path == NULL)
    luaD_throw(L, LUA_ERRMEM);
  v.path[0] = '0';
  v.path[1] = '\0';
  verifyproto(&v, f, chunknup);
  free(v.path);
}
