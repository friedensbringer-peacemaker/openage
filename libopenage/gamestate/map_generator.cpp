// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "map_generator.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <queue>
#include <sstream>


namespace openage::gamestate {

namespace {

/*
 * Determinism: only integer hashing, + - * / and std::sqrt (correctly rounded
 * by IEEE 754) are used, no std:: distributions and no sin/cos/exp whose last
 * bits differ between C libraries. Same settings = same map on PC and Quest.
 */

uint64_t mix64(uint64_t x) {
	// splitmix64 finalizer
	x += 0x9e3779b97f4a7c15ull;
	x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
	x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
	return x ^ (x >> 31);
}

struct Rng {
	uint64_t state;

	uint64_t next() {
		this->state += 0x9e3779b97f4a7c15ull;
		return mix64(this->state);
	}

	/// [0, 1)
	double uniform() {
		return static_cast<double>(this->next() >> 11) * (1.0 / 9007199254740992.0);
	}

	/// [-1, 1)
	double signed_uniform() {
		return 2.0 * this->uniform() - 1.0;
	}

	/// [0, n)
	size_t below(size_t n) {
		return static_cast<size_t>(this->next() % n);
	}
};

double lattice(int64_t x, int64_t y, uint64_t seed) {
	uint64_t h = mix64(seed ^ mix64(static_cast<uint64_t>(x) * 0x632be59bd9b4e019ull
	                                + static_cast<uint64_t>(y) * 0x85157af5ull));
	return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0);
}

double smooth(double t) {
	return t * t * (3.0 - 2.0 * t);
}

double smoothstep(double edge0, double edge1, double x) {
	if (edge1 <= edge0) {
		return x < edge0 ? 0.0 : 1.0;
	}
	double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
	return smooth(t);
}

/// value noise in [0, 1], one lattice cell per unit
double value_noise(double x, double y, uint64_t seed) {
	double fx = std::floor(x);
	double fy = std::floor(y);
	auto ix = static_cast<int64_t>(fx);
	auto iy = static_cast<int64_t>(fy);
	double tx = smooth(x - fx);
	double ty = smooth(y - fy);
	double a = lattice(ix, iy, seed);
	double b = lattice(ix + 1, iy, seed);
	double c = lattice(ix, iy + 1, seed);
	double d = lattice(ix + 1, iy + 1, seed);
	double top = a + (b - a) * tx;
	double bottom = c + (d - c) * tx;
	return top + (bottom - top) * ty;
}

/// fractal value noise in [0, 1]; scale = feature size in tiles
double fbm(double x, double y, double scale, uint64_t seed, int octaves) {
	double sum = 0.0;
	double amp = 1.0;
	double norm = 0.0;
	double freq = 1.0 / scale;
	for (int i = 0; i < octaves; ++i) {
		sum += amp * value_noise(x * freq, y * freq, seed + 0x1000 * static_cast<uint64_t>(i));
		norm += amp;
		amp *= 0.5;
		freq *= 2.0;
	}
	return sum / norm;
}

/// value below which the fraction q of the values lies
double quantile(std::vector<double> values, double q) {
	if (values.empty()) {
		return 0.0;
	}
	size_t k = std::min(values.size() - 1, static_cast<size_t>(q * static_cast<double>(values.size())));
	std::nth_element(values.begin(), values.begin() + static_cast<long>(k), values.end());
	return values[k];
}

/// 16 directions (cos, sin of k * 22.5 degrees) without trig functions
constexpr double DIRS[16][2] = {
	{1.0, 0.0},
	{0.9238795325112867, 0.3826834323650898},
	{0.7071067811865476, 0.7071067811865476},
	{0.3826834323650898, 0.9238795325112867},
	{0.0, 1.0},
	{-0.3826834323650898, 0.9238795325112867},
	{-0.7071067811865476, 0.7071067811865476},
	{-0.9238795325112867, 0.3826834323650898},
	{-1.0, 0.0},
	{-0.9238795325112867, -0.3826834323650898},
	{-0.7071067811865476, -0.7071067811865476},
	{-0.3826834323650898, -0.9238795325112867},
	{0.0, -1.0},
	{0.3826834323650898, -0.9238795325112867},
	{0.7071067811865476, -0.7071067811865476},
	{0.9238795325112867, -0.3826834323650898},
};

/// multi-source distance (8-neighbourhood, steps of 1) to the tiles where source is true
std::vector<int> distance_field(size_t width, size_t height, const std::vector<uint8_t> &source) {
	const int far = static_cast<int>(width + height);
	std::vector<int> dist(width * height, far);
	std::deque<size_t> open;
	for (size_t i = 0; i < source.size(); ++i) {
		if (source[i]) {
			dist[i] = 0;
			open.push_back(i);
		}
	}
	while (not open.empty()) {
		size_t i = open.front();
		open.pop_front();
		long ne = static_cast<long>(i % width);
		long se = static_cast<long>(i / width);
		for (long dy = -1; dy <= 1; ++dy) {
			for (long dx = -1; dx <= 1; ++dx) {
				long nx = ne + dx;
				long ny = se + dy;
				if ((dx == 0 and dy == 0) or nx < 0 or ny < 0
				    or nx >= static_cast<long>(width) or ny >= static_cast<long>(height)) {
					continue;
				}
				size_t j = static_cast<size_t>(nx) + static_cast<size_t>(ny) * width;
				if (dist[j] > dist[i] + 1) {
					dist[j] = dist[i] + 1;
					open.push_back(j);
				}
			}
		}
	}
	return dist;
}

bool is_water(map_terrain_t t) {
	return t == map_terrain_t::WATER or t == map_terrain_t::WATER_MEDIUM
	       or t == map_terrain_t::WATER_DEEP or t == map_terrain_t::SHALLOWS;
}

/// tile offsets of resource clusters
struct Cluster {
	map_object_t kind;
	std::vector<std::array<int, 2>> tiles;
};

const Cluster BERRIES{map_object_t::BERRIES, {{0, 0}, {1, 0}, {0, 1}, {1, 1}, {2, 0}, {2, 1}}};
const Cluster GOLD{map_object_t::GOLD, {{0, 0}, {1, 0}, {0, 1}, {1, 1}, {2, 1}, {1, 2}, {-1, 0}}};
const Cluster STONE{map_object_t::STONE, {{0, 0}, {1, 0}, {0, 1}, {1, 1}, {2, 1}}};
const Cluster GOLD_SMALL{map_object_t::GOLD, {{0, 0}, {1, 0}, {0, 1}, {1, 1}}};
const Cluster STONE_SMALL{map_object_t::STONE, {{0, 0}, {1, 0}, {0, 1}}};
// gold rush: large field in the map centre (13 tiles)
const Cluster GOLD_FIELD{map_object_t::GOLD, {{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, -1}, {1, -1}, {-1, 1}, {2, 0}, {-2, 0}, {0, 2}, {0, -2}}};

/*
 * Landscape presets (map_biome_t). The defaults are the values of the original
 * random map (GRASSLAND): with them every step below computes exactly what the
 * generator computed before the presets existed (regression check:
 * scripts/wsl/85-biomes-check.sh compares against the old source).
 */

enum class river_mode {
	/// one river between the players with a chance of 60 % (original)
	RANDOM,
	NONE,
	/// main river between the players and two crossing rivers, all with fords
	THREE,
};

enum class sea_mode {
	NONE,
	/// sea along the top edge of the map (screen)
	COAST,
	/// elongated sea in the map centre between the players
	INLAND,
	/// small round ponds with a green ring (desert)
	OASES,
};

struct BiomeParams {
	// --- water
	/// lakes from noise: feature size, share of the map (0 = no lakes), kept away from the
	/// starts up to start_r + start_land tiles
	double lake_scale = 16.0;
	double lake_share = 0.06;
	double start_land = 8.0;
	river_mode river = river_mode::RANDOM;
	sea_mode sea = sea_mode::NONE;
	/// most lakes are frozen (ice, passable)
	bool frozen_lakes = false;

