// Copyright 2026-2026 the openage authors. See copying.md for legal info.

/*
 * Host test of the food rules (XR fork W3, gamestate/food_math.h) and of the animals
 * of the random map (map_generator.cpp, MapSettings::herds):
 *
 *   openage-food-check
 *
 * Parameter sweeps:
 * - source kinds from the nyan types, labels, info lines
 * - strike ranges (prey at the spear range, melee for boars/herdables), edge distances
 * - flee points: away from the attacker, rotated at most 40°, inside the map
 * - decay: linear with the rate, never below 0, independent of the step size
 * - herd owner: gaia -> the nearest player unit that sees it; an owned herdable stays
 *   while its owner sees it, else the nearest foreign unit within 2 tiles takes it;
 *   deterministic tie break; random sweeps against a reference rule
 * - fields: reseed only with a queue and a farmer, decay time, construction stages
 * - reseed queue: limit, cancel at any index refunds that cost, take is FIFO
 * - water placement: dock needs water + shore, trap only water; order of the checks
 * - random maps (12 landscapes x seeds x sizes): sheep in sight of every town centre,
 *   more sheep further away, a boar and deer per start, no animal on water or a blocked
 *   tile; herds = false gives the same objects as before W3 (prefix of the object list)
 * Exit code 0 if all checks pass. No dependencies besides the map generator.
 */

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "gamestate/food_math.h"
#include "gamestate/map_generator.h"

using namespace openage::gamestate;
using namespace openage::gamestate::food;

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

void check_kinds() {
	check(source_of({"Unit", "Herdable"}, "Sheep") == source_t::HERDABLE, "sheep herdable");
	check(source_of({"Unit", "AnimalPrey"}, "Deer") == source_t::PREY, "deer prey");
	check(source_of({"Unit", "AnimalPredator"}, "Boar") == source_t::PREDATOR, "boar predator");
	check(source_of({"Ambient", "ShoreFish"}, "Shorefish") == source_t::FISH, "shore fish");
	check(source_of({"Ambient", "OceanFish"}, "BigOceanFish") == source_t::FISH, "ocean fish");
	check(source_of({"Building", "Restockable"}, "Farm") == source_t::FARM, "farm");
	check(source_of({"Building", "Restockable"}, "FishingTrap") == source_t::TRAP, "trap");
	check(source_of({"Ambient"}, "BerryBush") == source_t::OTHER, "berries other");
	check(source_of({}, "GoldMine") == source_t::OTHER, "gold other");
	for (auto k : {source_t::PREY, source_t::PREDATOR, source_t::HERDABLE}) {
		check(is_animal(k) and not is_field(k), "animal kinds");
	}
	for (auto k : {source_t::FARM, source_t::TRAP}) {
		check(is_field(k) and not is_animal(k), "field kinds");
	}
	check(not is_animal(source_t::FISH) and not is_field(source_t::FISH) and not is_animal(source_t::OTHER), "fish/other");
	check(amount_text(source_t::FARM, 175.4, false, false) == "Feld – Nahrung 175", amount_text(source_t::FARM, 175.4, false, false));
	check(amount_text(source_t::PREY, 120.9, true, false) == "Wild – Nahrung 120 (verdirbt)",
	      amount_text(source_t::PREY, 120.9, true, false));
	check(amount_text(source_t::FARM, 0.0, false, true) == "Feld – verdorrt", amount_text(source_t::FARM, 0.0, false, true));
	check(amount_text(source_t::OTHER, 10.0, false, false) == "Nahrung 10", "other source");
	check(amount_text(source_t::HERDABLE, -5.0, true, false).find("Nahrung 0") != std::string::npos, "never negative");
}

