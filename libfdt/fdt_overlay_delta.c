// SPDX-License-Identifier: (GPL-2.0-or-later OR BSD-2-Clause)
/*
 * libfdt - Flat Device Tree manipulation
 *	Generate a device tree overlay (DTBO) describing the difference
 *	between two complete device tree blobs.
 *
 * The generated overlay uses the same mechanisms as dtc-produced
 * overlays (fragments with target-path, /__fixups__, /__local_fixups__
 * and /__symbols__) so it can be applied with fdt_overlay_apply().
 *
 * Only explicitly annotated property cells (see struct fdt_delta_ref)
 * are interpreted as phandle references; this file does not attempt to
 * understand device tree bindings.  Everything else is compared and
 * copied as plain bytes.
 */
#include "libfdt_env.h"

#include <fdt.h>
#include <libfdt.h>
#include <fdt_overlay_delta.h>

#include "libfdt_internal.h"

/* A reference location, resolved against the tree it describes */
struct od_ref {
	char *path;		/* absolute path of the node holding the property */
	char *prop;		/* property name */
	uint32_t cell;		/* cell offset within the property value */
	char *target;		/* absolute path of the referenced node */
};

/* A property to emit into the overlay */
struct od_prop {
	char *name;
	unsigned char *data;	/* value bytes (patched for references) */
	int len;
	int assign;		/* value is an assigned local phandle for the
				   node owning the property */
};

/* A node added by the desired tree */
struct od_node {
	char *name;		/* node name (last path component) */
	char *path;		/* absolute path in the desired tree */
	char *fragpath;		/* absolute path of the fragment target */
	struct od_prop *props;
	int nprops, aprops;
	struct od_node **kids;
	int nkids, akids;
	int want_ph;		/* desired node has a "phandle" property */
	int want_lph;		/* desired node has a "linux,phandle" property */
};

/* One fragment: changes anchored at a pre-existing base node */
struct od_frag {
	char *path;		/* absolute target path */
	struct od_prop *props;	/* added/overridden properties, sorted */
	int nprops, aprops;
	struct od_node **kids;	/* added direct subnodes, sorted */
	int nkids, akids;
};

/* A phandle number assignment for the overlay */
struct od_assign {
	char *path;		/* absolute path of the node */
	uint32_t ph;		/* assigned local phandle */
	uint8_t isnew;		/* node is added by the desired tree */
	uint8_t referenced;	/* some annotated reference points here */
};

/* A label introduced by the desired tree's /__symbols__ */
struct od_symbol {
	char *label;
	char *path;		/* absolute path of the target node */
	char *ovpath;		/* corresponding path inside the overlay */
};

/* One /__fixups__ entry: "label" -> "path:prop:offset" */
struct od_extfix {
	char *label;
	char *entry;
};

/* /__local_fixups__ offsets for one property of one overlay node */
struct od_locfix {
	char *nodepath;		/* absolute path of the node in the overlay */
	char *prop;
	uint32_t *offs;		/* byte offsets within the property value */
	int noffs, aoffs;
};

struct od_ctx {
	const void *base;
	const void *desired;

	struct od_ref *brefs;
	int nbrefs;
	struct od_ref *drefs;
	int ndrefs;

	struct od_frag *frags;
	int nfrags, afrags;

	struct od_assign *assign;	/* sorted by path */
	int nassign, aassign;

	struct od_symbol *syms;
	int nsyms, asyms;

	struct od_extfix *extfix;
	int nextfix, aextfix;
	struct od_locfix *locfix;	/* in tree walk order */
	int nlocfix, alocfix;

	void *fdt;		/* overlay being written, NULL while sizing */
	size_t size;		/* size accounting for the sizing pass */

	char *errpath;
	size_t errpath_size;
};

/**********************************************************************/
/* Small helpers                                                      */
/**********************************************************************/

/* Record an error location and return the (negative) error code */
static int od_fail(struct od_ctx *ctx, int code, const char *const *parts)
{
	if (ctx->errpath && ctx->errpath_size) {
		size_t left = ctx->errpath_size - 1;
		size_t n = 0;
		int i;

		ctx->errpath[0] = '\0';
		for (i = 0; parts && parts[i] && left; i++) {
			size_t l = strlen(parts[i]);

			if (l > left)
				l = left;
			memcpy(ctx->errpath + n, parts[i], l);
			n += l;
			left -= l;
		}
		ctx->errpath[n] = '\0';
	}
	return code;
}

#define OD_FAIL(ctx, code, ...) \
	od_fail((ctx), (code), (const char *const []){ __VA_ARGS__, NULL })

static char *od_strdup(const char *s)
{
	size_t len = strlen(s) + 1;
	char *p = malloc(len);

	if (p)
		memcpy(p, s, len);
	return p;
}

/* Grow an array to hold at least @need elements of @elemsz bytes */
static int od_grow(void **arr, size_t elemsz, int *cap, int need)
{
	void *p;
	int ncap;

	if (need <= *cap)
		return 0;
	ncap = *cap ? *cap : 8;
	while (ncap < need) {
		if (ncap > INT_MAX / 2)
			return -FDT_ERR_NOSPACE;
		ncap *= 2;
	}
	if (elemsz && (size_t)ncap > SIZE_MAX / elemsz)
		return -FDT_ERR_NOSPACE;
	p = realloc(*arr, elemsz * (size_t)ncap);
	if (!p)
		return -FDT_ERR_NOSPACE;
	*arr = p;
	*cap = ncap;
	return 0;
}

/* Write the decimal representation of v into buf (must hold 11 bytes) */
static void od_u32str(uint32_t v, char *buf)
{
	char tmp[10];
	int n = 0, i = 0;

	do {
		tmp[n++] = '0' + (v % 10);
		v /= 10;
	} while (v);
	while (n)
		buf[i++] = tmp[--n];
	buf[i] = '\0';
}

static int od_ranges_overlap(const void *a, size_t alen,
			     const void *b, size_t blen)
{
	uintptr_t ap = (uintptr_t)a, bp = (uintptr_t)b;

	if (!alen || !blen)
		return 0;
	if (ap > UINTPTR_MAX - alen || bp > UINTPTR_MAX - blen)
		return 1;
	return (ap < bp + blen) && (bp < ap + alen);
}

/* Return a malloc'd absolute path for the node at @offset */
static char *od_node_path(const void *fdt, int offset, int *err)
{
	int size = 64;
	int ret;

	for (;;) {
		char *buf = malloc(size);

		if (!buf) {
			*err = -FDT_ERR_NOSPACE;
			return NULL;
		}
		ret = fdt_get_path(fdt, offset, buf, size);
		if (!ret)
			return buf;
		free(buf);
		if (ret != -FDT_ERR_NOSPACE) {
			*err = ret;
			return NULL;
		}
		if (size > INT_MAX / 2) {
			*err = -FDT_ERR_NOSPACE;
			return NULL;
		}
		size *= 2;
	}
}

/* Return a malloc'd "parent/name" path (parent may be "/") */
static char *od_join_path(const char *parent, const char *name)
{
	size_t pl = strlen(parent), nl = strlen(name);
	int slash = (pl > 1);	/* root needs no extra slash */
	char *s = malloc(pl + slash + nl + 1);

	if (!s)
		return NULL;
	memcpy(s, parent, pl);
	if (slash)
		s[pl++] = '/';
	memcpy(s + pl, name, nl + 1);
	return s;
}