	// --- hills
	/// factor for settings.max_elevation
	float elevation_scale = 1.0f;
	double hill_scale = 13.0;
	/// raised part of the noise (quantiles); ridged noise makes hill chains
	double hill_lo = 0.60;
	double hill_hi = 0.985;
	bool ridged = false;
	/// largest height difference of neighbouring corners
	double max_step = 0.75;
	/// flat radius around the starts = start_r * flat_factor (at least 6.5), rising over flat_falloff
	double flat_factor = 1.0;
	double flat_falloff = 6.0;
	/// dirt on steep slopes
	bool slope_terrain = false;

	// --- forests and trees
	double forest_scale = 9.0;
	/// forest above this quantile of the forest noise (on candidate tiles)
	double forest_quantile = 0.78;
	double tree_density = 0.92;
	double single_tree = 0.006;
	/// tree limit = settings.max_trees * tree_percent / 100
	size_t tree_percent = 100;
	map_object_t tree = map_object_t::TREE_PINE;
	/// tree within 3 tiles of water
	map_object_t tree_water = map_object_t::TREE_JUNGLE;
	/// second tree kind mixed into the forests
	map_object_t tree_alt = map_object_t::TREE_PINE;
	double alt_share = 0.0;
	/// two winding paths through the forest between the starts
	bool forest_paths = false;
	/// central gold field in a forest ring, paths from the starts to the centre
	bool gold_rush = false;

	// --- terrain
	map_terrain_t base = map_terrain_t::GRASS;
	map_terrain_t shore = map_terrain_t::BEACH;
	map_terrain_t forest_floor = map_terrain_t::FOREST;
	/// variants on the base terrain: strongest dirt noise, strong dirt noise, high and low grass noise
	map_terrain_t v_d1 = map_terrain_t::DIRT;
	map_terrain_t v_d3 = map_terrain_t::DIRT3;
	map_terrain_t v_g2 = map_terrain_t::GRASS2;
	map_terrain_t v_g3 = map_terrain_t::GRASS3;

	// --- extras (never on GRASSLAND)
	bool deer = false;
	bool fish = false;
	bool cactus = false;

	// --- expected shares (checks)
	double water_min = 0.02;
	double water_max = 0.25;
	double forest_min = 0.03;
	double forest_max = 0.35;
};

BiomeParams biome_params(map_biome_t biome) {
	BiomeParams b;
	switch (biome) {
	case map_biome_t::GRASSLAND:
	case map_biome_t::COUNT:
		break;
	case map_biome_t::STEPPE:
		// open and dry: dry grass and dirt, little water, small groves, many low hills
		b.lake_share = 0.025;
		b.river = river_mode::NONE;
		b.hill_scale = 9.0;
		b.hill_lo = 0.40;
		b.forest_scale = 5.0;
		b.forest_quantile = 0.91;
		b.single_tree = 0.008;
		b.base = map_terrain_t::DIRT3;
		b.v_d1 = map_terrain_t::DIRT;
		b.v_d3 = map_terrain_t::DIRT;
		b.v_g2 = map_terrain_t::GRASS3;
		b.v_g3 = map_terrain_t::GRASS3;
		b.deer = true;
		b.water_min = 0.005;
		b.water_max = 0.08;
		b.forest_min = 0.03;
		b.forest_max = 0.15;
		break;
	case map_biome_t::HILLS:
		// high hill chains with steep slopes, small flat start areas
		b.lake_share = 0.035;
		b.river = river_mode::NONE;
		b.elevation_scale = 2.25f;
		b.hill_scale = 11.0;
		b.hill_lo = 0.30;
		b.hill_hi = 0.97;
		b.ridged = true;
		b.max_step = 1.0;
		b.flat_factor = 0.8;
		b.flat_falloff = 4.0;
		b.slope_terrain = true;
		b.forest_quantile = 0.80;
		b.deer = true;
		b.water_min = 0.01;
		b.water_max = 0.10;
		break;
	case map_biome_t::FOREST:
		// dense forest everywhere, clearings at the starts, two narrow paths between them
		b.lake_share = 0.03;
		b.river = river_mode::NONE;
		b.elevation_scale = 0.75f;
		b.forest_scale = 7.0;
		b.forest_quantile = 0.15;
		b.tree_density = 0.95;
		b.single_tree = 0.0;
		b.tree_percent = 150;
		b.forest_paths = true;
		b.deer = true;
		b.water_min = 0.005;
		b.water_max = 0.10;
		b.forest_min = 0.08;
		b.forest_max = 0.90;
		break;
	case map_biome_t::RIVERS:
		// three rivers with fords divide the map into six parts
		b.lake_share = 0.015;
		b.river = river_mode::THREE;
		b.forest_quantile = 0.80;
		b.fish = true;
		b.water_min = 0.05;
		b.water_max = 0.30;
		break;
	case map_biome_t::COAST:
		// sea along one side, land with forests on the rest
		b.lake_share = 0.02;
		b.river = river_mode::NONE;
		b.sea = sea_mode::COAST;
		b.tree_water = map_object_t::TREE_PALM;
		b.fish = true;
		b.water_min = 0.12;
		b.water_max = 0.40;
		break;
	case map_biome_t::INLAND_SEA:
		// large sea between the players, the land route leads around it
		b.lake_share = 0.015;
		b.river = river_mode::NONE;
		b.sea = sea_mode::INLAND;
		b.tree_water = map_object_t::TREE_PALM;
		b.fish = true;
		b.water_min = 0.12;
		b.water_max = 0.40;
		break;
	case map_biome_t::ISLANDS:
		// much water, islands; fords are carved where the starts are not connected
		b.lake_scale = 11.0;
		b.lake_share = 0.48;
		b.start_land = 12.0;
		b.river = river_mode::NONE;
		b.elevation_scale = 0.5f;
		b.forest_quantile = 0.72;
		b.tree_water = map_object_t::TREE_PALM;
		b.fish = true;
		b.water_min = 0.25;
		b.water_max = 0.60;
		b.forest_min = 0.02;
		b.forest_max = 0.30;
		break;
	case map_biome_t::GOLD_RUSH:
		// forest ring around a large gold field in the map centre
		b.lake_share = 0.03;
		b.river = river_mode::NONE;
		b.forest_quantile = 0.86;
		b.gold_rush = true;
		b.water_min = 0.005;
		b.water_max = 0.10;
		b.forest_min = 0.08;
		b.forest_max = 0.40;
		break;
	case map_biome_t::DESERT:
		// sand and dirt, oases with palms, few trees, cacti, low dunes
		b.lake_share = 0.0;
		b.river = river_mode::NONE;
		b.sea = sea_mode::OASES;
		b.elevation_scale = 0.75f;
		b.hill_scale = 9.0;
		b.hill_lo = 0.55;
		b.forest_scale = 6.0;
		b.forest_quantile = 0.95;
		b.tree_density = 0.7;
		b.single_tree = 0.0;
		b.tree = map_object_t::TREE_PALM;
		b.tree_water = map_object_t::TREE_PALM;
		b.base = map_terrain_t::SAND;
		b.forest_floor = map_terrain_t::DIRT3;
		b.v_d1 = map_terrain_t::DIRT;
		b.v_d3 = map_terrain_t::DIRT;
		b.v_g2 = map_terrain_t::SAND;
		b.v_g3 = map_terrain_t::SAND;
		b.cactus = true;
		b.water_min = 0.005;
		b.water_max = 0.08;
		b.forest_min = 0.02;
		b.forest_max = 0.20;
		break;
	case map_biome_t::WINTER:
		// snow, snowy conifers, lakes mostly frozen, deer
		b.lake_scale = 14.0;
		b.lake_share = 0.09;
		b.tree = map_object_t::TREE_SNOW;
		b.tree_water = map_object_t::TREE_SNOW;
		b.frozen_lakes = true;
		b.base = map_terrain_t::SNOW;
		b.shore = map_terrain_t::SNOW_DIRT;
		b.forest_floor = map_terrain_t::SNOW_FOREST;
		b.v_d1 = map_terrain_t::SNOW_DIRT;
		b.v_d3 = map_terrain_t::SNOW_GRASS;
		b.v_g2 = map_terrain_t::SNOW_GRASS;
		b.v_g3 = map_terrain_t::SNOW;
		b.deer = true;
		b.water_min = 0.0;
		b.water_max = 0.20;
		break;
	case map_biome_t::JUNGLE:
		// lush green, dense jungle with bamboo, many lakes
		b.lake_scale = 12.0;
		b.lake_share = 0.10;
		b.elevation_scale = 0.5f;
		b.forest_scale = 8.0;
		b.forest_quantile = 0.55;
		b.single_tree = 0.02;
		b.tree_percent = 125;
		b.tree = map_object_t::TREE_JUNGLE;
		b.tree_water = map_object_t::TREE_JUNGLE;
		b.tree_alt = map_object_t::TREE_BAMBOO;
		b.alt_share = 0.35;
		b.base = map_terrain_t::GRASS2;
		b.v_d1 = map_terrain_t::DIRT3;
		b.v_d3 = map_terrain_t::GRASS;
		b.v_g2 = map_terrain_t::GRASS;
		b.v_g3 = map_terrain_t::GRASS3;
		b.fish = true;
		b.water_min = 0.04;
		b.water_max = 0.30;
		b.forest_min = 0.08;
		b.forest_max = 0.60;
		break;
	}
	return b;
}

double start_radius(size_t N) {
	return std::max(7.0, 0.12 * static_cast<double>(N));
}

double flat_radius(const BiomeParams &b, double start_r) {
	return b.flat_factor == 1.0 ? start_r : std::max(6.5, start_r * b.flat_factor);
}

} // namespace


