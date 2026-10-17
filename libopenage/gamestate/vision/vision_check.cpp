// Copyright 2026-2026 the openage authors. See copying.md for legal info.

/**
 * Host test of the fog of war grid (XR fork, gamestate/vision/vision_grid.h),
 * without the engine:
 *  - sight circles: symmetric, contain the centre, grow with the radius,
 *    the familiar width (radius 4 -> 9 tiles across),
 *  - incremental stamping == full recomputation after every step of random
 *    walks (parameter sweep: map sizes incl. non-square, radii, unit counts,
 *    units on and beyond the map border), explored never shrinks,
 *  - order independence (deterministic), visible/explored counters,
 *  - footprints of buildings (any tile visible), display rules (classify, all
 *    combinations), explored text round trip and damaged texts,
 *  - save format round trip of the vision mode and the explored tiles,
 *  - minimap ground colours of a generated map,
 *  - time per tick of 200 moving units on a 256 x 256 map.
 *
 *   openage-vision-check
 */

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "gamestate/map_generator.h"
#include "gamestate/save_format.h"
#include "gamestate/vision/vision_grid.h"
#include "gamestate/vision/vision_snapshot.h"

using namespace openage::gamestate;
using namespace openage::gamestate::vision;

namespace {

int failures = 0;
size_t checks = 0;

#define CHECK(cond, ...)                                       \
	do {                                                       \
		++checks;                                              \
		if (not(cond)) {                                       \
			++failures;                                        \
			if (failures < 40) {                               \
				std::printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
				std::printf(__VA_ARGS__);                      \
				std::printf("\n");                             \
			}                                                  \
		}                                                      \
	} while (0)

struct Unit {
	size_t player;
	double ne;
	double se;
	int q;
};

/// counters from scratch (reference)
std::vector<std::vector<uint16_t>> full(size_t w, size_t h, size_t players, const std::vector<Unit> &units) {
	std::vector<std::vector<uint16_t>> c(players, std::vector<uint16_t>(w * h, 0));
	for (const auto &u : units) {
		const int cn = tile_of(u.ne);
		const int cs = tile_of(u.se);
		for (const auto &[dn, ds] : make_disk(u.q)) {
			const int x = cn + dn;
			const int y = cs + ds;
			if (x >= 0 and y >= 0 and static_cast<size_t>(x) < w and static_cast<size_t>(y) < h) {
				c[u.player][static_cast<size_t>(x) + static_cast<size_t>(y) * w] += 1;
			}
		}
	}
	return c;
}

void check_disks() {
	size_t last = 0;
	for (int q = 0; q <= 64; ++q) {
		auto d = make_disk(q);
		bool centre = false;
		int extent = 0;
		for (const auto &[dn, ds] : d) {
			centre = centre or (dn == 0 and ds == 0);
			extent = std::max(extent, std::abs(dn));
			bool mirrored = false;
			for (const auto &[a, b] : d) {
				if (a == -dn and b == ds) {
					mirrored = true;
				}
			}
			CHECK(mirrored, "disk q=%d not symmetric at (%d, %d)", q, dn, ds);
		}
		CHECK(centre, "disk q=%d without centre", q);
		CHECK(d.size() >= last, "disk q=%d smaller than q=%d", q, q - 1);
		const double r = q / 4.0;
		CHECK(extent == static_cast<int>(std::floor(r + 0.5)), "disk q=%d extent %d", q, extent);
		last = d.size();
	}
	CHECK(make_disk(quantize_radius(4.0)).size() == 69, "radius 4: %zu tiles", make_disk(16).size());
	int across = 0;
	for (const auto &[dn, ds] : make_disk(16)) {
		if (ds == 0) {
			across += 1;
		}
	}
	CHECK(across == 9, "radius 4: %d tiles across", across);
	CHECK(quantize_radius(-1.0) == 0 and quantize_radius(1000.0) == 256, "quantize limits");
}

void check_walks() {
	struct Case {
		size_t w, h, players, units;
		int q_min, q_max;
	};
	const std::vector<Case> cases{
		{16, 16, 2, 4, 0, 24}, {48, 48, 3, 20, 8, 40}, {48, 64, 2, 12, 4, 32}, {65, 17, 2, 9, 12, 64},
		{64, 64, 4, 40, 16, 48}, {1, 1, 1, 2, 0, 12},
	};
	size_t step_count = 0;
	for (size_t ci = 0; ci < cases.size(); ++ci) {
		const auto &c = cases[ci];
		for (uint32_t seed = 1; seed <= 6; ++seed) {
			std::mt19937 rng{seed * 7919u + static_cast<uint32_t>(ci)};
			auto uni = [&](double a, double b) { return std::uniform_real_distribution<double>{a, b}(rng); };
			VisionGrid grid{c.w, c.h, c.players};
			std::vector<Unit> units;
			std::vector<std::vector<uint8_t>> explored_before(c.players, std::vector<uint8_t>(c.w * c.h, 0));
			for (size_t i = 0; i < c.units; ++i) {
				Unit u{i % c.players, uni(-3.0, static_cast<double>(c.w) + 3.0), uni(-3.0, static_cast<double>(c.h) + 3.0),
				       std::uniform_int_distribution<int>{c.q_min, c.q_max}(rng)};
				grid.stamp(u.player, tile_of(u.ne), tile_of(u.se), u.q, +1);
				units.push_back(u);
			}
			for (int step = 0; step < 40; ++step, ++step_count) {
				// move some units (incremental: remove old circle, add new one), change some ranges
				for (auto &u : units) {
					if (uni(0.0, 1.0) < 0.5) {
						continue;
					}
					Unit n = u;
					n.ne += uni(-1.7, 1.7);
					n.se += uni(-1.7, 1.7);
					if (uni(0.0, 1.0) < 0.1) {
						n.q = std::uniform_int_distribution<int>{c.q_min, c.q_max}(rng);
					}
					if (tile_of(n.ne) != tile_of(u.ne) or tile_of(n.se) != tile_of(u.se) or n.q != u.q) {
						grid.stamp(u.player, tile_of(u.ne), tile_of(u.se), u.q, -1);
						grid.stamp(n.player, tile_of(n.ne), tile_of(n.se), n.q, +1);
					}
					u = n;
				}
				const auto ref = full(c.w, c.h, c.players, units);
				for (size_t p = 0; p < c.players; ++p) {
					size_t vis = 0;
					size_t mismatch = 0;
					size_t shrunk = 0;
					std::vector<uint8_t> states;
					grid.fill_states(p, states);
					for (size_t y = 0; y < c.h; ++y) {
						for (size_t x = 0; x < c.w; ++x) {
							const size_t i = x + y * c.w;
							const auto count = grid.count(p, static_cast<int>(x), static_cast<int>(y));
							if (count != ref[p][i]) {
								mismatch += 1;
							}
							if (count > 0) {
								vis += 1;
							}
							const bool ex = grid.explored(p, static_cast<int>(x), static_cast<int>(y));
							if (count > 0 and not ex) {
								mismatch += 1;
							}
							if (explored_before[p][i] and not ex) {
								shrunk += 1;
							}
							explored_before[p][i] = ex ? 1 : 0;
							const uint8_t expect = count > 0 ? 2 : ex ? 1 : 0;
							if (states[i] != expect) {
								mismatch += 1;
							}
						}
					}
					CHECK(mismatch == 0, "case %zu seed %u step %d player %zu: %zu cells differ from the full recomputation",
					      ci, seed, step, p, mismatch);
					CHECK(shrunk == 0, "case %zu seed %u: explored shrank by %zu", ci, seed, shrunk);
					CHECK(vis == grid.visible_count(p), "case %zu: visible counter %zu != %zu", ci, grid.visible_count(p), vis);
				}
			}
			// all units gone: nothing visible, explored stays
			const size_t explored_n = grid.explored_count(0);
			for (const auto &u : units) {
				grid.stamp(u.player, tile_of(u.ne), tile_of(u.se), u.q, -1);
			}
			for (size_t p = 0; p < c.players; ++p) {
				CHECK(grid.visible_count(p) == 0, "case %zu: %zu tiles visible after removing all", ci, grid.visible_count(p));
			}
			CHECK(grid.explored_count(0) == explored_n, "case %zu: explored changed by removing sight", ci);
		}
	}
	std::printf("random walks: %zu steps compared with the full recomputation\n", step_count);
}

void check_order() {
	std::mt19937 rng{42};
	std::vector<Unit> units;
	for (int i = 0; i < 60; ++i) {
		units.push_back({static_cast<size_t>(i % 2), std::uniform_real_distribution<double>{0, 48}(rng),
		                 std::uniform_real_distribution<double>{0, 48}(rng), 8 + i % 20});
	}
	VisionGrid a{48, 48, 2};
	for (const auto &u : units) {
		a.stamp(u.player, tile_of(u.ne), tile_of(u.se), u.q, +1);
	}
	for (int round = 0; round < 5; ++round) {
		std::shuffle(units.begin(), units.end(), rng);
		VisionGrid b{48, 48, 2};
		for (const auto &u : units) {
			b.stamp(u.player, tile_of(u.ne), tile_of(u.se), u.q, +1);
		}
		bool same = true;
		for (size_t p = 0; p < 2; ++p) {
			for (int y = 0; y < 48; ++y) {
				for (int x = 0; x < 48; ++x) {
					same = same and a.count(p, x, y) == b.count(p, x, y) and a.explored(p, x, y) == b.explored(p, x, y);
				}
			}
		}
		CHECK(same, "stamp order %d changes the grid", round);
		CHECK(a.explored_text(0) == b.explored_text(0), "explored text depends on the order");
	}
}

void check_footprint() {
	VisionGrid g{32, 32, 1};
	// one circle radius 1 at tile (10, 10)
	g.stamp(0, 10, 10, 4, +1);
	CHECK(g.footprint_state(0, 10.5, 10.5, 0.0) == tile_state_t::VISIBLE, "unit on the centre tile");
	CHECK(g.footprint_state(0, 12.5, 10.5, 0.0) == tile_state_t::UNEXPLORED, "unit two tiles away (radius 1)");
	// a building of 4 x 4 tiles (radius 2) with the anchor at (14, 10): tiles 12..15 -> 12 is not in sight
	CHECK(g.footprint_state(0, 14.0, 10.0, 2.0) == tile_state_t::UNEXPLORED, "building outside");
	// anchor (13, 10): tiles 11..14 -> tile 11 is in sight
	CHECK(g.footprint_state(0, 13.0, 10.0, 2.0) == tile_state_t::VISIBLE, "building partly in sight");
	g.stamp(0, 10, 10, 4, -1);
	CHECK(g.footprint_state(0, 13.0, 10.0, 2.0) == tile_state_t::EXPLORED, "building in the fog");
	// footprint sweep: the state is the best state of the econ footprint tiles
	for (int q = 0; q <= 32; q += 4) {
		VisionGrid h{24, 24, 1};
		h.stamp(0, 12, 12, q, +1);
		for (double ne = 2.0; ne < 22.0; ne += 0.5) {
			for (double r : {0.0, 0.5, 1.0, 1.5, 2.0}) {
				auto best = tile_state_t::UNEXPLORED;
				for_footprint(ne, 12.25, r, [&](int x, int y) {
					if (h.state(0, x, y) == tile_state_t::VISIBLE) {
						best = tile_state_t::VISIBLE;
					}
					else if (h.state(0, x, y) == tile_state_t::EXPLORED and best == tile_state_t::UNEXPLORED) {
						best = tile_state_t::EXPLORED;
					}
				});
				CHECK(h.footprint_state(0, ne, 12.25, r) == best, "footprint q=%d ne=%.1f r=%.1f", q, ne, r);
			}
		}
	}
}

void check_classify() {
	for (int own = 0; own < 2; ++own) {
		for (int movable = 0; movable < 2; ++movable) {
			for (int gaia = 0; gaia < 2; ++gaia) {
				for (int rem = 0; rem < 2; ++rem) {
					for (auto best : {tile_state_t::UNEXPLORED, tile_state_t::EXPLORED, tile_state_t::VISIBLE}) {
						auto d = classify(own, movable, gaia, rem, best);
						display_t expect;
						if (own or best == tile_state_t::VISIBLE) {
							expect = display_t::NORMAL;
						}
						else if (best == tile_state_t::UNEXPLORED or movable) {
							expect = display_t::HIDDEN;
						}
						else {
							expect = gaia or rem ? display_t::FOGGED : display_t::HIDDEN;
						}
						CHECK(d == expect, "classify own=%d movable=%d gaia=%d remembered=%d best=%d", own, movable, gaia,
						      rem, static_cast<int>(best));
					}
				}
			}
		}
	}
	// the important cases in words
	CHECK(classify(false, true, false, false, tile_state_t::EXPLORED) == display_t::HIDDEN, "enemy unit in the fog");
	CHECK(classify(false, false, false, true, tile_state_t::EXPLORED) == display_t::FOGGED, "seen enemy building in the fog");
	CHECK(classify(false, false, false, false, tile_state_t::EXPLORED) == display_t::HIDDEN, "unseen enemy building");
	CHECK(classify(false, false, true, false, tile_state_t::EXPLORED) == display_t::FOGGED, "gold mine on explored land");
	CHECK(classify(false, false, true, false, tile_state_t::UNEXPLORED) == display_t::HIDDEN, "gold mine in the dark");
	CHECK(classify(true, true, false, false, tile_state_t::UNEXPLORED) == display_t::NORMAL, "own unit");
}

void check_text() {
	std::mt19937 rng{7};
	for (size_t w : {1, 3, 16, 48, 255}) {
		for (size_t h : {1, 2, 16, 64}) {
			for (int pattern = 0; pattern < 5; ++pattern) {
				VisionGrid g{w, h, 2};
				if (pattern == 1) {
					g.explore_all(0);
				}
				else if (pattern >= 2) {
					for (int i = 0; i < 1 + pattern * 3; ++i) {
						g.stamp(0, std::uniform_int_distribution<int>{0, static_cast<int>(w)}(rng),
						        std::uniform_int_distribution<int>{0, static_cast<int>(h)}(rng),
						        std::uniform_int_distribution<int>{0, 30}(rng), +1);
					}
				}
				const auto text = g.explored_text(0);
				VisionGrid r{w, h, 2};
				CHECK(r.set_explored_text(0, text), "%zux%zu pattern %d: text rejected (%s)", w, h, pattern, text.substr(0, 40).c_str());
				bool same = r.explored_count(0) == g.explored_count(0);
				for (int y = 0; y < static_cast<int>(h); ++y) {
					for (int x = 0; x < static_cast<int>(w); ++x) {
						same = same and r.explored(0, x, y) == g.explored(0, x, y);
					}
				}
				CHECK(same, "%zux%zu pattern %d: round trip differs", w, h, pattern);
				CHECK(r.explored_text(0) == text, "%zux%zu: text not stable", w, h);
				// another map size and damaged texts are rejected, nothing changes
				VisionGrid other{w + 1, h, 1};
				CHECK(not other.set_explored_text(0, text), "%zux%zu: accepted for another map size", w, h);
				for (const std::string &bad : {std::string{""}, std::string{"x:"}, text + ",1", text + ",", text + "a",
				                               std::to_string(w) + "x" + std::to_string(h) + ":1,-1"}) {
					VisionGrid b{w, h, 1};
					CHECK(not b.set_explored_text(0, bad) and b.explored_count(0) == 0, "damaged text accepted: '%s'",
					      bad.substr(0, 40).c_str());
				}
			}
		}
	}
	VisionGrid small{4, 4, 1};
	small.stamp(0, 1, 1, 0, +1);
	CHECK(small.explored_text(0) == "4x4:5,1,10", "text format: %s", small.explored_text(0).c_str());
}

void check_save() {
	for (auto mode : {vision_mode_t::NORMAL, vision_mode_t::EXPLORED, vision_mode_t::ALL}) {
		save::SaveData d;
		d.title = "Sicht";
		d.map.type = map_type_t::RANDOM;
		d.map.vision = mode;
		for (uint64_t p = 0; p < 3; ++p) {
			save::SavedPlayer sp;
			sp.id = p;
			if (p < 2) {
				VisionGrid g{48, 48, 1};
				g.stamp(0, static_cast<int>(10 + p * 20), 24, 24, +1);
				sp.explored = g.explored_text(0);
			}
			d.players.push_back(sp);
		}
		const auto text = save::serialize(d);
		save::SaveData back;
		std::string error;
		CHECK(save::parse(text, back, error), "parse failed: %s", error.c_str());
		CHECK(back.map.vision == mode, "vision mode %s lost", vision_mode_name(mode));
		CHECK(back.players.size() == 3 and back.players[0].explored == d.players[0].explored
		          and back.players[1].explored == d.players[1].explored and back.players[2].explored.empty(),
		      "explored tiles lost");
		CHECK(save::serialize(back) == text, "save text not stable");
	}
	// old saves (without the keys) load with the default vision
	save::SaveData old;
	std::string error;
	CHECK(save::parse("[save]\nversion = 1\n[map]\ntype = random\n[player]\nid = 0\n", old, error)
	          and old.map.vision == vision_mode_t::NORMAL and old.players.at(0).explored.empty(),
	      "old save: %s", error.c_str());
	save::SaveData bad;
	CHECK(not save::parse("[save]\nversion = 1\n[map]\nvision = sometimes\n", bad, error), "unknown vision accepted");
	for (auto mode : {vision_mode_t::NORMAL, vision_mode_t::EXPLORED, vision_mode_t::ALL}) {
		vision_mode_t parsed = vision_mode_t::COUNT;
		CHECK(vision_mode_parse(vision_mode_name(mode), parsed) and parsed == mode, "vision name %s", vision_mode_name(mode));
	}
}

void check_colors() {
	for (uint32_t seed = 1; seed <= 3; ++seed) {
		for (auto biome : {map_biome_t::GRASSLAND, map_biome_t::HILLS, map_biome_t::ISLANDS, map_biome_t::WINTER}) {
			MapSettings s;
			s.type = map_type_t::RANDOM;
			s.seed = seed;
			s.size = 48;
			s.biome = biome;
			auto map = generate_map(s);
			auto colors = terrain_colors(map);
			CHECK(colors.size() == map.width * map.height, "colour count");
			size_t water = 0, opaque = 0;
			for (size_t i = 0; i < colors.size(); ++i) {
				opaque += (colors[i] >> 24) == 255 ? 1 : 0;
				const bool is_water = map.tiles[i] == map_terrain_t::WATER or map.tiles[i] == map_terrain_t::WATER_MEDIUM
				                      or map.tiles[i] == map_terrain_t::WATER_DEEP;
				if (is_water) {
					const uint32_t r = colors[i] & 0xff, b = (colors[i] >> 16) & 0xff;
					water += b > r ? 1 : 0;
				}
			}
			CHECK(opaque == colors.size(), "all ground colours opaque");
			size_t water_tiles = 0;
			for (auto t : map.tiles) {
				water_tiles += (t == map_terrain_t::WATER or t == map_terrain_t::WATER_MEDIUM or t == map_terrain_t::WATER_DEEP) ? 1 : 0;
			}
			CHECK(water == water_tiles, "water is blue on the minimap (%zu of %zu)", water, water_tiles);
		}
	}
	CHECK(player_color(0) == rgba8(0, 76, 255) and player_color(1) == rgba8(255, 0, 0) and player_color(9) == player_color(1),
	      "player colours");
}

void check_speed() {
	const size_t n = 256;
	VisionGrid g{n, n, 2};
	std::mt19937 rng{3};
	std::vector<Unit> units;
	for (int i = 0; i < 200; ++i) {
		units.push_back({static_cast<size_t>(i % 2), std::uniform_real_distribution<double>{0, 256}(rng),
		                 std::uniform_real_distribution<double>{0, 256}(rng), i % 10 == 0 ? 32 : 16});
		g.stamp(units.back().player, tile_of(units.back().ne), tile_of(units.back().se), units.back().q, +1);
	}
	const auto t0 = std::chrono::steady_clock::now();
	const int ticks = 200;
	size_t restamps = 0;
	for (int t = 0; t < ticks; ++t) {
		// a tick of 0.1 s: units walk about 0.1 tiles, a few cross a tile border
		for (auto &u : units) {
			Unit m = u;
			m.ne += 0.12;
			m.se += 0.05;
			if (tile_of(m.ne) != tile_of(u.ne) or tile_of(m.se) != tile_of(u.se)) {
				g.stamp(u.player, tile_of(u.ne), tile_of(u.se), u.q, -1);
				g.stamp(m.player, tile_of(m.ne), tile_of(m.se), m.q, +1);
				restamps += 1;
			}
			u = m;
		}
		std::vector<uint8_t> states;
		g.fill_states(0, states);
	}
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / ticks;
	std::printf("speed: 200 units on 256 x 256, %.3f ms per tick (incl. tile states of one player), %.1f restamps per tick\n",
	            ms, static_cast<double>(restamps) / ticks);
	CHECK(ms < 4.0, "tick too slow: %.3f ms", ms);
}

} // namespace


int main() {
	check_disks();
	check_walks();
	check_order();
	check_footprint();
	check_classify();
	check_text();
	check_save();
	check_colors();
	check_speed();
	std::printf("vision check: %zu checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
