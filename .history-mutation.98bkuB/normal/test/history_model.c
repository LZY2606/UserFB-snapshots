/**********************************************************************
  history_model.c - model driven capture-history regression (model part)

  This file is an independent, finite-construction reference model.  It
  does not read anything back from Oniguruma and it does not mimic the
  matcher stack code: trees, call plans, expected capture ranges and the
  expected capture-history tree are derived directly from the grammar

      W = a W* b

  and from the declarative semantics of the small plan family used by the
  regression (ordered alternatives, greedy repetition, back references and
  the end anchor).  Every offset is a byte offset.

  Model entry points (file-local; history_contract.c #includes this file):
    static int  hm_selfcheck(int case_id, unsigned seed, char *err, size_t errsz);
    static void hm_plan_build(hm_plan *pl, int kind, int wrapped,
                       unsigned seed, int case_id);
    static void hm_plan_print(const hm_plan *pl, FILE *fp);
**********************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

/* This translation unit is compiled on its own (see baseline/run-target.sh)
 * and is also #included by history_contract.c.  Everything stays file-local:
 * the standalone object only carries local symbols, while the contract TU
 * gets one private copy, so linking the two objects never collides. */
#ifndef HISTORY_MODEL_C_INCLUDED
#define HISTORY_MODEL_C_INCLUDED

#if defined(__GNUC__)
#define HM_UNUSED __attribute__((unused))
#else
#define HM_UNUSED
#endif

#define HM_MAX_NODES   8
#define HM_MAX_BYTES   64
#define HM_PAT_MAX     256
#define HM_IN_MAX      128
#define HM_MAX_REGS    8
#define HM_MAX_HIST    64
#define HM_MAX_EV      160
#define HM_MAX_INST    32
#define HM_MAX_RETR    16
#define HM_TEXT_MAX    1100

#define HM_NOTPOS      (-1)

/* plan kinds */
#define HM_KIND_BRANCH_TEST     0  /* failed probe call, then backref/fallback */
#define HM_KIND_BRANCH_CONTROL  1  /* same plan without the probe call        */
#define HM_KIND_REPEAT          2  /* greedy calls + backref, literal fallback*/
#define HM_KIND_COMBINED        3  /* probe alternative inside the repeat body*/

/* winner identities */
#define HM_WIN_BACKREF   1
#define HM_WIN_FALLBACK  2
#define HM_WIN_REPEAT    3

/* instance status */
#define HM_INST_RETRACTED 0
#define HM_INST_COMMITTED 1

/* traversal event attachment */
#define HM_AT_FIRST 1
#define HM_AT_LAST  2

typedef struct hm_rng_st {
  uint32_t s;
} hm_rng_st;

typedef struct hm_tnode {
  int beg;                 /* byte offset inside the rendered tree */
  int end;
  int parent;              /* -1 for the root */
  int first_child;         /* -1 when leaf */
  int next_sibling;        /* -1 for the last sibling */
  int nchildren;
  int depth;               /* root is depth 0 */
} hm_tnode;

typedef struct hm_tree {
  hm_tnode node[HM_MAX_NODES];
  int      n;
  char     text[HM_MAX_BYTES];
  int      len;
  char     word[2 * HM_MAX_NODES + 1]; /* '(' / ')' bracket notation */
} hm_tree;

typedef struct hm_decl {
  int         id;
  const char *name;
  int         number;     /* group number assigned in declaration order */
  int         is_history; /* nonzero for (?@<...>) groups */
} hm_decl;

typedef struct hm_snap {
  int p_beg, p_end;       /* capture of p when the call starts */
  int f_beg, f_end;       /* capture of f when the call starts */
} hm_snap;

typedef struct hm_inst {
  int      id;            /* instance identity, unique inside a plan */
  int      decl_id;       /* declaration identity (hm_decl.id)      */
  int      beg, end;      /* byte range covered by this call        */
  int      parent_inst;   /* enclosing p instance, -1 at top level  */
  int      status;        /* HM_INST_COMMITTED / HM_INST_RETRACTED  */
  hm_snap  before;        /* capture snapshot before the call       */
} hm_inst;

typedef struct hm_hnode {
  int group;
  int beg, end;
  int parent;             /* flat-array index, 0 is the root */
  int depth;
} hm_hnode;

typedef struct hm_hevent {
  int group;
  int beg, end;
  int depth;
  int at;                 /* HM_AT_FIRST / HM_AT_LAST */
} hm_hevent;

typedef struct hm_range { int beg, end; } hm_range;

typedef struct hm_plan {
  int      kind;
  int      wrapped;
  int      expect_match;
  int      winner;

  hm_decl  decl[4];
  int      ndecl;
  int      id_p, id_f, id_w;
  int      num_p, num_f, num_w; /* group numbers, HM_NOTPOS if absent */

  char     pattern[HM_PAT_MAX];
  char     input[HM_IN_MAX];
  int      ilen;

  hm_range reg[HM_MAX_REGS]; /* expected region, HM_NOTPOS when unset */
  int      nregs;

  hm_hnode  hist[HM_MAX_HIST];
  int       nhist;
  hm_hevent events[HM_MAX_EV];
  int       nevents;

  hm_inst  inst[HM_MAX_INST];
  int      ninst;
  int      retracted[HM_MAX_RETR];
  int      nret;

  char     text[HM_TEXT_MAX]; /* readable call plan */
} hm_plan;

/* ----------------------------- small utilities --------------------------- */

static uint32_t
hm_rng_next(hm_rng_st *r)
{
  /* deterministic 32-bit LCG (Numerical Recipes constants) */
  r->s = r->s * 1664525u + 1013904223u;
  return r->s;
}

