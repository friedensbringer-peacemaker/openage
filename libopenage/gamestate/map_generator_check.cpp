// Copyright 2026-2026 the openage authors. See copying.md for legal info.

/*
 * Host test of the random map generator and the heightmap (XR fork).
 *
 *   openage-mapgen-check [--print <seed> <size> | --views <seed> <size>]
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
 * --print writes the map as characters (terrain/objects) and its summary,
 * --views camera targets for render checks (map centre, hills with trees, start).
 * Exit code 0 if all checks pass. No dependencies besides the two sources.
 */

#include <chrono>
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

	Heightmap hm{N, N, m.corners};
	auto corner = [&](size_t ne, size_t se) { return m.corners[ne + se * (N + 1)]; };

	size_t water = 0;
	size_t forest = 0;
	float max_h = 0.0f;
	for (size_t i = 0; i < N * N; ++i) {
		size_t ne = i % N;
		size_t se = i / N;
		auto t = m.tiles[i];
		if (water_kind(t) or t == map_terrain_t::SHALLOWS or t == map_terrain_t::BEACH) {
			bool flat = corner(ne, se) == 0.0f and corner(ne + 1, se) == 0.0f
			            and corner(ne, se + 1) == 0.0f and corner(ne + 1, se + 1) == 0.0f;
			check(flat, tag + " elevation on water/beach tile " + std::to_string(ne) + "," + std::to_string(se));
		}
		water += water_kind(t) ? 1 : 0;
		forest += t == map_terrain_t::FOREST ? 1 : 0;
	}
	for (size_t se = 0; se <= N; ++se) {
		for (size_t ne = 0; ne <= N; ++ne) {
			float h = corner(ne, se);
			max_h = std::max(max_h, h);
			check(h >= 0.0f, tag + " negative elevation");
			if (ne < N) {
				check(std::abs(h - corner(ne + 1, se)) <= 0.75f + 1e-3f, tag + " slope ne");
			}
			if (se < N) {
				check(std::abs(h - corner(ne, se + 1)) <= 0.75f + 1e-3f, tag + " slope se");
			}
		}
	}
	check(max_h <= s.max_elevation + 1e-4f, tag + " max elevation");
	if (s.max_elevation > 0.0f) {
		check(max_h > 0.5f * s.max_elevation, tag + " has hills (max " + std::to_string(max_h) + ")");
	}
	double water_share = static_cast<double>(water) / static_cast<double>(N * N);
	double forest_share = static_cast<double>(forest) / static_cast<double>(N * N);
	check(water_share > 0.02 and water_share < 0.25, tag + " water share " + std::to_string(water_share));
	check(forest_share > 0.03 and forest_share < 0.35, tag + " forest share " + std::to_string(forest_share));

	// start areas: flat, dry, no forest, no blocked tile except the town center
	const double start_r = std::max(7.0, 0.12 * static_cast<double>(N));
	for (const auto &st : m.starts) {
		for (size_t i = 0; i < N * N; ++i) {
			double dx = static_cast<double>(i % N) + 0.5 - st[0];
			double dy = static_cast<double>(i / N) + 0.5 - st[1];
			double d = std::sqrt(dx * dx + dy * dy);
			if (d < start_r - 1.5) {
				auto t = m.tiles[i];
				check(not water_kind(t) and t != map_terrain_t::SHALLOWS and t != map_terrain_t::FOREST,
				      tag + " start area terrain");
				check(hm.at(static_cast<double>(i % N) + 0.5, static_cast<double>(i / N) + 0.5) == 0.0,
				      tag + " start area not flat");
			}
		}
	}

	// objects
	auto count = m.object_count();
	size_t trees = count[static_cast<size_t>(map_object_t::TREE_PINE)]
	               + count[static_cast<size_t>(map_object_t::TREE_JUNGLE)];
	check(trees <= s.max_trees, tag + " tree limit");
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
		check(not water_kind(t) and t != map_terrain_t::SHALLOWS, tag + " object in water: " + to_string(o.kind));
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

	// connectivity of the starts for land units
	std::vector<uint8_t> blocked(N * N, 0);
	for (auto b : m.blocked) {
		blocked[b] = 1;
	}
	auto passable = [&](size_t i) { return not water_kind(m.tiles[i]) and not blocked[i]; };
	// begin next to the town center (its tiles are blocked)
	auto first = static_cast<size_t>(m.starts[0][0] + 3) + static_cast<size_t>(m.starts[0][1]) * N;
	auto target = static_cast<size_t>(m.starts[1][0] + 3) + static_cast<size_t>(m.starts[1][1]) * N;
	check(passable(first) and passable(target), tag + " tiles next to the town centers passable");
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
	check(seen[target], tag + " starts connected for land units");
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
	default:
		return '~';
	}
}

void print_map(uint32_t seed, size_t size) {
	MapSettings s;
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
void print_views(uint32_t seed, size_t size) {
	MapSettings s;
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
		if (o.kind == map_object_t::TREE_PINE or o.kind == map_object_t::TREE_JUNGLE) {
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

} // namespace


int main(int argc, char **argv) {
	if (argc == 4 and std::string{argv[1]} == "--print") {
		print_map(static_cast<uint32_t>(std::stoul(argv[2])), std::stoul(argv[3]));
		return EXIT_SUCCESS;
	}
	if (argc == 4 and std::string{argv[1]} == "--views") {
		print_views(static_cast<uint32_t>(std::stoul(argv[2])), std::stoul(argv[3]));
		return EXIT_SUCCESS;
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
				auto c = m.object_count();
				trees_max = std::max(trees_max, c[0] + c[1]);
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
	auto c = m.object_count();
	check(c[0] + c[1] == 50, "tree limit 50");

	check_heightmap();

	for (uint32_t seed : {1u, 2u, 3u}) {
		MapSettings s;
		s.seed = seed;
		std::cout << "seed " << seed << ": " << generate_map(s).summary() << "\n";
	}

	std::cout << "mapgen check: " << maps << " maps, " << failures << " failures\n";
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
