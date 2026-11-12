// SPDX-License-Identifier: (GPL-2.0-or-later OR BSD-2-Clause)
/*
 * fdt_overlay_delta() example.
 *
 * Two complete device trees are built in memory: a base tree and a
 * desired tree that adds a button node referencing the pre-existing
 * LED node.  fdt_overlay_delta() turns the difference into an overlay
 * which is then applied to a copy of the base tree with
 * fdt_overlay_apply().
 *
 * Build from the repository root:
 *	cc -Ilibfdt examples/fdt_overlay_delta.c libfdt/fdt.c \
 *	    libfdt/fdt_ro.c libfdt/fdt_wip.c libfdt/fdt_sw.c libfdt/fdt_rw.c \
 *	    libfdt/fdt_strerror.c libfdt/fdt_empty_tree.c \
 *	    libfdt/fdt_addresses.c libfdt/fdt_overlay.c \
 *	    libfdt/fdt_overlay_delta.c libfdt/fdt_check.c -o od_example
 *	./od_example
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libfdt.h>
#include <fdt_overlay_delta.h>

static void die(const char *what, int err)
{
	fprintf(stderr, "%s: %s\n", what, fdt_strerror(err));
	exit(1);
}

int main(void)
{
	static unsigned char base[4096], desired[4096];
	static unsigned char overlay[4096], merged[8192];
	/* tell the generator which cells contain phandle references */
	static const struct fdt_delta_ref desired_refs[] = {
		{ "/button", "gpios", 0 },	/* first cell of "gpios" */
	};
	char errpath[128];
	int ret, off;

	/* base tree: an LED node with phandle 1, exported as "led" */
	ret = fdt_create_empty_tree(base, sizeof(base));
	if (ret)
		die("create base", ret);
	off = fdt_add_subnode(base, 0, "led");
	if (off < 0)
		die("add /led", off);
	ret = fdt_setprop_u32(base, off, "phandle", 1);
	if (ret)
		die("set phandle", ret);
	off = fdt_add_subnode(base, 0, "__symbols__");
	if (off < 0)
		die("add /__symbols__", off);
	ret = fdt_setprop_string(base, off, "led", "/led");
	if (ret)
		die("set symbol", ret);

	/* desired tree: same, plus a button referencing the LED */
	memcpy(desired, base, fdt_totalsize(base));
	ret = fdt_open_into(desired, desired, sizeof(desired));
	if (ret)
		die("open desired", ret);
	off = fdt_add_subnode(desired, 0, "button");
	if (off < 0)
		die("add /button", off);
	ret = fdt_setprop_u32(desired, off, "phandle", 7);
	if (ret)
		die("set phandle", ret);
	{
		fdt32_t gpios[2] = { cpu_to_fdt32(1), /* reference to /led */
				     cpu_to_fdt32(0x12) /* plain flags */ };

		ret = fdt_setprop(desired, off, "gpios", gpios, sizeof(gpios));
		if (ret)
			die("set gpios", ret);
	}

	/* generate the overlay */
	ret = fdt_overlay_delta(base, desired, NULL, 0,
				desired_refs, 1,
				overlay, sizeof(overlay),
				errpath, sizeof(errpath));
	if (ret < 0)
		die(errpath[0] ? errpath : "fdt_overlay_delta", ret);
	printf("generated overlay: %d bytes\n", ret);

	/* apply it to a copy of the base tree */
	ret = fdt_open_into(base, merged, sizeof(merged));
	if (ret)
		die("open merged", ret);
	ret = fdt_overlay_apply(merged, overlay);
	if (ret)
		die("fdt_overlay_apply", ret);

	/* show the result */
	off = fdt_path_offset(merged, "/button");
	if (off < 0)
		die("find /button", off);
	{
		const fdt32_t *gpios;
		int len;

		gpios = fdt_getprop(merged, off, "gpios", &len);
		if (!gpios || len != 8)
			die("get gpios", len);
		printf("/button gpios = <0x%x 0x%x>\n",
		       fdt32_to_cpu(gpios[0]), fdt32_to_cpu(gpios[1]));
	}
	return 0;
}