static int od_cmp_prop(const void *a, const void *b)
{
	return strcmp(((const struct od_prop *)a)->name,
		      ((const struct od_prop *)b)->name);
}

static int od_cmp_nodep(const void *a, const void *b)
{
	return strcmp((*(struct od_node *const *)a)->name,
		      (*(struct od_node *const *)b)->name);
}

static int od_cmp_assign(const void *a, const void *b)
{
	return strcmp(((const struct od_assign *)a)->path,
		      ((const struct od_assign *)b)->path);
}

static int od_cmp_assign_str(const void *key, const void *elem)
{
	return strcmp((const char *)key,
		      ((const struct od_assign *)elem)->path);
}

static int od_cmp_frag(const void *a, const void *b)
{
	return strcmp(((const struct od_frag *)a)->path,
		      ((const struct od_frag *)b)->path);
}

static int od_cmp_symbol(const void *a, const void *b)
{
	return strcmp(((const struct od_symbol *)a)->label,
		      ((const struct od_symbol *)b)->label);
}

static int od_cmp_extfix(const void *a, const void *b)
{
	const struct od_extfix *ea = a, *eb = b;
	int c = strcmp(ea->label, eb->label);

	return c ? c : strcmp(ea->entry, eb->entry);
}

static int od_cmp_refp(const void *a, const void *b)
{
	const struct od_ref *ra = *(struct od_ref *const *)a;
	const struct od_ref *rb = *(struct od_ref *const *)b;

	if (ra->cell < rb->cell)
		return -1;
	return ra->cell > rb->cell;
}

/**********************************************************************/
/* Reference annotations                                              */
/**********************************************************************/

/*
 * Find the node a phandle value points to and count how many nodes
 * carry that phandle.  Returns the node offset (valid only when
 * *count == 1) or a negative error code.
 */
static int od_phandle_target(const void *fdt, uint32_t phandle, int *count)
{
	int offset, found = -FDT_ERR_NOTFOUND;

	*count = 0;
	for (offset = fdt_next_node(fdt, -1, NULL);
	     offset >= 0;
	     offset = fdt_next_node(fdt, offset, NULL)) {
		if (fdt_get_phandle(fdt, offset) == phandle) {
			(*count)++;
			found = offset;
		}
	}
	if (offset != -FDT_ERR_NOTFOUND)
		return offset;
	return found;
}

/* Validate and resolve one list of reference annotations */
static int od_load_refs(struct od_ctx *ctx, const struct fdt_delta_ref *in,
			int n, const void *fdt, struct od_ref **outp, int *noutp)
{
	struct od_ref *refs = NULL;
	int i, ret;

	if (n > 0) {
		if ((size_t)n > SIZE_MAX / sizeof(*refs))
			return -FDT_ERR_NOSPACE;
		refs = malloc((size_t)n * sizeof(*refs));
		if (!refs)
			return -FDT_ERR_NOSPACE;
		memset(refs, 0, (size_t)n * sizeof(*refs));
	}

	for (i = 0; i < n; i++) {
		const struct fdt_delta_ref *r = &in[i];
		const unsigned char *val;
		fdt32_t word;
		uint32_t phandle;
		int node, len, count, toff, j;

		if (!r->path || !r->property || r->path[0] != '/') {
			ret = OD_FAIL(ctx, -FDT_ERR_BADPATH,
				      r->path ? r->path : "(null)", ":",
				      r->property ? r->property : "(null)");
			goto err;
		}

		node = fdt_path_offset(fdt, r->path);
		if (node < 0) {
			if (node == -FDT_ERR_NOTFOUND)
				node = -FDT_ERR_BADPATH;
			ret = OD_FAIL(ctx, node, r->path, ":", r->property);
			goto err;
		}

		val = fdt_getprop(fdt, node, r->property, &len);
		if (!val) {
			ret = OD_FAIL(ctx, len, r->path, ":", r->property);
			goto err;
		}

		if ((uint64_t)r->cell * 4 + 4 > (uint32_t)len) {
			ret = OD_FAIL(ctx, -FDT_ERR_BADVALUE,
				      r->path, ":", r->property);
			goto err;
		}

		memcpy(&word, val + (size_t)r->cell * 4, sizeof(word));
		phandle = fdt32_to_cpu(word);
		if (!phandle || phandle == (uint32_t)-1) {
			ret = OD_FAIL(ctx, -FDT_ERR_BADPHANDLE,
				      r->path, ":", r->property);
			goto err;
		}

		toff = od_phandle_target(fdt, phandle, &count);
		if (toff < 0 && toff != -FDT_ERR_NOTFOUND) {
			ret = toff;
			goto err;
		}
		if (count != 1) {
			ret = OD_FAIL(ctx, -FDT_ERR_BADPHANDLE,
				      r->path, ":", r->property);
			goto err;
		}

		refs[i].target = od_node_path(fdt, toff, &ret);
		if (!refs[i].target)
			goto err;

		/* The same location may only be annotated once */
		for (j = 0; j < i; j++) {
			if (refs[j].cell == r->cell &&
			    !strcmp(refs[j].path, r->path) &&
			    !strcmp(refs[j].prop, r->property)) {
				ret = OD_FAIL(ctx, -FDT_ERR_EXISTS,
					      r->path, ":", r->property);
				goto err;
			}
		}

		refs[i].path = od_strdup(r->path);
		refs[i].prop = od_strdup(r->property);
		if (!refs[i].path || !refs[i].prop) {
			ret = -FDT_ERR_NOSPACE;
			goto err;
		}
		refs[i].cell = r->cell;
	}

	*outp = refs;
	*noutp = n;
	return 0;

err:
	for (i = 0; i < n; i++) {
		free(refs[i].path);
		free(refs[i].prop);
		free(refs[i].target);
	}
	free(refs);
	return ret;
}

/* Look up an annotation for (path, prop, cell) in a resolved ref list */
static const struct od_ref *od_find_ref(const struct od_ref *refs, int n,
					const char *path, const char *prop,
					uint32_t cell)
{
	int i;

	for (i = 0; i < n; i++)
		if (refs[i].cell == cell &&
		    !strcmp(refs[i].path, path) &&
		    !strcmp(refs[i].prop, prop))
			return &refs[i];
	return NULL;
}

/*
 * Compare two versions of one property.  Annotated cells are compared
 * by the absolute path of the node they reference, everything else
 * byte by byte.
 */
static int od_prop_equal(struct od_ctx *ctx, const char *path,
			 const char *name, const void *bval, int blen,
			 const void *dval, int dlen)
{
	int cells, c;

	if (blen != dlen)
		return 0;

	cells = blen / 4;
	for (c = 0; c < cells; c++) {
		const struct od_ref *br, *dr;

		br = od_find_ref(ctx->brefs, ctx->nbrefs, path, name, c);
		dr = od_find_ref(ctx->drefs, ctx->ndrefs, path, name, c);
		if (br && dr) {
			if (strcmp(br->target, dr->target))
				return 0;
		} else if (br || dr) {
			return 0;
		} else if (memcmp((const char *)bval + 4 * c,
				  (const char *)dval + 4 * c, 4)) {
			return 0;
		}
	}
	if (blen % 4 &&
	    memcmp((const char *)bval + 4 * cells,
		   (const char *)dval + 4 * cells, blen % 4))
		return 0;
	return 1;
}

