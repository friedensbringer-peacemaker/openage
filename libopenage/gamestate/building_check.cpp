// Copyright 2026-2026 the openage authors. See copying.md for legal info.

/*
 * Host test of the building rules (XR fork W5, gamestate/building_math.h):
 *
 *   openage-building-check
 *
 * Parameter sweeps:
 * - build share f(n) for n = 1..20: n builders together give 1 + 0.33 * (n - 1)
 * - build progress: work rate and task value scale, speed factor k <= 0 counts
 *   as 1, the wonder caps k at 1.2
 * - HP gain while building: hp_max * p / T, guard for T <= 0
 * - repair: share f(n) with the half factor, one repairer = 12.5 HP per second,
 *   units at a quarter, costs = build cost * hp / (2 * hp_max) (full repair =
 *   half the build price, applied per resource kind), alive limit 0.5 HP
 * - wall line (L31): all directions on a circle of radius 1..15: length
 *   max(|dx|, |dy|) + 1, starts at the start, ends at the end, no tile twice,
 *   connected in the 8-neighbourhood, same tile set when dragged the other
 *   way, start == end is a single tile
 * - gates: orientation from the wall direction, 4 gate tiles along the wall,
 *   passage for own/ally units, locked gates block everyone (L32)
 * - garrison ejection below 20 % HP, only for finished buildings (spec 04 §4.6)
 * - wall costs: only valid segments count, cost = segments x segment cost
 * - placement (gamestate/building_place.h): elevation modes 0/1/2/3 on flat
 *   and sloped footprints, gate fit on straight walls in all four directions
 *   (anchor, centre and anchor back), no gate on bent or short walls, nearest
 *   wall segment, follow-up after building (L33), repair costs carried over
 *   steps sum up to the formula
 * Exit code 0 if all checks pass. No dependencies besides the standard library.
 */

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "gamestate/building_math.h"
#include "gamestate/building_place.h"

using namespace openage::gamestate;
using namespace openage::gamestate::building;

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, const std::string &what) {
	checks += 1;
	if (not ok) {
		failures += 1;
		if (failures <= 40) {
			std::cerr << "FAIL: " << what << std::endl;
		}
	}
}

bool near(double a, double b) {
	return std::abs(a - b) <= 1e-9;
}

void check_build() {
	// f(n) for n = 1..20: the first builder builds at full rate, every further
	// one adds a third; together they give 1 + 0.33 * (n - 1)
	for (int n = 1; n <= 20; n++) {
		const std::string tag = " n=" + std::to_string(n);
		check(build_share(n) == (1.0 + 0.33 * (n - 1)) / n, "build share," + tag);
		check(near(n * build_share(n), 1.0 + 0.33 * (n - 1)), "n builders together," + tag);
		check(build_share(n) >= 0.33 and build_share(n) <= 1.0, "share in [0.33, 1]," + tag);
	}
	check(build_share(0) == 0.0, "no builder");
	check(build_share(-3) == 0.0, "negative builder count");
	check(build_progress(1.0, 1.0, 1.0, 1.0, 1, false) == 1.0, "base progress");
	check(build_progress(1.0, 3.57, 2.0, 1.0, 1, false) == 7.14, "task value 3.57, dt 2");
	check(build_progress(1.0, 1.0, 1.0, 0.0, 1, false) == 1.0, "k = 0 counts as 1");
	check(build_progress(1.0, 1.0, 1.0, -5.0, 1, false) == 1.0, "k < 0 counts as 1");
	check(build_progress(1.0, 1.0, 1.0, 2.0, 1, false) == 2.0, "k > 1 speeds up");
	check(build_progress(1.0, 1.0, 1.0, 2.0, 1, true) == 1.2, "wonder caps k at 1.2");
	check(build_progress(1.0, 1.0, 1.0, 9.0, 1, true) == 1.2, "wonder caps high k");
	check(build_progress(1.0, 1.0, 1.0, 0.5, 1, true) == 0.5, "wonder keeps small k");
	check(build_progress(1.0, 1.0, 1.0, 1.0, 2, false) == (1.0 + 0.33) / 2, "second builder adds a third");
	check(build_progress(1.0, 1.0, 1.0, 1.0, 0, false) == 0.0, "no builder builds nothing");
	check(build_hp_gain(1000.0, 10.0, 100.0) == 100.0, "hp grows with the progress");
	check(build_hp_gain(1000.0, 100.0, 100.0) == 1000.0, "full progress = full hp");
	check(build_hp_gain(1200.0, 50.0, 100.0) == 600.0, "half progress = half hp");
	check(build_hp_gain(1000.0, 10.0, 0.0) == 0.0, "no build time");
	check(build_hp_gain(1000.0, 10.0, -1.0) == 0.0, "negative build time");
}

