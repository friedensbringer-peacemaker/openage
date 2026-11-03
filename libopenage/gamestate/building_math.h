// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <utility>
#include <vector>


namespace openage::gamestate::building {

/**
 * Rules of building, repairing and walls (XR fork W5, docs/aoe2-spec 04
 * §4.3–4.6): build progress of a foundation with several builders, HP growth
 * while building, repair rate and repair costs, the wall drag line (decision
 * L31: Bresenham raster on the tile centres), gate orientation and passage
 * (decision L32) and the garrison ejection of burning buildings. Written from
 * the specification; build times, HP and costs are parameters that come from
 * the nyan data at runtime.
 *
 * Tile coordinates use the map grid with y growing to the south (north = -y).
 *
 * This header has no engine dependencies (host test gamestate/building_check.cpp).
 */

/// a second and every further builder adds a third of a single builder (spec 04 §4.3)
constexpr double BUILD_TEAM_FACTOR = 0.33;

/// a second and every further repairer adds half of a single repairer (spec 04 §4.5)
constexpr double REPAIR_TEAM_FACTOR = 0.5;

/// units (ships, siege) are repaired at a quarter of the building rate (spec 04 §4.5)
constexpr double REPAIR_UNIT_FACTOR = 0.25;

/// repairing 0 -> full costs half the build cost (spec 04 §4.5)
constexpr double REPAIR_COST_DIVISOR = 2.0;

/// speed factor of the wonder is capped (spec 04 §4.3, player resource 195)
constexpr double WONDER_SPEED_CAP = 1.2;

/// buildings below this share of HP_max eject their garrison (spec 04 §4.6)
constexpr double EJECT_HP_SHARE = 0.2;

/// a target must still be alive to be repaired (spec 04 §4.5)
constexpr double MIN_REPAIR_HP = 0.5;

/// a gate spans 4 tiles along its wall (spec 04 §4.4)
constexpr int GATE_LENGTH = 4;

// ---------------------------------------------------------------- building (spec 04 §4.3)

/// tile of a wall line or gate: (x, y) on the map grid
using tile_t = std::pair<int, int>;

/// share of one builder when n units build together: (1 + 0.33 * (n - 1)) / n;
/// n < 1 builds nothing
inline double build_share(int n) {
	if (n < 1) {
		return 0.0;
	}
	return (1.0 + BUILD_TEAM_FACTOR * (n - 1)) / n;
}

/**
 * Build points of one builder per tick: p = W * a1 * dt * k * f(n) (spec 04
 * §4.3). W = work rate of the builder, a1 = work value of the build task,
 * k = build speed resource 195 (k <= 0 counts as 1, the wonder caps k at 1.2),
 * n = number of units building together.
 */
inline double build_progress(double work_rate, double task_value, double dt, double speed_k, int n, bool wonder) {
	double k = speed_k <= 0.0 ? 1.0 : speed_k;
	if (wonder and k > WONDER_SPEED_CAP) {
		k = WONDER_SPEED_CAP;
	}
	return work_rate * task_value * dt * k * build_share(n);
}

/// HP the building gains for p build points: hp_max * p / T with T = build time
/// of the data (spec 04 §4.3); T <= 0 -> 0
inline double build_hp_gain(double hp_max, double p, double build_time) {
	if (build_time <= 0.0) {
		return 0.0;
	}
	return hp_max * p / build_time;
}

// ---------------------------------------------------------------- repair (spec 04 §4.5)

/// share of one repairer when n units repair together: (1 + 0.5 * (n - 1)) / n;
/// n < 1 repairs nothing
inline double repair_share(int n) {
	if (n < 1) {
		return 0.0;
	}
	return (1.0 + REPAIR_TEAM_FACTOR * (n - 1)) / n;
}

/**
 * HP one repairer restores per tick: W * a1 * dt * f(n) (spec 04 §4.5, one
 * repairer = 12.5 HP per second with W = 12.5). Units (ships, siege) are only
 * repaired at a quarter of the rate.
 */
inline double repair_hp(double work_rate, double task_value, double dt, int n, bool is_unit) {
	const double rate = work_rate * task_value * dt * repair_share(n);
	return is_unit ? rate * REPAIR_UNIT_FACTOR : rate;
}

/**
 * Resource cost of repairing h HP: build cost * h / (2 * hp_max) (spec 04 §4.5,
 * a full repair from 0 costs half the build price). Apply it per resource kind
 * with the build cost of that kind; hp_max <= 0 -> 0.
 */
inline double repair_cost(double build_cost, double hp, double hp_max) {
	if (hp_max <= 0.0) {
		return 0.0;
	}
	return build_cost * hp / (REPAIR_COST_DIVISOR * hp_max);
}

/// a target can be repaired while it still lives (HP >= 0.5, spec 04 §4.5)
inline bool can_repair(double hp) {
	return hp >= MIN_REPAIR_HP;
}

// ---------------------------------------------------------------- wall line (spec 04 §4.4, L31)

/**
 * Tiles of a wall drag from (x0, y0) to (x1, y1), both endpoints included
 * (spec 04 §4.4, decision L31): a Bresenham run on the tile centres with
 * 8-neighbourhood steps (horizontal, vertical, diagonal; stairs on other
 * slopes), each step moves at most 1 in x and y, no tile twice. The run starts
 * at the lexicographically smaller endpoint so both drag directions give the
 * same tile set; the returned order still runs from start to end.
 * Start == end gives a single tile.
 */
inline std::vector<tile_t> wall_line(int x0, int y0, int x1, int y1) {
	bool flipped = false;
	if (y1 < y0 or (y1 == y0 and x1 < x0)) {
		std::swap(x0, x1);
		std::swap(y0, y1);
		flipped = true;
	}
	const int dx = std::abs(x1 - x0);
	const int dy = -std::abs(y1 - y0);
	const int sx = x0 < x1 ? 1 : -1;
	const int sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;
	int x = x0;
	int y = y0;
	std::vector<tile_t> tiles;
	while (true) {
		tiles.emplace_back(x, y);
		if (x == x1 and y == y1) {
			break;
		}
		const int e2 = 2 * err;
		if (e2 >= dy) {
			err += dy;
			x += sx;
		}
		if (e2 <= dx) {
			err += dx;
			y += sy;
		}
	}
	if (flipped) {
		std::reverse(tiles.begin(), tiles.end());
	}
	return tiles;
}

// ---------------------------------------------------------------- gates (spec 04 §4.4, L32)

/// orientation of a gate: the wall runs along it (tile y grows to the south)
enum class gate_dir_t : uint8_t {
	/// wall from north-east to south-west (steps (+1, -1) / (-1, +1))
	NE_SW,
	/// wall from north-west to south-east (steps (+1, +1) / (-1, -1))
	NW_SE,
	/// wall from north to south (steps (0, +1))
	NORTH_SOUTH,
	/// wall from east to west (steps (+1, 0))
	EAST_WEST,
};

/**
 * Orientation of a wall of direction (dx, dy), e.g. for a gate placed on it:
 * exactly horizontal, vertical and diagonal (45°) walls, nullopt for a point
 * (0, 0) or a mixed slope.
 */
inline std::optional<gate_dir_t> gate_dir(int dx, int dy) {
	if (dx == 0 and dy == 0) {
		return std::nullopt;
	}
	if (dy == 0) {
		return gate_dir_t::EAST_WEST;
	}
	if (dx == 0) {
		return gate_dir_t::NORTH_SOUTH;
	}
	if (std::abs(dx) == std::abs(dy)) {
		return dx * dy < 0 ? gate_dir_t::NE_SW : gate_dir_t::NW_SE;
	}
	return std::nullopt;
}

/**
 * The GATE_LENGTH tiles of a gate: the anchor (cx, cy) is the first tile, the
 * others follow along the wall direction (in steps of +1: south, east,
 * south-east or north-east).
 */
inline std::vector<tile_t> gate_tiles(int cx, int cy, gate_dir_t dir) {
	int sx = 0;
	int sy = 0;
	switch (dir) {
	case gate_dir_t::NE_SW:
		sx = 1;
		sy = -1;
		break;
	case gate_dir_t::NW_SE:
		sx = 1;
		sy = 1;
		break;
	case gate_dir_t::NORTH_SOUTH:
		sx = 0;
		sy = 1;
		break;
	case gate_dir_t::EAST_WEST:
		sx = 1;
		sy = 0;
		break;
	}
	std::vector<tile_t> tiles;
	for (int i = 0; i < GATE_LENGTH; i++) {
		tiles.emplace_back(cx + sx * i, cy + sy * i);
	}
	return tiles;
}

/**
 * A gate lets own and allied units pass and blocks enemies; a locked gate
 * blocks everyone (spec 04 §4.4, decision L32).
 */
inline bool gate_passable(bool own_or_ally, bool locked) {
	return not locked and own_or_ally;
}

// ---------------------------------------------------------------- burning buildings (spec 04 §4.6)

/// a finished building below 20 % of HP_max ejects its garrison (spec 04 §4.6);
/// unfinished foundations keep it; hp_max <= 0 -> false
inline bool must_eject_garrison(double hp, double hp_max, bool finished) {
	if (not finished or hp_max <= 0.0) {
		return false;
	}
	return hp < EJECT_HP_SHARE * hp_max;
}

// ---------------------------------------------------------------- wall costs (spec 04 §4.4)

/// number of valid wall segments: every entry != 0 counts (invalid segments
/// are skipped when the wall is placed, spec 04 §4.4)
inline int segment_count(const std::vector<char> &valid) {
	int count = 0;
	for (char v : valid) {
		if (v) {
			count += 1;
		}
	}
	return count;
}

/// total cost of a wall drag: valid segments x cost per segment (spec 04 §4.4)
inline double wall_cost(const std::vector<char> &valid, double cost_per_segment) {
	return segment_count(valid) * cost_per_segment;
}

} // namespace openage::gamestate::building