/**********************************************************************/
/* Model building                                                     */
/**********************************************************************/

static int od_frag_find(struct od_ctx *ctx, const char *path)
{
	int i;

	for (i = 0; i < ctx->nfrags; i++)
		if (!strcmp(ctx->frags[i].path, path))
			return i;
	return -1;
}

/* Find or create the fragment anchored at @path; returns its index */
static int od_frag_get(struct od_ctx *ctx, const char *path)
{
	struct od_frag *f;
	int idx, ret;

	idx = od_frag_find(ctx, path);
	if (idx >= 0)
		return idx;

	ret = od_grow((void **)&ctx->frags, sizeof(*ctx->frags),
		      &ctx->afrags, ctx->nfrags + 1);
	if (ret)
		return ret;
	idx = ctx->nfrags++;
	f = &ctx->frags[idx];
	memset(f, 0, sizeof(*f));
	f->path = od_strdup(path);
	if (!f->path)
		return -FDT_ERR_NOSPACE;
	return idx;
}

/* Add a property to a fragment's or new node's property list */
static int od_prop_add(struct od_prop **props, int *nprops, int *aprops,
		       const char *name, const void *data, int len, int assign)
{
	struct od_prop *p;
	int ret;

	ret = od_grow((void **)props, sizeof(**props), aprops, *nprops + 1);
	if (ret)
		return ret;
	p = &(*props)[(*nprops)++];
	memset(p, 0, sizeof(*p));
	p->name = od_strdup(name);
	p->data = malloc(len ? len : 1);
	if (!p->name || !p->data)
		return -FDT_ERR_NOSPACE;
	if (len && data)
		memcpy(p->data, data, len);
	else if (len)
		memset(p->data, 0, len);
	p->len = len;
	p->assign = assign;
	return 0;
}

static int od_frag_addprop(struct od_ctx *ctx, const char *path,
			   const char *name, const void *data, int len,
			   int assign)
{
	int idx = od_frag_get(ctx, path);
	struct od_frag *f;

	if (idx < 0)
		return idx;
	f = &ctx->frags[idx];
	return od_prop_add(&f->props, &f->nprops, &f->aprops,
			   name, data, len, assign);
}

/* Register a node for a local phandle assignment */
static int od_assign_add(struct od_ctx *ctx, const char *path, int isnew)
{
	struct od_assign *a;
	int ret;

	ret = od_grow((void **)&ctx->assign, sizeof(*ctx->assign),
		      &ctx->aassign, ctx->nassign + 1);
	if (ret)
		return ret;
	a = &ctx->assign[ctx->nassign++];
	memset(a, 0, sizeof(*a));
	a->path = od_strdup(path);
	if (!a->path)
		return -FDT_ERR_NOSPACE;
	a->isnew = isnew;
	return 0;
}

static int od_assign_find(struct od_ctx *ctx, const char *path)
{
	struct od_assign *found;

	found = bsearch(path, ctx->assign, ctx->nassign,
			sizeof(*ctx->assign), od_cmp_assign_str);
	if (!found)
		return -1;
	return found - ctx->assign;
}

/* Recursively build the model of an added subtree from the desired tree */
static struct od_node *od_build_newnode(struct od_ctx *ctx, int doff,
					const char *path, const char *name,
					const char *fragpath, int *err)
{
	struct od_node *node;
	int poff, sub;

	node = malloc(sizeof(*node));
	if (!node) {
		*err = -FDT_ERR_NOSPACE;
		return NULL;
	}
	memset(node, 0, sizeof(*node));
	node->name = od_strdup(name);
	node->path = od_strdup(path);
	node->fragpath = od_strdup(fragpath);
	if (!node->name || !node->path || !node->fragpath) {
		*err = -FDT_ERR_NOSPACE;
		return node;
	}

	*err = od_assign_add(ctx, path, 1);
	if (*err)
		return node;

	fdt_for_each_property_offset(poff, ctx->desired, doff) {
		const char *pname;
		const void *val;
		int len;

		val = fdt_getprop_by_offset(ctx->desired, poff, &pname, &len);
		if (!val) {
			*err = len;
			return node;
		}
		if (!strcmp(pname, "phandle")) {
			node->want_ph = 1;
			continue;
		}
		if (!strcmp(pname, "linux,phandle")) {
			node->want_lph = 1;
			continue;
		}
		*err = od_prop_add(&node->props, &node->nprops, &node->aprops,
				   pname, val, len, 0);
		if (*err)
			return node;
	}
	if (poff < 0 && poff != -FDT_ERR_NOTFOUND) {
		*err = poff;
		return node;
	}

	fdt_for_each_subnode(sub, ctx->desired, doff) {
		const char *subname = fdt_get_name(ctx->desired, sub, NULL);
		struct od_node *kid;
		char *kidpath;

		if (!subname) {
			*err = -FDT_ERR_INTERNAL;
			return node;
		}
		kidpath = od_join_path(path, subname);
		if (!kidpath) {
			*err = -FDT_ERR_NOSPACE;
			return node;
		}
		*err = od_grow((void **)&node->kids, sizeof(*node->kids),
			       &node->akids, node->nkids + 1);
		if (*err) {
			free(kidpath);
			return node;
		}
		kid = od_build_newnode(ctx, sub, kidpath, subname, fragpath, err);
		free(kidpath);
		if (!kid) {
			*err = -FDT_ERR_NOSPACE;
			return node;
		}
		node->kids[node->nkids++] = kid;
		if (*err)
			return node;
	}
	if (sub < 0 && sub != -FDT_ERR_NOTFOUND) {
		*err = sub;
		return node;
	}

	if (node->nprops > 1)
		qsort(node->props, node->nprops, sizeof(*node->props),
		      od_cmp_prop);
	if (node->nkids > 1)
		qsort(node->kids, node->nkids, sizeof(*node->kids),
		      od_cmp_nodep);
	*err = 0;
	return node;
}

static int od_is_phandle_prop(const char *name)
{
	return !strcmp(name, "phandle") || !strcmp(name, "linux,phandle");
}

