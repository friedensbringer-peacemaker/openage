// Copyright 2026-2026 the openage authors. See copying.md for legal info.

/*
 * Host test of the random map generator and the heightmap (XR fork).
 *
 *   openage-mapgen-check [--print <seed> <size> [biome] | --views <seed> <size> [biome]
 *                         | --hash <biome> <seeds> | --biomes [biome]]
 *
 * Parameter sweep over sizes and seeds:
 * - deterministic (same settings twice = same map), size rounded to 16
 * - no elevation on water, shallows and beach, flat start areas without water/forest
 * - slope limit between neighbouring corners, maximum elevation
 * - tree limit, all objects on land inside the map, town center + 3 villagers per
 *   player, berries/gold/stone near every start
 * - the start positions are connected for land units (8-neighbourhood over
 *   passable, unblocked tiles: shallows yes, water no, trees/mines no)
 * - water and forest shares in sensible bounds
 * Heightmap: corners exact, continuous across tiles, picking finds the visible
 * terrain point for screen positions (plane hits) of points on hills.
 *
 * Landscape presets (map_biome_t, XR fork): the same checks with the limits of
 * the preset (map_biome_limits: elevation, slope, flat start radius, tree limit,
 * water and forest shares), fish only on water and all other objects on land
 * (not on ice), the gold rush field reachable from both starts; the sweep prints
 * the object counts per preset.
 *
 * --print writes the map as characters (terrain/objects) and its summary,
 * --views camera targets for render checks (map centre, hills with trees, start),
 * --hash one FNV-1a hash per map (sizes 48..256, elevations, tree limits; compared
 * with the generator before the presets by scripts/wsl/85-biomes-check.sh),
 * --biomes only the preset sweep (all presets or one).
 * Exit code 0 if all checks pass. No dependencies besides the two sources.
 */

#include <array>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <string>

#include "gamestate/heightmap.h"
#include "gamestate/map_generator.h"

using namespace openage::gamestate;

