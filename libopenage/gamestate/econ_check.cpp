// Copyright 2026-2026 the openage authors. See copying.md for legal info.

/*
 * Host test of the economy helpers (XR fork).
 *
 *   openage-econ-check
 *
 * Parameter sweeps over the dependency-free parts of the economy:
 * - resource names -> types, stockpile (start values, add, spend all-or-nothing,
 *   concurrent adds from several threads)
 * - footprints (tree, mine, town centre) and the tiles next to them, sorted by distance
 * - reach test: every tile next to a footprint is in reach, two tiles away is not
 * - gather chunks: one unit per step, capacity limit, trip time 10 wood / 0.39/s
 * - picking: the ray of the default camera through points on a tree's axis hits it,
 *   points beside it miss
 * - random maps: every start resource (berries, gold, stone) and every town centre
 *   has a free tile next to it (villagers can reach them), resources belong to gaia
 * Exit code 0 if all checks pass. No dependencies besides the map generator.
 */

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "gamestate/econ_math.h"
#include "gamestate/map_generator.h"
#include "gamestate/resources.h"

using namespace openage::gamestate;
using namespace openage::gamestate::econ;

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

/// world position of a map point (coord::scene3::to_world_space: x = se, y = up / sqrt(8), z = -ne)
vec3 world(double ne, double se, double up) {
	return {se, up * 0.353553391, -ne};
}

/// picking direction of the default camera (renderer/camera/definitions.h)
const vec3 CAM{-std::sqrt(6.0) / 4.0, -0.5, -std::sqrt(6.0) / 4.0};

/// ground point (y = 0) on the camera ray through a world point
vec3 ground_hit(const vec3 &p) {
	double t = -p.y / CAM.y;
	return p + CAM * t;
}

void check_resources() {
	check(resource_from_name("hd_base.data.util.resource.types.Food") == resource_t::FOOD, "name food");
	check(resource_from_name("hd_base.data.util.resource.types.Wood") == resource_t::WOOD, "name wood");
	check(resource_from_name("aoe2_base.data.util.resource.types.Gold") == resource_t::GOLD, "name gold");
	check(resource_from_name("Stone") == resource_t::STONE, "name stone without module");
	check(not resource_from_name("hd_base.data.util.resource.types.PopulationSpace"), "population is no stock");
	check(not resource_from_name("x.Woodland"), "no prefix match");

	ResourceStock stock;
	auto start = stock.get();
	check(start[0] == 200 and start[1] == 200 and start[2] == 100 and start[3] == 200, "start 200/200/100/200");
	check(stock.add(resource_t::WOOD, 10.0) == 210.0, "add wood");
	check(stock.add(resource_t::WOOD, -5.0) == 210.0, "negative add ignored");
	check(stock.spend({50, 60, 0, 0}), "spend affordable");
	check(stock.get(resource_t::FOOD) == 150 and stock.get(resource_t::WOOD) == 150, "spent");
	check(not stock.spend({0, 0, 101, 0}), "spend too much");
	check(stock.get(resource_t::GOLD) == 100, "nothing spent on failure");
	ResourceStock copy{stock};
	check(copy.get() == stock.get(), "copy");

	// concurrent adds (simulation) and reads (presenter)
	ResourceStock shared{{0, 0, 0, 0}};
	std::vector<std::thread> threads;
	for (int k = 0; k < 4; ++k) {
		threads.emplace_back([&shared, k]() {
			for (int i = 0; i < 10000; ++i) {
				shared.add(static_cast<resource_t>(k % 4), 1.0);
				(void)shared.get();
			}
		});
	}
	for (auto &t : threads) {
		t.join();
	}
	auto total = shared.get();
	check(total[0] == 10000 and total[1] == 10000 and total[2] == 10000 and total[3] == 10000, "concurrent adds");
}

