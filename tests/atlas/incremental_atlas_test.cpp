/*
	Unit tests for the incremental atlas interface and for the rollback
	guarantee of empty_spaces::insert.

	All layout checks use plain, independently written rect helpers -
	never the packing functions themselves.
*/

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <rectpack2D/incremental_atlas.h>
#include <rectpack2D/empty_space_allocators.h>

using namespace rectpack2D;

static int failures = 0;

#define CHECK(cond) do { \
	if (!(cond)) { \
		std::cerr << "CHECK failed (" << __LINE__ << "): " << #cond << '\n'; \
		++failures; \
	} \
} while (0)

static bool same_rect(const rect_xywh& a, const int x, const int y, const int w, const int h) {
	return a.x == x && a.y == y && a.w == w && a.h == h;
}

/* Independent half-open-interval intersection, written for these tests. */
static bool overlaps(const rect_xywh& a, const rect_xywh& b) {
	return a.x < b.x + b.w && b.x < a.x + a.w
		&& a.y < b.y + b.h && b.y < a.y + a.h;
}

static bool inside_canvas(const rect_xywh& r, const int canvas_w, const int canvas_h) {
	return r.x >= 0 && r.y >= 0
		&& r.x + r.w <= canvas_w
		&& r.y + r.h <= canvas_h;
}

template <class Provider>
static std::string serialize_spaces(const Provider& provider) {
	std::ostringstream out;

	for (int i = 0; i < static_cast<int>(provider.get_count()); ++i) {
		const auto& s = provider.get(i);
		out << s.x << ',' << s.y << ',' << s.w << ',' << s.h << ';';
	}

	return out.str();
}

static std::string serialize_entries(const std::vector<atlas_entry>& entries) {
	std::ostringstream out;

	for (const auto& e : entries) {
		out << e.id
			<< " orig=" << e.original_width << 'x' << e.original_height
			<< " p=" << e.padding
			<< " outer=" << e.layout.outer.x << ',' << e.layout.outer.y
			<< ',' << e.layout.outer.w << ',' << e.layout.outer.h
			<< " content=" << e.layout.content.x << ',' << e.layout.content.y
			<< ',' << e.layout.content.w << ',' << e.layout.content.h
			<< " rotated=" << (e.layout.rotated ? 1 : 0)
			<< '|';
	}

	return out.str();
}

static void check_frame_invariants(
	const atlas_entry& e,
	const int canvas_w = 4096,
	const int canvas_h = 4096
) {
	const auto& o = e.layout.outer;
	const auto& c = e.layout.content;

	CHECK(c.x == o.x + e.padding);
	CHECK(c.y == o.y + e.padding);

	/* Content dims are the original ones, swapped exactly once when rotated. */

	if (e.layout.rotated) {
		CHECK(c.w == e.original_height);
		CHECK(c.h == e.original_width);
	}
	else {
		CHECK(c.w == e.original_width);
		CHECK(c.h == e.original_height);
	}

	CHECK(c.x + c.w <= o.x + o.w);
	CHECK(c.y + c.h <= o.y + o.h);
	CHECK(inside_canvas(o, canvas_w, canvas_h));
}

