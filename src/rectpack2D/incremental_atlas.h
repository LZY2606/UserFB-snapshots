#pragma once

/*
	Incremental, fixed-size texture atlas.

	Unlike find_best_packing(), this interface never reorders images and never
	shrinks or grows the canvas: the width and height are fixed at construction and
	every successful insertion stays at the same coordinates for the lifetime of
	the atlas. This makes it safe to hand the coordinates over to draw batches
	while more images keep arriving.

	Insertion and space splitting are performed by the existing empty_spaces
	class - there is no re-sorting, bin search or global optimization.

	Every append() call is atomic:
		- either every image of the batch is inserted and its layout is published,
		- or none of them is, and the atlas (entries, used extent and empty spaces,
		  including their traversal order) is left exactly as it was before the call.

	After a failed call, retrying the same valid inputs yields the same result as
	a control instance that never saw the failed call.

	An empty batch succeeds and changes nothing.
*/

#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "empty_spaces.h"
#include "empty_space_allocators.h" // IWYU pragma: export

namespace rectpack2D {
	inline constexpr int INCREMENTAL_ATLAS_MIN_SIDE = 1;
	inline constexpr int INCREMENTAL_ATLAS_MAX_SIDE = 4096;
	inline constexpr int INCREMENTAL_ATLAS_MAX_PADDING = 64;

	struct atlas_image_request {
		std::string id;
		int width = 0;
		int height = 0;
		int padding = 0;

		/*
			Only applies to this single image.
			When false, the image is never rotated even if the atlas as a whole
			uses a flip-capable empty spaces provider.
		*/
		bool allow_rotation = true;
	};

	struct atlas_rect {
		/* Outer frame, half-open interval [x, x + w) x [y, y + h), including padding. */
		rect_xywh outer;

		/*
			Content frame, half-open interval inside the final outer frame,
			shrunk by padding on every side. Its width and height are always
			the original image width and height (already swapped once when
			rotated - they are not swapped again here).
		*/
		rect_xywh content;

		/* True when the whole outer frame was rotated clockwise by 90 degrees. */
		bool rotated = false;
	};

	struct atlas_entry {
		std::string id;
		int original_width = 0;
		int original_height = 0;
		int padding = 0;
		atlas_rect layout;
	};

	enum class atlas_append_status {
		SUCCESS,
		OUT_OF_SPACE,
		DUPLICATE_ID,
		INVALID_DIMENSIONS
	};

	struct atlas_append_result {
		atlas_append_status status = atlas_append_status::SUCCESS;

		/*
			One entry per request, in request order.
			Empty unless status == SUCCESS.
		*/
		std::vector<atlas_entry> entries;

		explicit operator bool() const {
			return status == atlas_append_status::SUCCESS;
		}
	};

	template <class empty_spaces_provider = default_empty_spaces>
	class incremental_atlas {
		/*
			Rotation is always enabled at the provider level: the per-image
			allow_rotation flag is enforced by temporarily disabling flipping
			for the images that opt out.
		*/
		using spaces_type = empty_spaces<true, empty_spaces_provider>;

		int canvas_width;
		int canvas_height;
		spaces_type spaces;
		std::vector<atlas_entry> published_entries;
		std::unordered_set<std::string> used_ids;

		static bool valid_side(const int side) {
			return side >= INCREMENTAL_ATLAS_MIN_SIDE && side <= INCREMENTAL_ATLAS_MAX_SIDE;
		}

		static bool valid_padding(const int padding) {
			return padding >= 0 && padding <= INCREMENTAL_ATLAS_MAX_PADDING;
		}

		static bool valid_request(const atlas_image_request& request) {
			return valid_side(request.width)
				&& valid_side(request.height)
				&& valid_padding(request.padding);
		}

	public:
		incremental_atlas(const int width, const int height)
			: canvas_width(width),
			  canvas_height(height),
			  spaces(rect_wh(width, height)) {
			if (!valid_side(width) || !valid_side(height)) {
				throw std::invalid_argument(
					"incremental_atlas: canvas width and height must be in the range 1..4096"
				);
			}

			spaces.flipping_mode = flipping_option::ENABLED;
		}

		int get_canvas_width() const {
			return canvas_width;
		}

		int get_canvas_height() const {
			return canvas_height;
		}