void check_repair() {
	// f(n) with the half factor: together the repairers give 1 + 0.5 * (n - 1)
	for (int n = 1; n <= 20; n++) {
		const std::string tag = " n=" + std::to_string(n);
		check(repair_share(n) == (1.0 + 0.5 * (n - 1)) / n, "repair share," + tag);
		check(near(n * repair_share(n), 1.0 + 0.5 * (n - 1)), "n repairers together," + tag);
		check(repair_share(n) >= 0.5 and repair_share(n) <= 1.0, "share in [0.5, 1]," + tag);
		check(near(repair_hp(12.5, 1.0, 1.0, n, false), 12.5 * (1.0 + 0.5 * (n - 1)) / n), "repair rate," + tag);
		check(near(repair_hp(12.5, 1.0, 1.0, n, true), 0.25 * repair_hp(12.5, 1.0, 1.0, n, false)), "units a quarter," + tag);
	}
	check(repair_share(0) == 0.0, "no repairer");
	check(repair_share(-2) == 0.0, "negative repairer count");
	check(repair_hp(12.5, 1.0, 1.0, 1, false) == 12.5, "one repairer = 12.5 hp per second");
	check(repair_hp(12.5, 1.0, 2.0, 1, false) == 25.0, "repair over dt");
	check(repair_hp(12.5, 1.0, 1.0, 1, true) == 3.125, "unit repair at a quarter");
	// costs: repairing h hp costs build cost * h / (2 * hp_max), per resource kind
	check(repair_cost(200.0, 1000.0, 1000.0) == 100.0, "full repair = half the build price");
	check(repair_cost(100.0, 500.0, 1000.0) == 25.0, "half repair = a quarter of the price");
	check(repair_cost(200.0, 1000.0, 0.0) == 0.0, "no hp max");
	check(near(repair_cost(275.0, 400.0, 1100.0), 275.0 * 400.0 / 2200.0), "cost formula");
	check(near(repair_cost(200.0, 400.0, 1000.0), 40.0) and near(repair_cost(100.0, 400.0, 1000.0), 20.0),
	      "costs per resource kind");
	// only living targets (spec 04 §4.5)
	check(can_repair(0.5), "alive at the limit");
	check(can_repair(1.0), "alive above the limit");
	check(not can_repair(0.4), "dead below the limit");
	check(not can_repair(0.0), "dead at 0");
}

