// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>


namespace openage::gamestate::econ {

/**
 * Geometry and timing helpers of the economy (XR fork): picking resources under
 * the cursor, the tiles next to an object, gather chunks.
 *
 * This header has no engine dependencies, so it can be tested without the engine
 * (gamestate/econ_check.cpp).
 */

struct vec3 {
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
};

inline vec3 operator-(const vec3 &a, const vec3 &b) {
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline vec3 operator+(const vec3 &a, const vec3 &b) {
	return {a.x + b.x, a.y + b.y, a.z + b.z};
}

inline vec3 operator*(const vec3 &a, double f) {
	return {a.x * f, a.y * f, a.z * f};
}

inline double dot(const vec3 &a, const vec3 &b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

/**
 * Shortest distance between an infinite line (picking ray) and a segment
 * (vertical axis of an object from its base to its top).
 *
 * @param origin Point on the line.
 * @param dir Direction of the line (any length > 0).
 * @param a Segment start.
 * @param b Segment end.
 *
 * @return Distance.
 */
inline double line_segment_distance(const vec3 &origin, const vec3 &dir, const vec3 &a, const vec3 &b) {
	const vec3 u = dir;
	const vec3 v = b - a;
	const vec3 w = origin - a;
	const double uu = dot(u, u);
	const double uv = dot(u, v);
	const double vv = dot(v, v);
	const double uw = dot(u, w);
	const double vw = dot(v, w);
	const double denom = uu * vv - uv * uv;

	// parameter on the segment (clamped), then the closest point on the line
	double s = 0.0;
	if (vv > 0.0) {
		if (denom > 1e-12 * uu * vv) {
			s = (uu * vw - uv * uw) / denom;
		}
		else {
			// parallel: any point, take the start
			s = 0.0;
		}
		s = std::clamp(s, 0.0, 1.0);
	}
	const vec3 p = a + v * s;
	const double t = uu > 0.0 ? dot(p - origin, u) / uu : 0.0;
	const vec3 q = origin + u * t;
	const vec3 d = p - q;
	return std::sqrt(dot(d, d));
}

struct tile_pos {
	long ne = 0;
	long se = 0;

	bool operator==(const tile_pos &other) const {
		return ne == other.ne and se == other.se;
	}
};

/**
 * Tiles covered by an object: the tile containing its anchor and every tile
 * whose centre lies within radius (per axis) of the anchor.
 *
 * @param ne Anchor position (tiles).
 * @param se Anchor position (tiles).
 * @param radius Hitbox radius (tiles), e.g. 0.5 for a tree, 2.0 for a town centre.
 */
inline std::vector<tile_pos> footprint(double ne, double se, double radius) {
	std::vector<tile_pos> tiles;
	const double r = std::max(0.0, radius) + 1e-6;
	const long x0 = static_cast<long>(std::floor(ne - r - 0.5));
	const long x1 = static_cast<long>(std::floor(ne + r - 0.5)) + 1;
	const long y0 = static_cast<long>(std::floor(se - r - 0.5));
	const long y1 = static_cast<long>(std::floor(se + r - 0.5)) + 1;
	const tile_pos own{static_cast<long>(std::floor(ne)), static_cast<long>(std::floor(se))};
	bool has_own = false;
	for (long y = y0; y <= y1; ++y) {
		for (long x = x0; x <= x1; ++x) {
			double cx = static_cast<double>(x) + 0.5;
			double cy = static_cast<double>(y) + 0.5;
			if (std::abs(cx - ne) <= r and std::abs(cy - se) <= r) {
				tiles.push_back({x, y});
				has_own = has_own or tile_pos{x, y} == own;
			}
		}
	}
	if (not has_own) {
		tiles.push_back(own);
	}
	return tiles;
}

/**
 * Tiles around an object's footprint (8-neighbourhood, without the footprint),
 * sorted by distance from a unit: candidates for the place where the unit
 * stands while gathering or dropping off.
 *
 * @param ne Object anchor (tiles).
 * @param se Object anchor (tiles).
 * @param radius Object hitbox radius (tiles).
 * @param from_ne Unit position (tiles).
 * @param from_se Unit position (tiles).
 */
inline std::vector<tile_pos> approach_tiles(double ne, double se, double radius, double from_ne, double from_se) {
	const auto covered = footprint(ne, se, radius);
	auto contains = [](const std::vector<tile_pos> &tiles, const tile_pos &t) {
		return std::find(tiles.begin(), tiles.end(), t) != tiles.end();
	};
	std::vector<tile_pos> ring;
	for (const auto &t : covered) {
		for (long dy = -1; dy <= 1; ++dy) {
			for (long dx = -1; dx <= 1; ++dx) {
				tile_pos n{t.ne + dx, t.se + dy};
				if (not contains(covered, n) and not contains(ring, n)) {
					ring.push_back(n);
				}
			}
		}
	}
	auto dist2 = [&](const tile_pos &t) {
		double dx = static_cast<double>(t.ne) + 0.5 - from_ne;
		double dy = static_cast<double>(t.se) + 0.5 - from_se;
		return dx * dx + dy * dy;
	};
	std::stable_sort(ring.begin(), ring.end(), [&](const tile_pos &a, const tile_pos &b) {
		return dist2(a) < dist2(b);
	});
	return ring;
}

/**
 * Check if a unit is close enough to an object to gather from it or drop off at it:
 * it stands on the footprint or on a tile next to it.
 *
 * @param slack Extra distance (tiles) that still counts, e.g. after a walk that ended
 *              slightly off the tile.
 */
inline bool in_reach(double unit_ne, double unit_se, double ne, double se, double radius, double slack = 0.0) {
	const auto covered = footprint(ne, se, radius);
	long x0 = covered.front().ne;
	long x1 = x0;
	long y0 = covered.front().se;
	long y1 = y0;
	for (const auto &t : covered) {
		x0 = std::min(x0, t.ne);
		x1 = std::max(x1, t.ne);
		y0 = std::min(y0, t.se);
		y1 = std::max(y1, t.se);
	}
	// tiles x0 - 1 .. x1 + 1 cover [x0 - 1, x1 + 2)
	return unit_ne >= static_cast<double>(x0 - 1) - slack and unit_ne < static_cast<double>(x1 + 2) + slack
	       and unit_se >= static_cast<double>(y0 - 1) - slack and unit_se < static_cast<double>(y1 + 2) + slack;
}

/**
 * One gather step: at most one unit of the resource, limited by the free capacity.
 *
 * @param carried Amount the unit carries.
 * @param capacity Carry capacity.
 * @param rate Gather rate (per second, > 0).
 *
 * @return Amount to gather in this step (0 if full) and its duration in seconds.
 */
inline std::pair<double, double> gather_chunk(double carried, double capacity, double rate) {
	double amount = std::min(1.0, capacity - carried);
	if (amount <= 1e-9 or rate <= 0.0) {
		return {0.0, 0.0};
	}
	return {amount, amount / rate};
}

} // namespace openage::gamestate::econ