		/* All entries published by successful batches, in the order of insertion. */
		const std::vector<atlas_entry>& get_entries() const {
			return published_entries;
		}

		/* Returns null when the id was never published. */
		const atlas_entry* find_entry(const std::string& id) const {
			for (const auto& entry : published_entries) {
				if (entry.id == id) {
					return &entry;
				}
			}

			return nullptr;
		}

		/* Extent of the canvas actually covered by published outer frames. */
		rect_wh get_used_aabb() const {
			return spaces.get_rects_aabb();
		}

		const auto& get_empty_spaces() const {
			return spaces.get_spaces();
		}

		atlas_append_result append(const std::vector<atlas_image_request>& requests) {
			atlas_append_result result;

			if (requests.empty()) {
				return result;
			}

			/* Phase 1: validate everything before touching the atlas. */

			for (const auto& request : requests) {
				/*
					Numeric range check only. A padded frame that exceeds the
					canvas is not an illegal request: it simply cannot fit and
					is reported as OUT_OF_SPACE once insertion is attempted.
					All values are small non-negative integers, so the doubled
					padding and the outer-size sums cannot overflow int.
				*/

				if (!valid_request(request)) {
					result.status = atlas_append_status::INVALID_DIMENSIONS;
					return result;
				}

				if (used_ids.find(request.id) != used_ids.end()) {
					result.status = atlas_append_status::DUPLICATE_ID;
					return result;
				}
			}

			{
				std::unordered_set<std::string> batch_ids;

				for (const auto& request : requests) {
					if (!batch_ids.insert(request.id).second) {
						result.status = atlas_append_status::DUPLICATE_ID;
						return result;
					}
				}
			}

			/*
				Phase 2: tentative insertion.

				Snapshot the whole spaces state (contents, traversal order and
				the used extent). Every successful insertion in this batch only
				mutates the snapshot's sibling; on any failure the snapshot is
				restored and the local entries are discarded, so the atlas ends
				up byte-for-byte in its pre-call state. Nothing is published
				until every image has been inserted.
			*/

			const spaces_type saved_spaces = spaces;

			std::vector<atlas_entry> tentative;
			tentative.reserve(requests.size());

			for (const auto& request : requests) {
				const int outer_w = request.width + 2 * request.padding;
				const int outer_h = request.height + 2 * request.padding;

				/*
					The per-image rotation switch only affects this one insert:
					flipping is disabled for images that opt out, and the mode
					is restored afterwards. The saved snapshot also carries the
					resting mode, so a failed batch restores it regardless.
				*/

				spaces.flipping_mode = request.allow_rotation
					? flipping_option::ENABLED
					: flipping_option::DISABLED;

				const auto inserted = spaces.insert(rect_wh(outer_w, outer_h));

				if (!inserted) {
					spaces = saved_spaces;
					result.status = atlas_append_status::OUT_OF_SPACE;
					return result;
				}

				atlas_entry entry;
				entry.id = request.id;
				entry.original_width = request.width;
				entry.original_height = request.height;
				entry.padding = request.padding;

				const bool rotated = inserted->flipped;
				const int final_outer_w = rotated ? outer_h : outer_w;
				const int final_outer_h = rotated ? outer_w : outer_h;

				entry.layout.outer = rect_xywh(
					inserted->x,
					inserted->y,
					final_outer_w,
					final_outer_h
				);

				/*
					The content frame is the outer frame shrunk by padding on
					every side. Its dimensions are the already-swapped ones of
					the rotated frame; the original width/height must not be
					swapped again.
				*/

				entry.layout.content = rect_xywh(
					inserted->x + request.padding,
					inserted->y + request.padding,
					final_outer_w - 2 * request.padding,
					final_outer_h - 2 * request.padding
				);

				entry.layout.rotated = rotated;
				tentative.push_back(std::move(entry));
			}

			/* Back to the resting flipping mode after the per-image overrides. */
			spaces.flipping_mode = flipping_option::ENABLED;

			/* Phase 3: publish the whole batch. */

			for (const auto& entry : tentative) {
				used_ids.insert(entry.id);
			}

			result.entries = std::move(tentative);

			for (auto& entry : result.entries) {
				published_entries.push_back(entry);
			}

			return result;
		}
	};
}