void check_wall_line() {
	// all directions on a circle of radius 1..15: the 16 angles collapse to
	// the raster directions, each direction is checked once per radius
	const double pi = std::acos(-1.0);
	for (int r = 1; r <= 15; r++) {
		std::set<tile_t> dirs;
		for (int a = 0; a < 16; a++) {
			const int dx = static_cast<int>(std::lround(r * std::cos(a * pi / 8.0)));
			const int dy = static_cast<int>(std::lround(r * std::sin(a * pi / 8.0)));
			if ((dx == 0 and dy == 0) or not dirs.insert({dx, dy}).second) {
				continue;
			}
			const std::string tag = " (r=" + std::to_string(r) + ", d=" + std::to_string(dx) + "," + std::to_string(dy) + ")";
			const auto tiles = wall_line(40, 30, 40 + dx, 30 + dy);
			check(tiles.size() == static_cast<size_t>(std::max(std::abs(dx), std::abs(dy)) + 1), "length" + tag);
			check(not tiles.empty() and tiles.front() == tile_t{40, 30}, "starts at the start" + tag);
			check(tiles.back() == tile_t{40 + dx, 30 + dy}, "ends at the end" + tag);
			const std::set<tile_t> unique{tiles.begin(), tiles.end()};
			check(unique.size() == tiles.size(), "no tile twice" + tag);
			bool connected = tiles.size() > 1;
			for (size_t i = 1; i < tiles.size(); i++) {
				connected = connected
				            and std::max(std::abs(tiles[i].first - tiles[i - 1].first),
				                         std::abs(tiles[i].second - tiles[i - 1].second)) == 1;
			}
			check(connected, "steps in the 8-neighbourhood" + tag);
			// the same tile set when the wall is dragged the other way (L31)
			const auto back = wall_line(40 + dx, 30 + dy, 40, 30);
			const std::set<tile_t> back_set{back.begin(), back.end()};
			check(back_set == unique, "symmetric tile set" + tag);
			check(back.size() == tiles.size(), "same length both ways" + tag);
		}
	}
	// hand-checked lines
	const auto straight = wall_line(0, 0, 3, 0);
	check(straight.size() == 4 and straight[1] == tile_t{1, 0} and straight[3] == tile_t{3, 0}, "straight wall");
	const auto diag = wall_line(0, 0, 3, 3);
	check(diag.size() == 4 and diag[2] == tile_t{2, 2}, "diagonal wall");
	const auto stair = wall_line(0, 0, 3, 1);
	check(stair.size() == 4 and stair.front() == tile_t{0, 0} and stair.back() == tile_t{3, 1}, "stair wall");
	const auto single = wall_line(5, 7, 5, 7);
	check(single.size() == 1 and single[0] == tile_t{5, 7}, "start == end is one tile");
}

void check_gates() {
	check(gate_dir(3, 0) == gate_dir_t::EAST_WEST and gate_dir(-7, 0) == gate_dir_t::EAST_WEST, "horizontal wall");
	check(gate_dir(0, 4) == gate_dir_t::NORTH_SOUTH and gate_dir(0, -9) == gate_dir_t::NORTH_SOUTH, "vertical wall");
	check(gate_dir(2, -2) == gate_dir_t::NE_SW and gate_dir(-5, 5) == gate_dir_t::NE_SW, "diagonal NE-SW");
	check(gate_dir(2, 2) == gate_dir_t::NW_SE and gate_dir(-5, -5) == gate_dir_t::NW_SE, "diagonal NW-SE");
	check(gate_dir(11, -11).has_value() and gate_dir(6, 6).has_value(), "long diagonals");
	check(not gate_dir(0, 0).has_value(), "no direction on a point");
	check(not gate_dir(2, 1).has_value() and not gate_dir(-3, 7).has_value(), "mixed slope");
	// the 4 tiles of a gate along its wall, no tile twice
	for (auto dir : {gate_dir_t::NE_SW, gate_dir_t::NW_SE, gate_dir_t::NORTH_SOUTH, gate_dir_t::EAST_WEST}) {
		const auto tiles = gate_tiles(10, 20, dir);
		check(tiles.size() == static_cast<size_t>(GATE_LENGTH), "gate length 4");
		const std::set<tile_t> unique{tiles.begin(), tiles.end()};
		check(unique.size() == tiles.size(), "gate tiles distinct");
		check(tiles.front() == tile_t{10, 20}, "gate starts at the anchor");
		bool along = true;
		for (int i = 1; i < GATE_LENGTH; i++) {
			const auto &prev = tiles[static_cast<size_t>(i - 1)];
			const auto &tile = tiles[static_cast<size_t>(i)];
			along = along and std::max(std::abs(tile.first - prev.first), std::abs(tile.second - prev.second)) == 1;
		}
		check(along, "gate tiles follow the wall");
	}
	check(gate_tiles(10, 20, gate_dir_t::NORTH_SOUTH)[3] == tile_t{10, 23}, "north-south gate");
	check(gate_tiles(10, 20, gate_dir_t::EAST_WEST)[3] == tile_t{13, 20}, "east-west gate");
	check(gate_tiles(0, 0, gate_dir_t::NE_SW)[3] == tile_t{3, -3}, "NE-SW gate");
	check(gate_tiles(0, 0, gate_dir_t::NW_SE)[3] == tile_t{3, 3}, "NW-SE gate");
	// passage (L32): open for own and allied units, closed for enemies
	check(gate_passable(true, false), "own units pass");
	check(not gate_passable(false, false), "enemies blocked");
	check(not gate_passable(true, true), "locked blocks own units too");
	check(not gate_passable(false, true), "locked blocks enemies too");
}