namespace {

int failures = 0;

void check(bool ok, const std::string &what) {
	if (not ok) {
		failures += 1;
		if (failures <= 40) {
			std::cerr << "FAIL: " << what << "\n";
		}
	}
}

bool water_kind(map_terrain_t t) {
	return t == map_terrain_t::WATER or t == map_terrain_t::WATER_MEDIUM or t == map_terrain_t::WATER_DEEP;
}

size_t tree_count(const GeneratedMap &m) {
	size_t trees = 0;
	for (const auto &o : m.objects) {
		trees += map_object_is_tree(o.kind) ? 1 : 0;
	}
	return trees;
}

/// land units: everything but open water and blocked tiles (shallows and ice are passable)
bool reachable(const GeneratedMap &m, size_t first, size_t target) {
	const size_t N = m.width;
	std::vector<uint8_t> blocked(N * N, 0);
	for (auto b : m.blocked) {
		blocked[b] = 1;
	}
	auto passable = [&](size_t i) { return not water_kind(m.tiles[i]) and not blocked[i]; };
	if (not passable(first) or not passable(target)) {
		return false;
	}
	std::vector<uint8_t> seen(N * N, 0);
	std::deque<size_t> open{first};
	seen[first] = 1;
	while (not open.empty()) {
		size_t i = open.front();
		open.pop_front();
		long x = static_cast<long>(i % N);
		long y = static_cast<long>(i / N);
		for (long dy = -1; dy <= 1; ++dy) {
			for (long dx = -1; dx <= 1; ++dx) {
				long nx = x + dx;
				long ny = y + dy;
				if (nx < 0 or ny < 0 or nx >= static_cast<long>(N) or ny >= static_cast<long>(N)) {
					continue;
				}
				size_t j = static_cast<size_t>(nx) + static_cast<size_t>(ny) * N;
				if (not seen[j] and passable(j)) {
					seen[j] = 1;
					open.push_back(j);
				}
			}
		}
	}
	return seen[target];
}

/// FNV-1a over everything the game uses from the map
uint64_t map_hash(const GeneratedMap &m) {
	uint64_t h = 0xcbf29ce484222325ull;
	auto bytes = [&](const void *data, size_t size) {
		const auto *p = static_cast<const unsigned char *>(data);
		for (size_t k = 0; k < size; ++k) {
			h = (h ^ p[k]) * 0x100000001b3ull;
		}
	};
	bytes(&m.width, sizeof(m.width));
	bytes(m.tiles.data(), m.tiles.size() * sizeof(m.tiles[0]));
	bytes(m.corners.data(), m.corners.size() * sizeof(float));
	for (const auto &o : m.objects) {
		auto kind = static_cast<uint8_t>(o.kind);
		bytes(&kind, 1);
		bytes(&o.ne, sizeof(double));
		bytes(&o.se, sizeof(double));
		bytes(&o.angle, sizeof(int));
		bytes(&o.owner, sizeof(size_t));
	}
	bytes(m.blocked.data(), m.blocked.size() * sizeof(size_t));
	for (const auto &st : m.starts) {
		bytes(st.data(), 2 * sizeof(double));
	}
	uint8_t river = m.river ? 1 : 0;
	bytes(&river, 1);
	return h;
}

bool same(const GeneratedMap &a, const GeneratedMap &b) {
	if (a.tiles != b.tiles or a.corners != b.corners or a.blocked != b.blocked
	    or a.objects.size() != b.objects.size() or a.starts != b.starts) {
		return false;
	}
	for (size_t i = 0; i < a.objects.size(); ++i) {
		const auto &x = a.objects[i];
		const auto &y = b.objects[i];
		if (x.kind != y.kind or x.ne != y.ne or x.se != y.se or x.angle != y.angle or x.owner != y.owner) {
			return false;
		}
	}
	return true;
}

void check_map(const MapSettings &s, const GeneratedMap &m, const std::string &tag) {
	const size_t N = m.width;
	check(N == map_generator_size(s.size) and m.height == N and N % 16 == 0, tag + " size");
	check(m.tiles.size() == N * N and m.corners.size() == (N + 1) * (N + 1), tag + " array sizes");
	check(m.starts.size() == 2, tag + " two starts");

	const auto limits = map_biome_limits(s);
	Heightmap hm{N, N, m.corners};
	auto corner = [&](size_t ne, size_t se) { return m.corners[ne + se * (N + 1)]; };

	size_t water = 0;
	size_t forest = 0;
	float max_h = 0.0f;
	for (size_t i = 0; i < N * N; ++i) {
		size_t ne = i % N;
		size_t se = i / N;
		auto t = m.tiles[i];
		if (water_kind(t) or t == map_terrain_t::SHALLOWS or t == map_terrain_t::BEACH or t == map_terrain_t::ICE) {
			bool flat = corner(ne, se) == 0.0f and corner(ne + 1, se) == 0.0f
			            and corner(ne, se + 1) == 0.0f and corner(ne + 1, se + 1) == 0.0f;
			check(flat, tag + " elevation on water/beach tile " + std::to_string(ne) + "," + std::to_string(se));
		}
		water += water_kind(t) ? 1 : 0;
		forest += t == m.forest_floor ? 1 : 0;
	}
	for (size_t se = 0; se <= N; ++se) {
		for (size_t ne = 0; ne <= N; ++ne) {
			float h = corner(ne, se);
			max_h = std::max(max_h, h);
			check(h >= 0.0f, tag + " negative elevation");
			if (ne < N) {
				check(std::abs(h - corner(ne + 1, se)) <= limits.max_slope + 1e-3f, tag + " slope ne");
			}
			if (se < N) {
				check(std::abs(h - corner(ne, se + 1)) <= limits.max_slope + 1e-3f, tag + " slope se");
			}
		}
	}
	check(max_h <= limits.max_elevation + 1e-4f, tag + " max elevation");
	if (limits.max_elevation > 0.0f) {
		// small maps of the presets with much water or flat land: fewer hills
		float share = m.biome == map_biome_t::GRASSLAND ? 0.5f : 0.25f;
		check(max_h > share * limits.max_elevation, tag + " has hills (max " + std::to_string(max_h) + ")");
	}
	double water_share = static_cast<double>(water) / static_cast<double>(N * N);
	double forest_share = static_cast<double>(forest) / static_cast<double>(N * N);
	check(water_share >= limits.water_min and water_share < limits.water_max,
	      tag + " water share " + std::to_string(water_share));
	// presets turn forest floor without trees into open land: no share for tiny tree limits
	if (m.biome == map_biome_t::GRASSLAND or s.max_trees >= 200) {
		check(forest_share > limits.forest_min and forest_share < limits.forest_max,
		      tag + " forest share " + std::to_string(forest_share));
	}

	// start areas: flat, dry, no forest, no blocked tile except the town center
	const double start_r = limits.flat_radius;
	for (const auto &st : m.starts) {
		for (size_t i = 0; i < N * N; ++i) {
			double dx = static_cast<double>(i % N) + 0.5 - st[0];
			double dy = static_cast<double>(i / N) + 0.5 - st[1];
			double d = std::sqrt(dx * dx + dy * dy);
			if (d < start_r - 1.5) {
				auto t = m.tiles[i];
				check(not water_kind(t) and t != map_terrain_t::SHALLOWS and t != m.forest_floor
				          and t != map_terrain_t::ICE,
				      tag + " start area terrain");
				check(hm.at(static_cast<double>(i % N) + 0.5, static_cast<double>(i / N) + 0.5) == 0.0,
				      tag + " start area not flat");
			}
		}
	}

	// objects
	auto count = m.object_count();
	size_t trees = tree_count(m);
	check(trees <= limits.max_trees, tag + " tree limit");
	check(trees > 0, tag + " has trees");
	check(count[static_cast<size_t>(map_object_t::TOWN_CENTER)] == 2, tag + " 2 town centers");
	check(count[static_cast<size_t>(map_object_t::VILLAGER)] == 6, tag + " 6 villagers");
	for (const auto &o : m.objects) {
		bool inside = o.ne >= 0.0 and o.se >= 0.0 and o.ne < static_cast<double>(N) and o.se < static_cast<double>(N);
		check(inside, tag + " object outside");
		if (not inside) {
			continue;
		}
		auto t = m.tiles[static_cast<size_t>(o.ne) + static_cast<size_t>(o.se) * N];
		if (map_object_on_water(o.kind)) {
			check(water_kind(t), tag + " fish on land");
		}
		else {
			check(not water_kind(t) and t != map_terrain_t::SHALLOWS and t != map_terrain_t::ICE,
			      tag + " object in water: " + to_string(o.kind));
		}
		bool resource = o.kind != map_object_t::TOWN_CENTER and o.kind != map_object_t::VILLAGER;
		check(resource ? o.owner == 2 : o.owner < 2, tag + " owner (gaia = 2 for resources)");
	}
	for (size_t p = 0; p < 2; ++p) {
		const auto &st = m.starts[p];
		size_t near[static_cast<size_t>(map_object_t::COUNT)]{};
		for (const auto &o : m.objects) {
			double dx = o.ne - st[0];
			double dy = o.se - st[1];
			if (dx * dx + dy * dy < 13.0 * 13.0) {
				near[static_cast<size_t>(o.kind)] += 1;
			}
		}
		check(near[static_cast<size_t>(map_object_t::BERRIES)] >= 3, tag + " berries near start");
		check(near[static_cast<size_t>(map_object_t::GOLD)] >= 3, tag + " gold near start");
		check(near[static_cast<size_t>(map_object_t::STONE)] >= 3, tag + " stone near start");
	}

	// connectivity of the starts for land units (begin next to the town center, its tiles are blocked)
	auto first = static_cast<size_t>(m.starts[0][0] + 3) + static_cast<size_t>(m.starts[0][1]) * N;
	auto target = static_cast<size_t>(m.starts[1][0] + 3) + static_cast<size_t>(m.starts[1][1]) * N;
	check(reachable(m, first, target), tag + " starts connected for land units");

	if (m.biome == map_biome_t::GOLD_RUSH) {
		// large gold field in the centre, reachable from both starts
		size_t gold = 0;
		size_t goal = N * N;
		for (const auto &o : m.objects) {
			double dx = o.ne - static_cast<double>(N / 2);
			double dy = o.se - static_cast<double>(N / 2);
			if (o.kind == map_object_t::GOLD and dx * dx + dy * dy < 9.0) {
				gold += 1;
				// tile beside this mine
				for (long k = -1; k <= 1 and goal == N * N; k += 2) {
					auto x = static_cast<size_t>(o.ne + static_cast<double>(k));
					size_t j = x + static_cast<size_t>(o.se) * N;
					if (reachable(m, first, j)) {
						goal = j;
					}
				}
			}
		}
		check(gold >= 10, tag + " gold field (" + std::to_string(gold) + ")");
		check(goal < N * N and reachable(m, target, goal), tag + " gold field reachable from both starts");
	}
}

void check_heightmap() {
	// corners exact, continuity, flat map
	Heightmap flat;
	check(flat.is_flat() and flat.at(3.3, 4.4) == 0.0, "flat heightmap");
	auto p = flat.pick(5.0, 6.0);
	check(p.first.first == 5.0 and p.first.second == 6.0 and p.second == 0.0, "flat pick identity");

	MapSettings s;
	s.type = map_type_t::RANDOM;
	size_t picks = 0;
	size_t exact = 0;
	double worst_plane_error = 0.0;
	for (uint32_t seed = 1; seed <= 6; ++seed) {
		s.seed = seed;
		auto m = generate_map(s);
		Heightmap hm{m.width, m.height, m.corners};
		const size_t N = m.width;
		for (size_t se = 0; se <= N; ++se) {
			for (size_t ne = 0; ne <= N; ++ne) {
				double h = hm.at(static_cast<double>(ne), static_cast<double>(se));
				check(std::abs(h - m.corners[ne + se * (N + 1)]) < 1e-6, "heightmap corner exact");
			}
		}
		// continuity across tile edges and the diagonal
		for (double y = 0.25; y < static_cast<double>(N); y += 1.0) {
			for (double x = 1.0; x < static_cast<double>(N); x += 1.0) {
				check(std::abs(hm.at(x - 1e-9, y) - hm.at(x + 1e-9, y)) < 1e-6, "heightmap continuous (ne)");
				check(std::abs(hm.at(y, x - 1e-9) - hm.at(y, x + 1e-9)) < 1e-6, "heightmap continuous (se)");
			}
		}
		// picking: screen position of a terrain point = plane hit
		for (double y = 0.3; y < static_cast<double>(N); y += 1.7) {
			for (double x = 0.6; x < static_cast<double>(N); x += 1.3) {
				double h = hm.at(x, y);
				if (h <= 0.0) {
					continue;
				}
				double plane_ne = x - h * Heightmap::PICK_NE;
				double plane_se = y - h * Heightmap::PICK_SE;
				auto hit = hm.pick(plane_ne, plane_se);
				double ex = hit.first.first - x;
				double ey = hit.first.second - y;
				double err = std::sqrt(ex * ex + ey * ey);
				// same pixel: the hit lies on the ray, at least as close to the camera
				double t_hit = (plane_ne - hit.first.first) / -Heightmap::PICK_NE;
				check(std::abs(hit.second - t_hit) < 1e-3, "pick on ray and surface");
				check(t_hit >= h - 1e-3, "pick not behind the true point");
				picks += 1;
				exact += err < 0.02 ? 1 : 0;
				worst_plane_error = std::max(worst_plane_error, std::sqrt(2.0) * 0.4330127 * h);
			}
		}
	}
	check(picks > 100 and exact * 100 >= picks * 95, "pick exact for >= 95 % of hill points");
	std::cout << "heightmap: " << picks << " picks, " << exact << " exact (rest occluded by hills in front), "
	          << "error without correction up to " << worst_plane_error << " tiles\n";
}

char tile_char(map_terrain_t t) {
	switch (t) {
	case map_terrain_t::GRASS:
		return '.';
	case map_terrain_t::GRASS2:
		return ',';
	case map_terrain_t::GRASS3:
		return ';';
	case map_terrain_t::DIRT:
	case map_terrain_t::DIRT2:
	case map_terrain_t::DIRT3:
		return ':';
	case map_terrain_t::FOREST:
		return 'f';
	case map_terrain_t::BEACH:
		return '_';
	case map_terrain_t::SHALLOWS:
		return '=';
	case map_terrain_t::SAND:
		return '-';
	case map_terrain_t::SNOW:
	case map_terrain_t::SNOW_GRASS:
	case map_terrain_t::SNOW_DIRT:
		return '*';
	case map_terrain_t::SNOW_FOREST:
		return 'f';
	case map_terrain_t::ICE:
		return '#';
	default:
		return '~';
	}
}

void print_map(uint32_t seed, size_t size, map_biome_t biome) {
	MapSettings s;
	s.biome = biome;
	s.type = map_type_t::RANDOM;
	s.seed = seed;
	s.size = size;
	auto m = generate_map(s);
	std::vector<std::string> rows(m.height, std::string(m.width, ' '));
	for (size_t i = 0; i < m.tiles.size(); ++i) {
		rows[i / m.width][i % m.width] = tile_char(m.tiles[i]);
	}
	for (const auto &o : m.objects) {
		char c = 'T';
		switch (o.kind) {
		case map_object_t::GOLD:
			c = 'G';
			break;
		case map_object_t::STONE:
			c = 'S';
			break;
		case map_object_t::BERRIES:
			c = 'B';
			break;
		case map_object_t::TOWN_CENTER:
			c = '#';
			break;
		case map_object_t::VILLAGER:
			c = 'v';
			break;
		case map_object_t::DEER:
			c = 'd';
			break;
		case map_object_t::FISH_SHORE:
		case map_object_t::FISH_OCEAN:
			c = 'F';
			break;
		case map_object_t::CACTUS:
			c = 'c';
			break;
		default:
			break;
		}
		rows[static_cast<size_t>(o.se)][static_cast<size_t>(o.ne)] = c;
	}
	std::cout << "rows = se, columns = ne\n";
	for (const auto &r : rows) {
		std::cout << r << "\n";
	}
	std::cout << m.summary() << "\n";
}

/**
 * Camera targets for render checks: map centre and the 12x12 window with the
 * most hills and trees (ne,se of its centre).
 */
void print_views(uint32_t seed, size_t size, map_biome_t biome) {
	MapSettings s;
	s.biome = biome;
	s.type = map_type_t::RANDOM;
	s.seed = seed;
	s.size = size;
	auto m = generate_map(s);
	const size_t N = m.width;
	std::vector<double> score(N * N, 0.0);
	for (size_t i = 0; i < N * N; ++i) {
		size_t ne = i % N;
		size_t se = i / N;
		score[i] = m.corners[ne + se * (N + 1)];
	}
	for (const auto &o : m.objects) {
		if (map_object_is_tree(o.kind)) {
			score[static_cast<size_t>(o.ne) + static_cast<size_t>(o.se) * N] += 0.6;
		}
	}
	const size_t w = 12;
	double best = -1.0;
	size_t best_ne = N / 2;
	size_t best_se = N / 2;
	for (size_t se = 0; se + w <= N; se += 2) {
		for (size_t ne = 0; ne + w <= N; ne += 2) {
			double sum = 0.0;
			size_t hills = 0;
			size_t trees = 0;
			for (size_t y = se; y < se + w; ++y) {
				for (size_t x = ne; x < ne + w; ++x) {
					sum += score[x + y * N];
					hills += m.corners[x + y * (N + 1)] > 0.5f ? 1 : 0;
					trees += score[x + y * N] - m.corners[x + y * (N + 1)] > 0.1 ? 1 : 0;
				}
			}
			// both hills and trees in view
			if (hills < 10 or trees < 10) {
				continue;
			}
			if (sum > best) {
				best = sum;
				best_ne = ne + w / 2;
				best_se = se + w / 2;
			}
		}
	}
	std::cout << "overview " << N / 2 << "," << N / 2 << "\n";
	std::cout << "closeup " << best_ne << "," << best_se << "\n";
	std::cout << "start " << m.starts[0][0] << "," << m.starts[0][1] << "\n";
}

/**
 * One line per map: biome, size, seed, elevation, tree limit, hash. Compared by
 * scripts/wsl/85-biomes-check.sh for GRASSLAND with a driver around the generator
 * before the presets (the hash function is the same there).
 */
void print_hashes(map_biome_t biome, uint32_t seeds) {
	for (size_t size : {48, 64, 80, 96, 128, 256}) {
		for (uint32_t seed = 1; seed <= (size >= 128 ? std::min(seeds, 8u) : seeds); ++seed) {
			for (float elevation : {0.0f, 4.0f, 6.5f}) {
				for (size_t trees : {800, 50}) {
					MapSettings s;
					s.type = map_type_t::RANDOM;
					s.biome = biome;
					s.seed = seed;
					s.size = size;
					s.max_elevation = elevation;
					s.max_trees = trees;
					std::cout << size << " " << seed << " " << elevation << " " << trees
					          << " " << std::hex << map_hash(generate_map(s)) << std::dec << "\n";
				}
			}
		}
	}
}

/**
 * Seed sweep of one landscape preset; prints the object counts (minimum..maximum over the seeds at 64x64).
 */
size_t check_biome(map_biome_t biome) {
	size_t maps = 0;
	const std::string name = map_biome_name(biome);
	std::array<size_t, static_cast<size_t>(map_object_t::COUNT)> lo{};
	std::array<size_t, static_cast<size_t>(map_object_t::COUNT)> hi{};
	lo.fill(SIZE_MAX);
	size_t trees_max = 0;
	size_t carved_max = 0;
	size_t carved_maps = 0;
	double ms_max = 0.0;
	for (size_t size : {48, 64, 96, 128}) {
		for (uint32_t seed = 1; seed <= (size >= 128 ? 8u : 30u); ++seed) {
			MapSettings s;
			s.type = map_type_t::RANDOM;
			s.biome = biome;
			s.seed = seed;
			s.size = size;
			s.max_trees = size >= 128 ? 2000 : 800;
			auto t0 = std::chrono::steady_clock::now();
			auto m = generate_map(s);
			ms_max = std::max(ms_max, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
			std::string tag = name + " size " + std::to_string(size) + " seed " + std::to_string(seed);
			check(m.biome == biome, tag + " biome");
			check(same(m, generate_map(s)), tag + " deterministic");
			check_map(s, m, tag);
			if (size == 64) {
				auto c = m.object_count();
				for (size_t k = 0; k < c.size(); ++k) {
					lo[k] = std::min(lo[k], c[k]);
					hi[k] = std::max(hi[k], c[k]);
				}
				trees_max = std::max(trees_max, tree_count(m));
			}
			carved_max = std::max(carved_max, m.carved);
			carved_maps += m.carved > 0 ? 1 : 0;
			maps += 1;
		}
	}
	// flat variant and a small tree limit
	MapSettings flat;
	flat.biome = biome;
	flat.max_elevation = 0.0f;
	check_map(flat, generate_map(flat), name + " flat");
	MapSettings few;
	few.biome = biome;
	few.max_trees = 50;
	auto m = generate_map(few);
	check(tree_count(m) <= map_biome_limits(few).max_trees, name + " tree limit 50");
	check_map(few, m, name + " few trees");

	MapSettings one;
	one.biome = biome;
	std::cout << "biome " << name << ": " << maps << " maps, up to " << ms_max << " ms, 64x64 objects";
	for (size_t k = 0; k < lo.size(); ++k) {
		if (hi[k] > 0) {
			std::cout << " " << to_string(static_cast<map_object_t>(k)) << "=" << lo[k] << ".." << hi[k];
		}
	}
	std::cout << ", trees up to " << trees_max << " (limit " << map_biome_limits(one).max_trees
	          << "), carved connections in " << carved_maps << " maps (up to " << carved_max << " tiles)\n";
	std::cout << "  seed 1: " << generate_map(one).summary() << "\n";
	return maps;
}

} // namespace


int main(int argc, char **argv) {
	auto biome_arg = [&](int k) {
		map_biome_t biome = map_biome_t::GRASSLAND;
		if (argc > k and not map_biome_parse(argv[k], biome)) {
			std::cerr << "unknown biome " << argv[k] << "\n";
			std::exit(EXIT_FAILURE);
		}
		return biome;
	};
	if ((argc == 4 or argc == 5) and std::string{argv[1]} == "--print") {
		print_map(static_cast<uint32_t>(std::stoul(argv[2])), std::stoul(argv[3]), biome_arg(4));
		return EXIT_SUCCESS;
	}
	if ((argc == 4 or argc == 5) and std::string{argv[1]} == "--views") {
		print_views(static_cast<uint32_t>(std::stoul(argv[2])), std::stoul(argv[3]), biome_arg(4));
		return EXIT_SUCCESS;
	}
	if (argc == 4 and std::string{argv[1]} == "--hash") {
		print_hashes(biome_arg(2), static_cast<uint32_t>(std::stoul(argv[3])));
		return EXIT_SUCCESS;
	}
	if ((argc == 2 or argc == 3) and std::string{argv[1]} == "--biomes") {
		size_t maps = 0;
		if (argc == 3) {
			maps = check_biome(biome_arg(2));
		}
		else {
			for (size_t k = 1; k < static_cast<size_t>(map_biome_t::COUNT); ++k) {
				maps += check_biome(static_cast<map_biome_t>(k));
			}
		}
		std::cout << "biomes check: " << maps << " maps, " << failures << " failures\n";
		return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	check(map_generator_size(64) == 64 and map_generator_size(70) == 64 and map_generator_size(10) == 48
	          and map_generator_size(1000) == 256 and map_generator_size(100) == 96,
	      "size rounding");

	size_t maps = 0;
	for (size_t size : {48, 64, 80, 96, 128, 256}) {
		double total_ms = 0.0;
		size_t runs = 0;
		size_t trees_max = 0;
		size_t objects_max = 0;
		for (uint32_t seed = 1; seed <= (size >= 128 ? 6u : 25u); ++seed) {
			for (float elevation : {0.0f, 4.0f}) {
				MapSettings s;
				s.type = map_type_t::RANDOM;
				s.seed = seed;
				s.size = size;
				s.max_elevation = elevation;
				s.max_trees = size >= 128 ? 2000 : 800;
				auto t0 = std::chrono::steady_clock::now();
				auto m = generate_map(s);
				total_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
				runs += 1;
				std::string tag = "size " + std::to_string(size) + " seed " + std::to_string(seed)
				                  + " elevation " + std::to_string(elevation);
				check(same(m, generate_map(s)), tag + " deterministic");
				check_map(s, m, tag);
				trees_max = std::max(trees_max, tree_count(m));
				objects_max = std::max(objects_max, m.objects.size());
				maps += 1;
			}
		}
		std::cout << "size " << size << ": " << runs << " maps, " << (total_ms / static_cast<double>(runs))
		          << " ms per map, up to " << trees_max << " trees / " << objects_max << " objects\n";
	}

	// different seeds give different maps
	MapSettings a;
	a.seed = 1;
	MapSettings b;
	b.seed = 2;
	check(generate_map(a).tiles != generate_map(b).tiles, "seeds differ");

	// tree limit is respected when smaller than the forest
	MapSettings few;
	few.max_trees = 50;
	auto m = generate_map(few);
	check(tree_count(m) == 50, "tree limit 50");

	check_heightmap();

	// landscape presets
	for (size_t k = 1; k < static_cast<size_t>(map_biome_t::COUNT); ++k) {
		maps += check_biome(static_cast<map_biome_t>(k));
	}

	for (uint32_t seed : {1u, 2u, 3u}) {
		MapSettings s;
		s.seed = seed;
		std::cout << "seed " << seed << ": " << generate_map(s).summary() << "\n";
	}

	std::cout << "mapgen check: " << maps << " maps, " << failures << " failures\n";
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
