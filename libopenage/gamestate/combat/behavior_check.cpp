// Copyright 2026-2026 the openage authors. See copying.md for legal info.

/*
 * Host test of the combat behaviour of round W4 (XR fork, gamestate/combat/behavior.h):
 *
 *   openage-combat-behavior-check
 *
 * Parameter sweeps:
 * - damage with factor: factor 1 = compute_damage(), monotonic in the factor,
 *   high ground 8 -> 10, low ground 8 -> 6, minimum 1
 * - elevation factor: thresholds, ridge on the line, symmetric
 * - flights: duration = distance / speed, start/end points exact, parabola
 *   peak in the middle, arrows flat, never below the straight line
 * - hit roll, miss points inside the square of side dispersion x distance
 * - lead: intercept point reached at the same time by target and projectile
 * - stances: allowed reach R+5 / R+0.5 / never / always, leash 5 / 0.5
 * - target choice: nearest first, threat memory weights, current target bonus,
 *   best three, separate normalisation of movable and fixed candidates
 * - garrison: mask bits, arrow contributions (villager 1 arrow at the town
 *   center), arrow count min/max, spawning area inside the rectangle, healing
 *   with carried fractions
 * - town bell: ring distribution nearest first, capacity, overflow stays,
 *   deterministic for shuffled input
 * - flight of villagers: away from attackers, 2 x speed, leash
 * - formations: one slot per member, distinct slots, front row highest
 *   priority, centred, group speed = slowest
 *
 * Exit code 0 if all checks pass. No dependencies besides the header.
 */

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <string>

#include "gamestate/combat/behavior.h"

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
const std::string FALLBACK = "engine.util.attribute_change_type.type.Fallback";

void damage_sweep() {
	for (int64_t attack = 0; attack <= 25; ++attack) {
		for (int64_t armor = 0; armor <= 25; ++armor) {
			AttackProfile a{{{MELEE, attack, 0}, {FALLBACK, 1, 1}}};
			ArmorProfile r;
			r.block[MELEE] = armor;
			check(compute_damage(a, r, 1.0) == compute_damage(a, r), "factor 1 = plain damage");
			int64_t last = 0;
			for (double f : {0.5, 0.75, 1.0, 1.25, 2.0}) {
				auto d = compute_damage(a, r, f);
				check(d >= 1, "damage >= 1");
				check(d >= last, "damage grows with the factor");
				last = d;
			}
		}
	}
	AttackProfile knight{{{MELEE, 10, 0}, {FALLBACK, 1, 1}}};
	ArmorProfile armor;
	armor.block[MELEE] = 2;
	check(compute_damage(knight, armor, ELEVATION_HIGH) == 10, "high ground: 8 x 1.25 = 10");
	check(compute_damage(knight, armor, ELEVATION_LOW) == 6, "low ground: 8 x 0.75 = 6");
	check(compute_damage(knight, armor, 0.0) == 8, "invalid factor counts as 1");
	// classes without armor entry still do nothing
	AttackProfile two{{{MELEE, 10, 0}, {PIERCE, 6, 0}}};
	check(compute_damage(two, armor, 1.25) == 10, "class without armor entry ignored with factor");
}

void elevation_sweep() {
	for (double ha = 0.0; ha <= 4.0; ha += 0.25) {
		for (double ht = 0.0; ht <= 4.0; ht += 0.25) {
			const double f = elevation_factor(ha, ht, std::max(ha, ht) == ha ? ha : ht);
			const double diff = ha - ht;
			if (diff >= ELEVATION_STEP) {
				check(f == ELEVATION_HIGH, "higher attacker x1.25");
			}
			else if (diff <= -ELEVATION_STEP) {
				check(f == ELEVATION_LOW, "lower attacker x0.75");
			}
			else {
				check(f == 1.0, "about the same height x1");
			}
			// swapping attacker and target swaps high and low
			const double g = elevation_factor(ht, ha, std::max(ha, ht) == ht ? ht : ha);
			check((f == ELEVATION_HIGH) == (g == ELEVATION_LOW), "high/low symmetric");
		}
	}
	check(elevation_factor(2.0, 0.0, 3.0) == 1.0, "ridge above the attacker: no bonus");
	check(terrain_factor(std::nullopt) == 1.0 and terrain_factor(0.0) == 1.0 and terrain_factor(1.5) == 1.5,
	      "terrain factor: none / 0 = 1");
}