void check_strike_flee() {
	check(strike_range(source_t::PREY) == PREY_STRIKE_RANGE and strike_range(source_t::PREDATOR) < 1.0
	          and strike_range(source_t::HERDABLE) < 1.0,
	      "strike ranges");
	for (double d = 0.0; d <= 6.0; d += 0.05) {
		check(can_strike(source_t::PREY, d) == (d <= PREY_STRIKE_RANGE + 1e-6), "prey strike at " + std::to_string(d));
		check(can_strike(source_t::PREDATOR, d) == (d <= MELEE_STRIKE_RANGE + 1e-6), "melee strike at " + std::to_string(d));
	}
	check(edge_distance(0, 0, 0.3, 3, 4, 0.2) == 4.5, "edge distance");
	check(edge_distance(0, 0, 1.0, 0.5, 0, 1.0) == 0.0, "overlap gives 0");
	std::mt19937 rng{7};
	std::uniform_real_distribution<double> u{0.0, 64.0};
	std::uniform_real_distribution<double> j{-1.0, 1.0};
	for (int i = 0; i < 4000; ++i) {
		double ne = u(rng), se = u(rng), ane = u(rng), ase = u(rng), jit = j(rng);
		auto [x, y] = flee_point(ne, se, ane, ase, FLEE_DISTANCE, jit, 64.0, 64.0);
		check(x >= 0.5 and x <= 63.5 and y >= 0.5 and y <= 63.5, "flee point inside the map");
		const double away = std::atan2(se - ase, ne - ane);
		const double dir = std::atan2(y - se, x - ne);
		double diff = std::abs(std::remainder(dir - away, 2.0 * M_PI));
		const bool clamped = x <= 0.5 + 1e-9 or x >= 63.5 - 1e-9 or y <= 0.5 + 1e-9 or y >= 63.5 - 1e-9;
		if (std::hypot(ne - ane, se - ase) > 1e-3 and not clamped) {
			check(diff <= 0.6982 + 1e-6, "flee away from the attacker (rotated <= 40°)");
			check(std::abs(std::hypot(x - ne, y - se) - FLEE_DISTANCE) < 1e-6, "flee distance");
		}
	}
	// same position: still a direction
	auto [x, y] = flee_point(10, 10, 10, 10, 3.0, 0.0, 64, 64);
	check(std::hypot(x - 10, y - 10) > 2.9, "flee without direction still moves");
}

void check_decay() {
	for (double rate : {0.0, 0.25, 0.4, 1.0}) {
		for (double amount : {0.0, 5.0, 100.0, 340.0}) {
			double a = amount;
			double b = amount;
			for (int i = 0; i < 400; ++i) {
				a = decay(a, rate, 0.5);
			}
			for (int i = 0; i < 4; ++i) {
				b = decay(b, rate, 50.0);
			}
			check(std::abs(a - b) < 1e-6, "decay independent of the step size");
			check(std::abs(a - std::max(0.0, amount - rate * 200.0)) < 1e-6, "decay linear");
			check(a >= 0.0, "decay never below 0");
		}
	}
	check(decay(10.0, 0.25, -1.0) == 10.0 and decay(10.0, -1.0, 5.0) == 10.0, "no decay for bad input");
}

/// reference rule for herd_owner (independent implementation)
std::optional<size_t> reference_owner(size_t owner, bool gaia, const std::vector<Watcher> &ws) {
	const Watcher *best = nullptr;
	for (const auto &w : ws) {
		bool ok = gaia ? w.distance <= w.sight : (w.player != owner and w.distance <= HERD_CONVERT_RANGE);
		if (not gaia and w.player == owner and w.distance <= w.sight) {
			return std::nullopt;
		}
		if (ok and (best == nullptr or w.distance < best->distance or (w.distance == best->distance and w.id < best->id))) {
			best = &w;
		}
	}
	return best ? std::optional<size_t>{best->player} : std::nullopt;
}

void check_herd() {
	// gaia: nearest unit that sees it
	check(herd_owner(2, true, {}) == std::nullopt, "nobody sees it");
	check(herd_owner(2, true, {{0, 5.0, 4.0, 1}}) == std::nullopt, "out of sight");
	check(herd_owner(2, true, {{0, 3.0, 4.0, 1}, {1, 2.0, 4.0, 2}}) == std::optional<size_t>{1}, "nearest sees first");
	check(herd_owner(2, true, {{0, 2.0, 4.0, 5}, {1, 2.0, 4.0, 3}}) == std::optional<size_t>{1}, "tie: lower id");
	// owned: own unit in sight keeps it
	check(herd_owner(0, false, {{0, 3.0, 4.0, 1}, {1, 1.0, 4.0, 2}}) == std::nullopt, "own unit keeps it");
	check(herd_owner(0, false, {{0, 5.0, 4.0, 1}, {1, 1.0, 4.0, 2}}) == std::optional<size_t>{1}, "foreign unit near takes it");
	check(herd_owner(0, false, {{1, 2.5, 4.0, 2}}) == std::nullopt, "foreign unit too far (> 2)");
	check(herd_owner(0, false, {{0, 9.0, 8.0, 1}, {1, 1.9, 4.0, 2}}) == std::optional<size_t>{1}, "own unit out of sight");
	std::mt19937 rng{11};
	std::uniform_int_distribution<int> players{0, 2};
	std::uniform_real_distribution<double> dist{0.0, 10.0};
	std::uniform_real_distribution<double> sight{1.0, 8.0};
	for (int i = 0; i < 20000; ++i) {
		std::vector<Watcher> ws;
		int n = static_cast<int>(rng() % 6);
		for (int k = 0; k < n; ++k) {
			ws.push_back({static_cast<size_t>(players(rng)), std::round(dist(rng) * 4.0) / 4.0, sight(rng),
			              static_cast<uint64_t>(rng() % 50)});
		}
		size_t owner = static_cast<size_t>(players(rng));
		bool gaia = owner == 2;
		check(herd_owner(owner, gaia, ws) == reference_owner(owner, gaia, ws), "herd owner = reference (sweep)");
	}
}