static hm_rng_st
hm_rng_make(unsigned seed, int salt, int case_id)
{
  hm_rng_st r;
  r.s = (uint32_t)seed ^ (uint32_t)(salt * 2654435761u)
          ^ ((uint32_t)case_id * 40503u) ^ 0x9e3779b9u;
  if (r.s == 0) r.s = 1;
  /* mix once so nearby seeds diverge immediately */
  hm_rng_next(&r);
  return r;
}

static int
hm_word_valid(const char *w, int len)
{
  int depth = 0;
  for (int i = 0; i < len; i++) {
    if (w[i] == '(') depth++;
    else {
      depth--;
      if (depth < 0) return 0;
    }
  }
  return depth == 0;
}

/* A single ordered tree W = a W* b renders from one root, so its bracket
 * word must be a primitive Dyck word: depth returns to zero only at the
 * final bracket.  Forests such as "()()" describe several trees and are
 * rejected here. */
static int
hm_word_single_root(const char *w, int len)
{
  int depth = 0;
  if (!hm_word_valid(w, len)) return 0;
  for (int i = 0; i < len; i++) {
    if (w[i] == '(') depth++;
    else {
      depth--;
      if (depth == 0 && i != len - 1) return 0;
    }
  }
  return 1;
}

/* Parse a bracket word into first-child / next-sibling nodes. */
static int
hm_tree_parse_word(hm_tree *t, const char *word)
{
  int stack[HM_MAX_NODES];
  int top = 0, n = 0;
  int last_child[HM_MAX_NODES];

  t->n = 0;
  for (int i = 0; i < HM_MAX_NODES; i++) {
    t->node[i].parent = -1;
    t->node[i].first_child = -1;
    t->node[i].next_sibling = -1;
    t->node[i].nchildren = 0;
    last_child[i] = -1;
  }

  for (const char *p = word; *p; p++) {
    if (*p == '(') {
      if (n >= HM_MAX_NODES) return -1;
      int id = n++;
      t->node[id].depth = top;
      if (top > 0) {
        int par = stack[top - 1];
        t->node[id].parent = par;
        if (t->node[par].first_child == -1)
          t->node[par].first_child = id;
        else
          t->node[last_child[par]].next_sibling = id;
        last_child[par] = id;
        t->node[par].nchildren++;
      }
      stack[top++] = id;
    }
    else if (*p == ')') {
      if (top == 0) return -1;
      top--;
    }
  }
  if (top != 0) return -1;
  t->n = n;
  strncpy(t->word, word, (size_t)(2 * HM_MAX_NODES));
  t->word[2 * HM_MAX_NODES] = 0;
  return 0;
}

/* Render W = a W* b while recording byte offsets with a running cursor. */
static void
hm_tree_render_rec(hm_tree *t, int id, int *cursor)
{
  t->node[id].beg = *cursor;
  t->text[(*cursor)++] = 'a';
  for (int c = t->node[id].first_child; c != -1;
       c = t->node[c].next_sibling)
    hm_tree_render_rec(t, c, cursor);
  t->text[(*cursor)++] = 'b';
  t->node[id].end = *cursor;
}

static void
hm_tree_render(hm_tree *t)
{
  int cursor = 0;
  hm_tree_render_rec(t, 0, &cursor);
  t->text[cursor] = 0;
  t->len = cursor;
}

static void
hm_tree_from_word(hm_tree *t, const char *word)
{
  hm_tree_parse_word(t, word);
  hm_tree_render(t);
}

static void
hm_tree_make_leaf(hm_tree *t)
{
  hm_tree_from_word(t, "()");
}

/* Uniform random ordered tree with n nodes: shuffle n pairs of brackets
 * until the word is a valid Dyck word, then parse it. */
static void
hm_tree_random(hm_tree *t, hm_rng_st *r, int n)
{
  char w[2 * HM_MAX_NODES + 1];
  int len = 2 * n;

  if (n < 1) n = 1;
  if (n > HM_MAX_NODES) n = HM_MAX_NODES;
  len = 2 * n;
  for (int i = 0; i < n; i++) { w[i] = '('; w[n + i] = ')'; }
  w[len] = 0;

  do {
    for (int i = len - 1; i > 0; i--) {
      int j = (int)(hm_rng_next(r) % (uint32_t)(i + 1));
      char tmp = w[i]; w[i] = w[j]; w[j] = tmp;
    }
  } while (!hm_word_single_root(w, len));

  hm_tree_from_word(t, w);
}

static int
hm_tree_equal_text(const hm_tree *a, const hm_tree *b)
{
  return a->len == b->len && memcmp(a->text, b->text, (size_t)a->len) == 0;
}

/* Build a tree guaranteed to render differently from `avoid`. */
static void
hm_tree_different(hm_tree *t, hm_rng_st *r, const hm_tree *avoid)
{
  static const char *words[] = {
    "()", "(())", "((()))", "(()())",
    "((()()))", "((())())", "(()()())", "(((())))"
  };
  for (int i = 0; i < (int)(sizeof(words) / sizeof(words[0])); i++) {
    hm_tree cand;
    hm_tree_from_word(&cand, words[i]);
    if (!hm_tree_equal_text(&cand, avoid)) { *t = cand; return; }
  }
  hm_tree_random(t, r, (int)(hm_rng_next(r) % HM_MAX_NODES) + 1);
}

/* ----------------------- declaration tables / plans ---------------------- */

static void
hm_plan_init(hm_plan *pl)
{
  memset(pl, 0, sizeof(*pl));
  pl->id_p = pl->id_f = pl->id_w = HM_NOTPOS;
  pl->num_p = pl->num_f = pl->num_w = HM_NOTPOS;
  pl->winner = 0;
  for (int i = 0; i < HM_MAX_REGS; i++)
    pl->reg[i].beg = pl->reg[i].end = HM_NOTPOS;
}