void check_eject() {
	check(must_eject_garrison(19.9, 100.0, true), "eject below 20 %");
	check(not must_eject_garrison(20.0, 100.0, true), "no eject at 20 %");
	check(not must_eject_garrison(50.0, 100.0, true), "no eject above 20 %");
	check(must_eject_garrison(0.0, 100.0, true), "burned down ejects");
	check(not must_eject_garrison(0.0, 100.0, false), "unfinished keeps the garrison");
	check(not must_eject_garrison(10.0, 0.0, true), "no hp max");
}

void check_wall_costs() {
	check(segment_count({1, 1, 0, 1, 1, 1}) == 5, "only valid segments count");
	check(segment_count({0, 0, 0}) == 0, "nothing valid");
	check(segment_count({}) == 0, "empty wall");
	check(wall_cost({1, 0, 1}, 5.0) == 10.0, "cost = segments x segment cost");
	check(wall_cost({1, 1, 1, 1}, 5.0) == 20.0, "full wall price");
	check(wall_cost({0, 0, 0}, 5.0) == 0.0, "invalid wall is free (nothing placed)");
}

void check_placement() {
	auto flat = [](double h) { return corners_t{h, h, h, h}; };
	const std::vector<corners_t> plain(9, flat(0.0));
	for (int mode = 0; mode <= 3; ++mode) {
		check(elevation_ok(mode, plain, 4), "flat footprint ok in mode " + std::to_string(mode));
	}
	// a slope of 0.1 per corner: too steep for mode 2 (0.4 in total)
	std::vector<corners_t> slope;
	for (int i = 0; i < 4; ++i) {
		const double h = 0.1 * i;
		slope.push_back(corners_t{h, h + 0.1, h, h + 0.1});
	}
	check(not elevation_ok(2, slope, 0), "mode 2 rejects a slope of 0.4");
	check(elevation_ok(3, slope, 0), "mode 3 accepts a gentle slope");
	check(elevation_ok(0, slope, 0), "mode 0 never checks");
	// sweep: one tile raised by d levels
	for (int step = 0; step <= 30; ++step) {
		const double d = 0.1 * step;
		std::vector<corners_t> bump(9, flat(0.0));
		bump[8] = flat(d);
		const std::string at = std::to_string(d);
		check(elevation_ok(3, bump, 4) == (d <= LEVEL_STEP + 1e-9), "mode 3 bump of " + at);
		check(elevation_ok(1, bump, 4) == (d <= LEVEL_STEP + 1e-9), "mode 1 like mode 3, bump of " + at);
		check(elevation_ok(2, bump, 4) == (d <= FLAT_TOLERANCE + 1e-9), "mode 2 bump of " + at);
		check(elevation_ok(0, bump, 4), "mode 0 bump of " + at);
	}

	// gates on straight walls in every direction, cursor on each tile of the wall
	const std::vector<gate_dir_t> dirs{gate_dir_t::EAST_WEST, gate_dir_t::NORTH_SOUTH, gate_dir_t::NW_SE,
	                                   gate_dir_t::NE_SW};
	for (auto dir : dirs) {
		const auto unit = gate_tiles(0, 0, dir);
		std::set<std::pair<int, int>> wall;
		for (int i = -3; i <= 8; ++i) {
			wall.insert({20 + i * unit[1].first, 20 + i * unit[1].second});
		}
		auto is_wall = [&](int x, int y) { return wall.contains({x, y}); };
		for (int i = -3; i <= 8; ++i) {
			const int cx = 20 + i * unit[1].first;
			const int cy = 20 + i * unit[1].second;
			auto fit = gate_fit(cx, cy, is_wall);
			bool ok = fit.has_value() and fit->dir == dir;
			if (ok) {
				bool covers = false;
				for (const auto &t : gate_tiles(fit->anchor.first, fit->anchor.second, fit->dir)) {
					ok = ok and is_wall(t.first, t.second);
					covers = covers or t == tile_t{cx, cy};
				}
				ok = ok and covers;
				auto [ne, se] = gate_center(*fit);
				ok = ok and gate_anchor(ne, se, fit->dir) == fit->anchor;
			}
			check(ok, "gate fits on a straight wall, direction " + std::to_string(static_cast<int>(dir)) + ", tile "
			              + std::to_string(i));
		}
	}
	{
		// inner positions preferred: the cursor tile becomes the second gate tile
		std::set<std::pair<int, int>> wall{{0, 5}, {1, 5}, {2, 5}, {3, 5}, {4, 5}, {5, 5}};
		auto fit = gate_fit(2, 5, [&](int x, int y) { return wall.contains({x, y}); });
		check(fit and fit->anchor == tile_t{1, 5}, "cursor tile is the second gate tile");
		// bent wall: 2 tiles east, then 2 south: no straight run of 4
		std::set<std::pair<int, int>> bent{{0, 0}, {1, 0}, {2, 0}, {2, 1}, {2, 2}};
		check(not gate_fit(1, 0, [&](int x, int y) { return bent.contains({x, y}); }), "no gate on a bent wall");
		std::set<std::pair<int, int>> short_wall{{0, 0}, {1, 0}, {2, 0}};
		check(not gate_fit(1, 0, [&](int x, int y) { return short_wall.contains({x, y}); }), "no gate on 3 tiles");
		check(not gate_fit(1, 0, [](int, int) { return false; }), "no gate without a wall");
	}

	// nearest segment
	const std::vector<std::pair<double, double>> open{{5.5, 5.5}, {6.5, 5.5}, {7.5, 5.5}};
	check(nearest_segment(8.0, 5.0, open) == std::optional<size_t>{2}, "nearest segment is the closest");
	check(nearest_segment(0.0, 0.0, open) == std::optional<size_t>{0}, "nearest segment from the other side");
	check(nearest_segment(6.0, 5.5, open) == std::optional<size_t>{0}, "tie keeps the line order");
	check(not nearest_segment(0.0, 0.0, {}), "no open segment");

	// follow-up (L33)
	check(follow_up(true, {true, false, false, false}) == follow_t::FARM, "farm builder farms");
	check(follow_up(false, {false, true, false, false}) == follow_t::GATHER, "lumber camp builder cuts wood");
	check(follow_up(false, {false, false, true, true}) == follow_t::GATHER, "mining camp builder mines");
	check(follow_up(false, {true, false, false, false}) == follow_t::SEARCH, "mill builder searches");
	check(follow_up(false, {true, true, true, true}) == follow_t::SEARCH, "town centre builder searches");
	check(follow_up(false, {false, false, false, false}) == follow_t::SEARCH, "house builder searches");

	// repair costs over many steps: the whole units sum up to the formula
	for (int steps = 1; steps <= 50; steps += 7) {
		std::array<double, 4> owed{};
		std::array<double, 4> paid{};
		const std::array<double, 4> cost{0.0, 275.0, 0.0, 100.0};
		const double hp_max = 2400.0;
		const double hp_step = hp_max / steps;
		bool whole = true;
		for (int i = 0; i < steps; ++i) {
			auto due = repair_due(owed, cost, hp_step, hp_max);
			for (size_t r = 0; r < 4; ++r) {
				paid[r] += due[r];
				whole = whole and due[r] == std::floor(due[r]);
			}
		}
		check(whole, "repair costs are whole units, " + std::to_string(steps) + " steps");
		check(std::abs(paid[1] + owed[1] - 137.5) < 1e-6 and std::abs(paid[3] + owed[3] - 50.0) < 1e-6,
		      "full repair costs half the build price, " + std::to_string(steps) + " steps");
	}
}

} // namespace

int main() {
	check_build();
	check_repair();
	check_wall_line();
	check_gates();
	check_eject();
	check_wall_costs();
	check_placement();
	if (failures > 0) {
		std::cout << "building check: " << checks << " checks, " << failures << " failures" << std::endl;
		return EXIT_FAILURE;
	}
	std::cout << "building check: " << checks << " checks, 0 failures" << std::endl;
	return EXIT_SUCCESS;
}