size_t map_generator_size(size_t requested) {
	size_t size = ((requested + 8) / 16) * 16;
	return std::clamp<size_t>(size, 48, 256);
}


bool map_object_is_tree(map_object_t object) {
	return object == map_object_t::TREE_PINE or object == map_object_t::TREE_JUNGLE
	       or object == map_object_t::TREE_PALM or object == map_object_t::TREE_SNOW
	       or object == map_object_t::TREE_BAMBOO;
}

bool map_object_on_water(map_object_t object) {
	return object == map_object_t::FISH_SHORE or object == map_object_t::FISH_OCEAN;
}

MapBiomeLimits map_biome_limits(const MapSettings &settings) {
	const auto b = biome_params(settings.biome);
	const size_t N = map_generator_size(settings.size);
	MapBiomeLimits limits;
	limits.max_elevation = settings.max_elevation * b.elevation_scale;
	limits.max_slope = static_cast<float>(b.max_step);
	limits.flat_radius = flat_radius(b, start_radius(N));
	limits.max_trees = b.tree_percent == 100 ? settings.max_trees : settings.max_trees * b.tree_percent / 100;
	limits.water_min = b.water_min;
	limits.water_max = b.water_max;
	limits.forest_min = b.forest_min;
	limits.forest_max = b.forest_max;
	return limits;
}