static void test_empty_spaces_rollback() {
	/* The exact scenario from tests/atlas/baseline.in. */

	empty_spaces<false, static_empty_spaces<1>> bin(rect_wh(8, 6));

	const auto first = bin.insert(rect_wh(3, 2));
	CHECK(!first.has_value());

	/* A failed insert must leave the single original space untouched. */

	CHECK(bin.get_spaces().get_count() == 1);
	const auto& remaining = bin.get_spaces().get(0);
	CHECK(same_rect(remaining, 0, 0, 8, 6));
	CHECK(bin.get_rects_aabb().w == 0);
	CHECK(bin.get_rects_aabb().h == 0);

	/* And the very next insertion must still work. */

	const auto full = bin.insert(rect_wh(8, 6));
	CHECK(full.has_value());
	CHECK(same_rect(*full, 0, 0, 8, 6));
	CHECK(bin.get_spaces().get_count() == 0);

	/*
		Two-slot provider: the first split is added successfully and only the
		second add fails, so the rollback must also undo an already-added split
		and restore the exact traversal order of a multi-space provider.
	*/

	/*
		The capacity-1 case above already fails at s == 1: the first split is
		added, the second add runs out of room and both are rolled back.
		A two-slot provider has just enough room for both splits, so the same
		insert succeeds - verify that the normal path is undisturbed.
	*/

	empty_spaces<false, static_empty_spaces<2>> two(rect_wh(8, 6));
	CHECK(two.insert(rect_wh(3, 2)).has_value());
	CHECK(two.get_spaces().get_count() == 2);

	/*
		Failure at a non-last index with a full provider: the swap-and-pop
		removal displaces another space, so the rollback must swap it back and
		reproduce the exact traversal order of a never-failed control.
	*/

	auto build_crowded = [](auto& provider) {
		CHECK(provider.insert(rect_wh(4, 4)).has_value());
		CHECK(provider.insert(rect_wh(2, 2)).has_value());
		CHECK(provider.get_spaces().get_count() == 3);
	};

	empty_spaces<false, static_empty_spaces<3>> crowded(rect_wh(10, 10));
	empty_spaces<false, static_empty_spaces<3>> control(rect_wh(10, 10));
	build_crowded(crowded);
	build_crowded(control);

	/* 3x3 fits only in a non-back space; the second split add then fails. */
	CHECK(!crowded.insert(rect_wh(3, 3)).has_value());
	CHECK(crowded.get_spaces().get_count() == 3);
	CHECK(serialize_spaces(crowded.get_spaces()) == serialize_spaces(control.get_spaces()));
	CHECK(crowded.get_rects_aabb().w == control.get_rects_aabb().w);
	CHECK(crowded.get_rects_aabb().h == control.get_rects_aabb().h);

	/*
		Both instances are a full three-slot provider, so every further insert
		fails for lack of split slots. The failed one must fail in exactly the
		same way as the never-failed control, with identical remaining state.
	*/

	const auto next_crowded = crowded.insert(rect_wh(1, 1));
	const auto next_control = control.insert(rect_wh(1, 1));
	CHECK(!next_crowded.has_value() && !next_control.has_value());
	CHECK(serialize_spaces(crowded.get_spaces()) == serialize_spaces(control.get_spaces()));
}

static void test_basic_append_and_order() {
	incremental_atlas<> atlas(10, 10);

	const auto r1 = atlas.append({
		atlas_image_request{ "x", 4, 3, 0, false }
	});

	CHECK(r1.status == atlas_append_status::SUCCESS);
	CHECK(r1.entries.size() == 1);
	CHECK(same_rect(r1.entries[0].layout.outer, 0, 0, 4, 3));
	CHECK(same_rect(r1.entries[0].layout.content, 0, 0, 4, 3));
	CHECK(r1.entries[0].layout.rotated == false);
	check_frame_invariants(r1.entries[0]);

	CHECK(atlas.get_entries().size() == 1);
	CHECK(atlas.find_entry("x") != nullptr);
	CHECK(atlas.find_entry("missing") == nullptr);
	CHECK(atlas.get_used_aabb().w == 4);
	CHECK(atlas.get_used_aabb().h == 3);

	const std::string spaces_after_first =
		"0,3,10,7;4,0,6,3;";
	CHECK(serialize_spaces(atlas.get_empty_spaces()) == spaces_after_first);

	/* Second batch lands beside the first one; the first one never moves. */

	const auto r2 = atlas.append({
		atlas_image_request{ "y", 6, 3, 0, false }
	});

	CHECK(r2.status == atlas_append_status::SUCCESS);
	CHECK(same_rect(r2.entries[0].layout.outer, 4, 0, 6, 3));
	CHECK(same_rect(atlas.find_entry("x")->layout.outer, 0, 0, 4, 3));
	CHECK(serialize_spaces(atlas.get_empty_spaces()) == "0,3,10,7;");

	/* Square image, padding 0. */

	const auto r3 = atlas.append({
		atlas_image_request{ "sq", 2, 2, 0, false }
	});

	CHECK(r3.status == atlas_append_status::SUCCESS);
	CHECK(same_rect(r3.entries[0].layout.outer, 0, 3, 2, 2));
	check_frame_invariants(r3.entries[0]);

	/* Independent occupancy invariants over every published frame. */

	const auto& entries = atlas.get_entries();

	for (const auto& e : entries) {
		CHECK(inside_canvas(e.layout.outer, 10, 10));

		for (const auto& other : entries) {
			if (&e == &other) {
				continue;
			}

			CHECK(!overlaps(e.layout.outer, other.layout.outer));
		}
	}
}