static void
hm_decl_add(hm_plan *pl, int id, const char *name, int is_history)
{
  hm_decl *d = &pl->decl[pl->ndecl];
  d->id = id;
  d->name = name;
  d->is_history = is_history;
  d->number = pl->ndecl + 1; /* textual declaration order, 1-based */
  if (id == pl->id_p) pl->num_p = d->number;
  if (id == pl->id_f) pl->num_f = d->number;
  if (id == pl->id_w) pl->num_w = d->number;
  pl->ndecl++;
}

static void
hm_build_decls(hm_plan *pl, int wrapped)
{
  pl->wrapped = wrapped;
  pl->id_p = 1;
  pl->id_f = 2;
  if (wrapped) {
    pl->id_w = 3;
    hm_decl_add(pl, pl->id_w, "w", 0);
    hm_decl_add(pl, pl->id_p, "p", 1);
  }
  else {
    hm_decl_add(pl, pl->id_p, "p", 1);
  }
  hm_decl_add(pl, pl->id_f, "f", 1);
}

static const hm_decl *
hm_plan_decl(const hm_plan *pl, int id)
{
  for (int i = 0; i < pl->ndecl; i++)
    if (pl->decl[i].id == id) return &pl->decl[i];
  return NULL;
}

/* --------------------------- expected history ---------------------------- */

static void
hm_hist_append(hm_plan *pl, int group, int beg, int end, int parent, int depth)
{
  int idx = pl->nhist++;
  pl->hist[idx].group = group;
  pl->hist[idx].beg = beg;
  pl->hist[idx].end = end;
  pl->hist[idx].parent = parent;
  pl->hist[idx].depth = depth;
}

/* Append the nodes of one p call (a whole rendered tree) to the history.
 * The root node of the history tree (group 0) always occupies index 0.
 * byte_base is the absolute input offset at which this tree starts; node
 * offsets stored in the tree are relative to the tree itself. */
static int
hm_hist_append_tree(hm_plan *pl, const hm_tree *t, int id, int parent,
                    int base_depth, int byte_base)
{
  int root_idx = pl->nhist;
  hm_hist_append(pl, pl->num_p,
                 byte_base + t->node[id].beg,
                 byte_base + t->node[id].end,
                 parent, base_depth);
  for (int c = t->node[id].first_child; c != -1;
       c = t->node[c].next_sibling)
    hm_hist_append_tree(pl, t, c, root_idx, base_depth + 1, byte_base);
  return root_idx;
}

/* BOTH traversal, depth first: FIRST of a node, the whole subtree, then
 * LAST of the node.  The flat history array is stored in pre-order, so the
 * children of node idx are exactly the later entries whose parent is idx,
 * already in sibling order. */
static void
hm_events_dfs(hm_plan *pl, int idx)
{
  hm_hevent *e;

  e = &pl->events[pl->nevents++];
  e->group = pl->hist[idx].group;
  e->beg = pl->hist[idx].beg;
  e->end = pl->hist[idx].end;
  e->depth = pl->hist[idx].depth;
  e->at = HM_AT_FIRST;

  for (int j = idx + 1; j < pl->nhist; j++)
    if (pl->hist[j].parent == idx)
      hm_events_dfs(pl, j);

  e = &pl->events[pl->nevents++];
  e->group = pl->hist[idx].group;
  e->beg = pl->hist[idx].beg;
  e->end = pl->hist[idx].end;
  e->depth = pl->hist[idx].depth;
  e->at = HM_AT_LAST;
}

static void
hm_events_from_hist(hm_plan *pl)
{
  pl->nevents = 0;
  if (pl->nhist > 0)
    hm_events_dfs(pl, 0); /* index 0 is the group-0 root */
}

/* --------------------------- instance bookkeeping ------------------------ */

static hm_inst *
hm_inst_add(hm_plan *pl, int decl_id, int beg, int end, int parent,
            const hm_snap *before)
{
  hm_inst *in = &pl->inst[pl->ninst];
  in->id = pl->ninst;
  in->decl_id = decl_id;
  in->beg = beg;
  in->end = end;
  in->parent_inst = parent;
  in->status = HM_INST_COMMITTED;
  in->before = *before;
  pl->ninst++;
  return in;
}

static void
hm_inst_retract(hm_plan *pl, int inst_id)
{
  pl->inst[inst_id].status = HM_INST_RETRACTED;
  pl->retracted[pl->nret++] = inst_id;
}

static void
hm_text_cat(char *dst, size_t cap, const char *src)
{
  size_t used = strlen(dst);
  if (used + 1 < cap) strncat(dst, src, cap - used - 1);
}

static void
hm_text_printf(char *dst, size_t cap, const char *fmt, ...)
{
  va_list ap;
  size_t used = strlen(dst);
  va_start(ap, fmt);
  vsnprintf(dst + used, cap - used, fmt, ap);
  va_end(ap);
}

