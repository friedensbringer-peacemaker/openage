// Copyright 2026-2026 the openage authors. See copying.md for legal info.

/*
 * Host test of the combat rules and the skirmish layout (XR fork).
 *
 *   openage-combat-rules-check [--layout <seed> <size>]
 *
 * Parameter sweeps:
 * - damage: AoE II formula max(1, attack - armor) per class, classes without
 *   armor entry ignored, fallback minimum, monotonic in attack and armor
 * - ranges: melee contact, ranged min/max range, edge distances of circles
 *   and squares (symmetric, 0 when overlapping)
 * - approach points: outside the target footprint, at the wanted gap, in range
 * - picking: point-segment distance
 * - victory: decided only with >= 2 participants and <= 1 left, winner/defeat/draw
 * - skirmish layout over seeds and sizes: placed, 9 units per player on free
 *   land tiles, armies facing each other out of sight, deterministic
 *
 * --layout prints the layout (centre, first enemy knight) for render checks.
 * Exit code 0 if all checks pass. No dependencies besides the sources.
 */

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_set>

#include "gamestate/combat/rules.h"
#include "gamestate/combat/skirmish.h"
#include "gamestate/map_generator.h"

using namespace openage::gamestate;
using namespace openage::gamestate::combat;

namespace {

int failures = 0;
size_t checks = 0;

void check(bool ok, const std::string &what) {
	checks += 1;
	if (not ok) {
		failures += 1;
		if (failures <= 40) {
			std::cerr << "FAIL: " << what << "\n";
		}
	}
}

const std::string MELEE = "x.types.Melee";
const std::string PIERCE = "x.types.Pierce";
const std::string CAVALRY = "x.types.Cavalry";
const std::string FALLBACK = "engine.util.attribute_change_type.type.Fallback";

void damage_sweep() {
	// single class: max(1, attack - armor)
	for (int64_t attack = 0; attack <= 30; ++attack) {
		for (int64_t armor = 0; armor <= 30; ++armor) {
			AttackProfile a{{{MELEE, attack, 0}, {FALLBACK, 1, 1}}};
			ArmorProfile r;
			r.block[MELEE] = armor;
			r.block[FALLBACK] = 0;
			auto dmg = compute_damage(a, r);
			check(dmg == std::max<int64_t>(1, attack - armor),
			      "damage " + std::to_string(attack) + " vs " + std::to_string(armor));
			// monotonic
			AttackProfile a2{{{MELEE, attack + 1, 0}}};
			check(compute_damage(a2, r) >= dmg, "monotonic in attack");
			ArmorProfile r2 = r;
			r2.block[MELEE] = armor + 1;
			check(compute_damage(a, r2) <= dmg, "monotonic in armor");
		}
	}
	// two classes add up; a class without armor entry does not apply
	AttackProfile knight{{{MELEE, 10, 0}, {"x.types.Archer", 0, 0}, {FALLBACK, 1, 1}}};
	ArmorProfile militia;
	militia.block[MELEE] = 0;
	militia.block[PIERCE] = 1;
	check(compute_damage(knight, militia) == 10, "knight vs militia = 10");
	ArmorProfile knight_armor;
	knight_armor.block[MELEE] = 2;
	knight_armor.block[PIERCE] = 2;
	knight_armor.block[CAVALRY] = 0;
	AttackProfile archer{{{PIERCE, 4, 0}, {"x.types.Spearman", 3, 0}, {FALLBACK, 1, 1}}};
	check(compute_damage(archer, knight_armor) == 2, "archer vs knight = 2");
	AttackProfile spear{{{MELEE, 3, 0}, {CAVALRY, 15, 0}}};
	check(compute_damage(spear, knight_armor) == 16, "bonus damage adds up");
	// no matching class at all: minimum damage
	AttackProfile odd{{{"x.types.Ship", 50, 0}}};
	check(compute_damage(odd, knight_armor) == MIN_DAMAGE, "no matching class -> 1");
	check(compute_damage(AttackProfile{}, ArmorProfile{}) == MIN_DAMAGE, "empty -> 1");
	check(attack_sum(knight) == 10, "attack sum ignores fallback");
}

void range_sweep() {
	for (double d = 0.0; d <= 12.0; d += 0.05) {
		for (double ra : {0.2, 0.25, 0.5}) {
			for (double rb : {0.2, 0.25, 2.0}) {
				double e = edge_distance(d, ra, rb);
				check(e >= 0.0 and std::abs(e - std::max(0.0, d - ra - rb)) < 1e-12, "edge distance");
				Footprint a{{0.0, 0.0}, ra, false};
				Footprint b{{d, 0.0}, rb, false};
				check(std::abs(edge_distance(a, b) - e) < 1e-9, "circle footprints");
				check(std::abs(edge_distance(a, b) - edge_distance(b, a)) < 1e-9, "symmetric");
				Footprint sq{{d, 0.0}, rb, true};
				check(std::abs(edge_distance(a, sq) - edge_distance(sq, a)) < 1e-9, "symmetric square");
				// along an axis the square and the circle have the same edge
				check(std::abs(edge_distance(a, sq) - e) < 1e-9, "square on the axis");
				// melee: in contact up to MELEE_RANGE
				check(in_attack_range(e, 0.0, 0.0) == (e <= MELEE_RANGE + RANGE_EPSILON), "melee range");
				// ranged with minimum range
				check(in_attack_range(e, 1.0, 4.0) == (e + RANGE_EPSILON >= 1.0 and e <= 4.0 + RANGE_EPSILON),
				      "ranged range");
			}
		}
	}
	// diagonal next to a building: the square edge is the real gap
	Footprint tc{{10.0, 10.0}, 2.0, true};
	Footprint unit{{12.5, 12.5}, 0.25, false};
	check(std::abs(edge_distance(unit, tc) - (std::hypot(0.5, 0.5) - 0.25)) < 1e-9, "diagonal square edge");
	check(edge_distance(Footprint{{11.0, 10.0}, 0.25, false}, tc) == 0.0, "inside the square");
	Footprint tc2{{16.0, 10.0}, 2.0, true};
	check(std::abs(edge_distance(tc, tc2) - 2.0) < 1e-9, "square to square");
}

void approach_sweep() {
	for (bool square : {false, true}) {
		for (double r : {0.25, 1.0, 2.0}) {
			Footprint target{{20.0, 20.0}, r, square};
			for (int k = 0; k < 64; ++k) {
				double ang = 6.283185307 * k / 64.0;
				for (double dist : {r + 0.1, r + 3.0, r + 10.0}) {
					Point attacker{20.0 + std::cos(ang) * dist * 1.5, 20.0 + std::sin(ang) * dist * 1.5};
					for (double gap : {0.2, 3.2}) {
						auto p = approach_point(attacker, 0.25, target, gap);
						double e = edge_distance(Footprint{p, 0.25, false}, target);
						double e0 = edge_distance(Footprint{attacker, 0.25, false}, target);
						if (e0 > gap + 1e-9) {
							check(std::abs(e - gap) < 1e-6, "approach point at the gap");
							check(in_attack_range(e, 0.0, gap > 1.0 ? 4.0 : 0.0), "approach point in range");
						}
						else {
							check(distance(p, attacker) < 1e-12, "already close: stay");
						}
					}
				}
			}
			// attacker inside the target: leaves in the fallback direction
			for (int k = 0; k < 8; ++k) {
				double fb = 0.785398 * k;
				auto p = approach_point(target.center, 0.25, target, 0.2, fb);
				check(edge_distance(Footprint{p, 0.25, false}, target) > 0.1, "out of the footprint");
			}
		}
	}
	// picking geometry
	check(point_segment_distance({0, 1}, {-1, 0}, {1, 0}) == 1.0, "segment distance middle");
	check(std::abs(point_segment_distance({3, 0}, {-1, 0}, {1, 0}) - 2.0) < 1e-12, "segment distance end");
	check(point_segment_distance({0, 0}, {0, 0}, {0, 0}) == 0.0, "degenerate segment");
}

void victory_sweep() {
	// participants 0..3, any number left
	for (size_t players = 0; players <= 4; ++players) {
		for (unsigned mask = 0; mask < (1u << players); ++mask) {
			std::map<uint64_t, size_t> alive;
			size_t left = 0;
			for (size_t p = 0; p < players; ++p) {
				bool has = mask & (1u << p);
				alive[p] = has ? 1 + p : 0;
				left += has ? 1 : 0;
			}
			auto r = evaluate_match(alive);
			bool expect_over = players >= 2 and left <= 1;
			check(r.over == expect_over, "match over");
			check(r.defeated.size() == players - left, "defeated count");
			if (expect_over and left == 1) {
				check(r.winner.has_value() and (mask & (1u << *r.winner)), "winner");
				check(r.state_for(*r.winner) == match_state_t::VICTORY, "state victory");
				for (auto p : r.defeated) {
					check(r.state_for(p) == match_state_t::DEFEAT, "state defeat");
				}
			}
			if (expect_over and left == 0) {
				check(not r.winner.has_value(), "draw has no winner");
				for (auto p : r.defeated) {
					check(r.state_for(p) == match_state_t::DRAW, "state draw");
				}
			}
			if (not expect_over) {
				check(not r.winner.has_value(), "running: no winner");
				for (size_t p = 0; p < players; ++p) {
					if (mask & (1u << p)) {
						check(r.state_for(p) == match_state_t::RUNNING, "state running");
					}
				}
			}
		}
	}
	check(evaluate_match({{0, 5}}).over == false, "single player never wins");
	check(std::string{to_string(match_state_t::VICTORY)} == "victory", "names");
}

bool water(map_terrain_t t) {
	return t == map_terrain_t::WATER or t == map_terrain_t::WATER_MEDIUM
	       or t == map_terrain_t::WATER_DEEP or t == map_terrain_t::SHALLOWS;
}

void layout_sweep() {
	size_t placed = 0;
	size_t total = 0;
	for (size_t size : {48, 64, 96, 128}) {
		for (uint32_t seed = 1; seed <= 25; ++seed) {
			MapSettings s;
			s.type = map_type_t::RANDOM;
			s.seed = seed;
			s.size = size;
			auto map = generate_map(s);
			auto layout = skirmish_layout(map);
			auto again = skirmish_layout(map);
			total += 1;
			check(layout.units.size() == again.units.size() and layout.center_ne == again.center_ne,
			      "layout deterministic");
			if (not layout.placed) {
				continue;
			}
			placed += 1;
			check(layout.units.size() == 18, "18 units");
			std::unordered_set<size_t> blocked(map.blocked.begin(), map.blocked.end());
			size_t per_player[2] = {0, 0};
			std::unordered_set<size_t> tiles;
			for (const auto &u : layout.units) {
				auto x = static_cast<size_t>(u.ne);
				auto y = static_cast<size_t>(u.se);
				size_t i = x + y * map.width;
				check(x < map.width and y < map.height, "unit inside the map");
				check(not water(map.tiles[i]) and not blocked.contains(i), "unit on free land");
				check(tiles.insert(i).second, "one unit per tile");
				per_player[u.owner] += 1;
				// faces the other army
				double fx = layout.center_ne - u.ne;
				double fy = layout.center_se - u.se;
				check(fx * u.face_ne + fy * u.face_se > 0.0, "faces the enemy");
			}
			check(per_player[0] == 9 and per_player[1] == 9, "9 units per player");
			// out of sight of each other (knights 4, archers 6 tiles)
			double closest = 1e9;
			for (const auto &a : layout.units) {
				for (const auto &b : layout.units) {
					if (a.owner != b.owner) {
						closest = std::min(closest, std::hypot(a.ne - b.ne, a.se - b.se));
					}
				}
			}
			check(closest > 6.5, "armies out of sight (" + std::to_string(closest) + ")");
		}
	}
	std::cout << "skirmish layouts: " << placed << "/" << total << " placed\n";
	check(placed * 10 >= total * 9, "skirmish placed on at least 90 % of the maps");
}

} // namespace


int main(int argc, char **argv) {
	if (argc == 4 and std::string{argv[1]} == "--layout") {
		MapSettings s;
		s.type = map_type_t::RANDOM;
		s.seed = static_cast<uint32_t>(std::stoul(argv[2]));
		s.size = std::stoul(argv[3]);
		auto layout = skirmish_layout(generate_map(s));
		if (not layout.placed) {
			std::cerr << "no skirmish layout\n";
			return EXIT_FAILURE;
		}
		std::cout << "center " << layout.center_ne << "," << layout.center_se << "\n";
		for (const auto &u : layout.units) {
			if (u.owner == 1 and u.kind == skirmish_unit_t::KNIGHT) {
				// middle knight of the enemy front row (second knight)
				static int n = 0;
				if (++n == 2) {
					std::cout << "enemy_knight " << u.ne << "," << u.se << "\n";
				}
			}
		}
		return EXIT_SUCCESS;
	}

	damage_sweep();
	range_sweep();
	approach_sweep();
	victory_sweep();
	layout_sweep();
	std::cout << "combat rules check: " << checks << " checks, " << failures << " failures\n";
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