static void test_exact_fill_then_out_of_space() {
	incremental_atlas<> atlas(5, 5);

	const auto full = atlas.append({
		atlas_image_request{ "full", 5, 5, 0, true }
	});

	CHECK(full.status == atlas_append_status::SUCCESS);
	CHECK(same_rect(full.entries[0].layout.outer, 0, 0, 5, 5));
	CHECK(atlas.get_empty_spaces().get_count() == 0);
	CHECK(atlas.get_used_aabb().w == 5);
	CHECK(atlas.get_used_aabb().h == 5);

	const auto again = atlas.append({
		atlas_image_request{ "one", 1, 1, 0, true }
	});

	CHECK(again.status == atlas_append_status::OUT_OF_SPACE);
	CHECK(again.entries.empty());
	CHECK(atlas.get_entries().size() == 1);
	CHECK(atlas.get_empty_spaces().get_count() == 0);
}

static void test_failed_batch_rollback_and_retry() {
	incremental_atlas<> tested(8, 8);
	incremental_atlas<> control(8, 8);

	const auto common_first = [](incremental_atlas<>& a) {
		return a.append({ atlas_image_request{ "r", 4, 4, 0, false } });
	};

	CHECK(common_first(tested).status == atlas_append_status::SUCCESS);
	CHECK(common_first(control).status == atlas_append_status::SUCCESS);

	const std::string spaces_before = serialize_spaces(tested.get_empty_spaces());

	/*
		The first image of this batch fits, the second does not.
		Nothing may be published and the spaces must be restored exactly,
		including their traversal order.
	*/

	const auto failed = tested.append({
		atlas_image_request{ "d", 8, 4, 0, false },
		atlas_image_request{ "e", 8, 4, 0, false }
	});

	CHECK(failed.status == atlas_append_status::OUT_OF_SPACE);
	CHECK(failed.entries.empty());
	CHECK(tested.get_entries().size() == 1);
	CHECK(serialize_spaces(tested.get_empty_spaces()) == spaces_before);
	CHECK(tested.get_used_aabb().w == 4);
	CHECK(tested.get_used_aabb().h == 4);

	/* Retrying the valid part must match an instance that never failed. */

	const auto retry = tested.append({
		atlas_image_request{ "f", 8, 4, 0, false }
	});

	const auto control_append = control.append({
		atlas_image_request{ "f", 8, 4, 0, false }
	});

	CHECK(retry.status == atlas_append_status::SUCCESS);
	CHECK(control_append.status == atlas_append_status::SUCCESS);
	CHECK(serialize_entries(tested.get_entries()) == serialize_entries(control.get_entries()));
	CHECK(serialize_spaces(tested.get_empty_spaces()) == serialize_spaces(control.get_empty_spaces()));
	CHECK(same_rect(retry.entries[0].layout.outer, 0, 4, 8, 4));
}

static void test_rollback_restores_traversal_order() {
	incremental_atlas<> atlas(10, 10);

	CHECK(atlas.append({ atlas_image_request{ "r", 4, 4, 0, false } }).status
		== atlas_append_status::SUCCESS);

	const std::string before = serialize_spaces(atlas.get_empty_spaces());

	const auto failed = atlas.append({
		atlas_image_request{ "small", 2, 2, 0, false },
		atlas_image_request{ "huge", 10, 10, 0, false }
	});

	CHECK(failed.status == atlas_append_status::OUT_OF_SPACE);
	CHECK(serialize_spaces(atlas.get_empty_spaces()) == before);
	CHECK(atlas.get_entries().size() == 1);
}

