/*
	Independent verifier for the grayscale atlas example.

	It reads the same hand-written case file plus the produced layout text and
	P2 PGM, and checks everything with logic written from scratch here:
		- frame geometry against the numbers mandated by the specification,
		- canvas containment and pairwise non-overlapping outer frames,
		- every pixel against the source textures, using its own texel
		  mapping, and zeroes everywhere unoccupied.

	It never includes or calls the packing code.
*/

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {
	struct source_image {
		std::string id;
		int width = 0;
		int height = 0;
		int padding = 0;
		bool allow_rotation = false;
		std::vector<int> pixels;
	};

	struct case_data {
		int canvas_width = 0;
		int canvas_height = 0;
		std::vector<source_image> images;
	};

	struct frame {
		int x = 0;
		int y = 0;
		int w = 0;
		int h = 0;
	};

	struct layout_line {
		std::string id;
		frame outer;
		frame content;
		bool rotated = false;
	};

	[[noreturn]] void fail(const std::string& message) {
		std::cerr << "verify_atlas: " << message << '\n';
		std::exit(1);
	}

	case_data read_case(const std::string& path) {
		std::ifstream in(path);

		if (!in) {
			fail("cannot open case file");
		}

		case_data data;
		std::string line;

		while (std::getline(in, line)) {
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
				tokens >> data.canvas_width >> data.canvas_height;
			}
			else if (keyword == "image") {
				source_image image;
				int allow_rotation = 0;
				tokens >> image.id >> image.width >> image.height
				       >> image.padding >> allow_rotation;
				image.allow_rotation = allow_rotation != 0;
				image.pixels.resize(image.width * image.height);

				for (int y = 0; y < image.height; ++y) {
					if (!std::getline(in, line)) {
						fail("truncated pixels");
					}

					std::istringstream row(line);

					for (int x = 0; x < image.width; ++x) {
						row >> image.pixels[y * image.width + x];
					}
				}

				data.images.push_back(std::move(image));
			}
		}

		return data;
	}

	frame parse_frame(const std::string& token) {
		/* Token shape: "x,y,w,h". */

		std::istringstream parser(token);
		frame result;
		char comma1;
		char comma2;
		char comma3;
		parser >> result.x >> comma1 >> result.y >> comma2
		       >> result.w >> comma3 >> result.h;

		if (!parser || comma1 != ',' || comma2 != ',' || comma3 != ',') {
			fail("malformed frame: " + token);
		}

		return result;
	}

	std::vector<layout_line> read_layout(const std::string& path) {
		std::ifstream in(path);

		if (!in) {
			fail("cannot open layout file");
		}

		std::vector<layout_line> lines;
		std::string line;

		while (std::getline(in, line)) {
			if (line.empty()) {
				continue;
			}

			std::istringstream tokens(line);
			layout_line parsed;
			std::string outer_token;
			std::string content_token;
			std::string rotated_token;

			tokens >> parsed.id >> outer_token >> content_token >> rotated_token;

			const auto split = [](const std::string& token) {
				const auto equals = token.find('=');

				if (equals == std::string::npos) {
					fail("malformed layout token: " + token);
				}

				return token.substr(equals + 1);
			};

			if (outer_token.rfind("outer=", 0) != 0
			    || content_token.rfind("content=", 0) != 0
			    || rotated_token.rfind("rotated=", 0) != 0) {
				fail("malformed layout line: " + line);
			}

			parsed.outer = parse_frame(split(outer_token));
			parsed.content = parse_frame(split(content_token));
			parsed.rotated = split(rotated_token) == "1";
			lines.push_back(parsed);
		}

		return lines;
	}

	std::vector<int> read_pgm(
		const std::string& path,
		const int expected_width,
		const int expected_height
	) {
		std::ifstream in(path);

		if (!in) {
			fail("cannot open pgm file");
		}

		std::string magic;
		int width;
		int height;
		int max_value;
		in >> magic >> width >> height >> max_value;

		if (magic != "P2" || width != expected_width
		    || height != expected_height || max_value != 255) {
			fail("unexpected pgm header");
		}

		std::vector<int> pixels(width * height);

		for (int& pixel : pixels) {
			in >> pixel;

			if (!in || pixel < 0 || pixel > max_value) {
				fail("malformed pgm pixels");
			}
		}

		return pixels;
	}

	bool contains(const frame& outer, const frame& inner) {
		return inner.x >= outer.x && inner.y >= outer.y
			&& inner.x + inner.w <= outer.x + outer.w
			&& inner.y + inner.h <= outer.y + outer.h;
	}

	/* Independent half-open rectangle intersection. */
	bool intersects(const frame& a, const frame& b) {
		return a.x < b.x + b.w && b.x < a.x + a.w
			&& a.y < b.y + b.h && b.y < a.y + a.h;
	}

	int clamp_int(const int value, const int low, const int high) {
		return value < low ? low : (value > high ? high : value);
	}

	void check(const bool condition, const std::string& message) {
		if (!condition) {
			fail(message);
		}
	}
}