static void
hm_plan_render_text(hm_plan *pl)
{
  pl->text[0] = 0;
  hm_text_printf(pl->text, sizeof(pl->text),
                 "PLAN kind=%s wrapped=%d expect_match=%d winner=%s\n",
                 pl->kind == HM_KIND_BRANCH_TEST ? "branch-test" :
                 pl->kind == HM_KIND_BRANCH_CONTROL ? "branch-control" :
                 pl->kind == HM_KIND_REPEAT ? "repeat" : "combined",
                 pl->wrapped, pl->expect_match,
                 pl->winner == HM_WIN_BACKREF ? "backref" :
                 pl->winner == HM_WIN_FALLBACK ? "fallback" :
                 pl->winner == HM_WIN_REPEAT ? "repeat-main" : "none");
  hm_text_cat(pl->text, sizeof(pl->text), "DECLS");
  for (int i = 0; i < pl->ndecl; i++) {
    const hm_decl *d = &pl->decl[i];
    hm_text_printf(pl->text, sizeof(pl->text),
                   " [id=%d name=%s group=%d history=%d]",
                   d->id, d->name, d->number, d->is_history);
  }
  hm_text_cat(pl->text, sizeof(pl->text), "\nINSTANCES");
  for (int i = 0; i < pl->ninst; i++) {
    const hm_inst *in = &pl->inst[i];
    const hm_decl *d = hm_plan_decl(pl, in->decl_id);
    hm_text_printf(pl->text, sizeof(pl->text),
                   "\n  inst=%d decl=%s(%d) range=(%d,%d) parent=%d %s "
                   "snap-before{p=(%d,%d) f=(%d,%d)}",
                   in->id, d ? d->name : "?", in->decl_id,
                   in->beg, in->end, in->parent_inst,
                   in->status == HM_INST_COMMITTED ? "COMMITTED" : "RETRACTED",
                   in->before.p_beg, in->before.p_end,
                   in->before.f_beg, in->before.f_end);
  }
  hm_text_printf(pl->text, sizeof(pl->text),
                 "\nRETRACTED(%d):", pl->nret);
  for (int i = 0; i < pl->nret; i++)
    hm_text_printf(pl->text, sizeof(pl->text), " %d", pl->retracted[i]);
  hm_text_printf(pl->text, sizeof(pl->text),
                 "\nHISTORY(%d nodes):", pl->nhist);
  for (int i = 0; i < pl->nhist; i++)
    hm_text_printf(pl->text, sizeof(pl->text),
                   " [%d g=%d (%d,%d) par=%d depth=%d]",
                   i, pl->hist[i].group, pl->hist[i].beg, pl->hist[i].end,
                   pl->hist[i].parent, pl->hist[i].depth);
  hm_text_cat(pl->text, sizeof(pl->text), "\n");
}

static HM_UNUSED void
hm_plan_print(const hm_plan *pl, FILE *fp)
{
  fputs(pl->text, fp);
  fprintf(fp, "PATTERN %s\n", pl->pattern);
  fprintf(fp, "INPUT(%d bytes) %s\n", pl->ilen, pl->input);
  fprintf(fp, "EXPECT %s region-regs=%d\n",
          pl->expect_match ? "MATCH" : "NO-MATCH", pl->nregs);
  for (int i = 0; i < pl->nregs; i++)
    fprintf(fp, "  reg[%d]=(%d,%d)\n", i, pl->reg[i].beg, pl->reg[i].end);
}

/* ----------------------------- pattern building -------------------------- */

static void
hm_pattern_p_decl(char *dst, size_t cap, int wrapped)
{
  if (wrapped)
    hm_text_cat(dst, cap, "(?<w>(?@<p>a(?:\\g<p>)*b))");
  else
    hm_text_cat(dst, cap, "(?@<p>a(?:\\g<p>)*b)");
}

/* ------------------------------ branch plans ----------------------------- */

static void
hm_make_branch(hm_plan *pl, int kind, int wrapped, const hm_tree *w,
               const hm_tree *suffix, int suffix_is_w, int negative)
{
  hm_snap snap0 = { HM_NOTPOS, HM_NOTPOS, HM_NOTPOS, HM_NOTPOS };
  hm_snap snap_w;
  hm_inst *wcall;
  int suffix_beg;

  hm_plan_init(pl);
  pl->kind = kind;
  hm_build_decls(pl, wrapped);

  /* input: W then the suffix tree text, optionally a corrupting tail byte */
  memset(pl->input, 0, sizeof(pl->input));
  memcpy(pl->input, w->text, (size_t)w->len);
  memcpy(pl->input + w->len, suffix->text, (size_t)suffix->len);
  pl->ilen = w->len + suffix->len;
  if (negative) pl->input[pl->ilen++] = 'Z';
  suffix_beg = w->len;

  /* pattern */
  pl->pattern[0] = 0;
  hm_text_cat(pl->pattern, sizeof(pl->pattern), "\\A");
  hm_pattern_p_decl(pl->pattern, sizeof(pl->pattern), wrapped);
  hm_text_cat(pl->pattern, sizeof(pl->pattern), "(?:");
  if (kind == HM_KIND_BRANCH_TEST)
    hm_text_cat(pl->pattern, sizeof(pl->pattern), "\\g<p>X|");
  hm_text_cat(pl->pattern, sizeof(pl->pattern), "\\k<p>|(?@<f>");
  hm_text_cat(pl->pattern, sizeof(pl->pattern), suffix->text);
  hm_text_cat(pl->pattern, sizeof(pl->pattern), "))\\z");

  /* instance bookkeeping: the initial W call, then the probe attempt. */
  wcall = hm_inst_add(pl, pl->id_p, 0, w->len, -1, &snap0);
  snap_w = (hm_snap){ 0, w->len, HM_NOTPOS, HM_NOTPOS };
  if (kind == HM_KIND_BRANCH_TEST) {
    /* probe call p covers the suffix tree, then literal X is demanded;
     * X never occurs, so this whole attempt is retracted and the p
     * capture snapshot is restored. */
    hm_inst *probe = hm_inst_add(pl, pl->id_p, suffix_beg,
                                 suffix_beg + suffix->len, -1, &snap_w);
    hm_inst_retract(pl, probe->id);
  }

  if (negative) {
    pl->expect_match = 0;
    pl->winner = 0;
  }
  else if (suffix_is_w) {
    /* back reference to the restored W capture eats the suffix */
    pl->expect_match = 1;
    pl->winner = HM_WIN_BACKREF;
  }
  else {
    /* back reference fails; the literal fallback group f wins */
    pl->expect_match = 1;
    pl->winner = HM_WIN_FALLBACK;
  }

  if (pl->expect_match) {
    /* group 0 + declarations; wrapper and p ranges */
    pl->nregs = pl->ndecl + 1;
    pl->reg[0] = (hm_range){0, pl->ilen};
    pl->reg[pl->num_p] = (hm_range){0, w->len};
    if (pl->num_w != HM_NOTPOS)
      pl->reg[pl->num_w] = (hm_range){0, w->len};
    if (pl->winner == HM_WIN_FALLBACK)
      pl->reg[pl->num_f] = (hm_range){suffix_beg, pl->ilen};

    /* history root, then the W call tree; f node only on fallback.
     * The retracted probe and the back reference add no history node. */
    hm_hist_append(pl, 0, 0, pl->ilen, -1, 0);
    hm_hist_append_tree(pl, w, 0, 0, 1, 0);
    if (pl->winner == HM_WIN_FALLBACK)
      hm_hist_append(pl, pl->num_f, suffix_beg, pl->ilen, 0, 1);
    hm_events_from_hist(pl);
  }
  (void)wcall;
  hm_plan_render_text(pl);
}