static void test_duplicate_ids() {
	incremental_atlas<> atlas(10, 10);

	CHECK(atlas.append({ atlas_image_request{ "a", 2, 2, 0, true } }).status
		== atlas_append_status::SUCCESS);

	const std::string before = serialize_spaces(atlas.get_empty_spaces());

	const auto old_duplicate = atlas.append({
		atlas_image_request{ "a", 1, 1, 0, true }
	});

	CHECK(old_duplicate.status == atlas_append_status::DUPLICATE_ID);
	CHECK(old_duplicate.entries.empty());

	const auto batch_duplicate = atlas.append({
		atlas_image_request{ "z", 1, 1, 0, true },
		atlas_image_request{ "z", 1, 1, 0, true }
	});

	CHECK(batch_duplicate.status == atlas_append_status::DUPLICATE_ID);
	CHECK(batch_duplicate.entries.empty());

	/* Neither failure changed anything. */

	CHECK(atlas.get_entries().size() == 1);
	CHECK(serialize_spaces(atlas.get_empty_spaces()) == before);
}

static bool invalid(const atlas_image_request& request, const int canvas_w = 10, const int canvas_h = 10) {
	incremental_atlas<> atlas(canvas_w, canvas_h);
	return atlas.append({ request }).status == atlas_append_status::INVALID_DIMENSIONS;
}

static void test_invalid_inputs() {
	CHECK(invalid(atlas_image_request{ "w0", 0, 2, 0, true }));
	CHECK(invalid(atlas_image_request{ "h0", 2, 0, 0, true }));
	CHECK(invalid(atlas_image_request{ "big", 4097, 2, 0, true }));
	CHECK(invalid(atlas_image_request{ "neg", 2, -5, 0, true }));
	CHECK(invalid(atlas_image_request{ "pbig", 2, 2, 65, true }));
	CHECK(invalid(atlas_image_request{ "pneg", 2, 2, -1, true }));

	/* Padding pushes the outer frame past the canvas in every orientation:
	   the numbers are valid, but nothing can fit, so it is OUT_OF_SPACE. */

	{
		incremental_atlas<> padded(10, 10);
		CHECK(padded.append({ atlas_image_request{ "pad", 10, 8, 1, true } }).status
			== atlas_append_status::OUT_OF_SPACE);
		CHECK(padded.get_entries().empty());
	}

	/* Boundary values that are legal. */

	incremental_atlas<> max_canvas(4096, 4096);
	const auto boundary = max_canvas.append({
		atlas_image_request{ "edge", 4096, 4096, 0, true }
	});
	CHECK(boundary.status == atlas_append_status::SUCCESS);

	incremental_atlas<> one(1, 1);
	CHECK(one.append({ atlas_image_request{ "pixel", 1, 1, 0, true } }).status
		== atlas_append_status::SUCCESS);

	/* Construction out of range throws. */

	for (const auto dims : { std::pair<int, int>{0, 1}, {1, 0}, {4097, 1}, {1, 4097} }) {
		bool threw = false;

		try {
			incremental_atlas<> bad(dims.first, dims.second);
		}
		catch (const std::invalid_argument&) {
			threw = true;
		}

		CHECK(threw);
	}
}

static void test_static_provider_exhaustion() {
	incremental_atlas<static_empty_spaces<1>> atlas(8, 6);

	const auto first = atlas.append({
		atlas_image_request{ "a", 3, 2, 0, false }
	});

	/* Two splits do not fit a one-slot provider: the batch fails wholesale. */

	CHECK(first.status == atlas_append_status::OUT_OF_SPACE);
	CHECK(first.entries.empty());
	CHECK(atlas.get_entries().empty());
	CHECK(atlas.get_empty_spaces().get_count() == 1);
	CHECK(same_rect(atlas.get_empty_spaces().get(0), 0, 0, 8, 6));

	const auto full = atlas.append({
		atlas_image_request{ "full", 8, 6, 0, false }
	});

	CHECK(full.status == atlas_append_status::SUCCESS);
	CHECK(same_rect(full.entries[0].layout.outer, 0, 0, 8, 6));
	CHECK(atlas.get_empty_spaces().get_count() == 0);

	/* A single split fits into the one slot, so padding-like cases still work. */

	incremental_atlas<static_empty_spaces<1>> strip(8, 6);
	CHECK(strip.append({ atlas_image_request{ "s", 8, 2, 0, false } }).status
		== atlas_append_status::SUCCESS);
	CHECK(strip.get_empty_spaces().get_count() == 1);
	CHECK(same_rect(strip.get_empty_spaces().get(0), 0, 2, 8, 4));
	CHECK(strip.append({ atlas_image_request{ "rest", 8, 4, 0, false } }).status
		== atlas_append_status::SUCCESS);
	CHECK(strip.get_empty_spaces().get_count() == 0);
}