GeneratedMap generate_map(const MapSettings &settings) {
	const BiomeParams b = biome_params(settings.biome);
	const bool original = settings.biome == map_biome_t::GRASSLAND;
	const size_t N = map_generator_size(settings.size);
	const auto n = static_cast<double>(N);
	const uint64_t seed = mix64(0x6f70656e61676500ull ^ settings.seed);
	Rng rng{seed};

	GeneratedMap map;
	map.width = N;
	map.height = N;
	map.biome = settings.biome;
	map.forest_floor = b.forest_floor;
	const size_t tile_count = N * N;
	const size_t corner_count = (N + 1) * (N + 1);
	auto tile_index = [&](long ne, long se) {
		return static_cast<size_t>(ne) + static_cast<size_t>(se) * N;
	};
	auto in_map = [&](long ne, long se) {
		return ne >= 0 and se >= 0 and ne < static_cast<long>(N) and se < static_cast<long>(N);
	};

	// --- start positions: left and right on screen (opposite ends of the ne = -se diagonal)
	const double jitter = 0.04 * n;
	for (auto base : {std::array<double, 2>{0.27 * n, 0.73 * n}, std::array<double, 2>{0.73 * n, 0.27 * n}}) {
		double ne = std::floor(base[0] + jitter * rng.signed_uniform() + 0.5);
		double se = std::floor(base[1] + jitter * rng.signed_uniform() + 0.5);
		map.starts.push_back({ne, se});
	}
	// neutral owner of trees and resources (gaia): the player after the last start
	const size_t gaia = map.starts.size();
	const double start_r = start_radius(N);
	auto start_dist = [&](double ne, double se) {
		double best = 1e9;
		for (const auto &s : map.starts) {
			double dx = ne - s[0];
			double dy = se - s[1];
			best = std::min(best, std::sqrt(dx * dx + dy * dy));
		}
		return best;
	};
	auto centre_dist = [&](double ne, double se) {
		double dx = ne - 0.5 * n;
		double dy = se - 0.5 * n;
		return std::sqrt(dx * dx + dy * dy);
	};
	// gold rush: dry centre (gold field) with room for the forest ring
	const double centre_r = 0.2 * n;

	// --- water: lakes from noise, kept away from the start areas
	std::vector<uint8_t> water(tile_count, 0);
	std::vector<uint8_t> ford(tile_count, 0);
	std::vector<uint8_t> frozen(tile_count, 0);
	if (b.lake_share > 0.0) {
		std::vector<double> lake(tile_count);
		for (size_t i = 0; i < tile_count; ++i) {
			double x = static_cast<double>(i % N) + 0.5;
			double y = static_cast<double>(i / N) + 0.5;
			double v = fbm(x, y, b.lake_scale, seed + 1, 3);
			double d = start_dist(x, y);
			if (d < start_r + b.start_land) {
				v += (start_r + b.start_land - d) * 0.1;
			}
			if (b.gold_rush) {
				double dc = centre_dist(x, y);
				if (dc < centre_r + 3.0) {
					v += (centre_r + 3.0 - dc) * 0.1;
				}
			}
			lake[i] = v;
		}
		double threshold = quantile(lake, b.lake_share);
		for (size_t i = 0; i < tile_count; ++i) {
			if (lake[i] < threshold) {
				water[i] = 1;
			}
		}

		// no puddles: drop lakes smaller than 10 tiles (4-neighbourhood)
		std::vector<uint8_t> seen(tile_count, 0);
		for (size_t first = 0; first < tile_count; ++first) {
			if (not water[first] or seen[first]) {
				continue;
			}
			std::vector<size_t> component{first};
			seen[first] = 1;
			for (size_t k = 0; k < component.size(); ++k) {
				long x = static_cast<long>(component[k] % N);
				long y = static_cast<long>(component[k] / N);
				const long nb[4][2] = {{x + 1, y}, {x - 1, y}, {x, y + 1}, {x, y - 1}};
				for (const auto &p : nb) {
					if (in_map(p[0], p[1])) {
						size_t j = tile_index(p[0], p[1]);
						if (water[j] and not seen[j]) {
							seen[j] = 1;
							component.push_back(j);
						}
					}
				}
			}
			if (component.size() < 10) {
				for (auto i : component) {
					water[i] = 0;
				}
			}
			else if (b.frozen_lakes and lattice(static_cast<int64_t>(first), 17, seed + 8) < 0.7) {
				// winter: most lakes are frozen
				for (auto i : component) {
					frozen[i] = 1;
				}
			}
		}
	}

	// --- seas and oases (presets)
	std::vector<uint8_t> oasis(tile_count, 0);
	if (b.sea == sea_mode::COAST or b.sea == sea_mode::INLAND) {
		for (size_t i = 0; i < tile_count; ++i) {
			double x = static_cast<double>(i % N) + 0.5;
			double y = static_cast<double>(i / N) + 0.5;
			if (start_dist(x, y) < start_r + 4.0) {
				continue;
			}
			double wobble = (fbm(x, y, 12.0, seed + 11, 3) - 0.5) * 2.0;
			bool sea = false;
			if (b.sea == sea_mode::COAST) {
				// top edge on screen: small ne + se (the starts lie at ne + se = n)
				sea = (x + y) / (2.0 * n) + 0.08 * wobble < 0.34;
			}
			else {
				// ellipse along the screen vertical (1, 1), narrow towards the players (1, -1)
				const double r = 0.7071067811865476;
				double u = ((x - 0.5 * n) + (y - 0.5 * n)) * r / (0.40 * n);
				double v = ((x - 0.5 * n) - (y - 0.5 * n)) * r / (0.16 * n);
				sea = u * u + v * v < 1.0 + 0.45 * wobble;
			}
			if (sea) {
				water[i] = 1;
			}
		}
	}
	else if (b.sea == sea_mode::OASES) {
		// one oasis near each start (away from the town), more in the open desert
		const size_t count = std::max<size_t>(4, N / 12);
		for (size_t k = 0; k < count; ++k) {
			for (int attempt = 0; attempt < 64; ++attempt) {
				double cx;
				double cy;
				if (k < map.starts.size()) {
					const auto &d = DIRS[rng.below(16)];
					double dist = start_r + 5.0 + 3.0 * rng.uniform();
					cx = map.starts[k][0] + d[0] * dist;
					cy = map.starts[k][1] + d[1] * dist;
				}
				else {
					cx = 4.0 + rng.uniform() * (n - 8.0);
					cy = 4.0 + rng.uniform() * (n - 8.0);
				}
				double radius = 1.8 + 1.2 * rng.uniform();
				if (cx < radius + 4.0 or cy < radius + 4.0 or cx > n - radius - 4.0 or cy > n - radius - 4.0
				    or start_dist(cx, cy) < start_r + radius + 2.0) {
					continue;
				}
				for (long y = static_cast<long>(cy - radius - 4.0); y <= static_cast<long>(cy + radius + 4.0); ++y) {
					for (long x = static_cast<long>(cx - radius - 4.0); x <= static_cast<long>(cx + radius + 4.0); ++x) {
						if (not in_map(x, y)) {
							continue;
						}
						double dx = static_cast<double>(x) + 0.5 - cx;
						double dy = static_cast<double>(y) + 0.5 - cy;
						double d2 = dx * dx + dy * dy;
						size_t i = tile_index(x, y);
						if (d2 <= radius * radius) {
							water[i] = 1;
						}
						else if (d2 <= (radius + 3.5) * (radius + 3.5)
						         and start_dist(static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5) > start_r + 1.5) {
							oasis[i] = 1;
						}
					}
				}
				break;
			}
		}
	}

	// --- river between the players (perpendicular to their axis), with two fords
	// axis (ux, uy), normal (vx, vy); centre line c + s * axis + (offset + noise) * normal
	struct River {
		double ux, uy, vx, vy;
		double offset;
		double amp;
		uint64_t noise;
		std::array<double, 2> fords;
	};
	auto draw_river = [&](const River &rv) {
		const double c = 0.5 * n;
		auto centre = [&](double s) {
			double off = rv.offset + (fbm(s, 0.5, 14.0, seed + rv.noise, 3) - 0.5) * 2.0 * rv.amp;
			return std::array<double, 2>{c + s * rv.ux + off * rv.vx, c + s * rv.uy + off * rv.vy};
		};
		for (double s = -0.75 * n; s <= 0.75 * n; s += 0.5) {
			auto p = centre(s);
			double radius = 1.1 + 0.9 * fbm(s, 7.5, 10.0, seed + rv.noise + 1, 2);
			for (long y = static_cast<long>(p[1] - radius - 1); y <= static_cast<long>(p[1] + radius + 1); ++y) {
				for (long x = static_cast<long>(p[0] - radius - 1); x <= static_cast<long>(p[0] + radius + 1); ++x) {
					if (not in_map(x, y)) {
						continue;
					}
					double dx = static_cast<double>(x) + 0.5 - p[0];
					double dy = static_cast<double>(y) + 0.5 - p[1];
					if (dx * dx + dy * dy <= radius * radius
					    and start_dist(static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5) > start_r + 3.0) {
						water[tile_index(x, y)] = 1;
					}
				}
			}
		}
		for (double s : rv.fords) {
			auto p = centre(s);
			for (long y = static_cast<long>(p[1]) - 4; y <= static_cast<long>(p[1]) + 4; ++y) {
				for (long x = static_cast<long>(p[0]) - 4; x <= static_cast<long>(p[0]) + 4; ++x) {
					if (not in_map(x, y)) {
						continue;
					}
					double dx = static_cast<double>(x) + 0.5 - p[0];
					double dy = static_cast<double>(y) + 0.5 - p[1];
					size_t i = tile_index(x, y);
					if (water[i] and dx * dx + dy * dy <= 2.6 * 2.6) {
						ford[i] = 1;
					}
				}
			}
		}
	};
	const double r = 0.7071067811865476;
	if (b.river == river_mode::RANDOM) {
		map.river = rng.uniform() < 0.6;
		if (map.river) {
			draw_river({r, r, r, -r, 0.0, 0.12 * n, 2, {-0.22 * n, 0.22 * n}});
		}
	}
	else if (b.river == river_mode::THREE) {
		map.river = true;
		// main river between the players, two rivers across it (top and bottom of the screen)
		draw_river({r, r, r, -r, 0.0, 0.10 * n, 2, {-0.22 * n, 0.22 * n}});
		draw_river({r, -r, r, r, -0.30 * n, 0.05 * n, 20, {-0.25 * n, 0.25 * n}});
		draw_river({r, -r, r, r, 0.30 * n, 0.05 * n, 22, {-0.18 * n, 0.30 * n}});
	}
	for (size_t i = 0; i < tile_count; ++i) {
		if (ford[i]) {
			frozen[i] = 0;
		}
		if (not water[i]) {
			frozen[i] = 0;
		}
	}

	// distances: land tiles to water (fords count as water), water tiles to land (fords count as land)
	auto dist_water = distance_field(N, N, water);
	std::vector<uint8_t> land_or_ford(tile_count);
	for (size_t i = 0; i < tile_count; ++i) {
		land_or_ford[i] = (not water[i]) or ford[i];
	}
	auto dist_land = distance_field(N, N, land_or_ford);

	// winter: fords are frozen, too
	const map_terrain_t ford_kind = b.frozen_lakes ? map_terrain_t::ICE : map_terrain_t::SHALLOWS;
	map.tiles.assign(tile_count, b.base);
	for (size_t i = 0; i < tile_count; ++i) {
		if (ford[i]) {
			map.tiles[i] = ford_kind;
		}
		else if (frozen[i]) {
			map.tiles[i] = map_terrain_t::ICE;
		}
		else if (water[i]) {
			int d = dist_land[i];
			map.tiles[i] = d <= 1 ? map_terrain_t::WATER
			             : d <= 3 ? map_terrain_t::WATER_MEDIUM
			                      : map_terrain_t::WATER_DEEP;
		}
		else if (dist_water[i] <= 1) {
			map.tiles[i] = b.shore;
		}
	}

	// --- hills: smooth noise, only the upper part raised; flat at water, shore and start areas
	const float max_elevation = settings.max_elevation * b.elevation_scale;
	const double flat_r = flat_radius(b, start_r);
	map.corners.assign(corner_count, 0.0f);
	if (max_elevation > 0.0f) {
		std::vector<double> hill(corner_count);
		for (size_t i = 0; i < corner_count; ++i) {
			double x = static_cast<double>(i % (N + 1));
			double y = static_cast<double>(i / (N + 1));
			hill[i] = fbm(x, y, b.hill_scale, seed + 4, 3);
			if (b.ridged) {
				// hill chains along the middle values of the noise
				hill[i] = 1.0 - std::abs(2.0 * hill[i] - 1.0);
			}
		}
		const double lo = quantile(hill, b.hill_lo);
		const double hi = quantile(hill, b.hill_hi);
		std::vector<double> h(corner_count);
		for (size_t i = 0; i < corner_count; ++i) {
			long x = static_cast<long>(i % (N + 1));
			long y = static_cast<long>(i / (N + 1));
			// smallest water distance of the (up to) 4 tiles at this corner
			int wd = static_cast<int>(2 * N);
			for (long ty = y - 1; ty <= y; ++ty) {
				for (long tx = x - 1; tx <= x; ++tx) {
					if (in_map(tx, ty)) {
						wd = std::min(wd, dist_water[tile_index(tx, ty)]);
					}
				}
			}
			double v = smoothstep(lo, hi, hill[i]) * max_elevation;
			v *= smoothstep(1.0, 4.0, static_cast<double>(wd));
			v *= smoothstep(flat_r, flat_r + b.flat_falloff, start_dist(static_cast<double>(x), static_cast<double>(y)));
			if (b.gold_rush) {
				v *= smoothstep(4.0, 8.0, centre_dist(static_cast<double>(x), static_cast<double>(y)));
			}
			h[i] = v;
		}
		// limit slopes: neighbouring corners differ by at most max_step
		const double max_step = b.max_step;
		for (int pass = 0; pass < 16; ++pass) {
			bool changed = false;
			auto relax = [&](size_t i, size_t j) {
				if (h[i] > h[j] + max_step) {
					h[i] = h[j] + max_step;
					changed = true;
				}
			};
			for (size_t y = 0; y <= N; ++y) {
				for (size_t x = 0; x <= N; ++x) {
					size_t i = x + y * (N + 1);
					if (x > 0) {
						relax(i, i - 1);
					}
					if (y > 0) {
						relax(i, i - (N + 1));
					}
				}
			}
			for (size_t y = N + 1; y-- > 0;) {
				for (size_t x = N + 1; x-- > 0;) {
					size_t i = x + y * (N + 1);
					if (x < N) {
						relax(i, i + 1);
					}
					if (y < N) {
						relax(i, i + (N + 1));
					}
				}
			}
			if (not changed) {
				break;
			}
		}
		for (size_t i = 0; i < corner_count; ++i) {
			// quantised to 1/256 (exact in float and in the 16-bit fixed point elevation)
			map.corners[i] = static_cast<float>(std::floor(h[i] * 256.0 + 0.5) / 256.0);
		}
	}

	// --- objects: start areas and resources
	std::vector<uint8_t> blocked(tile_count, 0);
	std::vector<uint8_t> reserved(tile_count, 0); // no forest
	for (size_t i = 0; i < tile_count; ++i) {
		double x = static_cast<double>(i % N) + 0.5;
		double y = static_cast<double>(i / N) + 0.5;
		if (start_dist(x, y) < start_r + 1.5) {
			reserved[i] = 1;
		}
	}
	auto reserve_around = [&](long ne, long se, long margin) {
		for (long y = se - margin; y <= se + margin; ++y) {
			for (long x = ne - margin; x <= ne + margin; ++x) {
				if (in_map(x, y)) {
					reserved[tile_index(x, y)] = 1;
				}
			}
		}
	};
	auto free_land = [&](long ne, long se) {
		if (not in_map(ne, se)) {
			return false;
		}
		size_t i = tile_index(ne, se);
		return not is_water(map.tiles[i]) and map.tiles[i] != map_terrain_t::ICE and not blocked[i];
	};
	auto place_cluster = [&](const Cluster &cluster, long ne, long se) {
		size_t placed = 0;
		for (const auto &off : cluster.tiles) {
			long x = ne + off[0];
			long y = se + off[1];
			if (not free_land(x, y)) {
				continue;
			}
			size_t i = tile_index(x, y);
			blocked[i] = 1;
			reserve_around(x, y, 1);
			map.objects.push_back({cluster.kind, static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5, 0, gaia});
			placed += 1;
		}
		return placed;
	};

	for (size_t player = 0; player < map.starts.size(); ++player) {
		const auto &s = map.starts[player];
		const auto sne = static_cast<long>(s[0]);
		const auto sse = static_cast<long>(s[1]);

		// town center: 4x4 tiles around the start corner
		map.objects.push_back({map_object_t::TOWN_CENTER, s[0], s[1], 0, player});
		for (long y = sse - 2; y < sse + 2; ++y) {
			for (long x = sne - 2; x < sne + 2; ++x) {
				if (in_map(x, y)) {
					blocked[tile_index(x, y)] = 1;
				}
			}
		}

		// villagers in front of the town center, towards the map centre
		size_t to_centre = 0;
		double best = -2.0;
		for (size_t k = 0; k < 16; ++k) {
			double dx = 0.5 * n - s[0];
			double dy = 0.5 * n - s[1];
			double dot = DIRS[k][0] * dx + DIRS[k][1] * dy;
			if (dot > best) {
				best = dot;
				to_centre = k;
			}
		}
		const auto &dir = DIRS[to_centre];
		for (int k = -1; k <= 1; ++k) {
			double ne = s[0] + dir[0] * 3.4 - dir[1] * 0.9 * k;
			double se = s[1] + dir[1] * 3.4 + dir[0] * 0.9 * k;
			map.objects.push_back({map_object_t::VILLAGER, ne, se, 0, player});
		}

		// berries, gold and stone in different directions
		size_t a0 = rng.below(16);
		struct Spot {
			const Cluster *cluster;
			size_t dir;
			double dist;
		};
		for (const auto &spot : {Spot{&BERRIES, a0, 6.5}, Spot{&GOLD, (a0 + 5) % 16, 8.5}, Spot{&STONE, (a0 + 10) % 16, 9.5}}) {
			const auto &d = DIRS[spot.dir];
			auto ne = static_cast<long>(std::floor(s[0] + d[0] * spot.dist)) - 1;
			auto se = static_cast<long>(std::floor(s[1] + d[1] * spot.dist)) - 1;
			place_cluster(*spot.cluster, ne, se);
		}
	}

	// extra gold and stone away from the start areas
	auto place_far = [&](const Cluster &cluster) {
		for (int attempt = 0; attempt < 64; ++attempt) {
			auto ne = static_cast<long>(rng.below(N - 6)) + 3;
			auto se = static_cast<long>(rng.below(N - 6)) + 3;
			size_t i = tile_index(ne, se);
			double x = static_cast<double>(ne) + 0.5;
			double y = static_cast<double>(se) + 0.5;
			if (start_dist(x, y) < start_r + 7.0 or dist_water[i] < 3 or blocked[i]) {
				continue;
			}
			if (place_cluster(cluster, ne, se) > 0) {
				return;
			}
		}
	};
	for (size_t k = 0; k < std::max<size_t>(1, N / 32); ++k) {
		place_far(GOLD_SMALL);
	}
	for (size_t k = 0; k < std::max<size_t>(1, N / 64); ++k) {
		place_far(STONE_SMALL);
	}

	// gold rush: large gold field in the centre with stone beside it
	const auto centre_tile = static_cast<long>(N / 2);
	if (b.gold_rush) {
		place_cluster(GOLD_FIELD, centre_tile, centre_tile);
		place_cluster(STONE_SMALL, centre_tile + 3, centre_tile - 4);
		place_cluster(STONE_SMALL, centre_tile - 4, centre_tile + 3);
	}

	// --- designed paths (no forest): winding lines between two points, width about 2 tiles
	std::vector<uint8_t> path(tile_count, 0);
	auto carve_path = [&](std::array<double, 2> a, std::array<double, 2> z, double amp, uint64_t noise) {
		double dx = z[0] - a[0];
		double dy = z[1] - a[1];
		double len = std::sqrt(dx * dx + dy * dy);
		if (len < 1.0) {
			return;
		}
		double px = -dy / len;
		double py = dx / len;
		for (double t = 0.0; t <= 1.0; t += 0.25 / len) {
			// bend in the middle, ends fixed
			double bend = 4.0 * t * (1.0 - t) * amp * (fbm(t * len, 3.5, 9.0, seed + noise, 2) - 0.5) * 2.0;
			double cx = a[0] + t * dx + bend * px;
			double cy = a[1] + t * dy + bend * py;
			for (long y = static_cast<long>(cy) - 1; y <= static_cast<long>(cy) + 1; ++y) {
				for (long x = static_cast<long>(cx) - 1; x <= static_cast<long>(cx) + 1; ++x) {
					double ex = static_cast<double>(x) + 0.5 - cx;
					double ey = static_cast<double>(y) + 0.5 - cy;
					if (in_map(x, y) and ex * ex + ey * ey <= 1.0) {
						path[tile_index(x, y)] = 1;
						reserved[tile_index(x, y)] = 1;
					}
				}
			}
		}
	};
	if (b.forest_paths) {
		carve_path(map.starts[0], map.starts[1], 0.25 * n, 30);
		carve_path(map.starts[0], map.starts[1], -0.25 * n, 31);
	}
	if (b.gold_rush) {
		const std::array<double, 2> c{0.5 * n, 0.5 * n};
		carve_path(map.starts[0], c, 0.15 * n, 32);
		carve_path(map.starts[1], c, 0.15 * n, 33);
	}

	// --- forests
	{
		std::vector<double> forest(tile_count);
		std::vector<double> candidates;
		auto candidate = [&](size_t i) {
			return map.tiles[i] != b.shore and not is_water(map.tiles[i]) and map.tiles[i] != map_terrain_t::ICE
			       and not reserved[i] and not blocked[i];
		};
		for (size_t i = 0; i < tile_count; ++i) {
			double x = static_cast<double>(i % N) + 0.5;
			double y = static_cast<double>(i / N) + 0.5;
			forest[i] = fbm(x, y, b.forest_scale, seed + 5, 3);
			if (candidate(i)) {
				candidates.push_back(forest[i]);
			}
		}
		double threshold = quantile(candidates, b.forest_quantile);
		for (size_t i = 0; i < tile_count; ++i) {
			if (candidate(i) and forest[i] > threshold) {
				map.tiles[i] = b.forest_floor;
			}
		}
		if (b.gold_rush) {
			// forest ring around the gold field
			for (size_t i = 0; i < tile_count; ++i) {
				double dc = centre_dist(static_cast<double>(i % N) + 0.5, static_cast<double>(i / N) + 0.5);
				if (candidate(i) and dc > 5.0 and dc < centre_r * (0.8 + 0.3 * forest[i])) {
					map.tiles[i] = b.forest_floor;
				}
			}
		}
		if (b.sea == sea_mode::OASES) {
			// green ring around the oases (palms, see trees)
			for (size_t i = 0; i < tile_count; ++i) {
				if (oasis[i] and candidate(i)) {
					map.tiles[i] = b.forest_floor;
				}
			}
		}

		// every player gets wood nearby (AoE: a woodline next to the town)
		for (const auto &s : map.starts) {
			size_t near = 0;
			for (size_t i = 0; i < tile_count; ++i) {
				double d = start_dist(static_cast<double>(i % N) + 0.5, static_cast<double>(i / N) + 0.5);
				if (map.tiles[i] == b.forest_floor and d < start_r + 9.0) {
					near += 1;
				}
			}
			if (near >= 14) {
				continue;
			}
			// grow a wood away from the map centre
			double dx = s[0] - 0.5 * n;
			double dy = s[1] - 0.5 * n;
			double len = std::sqrt(dx * dx + dy * dy);
			if (len < 1e-6) {
				dx = 1.0;
				dy = 0.0;
				len = 1.0;
			}
			double cx = s[0] + dx / len * (start_r + 5.0);
			double cy = s[1] + dy / len * (start_r + 5.0);
			for (long y = static_cast<long>(cy) - 5; y <= static_cast<long>(cy) + 5; ++y) {
				for (long x = static_cast<long>(cx) - 5; x <= static_cast<long>(cx) + 5; ++x) {
					if (not in_map(x, y)) {
						continue;
					}
					double ex = static_cast<double>(x) + 0.5 - cx;
					double ey = static_cast<double>(y) + 0.5 - cy;
					size_t i = tile_index(x, y);
					if (ex * ex + ey * ey <= 3.6 * 3.6 and candidate(i)) {
						map.tiles[i] = b.forest_floor;
					}
				}
			}
		}
	}

	// --- grass and dirt variants on the remaining land
	{
		std::vector<double> g(tile_count);
		std::vector<double> d(tile_count);
		std::vector<double> land_g;
		std::vector<double> land_d;
		for (size_t i = 0; i < tile_count; ++i) {
			double x = static_cast<double>(i % N) + 0.5;
			double y = static_cast<double>(i / N) + 0.5;
			g[i] = fbm(x, y, 8.0, seed + 6, 2);
			d[i] = fbm(x, y, 6.0, seed + 7, 2);
			if (map.tiles[i] == b.base) {
				land_g.push_back(g[i]);
				land_d.push_back(d[i]);
			}
		}
		double g2 = quantile(land_g, 0.80);
		double g3 = quantile(land_g, 0.10);
		double d3 = quantile(land_d, 0.93);
		double d1 = quantile(land_d, 0.985);
		for (size_t i = 0; i < tile_count; ++i) {
			if (map.tiles[i] != b.base) {
				continue;
			}
			if (d[i] > d1) {
				map.tiles[i] = b.v_d1;
			}
			else if (d[i] > d3) {
				map.tiles[i] = b.v_d3;
			}
			else if (g[i] > g2) {
				map.tiles[i] = b.v_g2;
			}
			else if (g[i] < g3) {
				map.tiles[i] = b.v_g3;
			}
		}
	}

	// hills: bare dirt on steep slopes (not under forests)
	if (b.slope_terrain) {
		for (size_t i = 0; i < tile_count; ++i) {
			auto t = map.tiles[i];
			if (t == b.forest_floor or t == b.shore or is_water(t) or t == map_terrain_t::ICE) {
				continue;
			}
			size_t ne = i % N;
			size_t se = i / N;
			float c[4] = {map.corners[ne + se * (N + 1)], map.corners[ne + 1 + se * (N + 1)],
			              map.corners[ne + (se + 1) * (N + 1)], map.corners[ne + 1 + (se + 1) * (N + 1)]};
			float steep = *std::max_element(c, c + 4) - *std::min_element(c, c + 4);
			if (steep > 1.6f) {
				map.tiles[i] = map_terrain_t::DIRT;
			}
			else if (steep > 1.15f) {
				map.tiles[i] = map_terrain_t::DIRT3;
			}
		}
	}

	// tiles with a removable object (tree, cactus): a carved connection may clear them
	std::vector<uint8_t> removable(tile_count, 0);

	// --- trees: one per forest tile (density < 1 for gaps), a few single trees on grass
	{
		std::vector<MapObject> trees;
		for (size_t i = 0; i < tile_count; ++i) {
			const auto t = map.tiles[i];
			double x = static_cast<double>(i % N) + 0.5;
			double y = static_cast<double>(i / N) + 0.5;
			bool tree = false;
			if (t == b.forest_floor) {
				tree = rng.uniform() < (oasis[i] ? 0.45 : b.tree_density);
			}
			else if ((t == b.base or t == b.v_g2) and not reserved[i] and not blocked[i]) {
				tree = rng.uniform() < b.single_tree;
			}
			if (not tree) {
				continue;
			}
			double jx = 0.12 * rng.signed_uniform();
			double jy = 0.12 * rng.signed_uniform();
			int angle = 40 * static_cast<int>(rng.below(9));
			// pine forests; broadleaf/palm trees ("jungle") only close to the water
			auto kind = dist_water[i] <= 3 ? b.tree_water : b.tree;
			if (b.alt_share > 0.0 and lattice(static_cast<int64_t>(i % N) / 3, static_cast<int64_t>(i / N) / 3, seed + 9) < b.alt_share) {
				kind = b.tree_alt;
			}
			trees.push_back({kind, x + jx, y + jy, angle, gaia});
		}
		const size_t tree_limit = map_biome_limits(settings).max_trees;
		if (trees.size() > tree_limit and not original) {
			// presets: keep the dense parts of the forests (low frequency noise), glades elsewhere
			auto density = [&](const MapObject &t) {
				return fbm(t.ne, t.se, 6.0, seed + 12, 2);
			};
			std::stable_sort(trees.begin(), trees.end(), [&](const MapObject &a, const MapObject &b) {
				return density(a) > density(b);
			});
			// forest floor without its tree becomes open land again (glade)
			for (size_t k = tree_limit; k < trees.size(); ++k) {
				size_t i = tile_index(static_cast<long>(trees[k].ne), static_cast<long>(trees[k].se));
				if (map.tiles[i] == b.forest_floor and not oasis[i]) {
					map.tiles[i] = b.base;
				}
			}
			trees.resize(tree_limit);
			std::sort(trees.begin(), trees.end(), [](const MapObject &a, const MapObject &b) {
				return a.se < b.se or (a.se == b.se and a.ne < b.ne);
			});
		}
		else if (trees.size() > tree_limit) {
			// deterministic thinning, the forest floor stays
			for (size_t k = trees.size() - 1; k > 0; --k) {
				std::swap(trees[k], trees[rng.below(k + 1)]);
			}
			trees.resize(tree_limit);
			std::sort(trees.begin(), trees.end(), [](const MapObject &a, const MapObject &b) {
				return a.se < b.se or (a.se == b.se and a.ne < b.ne);
			});
		}
		for (const auto &tree : trees) {
			size_t i = tile_index(static_cast<long>(tree.ne), static_cast<long>(tree.se));
			blocked[i] = 1;
			removable[i] = 1;
			map.objects.push_back(tree);
		}
	}

	if (not original) {
		// --- extras of the presets: cacti, deer, fish
		if (b.cactus) {
			size_t cacti = 0;
			for (size_t i = 0; i < tile_count and cacti < N; ++i) {
				auto t = map.tiles[i];
				if ((t == b.base or t == b.v_d1) and not reserved[i] and not blocked[i] and rng.uniform() < 0.012) {
					double x = static_cast<double>(i % N) + 0.5 + 0.15 * rng.signed_uniform();
					double y = static_cast<double>(i / N) + 0.5 + 0.15 * rng.signed_uniform();
					map.objects.push_back({map_object_t::CACTUS, x, y, 40 * static_cast<int>(rng.below(9)), gaia});
					blocked[i] = 1;
					removable[i] = 1;
					cacti += 1;
				}
			}
		}
		if (b.deer) {
			// a herd of three near each start, more herds in the open land
			auto herd = [&](double cx, double cy) {
				size_t placed = 0;
				for (const auto &off : {std::array<double, 2>{0.0, 0.0}, std::array<double, 2>{1.1, 0.4}, std::array<double, 2>{0.3, 1.2}}) {
					double x = cx + off[0];
					double y = cy + off[1];
					if (x < 1.0 or y < 1.0 or x > n - 1.0 or y > n - 1.0) {
						continue;
					}
					auto tx = static_cast<long>(x);
					auto ty = static_cast<long>(y);
					if (free_land(tx, ty) and not reserved[tile_index(tx, ty)]) {
						map.objects.push_back({map_object_t::DEER, x, y, 40 * static_cast<int>(rng.below(9)), gaia});
						placed += 1;
					}
				}
				return placed;
			};
			for (const auto &s : map.starts) {
				for (int attempt = 0; attempt < 32; ++attempt) {
					const auto &d = DIRS[rng.below(16)];
					double dist = start_r + 3.0 + 3.0 * rng.uniform();
					if (herd(s[0] + d[0] * dist, s[1] + d[1] * dist) >= 2) {
						break;
					}
				}
			}
			for (size_t k = 0; k < N / 24; ++k) {
				for (int attempt = 0; attempt < 32; ++attempt) {
					double x = 3.0 + rng.uniform() * (n - 6.0);
					double y = 3.0 + rng.uniform() * (n - 6.0);
					if (start_dist(x, y) > start_r + 8.0 and herd(x, y) >= 2) {
						break;
					}
				}
			}
		}
		if (b.fish) {
			size_t fish = 0;
			for (size_t i = 0; i < tile_count and fish < N; ++i) {
				auto t = map.tiles[i];
				if (t == map_terrain_t::WATER and rng.uniform() < 0.05) {
					map.objects.push_back({map_object_t::FISH_SHORE, static_cast<double>(i % N) + 0.5,
					                       static_cast<double>(i / N) + 0.5, 0, gaia});
					fish += 1;
				}
				else if (t == map_terrain_t::WATER_DEEP and rng.uniform() < 0.015) {
					map.objects.push_back({map_object_t::FISH_OCEAN, static_cast<double>(i % N) + 0.5,
					                       static_cast<double>(i / N) + 0.5, 40 * static_cast<int>(rng.below(9)), gaia});
					fish += 1;
				}
			}
		}

		// --- connect the starts (and the gold field) for land units: cheapest path where
		// water becomes a ford and trees/cacti are cleared, mines and buildings stay
		auto connect = [&](size_t from, size_t to) {
			const double inf = 1e18;
			std::vector<double> cost(tile_count, inf);
			std::vector<size_t> prev(tile_count, tile_count);
			using Item = std::pair<double, size_t>;
			std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
			cost[from] = 0.0;
			open.push({0.0, from});
			auto step_cost = [&](size_t i) {
				auto t = map.tiles[i];
				if (blocked[i]) {
					return removable[i] ? 6.0 : inf;
				}
				if (is_water(t) and t != map_terrain_t::SHALLOWS) {
					return 8.0;
				}
				return 1.0;
			};
			while (not open.empty()) {
				auto [c, i] = open.top();
				open.pop();
				if (c > cost[i]) {
					continue;
				}
				if (i == to) {
					break;
				}
				long x = static_cast<long>(i % N);
				long y = static_cast<long>(i / N);
				for (long dy = -1; dy <= 1; ++dy) {
					for (long dx = -1; dx <= 1; ++dx) {
						if ((dx == 0 and dy == 0) or not in_map(x + dx, y + dy)) {
							continue;
						}
						size_t j = tile_index(x + dx, y + dy);
						double sc = step_cost(j);
						if (sc >= inf) {
							continue;
						}
						// ties: prefer straight steps
						double nc = c + sc * ((dx != 0 and dy != 0) ? 1.01 : 1.0);
						if (nc < cost[j]) {
							cost[j] = nc;
							prev[j] = i;
							open.push({nc, j});
						}
					}
				}
			}
			if (cost[to] >= inf) {
				return;
			}
			std::vector<uint8_t> clear(tile_count, 0);
			for (size_t i = to; i != tile_count; i = prev[i]) {
				auto t = map.tiles[i];
				if (blocked[i] and removable[i]) {
					clear[i] = 1;
					blocked[i] = 0;
					removable[i] = 0;
					map.carved += 1;
				}
				else if (is_water(t) and t != map_terrain_t::SHALLOWS) {
					map.tiles[i] = ford_kind;
					map.carved += 1;
					// a wider ford: water neighbours along the path become shallow, too
					long x = static_cast<long>(i % N);
					long y = static_cast<long>(i / N);
					const long nb[4][2] = {{x + 1, y}, {x - 1, y}, {x, y + 1}, {x, y - 1}};
					for (const auto &p : nb) {
						if (in_map(p[0], p[1]) and is_water(map.tiles[tile_index(p[0], p[1])])) {
							map.tiles[tile_index(p[0], p[1])] = ford_kind;
						}
					}
				}
			}
			std::erase_if(map.objects, [&](const MapObject &o) {
				return (map_object_is_tree(o.kind) or o.kind == map_object_t::CACTUS)
				       and clear[tile_index(static_cast<long>(o.ne), static_cast<long>(o.se))];
			});
		};
		auto beside_town = [&](size_t player) {
			return tile_index(static_cast<long>(map.starts[player][0] + 3), static_cast<long>(map.starts[player][1]));
		};
		connect(beside_town(0), beside_town(1));
		if (b.gold_rush) {
			// free tile next to the gold field
			size_t goal = tile_index(centre_tile + 3, centre_tile);
			for (long dy = 0; dy <= 4 and blocked[goal]; ++dy) {
				goal = tile_index(centre_tile + 3, centre_tile + dy);
			}
			connect(beside_town(0), goal);
			connect(beside_town(1), goal);
		}
		// fish only on open water (a carved ford may have replaced their tile)
		std::erase_if(map.objects, [&](const MapObject &o) {
			if (not map_object_on_water(o.kind)) {
				return false;
			}
			auto t = map.tiles[tile_index(static_cast<long>(o.ne), static_cast<long>(o.se))];
			return t == map_terrain_t::SHALLOWS or not is_water(t);
		});
	}

	for (size_t i = 0; i < tile_count; ++i) {
		if (blocked[i]) {
			map.blocked.push_back(i);
		}
	}

	return map;
}


