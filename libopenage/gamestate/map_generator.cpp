// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "map_generator.h"

#include <algorithm>
#include <cmath>
#include <deque>
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

} // namespace


size_t map_generator_size(size_t requested) {
	size_t size = ((requested + 8) / 16) * 16;
	return std::clamp<size_t>(size, 48, 256);
}


GeneratedMap generate_map(const MapSettings &settings) {
	const size_t N = map_generator_size(settings.size);
	const auto n = static_cast<double>(N);
	const uint64_t seed = mix64(0x6f70656e61676500ull ^ settings.seed);
	Rng rng{seed};

	GeneratedMap map;
	map.width = N;
	map.height = N;
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
	const double start_r = std::max(7.0, 0.12 * n);
	auto start_dist = [&](double ne, double se) {
		double best = 1e9;
		for (const auto &s : map.starts) {
			double dx = ne - s[0];
			double dy = se - s[1];
			best = std::min(best, std::sqrt(dx * dx + dy * dy));
		}
		return best;
	};

	// --- water: lakes from noise, kept away from the start areas
	std::vector<uint8_t> water(tile_count, 0);
	std::vector<uint8_t> ford(tile_count, 0);
	{
		std::vector<double> lake(tile_count);
		for (size_t i = 0; i < tile_count; ++i) {
			double x = static_cast<double>(i % N) + 0.5;
			double y = static_cast<double>(i / N) + 0.5;
			double v = fbm(x, y, 16.0, seed + 1, 3);
			double d = start_dist(x, y);
			if (d < start_r + 8.0) {
				v += (start_r + 8.0 - d) * 0.1;
			}
			lake[i] = v;
		}
		double threshold = quantile(lake, 0.06);
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
		}
	}

	// --- river between the players (perpendicular to their axis), with two fords
	map.river = rng.uniform() < 0.6;
	if (map.river) {
		const double c = 0.5 * n;
		const double amp = 0.12 * n;
		auto centre = [&](double s) {
			double off = (fbm(s, 0.5, 14.0, seed + 2, 3) - 0.5) * 2.0 * amp;
			// axis u = (1, 1) / sqrt(2), normal v = (1, -1) / sqrt(2)
			const double r = 0.7071067811865476;
			return std::array<double, 2>{c + s * r + off * r, c + s * r - off * r};
		};
		for (double s = -0.75 * n; s <= 0.75 * n; s += 0.5) {
			auto p = centre(s);
			double radius = 1.1 + 0.9 * fbm(s, 7.5, 10.0, seed + 3, 2);
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
		for (double s : {-0.22 * n, 0.22 * n}) {
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
	}

	// distances: land tiles to water (fords count as water), water tiles to land (fords count as land)
	auto dist_water = distance_field(N, N, water);
	std::vector<uint8_t> land_or_ford(tile_count);
	for (size_t i = 0; i < tile_count; ++i) {
		land_or_ford[i] = (not water[i]) or ford[i];
	}
	auto dist_land = distance_field(N, N, land_or_ford);

	map.tiles.assign(tile_count, map_terrain_t::GRASS);
	for (size_t i = 0; i < tile_count; ++i) {
		if (ford[i]) {
			map.tiles[i] = map_terrain_t::SHALLOWS;
		}
		else if (water[i]) {
			int d = dist_land[i];
			map.tiles[i] = d <= 1 ? map_terrain_t::WATER
			             : d <= 3 ? map_terrain_t::WATER_MEDIUM
			                      : map_terrain_t::WATER_DEEP;
		}
		else if (dist_water[i] <= 1) {
			map.tiles[i] = map_terrain_t::BEACH;
		}
	}

	// --- hills: smooth noise, only the upper part raised; flat at water, shore and start areas
	map.corners.assign(corner_count, 0.0f);
	if (settings.max_elevation > 0.0f) {
		std::vector<double> hill(corner_count);
		for (size_t i = 0; i < corner_count; ++i) {
			double x = static_cast<double>(i % (N + 1));
			double y = static_cast<double>(i / (N + 1));
			hill[i] = fbm(x, y, 13.0, seed + 4, 3);
		}
		const double lo = quantile(hill, 0.60);
		const double hi = quantile(hill, 0.985);
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
			double v = smoothstep(lo, hi, hill[i]) * settings.max_elevation;
			v *= smoothstep(1.0, 4.0, static_cast<double>(wd));
			v *= smoothstep(start_r, start_r + 6.0, start_dist(static_cast<double>(x), static_cast<double>(y)));
			h[i] = v;
		}
		// limit slopes: neighbouring corners differ by at most max_step
		const double max_step = 0.75;
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
		return not is_water(map.tiles[i]) and not blocked[i];
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
			map.objects.push_back({cluster.kind, static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5, 0, 0});
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

	// --- forests
	{
		std::vector<double> forest(tile_count);
		std::vector<double> candidates;
		auto candidate = [&](size_t i) {
			return map.tiles[i] != map_terrain_t::BEACH and not is_water(map.tiles[i])
			       and not reserved[i] and not blocked[i];
		};
		for (size_t i = 0; i < tile_count; ++i) {
			double x = static_cast<double>(i % N) + 0.5;
			double y = static_cast<double>(i / N) + 0.5;
			forest[i] = fbm(x, y, 9.0, seed + 5, 3);
			if (candidate(i)) {
				candidates.push_back(forest[i]);
			}
		}
		double threshold = quantile(candidates, 0.78);
		for (size_t i = 0; i < tile_count; ++i) {
			if (candidate(i) and forest[i] > threshold) {
				map.tiles[i] = map_terrain_t::FOREST;
			}
		}

		// every player gets wood nearby (AoE: a woodline next to the town)
		for (const auto &s : map.starts) {
			size_t near = 0;
			for (size_t i = 0; i < tile_count; ++i) {
				double d = start_dist(static_cast<double>(i % N) + 0.5, static_cast<double>(i / N) + 0.5);
				if (map.tiles[i] == map_terrain_t::FOREST and d < start_r + 9.0) {
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
						map.tiles[i] = map_terrain_t::FOREST;
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
			if (map.tiles[i] == map_terrain_t::GRASS) {
				land_g.push_back(g[i]);
				land_d.push_back(d[i]);
			}
		}
		double g2 = quantile(land_g, 0.80);
		double g3 = quantile(land_g, 0.10);
		double d3 = quantile(land_d, 0.93);
		double d1 = quantile(land_d, 0.985);
		for (size_t i = 0; i < tile_count; ++i) {
			if (map.tiles[i] != map_terrain_t::GRASS) {
				continue;
			}
			if (d[i] > d1) {
				map.tiles[i] = map_terrain_t::DIRT;
			}
			else if (d[i] > d3) {
				map.tiles[i] = map_terrain_t::DIRT3;
			}
			else if (g[i] > g2) {
				map.tiles[i] = map_terrain_t::GRASS2;
			}
			else if (g[i] < g3) {
				map.tiles[i] = map_terrain_t::GRASS3;
			}
		}
	}

	// --- trees: one per forest tile (density < 1 for gaps), a few single trees on grass
	{
		std::vector<MapObject> trees;
		for (size_t i = 0; i < tile_count; ++i) {
			const auto t = map.tiles[i];
			double x = static_cast<double>(i % N) + 0.5;
			double y = static_cast<double>(i / N) + 0.5;
			bool tree = false;
			if (t == map_terrain_t::FOREST) {
				tree = rng.uniform() < 0.92;
			}
			else if ((t == map_terrain_t::GRASS or t == map_terrain_t::GRASS2) and not reserved[i] and not blocked[i]) {
				tree = rng.uniform() < 0.006;
			}
			if (not tree) {
				continue;
			}
			double jx = 0.12 * rng.signed_uniform();
			double jy = 0.12 * rng.signed_uniform();
			int angle = 40 * static_cast<int>(rng.below(9));
			// pine forests; broadleaf/palm trees ("jungle") only close to the water
			auto kind = dist_water[i] <= 3 ? map_object_t::TREE_JUNGLE : map_object_t::TREE_PINE;
			trees.push_back({kind, x + jx, y + jy, angle, 0});
		}
		if (trees.size() > settings.max_trees) {
			// deterministic thinning, the forest floor stays
			for (size_t k = trees.size() - 1; k > 0; --k) {
				std::swap(trees[k], trees[rng.below(k + 1)]);
			}
			trees.resize(settings.max_trees);
			std::sort(trees.begin(), trees.end(), [](const MapObject &a, const MapObject &b) {
				return a.se < b.se or (a.se == b.se and a.ne < b.ne);
			});
		}
		for (const auto &tree : trees) {
			blocked[tile_index(static_cast<long>(tree.ne), static_cast<long>(tree.se))] = 1;
			map.objects.push_back(tree);
		}
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
	std::ostringstream out;
	out << this->width << "x" << this->height << (this->river ? ", river" : ", no river");
	float max_h = 0.0f;
	for (auto h : this->corners) {
		max_h = std::max(max_h, h);
	}
	out << ", max elevation " << max_h << ", objects " << this->objects.size() << " (";
	auto objects = this->object_count();
	for (size_t k = 0; k < objects.size(); ++k) {
		out << (k ? " " : "") << to_string(static_cast<map_object_t>(k)) << "=" << objects[k];
	}
	out << "), blocked tiles " << this->blocked.size() << ", terrain (";
	auto terrain = this->terrain_count();
	for (size_t k = 0; k < terrain.size(); ++k) {
		out << (k ? " " : "") << to_string(static_cast<map_terrain_t>(k)) << "=" << terrain[k];
	}
	out << ")";
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
	default:
		return "?";
	}
}

} // namespace openage::gamestate