static void test_instance_isolation() {
	incremental_atlas<> a(8, 8);
	incremental_atlas<> b(8, 8);

	CHECK(a.append({ atlas_image_request{ "only-a", 3, 3, 0, false } }).status
		== atlas_append_status::SUCCESS);

	CHECK(b.get_entries().empty());
	CHECK(serialize_spaces(b.get_empty_spaces()) == "0,0,8,8;");

	CHECK(b.append({ atlas_image_request{ "only-b", 5, 5, 0, false } }).status
		== atlas_append_status::SUCCESS);

	CHECK(a.get_entries().size() == 1);
	CHECK(a.find_entry("only-a") != nullptr);
	CHECK(a.find_entry("only-b") == nullptr);
	CHECK(b.find_entry("only-a") == nullptr);
	CHECK(same_rect(a.find_entry("only-a")->layout.outer, 0, 0, 3, 3));
}

static void test_empty_batch() {
	incremental_atlas<> atlas(10, 10);

	const std::string before = serialize_spaces(atlas.get_empty_spaces());
	const auto empty = atlas.append({});

	CHECK(empty.status == atlas_append_status::SUCCESS);
	CHECK(empty.entries.empty());
	CHECK(atlas.get_entries().empty());
	CHECK(serialize_spaces(atlas.get_empty_spaces()) == before);
}

static void test_rotation_and_padding_geometry() {
	/* The documented atlas_case.txt scenario on a 6x10 canvas. */

	incremental_atlas<> atlas(6, 10);

	const auto a = atlas.append({
		atlas_image_request{ "A", 6, 2, 0, false }
	});

	CHECK(a.status == atlas_append_status::SUCCESS);
	CHECK(same_rect(a.entries[0].layout.outer, 0, 0, 6, 2));
	CHECK(same_rect(a.entries[0].layout.content, 0, 0, 6, 2));
	CHECK(a.entries[0].layout.rotated == false);

	const auto b = atlas.append({
		atlas_image_request{ "B", 6, 4, 1, true }
	});

	CHECK(b.status == atlas_append_status::SUCCESS);
	CHECK(same_rect(b.entries[0].layout.outer, 0, 2, 6, 8));
	CHECK(same_rect(b.entries[0].layout.content, 1, 3, 4, 6));
	CHECK(b.entries[0].layout.rotated == true);
	check_frame_invariants(b.entries[0]);

	/*
		Hand-computed texel mapping for B (h = original height = 4):
		rotated source (u, v) lands at (content.x + h - 1 - v, content.y + u).
	*/

	const auto& content = b.entries[0].layout.content;
	CHECK(content.x + 4 - 1 - 0 == 4 && content.y + 0 == 3); /* (0, 0) -> (4, 3) */
	CHECK(content.x + 4 - 1 - 3 == 1 && content.y + 5 == 8); /* (5, 3) -> (1, 8) */

	/* The per-image rotation switch only affects that single image. */

	incremental_atlas<> no_rot(6, 10);
	CHECK(no_rot.append({ atlas_image_request{ "A", 6, 2, 0, false } }).status
		== atlas_append_status::SUCCESS);

	const auto forced = no_rot.append({
		atlas_image_request{ "B", 6, 4, 1, false }
	});

	CHECK(forced.status == atlas_append_status::OUT_OF_SPACE);
	CHECK(no_rot.get_entries().size() == 1);

	/* The same request succeeds as soon as that image allows rotation. */

	const auto allowed = no_rot.append({
		atlas_image_request{ "B", 6, 4, 1, true }
	});

	CHECK(allowed.status == atlas_append_status::SUCCESS);
	CHECK(allowed.entries[0].layout.rotated == true);
	CHECK(same_rect(allowed.entries[0].layout.outer, 0, 2, 6, 8));
}