void flight_sweep() {
	for (double d = 0.5; d <= 12.0; d += 0.5) {
		for (double speed : {3.0, 5.5, 7.0}) {
			for (double arc : {-3.0, 0.0, 2.0, 22.0}) {
				Point3 a{10.0, 10.0, 1.5};
				Point3 b{10.0 + d * 0.6, 10.0 + d * 0.8, 0.3};
				auto f = plan_flight(a, b, speed, arc);
				check(std::abs(f.duration - std::max(MIN_FLIGHT, d / speed)) < 1e-9, "flight time = distance / speed");
				auto p0 = flight_at(f, 0.0);
				auto p1 = flight_at(f, f.duration);
				check(std::abs(p0.ne - a.ne) < 1e-9 and std::abs(p0.up - a.up) < 1e-9, "starts at the shooter");
				check(std::abs(p1.ne - b.ne) < 1e-9 and std::abs(p1.se - b.se) < 1e-9 and std::abs(p1.up - b.up) < 1e-9,
				      "ends at the aim point");
				auto mid = flight_at(f, f.duration / 2.0);
				const double straight = (a.up + b.up) / 2.0;
				check(mid.up >= straight - 1e-9, "never below the straight line");
				if (arc <= 0.0) {
					check(f.peak == 0.0, "arrows (arc <= 0) fly straight");
				}
				else {
					check(std::abs(mid.up - straight - f.peak) < 1e-9 and f.peak > 0.0, "peak in the middle");
				}
				check(flight_at(f, -1.0).ne == p0.ne and flight_at(f, 2.0 * f.duration).ne == p1.ne, "clamped");
			}
		}
	}
	check(plan_flight({0, 0, 0}, {0, 0, 0}, 7.0, 0.0).duration == MIN_FLIGHT, "zero distance: minimum flight");
	check(plan_flight({0, 0, 0}, {5, 0, 0}, 0.0, 0.0).duration == MIN_FLIGHT, "no speed: minimum flight");
}

void accuracy_sweep() {
	for (int acc = 0; acc <= 100; acc += 5) {
		int hits = 0;
		for (int z = 0; z <= 100; ++z) {
			hits += roll_hit(z, acc) ? 1 : 0;
		}
		check(hits == acc + 1, "hit if z <= accuracy");
	}
	std::mt19937_64 rng{7};
	std::uniform_real_distribution<double> u{0.0, 1.0};
	for (double disp : {0.0, 0.33, 0.5}) {
		for (double d = 1.0; d <= 10.0; d += 1.0) {
			for (int i = 0; i < 50; ++i) {
				Point aim{20.0, 30.0};
				auto m = miss_point(aim, disp, d, u(rng), u(rng));
				const double s = disp * d;
				check(std::abs(m.x - aim.x) <= s / 2 + 1e-9 and std::abs(m.y - aim.y) <= s / 2 + 1e-9,
				      "miss inside the square of side dispersion x distance");
			}
		}
	}
	// lead: target and projectile arrive at the same time
	for (double vx : {-1.5, 0.0, 0.8, 1.4}) {
		for (double vy : {-1.0, 0.5, 1.2}) {
			Point shooter{0.0, 0.0}, target{4.0, 3.0}, v{vx, vy};
			auto p = lead_point(shooter, target, v, 7.0);
			const double t_proj = distance(shooter, p) / 7.0;
			Point there{target.x + vx * t_proj, target.y + vy * t_proj};
			check(distance(there, p) < 1e-6, "lead point is the intercept");
		}
	}
	Point still = lead_point({0, 0}, {4, 3}, {0, 0}, 7.0);
	check(distance(still, {4, 3}) < 1e-9, "standing target: no lead");
	check(aim_height(1.0, 1.5) == 1.5, "aim 1/3 of the target height");
	// a missed arrow touches an object on its way
	Footprint unit{{5.0, 0.0}, 0.3, false};
	auto touch = segment_touches({0.0, 0.0}, {10.0, 0.0}, unit);
	check(touch and *touch > 0.4 and *touch < 0.55, "missed shot touches the unit on its line");
	check(not segment_touches({0.0, 2.0}, {10.0, 2.0}, unit), "line beside the unit: no contact");
}