/* ------------------------------ repeat plans ----------------------------- */

static void
hm_make_repeat(hm_plan *pl, int kind, int wrapped, const hm_tree *w,
               hm_tree *u[], int k, int negative)
{
  hm_snap snap0 = { HM_NOTPOS, HM_NOTPOS, HM_NOTPOS, HM_NOTPOS };
  hm_snap cur;
  int suffix_beg = w->len;
  int suffix_len = 0;
  int offsets[HM_MAX_NODES]; /* start offset of each U tree */
  int main_wins;
  int retained;

  hm_plan_init(pl);
  pl->kind = kind;
  hm_build_decls(pl, wrapped);

  for (int i = 0; i < k; i++) {
    offsets[i] = w->len + suffix_len;
    suffix_len += u[i]->len;
  }

  memset(pl->input, 0, sizeof(pl->input));
  memcpy(pl->input, w->text, (size_t)w->len);
  for (int i = 0; i < k; i++)
    memcpy(pl->input + offsets[i], u[i]->text, (size_t)u[i]->len);
  pl->ilen = w->len + suffix_len;
  if (negative) pl->input[pl->ilen++] = 'Z';

  /* literal fallback text = the whole suffix */
  pl->pattern[0] = 0;
  hm_text_cat(pl->pattern, sizeof(pl->pattern), "\\A");
  hm_pattern_p_decl(pl->pattern, sizeof(pl->pattern), wrapped);
  hm_text_cat(pl->pattern, sizeof(pl->pattern), "(?:");
  if (kind == HM_KIND_COMBINED)
    hm_text_cat(pl->pattern, sizeof(pl->pattern), "(?:\\g<p>X|\\g<p>)+");
  else
    hm_text_cat(pl->pattern, sizeof(pl->pattern), "(\\g<p>)+");
  hm_text_cat(pl->pattern, sizeof(pl->pattern), "\\k<p>\\z|(?@<f>");
  for (int i = 0; i < k; i++)
    hm_text_cat(pl->pattern, sizeof(pl->pattern), u[i]->text);
  hm_text_cat(pl->pattern, sizeof(pl->pattern), "))\\z");

  /* initial W call */
  hm_inst_add(pl, pl->id_p, 0, w->len, -1, &snap0);
  cur = (hm_snap){ 0, w->len, HM_NOTPOS, HM_NOTPOS };

  /* greedy calls: one instance per suffix tree; combined plans also log a
   * retracted probe attempt for every iteration (X is never present). */
  for (int i = 0; i < k; i++) {
    if (kind == HM_KIND_COMBINED) {
      hm_inst *probe = hm_inst_add(pl, pl->id_p, offsets[i],
                                   offsets[i] + u[i]->len, -1, &cur);
      hm_inst_retract(pl, probe->id);
    }
    hm_inst_add(pl, pl->id_p, offsets[i], offsets[i] + u[i]->len, -1, &cur);
    cur.p_beg = offsets[i];
    cur.p_end = offsets[i] + u[i]->len;
  }

  /* Model semantics, derived from the suffix content:
   * after greedy consumption the back reference faces an empty remainder,
   * so calls are unwound one at a time.  The repeat is one-or-more, so it
   * can never unwind below one surviving call.  A back reference can match
   * only a single tree-sized remainder, hence only the first unwind can
   * succeed: it needs k >= 2 calls and U[k-1] == U[k-2].  With k == 1 the
   * single call would have to vanish entirely, so the whole repeat branch
   * fails and the literal fallback wins instead. */
  main_wins = (k >= 2) && hm_tree_equal_text(u[k - 1], u[k - 2]);
  retained = main_wins ? k - 1 : 0;

  if (negative) {
    pl->expect_match = 0;
    pl->winner = 0;
    /* every attempted call is undone before the failing alternative ends */
    for (int id = 1; id < pl->ninst; id++)
      if (pl->inst[id].status == HM_INST_COMMITTED &&
          pl->inst[id].decl_id == pl->id_p)
        hm_inst_retract(pl, id);
  }
  else if (main_wins) {
    pl->expect_match = 1;
    pl->winner = HM_WIN_REPEAT;
    /* the last suffix tree is consumed by the back reference: retract its
     * call; retained calls stay committed. */
    for (int id = pl->ninst - 1; id >= 1; id--) {
      if (pl->inst[id].decl_id == pl->id_p &&
          pl->inst[id].status == HM_INST_COMMITTED) {
        hm_inst_retract(pl, id);
        break;
      }
    }
  }
  else {
    pl->expect_match = 1;
    pl->winner = HM_WIN_FALLBACK;
    /* whole repeat alternative fails: all repeat calls retracted */
    for (int id = 1; id < pl->ninst; id++)
      if (pl->inst[id].status == HM_INST_COMMITTED)
        hm_inst_retract(pl, id);
  }

  if (pl->expect_match) {
    pl->nregs = pl->ndecl + 1;
    pl->reg[0] = (hm_range){0, pl->ilen};
    if (pl->num_w != HM_NOTPOS)
      pl->reg[pl->num_w] = (hm_range){0, w->len};

    hm_hist_append(pl, 0, 0, pl->ilen, -1, 0);
    hm_hist_append_tree(pl, w, 0, 0, 1, 0);

    if (pl->winner == HM_WIN_REPEAT) {
      /* p ends as the last retained call (W itself when k == 1) */
      if (retained == 0)
        pl->reg[pl->num_p] = (hm_range){0, w->len};
      else {
        int idx = retained - 1;
        pl->reg[pl->num_p] = (hm_range){offsets[idx],
                                        offsets[idx] + u[idx]->len};
      }
      for (int i = 0; i < retained; i++)
        hm_hist_append_tree(pl, u[i], 0, 0, 1, offsets[i]);
      /* back reference itself adds no history node */
    }
    else {
      /* fallback: restored p capture is W; f spans the whole suffix */
      pl->reg[pl->num_p] = (hm_range){0, w->len};
      pl->reg[pl->num_f] = (hm_range){suffix_beg, pl->ilen};
      hm_hist_append(pl, pl->num_f, suffix_beg, pl->ilen, 0, 1);
    }
    hm_events_from_hist(pl);
  }
  hm_plan_render_text(pl);
}