void check_geometry() {
	// tree: own tile only, for anchors jittered by up to 0.12 tiles
	for (double jx : {-0.12, 0.0, 0.12}) {
		for (double jy : {-0.12, 0.0, 0.12}) {
			auto fp = footprint(10.5 + jx, 7.5 + jy, 0.5);
			check(fp.size() == 1 and fp[0] == tile_pos{10, 7}, "tree footprint");
			auto ring = approach_tiles(10.5 + jx, 7.5 + jy, 0.5, 3.0, 7.5);
			check(ring.size() == 8, "tree ring 8 tiles");
			check(ring.front() == tile_pos{9, 7}, "nearest side towards the unit");
			for (const auto &t : ring) {
				check(in_reach(t.ne + 0.5, t.se + 0.5, 10.5 + jx, 7.5 + jy, 0.5), "ring tile in reach (tree)");
			}
			check(not in_reach(12.5, 7.5, 10.5 + jx, 7.5 + jy, 0.5), "two tiles away not in reach");
		}
	}
	// town centre: anchor on a tile corner, 4x4 tiles
	{
		auto fp = footprint(20.0, 30.0, 2.0);
		check(fp.size() == 16, "town centre footprint 4x4");
		auto ring = approach_tiles(20.0, 30.0, 2.0, 25.0, 30.0);
		check(ring.size() == 20, "town centre ring 20 tiles");
		check(ring.front().ne == 22, "nearest side of the town centre");
		for (const auto &t : ring) {
			check(in_reach(t.ne + 0.5, t.se + 0.5, 20.0, 30.0, 2.0), "ring tile in reach (town centre)");
			for (const auto &f : fp) {
				check(not(t == f), "ring outside footprint");
			}
		}
		check(not in_reach(23.5, 30.5, 20.0, 30.0, 2.0), "outside reach of the town centre");
	}
	// radius sweep: footprint contains the anchor tile, ring surrounds it
	for (double r = 0.0; r <= 3.0; r += 0.25) {
		for (double ne = 5.0; ne < 6.0; ne += 0.125) {
			auto fp = footprint(ne, 5.5, r);
			bool own = false;
			for (const auto &t : fp) {
				own = own or t == tile_pos{static_cast<long>(std::floor(ne)), 5};
			}
			check(own, "footprint contains the anchor tile");
			auto ring = approach_tiles(ne, 5.5, r, 0.0, 0.0);
			check(ring.size() >= 8, "ring has at least 8 tiles");
			for (const auto &t : ring) {
				check(in_reach(t.ne + 0.5, t.se + 0.5, ne, 5.5, r), "ring in reach (sweep)");
			}
		}
	}
}

void check_chunks() {
	for (double rate : {0.31, 0.38, 0.39}) {
		double carried = 0.0;
		double seconds = 0.0;
		int steps = 0;
		while (true) {
			auto [amount, t] = gather_chunk(carried, 10.0, rate);
			if (amount <= 0.0) {
				break;
			}
			check(amount <= 1.0 + 1e-9, "chunk at most one unit");
			carried += amount;
			seconds += t;
			steps += 1;
			check(steps < 100, "chunks terminate");
			if (steps >= 100) {
				break;
			}
		}
		check(std::abs(carried - 10.0) < 1e-9, "fills to capacity");
		check(steps == 10, "10 steps for 10 units");
		check(std::abs(seconds - 10.0 / rate) < 1e-6, "trip time = capacity / rate");
	}
	auto [a, t] = gather_chunk(9.5, 10.0, 0.5);
	check(std::abs(a - 0.5) < 1e-9 and std::abs(t - 1.0) < 1e-9, "partial last chunk");
	auto [z, tz] = gather_chunk(10.0, 10.0, 0.5);
	check(z == 0.0 and tz == 0.0, "full: no chunk");
	auto [zr, tzr] = gather_chunk(0.0, 10.0, 0.0);
	check(zr == 0.0 and tzr == 0.0, "no rate: no chunk");
}