/* Compare the nodes at @boff and @doff (both at @path) recursively */
static int od_diff_node(struct od_ctx *ctx, int boff, int doff,
			const char *path)
{
	static const char * const phprops[] = { "phandle", "linux,phandle" };
	int poff, sub, i, ret;

	/* Added and overridden properties */
	fdt_for_each_property_offset(poff, ctx->desired, doff) {
		const char *name;
		const void *dval, *bval;
		int dlen, blen;

		dval = fdt_getprop_by_offset(ctx->desired, poff, &name, &dlen);
		if (!dval)
			return dlen;
		if (od_is_phandle_prop(name))
			continue;
		bval = fdt_getprop(ctx->base, boff, name, &blen);
		if (!bval) {
			if (blen != -FDT_ERR_NOTFOUND)
				return blen;
			ret = od_frag_addprop(ctx, path, name, dval, dlen, 0);
		} else if (!od_prop_equal(ctx, path, name, bval, blen,
					  dval, dlen)) {
			ret = od_frag_addprop(ctx, path, name, dval, dlen, 0);
		} else {
			ret = 0;
		}
		if (ret)
			return ret;
	}
	if (poff < 0 && poff != -FDT_ERR_NOTFOUND)
		return poff;

	/* Deleted properties are not supported */
	fdt_for_each_property_offset(poff, ctx->base, boff) {
		const char *name;
		const void *bval;
		int blen, dlen;

		bval = fdt_getprop_by_offset(ctx->base, poff, &name, &blen);
		if (!bval)
			return blen;
		if (od_is_phandle_prop(name))
			continue;
		if (!fdt_getprop(ctx->desired, doff, name, &dlen))
			return OD_FAIL(ctx, -FDT_ERR_NOTFOUND, path, ":", name);
	}
	if (poff < 0 && poff != -FDT_ERR_NOTFOUND)
		return poff;

	/*
	 * Phandles of pre-existing nodes: renumbering is not a
	 * difference, removal is an unsupported deletion, and a phandle
	 * added to a node that had none is emitted with a local number
	 * assigned by us.
	 */
	for (i = 0; i < 2; i++) {
		const void *bval, *dval;
		int blen, dlen;

		bval = fdt_getprop(ctx->base, boff, phprops[i], &blen);
		dval = fdt_getprop(ctx->desired, doff, phprops[i], &dlen);
		if (bval && !dval)
			return OD_FAIL(ctx, -FDT_ERR_NOTFOUND, path, ":",
				       phprops[i]);
		if (!bval && dval) {
			if (dlen != 4)
				return OD_FAIL(ctx, -FDT_ERR_BADPHANDLE,
					       path, ":", phprops[i]);
			ret = od_frag_addprop(ctx, path, phprops[i],
					      NULL, 4, 1);
			if (ret)
				return ret;
			ret = od_assign_add(ctx, path, 0);
			if (ret)
				return ret;
		}
	}

	/* Added and changed subnodes */
	fdt_for_each_subnode(sub, ctx->desired, doff) {
		const char *name = fdt_get_name(ctx->desired, sub, NULL);
		struct od_node *kid;
		char *kidpath;
		int bsub, idx, err;

		if (!name)
			return -FDT_ERR_INTERNAL;
		if (!doff && !strcmp(name, "__symbols__"))
			continue;	/* label metadata, handled separately */
		kidpath = od_join_path(path, name);
		if (!kidpath)
			return -FDT_ERR_NOSPACE;
		bsub = fdt_subnode_offset(ctx->base, boff, name);
		if (bsub >= 0) {
			ret = od_diff_node(ctx, bsub, sub, kidpath);
			free(kidpath);
			if (ret)
				return ret;
			continue;
		}
		if (bsub != -FDT_ERR_NOTFOUND) {
			free(kidpath);
			return bsub;
		}
		/* root of an added subtree */
		idx = od_frag_get(ctx, path);
		if (idx < 0) {
			free(kidpath);
			return idx;
		}
		kid = od_build_newnode(ctx, sub, kidpath, name, path, &err);
		free(kidpath);
		if (!kid)
			return -FDT_ERR_NOSPACE;
		if (err)
			return err;
		ret = od_grow((void **)&ctx->frags[idx].kids,
			      sizeof(*ctx->frags[idx].kids),
			      &ctx->frags[idx].akids,
			      ctx->frags[idx].nkids + 1);
		if (ret)
			return ret;
		ctx->frags[idx].kids[ctx->frags[idx].nkids++] = kid;
	}
	if (sub < 0 && sub != -FDT_ERR_NOTFOUND)
		return sub;

	/* Deleted subnodes are not supported */
	fdt_for_each_subnode(sub, ctx->base, boff) {
		const char *name = fdt_get_name(ctx->base, sub, NULL);
		char *kidpath;
		int dsub;

		if (!name)
			return -FDT_ERR_INTERNAL;
		if (!boff && !strcmp(name, "__symbols__"))
			continue;
		dsub = fdt_subnode_offset(ctx->desired, doff, name);
		if (dsub >= 0)
			continue;
		if (dsub != -FDT_ERR_NOTFOUND)
			return dsub;
		kidpath = od_join_path(path, name);
		if (!kidpath)
			return -FDT_ERR_NOSPACE;
		ret = OD_FAIL(ctx, -FDT_ERR_NOTFOUND, kidpath);
		free(kidpath);
		return ret;
	}
	if (sub < 0 && sub != -FDT_ERR_NOTFOUND)
		return sub;

	return 0;
}

/*
 * Compare the /__symbols__ nodes of both trees.  They carry label
 * metadata and are never copied as ordinary device nodes.
 */
static int od_diff_symbols(struct od_ctx *ctx)
{
	int bsym, dsym, poff;

	bsym = fdt_path_offset(ctx->base, "/__symbols__");
	if (bsym == -FDT_ERR_NOTFOUND)
		bsym = -1;
	else if (bsym < 0)
		return bsym;
	dsym = fdt_path_offset(ctx->desired, "/__symbols__");
	if (dsym == -FDT_ERR_NOTFOUND)
		dsym = -1;
	else if (dsym < 0)
		return dsym;

	/* Labels may not disappear or be retargeted */
	if (bsym >= 0) {
		fdt_for_each_property_offset(poff, ctx->base, bsym) {
			const char *label, *bpath, *dpath;
			int blen, dlen;

			bpath = fdt_getprop_by_offset(ctx->base, poff, &label,
						      &blen);
			if (!bpath)
				return blen;
			dpath = (dsym >= 0) ?
				fdt_getprop(ctx->desired, dsym, label, &dlen) :
				NULL;
			if (!dpath)
				return OD_FAIL(ctx, -FDT_ERR_NOTFOUND,
					       "/__symbols__:", label);
			if (blen != dlen || memcmp(bpath, dpath, blen))
				return OD_FAIL(ctx, -FDT_ERR_BADVALUE,
					       "/__symbols__:", label);
		}
		if (poff < 0 && poff != -FDT_ERR_NOTFOUND)
			return poff;
	}

	/* Labels added by the desired tree */
	if (dsym >= 0) {
		fdt_for_each_property_offset(poff, ctx->desired, dsym) {
			const char *label, *dpath;
			int dlen, blen;
			struct od_symbol *s;
			int ret;

			dpath = fdt_getprop_by_offset(ctx->desired, poff,
						      &label, &dlen);
			if (!dpath)
				return dlen;
			if (bsym >= 0 &&
			    fdt_getprop(ctx->base, bsym, label, &blen))
				continue;	/* already checked above */
			if (dlen < 1 || dpath[dlen - 1] != '\0' ||
			    dpath[0] != '/')
				return OD_FAIL(ctx, -FDT_ERR_BADVALUE,
					       "/__symbols__:", label);
			ret = od_grow((void **)&ctx->syms, sizeof(*ctx->syms),
				      &ctx->asyms, ctx->nsyms + 1);
			if (ret)
				return ret;
			s = &ctx->syms[ctx->nsyms++];
			memset(s, 0, sizeof(*s));
			s->label = od_strdup(label);
			s->path = od_strdup(dpath);
			if (!s->label || !s->path)
				return -FDT_ERR_NOSPACE;
		}
		if (poff < 0 && poff != -FDT_ERR_NOTFOUND)
			return poff;
	}

	return 0;
}

