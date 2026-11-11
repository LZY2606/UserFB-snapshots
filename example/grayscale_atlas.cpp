/*
	Headless grayscale atlas example.

	It exercises rectpack2D::incremental_atlas without any window, GPU or image
	decoding library:

		1. Parses a tiny text case describing a canvas and a few small integer
		   textures (see tests/atlas/atlas_case.txt).
		2. Inserts the textures through the incremental interface and writes:
			- a layout text file (outer frame, content frame, rotated flag),
			- a P2 PGM image of the whole canvas.

	Padding is filled by extending the nearest content-edge texel, including
	corners; unoccupied canvas pixels stay zero.

		3. Runs a deterministic mid-batch failure demo on a second atlas and
		   proves that the shared atlas bytes and the previously published
		   coordinates are left untouched. Images of a failed batch are never
		   painted, even when some of them had already been tentatively placed.

	Usage:
		grayscale_atlas <case.txt> <layout_out.txt> <atlas_out.pgm>
*/

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <rectpack2D/incremental_atlas.h>

using namespace rectpack2D;

namespace {
	struct grayscale_image {
		std::string id;
		int width = 0;
		int height = 0;
		int padding = 0;
		bool allow_rotation = false;
		std::vector<int> pixels; /* Row-major, height rows of width values. */
	};

	struct parsed_case {
		int canvas_width = 0;
		int canvas_height = 0;
		std::vector<grayscale_image> images;
	};

	[[noreturn]] void fail(const std::string& message) {
		std::cerr << "grayscale_atlas: " << message << '\n';
		std::exit(1);
	}

	parsed_case parse_case(const std::string& path) {
		std::ifstream in(path);

		if (!in) {
			fail("cannot open case file: " + path);
		}

		parsed_case result;
		std::string line;

		while (std::getline(in, line)) {
			/* Strip comments; the case file documents itself with '#'. */

			if (const auto hash = line.find('#'); hash != std::string::npos) {
				line.erase(hash);
			}

			if (line.find_first_not_of(" \t\r\n") == std::string::npos) {
				continue;
			}

			std::istringstream tokens(line);
			std::string keyword;
			tokens >> keyword;

			if (keyword == "canvas") {
				tokens >> result.canvas_width >> result.canvas_height;

				if (!tokens) {
					fail("malformed canvas line");
				}
			}
			else if (keyword == "image") {
				grayscale_image image;
				int allow_rotation = 0;
				tokens >> image.id >> image.width >> image.height
				       >> image.padding >> allow_rotation;

				if (!tokens) {
					fail("malformed image header for " + image.id);
				}

				image.allow_rotation = allow_rotation != 0;
				image.pixels.resize(image.width * image.height);

				for (int y = 0; y < image.height; ++y) {
					if (!std::getline(in, line)) {
						fail("unexpected end of pixels for " + image.id);
					}

					std::istringstream row(line);

					for (int x = 0; x < image.width; ++x) {
						row >> image.pixels[y * image.width + x];

						if (!row) {
							fail("malformed pixel row for " + image.id);
						}
					}
				}

				result.images.push_back(std::move(image));
			}
			else {
				fail("unknown keyword: " + keyword);
			}
		}

		if (result.canvas_width <= 0 || result.canvas_height <= 0) {
			fail("case file has no valid canvas");
		}

		return result;
	}

	/*
		Paints one published entry into the shared canvas.

		For every pixel of the outer frame we clamp its position into the
		content frame, so padding naturally extends the nearest content-edge
		texel and the corners extend the nearest content corner texel.
	*/
	void blit(
		std::vector<int>& canvas,
		const int canvas_width,
		const grayscale_image& image,
		const atlas_entry& entry
	) {
		const auto& outer = entry.layout.outer;
		const auto& content = entry.layout.content;

		for (int y = outer.y; y < outer.y + outer.h; ++y) {
			for (int x = outer.x; x < outer.x + outer.w; ++x) {
				const int cx = std::clamp(x, content.x, content.x + content.w - 1);
				const int cy = std::clamp(y, content.y, content.y + content.h - 1);

				int u;
				int v;

				if (!entry.layout.rotated) {
					/* (u, v) -> (content.x + u, content.y + v) */
					u = cx - content.x;
					v = cy - content.y;
				}
				else {
					/* (u, v) -> (content.x + h - 1 - v, content.y + u) */
					u = cy - content.y;
					v = entry.original_height - 1 - (cx - content.x);
				}

				canvas[y * canvas_width + x] = image.pixels[v * image.width + u];
			}
		}
	}

	void write_layout(
		const std::string& path,
		const std::vector<atlas_entry>& entries
	) {
		std::ofstream out(path);

		if (!out) {
			fail("cannot write layout file: " + path);
		}

		for (const auto& entry : entries) {
			const auto& outer = entry.layout.outer;
			const auto& content = entry.layout.content;

			out << entry.id
			    << " outer=" << outer.x << ',' << outer.y << ',' << outer.w << ',' << outer.h
			    << " content=" << content.x << ',' << content.y << ',' << content.w << ',' << content.h
			    << " rotated=" << (entry.layout.rotated ? 1 : 0) << '\n';
		}
	}