void check_fields() {
	check(on_field_empty(source_t::FARM, 1, true) == empty_t::RESEED, "reseed with queue and farmer");
	check(on_field_empty(source_t::FARM, 0, true) == empty_t::DECAY, "no queue: decay");
	check(on_field_empty(source_t::FARM, 3, false) == empty_t::DECAY, "no farmer: decay");
	check(on_field_empty(source_t::TRAP, 3, true) == empty_t::DECAY, "traps are not reseeded by the mill");
	check(not field_expired(100.0, 100.0 + FIELD_DECAY_SECONDS - 0.1) and field_expired(100.0, 100.0 + FIELD_DECAY_SECONDS),
	      "decay time");
	for (size_t stages : {0u, 1u, 3u, 4u}) {
		size_t last = 0;
		for (double p = -0.5; p <= 1.5; p += 0.01) {
			auto s = construction_stage(p, stages);
			check(stages == 0 ? s == 0 : s < stages, "stage in range");
			check(s >= last or p < 0.0, "stages grow with the progress");
			last = s;
		}
		if (stages > 0) {
			check(construction_stage(1.0, stages) == stages - 1 and construction_stage(0.0, stages) == 0, "first/last stage");
		}
	}
	ReseedQueue q;
	for (size_t i = 0; i < ReseedQueue::MAX + 3; ++i) {
		bool ok = q.push({0.0, static_cast<double>(i), 0.0, 0.0});
		check(ok == (i < ReseedQueue::MAX), "queue limit");
	}
	check(q.size() == ReseedQueue::MAX, "full queue");
	auto c = q.cancel_at(3);
	check(c and (*c)[1] == 3.0 and q.size() == ReseedQueue::MAX - 1, "cancel at index refunds its cost");
	check(not q.cancel_at(100), "cancel outside");
	auto t = q.take();
	check(t and (*t)[1] == 0.0, "take is FIFO");
	t = q.take();
	check(t and (*t)[1] == 1.0, "take is FIFO (2)");
	q.clear();
	check(q.empty() and not q.take(), "empty queue");
}

void check_water_rules() {
	// map: x < 5 land, x == 5 beach, 6..9 water (placement terrain), x >= 10 deep water (allowed); tile (8, 8) blocked, (9, 4) a ship
	auto state = [](const econ::tile_pos &t) {
		if (t.ne < 0 or t.se < 0 or t.ne >= 16 or t.se >= 16) {
			return water_tile_t::OUTSIDE;
		}
		if (t.ne == 8 and t.se == 8) {
			return water_tile_t::BLOCKED;
		}
		if (t.ne == 9 and t.se == 4) {
			return water_tile_t::OCCUPIED;
		}
		if (t.ne < 5) {
			return water_tile_t::LAND;
		}
		if (t.ne == 5) {
			return water_tile_t::SHORE;
		}
		return t.ne >= 10 ? water_tile_t::ALLOWED : water_tile_t::WATER;
	};
	for (long x = -2; x < 16; ++x) {
		for (long y = -2; y < 16; ++y) {
			// 3x3 dock with its corner at (x, y)
			std::vector<econ::tile_pos> tiles;
			for (long dy = 0; dy < 3; ++dy) {
				for (long dx = 0; dx < 3; ++dx) {
					tiles.push_back({x + dx, y + dy});
				}
			}
			auto dock = check_water_placement(tiles, true, true, state);
			auto trap = check_water_placement(tiles, false, false, state);
			bool outside = x < 0 or y < 0 or x + 2 >= 16 or y + 2 >= 16;
			bool all_water = x >= 6;
			bool has_blocked = x <= 8 and x + 2 >= 8 and y <= 8 and y + 2 >= 8;
			bool has_ship = x <= 9 and x + 2 >= 9 and y <= 4 and y + 2 >= 4;
			bool near_shore = x == 6;
			water_place_t expect_trap = outside ? water_place_t::OUTSIDE
			                          : not all_water ? water_place_t::NOT_WATER
			                          : has_blocked   ? water_place_t::BLOCKED
			                          : has_ship      ? water_place_t::OCCUPIED
			                                          : water_place_t::OK;
			// the dock needs one tile of its placement terrain (x <= 9) and the shore next to it
			water_place_t expect_dock = expect_trap;
			if (expect_trap == water_place_t::OK or expect_trap == water_place_t::BLOCKED
			    or expect_trap == water_place_t::OCCUPIED) {
				if (x > 9) {
					expect_dock = water_place_t::NOT_WATER;
				}
				else if (expect_trap == water_place_t::OK and not near_shore) {
					expect_dock = water_place_t::NO_SHORE;
				}
			}
			check(trap == expect_trap, "trap placement at " + std::to_string(x) + "," + std::to_string(y));
			check(dock == expect_dock, "dock placement at " + std::to_string(x) + "," + std::to_string(y));
		}
	}
	for (auto r : {water_place_t::OUTSIDE, water_place_t::NOT_WATER, water_place_t::NO_SHORE, water_place_t::BLOCKED,
	               water_place_t::OCCUPIED}) {
		check(std::string{water_place_message(r)}.find("Hier kann nicht gebaut werden") == 0, "message of a failed placement");
	}
	check(std::string{water_place_message(water_place_t::OK)}.empty(), "no message for OK");
}