/**********************************************************************/
/* Phandle assignment and reference patching                          */
/**********************************************************************/

/* Sort the model and assign stable local phandle numbers */
static int od_organize(struct od_ctx *ctx)
{
	int i;

	if (ctx->nfrags > 1)
		qsort(ctx->frags, ctx->nfrags, sizeof(*ctx->frags),
		      od_cmp_frag);
	for (i = 0; i < ctx->nfrags; i++) {
		struct od_frag *f = &ctx->frags[i];

		if (f->nprops > 1)
			qsort(f->props, f->nprops, sizeof(*f->props),
			      od_cmp_prop);
		if (f->nkids > 1)
			qsort(f->kids, f->nkids, sizeof(*f->kids),
			      od_cmp_nodep);
	}

	if (ctx->nassign > 1)
		qsort(ctx->assign, ctx->nassign, sizeof(*ctx->assign),
		      od_cmp_assign);
	for (i = 0; i < ctx->nassign; i++)
		ctx->assign[i].ph = (uint32_t)i + 1;

	if (ctx->nsyms > 1)
		qsort(ctx->syms, ctx->nsyms, sizeof(*ctx->syms), od_cmp_symbol);

	/* Mark assignment entries that some reference points to */
	for (i = 0; i < ctx->ndrefs; i++) {
		int idx = od_assign_find(ctx, ctx->drefs[i].target);

		if (idx >= 0)
			ctx->assign[idx].referenced = 1;
	}
	return 0;
}

/* Give added nodes their phandle properties, in sorted order */
static int od_node_phandles(struct od_ctx *ctx, struct od_node *node)
{
	int i, idx, ret;

	idx = od_assign_find(ctx, node->path);
	if (idx < 0)
		return -FDT_ERR_INTERNAL;
	if (node->want_ph || ctx->assign[idx].referenced) {
		fdt32_t v = cpu_to_fdt32(ctx->assign[idx].ph);

		ret = od_prop_add(&node->props, &node->nprops, &node->aprops,
				  "phandle", &v, sizeof(v), 0);
		if (ret)
			return ret;
	}
	if (node->want_lph) {
		fdt32_t v = cpu_to_fdt32(ctx->assign[idx].ph);

		ret = od_prop_add(&node->props, &node->nprops, &node->aprops,
				  "linux,phandle", &v, sizeof(v), 0);
		if (ret)
			return ret;
	}
	if (node->nprops > 1)
		qsort(node->props, node->nprops, sizeof(*node->props),
		      od_cmp_prop);
	for (i = 0; i < node->nkids; i++) {
		ret = od_node_phandles(ctx, node->kids[i]);
		if (ret)
			return ret;
	}
	return 0;
}

/* Find the lexicographically smallest base-tree label for @target */
static char *od_base_label(struct od_ctx *ctx, const char *target, int *err)
{
	const char *best = NULL;
	int sym, poff;
	size_t tlen = strlen(target) + 1;

	*err = 0;
	sym = fdt_path_offset(ctx->base, "/__symbols__");
	if (sym == -FDT_ERR_NOTFOUND)
		return NULL;
	if (sym < 0) {
		*err = sym;
		return NULL;
	}

	fdt_for_each_property_offset(poff, ctx->base, sym) {
		const char *label, *val;
		int len;

		val = fdt_getprop_by_offset(ctx->base, poff, &label, &len);
		if (!val) {
			*err = len;
			return NULL;
		}
		if ((size_t)len == tlen && !memcmp(val, target, tlen) &&
		    (!best || strcmp(label, best) < 0))
			best = label;
	}
	if (poff < 0 && poff != -FDT_ERR_NOTFOUND) {
		*err = poff;
		return NULL;
	}
	if (!best)
		return NULL;
	*err = 0;
	return od_strdup(best);
}

static int od_add_extfix(struct od_ctx *ctx, const char *label,
			 const char *entry)
{
	struct od_extfix *f;
	int ret;

	ret = od_grow((void **)&ctx->extfix, sizeof(*ctx->extfix),
		      &ctx->aextfix, ctx->nextfix + 1);
	if (ret)
		return ret;
	f = &ctx->extfix[ctx->nextfix++];
	memset(f, 0, sizeof(*f));
	f->label = od_strdup(label);
	f->entry = od_strdup(entry);
	if (!f->label || !f->entry)
		return -FDT_ERR_NOSPACE;
	return 0;
}

static int od_add_locfix(struct od_ctx *ctx, const char *nodepath,
			 const char *prop, uint32_t off)
{
	struct od_locfix *f = NULL;
	int i, ret;

	for (i = 0; i < ctx->nlocfix; i++) {
		if (!strcmp(ctx->locfix[i].nodepath, nodepath) &&
		    !strcmp(ctx->locfix[i].prop, prop)) {
			f = &ctx->locfix[i];
			break;
		}
	}
	if (!f) {
		ret = od_grow((void **)&ctx->locfix, sizeof(*ctx->locfix),
			      &ctx->alocfix, ctx->nlocfix + 1);
		if (ret)
			return ret;
		f = &ctx->locfix[ctx->nlocfix++];
		memset(f, 0, sizeof(*f));
		f->nodepath = od_strdup(nodepath);
		f->prop = od_strdup(prop);
		if (!f->nodepath || !f->prop)
			return -FDT_ERR_NOSPACE;
	}
	ret = od_grow((void **)&f->offs, sizeof(*f->offs), &f->aoffs,
		      f->noffs + 1);
	if (ret)
		return ret;
	f->offs[f->noffs++] = off;
	return 0;
}

/*
 * Patch the annotated cells of one property to be emitted at @ovpath
 * in the overlay, recording the matching fixup entries.
 */
static int od_patch_prop(struct od_ctx *ctx, const char *path,
			 struct od_prop *prop, const char *ovpath)
{
	const struct od_ref **rr;
	int i, n, ret;

	n = 0;
	for (i = 0; i < ctx->ndrefs; i++)
		if (!strcmp(ctx->drefs[i].path, path) &&
		    !strcmp(ctx->drefs[i].prop, prop->name))
			n++;
	if (!n)
		return 0;

	rr = malloc(n * sizeof(*rr));
	if (!rr)
		return -FDT_ERR_NOSPACE;
	n = 0;
	for (i = 0; i < ctx->ndrefs; i++)
		if (!strcmp(ctx->drefs[i].path, path) &&
		    !strcmp(ctx->drefs[i].prop, prop->name))
			rr[n++] = &ctx->drefs[i];
	if (n > 1)
		qsort(rr, n, sizeof(*rr), od_cmp_refp);

