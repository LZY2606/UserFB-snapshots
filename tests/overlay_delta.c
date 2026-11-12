// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * libfdt - Flat Device Tree manipulation
 *	Testcase for fdt_overlay_delta()
 *
 * The checks/run.sh harness links this file together with
 * checks/delta_driver.c, which calls overlay_delta_case() once per
 * group.  Each group returns 0 on success and counts every passed
 * assertion in *checks.
 */

#include <libfdt.h>
#include <fdt_overlay_delta.h>

#include <stdio.h>
#include <string.h>

static unsigned *g_checks;
static const char *g_group;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL [%s] %s:%d: %s\n", \
				g_group, __FILE__, __LINE__, #cond); \
			return -1; \
		} \
		++*g_checks; \
	} while (0)

#define TCHK(expr) \
	do { \
		int e_ = (expr); \
		if (e_) { \
			fprintf(stderr, "BUILD FAIL [%s] %s:%d: %s -> %d\n", \
				g_group, __FILE__, __LINE__, #expr, e_); \
			return -1; \
		} \
	} while (0)

struct tref {
	const char *path;
	const char *prop;
	uint32_t cell;
};

/**********************************************************************/
/* Tree building helpers                                              */
/**********************************************************************/

/* Create the node at @path (and any missing intermediates) */
static int tnode(void *fdt, const char *path)
{
	char name[64];
	const char *p = path;
	int parent = 0;

	if (path[0] != '/')
		return -1;
	if (!path[1])
		return 0;
	while (*p) {
		const char *s;
		int len, off;

		p++;
		s = strchr(p, '/');
		len = s ? (int)(s - p) : (int)strlen(p);
		if (!len || (size_t)len >= sizeof(name))
			return -1;
		memcpy(name, p, len);
		name[len] = '\0';
		off = fdt_subnode_offset(fdt, parent, name);
		if (off == -FDT_ERR_NOTFOUND)
			off = fdt_add_subnode(fdt, parent, name);
		if (off < 0)
			return off;
		parent = off;
		if (!s)
			break;
		p = s;
	}
	return 0;
}

static int tprop(void *fdt, const char *path, const char *name,
		 const void *val, int len)
{
	int off = fdt_path_offset(fdt, path);

	if (off < 0)
		return off;
	return fdt_setprop(fdt, off, name, val, len);
}

static int tprop_u32(void *fdt, const char *path, const char *name, uint32_t v)
{
	int off = fdt_path_offset(fdt, path);

	if (off < 0)
		return off;
	return fdt_setprop_u32(fdt, off, name, v);
}

static int tprop_str(void *fdt, const char *path, const char *name,
		     const char *s)
{
	int off = fdt_path_offset(fdt, path);

	if (off < 0)
		return off;
	return fdt_setprop_string(fdt, off, name, s);
}

static int tprop_cells(void *fdt, const char *path, const char *name,
		       const uint32_t *cells, int n)
{
	fdt32_t buf[16];
	int i, off = fdt_path_offset(fdt, path);

	if (off < 0)
		return off;
	if (n > 16)
		return -1;
	for (i = 0; i < n; i++)
		buf[i] = cpu_to_fdt32(cells[i]);
	return fdt_setprop(fdt, off, name, buf, n * (int)sizeof(fdt32_t));
}

static int tphandle(void *fdt, const char *path, uint32_t phandle)
{
	return tprop_u32(fdt, path, "phandle", phandle);
}

static int tsymbol(void *fdt, const char *label, const char *path)
{
	int off = fdt_path_offset(fdt, "/__symbols__");

	if (off == -FDT_ERR_NOTFOUND) {
		if (tnode(fdt, "/__symbols__"))
			return -1;
		off = fdt_path_offset(fdt, "/__symbols__");
	}
	if (off < 0)
		return off;
	return fdt_setprop_string(fdt, off, label, path);
}

/**********************************************************************/
/* Inspection helpers                                                 */
/**********************************************************************/

static int prop_u32_eq(const void *fdt, int off, const char *name, uint32_t v)
{
	const fdt32_t *p;
	int len;

	p = fdt_getprop(fdt, off, name, &len);
	return p && len == 4 && fdt32_to_cpu(*p) == v;
}

static int prop_str_eq(const void *fdt, int off, const char *name,
		       const char *s)
{
	const char *v;
	int len;

	v = fdt_getprop(fdt, off, name, &len);
	return v && len == (int)strlen(s) + 1 && !memcmp(v, s, len);
}

/* Check a property holds exactly the given NUL-separated string list */
static int prop_strlist_eq(const void *fdt, int off, const char *name,
			   const char *const *strs, int n)
{
	char expect[512];
	size_t len = 0;
	const char *v;
	int i, vlen;

	for (i = 0; i < n; i++) {
		size_t l = strlen(strs[i]) + 1;

		if (len + l > sizeof(expect))
			return 0;
		memcpy(expect + len, strs[i], l);
		len += l;
	}
	v = fdt_getprop(fdt, off, name, &vlen);
	return v && vlen == (int)len && !memcmp(v, expect, len);
}

static const struct tref *find_tref(const struct tref *refs, int n,
				    const char *path, const char *prop,
				    uint32_t cell)
{
	int i;

	for (i = 0; i < n; i++)
		if (refs[i].cell == cell && !strcmp(refs[i].path, path) &&
		    !strcmp(refs[i].prop, prop))
			return &refs[i];
	return NULL;
}

/* Resolve the phandle stored at @val to the absolute path of its node */
static int resolve_path(const void *fdt, const void *val, char *buf,
			size_t bufsize)
{
	uint32_t ph;
	int off;

	memcpy(&ph, val, sizeof(ph));
	ph = fdt32_to_cpu(ph);
	off = fdt_node_offset_by_phandle(fdt, ph);
	CHECK(off >= 0);
	CHECK(fdt_get_path(fdt, off, buf, bufsize) == 0);
	return 0;
}

/* Compare one property, interpreting annotated cells as references */
static int check_prop(const void *merged, const void *desired,
		      const char *path, const char *name,
		      const void *mv, int ml, const void *dv, int dl,
		      const struct tref *mrefs, int nmrefs,
		      const struct tref *drefs, int ndrefs)
{
	int cells, c;

	CHECK(ml == dl);
	cells = ml / 4;
	for (c = 0; c < cells; c++) {
		const struct tref *mr, *dr;

		mr = find_tref(mrefs, nmrefs, path, name, c);
		dr = find_tref(drefs, ndrefs, path, name, c);
		if (dr) {
			char mpath[256], dpath[256];

			CHECK(mr != NULL);
			CHECK(!resolve_path(merged, (const char *)mv + 4 * c,
					    mpath, sizeof(mpath)));
			CHECK(!resolve_path(desired, (const char *)dv + 4 * c,
					    dpath, sizeof(dpath)));
			CHECK(!strcmp(mpath, dpath));
		} else {
			CHECK(mr == NULL);
			CHECK(!memcmp((const char *)mv + 4 * c,
				      (const char *)dv + 4 * c, 4));
		}
	}
	if (ml % 4)
		CHECK(!memcmp((const char *)mv + 4 * cells,
			      (const char *)dv + 4 * cells, ml % 4));
	return 0;
}

static int is_phandle_prop(const char *name)
{
	return !strcmp(name, "phandle") || !strcmp(name, "linux,phandle");
}

static void join_path(char *buf, size_t bufsize, const char *parent,
		      const char *name)
{
	if (!strcmp(parent, "/"))
		snprintf(buf, bufsize, "/%s", name);
	else
		snprintf(buf, bufsize, "%s/%s", parent, name);
}

/* Recursively verify that @merged matches @desired below this node */
static int check_node(const void *merged, const void *desired,
		      int moff, int doff, const char *path,
		      const struct tref *mrefs, int nmrefs,
		      const struct tref *drefs, int ndrefs)
{
	int poff, sub;

	fdt_for_each_property_offset(poff, desired, doff) {
		const char *name;
		const void *dv, *mv;
		int dl, ml;

		dv = fdt_getprop_by_offset(desired, poff, &name, &dl);
		CHECK(dv != NULL);
		if (is_phandle_prop(name))
			continue;
		mv = fdt_getprop(merged, moff, name, &ml);
		CHECK(mv != NULL);
		CHECK(!check_prop(merged, desired, path, name, mv, ml, dv, dl,
				  mrefs, nmrefs, drefs, ndrefs));
	}
	/* no extra properties in the merged tree */
	fdt_for_each_property_offset(poff, merged, moff) {
		const char *name;
		const void *mv;
		int ml, dl;

		mv = fdt_getprop_by_offset(merged, poff, &name, &ml);
		CHECK(mv != NULL);
		if (is_phandle_prop(name))
			continue;
		CHECK(fdt_getprop(desired, doff, name, &dl) != NULL);
	}
	/* phandle presence matches (numbering may differ) */
	{
		static const char *const phprops[] = { "phandle",
						       "linux,phandle" };
		int i;

		for (i = 0; i < 2; i++) {
			int ml, dl;
			int m_has = fdt_getprop(merged, moff, phprops[i],
						&ml) != NULL;
			int d_has = fdt_getprop(desired, doff, phprops[i],
						&dl) != NULL;

			CHECK(m_has == d_has);
		}
	}
	/* children, both directions */
	fdt_for_each_subnode(sub, desired, doff) {
		const char *name = fdt_get_name(desired, sub, NULL);
		char child[256];
		int msub;

		CHECK(name != NULL);
		if (!doff && !strcmp(name, "__symbols__"))
			continue;
		join_path(child, sizeof(child), path, name);
		msub = fdt_subnode_offset(merged, moff, name);
		CHECK(msub >= 0);
		CHECK(!check_node(merged, desired, msub, sub, child,
				  mrefs, nmrefs, drefs, ndrefs));
	}
	fdt_for_each_subnode(sub, merged, moff) {
		const char *name = fdt_get_name(merged, sub, NULL);

		CHECK(name != NULL);
		if (!moff && !strcmp(name, "__symbols__"))
			continue;
		CHECK(fdt_subnode_offset(desired, doff, name) >= 0);
	}
	return 0;
}

static int check_symbols(const void *merged, const void *desired)
{
	int msym = fdt_path_offset(merged, "/__symbols__");
	int dsym = fdt_path_offset(desired, "/__symbols__");
	int poff;

	CHECK((msym >= 0) == (dsym >= 0));
	if (msym < 0)
		return 0;
	fdt_for_each_property_offset(poff, merged, msym) {
		const char *label, *mpath, *dpath;
		int mlen, dlen;

		mpath = fdt_getprop_by_offset(merged, poff, &label, &mlen);
		CHECK(mpath != NULL);
		dpath = fdt_getprop(desired, dsym, label, &dlen);
		CHECK(dpath != NULL);
		CHECK(mlen == dlen && !memcmp(mpath, dpath, mlen));
	}
	fdt_for_each_property_offset(poff, desired, dsym) {
		const char *label, *dpath;
		int dlen, mlen;

		dpath = fdt_getprop_by_offset(desired, poff, &label, &dlen);
		CHECK(dpath != NULL);
		CHECK(fdt_getprop(merged, msym, label, &mlen) != NULL);
	}
	return 0;
}

static int check_trees(const void *merged, const void *desired,
		       const struct tref *mrefs, int nmrefs,
		       const struct tref *drefs, int ndrefs)
{
	CHECK(!check_node(merged, desired, 0, 0, "/",
			  mrefs, nmrefs, drefs, ndrefs));
	CHECK(!check_symbols(merged, desired));
	return 0;
}

/*
 * Copy @base and the overlay, merge them for real with
 * fdt_overlay_apply(), then compare the result against @desired.
 */
static int apply_and_check(const void *base, const void *ov, int ovlen,
			   void *merged, size_t merged_size,
			   const void *desired,
			   const struct tref *mrefs, int nmrefs,
			   const struct tref *drefs, int ndrefs)
{
	static unsigned char ovbuf[65536];

	CHECK(ovlen > 0 && (size_t)ovlen <= sizeof(ovbuf));
	memcpy(ovbuf, ov, ovlen);
	CHECK(fdt_open_into(base, merged, merged_size) == 0);
	CHECK(fdt_overlay_apply(merged, ovbuf) == 0);
	CHECK(fdt_check_full(merged, fdt_totalsize(merged)) == 0);
	CHECK(!check_trees(merged, desired, mrefs, nmrefs, drefs, ndrefs));
	return 0;
}

static int buf_filled(const void *buf, size_t n, unsigned char v)
{
	const unsigned char *p = buf;
	size_t i;

	for (i = 0; i < n; i++)
		if (p[i] != v)
			return 0;
	return 1;
}

/**********************************************************************/
/* diff: added/overridden properties and added nodes                  */
/**********************************************************************/

static int group_diff(void)
{
	static unsigned char base[16384], desired[16384];
	static unsigned char ov[16384], ov2[8192], merged[32768];
	char err[256];
	int ret, ret2, f0, f1, o1, nn;

	TCHK(fdt_create_empty_tree(base, sizeof(base)));
	TCHK(tnode(base, "/existing"));
	TCHK(tprop_u32(base, "/existing", "a", 0x11));
	TCHK(tprop_str(base, "/existing", "keep", "k"));
	TCHK(tnode(base, "/other"));
	TCHK(tphandle(base, "/other", 5));

	TCHK(fdt_create_empty_tree(desired, sizeof(desired)));
	TCHK(tnode(desired, "/existing"));
	TCHK(tprop_u32(desired, "/existing", "a", 0x22));
	TCHK(tprop_str(desired, "/existing", "keep", "k"));
	TCHK(tprop_str(desired, "/existing", "b", "hi"));
	TCHK(tprop(desired, "/existing", "z", NULL, 0));
	TCHK(tnode(desired, "/other"));
	TCHK(tphandle(desired, "/other", 5));
	TCHK(tnode(desired, "/newnode"));
	TCHK(tprop_u32(desired, "/newnode", "x", 1));

	ret = fdt_overlay_delta(base, desired, NULL, 0, NULL, 0,
				ov, sizeof(ov), err, sizeof(err));
	CHECK(ret > 0);

	/* fragments are ordered by sorted target path: "/" < "/existing" */
	f0 = fdt_path_offset(ov, "/fragment@0");
	f1 = fdt_path_offset(ov, "/fragment@1");
	CHECK(f0 >= 0);
	CHECK(f1 >= 0);
	CHECK(fdt_path_offset(ov, "/fragment@2") == -FDT_ERR_NOTFOUND);
	CHECK(prop_str_eq(ov, f0, "target-path", "/"));
	CHECK(prop_str_eq(ov, f1, "target-path", "/existing"));

	/* overridden and added properties, zero-length one included */
	o1 = fdt_path_offset(ov, "/fragment@1/__overlay__");
	CHECK(o1 >= 0);
	CHECK(prop_u32_eq(ov, o1, "a", 0x22));
	CHECK(prop_str_eq(ov, o1, "b", "hi"));
	{
		const void *z;
		int zlen;

		z = fdt_getprop(ov, o1, "z", &zlen);
		CHECK(z != NULL);
		CHECK(zlen == 0);
	}
	/* unchanged properties are not emitted */
	{
		int klen;

		CHECK(fdt_getprop(ov, o1, "keep", &klen) == NULL);
	}

	/* the added node hangs off the fragment targeting "/" */
	nn = fdt_path_offset(ov, "/fragment@0/__overlay__/newnode");
	CHECK(nn >= 0);
	CHECK(prop_u32_eq(ov, nn, "x", 1));

	/* no fixup metadata without references */
	CHECK(fdt_path_offset(ov, "/__fixups__") == -FDT_ERR_NOTFOUND);
	CHECK(fdt_path_offset(ov, "/__local_fixups__") == -FDT_ERR_NOTFOUND);

	/* a different output buffer size yields identical bytes */
	ret2 = fdt_overlay_delta(base, desired, NULL, 0, NULL, 0,
				 ov2, sizeof(ov2), err, sizeof(err));
	CHECK(ret2 == ret);
	CHECK(!memcmp(ov, ov2, ret));

	/* the needed size can be queried with a NULL buffer */
	ret2 = fdt_overlay_delta(base, desired, NULL, 0, NULL, 0,
				 NULL, 0, err, sizeof(err));
	CHECK(ret2 == ret);

	/* merge for real and compare against the desired tree */
	CHECK(!apply_and_check(base, ov, ret, merged, sizeof(merged),
			       desired, NULL, 0, NULL, 0));
	return 0;
}

/**********************************************************************/
/* external: references to pre-existing nodes, plain values in between */
/**********************************************************************/

static int group_external(void)
{
	static unsigned char base[16384], desired[16384];
	static unsigned char ov[16384], merged[32768];
	static const struct fdt_delta_ref brefs[] = {
		{ "/consumer", "old", 0 },
	};
	static const struct fdt_delta_ref drefs[] = {
		{ "/consumer", "old", 0 },
		{ "/consumer", "mixed", 0 },
		{ "/consumer", "mixed", 2 },
	};
	static const struct tref refs[] = {
		{ "/consumer", "old", 0 },
		{ "/consumer", "mixed", 0 },
		{ "/consumer", "mixed", 2 },
	};
	static const char *const aclk_entries[] = {
		"/fragment@0/__overlay__:mixed:0",
		"/fragment@0/__overlay__:old:0",
	};
	static const char *const uart_entries[] = {
		"/fragment@0/__overlay__:mixed:8",
	};
	char err[256];
	int ret, fx, off;

	TCHK(fdt_create_empty_tree(base, sizeof(base)));
	TCHK(tnode(base, "/clk"));
	TCHK(tphandle(base, "/clk", 3));
	TCHK(tnode(base, "/uart"));
	TCHK(tphandle(base, "/uart", 2));
	TCHK(tnode(base, "/consumer"));
	TCHK(tprop_cells(base, "/consumer", "old",
			 (uint32_t[]){ 2 }, 1));
	TCHK(tprop_u32(base, "/consumer", "id", 0xabc));
	/* two labels for /clk: the smaller one must win */
	TCHK(tsymbol(base, "zclk", "/clk"));
	TCHK(tsymbol(base, "aclk", "/clk"));
	TCHK(tsymbol(base, "uart", "/uart"));

	TCHK(fdt_create_empty_tree(desired, sizeof(desired)));
	TCHK(tnode(desired, "/clk"));
	TCHK(tphandle(desired, "/clk", 3));
	TCHK(tnode(desired, "/uart"));
	TCHK(tphandle(desired, "/uart", 2));
	TCHK(tnode(desired, "/consumer"));
	/* "old" now points at /clk instead of /uart */
	TCHK(tprop_cells(desired, "/consumer", "old",
			 (uint32_t[]){ 3 }, 1));
	TCHK(tprop_u32(desired, "/consumer", "id", 0xabc));
	/* reference array with a plain value in the middle */
	TCHK(tprop_cells(desired, "/consumer", "mixed",
			 (uint32_t[]){ 3, 0xdead, 2 }, 3));
	TCHK(tsymbol(desired, "zclk", "/clk"));
	TCHK(tsymbol(desired, "aclk", "/clk"));
	TCHK(tsymbol(desired, "uart", "/uart"));

	ret = fdt_overlay_delta(base, desired, brefs, 1, drefs, 3,
				ov, sizeof(ov), err, sizeof(err));
	CHECK(ret > 0);

	/* external fixups use the base tree's labels */
	fx = fdt_path_offset(ov, "/__fixups__");
	CHECK(fx >= 0);
	CHECK(prop_strlist_eq(ov, fx, "aclk", aclk_entries, 2));
	{
		int zlen;

		CHECK(fdt_getprop(ov, fx, "zclk", &zlen) == NULL);
	}
	CHECK(prop_strlist_eq(ov, fx, "uart", uart_entries, 1));
	CHECK(fdt_path_offset(ov, "/__local_fixups__") == -FDT_ERR_NOTFOUND);

	/* the reference cells carry placeholder zeros until applied */
	off = fdt_path_offset(ov, "/fragment@0/__overlay__");
	CHECK(off >= 0);
	CHECK(prop_u32_eq(ov, off, "old", 0));
	{
		const fdt32_t *mixed;
		int mlen;

		mixed = fdt_getprop(ov, off, "mixed", &mlen);
		CHECK(mixed != NULL);
		CHECK(mlen == 12);
		CHECK(fdt32_to_cpu(mixed[0]) == 0);
		CHECK(fdt32_to_cpu(mixed[1]) == 0xdead);
		CHECK(fdt32_to_cpu(mixed[2]) == 0);
	}

	CHECK(!apply_and_check(base, ov, ret, merged, sizeof(merged),
			       desired, refs, 3, refs, 3));

	/* the plain value in the middle survives verbatim */
	off = fdt_path_offset(merged, "/consumer");
	CHECK(off >= 0);
	{
		const fdt32_t *mixed;
		int mlen;

		mixed = fdt_getprop(merged, off, "mixed", &mlen);
		CHECK(mixed != NULL);
		CHECK(mlen == 12);
		CHECK(fdt32_to_cpu(mixed[1]) == 0xdead);
	}
	return 0;
}

/**********************************************************************/
/* local: references between added nodes, mixed with existing ones    */
/**********************************************************************/

static int group_local(void)
{
	static unsigned char base[16384], desired[16384];
	static unsigned char ov[16384], merged[32768];
	static const struct fdt_delta_ref drefs[] = {
		{ "/x/n1", "peer", 0 },
		{ "/x/n1", "back", 0 },
		{ "/y/n2", "peer", 0 },
	};
	static const struct tref refs[] = {
		{ "/x/n1", "peer", 0 },
		{ "/x/n1", "back", 0 },
		{ "/y/n2", "peer", 0 },
	};
	static const char *const a_entries[] = {
		"/fragment@0/__overlay__/x/n1:back:0",
	};
	char err[256];
	int ret, lf, n1, n2, fx;

	TCHK(fdt_create_empty_tree(base, sizeof(base)));
	TCHK(tnode(base, "/a"));
	TCHK(tphandle(base, "/a", 1));
	TCHK(tsymbol(base, "a", "/a"));

	TCHK(fdt_create_empty_tree(desired, sizeof(desired)));
	TCHK(tnode(desired, "/a"));
	TCHK(tphandle(desired, "/a", 1));
	/* two added nodes in different branches pointing at each other */
	TCHK(tnode(desired, "/x/n1"));
	TCHK(tphandle(desired, "/x/n1", 7));
	TCHK(tprop_cells(desired, "/x/n1", "peer", (uint32_t[]){ 8 }, 1));
	TCHK(tprop_cells(desired, "/x/n1", "back", (uint32_t[]){ 1 }, 1));
	TCHK(tnode(desired, "/y/n2"));
	TCHK(tphandle(desired, "/y/n2", 8));
	TCHK(tprop_cells(desired, "/y/n2", "peer", (uint32_t[]){ 7 }, 1));
	TCHK(tsymbol(desired, "a", "/a"));

	ret = fdt_overlay_delta(base, desired, NULL, 0, drefs, 3,
				ov, sizeof(ov), err, sizeof(err));
	CHECK(ret > 0);

	/*
	 * Local phandles are assigned by sorted absolute path, not by
	 * the desired tree's numbering.  All added nodes take part:
	 * /x -> 1, /x/n1 -> 2, /y -> 3, /y/n2 -> 4; only nodes that
	 * carry or need a phandle get the property emitted.
	 */
	n1 = fdt_path_offset(ov, "/fragment@0/__overlay__/x/n1");
	n2 = fdt_path_offset(ov, "/fragment@0/__overlay__/y/n2");
	CHECK(n1 >= 0);
	CHECK(n2 >= 0);
	CHECK(prop_u32_eq(ov, n1, "phandle", 2));
	CHECK(prop_u32_eq(ov, n2, "phandle", 4));
	CHECK(prop_u32_eq(ov, n1, "peer", 4));
	CHECK(prop_u32_eq(ov, n2, "peer", 2));
	CHECK(prop_u32_eq(ov, n1, "back", 0));

	/* local fixups mirror the overlay node hierarchy */
	lf = fdt_path_offset(ov, "/__local_fixups__/fragment@0/__overlay__/x/n1");
	CHECK(lf >= 0);
	CHECK(prop_u32_eq(ov, lf, "peer", 0));
	lf = fdt_path_offset(ov, "/__local_fixups__/fragment@0/__overlay__/y/n2");
	CHECK(lf >= 0);
	CHECK(prop_u32_eq(ov, lf, "peer", 0));
	{
		int l;
		/* "back" points at a base node: external, not local */
		lf = fdt_path_offset(ov, "/__local_fixups__/fragment@0/__overlay__/x/n1");
		CHECK(lf >= 0);
		CHECK(fdt_getprop(ov, lf, "back", &l) == NULL);
	}

	/* the reference to the pre-existing /a uses its label */
	fx = fdt_path_offset(ov, "/__fixups__");
	CHECK(fx >= 0);
	CHECK(prop_strlist_eq(ov, fx, "a", a_entries, 1));

	CHECK(!apply_and_check(base, ov, ret, merged, sizeof(merged),
			       desired, refs, 3, refs, 3));
	return 0;
}

/**********************************************************************/
/* renumber: pure phandle renumbering produces an empty overlay       */
/**********************************************************************/

static int group_renumber(void)
{
	static unsigned char base[16384], desired[16384];
	static unsigned char ov[16384], merged[32768];
	static const struct fdt_delta_ref brefs[] = {
		{ "/q", "ref", 0 },
	};
	static const struct fdt_delta_ref drefs[] = {
		{ "/q", "ref", 0 },
	};
	static const struct tref refs[] = {
		{ "/q", "ref", 0 },
	};
	char err[256];
	int ret, off;

	TCHK(fdt_create_empty_tree(base, sizeof(base)));
	TCHK(tnode(base, "/p"));
	TCHK(tphandle(base, "/p", 10));
	TCHK(tnode(base, "/q"));
	TCHK(tphandle(base, "/q", 20));
	TCHK(tprop_cells(base, "/q", "ref", (uint32_t[]){ 10 }, 1));
	TCHK(tprop_u32(base, "/q", "v", 42));
	TCHK(tsymbol(base, "p", "/p"));
	TCHK(tsymbol(base, "q", "/q"));

	/* identical tree, only the phandle numbers changed */
	TCHK(fdt_create_empty_tree(desired, sizeof(desired)));
	TCHK(tnode(desired, "/p"));
	TCHK(tphandle(desired, "/p", 110));
	TCHK(tnode(desired, "/q"));
	TCHK(tphandle(desired, "/q", 120));
	TCHK(tprop_cells(desired, "/q", "ref", (uint32_t[]){ 110 }, 1));
	TCHK(tprop_u32(desired, "/q", "v", 42));
	TCHK(tsymbol(desired, "p", "/p"));
	TCHK(tsymbol(desired, "q", "/q"));

	ret = fdt_overlay_delta(base, desired, brefs, 1, drefs, 1,
				ov, sizeof(ov), err, sizeof(err));
	CHECK(ret > 0);

	/* a valid but empty overlay */
	CHECK(fdt_check_full(ov, ret) == 0);
	CHECK(fdt_path_offset(ov, "/fragment@0") == -FDT_ERR_NOTFOUND);
	CHECK(fdt_path_offset(ov, "/__fixups__") == -FDT_ERR_NOTFOUND);
	CHECK(fdt_path_offset(ov, "/__local_fixups__") == -FDT_ERR_NOTFOUND);
	CHECK(fdt_path_offset(ov, "/__symbols__") == -FDT_ERR_NOTFOUND);

	/* applying it changes nothing */
	CHECK(fdt_open_into(base, merged, sizeof(merged)) == 0);
	{
		static unsigned char ovcopy[16384];

		CHECK(ret <= (int)sizeof(ovcopy));
		memcpy(ovcopy, ov, ret);
		CHECK(fdt_overlay_apply(merged, ovcopy) == 0);
	}
	off = fdt_path_offset(merged, "/p");
	CHECK(off >= 0);
	CHECK(prop_u32_eq(merged, off, "phandle", 10));
	off = fdt_path_offset(merged, "/q");
	CHECK(off >= 0);
	CHECK(prop_u32_eq(merged, off, "phandle", 20));
	CHECK(prop_u32_eq(merged, off, "ref", 10));

	CHECK(!check_trees(merged, desired, refs, 1, refs, 1));
	return 0;
}

/**********************************************************************/
/* roundtrip: apply, then diff again using the newly exported label   */
/**********************************************************************/

static int group_roundtrip(void)
{
	static unsigned char base[16384], desired1[16384], desired2[16384];
	static unsigned char ov1[16384], ov2[16384];
	static unsigned char merged1[32768], merged2[32768];
	static const struct fdt_delta_ref drefs1[] = {
		{ "/soc/spi", "clock", 0 },
	};
	static const struct tref refs1[] = {
		{ "/soc/spi", "clock", 0 },
	};
	static const struct fdt_delta_ref drefs2[] = {
		{ "/user", "spidev", 0 },
	};
	static const struct tref refs2[] = {
		{ "/user", "spidev", 0 },
	};
	static const char *const spi_entries[] = {
		"/fragment@0/__overlay__/user:spidev:0",
	};
	char err[256];
	int ret1, ret2, off, sym;

	TCHK(fdt_create_empty_tree(base, sizeof(base)));
	TCHK(tnode(base, "/soc/uart"));
	TCHK(tphandle(base, "/soc/uart", 2));
	TCHK(tprop_str(base, "/soc/uart", "status", "okay"));
	TCHK(tnode(base, "/soc/clk"));
	TCHK(tphandle(base, "/soc/clk", 3));
	TCHK(tprop_u32(base, "/soc/clk", "freq", 100));
	TCHK(tsymbol(base, "uart", "/soc/uart"));
	TCHK(tsymbol(base, "clk", "/soc/clk"));

	/* first desired tree: renumber, override, add a labelled node */
	TCHK(fdt_create_empty_tree(desired1, sizeof(desired1)));
	TCHK(tnode(desired1, "/soc/uart"));
	TCHK(tphandle(desired1, "/soc/uart", 22));
	TCHK(tprop_str(desired1, "/soc/uart", "status", "disabled"));
	TCHK(tnode(desired1, "/soc/clk"));
	TCHK(tphandle(desired1, "/soc/clk", 33));
	TCHK(tprop_u32(desired1, "/soc/clk", "freq", 100));
	TCHK(tnode(desired1, "/soc/spi"));
	TCHK(tphandle(desired1, "/soc/spi", 44));
	TCHK(tprop_cells(desired1, "/soc/spi", "clock",
			 (uint32_t[]){ 33 }, 1));
	TCHK(tsymbol(desired1, "uart", "/soc/uart"));
	TCHK(tsymbol(desired1, "clk", "/soc/clk"));
	TCHK(tsymbol(desired1, "spi", "/soc/spi"));

	ret1 = fdt_overlay_delta(base, desired1, NULL, 0, drefs1, 1,
				 ov1, sizeof(ov1), err, sizeof(err));
	CHECK(ret1 > 0);

	/* the new label points at the overlay path of the added node */
	sym = fdt_path_offset(ov1, "/__symbols__");
	CHECK(sym >= 0);
	CHECK(prop_str_eq(ov1, sym, "spi", "/fragment@0/__overlay__/spi"));

	/* the override landed in the fragment for the existing node */
	off = fdt_path_offset(ov1, "/fragment@1/__overlay__");
	CHECK(off >= 0);
	CHECK(prop_str_eq(ov1, off, "status", "disabled"));

	/* merge it for real */
	CHECK(!apply_and_check(base, ov1, ret1, merged1, sizeof(merged1),
			       desired1, refs1, 1, refs1, 1));

	/* the merge exported the new label with its final path */
	sym = fdt_path_offset(merged1, "/__symbols__");
	CHECK(sym >= 0);
	CHECK(prop_str_eq(merged1, sym, "spi", "/soc/spi"));

	/* second desired tree: reference the added node through its label */
	TCHK(fdt_open_into(desired1, desired2, sizeof(desired2)));
	TCHK(tnode(desired2, "/user"));
	TCHK(tphandle(desired2, "/user", 55));
	TCHK(tprop_cells(desired2, "/user", "spidev",
			 (uint32_t[]){ 44 }, 1));

	ret2 = fdt_overlay_delta(merged1, desired2, NULL, 0, drefs2, 1,
				 ov2, sizeof(ov2), err, sizeof(err));
	CHECK(ret2 > 0);

	/* the second overlay resolves the reference via the new label */
	off = fdt_path_offset(ov2, "/__fixups__");
	CHECK(off >= 0);
	CHECK(prop_strlist_eq(ov2, off, "spi", spi_entries, 1));

	/* merge the second overlay and compare against the final tree */
	CHECK(!apply_and_check(merged1, ov2, ret2, merged2, sizeof(merged2),
			       desired2, refs2, 1, refs2, 1));
	return 0;
}

/**********************************************************************/
/* reject: unsupported or malformed inputs leave the buffer untouched */
/**********************************************************************/

/* Build a minimal pair of trees with one referenceable node */
static int reject_base(void *base, void *desired)
{
	TCHK(fdt_create_empty_tree(base, 16384));
	TCHK(tnode(base, "/n"));
	TCHK(tprop_cells(base, "/n", "r", (uint32_t[]){ 1 }, 1));
	TCHK(tnode(base, "/t"));
	TCHK(tphandle(base, "/t", 1));
	TCHK(fdt_create_empty_tree(desired, 16384));
	TCHK(tnode(desired, "/n"));
	TCHK(tprop_cells(desired, "/n", "r", (uint32_t[]){ 1 }, 1));
	TCHK(tnode(desired, "/t"));
	TCHK(tphandle(desired, "/t", 1));
	return 0;
}

static int group_reject(void)
{
	static unsigned char base[16384], desired[16384], ov[16384];
	char err[256];
	int ret, off;

	/* 1. deleting a node is rejected and reports the path */
	TCHK(reject_base(base, desired));
	TCHK(tnode(base, "/gone"));
	memset(ov, 0xaa, sizeof(ov));
	ret = fdt_overlay_delta(base, desired, NULL, 0, NULL, 0,
				ov, sizeof(ov), err, sizeof(err));
	CHECK(ret == -FDT_ERR_NOTFOUND);
	CHECK(!strcmp(err, "/gone"));
	CHECK(buf_filled(ov, sizeof(ov), 0xaa));

	/* 2. deleting a property is rejected and reports path:property */
	TCHK(reject_base(base, desired));
	TCHK(tprop_u32(base, "/n", "x", 7));
	memset(ov, 0xaa, sizeof(ov));
	ret = fdt_overlay_delta(base, desired, NULL, 0, NULL, 0,
				ov, sizeof(ov), err, sizeof(err));
	CHECK(ret == -FDT_ERR_NOTFOUND);
	CHECK(!strcmp(err, "/n:x"));
	CHECK(buf_filled(ov, sizeof(ov), 0xaa));

	/* 3. deleting a phandle is a deletion too */
	TCHK(fdt_create_empty_tree(base, sizeof(base)));
	TCHK(tnode(base, "/t"));
	TCHK(tphandle(base, "/t", 1));
	TCHK(fdt_create_empty_tree(desired, sizeof(desired)));
	TCHK(tnode(desired, "/t"));
	memset(ov, 0xaa, sizeof(ov));
	ret = fdt_overlay_delta(base, desired, NULL, 0, NULL, 0,
				ov, sizeof(ov), err, sizeof(err));
	CHECK(ret == -FDT_ERR_NOTFOUND);
	CHECK(!strcmp(err, "/t:phandle"));
	CHECK(buf_filled(ov, sizeof(ov), 0xaa));

	/* 4. a differing reserve map is rejected */
	TCHK(reject_base(base, desired));
	{
		static unsigned char drsv[32768];

		TCHK(fdt_open_into(desired, drsv, sizeof(drsv)));
		TCHK(fdt_add_mem_rsv(drsv, 0x1000, 0x200));
		memset(ov, 0xaa, sizeof(ov));
		ret = fdt_overlay_delta(base, drsv, NULL, 0, NULL, 0,
					ov, sizeof(ov), err, sizeof(err));
		CHECK(ret == -FDT_ERR_BADVALUE);
		CHECK(buf_filled(ov, sizeof(ov), 0xaa));
	}

	/* 5. annotating the same location twice is rejected */
	TCHK(reject_base(base, desired));
	{
		static const struct fdt_delta_ref dup[] = {
			{ "/n", "r", 0 },
			{ "/n", "r", 0 },
		};

		memset(ov, 0xaa, sizeof(ov));
		ret = fdt_overlay_delta(base, desired, NULL, 0, dup, 2,
					ov, sizeof(ov), err, sizeof(err));
		CHECK(ret == -FDT_ERR_EXISTS);
		CHECK(buf_filled(ov, sizeof(ov), 0xaa));
	}

	/* 6. an annotation past the end of the property is rejected */
	TCHK(reject_base(base, desired));
	{
		static const struct fdt_delta_ref oob[] = {
			{ "/n", "r", 1 },
		};

		memset(ov, 0xaa, sizeof(ov));
		ret = fdt_overlay_delta(base, desired, NULL, 0, oob, 1,
					ov, sizeof(ov), err, sizeof(err));
		CHECK(ret == -FDT_ERR_BADVALUE);
		CHECK(buf_filled(ov, sizeof(ov), 0xaa));
	}

	/* 7. an annotation that does not resolve to a node is rejected */
	TCHK(reject_base(base, desired));
	TCHK(tprop_cells(desired, "/n", "r", (uint32_t[]){ 0x999 }, 1));
	{
		static const struct fdt_delta_ref unres[] = {
			{ "/n", "r", 0 },
		};

		memset(ov, 0xaa, sizeof(ov));
		ret = fdt_overlay_delta(base, desired, NULL, 0, unres, 1,
					ov, sizeof(ov), err, sizeof(err));
		CHECK(ret == -FDT_ERR_BADPHANDLE);
		CHECK(buf_filled(ov, sizeof(ov), 0xaa));
	}

	/* 8. a phandle owned by two nodes is rejected */
	TCHK(reject_base(base, desired));
	TCHK(tnode(desired, "/t2"));
	TCHK(tphandle(desired, "/t2", 1));
	{
		static const struct fdt_delta_ref dupph[] = {
			{ "/n", "r", 0 },
		};

		memset(ov, 0xaa, sizeof(ov));
		ret = fdt_overlay_delta(base, desired, NULL, 0, dupph, 1,
					ov, sizeof(ov), err, sizeof(err));
		CHECK(ret == -FDT_ERR_BADPHANDLE);
		CHECK(buf_filled(ov, sizeof(ov), 0xaa));
	}

	/* 9. a zero phandle value is invalid */
	TCHK(reject_base(base, desired));
	TCHK(tprop_cells(desired, "/n", "r", (uint32_t[]){ 0 }, 1));
	{
		static const struct fdt_delta_ref zero[] = {
			{ "/n", "r", 0 },
		};

		memset(ov, 0xaa, sizeof(ov));
		ret = fdt_overlay_delta(base, desired, NULL, 0, zero, 1,
					ov, sizeof(ov), err, sizeof(err));
		CHECK(ret == -FDT_ERR_BADPHANDLE);
		CHECK(buf_filled(ov, sizeof(ov), 0xaa));
	}

	/* 10. retargeting an existing label is rejected */
	TCHK(reject_base(base, desired));
	TCHK(tsymbol(base, "x", "/n"));
	TCHK(tsymbol(desired, "x", "/t"));
	memset(ov, 0xaa, sizeof(ov));
	ret = fdt_overlay_delta(base, desired, NULL, 0, NULL, 0,
				ov, sizeof(ov), err, sizeof(err));
	CHECK(ret == -FDT_ERR_BADVALUE);
	CHECK(strstr(err, "x") != NULL);
	CHECK(buf_filled(ov, sizeof(ov), 0xaa));

	/* 11. dropping an existing label is rejected */
	TCHK(reject_base(base, desired));
	TCHK(tsymbol(base, "x", "/n"));
	memset(ov, 0xaa, sizeof(ov));
	ret = fdt_overlay_delta(base, desired, NULL, 0, NULL, 0,
				ov, sizeof(ov), err, sizeof(err));
	CHECK(ret == -FDT_ERR_NOTFOUND);
	CHECK(strstr(err, "x") != NULL);
	CHECK(buf_filled(ov, sizeof(ov), 0xaa));

	/* 12. a reference to an unlabelled base node reports both ends */
	TCHK(reject_base(base, desired));
	TCHK(tnode(desired, "/c"));
	TCHK(tprop_cells(desired, "/c", "ref", (uint32_t[]){ 1 }, 1));
	{
		static const struct fdt_delta_ref nolabel[] = {
			{ "/c", "ref", 0 },
		};

		memset(ov, 0xaa, sizeof(ov));
		ret = fdt_overlay_delta(base, desired, NULL, 0, nolabel, 1,
					ov, sizeof(ov), err, sizeof(err));
		CHECK(ret == -FDT_ERR_NOTFOUND);
	CHECK(strstr(err, "/c:ref") != NULL);
	CHECK(strstr(err, "/t") != NULL);
	CHECK(buf_filled(ov, sizeof(ov), 0xaa));
	}

	/* 13. a small buffer reports the needed size and stays untouched */
	TCHK(reject_base(base, desired));
	TCHK(tprop_u32(desired, "/n", "v", 9));
	TCHK(tnode(desired, "/new"));
	ret = fdt_overlay_delta(base, desired, NULL, 0, NULL, 0,
				ov, sizeof(ov), err, sizeof(err));
	CHECK(ret > 0);
	memset(ov, 0xaa, sizeof(ov));
	{
		int ret2 = fdt_overlay_delta(base, desired, NULL, 0, NULL, 0,
					     ov, ret - 1, err, sizeof(err));

		CHECK(ret2 == ret);
		CHECK(buf_filled(ov, sizeof(ov), 0xaa));
		ret2 = fdt_overlay_delta(base, desired, NULL, 0, NULL, 0,
					 NULL, 0, err, sizeof(err));
		CHECK(ret2 == ret);
	}

	/* 14. overlapping input and output memory is rejected */
	TCHK(reject_base(base, desired));
	{
		int before, blen;
		const fdt32_t *bv;

		off = fdt_path_offset(base, "/n");
		CHECK(off >= 0);
		bv = fdt_getprop(base, off, "r", &blen);
		CHECK(bv != NULL && blen == 4);
		before = fdt32_to_cpu(*bv);
		ret = fdt_overlay_delta(base, desired, NULL, 0, NULL, 0,
					base, 128, err, sizeof(err));
		CHECK(ret == -FDT_ERR_BADVALUE);
		off = fdt_path_offset(base, "/n");
		CHECK(off >= 0);
		bv = fdt_getprop(base, off, "r", &blen);
		CHECK(bv != NULL && blen == 4);
		CHECK((int)fdt32_to_cpu(*bv) == before);
	}

	return 0;
}

/**********************************************************************/
/* Dispatch                                                           */
/**********************************************************************/

int overlay_delta_case(const char *group, unsigned *checks)
{
	g_group = group;
	g_checks = checks;

	if (!strcmp(group, "diff"))
		return group_diff();
	if (!strcmp(group, "external"))
		return group_external();
	if (!strcmp(group, "local"))
		return group_local();
	if (!strcmp(group, "renumber"))
		return group_renumber();
	if (!strcmp(group, "roundtrip"))
		return group_roundtrip();
	if (!strcmp(group, "reject"))
		return group_reject();
	fprintf(stderr, "unknown group: %s\n", group);
	return -1;
}