std::array<size_t, static_cast<size_t>(map_terrain_t::COUNT)> GeneratedMap::terrain_count() const {
	std::array<size_t, static_cast<size_t>(map_terrain_t::COUNT)> count{};
	for (auto t : this->tiles) {
		count[static_cast<size_t>(t)] += 1;
	}
	return count;
}

std::array<size_t, static_cast<size_t>(map_object_t::COUNT)> GeneratedMap::object_count() const {
	std::array<size_t, static_cast<size_t>(map_object_t::COUNT)> count{};
	for (const auto &o : this->objects) {
		count[static_cast<size_t>(o.kind)] += 1;
	}
	return count;
}

std::string GeneratedMap::summary() const {
	// kinds of the presets only when present: the summary of the original random map stays the same
	const size_t first_new_object = static_cast<size_t>(map_object_t::TREE_PALM);
	const size_t first_new_terrain = static_cast<size_t>(map_terrain_t::SAND);
	std::ostringstream out;
	if (this->biome != map_biome_t::GRASSLAND) {
		out << map_biome_name(this->biome) << ", ";
	}
	out << this->width << "x" << this->height << (this->river ? ", river" : ", no river");
	float max_h = 0.0f;
	for (auto h : this->corners) {
		max_h = std::max(max_h, h);
	}
	out << ", max elevation " << max_h << ", objects " << this->objects.size() << " (";
	auto objects = this->object_count();
	for (size_t k = 0; k < objects.size(); ++k) {
		if (k >= first_new_object and objects[k] == 0) {
			continue;
		}
		out << (k ? " " : "") << to_string(static_cast<map_object_t>(k)) << "=" << objects[k];
	}
	out << "), blocked tiles " << this->blocked.size() << ", terrain (";
	auto terrain = this->terrain_count();
	for (size_t k = 0; k < terrain.size(); ++k) {
		if (k >= first_new_terrain and terrain[k] == 0) {
			continue;
		}
		out << (k ? " " : "") << to_string(static_cast<map_terrain_t>(k)) << "=" << terrain[k];
	}
	out << ")";
	if (this->carved > 0) {
		out << ", carved " << this->carved;
	}
	return out.str();
}