int main(int argc, char** argv) {
	if (argc != 4) {
		std::cerr << "usage: " << argv[0]
		          << " <case.txt> <layout.txt> <atlas.pgm>\n";
		return 2;
	}

	const auto data = read_case(argv[1]);
	const auto layout = read_layout(argv[2]);
	const auto pixels = read_pgm(argv[3], data.canvas_width, data.canvas_height);

	check(layout.size() == data.images.size(), "layout/image count mismatch");

	/* Hard-coded geometry required by the specification for this case. */

	check(layout.size() == 2, "case must contain exactly A and B");

	check(layout[0].id == "A", "first entry must be A");
	check(layout[0].outer.x == 0 && layout[0].outer.y == 0
	      && layout[0].outer.w == 6 && layout[0].outer.h == 2,
	      "A outer frame mismatch");
	check(layout[0].content.x == 0 && layout[0].content.y == 0
	      && layout[0].content.w == 6 && layout[0].content.h == 2,
	      "A content frame mismatch");
	check(!layout[0].rotated, "A must not be rotated");

	check(layout[1].id == "B", "second entry must be B");
	check(layout[1].outer.x == 0 && layout[1].outer.y == 2
	      && layout[1].outer.w == 6 && layout[1].outer.h == 8,
	      "B outer frame mismatch");
	check(layout[1].content.x == 1 && layout[1].content.y == 3
	      && layout[1].content.w == 4 && layout[1].content.h == 6,
	      "B content frame mismatch");
	check(layout[1].rotated, "B must be rotated");

	for (std::size_t i = 0; i < layout.size(); ++i) {
		const auto& entry = layout[i];
		const auto& source = data.images[i];

		check(entry.id == source.id, "layout order mismatch");

		/* Frames stay inside the canvas and the content stays inside its frame. */

		const frame canvas{ 0, 0, data.canvas_width, data.canvas_height };
		check(contains(canvas, entry.outer), "outer frame leaves canvas");
		check(contains(entry.outer, entry.content), "content frame leaves outer frame");

		/* Padding shrinks every side of the final outer frame. */

		check(entry.content.x == entry.outer.x + source.padding
		      && entry.content.y == entry.outer.y + source.padding,
		      "content frame origin mismatch");
		check(entry.content.w == entry.outer.w - 2 * source.padding
		      && entry.content.h == entry.outer.h - 2 * source.padding,
		      "content frame size mismatch");

		/* Content dimensions are the original ones, swapped once if rotated. */

		if (entry.rotated) {
			check(entry.content.w == source.height
			      && entry.content.h == source.width,
			      "rotated content dimensions mismatch");
		}
		else {
			check(entry.content.w == source.width
			      && entry.content.h == source.height,
			      "content dimensions mismatch");
		}

		for (std::size_t j = 0; j < layout.size(); ++j) {
			if (i != j) {
				check(!intersects(entry.outer, layout[j].outer),
				      "outer frames overlap");
			}
		}
	}

	/*
		Pixel-by-pixel verification with an independently written mapping:
		each canvas pixel must either be unoccupied (zero) or equal to the
		source texel reached by clamping into the content frame (nearest-edge
		padding extension, corners included) and then inverting the rotation.
	*/

	for (int y = 0; y < data.canvas_height; ++y) {
		for (int x = 0; x < data.canvas_width; ++x) {
			const int actual = pixels[y * data.canvas_width + x];
			const frame point{ x, y, 1, 1 };

			const layout_line* owner = nullptr;
			const source_image* source = nullptr;

			for (std::size_t i = 0; i < layout.size(); ++i) {
				if (intersects(point, layout[i].outer)) {
					check(owner == nullptr, "pixel claimed by multiple frames");
					owner = &layout[i];
					source = &data.images[i];
				}
			}

			if (owner == nullptr) {
				check(actual == 0, "unoccupied pixel is not zero");
				continue;
			}

			const int cx = clamp_int(
				x, owner->content.x, owner->content.x + owner->content.w - 1
			);
			const int cy = clamp_int(
				y, owner->content.y, owner->content.y + owner->content.h - 1
			);

			int u;
			int v;

			if (!owner->rotated) {
				u = cx - owner->content.x;
				v = cy - owner->content.y;
			}
			else {
				/* Forward mapping is (x, y) = (cx + h - 1 - v, cy + u). */
				u = cy - owner->content.y;
				v = source->height - 1 - (cx - owner->content.x);
			}

			const int expected = source->pixels[v * source->width + u];
			check(actual == expected,
			      "pixel mismatch at (" + std::to_string(x) + ","
			      + std::to_string(y) + ")");
		}
	}

	std::cout << "verify_atlas: all independent checks passed\n";
	return 0;
}
