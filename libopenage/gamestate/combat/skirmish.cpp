// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "skirmish.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>


namespace openage::gamestate::combat {

namespace {

bool water(map_terrain_t t) {
	return t == map_terrain_t::WATER or t == map_terrain_t::WATER_MEDIUM
	       or t == map_terrain_t::WATER_DEEP or t == map_terrain_t::SHALLOWS;
}

constexpr double START_CLEARANCE = 7.0;
constexpr double SPACING = 1.0;

} // namespace


SkirmishLayout skirmish_layout(const GeneratedMap &map) {
	SkirmishLayout layout;
	if (map.starts.size() < 2 or map.width == 0) {
		return layout;
	}
	const auto &s0 = map.starts[0];
	const auto &s1 = map.starts[1];
	double ux = s1[0] - s0[0];
	double uy = s1[1] - s0[1];
	double len = std::hypot(ux, uy);
	if (len < 1e-6) {
		return layout;
	}
	ux /= len;
	uy /= len;
	// across the line between the starts
	const double vx = -uy;
	const double vy = ux;
	const double mx = 0.5 * (s0[0] + s1[0]);
	const double my = 0.5 * (s0[1] + s1[1]);

	std::unordered_set<size_t> blocked(map.blocked.begin(), map.blocked.end());
	const auto N = static_cast<long>(map.width);
	auto free_tile = [&](double ne, double se) {
		auto x = static_cast<long>(std::floor(ne));
		auto y = static_cast<long>(std::floor(se));
		if (x < 2 or y < 2 or x >= N - 2 or y >= N - 2) {
			return false;
		}
		auto i = static_cast<size_t>(x) + static_cast<size_t>(y) * map.width;
		if (water(map.tiles[i]) or map.tiles[i] == map_terrain_t::BEACH or blocked.contains(i)) {
			return false;
		}
		for (const auto &s : map.starts) {
			if (std::hypot(ne - s[0], se - s[1]) < START_CLEARANCE) {
				return false;
			}
		}
		return true;
	};

	auto build = [&](double cx, double cy, double gap) {
		std::vector<SkirmishUnit> units;
		for (size_t player = 0; player < 2; ++player) {
			// player 0 stands on the side of start 0
			double side = player == 0 ? -1.0 : 1.0;
			const skirmish_unit_t rows[] = {skirmish_unit_t::KNIGHT, skirmish_unit_t::MILITIA, skirmish_unit_t::ARCHER};
			for (int row = 0; row < 3; ++row) {
				double d = 0.5 * gap + SPACING * row;
				for (int col = -1; col <= 1; ++col) {
					SkirmishUnit unit;
					unit.kind = rows[row];
					// unit centres in the middle of tiles
					unit.ne = std::floor(cx + side * ux * d + vx * SPACING * col) + 0.5;
					unit.se = std::floor(cy + side * uy * d + vy * SPACING * col) + 0.5;
					unit.owner = player;
					unit.face_ne = -side * ux;
					unit.face_se = -side * uy;
					if (not free_tile(unit.ne, unit.se)) {
						return std::vector<SkirmishUnit>{};
					}
					units.push_back(unit);
				}
			}
		}
		// all positions on different tiles
		std::unordered_set<long> seen;
		for (const auto &u : units) {
			long key = static_cast<long>(std::floor(u.ne)) + static_cast<long>(std::floor(u.se)) * N;
			if (not seen.insert(key).second) {
				return std::vector<SkirmishUnit>{};
			}
		}
		return units;
	};

	// candidate centres on a grid around the midpoint, nearest first
	struct Candidate {
		double along;
		double across;
		double order;
	};
	std::vector<Candidate> candidates;
	const double reach = 0.35 * static_cast<double>(map.width);
	for (double along = -reach; along <= reach; along += 1.0) {
		for (double across = -reach; across <= reach; across += 1.0) {
			candidates.push_back({along, across, along * along + across * across + 1e-3 * (along + 2.0 * across)});
		}
	}
	std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
		return a.order < b.order;
	});
	for (double gap : {SKIRMISH_GAP, SKIRMISH_GAP + 2.0, SKIRMISH_GAP + 4.0}) {
		for (const auto &c : candidates) {
			double cx = mx + ux * c.along + vx * c.across;
			double cy = my + uy * c.along + vy * c.across;
			auto units = build(cx, cy, gap);
			if (not units.empty()) {
				layout.units = std::move(units);
				layout.center_ne = cx;
				layout.center_se = cy;
				layout.placed = true;
				return layout;
			}
		}
	}
	return layout;
}

const char *to_string(skirmish_unit_t kind) {
	switch (kind) {
	case skirmish_unit_t::KNIGHT:
		return "knight";
	case skirmish_unit_t::MILITIA:
		return "militia";
	case skirmish_unit_t::ARCHER:
	default:
		return "archer";
	}
}

} // namespace openage::gamestate::combat