void check_maps() {
	size_t maps = 0;
	for (int b = 0; b <= static_cast<int>(map_biome_t::JUNGLE); ++b) {
		for (size_t size : {48, 64, 96}) {
			for (uint32_t seed = 1; seed <= 6; ++seed) {
				MapSettings s;
				s.type = map_type_t::RANDOM;
				s.biome = static_cast<map_biome_t>(b);
				s.seed = seed;
				s.size = size;
				s.relics = false;  // W6: relics come after the animals (own check in special-game-check)
				auto with = generate_map(s);
				s.herds = false;
				auto without = generate_map(s);
				maps += 1;
				const std::string what = std::string{map_biome_name(static_cast<map_biome_t>(b))} + " " + std::to_string(size)
				                         + " #" + std::to_string(seed);
				// the animals come last: without them the object list is the old one
				bool prefix = without.objects.size() <= with.objects.size() and without.tiles == with.tiles
				              and without.blocked == with.blocked;
				for (size_t i = 0; prefix and i < without.objects.size(); ++i) {
					const auto &a = without.objects[i];
					const auto &c = with.objects[i];
					prefix = a.kind == c.kind and a.ne == c.ne and a.se == c.se and a.owner == c.owner and a.angle == c.angle;
				}
				check(prefix, what + ": herds are added after the old objects");
				std::set<size_t> blocked(with.blocked.begin(), with.blocked.end());
				const size_t gaia = with.starts.size();
				for (size_t p = 0; p < with.starts.size(); ++p) {
					const auto &st = with.starts[p];
					size_t near = 0, far = 0, boar = 0, deer = 0;
					for (const auto &o : with.objects) {
						const double d = std::hypot(o.ne - st[0], o.se - st[1]);
						if (o.kind == map_object_t::SHEEP) {
							(d <= 8.0 ? near : far) += d <= 20.0 ? 1 : 0;
						}
						boar += o.kind == map_object_t::BOAR and d <= 24.0 ? 1 : 0;
						deer += o.kind == map_object_t::DEER and d <= 24.0 ? 1 : 0;
					}
					check(near >= 3, what + ": sheep in sight of start " + std::to_string(p) + " (" + std::to_string(near) + ")");
					check(far >= 2, what + ": more sheep further away (" + std::to_string(far) + ")");
					check(boar >= 1, what + ": a boar near start " + std::to_string(p));
					check(deer >= 2, what + ": deer near start " + std::to_string(p) + " (" + std::to_string(deer) + ")");
				}
				for (const auto &o : with.objects) {
					if (o.kind != map_object_t::SHEEP and o.kind != map_object_t::BOAR and o.kind != map_object_t::DEER) {
						continue;
					}
					const auto tx = static_cast<size_t>(o.ne);
					const auto ty = static_cast<size_t>(o.se);
					const auto t = with.tiles[tx + ty * with.width];
					check(t != map_terrain_t::WATER and t != map_terrain_t::WATER_MEDIUM and t != map_terrain_t::WATER_DEEP
					          and t != map_terrain_t::ICE,
					      what + ": animal on land");
					check(not blocked.contains(tx + ty * with.width), what + ": animal not on a blocked tile");
					check(o.owner == gaia, what + ": animals belong to gaia on the map");
				}
				// deterministic
				s.herds = true;
				auto again = generate_map(s);
				bool same = again.objects.size() == with.objects.size();
				for (size_t i = 0; same and i < again.objects.size(); ++i) {
					same = again.objects[i].ne == with.objects[i].ne and again.objects[i].se == with.objects[i].se;
				}
				check(same, what + ": deterministic");
			}
		}
	}
	std::cout << "  " << maps << " maps" << std::endl;
}

} // namespace

int main() {
	check_kinds();
	check_strike_flee();
	check_decay();
	check_herd();
	check_fields();
	check_water_rules();
	check_maps();
	if (failures > 0) {
		std::cout << "food check: " << checks << " checks, " << failures << " failures" << std::endl;
		return EXIT_FAILURE;
	}
	std::cout << "food check: " << checks << " checks, 0 failures" << std::endl;
	return EXIT_SUCCESS;
}