/* ------------------------- per case parameterization --------------------- */

static void
hm_pick_trees(unsigned seed, int case_id, int salt, hm_tree *w, hm_tree *v,
              hm_tree *u[], int *k_out)
{
  hm_rng_st r = hm_rng_make(seed, salt, case_id);
  int n = 1 + (int)(hm_rng_next(&r) % 5);
  int mode = case_id % 4;
  int k = 1 + (case_id % 3);

  /* fixed shapes first so the required nested/sibling variety is guaranteed
   * regardless of the stream from the LCG */
  if (case_id % 7 == 0)
    hm_tree_from_word(w, "()");                 /* leaf */
  else if (case_id % 7 == 1)
    hm_tree_from_word(w, "((()))");             /* purely nested */
  else if (case_id % 7 == 2)
    hm_tree_from_word(w, "(()()())");           /* one root, sibling children */
  else if (case_id % 7 == 3)
    hm_tree_from_word(w, "(()())");             /* nested with siblings */
  else
    hm_tree_random(w, &r, n);

  hm_tree_different(v, &r, w);

  for (int i = 0; i < k; i++) {
    if (mode == 0)
      *u[i] = *w;                               /* all equal */
    else if (mode == 1 && i == k - 1)
      *u[i] = *v;                               /* last one differs */
    else if (mode == 2 && i == 0)
      *u[i] = *v;                               /* first one differs */
    else if (mode == 3)
      hm_tree_random(u[i], &r, 1 + (int)(hm_rng_next(&r) % 4));
    else
      *u[i] = *w;
  }
  *k_out = k;
}

static HM_UNUSED void
hm_plan_build(hm_plan *pl, int kind, int wrapped, unsigned seed, int case_id)
{
  hm_tree w, v;
  hm_tree ustore[3];
  hm_tree *u[3] = { &ustore[0], &ustore[1], &ustore[2] };
  int k;
  /* test/control and repeat/combined pairs must reason about the same
   * trees, so the RNG salt depends on the plan family, not the kind. */
  int family_salt = (kind <= HM_KIND_BRANCH_CONTROL) ? 5 : 23;

  hm_pick_trees(seed, case_id, family_salt, &w, &v, u, &k);

  switch (kind) {
  case HM_KIND_BRANCH_TEST:
  case HM_KIND_BRANCH_CONTROL:
    {
      int use_backref = (case_id % 3 != 1); /* every third case is fallback */
      const hm_tree *suffix = use_backref ? &w : &v;
      int negative = (case_id % 6 == 4);
      hm_make_branch(pl, kind, wrapped, &w, suffix, use_backref, negative);
    }
    break;
  case HM_KIND_REPEAT:
  case HM_KIND_COMBINED:
    {
      int negative = (case_id % 5 == 4);
      hm_make_repeat(pl, kind, wrapped, &w, u, k, negative);
    }
    break;
  default:
    hm_plan_init(pl);
    break;
  }
}

/* ------------------------------- self check ------------------------------ */

/* Human-readable, hand-computed examples.  The constants below are written
 * out explicitly; the model is asked to compute the same objects and every
 * constant is then compared against the model result. */

static int
hm_check(int cond, const char *what, char *err, size_t errsz, int *count)
{
  (*count)++;
  if (!cond) {
    snprintf(err, errsz, "model selfcheck failed: %s", what);
    return 0;
  }
  return 1;
}