	ret = 0;
	for (i = 0; i < n; i++) {
		const struct od_ref *r = rr[i];
		uint32_t byteoff = r->cell * 4;
		int idx = od_assign_find(ctx, r->target);
		fdt32_t v;

		if (idx >= 0 && ctx->assign[idx].isnew) {
			/* reference to an added node: local phandle */
			v = cpu_to_fdt32(ctx->assign[idx].ph);
			memcpy(prop->data + byteoff, &v, sizeof(v));
			ret = od_add_locfix(ctx, ovpath, prop->name, byteoff);
			if (ret)
				break;
		} else {
			/* reference to a pre-existing node: external fixup */
			char *label, *entry;
			char num[11];
			int err;

			label = od_base_label(ctx, r->target, &err);
			if (!label) {
				ret = err ? err :
					OD_FAIL(ctx, -FDT_ERR_NOTFOUND,
						path, ":", prop->name, "->",
						r->target);
				break;
			}
			v = cpu_to_fdt32(0);	/* resolved when applied */
			memcpy(prop->data + byteoff, &v, sizeof(v));
			od_u32str(byteoff, num);
			entry = malloc(strlen(ovpath) + strlen(prop->name) +
				       strlen(num) + 3);
			if (!entry) {
				free(label);
				ret = -FDT_ERR_NOSPACE;
				break;
			}
			strcpy(entry, ovpath);
			strcat(entry, ":");
			strcat(entry, prop->name);
			strcat(entry, ":");
			strcat(entry, num);
			ret = od_add_extfix(ctx, label, entry);
			free(label);
			free(entry);
			if (ret)
				break;
		}
	}
	free(rr);
	return ret;
}

static int od_patch_node(struct od_ctx *ctx, struct od_node *node,
			 const char *ovpath)
{
	int i, ret;

	for (i = 0; i < node->nprops; i++) {
		ret = od_patch_prop(ctx, node->path, &node->props[i], ovpath);
		if (ret)
			return ret;
	}
	for (i = 0; i < node->nkids; i++) {
		char *kidpath = od_join_path(ovpath, node->kids[i]->name);

		if (!kidpath)
			return -FDT_ERR_NOSPACE;
		ret = od_patch_node(ctx, node->kids[i], kidpath);
		free(kidpath);
		if (ret)
			return ret;
	}
	return 0;
}

/* Fill in assigned phandles and patch all reference cells */
static int od_patch(struct od_ctx *ctx)
{
	int i, j, ret;

	for (i = 0; i < ctx->nfrags; i++) {
		struct od_frag *f = &ctx->frags[i];
		char num[11];
		char *ovpath;

		od_u32str((uint32_t)i, num);
		ovpath = malloc(strlen("/fragment@") + strlen(num) +
				strlen("/__overlay__") + 1);
		if (!ovpath)
			return -FDT_ERR_NOSPACE;
		strcpy(ovpath, "/fragment@");
		strcat(ovpath, num);
		strcat(ovpath, "/__overlay__");

		for (j = 0; j < f->nprops; j++) {
			struct od_prop *p = &f->props[j];

			if (p->assign) {
				/* phandle added to a pre-existing node */
				int idx = od_assign_find(ctx, f->path);
				fdt32_t v;

				if (idx < 0) {
					free(ovpath);
					return -FDT_ERR_INTERNAL;
				}
				v = cpu_to_fdt32(ctx->assign[idx].ph);
				memcpy(p->data, &v, sizeof(v));
				continue;
			}
			ret = od_patch_prop(ctx, f->path, p, ovpath);
			if (ret) {
				free(ovpath);
				return ret;
			}
		}
		for (j = 0; j < f->nkids; j++) {
			char *kidpath = od_join_path(ovpath, f->kids[j]->name);

			if (!kidpath) {
				free(ovpath);
				return -FDT_ERR_NOSPACE;
			}
			ret = od_patch_node(ctx, f->kids[j], kidpath);
			free(kidpath);
			if (ret) {
				free(ovpath);
				return ret;
			}
		}
		free(ovpath);
	}
	return 0;
}

/* Find an added node by absolute path */
static struct od_node *od_find_newnode(struct od_node *node, const char *path)
{
	int i;

	if (!strcmp(node->path, path))
		return node;
	for (i = 0; i < node->nkids; i++) {
		struct od_node *found = od_find_newnode(node->kids[i], path);

		if (found)
			return found;
	}
	return NULL;
}

/* Compute the overlay-internal path for each new symbol */
static int od_symbol_ovpaths(struct od_ctx *ctx)
{
	int i, j, k;

	for (i = 0; i < ctx->nsyms; i++) {
		struct od_symbol *s = &ctx->syms[i];
		struct od_node *node = NULL;
		const char *rel;
		char num[11];

		for (j = 0; j < ctx->nfrags; j++) {
			if (!strcmp(ctx->frags[j].path, s->path))
				break;
			for (k = 0; k < ctx->frags[j].nkids && !node; k++)
				node = od_find_newnode(ctx->frags[j].kids[k],
						       s->path);
			if (node)
				break;
		}
		if (j == ctx->nfrags)
			return OD_FAIL(ctx, -FDT_ERR_BADVALUE,
				       "/__symbols__:", s->label);

		od_u32str((uint32_t)j, num);
		if (node) {
			/* path of the added node relative to the fragment */
			if (!strcmp(node->fragpath, "/"))
				rel = node->path;
			else
				rel = node->path + strlen(node->fragpath);
		} else {
			rel = "";
		}
		s->ovpath = malloc(strlen("/fragment@") + strlen(num) +
				   strlen("/__overlay__") + strlen(rel) + 1);
		if (!s->ovpath)
			return -FDT_ERR_NOSPACE;
		strcpy(s->ovpath, "/fragment@");
		strcat(s->ovpath, num);
		strcat(s->ovpath, "/__overlay__");
		strcat(s->ovpath, rel);
	}
	return 0;
}

/**********************************************************************/
/* Emission                                                           */
/**********************************************************************/

static int od_emit_begin(struct od_ctx *ctx, const char *name)
{
	if (ctx->fdt)
		return fdt_begin_node(ctx->fdt, name);
	ctx->size += FDT_TAGSIZE + FDT_TAGALIGN(strlen(name) + 1);
	return 0;
}

static int od_emit_end(struct od_ctx *ctx)
{
	if (ctx->fdt)
		return fdt_end_node(ctx->fdt);
	ctx->size += FDT_TAGSIZE;
	return 0;
}

static int od_emit_prop(struct od_ctx *ctx, const char *name,
			const void *val, int len)
{
	if (ctx->fdt)
		return fdt_property(ctx->fdt, name, val, len);
	/* struct fdt_property is 3 tags; string dedup only shrinks */
	ctx->size += 3 * FDT_TAGSIZE + FDT_TAGALIGN(len);
	ctx->size += strlen(name) + 1;
	return 0;
}

static int od_emit_newnode(struct od_ctx *ctx, struct od_node *node)
{
	int i, ret;

	ret = od_emit_begin(ctx, node->name);
	if (ret)
		return ret;
	for (i = 0; i < node->nprops; i++) {
		ret = od_emit_prop(ctx, node->props[i].name,
				   node->props[i].data, node->props[i].len);
		if (ret)
			return ret;
	}
	for (i = 0; i < node->nkids; i++) {
		ret = od_emit_newnode(ctx, node->kids[i]);
		if (ret)
			return ret;
	}
	return od_emit_end(ctx);
}