static void test_padding_without_rotation() {
	incremental_atlas<> atlas(10, 10);

	const auto r = atlas.append({
		atlas_image_request{ "padded", 4, 3, 2, false }
	});

	CHECK(r.status == atlas_append_status::SUCCESS);
	CHECK(same_rect(r.entries[0].layout.outer, 0, 0, 8, 7));
	CHECK(same_rect(r.entries[0].layout.content, 2, 2, 4, 3));
	CHECK(r.entries[0].layout.rotated == false);
	check_frame_invariants(r.entries[0]);

	/* A non-rotatable image that would only fit rotated must fail. */

	incremental_atlas<> tight(6, 8);
	const auto wont_fit = tight.append({
		atlas_image_request{ "tall", 8, 6, 0, false }
	});
	CHECK(wont_fit.status == atlas_append_status::OUT_OF_SPACE);

	const auto rotated = tight.append({
		atlas_image_request{ "tall", 8, 6, 0, true }
	});
	CHECK(rotated.status == atlas_append_status::SUCCESS);
	CHECK(rotated.entries[0].layout.rotated == true);
	CHECK(same_rect(rotated.entries[0].layout.outer, 0, 0, 6, 8));
}

static void test_multi_image_batch() {
	incremental_atlas<> atlas(6, 12);

	/* One batch mixing a non-rotated and a rotated image. */

	const auto batch = atlas.append({
		atlas_image_request{ "X", 6, 2, 0, false },
		atlas_image_request{ "Y", 8, 4, 0, true }
	});

	CHECK(batch.status == atlas_append_status::SUCCESS);
	CHECK(batch.entries.size() == 2);

	/* Entries come back in request order. */

	CHECK(batch.entries[0].id == "X");
	CHECK(batch.entries[1].id == "Y");

	CHECK(same_rect(batch.entries[0].layout.outer, 0, 0, 6, 2));
	CHECK(batch.entries[0].layout.rotated == false);

	CHECK(same_rect(batch.entries[1].layout.outer, 0, 2, 4, 8));
	CHECK(batch.entries[1].layout.rotated == true);
	/* Rotated content keeps the original 8x4 texels, swapped once to 4x8. */
	CHECK(same_rect(batch.entries[1].layout.content, 0, 2, 4, 8));

	/* Published state and per-entry lookup agree across the batch. */

	CHECK(atlas.get_entries().size() == 2);
	CHECK(atlas.find_entry("X") == &atlas.get_entries()[0]);
	CHECK(atlas.find_entry("Y") == &atlas.get_entries()[1]);
	CHECK(!overlaps(batch.entries[0].layout.outer, batch.entries[1].layout.outer));
	CHECK(atlas.get_used_aabb().w == 6);
	CHECK(atlas.get_used_aabb().h == 10);

	/* A second batch accumulates after the first; old coordinates stay put. */

	const auto more = atlas.append({
		atlas_image_request{ "Z", 2, 2, 0, false }
	});

	CHECK(more.status == atlas_append_status::SUCCESS);
	CHECK(atlas.get_entries().size() == 3);
	CHECK(same_rect(atlas.find_entry("X")->layout.outer, 0, 0, 6, 2));
	CHECK(same_rect(atlas.find_entry("Y")->layout.outer, 0, 2, 4, 8));
	CHECK(same_rect(more.entries[0].layout.outer, 4, 2, 2, 2));
}

int main() {
	test_empty_spaces_rollback();
	test_basic_append_and_order();
	test_exact_fill_then_out_of_space();
	test_failed_batch_rollback_and_retry();
	test_rollback_restores_traversal_order();
	test_duplicate_ids();
	test_invalid_inputs();
	test_static_provider_exhaustion();
	test_instance_isolation();
	test_empty_batch();
	test_rotation_and_padding_geometry();
	test_padding_without_rotation();
	test_multi_image_batch();

	if (failures == 0) {
		std::cout << "incremental atlas tests: all checks passed\n";
		return 0;
	}

	std::cerr << "incremental atlas tests: " << failures << " failed checks\n";
	return 1;
}