void check_picking() {
	// tree at (12.5, 8.5) with hitbox height 2 (sprite axis 0.6 * 2 up units, as econ.cpp)
	for (double up0 : {0.0, 1.5, 3.0}) {
		const vec3 base = world(12.5, 8.5, up0);
		const vec3 top = base + vec3{0.0, 0.6 * 2.0, 0.0};
		for (double f = 0.0; f <= 1.0; f += 0.1) {
			auto p = base + (top - base) * f;
			auto hit = ground_hit(p);
			check(line_segment_distance(hit, CAM, base, top) < 1e-6, "ray through the axis hits");
		}
		// beside the tree (along the screen x axis: perpendicular to the camera in the ground plane)
		const vec3 side{std::sqrt(0.5), 0.0, -std::sqrt(0.5)};
		auto miss = ground_hit(base + side * 1.0);
		check(line_segment_distance(miss, CAM, base, top) > 0.9, "one tile beside misses");
		auto near = ground_hit(base + side * 0.3);
		check(line_segment_distance(near, CAM, base, top) < 0.45, "0.3 beside still picks");
		// far above the top
		auto above = ground_hit(top + vec3{0.0, 2.0, 0.0});
		check(line_segment_distance(above, CAM, base, top) > 0.9, "far above misses");
	}
}

void check_maps() {
	size_t maps = 0;
	for (size_t size : {48, 64, 96}) {
		for (uint32_t seed = 1; seed <= 12; ++seed) {
			MapSettings s;
			s.type = map_type_t::RANDOM;
			s.seed = seed;
			s.size = size;
			auto m = generate_map(s);
			const auto N = m.width;
			std::vector<bool> blocked(N * N, false);
			for (auto i : m.blocked) {
				blocked[i] = true;
			}
			auto free = [&](long ne, long se) {
				if (ne < 0 or se < 0 or ne >= static_cast<long>(N) or se >= static_cast<long>(N)) {
					return false;
				}
				auto i = static_cast<size_t>(ne) + static_cast<size_t>(se) * N;
				auto t = m.tiles[i];
				bool water = t == map_terrain_t::WATER or t == map_terrain_t::WATER_MEDIUM
				             or t == map_terrain_t::WATER_DEEP;
				return not water and not blocked[i];
			};
			const std::string tag = "seed " + std::to_string(seed) + " size " + std::to_string(size);
			const size_t gaia = m.starts.size();
			size_t resources = 0;
			for (const auto &o : m.objects) {
				double radius = o.kind == map_object_t::TOWN_CENTER ? 2.0 : 0.5;
				bool start_object = o.kind == map_object_t::TOWN_CENTER or o.kind == map_object_t::BERRIES
				                    or o.kind == map_object_t::GOLD or o.kind == map_object_t::STONE;
				if (o.kind == map_object_t::VILLAGER) {
					check(o.owner < gaia, tag + " villager owned by a player");
					continue;
				}
				if (o.kind == map_object_t::TOWN_CENTER) {
					check(o.owner < gaia, tag + " town centre owned by a player");
				}
				else {
					check(o.owner == gaia, tag + " resource owned by gaia");
					resources += 1;
				}
				if (not start_object) {
					continue;
				}
				size_t sides = 0;
				for (const auto &t : approach_tiles(o.ne, o.se, radius, o.ne, o.se)) {
					sides += free(t.ne, t.se) ? 1 : 0;
				}
				check(sides > 0, tag + " " + to_string(o.kind) + " has a free side");
			}
			check(resources > 0, tag + " has resources");
			maps += 1;
		}
	}
	std::cout << "maps checked: " << maps << std::endl;
}

} // namespace

int main() {
	check_resources();
	check_geometry();
	check_chunks();
	check_picking();
	check_maps();

	std::cout << "econ check: " << checks << " checks, " << failures << " failed" << std::endl;
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