/* Emit /__local_fixups__, mirroring the overlay's node hierarchy */
static int od_emit_locfixes(struct od_ctx *ctx)
{
	char **stack = NULL;
	int nstack = 0, astack = 0;
	int i, j, ret;

	ret = od_emit_begin(ctx, "__local_fixups__");
	if (ret)
		return ret;
	for (i = 0; i < ctx->nlocfix; i++) {
		struct od_locfix *f = &ctx->locfix[i];
		const char *p, *start;
		char **comps = NULL;
		int ncomps = 0, acomps = 0, common;
		fdt32_t *offs;

		/* split the overlay node path into components */
		p = f->nodepath;
		while (*p == '/')
			p++;
		while (*p) {
			char *comp;

			start = p;
			while (*p && *p != '/')
				p++;
			comp = malloc(p - start + 1);
			if (!comp) {
				ret = -FDT_ERR_NOSPACE;
				goto fail_comps;
			}
			memcpy(comp, start, p - start);
			comp[p - start] = '\0';
			ret = od_grow((void **)&comps, sizeof(*comps),
				      &acomps, ncomps + 1);
			if (ret) {
				free(comp);
				goto fail_comps;
			}
			comps[ncomps++] = comp;
			while (*p == '/')
				p++;
		}

		common = 0;
		while (common < nstack && common < ncomps &&
		       !strcmp(stack[common], comps[common]))
			common++;
		while (nstack > common) {
			ret = od_emit_end(ctx);
			if (ret)
				goto fail_comps;
			free(stack[--nstack]);
		}
		for (j = common; j < ncomps; j++) {
			ret = od_emit_begin(ctx, comps[j]);
			if (ret)
				goto fail_comps;
			ret = od_grow((void **)&stack, sizeof(*stack),
				      &astack, nstack + 1);
			if (ret)
				goto fail_comps;
			stack[nstack++] = comps[j];
			comps[j] = NULL;
		}

		offs = malloc(f->noffs * sizeof(*offs));
		if (!offs) {
			ret = -FDT_ERR_NOSPACE;
			goto fail_comps;
		}
		for (j = 0; j < f->noffs; j++)
			offs[j] = cpu_to_fdt32(f->offs[j]);
		ret = od_emit_prop(ctx, f->prop, offs,
				   f->noffs * (int)sizeof(*offs));
		free(offs);
		if (ret)
			goto fail_comps;

		for (j = 0; j < ncomps; j++)
			free(comps[j]);
		free(comps);
		continue;

fail_comps:
		if (comps) {
			for (j = 0; j < ncomps; j++)
				free(comps[j]);
			free(comps);
		}
		goto fail;
	}
	while (nstack > 0) {
		ret = od_emit_end(ctx);
		if (ret)
			goto fail;
		free(stack[--nstack]);
	}
	ret = od_emit_end(ctx);
	if (ret)
		goto fail;
	free(stack);
	return 0;

fail:
	for (i = 0; i < nstack; i++)
		free(stack[i]);
	free(stack);
	return ret;
}

static int od_emit_fixups(struct od_ctx *ctx)
{
	int i, ret;

	if (ctx->nextfix) {
		qsort(ctx->extfix, ctx->nextfix, sizeof(*ctx->extfix),
		      od_cmp_extfix);
		ret = od_emit_begin(ctx, "__fixups__");
		if (ret)
			return ret;
		for (i = 0; i < ctx->nextfix; ) {
			const char *label = ctx->extfix[i].label;
			int j = i, k, len = 0;
			char *buf, *p;

			while (j < ctx->nextfix &&
			       !strcmp(ctx->extfix[j].label, label)) {
				len += (int)strlen(ctx->extfix[j].entry) + 1;
				j++;
			}
			buf = malloc(len);
			if (!buf)
				return -FDT_ERR_NOSPACE;
			p = buf;
			for (k = i; k < j; k++) {
				strcpy(p, ctx->extfix[k].entry);
				p += strlen(p) + 1;
			}
			ret = od_emit_prop(ctx, label, buf, len);
			free(buf);
			if (ret)
				return ret;
			i = j;
		}
		ret = od_emit_end(ctx);
		if (ret)
			return ret;
	}

	if (ctx->nlocfix) {
		ret = od_emit_locfixes(ctx);
		if (ret)
			return ret;
	}

	if (ctx->nsyms) {
		ret = od_emit_begin(ctx, "__symbols__");
		if (ret)
			return ret;
		for (i = 0; i < ctx->nsyms; i++) {
			ret = od_emit_prop(ctx, ctx->syms[i].label,
					   ctx->syms[i].ovpath,
					   (int)strlen(ctx->syms[i].ovpath) + 1);
			if (ret)
				return ret;
		}
		ret = od_emit_end(ctx);
		if (ret)
			return ret;
	}
	return 0;
}

/* Emit the whole overlay; with ctx->fdt == NULL only account sizes */
static int od_emit_all(struct od_ctx *ctx)
{
	int i, j, ret;

	ret = od_emit_begin(ctx, "");
	if (ret)
		return ret;
	for (i = 0; i < ctx->nfrags; i++) {
		struct od_frag *f = &ctx->frags[i];
		char name[11 + sizeof("fragment@")];
		char num[11];

		od_u32str((uint32_t)i, num);
		strcpy(name, "fragment@");
		strcat(name, num);

		ret = od_emit_begin(ctx, name);
		if (ret)
			return ret;
		ret = od_emit_prop(ctx, "target-path", f->path,
				   strlen(f->path) + 1);
		if (ret)
			return ret;
		ret = od_emit_begin(ctx, "__overlay__");
		if (ret)
			return ret;
		for (j = 0; j < f->nprops; j++) {
			ret = od_emit_prop(ctx, f->props[j].name,
					   f->props[j].data, f->props[j].len);
			if (ret)
				return ret;
		}
		for (j = 0; j < f->nkids; j++) {
			ret = od_emit_newnode(ctx, f->kids[j]);
			if (ret)
				return ret;
		}
		ret = od_emit_end(ctx);		/* __overlay__ */
		if (ret)
			return ret;
		ret = od_emit_end(ctx);		/* fragment */
		if (ret)
			return ret;
	}
	ret = od_emit_fixups(ctx);
	if (ret)
		return ret;
	return od_emit_end(ctx);		/* root */
}

/*
 * Generate the overlay into a scratch buffer, then copy it to the
 * caller's buffer if it fits.  The caller's buffer is only touched
 * when the complete overlay fits.
 */
static int od_generate(struct od_ctx *ctx, void *buf, size_t bufsize,
		       size_t *neededp)
{
	void *scratch;
	size_t bound, needed;
	int ret;

	/* sizing pass */
	ctx->fdt = NULL;
	ctx->size = 0;
	ret = od_emit_all(ctx);
	if (ret)
		return ret;

	/* 48-byte header, 16-byte reserve map terminator, FDT_END tag */
	if (ctx->size > INT_MAX - 64 - FDT_TAGSIZE - 16)
		return -FDT_ERR_NOSPACE;
	bound = 64 + ctx->size + FDT_TAGSIZE + 16;

	scratch = malloc(bound);
	if (!scratch)
		return -FDT_ERR_NOSPACE;

	ret = fdt_create(scratch, (int)bound);
	if (ret)
		goto out;
	ret = fdt_finish_reservemap(scratch);
	if (ret)
		goto out;
	ctx->fdt = scratch;
	ret = od_emit_all(ctx);
	ctx->fdt = NULL;
	if (ret)
		goto out;
	ret = fdt_finish(scratch);
	if (ret)
		goto out;

	needed = fdt_totalsize(scratch);
	if (needed <= bufsize && buf)
		memcpy(buf, scratch, needed);
	*neededp = needed;
	ret = 0;
out:
	free(scratch);
	return ret;
}