void stance_sweep() {
	for (double range : {0.0, MELEE_RANGE, 4.0, 7.0}) {
		const double r = range > 0.0 ? range : MELEE_RANGE;
		for (double d = 0.0; d <= 20.0; d += 0.25) {
			check(stance_allows(stance_t::AGGRESSIVE, d, r), "aggressive: always");
			check(not stance_allows(stance_t::NO_ATTACK, d, r), "no attack: never");
			check(stance_allows(stance_t::DEFENSIVE, d, r) == (d <= r + 5.0 + RANGE_EPSILON), "defensive: R + 5");
			check(stance_allows(stance_t::STAND_GROUND, d, r) == (d <= r + 0.5 + RANGE_EPSILON), "stand ground: R + 0.5");
			check(stance_allows(stance_t::STAND_GROUND, d, r, true) == (d <= r + 2.0 + RANGE_EPSILON),
			      "stand ground vs animals: R + 2");
		}
	}
	for (double d = 0.0; d <= 10.0; d += 0.1) {
		check(stance_returns(stance_t::DEFENSIVE, d) == (d > 5.0), "defensive back beyond 5");
		check(stance_returns(stance_t::STAND_GROUND, d) == (d > 0.5), "stand ground back beyond 0.5");
		check(not stance_returns(stance_t::AGGRESSIVE, d) and not stance_returns(stance_t::NO_ATTACK, d), "others stay");
	}
	check(stance_moves(stance_t::AGGRESSIVE) and stance_moves(stance_t::DEFENSIVE)
	          and not stance_moves(stance_t::STAND_GROUND) and not stance_moves(stance_t::NO_ATTACK),
	      "only aggressive and defensive chase");
	check(scan_radius(4.0, 7.0) == 7.0 and scan_radius(6.0, 4.0) == 7.0, "surroundings max(sight + 1, range)");
	for (size_t i = 0; i < STANCE_COUNT; ++i) {
		auto s = static_cast<stance_t>(i);
		check(stance_from_string(to_string(s)) == s, "stance names round trip");
	}
	for (auto o : {order_t::NONE, order_t::ATTACK, order_t::PATROL, order_t::GUARD, order_t::FOLLOW, order_t::ATTACK_MOVE}) {
		check(order_from_string(to_string(o)) == o, "order names round trip");
	}
	for (auto f : {formation_t::NONE, formation_t::LINE, formation_t::BOX, formation_t::STAGGERED, formation_t::FLANK}) {
		check(formation_from_string(to_string(f)) == f, "formation names round trip");
	}
}