const char *to_string(map_terrain_t terrain) {
	switch (terrain) {
	case map_terrain_t::GRASS:
		return "grass";
	case map_terrain_t::GRASS2:
		return "grass2";
	case map_terrain_t::GRASS3:
		return "grass3";
	case map_terrain_t::DIRT:
		return "dirt";
	case map_terrain_t::DIRT2:
		return "dirt2";
	case map_terrain_t::DIRT3:
		return "dirt3";
	case map_terrain_t::FOREST:
		return "forest";
	case map_terrain_t::BEACH:
		return "beach";
	case map_terrain_t::SHALLOWS:
		return "shallows";
	case map_terrain_t::WATER:
		return "water";
	case map_terrain_t::WATER_MEDIUM:
		return "water_medium";
	case map_terrain_t::WATER_DEEP:
		return "water_deep";
	case map_terrain_t::SAND:
		return "sand";
	case map_terrain_t::SNOW:
		return "snow";
	case map_terrain_t::SNOW_GRASS:
		return "snow_grass";
	case map_terrain_t::SNOW_DIRT:
		return "snow_dirt";
	case map_terrain_t::SNOW_FOREST:
		return "snow_forest";
	case map_terrain_t::ICE:
		return "ice";
	default:
		return "?";
	}
}

const char *to_string(map_object_t object) {
	switch (object) {
	case map_object_t::TREE_PINE:
		return "pine";
	case map_object_t::TREE_JUNGLE:
		return "jungle_tree";
	case map_object_t::GOLD:
		return "gold";
	case map_object_t::STONE:
		return "stone";
	case map_object_t::BERRIES:
		return "berries";
	case map_object_t::TOWN_CENTER:
		return "town_center";
	case map_object_t::VILLAGER:
		return "villager";
	case map_object_t::TREE_PALM:
		return "palm";
	case map_object_t::TREE_SNOW:
		return "snowy_conifer";
	case map_object_t::TREE_BAMBOO:
		return "bamboo";
	case map_object_t::CACTUS:
		return "cactus";
	case map_object_t::DEER:
		return "deer";
	case map_object_t::FISH_SHORE:
		return "shore_fish";
	case map_object_t::FISH_OCEAN:
		return "big_fish";
	default:
		return "?";
	}
}

} // namespace openage::gamestate
