// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

#include "gamestate/building_math.h"


namespace openage::gamestate::building {

/**
 * Placement rules of round W5 that need more than one tile (docs/aoe2-spec 04 §4.2
 * rule 4 "Höhe", §4.4 gates on walls), the follow-up of a builder (decision L33)
 * and the next wall segment of a builder (§4.4 "nächstes zuerst"). Inputs are plain
 * numbers, so the rules are tested without the engine (gamestate/building_check.cpp).
 */

/// tolerance of "flat" (elevation mode 2) in elevation levels (own assumption: the generated
/// hills are smooth, an exact 0 would forbid town centres on any slope)
constexpr double FLAT_TOLERANCE = 0.25;

/// elevation mode 3 (and 1): footprint tiles may differ from the reference tile by one level
constexpr double LEVEL_STEP = 1.0;

/// a builder of another building looks this long for a foundation nearby (spec 04 §4.3, L33)
constexpr double FOLLOW_SEARCH_TIME = 3.0;

/// ... within this distance (tiles, own assumption: search radius of the build task unknown)
constexpr double FOLLOW_SEARCH_RADIUS = 10.0;

/// a camp builder starts gathering a resource within this distance of the camp (tiles)
constexpr double CAMP_RESOURCE_RADIUS = 10.0;

/// the four corner heights of a tile (any order)
using corners_t = std::array<double, 4>;

/// mean height of a tile
inline double tile_height(const corners_t &c) {
	return (c[0] + c[1] + c[2] + c[3]) / 4.0;
}

/**
 * Elevation rule of a footprint (spec 04 §4.2 rule 4, data field "elevation mode"):
 *  - 0: no check (walls, farms, towers, gates)
 *  - 2: all tiles flat: highest minus lowest corner at most FLAT_TOLERANCE
 *  - 3 (and 1, own assumption): every tile at most LEVEL_STEP from the reference tile
 *
 * @param tiles Corner heights of the footprint tiles.
 * @param reference Index of the reference tile (the anchor tile).
 */
inline bool elevation_ok(int mode, const std::vector<corners_t> &tiles, size_t reference) {
	if (tiles.empty()) {
		return true;
	}
	if (mode == 2) {
		double lo = tiles.front()[0];
		double hi = lo;
		for (const auto &t : tiles) {
			for (double h : t) {
				lo = std::min(lo, h);
				hi = std::max(hi, h);
			}
		}
		return hi - lo <= FLAT_TOLERANCE + 1e-9;
	}
	if (mode == 1 or mode == 3) {
		const double ref = tile_height(tiles[std::min(reference, tiles.size() - 1)]);
		for (const auto &t : tiles) {
			if (std::abs(tile_height(t) - ref) > LEVEL_STEP + 1e-9) {
				return false;
			}
		}
	}
	return true;
}

/// a gate that fits on a wall: first tile and orientation
struct GateFit {
	tile_t anchor;
	gate_dir_t dir;
};

/**
 * Where a gate goes when the cursor is on tile (x, y) (spec 04 §4.2 rule 5, §4.4):
 * all GATE_LENGTH tiles must be own wall tiles (\p wall(x, y)) on one straight line
 * through the cursor tile. Orientations in the order east-west, north-south,
 * north-west/south-east, north-east/south-west; the cursor tile prefers the inner
 * positions of the gate (second, third, first, last tile).
 *
 * @return nothing if no gate fits there.
 */
template <typename WallFn>
std::optional<GateFit> gate_fit(int x, int y, WallFn &&wall) {
	static constexpr std::array<gate_dir_t, 4> dirs{gate_dir_t::EAST_WEST, gate_dir_t::NORTH_SOUTH,
	                                                gate_dir_t::NW_SE, gate_dir_t::NE_SW};
	static constexpr std::array<int, GATE_LENGTH> offsets{1, 2, 0, 3};
	for (auto dir : dirs) {
		const auto unit = gate_tiles(0, 0, dir);
		const int sx = unit[1].first;
		const int sy = unit[1].second;
		for (int k : offsets) {
			const int ax = x - k * sx;
			const int ay = y - k * sy;
			bool fits = true;
			for (const auto &[tx, ty] : gate_tiles(ax, ay, dir)) {
				if (not wall(tx, ty)) {
					fits = false;
					break;
				}
			}
			if (fits) {
				return GateFit{{ax, ay}, dir};
			}
		}
	}
	return std::nullopt;
}

/// centre of a gate (tile coordinates of the map, between the tile centres)
inline std::pair<double, double> gate_center(const GateFit &fit) {
	const auto tiles = gate_tiles(fit.anchor.first, fit.anchor.second, fit.dir);
	double ne = 0.0;
	double se = 0.0;
	for (const auto &[tx, ty] : tiles) {
		ne += tx + 0.5;
		se += ty + 0.5;
	}
	return {ne / tiles.size(), se / tiles.size()};
}

/// first tile of a gate from its centre and orientation (inverse of gate_center)
inline tile_t gate_anchor(double ne, double se, gate_dir_t dir) {
	const auto unit = gate_tiles(0, 0, dir);
	const double half = 0.5 * (GATE_LENGTH - 1);
	return {static_cast<int>(std::floor(ne - half * unit[1].first)),
	        static_cast<int>(std::floor(se - half * unit[1].second))};
}

/**
 * Next wall segment of a builder: the nearest (squared distance) of the open
 * segments (spec 04 §4.4 "nächstes zuerst"); ties keep the order of the line.
 *
 * @return index into \p open, nothing if it is empty.
 */
inline std::optional<size_t> nearest_segment(double ne, double se, const std::vector<std::pair<double, double>> &open) {
	std::optional<size_t> best;
	double best_d = 0.0;
	for (size_t i = 0; i < open.size(); ++i) {
		const double dx = open[i].first - ne;
		const double dy = open[i].second - se;
		const double d = dx * dx + dy * dy;
		if (not best or d < best_d - 1e-9) {
			best = i;
			best_d = d;
		}
	}
	return best;
}

/// what a builder does after its building is finished (decision L33)
enum class follow_t {
	/// farm: its builder farms it (round W3)
	FARM,
	/// camp (drop site for wood, gold or stone, not food): gather the matching resource nearby
	GATHER,
	/// any other building: look for a foundation nearby for FOLLOW_SEARCH_TIME seconds
	SEARCH,
};

/**
 * Follow-up of a builder (decision L33): \p accepts = resources the finished building
 * takes as a drop site (food, wood, gold, stone), \p farm = the building is a field.
 */
inline follow_t follow_up(bool farm, const std::array<bool, 4> &accepts) {
	if (farm) {
		return follow_t::FARM;
	}
	if (not accepts[0] and (accepts[1] or accepts[2] or accepts[3])) {
		return follow_t::GATHER;
	}
	return follow_t::SEARCH;
}

/**
 * Repair costs of one step, carried over between steps: adds the cost of \p hp
 * repaired HP to \p owed (per resource, repair_cost) and returns the whole units
 * that are due now (they leave \p owed). Rounding to whole units keeps the
 * stockpile readable; nothing is lost over a repair.
 */
inline std::array<double, 4> repair_due(std::array<double, 4> &owed,
                                        const std::array<double, 4> &build_cost,
                                        double hp,
                                        double hp_max) {
	std::array<double, 4> due{};
	for (size_t i = 0; i < owed.size(); ++i) {
		owed[i] += repair_cost(build_cost[i], hp, hp_max);
		due[i] = std::floor(owed[i] + 1e-9);
		owed[i] -= due[i];
	}
	return due;
}

} // namespace openage::gamestate::building