static HM_UNUSED int
hm_selfcheck(int case_id, unsigned seed, char *err, size_t errsz)
{
  int count = 0;
  hm_tree t;
  hm_plan pl;

  err[0] = 0;

  /* --- hand example 1: leaf W = ab ------------------------------------- */
  hm_tree_make_leaf(&t);
  if (!hm_check(t.n == 1, "leaf node count", err, errsz, &count)) return -1;
  if (!hm_check(t.len == 2 && t.text[0] == 'a' && t.text[1] == 'b',
                "leaf renders to ab", err, errsz, &count)) return -1;
  if (!hm_check(t.node[0].beg == 0 && t.node[0].end == 2,
                "leaf offsets (0,2)", err, errsz, &count)) return -1;

  /* --- hand example 2: W = aabb, word (()) ----------------------------- */
  hm_tree_from_word(&t, "(())");
  if (!hm_check(t.n == 2 && t.len == 4, "aabb size", err, errsz, &count))
    return -1;
  if (!hm_check(memcmp(t.text, "aabb", 4) == 0, "aabb text",
                err, errsz, &count)) return -1;
  if (!hm_check(t.node[0].beg == 0 && t.node[0].end == 4,
                "aabb root (0,4)", err, errsz, &count)) return -1;
  if (!hm_check(t.node[1].beg == 1 && t.node[1].end == 3 &&
                t.node[1].parent == 0,
                "aabb child (1,3) under root", err, errsz, &count))
    return -1;

  /* --- hand example 3: W = aababb, word (()()): nested node with two
   * sibling children, offsets (1,3) and (3,5) --------------------------- */
  hm_tree_from_word(&t, "(()())");
  if (!hm_check(t.n == 3 && t.len == 6, "aababb size", err, errsz, &count))
    return -1;
  if (!hm_check(memcmp(t.text, "aababb", 6) == 0, "aababb text",
                err, errsz, &count)) return -1;
  if (!hm_check(t.node[0].nchildren == 2, "root has two siblings",
                err, errsz, &count)) return -1;
  if (!hm_check(t.node[1].beg == 1 && t.node[1].end == 3,
                "first sibling (1,3)", err, errsz, &count)) return -1;
  if (!hm_check(t.node[2].beg == 3 && t.node[2].end == 5 &&
                t.node[2].next_sibling == -1 &&
                t.node[1].next_sibling == 2,
                "second sibling (3,5) and sibling order",
                err, errsz, &count)) return -1;

  /* --- hand example 4: branch plan on aabbaabb / aabbab ---------------- */
  {
    hm_tree w, ab;
    hm_tree *us[3];
    int dummy;
    hm_tree_from_word(&w, "(())");     /* aabb */
    hm_tree_from_word(&ab, "()");      /* ab   */

    hm_plan_build(&pl, HM_KIND_BRANCH_TEST, 0, seed,
                  1000 + (case_id % 32));
    /* explicit hand-built plan: W=aabb, suffix aabb -> back reference */
    hm_make_branch(&pl, HM_KIND_BRANCH_TEST, 0, &w, &w, 1, 0);
    if (!hm_check(pl.expect_match && pl.winner == HM_WIN_BACKREF,
                  "aabbaabb backref winner", err, errsz, &count)) return -1;
    if (!hm_check(pl.reg[pl.num_p].beg == 0 && pl.reg[pl.num_p].end == 4,
                  "aabbaabb restored p=(0,4)", err, errsz, &count))
      return -1;
    if (!hm_check(pl.reg[pl.num_f].beg == HM_NOTPOS,
                  "aabbaabb f unset, no new history node",
                  err, errsz, &count)) return -1;
    if (!hm_check(pl.nhist == 3 /* root + root p + child p */,
                  "aabbaabb history node count", err, errsz, &count))
      return -1;
    if (!hm_check(pl.nret == 1 && pl.inst[pl.retracted[0]].end == 8,
                  "aabbaabb probe retracted after completing",
                  err, errsz, &count)) return -1;

    hm_make_branch(&pl, HM_KIND_BRANCH_TEST, 0, &w, &ab, 0, 0);
    if (!hm_check(pl.expect_match && pl.winner == HM_WIN_FALLBACK,
                  "aabbab fallback winner", err, errsz, &count)) return -1;
    if (!hm_check(pl.reg[pl.num_f].beg == 4 && pl.reg[pl.num_f].end == 6,
                  "aabbab f=(4,6)", err, errsz, &count)) return -1;
    if (!hm_check(pl.nhist == 4 /* root, W tree x2, f */,
                  "aabbab history includes f node",
                  err, errsz, &count)) return -1;
    (void)us; (void)dummy;
  }

  /* --- hand example 5: repeat W U U ------------------------------------- */
  {
    hm_tree w, v, uu[3];
    hm_tree *u[3] = { &uu[0], &uu[1], &uu[2] };
    hm_tree_from_word(&w, "(())"); /* aabb */
    hm_tree_from_word(&v, "()");   /* ab   */

    /* equal chain: W U U, last U consumed by back reference */
    uu[0] = w; uu[1] = w;
    hm_make_repeat(&pl, HM_KIND_REPEAT, 0, &w, u, 2, 0);
    if (!hm_check(pl.winner == HM_WIN_REPEAT, "WUU repeat winner",
                  err, errsz, &count)) return -1;
    if (!hm_check(pl.nhist == 5 /* root + W(2) + U1(2) */,
                  "WUU keeps W and first U call trees only",
                  err, errsz, &count)) return -1;
    if (!hm_check(pl.nret == 1, "WUU only the last call is retracted",
                  err, errsz, &count)) return -1;
    if (!hm_check(pl.reg[pl.num_p].beg == 4 && pl.reg[pl.num_p].end == 8,
                  "WUU p ends at first U (4,8)", err, errsz, &count))
      return -1;

    /* unequal last tree: whole repeat branch unwinds, f takes the suffix */
    uu[0] = w; uu[1] = v;
    hm_make_repeat(&pl, HM_KIND_REPEAT, 0, &w, u, 2, 0);
    if (!hm_check(pl.winner == HM_WIN_FALLBACK, "WUV fallback winner",
                  err, errsz, &count)) return -1;
    if (!hm_check(pl.nret == 2, "WUV both repeat calls retracted",
                  err, errsz, &count)) return -1;
    if (!hm_check(pl.reg[pl.num_f].beg == 4 && pl.reg[pl.num_f].end == 10,
                  "WUV f spans full suffix (4,10)", err, errsz, &count))
      return -1;
    if (!hm_check(pl.reg[pl.num_p].beg == 0 && pl.reg[pl.num_p].end == 4,
                  "WUV p restored to W (0,4)", err, errsz, &count))
      return -1;

    /* single suffix tree: the lone repeat call cannot be reduced below one
     * iteration, so even W == U the main branch fails and f wins, matching
     * the whole suffix itself. */
    uu[0] = w;
    hm_make_repeat(&pl, HM_KIND_REPEAT, 0, &w, u, 1, 0);
    if (!hm_check(pl.winner == HM_WIN_FALLBACK,
                  "WU (k=1) falls back because + needs >=1 call",
                  err, errsz, &count)) return -1;
    if (!hm_check(pl.nret == 1, "WU sole repeat call retracted",
                  err, errsz, &count)) return -1;
    if (!hm_check(pl.reg[pl.num_f].beg == 4 && pl.reg[pl.num_f].end == 8,
                  "WU f spans the suffix (4,8)", err, errsz, &count))
      return -1;
    if (!hm_check(pl.reg[pl.num_p].beg == 0 && pl.reg[pl.num_p].end == 4,
                  "WU p restored to W (0,4)", err, errsz, &count))
      return -1;
  }

  /* --- hand example 6: wrapper renumbering, never a history group ------- */
  {
    hm_tree w;
    hm_tree_from_word(&w, "()");
    hm_make_branch(&pl, HM_KIND_BRANCH_CONTROL, 1, &w, &w, 1, 0);
    if (!hm_check(pl.num_w == 1 && pl.num_p == 2 && pl.num_f == 3,
                  "wrapper numbering w=1 p=2 f=3", err, errsz, &count))
      return -1;
    {
      int wrapper_in_history = 0;
      for (int i = 0; i < pl.nhist; i++)
        if (pl.hist[i].group == pl.num_w) wrapper_in_history = 1;
      if (!hm_check(!wrapper_in_history,
                    "plain wrapper never enters the history tree",
                    err, errsz, &count)) return -1;
    }
    if (!hm_check(pl.nregs == 4 && pl.reg[1].beg == 0 && pl.reg[1].end == 2,
                  "wrapper ordinary capture (0,2)", err, errsz, &count))
      return -1;
  }

  /* --- generated-case invariants ---------------------------------------- */
  {
    hm_rng_st r = hm_rng_make(seed, 77, case_id);
    hm_tree g;
    int saw_nested = 0, saw_sibling = 0;
    for (int round = 0; round < 32; round++) {
      int n = 1 + (int)(hm_rng_next(&r) % HM_MAX_NODES);
      hm_tree_random(&g, &r, n);
      if (!hm_check(g.n == n, "generated node count", err, errsz, &count))
        return -1;
      if (!hm_check(g.len == 2 * n, "rendering uses two bytes per node",
                    err, errsz, &count)) return -1;
      if (!hm_check(g.text[0] == 'a' && g.text[g.len - 1] == 'b',
                    "rendering is a...b", err, errsz, &count)) return -1;
      /* every node range stays inside the string and tiles it */
      for (int i = 0; i < g.n; i++) {
        if (!hm_check(g.node[i].beg >= 0 && g.node[i].end <= g.len &&
                      g.node[i].beg < g.node[i].end,
                      "node offsets in range and ordered",
                      err, errsz, &count)) return -1;
      }
      if (g.node[0].nchildren > 0) saw_nested = 1;
      for (int i = 0; i < g.n; i++)
        if (g.node[i].nchildren >= 2) saw_sibling = 1;
    }
    if (!hm_check(saw_nested, "generated set contains nesting",
                  err, errsz, &count)) return -1;
    if (!hm_check(saw_sibling, "generated set contains siblings",
                  err, errsz, &count)) return -1;
  }

  /* --- every plan kind builds, stays in bounds, and keeps event parity -- */
  for (int kind = 0; kind < 4; kind++) {
    for (int wrapped = 0; wrapped < 2; wrapped++) {
      hm_plan_build(&pl, kind, wrapped, seed, case_id);
      if (!hm_check(pl.ilen > 0 && pl.ilen < HM_IN_MAX,
                    "plan input in bounds", err, errsz, &count)) return -1;
      if (!hm_check(strlen(pl.pattern) < HM_PAT_MAX,
                    "plan pattern in bounds", err, errsz, &count)) return -1;
      if (!hm_check(pl.ninst > 0 && pl.ninst < HM_MAX_INST,
                    "plan records instances", err, errsz, &count)) return -1;
      if (pl.expect_match) {
        if (!hm_check(pl.nhist > 0 && pl.nevents == 2 * pl.nhist,
                      "BOTH events expand to two per history node",
                      err, errsz, &count)) return -1;
        if (!hm_check(pl.hist[0].group == 0 && pl.hist[0].parent == -1,
                      "history root is group 0", err, errsz, &count))
          return -1;
      }
    }
  }

  return count;
}

#endif /* HISTORY_MODEL_C_INCLUDED */