void target_sweep() {
	std::array<double, CATEGORY_COUNT> none{};
	// nearest wins with equal kill times
	for (int n = 2; n <= 8; ++n) {
		std::vector<Candidate> c;
		for (int i = 0; i < n; ++i) {
			c.push_back({static_cast<uint64_t>(100 + i), 1.0 + (n - i), 5.0, category_t::INFANTRY, false, true});
		}
		auto best = best_targets(c, none);
		check(best.size() == std::min<size_t>(3, c.size()), "at most three");
		check(c[best[0]].id == static_cast<uint64_t>(100 + n - 1), "nearest first");
		auto scores = score_targets(c, none);
		for (size_t i = 0; i < scores.size(); ++i) {
			check(scores[i] >= 0.0 and scores[i] <= SCORE_DISTANCE + SCORE_KILL + SCORE_THREAT + SCORE_CURRENT + 1e-9,
			      "score within 0..100");
		}
	}
	// distance range 75 points, kill time 10: a slightly farther, much weaker target loses
	std::vector<Candidate> c{{1, 2.0, 30.0, category_t::INFANTRY, false, true},
	                         {2, 2.2, 1.0, category_t::INFANTRY, false, true},
	                         {3, 6.0, 1.0, category_t::INFANTRY, false, true}};
	auto best = best_targets(c, none);
	check(c[best[0]].id == 2, "almost as near and quick to kill wins");
	// threat memory: archers attacked recently make archers more attractive
	ThreatMemory memory;
	for (int i = 0; i < 5; ++i) {
		memory.add(100.0 + i, category_t::ARCHER);
	}
	memory.add(70.0, category_t::CAVALRY);
	auto w = memory.normalized(110.0);
	check(w[static_cast<size_t>(category_t::ARCHER)] == 1.0, "strongest threat normalised to 1");
	check(w[static_cast<size_t>(category_t::CAVALRY)] > 0.0 and w[static_cast<size_t>(category_t::CAVALRY)] < 0.2,
	      "older threat counts less");
	check(memory.weight(130.0, category_t::CAVALRY) == 0.0, "after 60 s forgotten");
	memory.prune(130.0);
	check(memory.size() == 5, "pruned the old report");
	std::vector<Candidate> mixed{{1, 3.0, 5.0, category_t::INFANTRY, false, true},
	                             {2, 3.0, 5.0, category_t::ARCHER, false, true}};
	check(mixed[best_targets(mixed, w)[0]].id == 2, "threat memory decides between equal targets");
	// current target bonus
	std::vector<Candidate> cur{{1, 3.0, 5.0, category_t::INFANTRY, false, true},
	                           {2, 3.0, 5.0, category_t::INFANTRY, true, true}};
	check(cur[best_targets(cur, none)[0]].id == 2, "current target +5");
	// fixed objects are normalised on their own
	std::vector<Candidate> fixed{{1, 1.0, 5.0, category_t::INFANTRY, false, true},
	                             {2, 8.0, 5.0, category_t::BUILDING, false, false}};
	auto fs = score_targets(fixed, none);
	check(fs[0] == fs[1], "single movable and single fixed candidate both full points");
	check(kill_time(100.0, 10.0, 2.0) == 20.0 and kill_time(100.0, 0.0, 2.0) == 200.0, "kill time");
	check(category_of(0, false, false) == category_t::ARCHER and category_of(6, false, false) == category_t::INFANTRY
	          and category_of(12, false, false) == category_t::CAVALRY and category_of(3, true, false) == category_t::BUILDING
	          and category_of(4, false, true) == category_t::CIVILIAN,
	      "categories from unit classes");
}

void garrison_sweep() {
	// town center 11 = villager + infantry + monk; castle 15 = all
	check(garrison_accepts(11, garrison_kind_t::VILLAGER) and garrison_accepts(11, garrison_kind_t::INFANTRY)
	          and garrison_accepts(11, garrison_kind_t::MONK) and not garrison_accepts(11, garrison_kind_t::CAVALRY),
	      "mask 11");
	for (auto k : {garrison_kind_t::VILLAGER, garrison_kind_t::INFANTRY, garrison_kind_t::CAVALRY, garrison_kind_t::MONK}) {
		check(garrison_accepts(15, k), "mask 15 takes all");
		check(not garrison_accepts(0, k), "mask 0 takes none");
	}
	check(not garrison_accepts(15, garrison_kind_t::NONE), "siege and ships never");
	check(garrison_kind(4, false) == garrison_kind_t::VILLAGER and garrison_kind(0, false) == garrison_kind_t::INFANTRY
	          and garrison_kind(12, false) == garrison_kind_t::CAVALRY and garrison_kind(18, false) == garrison_kind_t::MONK
	          and garrison_kind(13, false) == garrison_kind_t::NONE,
	      "garrison kinds");
	// contributions: town center arrow 5 / 2 = 2.5 -> a villager brings exactly one arrow
	const double tc_dps = 5.0 / 2.0;
	check(arrow_contribution(4, true, 0.0, tc_dps, false) == 1.0, "villager = 1 arrow at the town center");
	check(std::abs(arrow_contribution(0, false, 4.0 / 2.0, tc_dps, false) - 0.8) < 1e-9, "archer 4/2 / 2.5 = 0.8");
	check(arrow_contribution(6, false, 4.0, tc_dps, false) == 0.0, "infantry without crenellations 0");
	check(arrow_contribution(6, false, 4.0, tc_dps, true) == 1.0, "infantry with crenellations 2.5");
	check(arrow_contribution(12, false, 5.0, tc_dps, false) == 0.0 and arrow_contribution(18, false, 0.0, tc_dps, false) == 0.0,
	      "cavalry and monks nothing");
	check(arrow_contribution(4, true, 0.0, 0.0, false) == 0.0, "building without arrow");
	for (int villagers = 0; villagers <= 15; ++villagers) {
		const double running = 1.0 + villagers * arrow_contribution(4, true, 0.0, tc_dps, false);
		check(arrow_count(running, 11) == std::min(11, 1 + villagers), "town center 1 + villagers, at most 11");
	}
	check(arrow_count(0.0, 10) == 0 and arrow_count(0.99, 10) == 0 and arrow_count(4.8, 10) == 4, "whole part");
	std::mt19937_64 rng{3};
	std::uniform_real_distribution<double> u{0.0, 1.0};
	for (int i = 0; i < 200; ++i) {
		Point aim{10.0, 10.0};
		const double angle = u(rng) * 6.283;
		auto p = area_point(aim, std::cos(angle), std::sin(angle), 2.0, 0.5, u(rng), u(rng));
		const double along = (p.x - aim.x) * std::cos(angle) + (p.y - aim.y) * std::sin(angle);
		const double across = -(p.x - aim.x) * std::sin(angle) + (p.y - aim.y) * std::cos(angle);
		check(std::abs(along) <= 0.25 + 1e-9 and std::abs(across) <= 1.0 + 1e-9, "area point inside width x length");
	}
	// healing: 0.1 HP/s for 30 s in 0.5 s steps = 3 HP
	for (double rate : {0.1, 0.2, 0.5, 1.5}) {
		double carry = 0.0;
		int64_t healed = 0;
		for (int i = 0; i < 60; ++i) {
			healed += garrison_heal(rate, 0.5, carry, 1000 - healed);
		}
		check(healed == static_cast<int64_t>(std::floor(rate * 30.0 + 1e-9)), "healing rate x time");
		double c2 = 0.0;
		check(garrison_heal(rate, 100.0, c2, 3) == 3, "never more than missing");
	}
}