/**********************************************************************/
/* Cleanup                                                            */
/**********************************************************************/

static void od_free_props(struct od_prop *props, int nprops)
{
	int i;

	for (i = 0; i < nprops; i++) {
		free(props[i].name);
		free(props[i].data);
	}
	free(props);
}

static void od_free_node(struct od_node *node)
{
	int i;

	if (!node)
		return;
	free(node->name);
	free(node->path);
	free(node->fragpath);
	od_free_props(node->props, node->nprops);
	for (i = 0; i < node->nkids; i++)
		od_free_node(node->kids[i]);
	free(node->kids);
	free(node);
}

static void od_ctx_free(struct od_ctx *ctx)
{
	int i;

	for (i = 0; i < ctx->nbrefs; i++) {
		free(ctx->brefs[i].path);
		free(ctx->brefs[i].prop);
		free(ctx->brefs[i].target);
	}
	free(ctx->brefs);
	for (i = 0; i < ctx->ndrefs; i++) {
		free(ctx->drefs[i].path);
		free(ctx->drefs[i].prop);
		free(ctx->drefs[i].target);
	}
	free(ctx->drefs);
	for (i = 0; i < ctx->nfrags; i++) {
		struct od_frag *f = &ctx->frags[i];
		int j;

		free(f->path);
		od_free_props(f->props, f->nprops);
		for (j = 0; j < f->nkids; j++)
			od_free_node(f->kids[j]);
		free(f->kids);
	}
	free(ctx->frags);
	for (i = 0; i < ctx->nassign; i++)
		free(ctx->assign[i].path);
	free(ctx->assign);
	for (i = 0; i < ctx->nsyms; i++) {
		free(ctx->syms[i].label);
		free(ctx->syms[i].path);
		free(ctx->syms[i].ovpath);
	}
	free(ctx->syms);
	for (i = 0; i < ctx->nextfix; i++) {
		free(ctx->extfix[i].label);
		free(ctx->extfix[i].entry);
	}
	free(ctx->extfix);
	for (i = 0; i < ctx->nlocfix; i++) {
		free(ctx->locfix[i].nodepath);
		free(ctx->locfix[i].prop);
		free(ctx->locfix[i].offs);
	}
	free(ctx->locfix);
}

/**********************************************************************/
/* Public entry point                                                 */
/**********************************************************************/

int fdt_overlay_delta(const void *base, const void *desired,
		      const struct fdt_delta_ref *base_refs, int num_base_refs,
		      const struct fdt_delta_ref *desired_refs,
		      int num_desired_refs,
		      void *buf, size_t bufsize,
		      char *errpath, size_t errpath_size)
{
	struct od_ctx ctx;
	size_t bsize, dsize, needed;
	int ret, i;

	memset(&ctx, 0, sizeof(ctx));
	ctx.base = base;
	ctx.desired = desired;
	ctx.errpath = errpath;
	ctx.errpath_size = errpath_size;
	if (errpath && errpath_size)
		errpath[0] = '\0';

	if (!base || !desired || num_base_refs < 0 || num_desired_refs < 0 ||
	    (!base_refs && num_base_refs > 0) ||
	    (!desired_refs && num_desired_refs > 0) ||
	    (!buf && bufsize > 0))
		return od_fail(&ctx, -FDT_ERR_BADVALUE, NULL);

	if (fdt_magic(base) != FDT_MAGIC || fdt_magic(desired) != FDT_MAGIC)
		return od_fail(&ctx, -FDT_ERR_BADMAGIC, NULL);

	/* The inputs, the annotations and the output must not overlap */
	bsize = fdt_totalsize(base);
	dsize = fdt_totalsize(desired);
	if (od_ranges_overlap(base, bsize, desired, dsize) ||
	    od_ranges_overlap(base, bsize, buf, bufsize) ||
	    od_ranges_overlap(desired, dsize, buf, bufsize) ||
	    od_ranges_overlap(base_refs,
			      (size_t)num_base_refs * sizeof(*base_refs),
			      buf, bufsize) ||
	    od_ranges_overlap(desired_refs,
			      (size_t)num_desired_refs * sizeof(*desired_refs),
			      buf, bufsize) ||
	    od_ranges_overlap(base_refs,
			      (size_t)num_base_refs * sizeof(*base_refs),
			      base, bsize) ||
	    od_ranges_overlap(base_refs,
			      (size_t)num_base_refs * sizeof(*base_refs),
			      desired, dsize) ||
	    od_ranges_overlap(desired_refs,
			      (size_t)num_desired_refs * sizeof(*desired_refs),
			      base, bsize) ||
	    od_ranges_overlap(desired_refs,
			      (size_t)num_desired_refs * sizeof(*desired_refs),
			      desired, dsize))
		return od_fail(&ctx, -FDT_ERR_BADVALUE, NULL);

	ret = fdt_check_full(base, bsize);
	if (ret)
		goto out;
	ret = fdt_check_full(desired, dsize);
	if (ret)
		goto out;

	/*
	 * The overlay cannot change the base tree's reserve map, so the
	 * desired tree must agree with it.
	 */
	{
		int nb = fdt_num_mem_rsv(base);
		int nd = fdt_num_mem_rsv(desired);

		if (nb < 0) {
			ret = nb;
			goto out;
		}
		if (nd < 0) {
			ret = nd;
			goto out;
		}
		if (nb != nd) {
			ret = OD_FAIL(&ctx, -FDT_ERR_BADVALUE, "<reserve-map>");
			goto out;
		}
		for (i = 0; i < nb; i++) {
			uint64_t ba, bs, da, ds;

			ret = fdt_get_mem_rsv(base, i, &ba, &bs);
			if (ret)
				goto out;
			ret = fdt_get_mem_rsv(desired, i, &da, &ds);
			if (ret)
				goto out;
			if (ba != da || bs != ds) {
				ret = OD_FAIL(&ctx, -FDT_ERR_BADVALUE,
					      "<reserve-map>");
				goto out;
			}
		}
	}

	ret = od_load_refs(&ctx, base_refs, num_base_refs, base,
			   &ctx.brefs, &ctx.nbrefs);
	if (ret)
		goto out;
	ret = od_load_refs(&ctx, desired_refs, num_desired_refs, desired,
			   &ctx.drefs, &ctx.ndrefs);
	if (ret)
		goto out;

	ret = od_diff_node(&ctx, 0, 0, "/");
	if (ret)
		goto out;
	ret = od_diff_symbols(&ctx);
	if (ret)
		goto out;

	ret = od_organize(&ctx);
	if (ret)
		goto out;
	for (i = 0; i < ctx.nfrags; i++) {
		int j;

		for (j = 0; j < ctx.frags[i].nkids; j++) {
			ret = od_node_phandles(&ctx, ctx.frags[i].kids[j]);
			if (ret)
				goto out;
		}
	}
	ret = od_patch(&ctx);
	if (ret)
		goto out;
	ret = od_symbol_ovpaths(&ctx);
	if (ret)
		goto out;

	ret = od_generate(&ctx, buf, bufsize, &needed);
	if (ret)
		goto out;
	if (needed > INT_MAX) {
		ret = -FDT_ERR_NOSPACE;
		goto out;
	}
	ret = (int)needed;

out:
	od_ctx_free(&ctx);
	return ret;
}