	void write_pgm(
		const std::string& path,
		const std::vector<int>& canvas,
		const int width,
		const int height
	) {
		std::ofstream out(path);

		if (!out) {
			fail("cannot write pgm file: " + path);
		}

		out << "P2\n" << width << ' ' << height << "\n255\n";

		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				if (x != 0) {
					out << ' ';
				}

				out << canvas[y * width + x];
			}

			out << '\n';
		}
	}

	std::vector<int> make_texture(const int width, const int height, const int base) {
		std::vector<int> pixels(width * height);

		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				pixels[y * width + x] = base + y * width + x;
			}
		}

		return pixels;
	}

	void check(const bool condition, const std::string& message) {
		if (!condition) {
			fail(message);
		}
	}

	/*
		Demonstrates an all-or-nothing batch on a separate atlas.

		The first image of the failing batch fits, but the second does not.
		The shared pixel buffer is only painted after a batch succeeds, so the
		first image must never appear; the already published entry and every
		canvas byte must remain exactly as they were.
	*/
	void run_failure_demo() {
		constexpr int side = 6;

		incremental_atlas<> atlas(side, side);
		std::vector<int> canvas(side * side, 0);

		grayscale_image base;
		base.id = "base";
		base.width = 3;
		base.height = 3;
		base.padding = 0;
		base.allow_rotation = false;
		base.pixels = make_texture(3, 3, 10);

		const auto base_result = atlas.append({
			atlas_image_request{ base.id, base.width, base.height, base.padding, base.allow_rotation }
		});

		check(base_result.status == atlas_append_status::SUCCESS, "demo: base insert failed");
		blit(canvas, side, base, base_result.entries.front());

		/* State that must survive the failed batch. */

		const std::vector<int> canvas_before = canvas;
		const rect_xywh base_outer_before = base_result.entries.front().layout.outer;
		const auto spaces_before = atlas.get_empty_spaces().get_count();

		grayscale_image okay;
		okay.id = "okay";
		okay.width = 2;
		okay.height = 2;
		okay.pixels = make_texture(2, 2, 40);

		grayscale_image huge;
		huge.id = "huge";
		huge.width = 5;
		huge.height = 5;
		huge.pixels = make_texture(5, 5, 70);

		const auto failed = atlas.append({
			atlas_image_request{ okay.id, okay.width, okay.height, 0, false },
			atlas_image_request{ huge.id, huge.width, huge.height, 0, false }
		});

		std::cout << "failure demo: batch status="
		          << static_cast<int>(failed.status) << " (1=OUT_OF_SPACE)\n";

		check(failed.status == atlas_append_status::OUT_OF_SPACE, "demo: expected OUT_OF_SPACE");
		check(failed.entries.empty(), "demo: failed batch published entries");
		check(atlas.find_entry(okay.id) == nullptr, "demo: tentative entry was published");
		check(atlas.get_empty_spaces().get_count() == spaces_before,
			"demo: empty spaces changed on failure");
		check(canvas == canvas_before, "demo: atlas bytes changed on failure");

		const auto* base_after = atlas.find_entry(base.id);
		check(base_after != nullptr, "demo: old entry disappeared");
		check(base_after->layout.outer.x == base_outer_before.x
			&& base_after->layout.outer.y == base_outer_before.y
			&& base_after->layout.outer.w == base_outer_before.w
			&& base_after->layout.outer.h == base_outer_before.h,
			"demo: old coordinates changed");

		std::cout << "failure demo: atlas bytes unchanged=1\n";
		std::cout << "failure demo: old coordinates unchanged=1\n";

		/* The same valid input retried after the failure must succeed. */

		const auto retry = atlas.append({
			atlas_image_request{ okay.id, okay.width, okay.height, 0, false }
		});

		std::cout << "failure demo: retry status="
		          << static_cast<int>(retry.status) << " (0=SUCCESS)\n";

		check(retry.status == atlas_append_status::SUCCESS, "demo: retry failed");
		blit(canvas, side, okay, retry.entries.front());
		check(canvas != canvas_before, "demo: successful retry did not paint anything");
		check(atlas.get_entries().size() == 2, "demo: unexpected entry count after retry");

		std::cout << "failure demo: entries after retry=" << atlas.get_entries().size() << '\n';
	}
}

int main(int argc, char** argv) {
	if (argc != 4) {
		std::cerr << "usage: " << argv[0]
		          << " <case.txt> <layout_out.txt> <atlas_out.pgm>\n";
		return 2;
	}

	const auto parsed = parse_case(argv[1]);

	incremental_atlas<> atlas(parsed.canvas_width, parsed.canvas_height);
	std::vector<int> canvas(parsed.canvas_width * parsed.canvas_height, 0);

	/* Each image is its own batch, appended exactly in the case file's order. */

	for (const auto& image : parsed.images) {
		const auto result = atlas.append({
			atlas_image_request{
				image.id,
				image.width,
				image.height,
				image.padding,
				image.allow_rotation
			}
		});

		if (result.status != atlas_append_status::SUCCESS) {
			fail("image " + image.id + " could not be inserted, status "
			     + std::to_string(static_cast<int>(result.status)));
		}

		/* Painting happens only after the whole batch was published. */
		blit(canvas, parsed.canvas_width, image, result.entries.front());
	}

	write_layout(argv[2], atlas.get_entries());
	write_pgm(argv[3], canvas, parsed.canvas_width, parsed.canvas_height);

	run_failure_demo();

	std::cout << "grayscale atlas example finished successfully\n";
	return 0;
}