void bell_sweep() {
	std::mt19937_64 rng{11};
	std::uniform_real_distribution<double> u{-24.0, 24.0};
	for (int run = 0; run < 40; ++run) {
		std::vector<BellVillager> villagers;
		const int n = 5 + run % 20;
		for (int i = 0; i < n; ++i) {
			villagers.push_back({static_cast<uint64_t>(1000 + i), {50.0 + u(rng), 50.0 + u(rng)}});
		}
		std::vector<BellShelter> shelters{{1, {50.0, 50.0}, 15}, {2, {60.0, 45.0}, 5}};
		auto a = assign_bell(villagers, shelters);
		std::map<uint64_t, int> per;
		std::set<uint64_t> placed;
		for (const auto &[v, s] : a) {
			per[s] += 1;
			placed.insert(v);
		}
		check(per[1] <= 15 and per[2] <= 5, "capacity respected");
		// whoever stays has no shelter with a free place within 25 tiles (last ring 625)
		bool rest_far = true;
		for (const auto &v : villagers) {
			if (placed.contains(v.id)) {
				continue;
			}
			for (const auto &s : shelters) {
				const int left = s.free - per[s.id];
				const double dx = v.pos.x - s.pos.x, dy = v.pos.y - s.pos.y;
				if (left > 0 and dx * dx + dy * dy <= BELL_RING_MAX) {
					rest_far = false;
				}
			}
		}
		check(rest_far, "everyone placed while there is room within the rings");
		check(a.size() <= std::min<size_t>(villagers.size(), 20), "not more than the places");
		check(placed.size() == a.size(), "nobody twice");
		// deterministic for shuffled input
		auto shuffled = villagers;
		std::shuffle(shuffled.begin(), shuffled.end(), rng);
		auto b = assign_bell(shuffled, shelters);
		std::sort(a.begin(), a.end());
		std::sort(b.begin(), b.end());
		check(a == b, "independent of the order");
	}
	// nearest first: one place, two villagers -> the nearer one
	std::vector<BellVillager> two{{1, {10.0, 10.0}}, {2, {1.0, 1.0}}};
	auto one = assign_bell(two, {{9, {0.0, 0.0}, 1}});
	check(one.size() == 1 and one[0].first == 2, "the nearest villager gets the last place");
	// far beyond 25 tiles (squared distance > 625): stays
	auto far = assign_bell({{1, {30.0, 0.0}}}, {{9, {0.0, 0.0}, 5}});
	check(far.empty(), "outside the last ring: stays");
	check(in_bell_area({0, 0}, {25, -25}) and not in_bell_area({0, 0}, {25.5, 0}), "bell area +-25");
}

