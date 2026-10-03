// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>


namespace openage::gamestate {

/**
 * Terrain elevation of a map, stored per tile corner (XR fork).
 *
 * Corner (ne, se) has the index ne + se * (width + 1). Between the corners the
 * height follows the two triangles of the terrain mesh
 * (renderer/stages/terrain/chunk.cpp), i.e. the diagonal from corner (0, 0)
 * to (1, 1) of each tile, so that objects stand exactly on the drawn surface.
 *
 * Heights are in scene "up" units (coord::scene3::up), like the tile elevation
 * of the terrain chunks. An empty heightmap is flat (height 0 everywhere).
 *
 * This header has no dependencies, so it can be tested without the engine.
 */
class Heightmap {
public:
	/**
	 * Picking direction of the default openage camera: a point at height h
	 * is drawn at the same pixel as the ground point (ne + h * PICK_NE,
	 * se + h * PICK_SE, 0).
	 *
	 * Derivation: renderer/camera/definitions.h CAM_DIRECTION =
	 * (-sqrt(6)/4, -1/2, -sqrt(6)/4) in world space (x = se, y = up * r,
	 * z = -ne, r = 1/sqrt(8) from coord/scene.cpp). Moving up by h along the
	 * ray towards the camera moves (x, z) by h * r * 2 * sqrt(6)/4 = h * sqrt(3)/4.
	 */
	static constexpr double PICK_NE = -0.4330127018922193; // -sqrt(3)/4
	static constexpr double PICK_SE = 0.4330127018922193;  //  sqrt(3)/4

	Heightmap() = default;

	/**
	 * @param width Map width in tiles (ne direction).
	 * @param height Map height in tiles (se direction).
	 * @param corners (width + 1) * (height + 1) corner heights; empty = flat.
	 */
	Heightmap(size_t width, size_t height, std::vector<float> corners) :
		width{width},
		height{height},
		corners{std::move(corners)} {
		if (this->corners.size() != (width + 1) * (height + 1)) {
			this->corners.clear();
		}
		for (auto h : this->corners) {
			this->max_height = std::max(this->max_height, h);
		}
	}

	/**
	 * @return true if all corners have the height 0 (or there is no data).
	 */
	bool is_flat() const {
		return this->max_height <= 0.0f;
	}

	size_t get_width() const {
		return this->width;
	}

	size_t get_height() const {
		return this->height;
	}

	float get_max_height() const {
		return this->max_height;
	}

	const std::vector<float> &get_corners() const {
		return this->corners;
	}

	/**
	 * Height of a corner, clamped to the map.
	 */
	float corner(long ne, long se) const {
		if (this->corners.empty()) {
			return 0.0f;
		}
		ne = std::clamp(ne, 0L, static_cast<long>(this->width));
		se = std::clamp(se, 0L, static_cast<long>(this->height));
		return this->corners[ne + se * (this->width + 1)];
	}

	/**
	 * Height of the terrain surface at a position (tiles), clamped to the map.
	 */
	double at(double ne, double se) const {
		if (this->corners.empty()) {
			return 0.0;
		}
		ne = std::clamp(ne, 0.0, static_cast<double>(this->width));
		se = std::clamp(se, 0.0, static_cast<double>(this->height));
		long tile_ne = std::min(static_cast<long>(ne), static_cast<long>(this->width) - 1);
		long tile_se = std::min(static_cast<long>(se), static_cast<long>(this->height) - 1);
		double fx = ne - tile_ne;
		double fy = se - tile_se;

		double h00 = this->corner(tile_ne, tile_se);
		double h01 = this->corner(tile_ne, tile_se + 1);
		double h10 = this->corner(tile_ne + 1, tile_se);
		double h11 = this->corner(tile_ne + 1, tile_se + 1);

		if (fy >= fx) {
			// triangle (0,0) (0,1) (1,1)
			return h00 + fy * (h01 - h00) + fx * (h11 - h01);
		}
		// triangle (0,0) (1,1) (1,0)
		return h00 + fx * (h10 - h00) + fy * (h11 - h10);
	}

	/**
	 * Visible terrain point under a screen position.
	 *
	 * Input positions are the intersection of the view ray with the plane
	 * up = 0. The ray continues towards the camera in the direction
	 * (PICK_NE, PICK_SE) per height unit; the first terrain point seen from the
	 * camera is the largest height t with t <= height(ray(t)).
	 *
	 * @param ne Plane hit (ne).
	 * @param se Plane hit (se).
	 *
	 * @return Terrain point (ne, se, up).
	 */
	std::pair<std::pair<double, double>, double> pick(double ne, double se) const {
		if (this->is_flat()) {
			return {{ne, se}, 0.0};
		}

		auto above = [&](double t) {
			return t - this->at(ne + t * PICK_NE, se + t * PICK_SE);
		};

		// march from the highest possible point down to the plane (step < slope features)
		const double step = 0.05;
		double t_hi = this->max_height + step;
		double t_lo = t_hi;
		bool hit = false;
		while (t_lo > 0.0) {
			t_lo = std::max(0.0, t_hi - step);
			if (above(t_lo) <= 0.0) {
				hit = true;
				break;
			}
			t_hi = t_lo;
		}
		if (not hit) {
			// below the plane: cannot happen for heights >= 0
			return {{ne, se}, this->at(ne, se)};
		}

		// refine between t_lo (below/at surface) and t_hi (above)
		for (int i = 0; i < 20; ++i) {
			double mid = 0.5 * (t_lo + t_hi);
			if (above(mid) <= 0.0) {
				t_lo = mid;
			}
			else {
				t_hi = mid;
			}
		}
		double hit_ne = ne + t_lo * PICK_NE;
		double hit_se = se + t_lo * PICK_SE;
		return {{hit_ne, hit_se}, this->at(hit_ne, hit_se)};
	}

private:
	size_t width = 0;
	size_t height = 0;
	std::vector<float> corners{};
	float max_height = 0.0f;
};

} // namespace openage::gamestate