void flee_sweep() {
	for (double a = 0.0; a < 6.28; a += 0.3) {
		Point self{20.0, 20.0};
		Point attacker{20.0 + 3.0 * std::cos(a), 20.0 + 3.0 * std::sin(a)};
		auto p = flee_point(self, {attacker}, 0.8, 0.0);
		check(std::abs(distance(p, self) - 1.6) < 1e-9, "flees 2 x speed");
		check(distance(p, attacker) > distance(self, attacker), "away from the attacker");
		auto q = flee_point(self, {attacker}, 10.0, 0.0, Point{20.0, 20.0});
		check(distance(q, self) <= DEFENSIVE_LEASH + 1e-9, "defensive: at most 5 from the post");
	}
	auto r = flee_point({0, 0}, {}, 1.0, 0.0);
	check(std::abs(distance(r, {0, 0}) - 2.0) < 1e-9, "no attacker: random direction");
	// two attackers from opposite sides cancel: random direction
	auto s = flee_point({0, 0}, {{1, 0}, {-1, 0}}, 1.0, 1.0);
	check(std::abs(distance(s, {0, 0}) - 2.0) < 1e-9, "opposite attackers: random direction");
}

void formation_sweep() {
	for (auto f : {formation_t::LINE, formation_t::BOX, formation_t::STAGGERED, formation_t::FLANK}) {
		for (size_t n = 1; n <= 40; ++n) {
			std::vector<FormationMember> m;
			for (size_t i = 0; i < n; ++i) {
				m.push_back({i + 1, static_cast<int>(1 + i % 5), {0.0, 0.0}});
			}
			Point center{30.0, 30.0};
			Point dir{1.0, 0.0};
			auto slots = formation_slots(f, m, center, dir);
			check(slots.size() == n, "one slot per member");
			std::set<uint64_t> ids;
			double cy = 0.0;
			double min_gap = 1e9;
			for (size_t i = 0; i < slots.size(); ++i) {
				ids.insert(slots[i].first);
				cy += slots[i].second.y;
				for (size_t j = i + 1; j < slots.size(); ++j) {
					min_gap = std::min(min_gap, distance(slots[i].second, slots[j].second));
				}
				check(distance(slots[i].second, center) < 4.0 + static_cast<double>(n), "slots near the centre");
			}
			check(ids.size() == n, "every member once");
			if (n > 1) {
				check(min_gap >= 0.5, "slots apart");
			}
			cy /= static_cast<double>(n);
			if (f == formation_t::LINE or f == formation_t::FLANK) {
				check(std::abs(cy - center.y) < 1.0, "line centred across");
			}
			if (f == formation_t::LINE and n >= 4) {
				// the front row (towards dir = +x) has the highest priority
				double front_x = -1e9;
				int front_prio = -1;
				for (const auto &[id, p] : slots) {
					if (p.x > front_x + 1e-9) {
						front_x = p.x;
						front_prio = m[id - 1].priority;
					}
				}
				int top = 0;
				for (const auto &mm : m) {
					top = std::max(top, mm.priority);
				}
				check(front_prio == top, "front row = highest priority");
			}
		}
	}
	check(group_speed({0.96, 1.35, 0.9}) == 0.9, "group speed = slowest");
	check(group_speed({}) == 0.0, "no speeds");
}

} // namespace


int main() {
	damage_sweep();
	elevation_sweep();
	flight_sweep();
	accuracy_sweep();
	stance_sweep();
	target_sweep();
	garrison_sweep();
	bell_sweep();
	flee_sweep();
	formation_sweep();
	std::cout << checks << " checks, " << failures << " failures" << std::endl;
	std::cout << (failures == 0 ? "combat behavior check ok" : "combat behavior check FAILED") << std::endl;
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
