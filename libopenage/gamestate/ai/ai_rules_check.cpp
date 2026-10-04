// Copyright 2026-2026 the openage authors. See copying.md for legal info.

/*
 * Host test of the AI decision rules (XR fork, dependency-free, parameter sweeps):
 *
 *   openage-ai-rules-check
 *
 * Prints "ai rules check: N checks, 0 failures" and exits with 0 if all pass.
 */

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>

#include "gamestate/ai/ai_rules.h"

using namespace openage::gamestate::ai;

namespace {

size_t checks = 0;
size_t failures = 0;

void check(bool ok, const std::string &what) {
	checks += 1;
	if (not ok) {
		failures += 1;
		if (failures <= 30) {
			std::cout << "  FAIL " << what << std::endl;
		}
	}
}

} // namespace


int main() {
	// ---- parameters per difficulty
	for (auto d : {difficulty_t::EASY, difficulty_t::NORMAL}) {
		auto p = params_for(d);
		std::string n = to_string(d);
		check(p.difficulty == d, n + ": difficulty kept");
		check(p.think_period > 0.0 and p.think_period <= 2.0, n + ": reaction time 0..2 s");
		check(std::abs(p.gather_split[0] + p.gather_split[1] + p.gather_split[2] - 1.0) < 1e-9, n + ": split sums to 1");
		check(p.attack_threshold >= 1, n + ": threshold >= 1");
		check(p.attack_anyway_after >= p.first_attack_earliest, n + ": timeout after earliest attack");
		check(p.villager_target == 15, n + ": 15 villagers");
	}
	auto easy = params_for(difficulty_t::EASY);
	auto normal = params_for(difficulty_t::NORMAL);
	check(easy.first_attack_earliest >= 480.0, "easy: first attack after 8 min at the earliest");
	check(normal.think_period < easy.think_period, "normal reacts faster");
	check(normal.attack_threshold <= easy.attack_threshold, "normal attacks with fewer units");
	check(normal.first_attack_earliest < easy.first_attack_earliest, "normal attacks earlier");

	// ---- gather split: sweep worker counts, the choice reduces the largest shortfall
	const std::array<double, GATHER_KINDS> split{0.4, 0.4, 0.2};
	for (size_t f = 0; f < 12; ++f) {
		for (size_t w = 0; w < 12; ++w) {
			for (size_t g = 0; g < 8; ++g) {
				for (int mask = 1; mask < 8; ++mask) {
					std::array<bool, GATHER_KINDS> avail{(mask & 1) != 0, (mask & 2) != 0, (mask & 4) != 0};
					std::array<size_t, GATHER_KINDS> workers{f, w, g};
					auto k = choose_gather(workers, split, avail);
					check(k.has_value(), "some kind available");
					if (not k) {
						continue;
					}
					check(avail[static_cast<size_t>(*k)], "chosen kind is available");
					double total = static_cast<double>(f + w + g + 1);
					double chosen = split[static_cast<size_t>(*k)] * total - workers[static_cast<size_t>(*k)];
					for (size_t o = 0; o < GATHER_KINDS; ++o) {
						if (avail[o]) {
							check(split[o] * total - workers[o] <= chosen + 1e-9, "largest shortfall chosen");
						}
					}
				}
			}
		}
	}
	check(not choose_gather({0, 0, 0}, split, {false, false, false}).has_value(), "nothing available");
	// assigning 20 villagers one by one approaches the split
	{
		std::array<size_t, GATHER_KINDS> workers{};
		for (int i = 0; i < 20; ++i) {
			auto k = choose_gather(workers, split, {true, true, true});
			workers[static_cast<size_t>(*k)] += 1;
		}
		check(workers[0] == 8 and workers[1] == 8 and workers[2] == 4, "20 villagers: 8 food, 8 wood, 4 gold");
		std::array<size_t, GATHER_KINDS> first{};
		first[static_cast<size_t>(*choose_gather({0, 0, 0}, split, {true, true, true}))] += 1;
		check(first[0] == 1, "first villager: food");
	}

	// ---- attack decision: sweep time, army, waves
	for (auto d : {difficulty_t::EASY, difficulty_t::NORMAL}) {
		auto p = params_for(d);
		for (double now = 0.0; now <= 1500.0; now += 7.5) {
			for (size_t army = 0; army <= 12; ++army) {
				for (size_t waves = 0; waves <= 2; ++waves) {
					for (double ago : {10.0, 59.0, 61.0, 130.0, 300.0}) {
						double last = now - ago;
						auto dec = attack_decision(now, army, waves, last, p);
						if (dec.attack) {
							check(army > 0, "attack needs units");
							check(now >= p.first_attack_earliest, "no attack before the earliest time");
							check(waves == 0 or ago >= p.wave_interval, "waves apart");
							check(army >= p.attack_threshold
							          or (waves == 0 ? now >= p.attack_anyway_after : ago >= 2.0 * p.wave_interval),
							      "threshold or timeout");
							double next = next_attack_time(now, army, waves, last, p);
							check(next <= now + 1e-9, "next attack time consistent (attack now)");
						}
						else {
							bool should = army > 0 and now >= p.first_attack_earliest
							              and (waves == 0 or ago >= p.wave_interval)
							              and (army >= p.attack_threshold
							                   or (waves == 0 ? now >= p.attack_anyway_after
							                                  : ago >= 2.0 * p.wave_interval));
							check(not should, "waits only for a reason");
							if (army > 0) {
								check(next_attack_time(now, army, waves, last, p) > now - 1e-9, "next attack later");
							}
						}
					}
				}
			}
		}
		check(attack_decision(p.first_attack_earliest, p.attack_threshold, 0, 0.0, p).reason == attack_reason_t::THRESHOLD,
		      "threshold at the earliest time");
		check(attack_decision(p.first_attack_earliest - 1.0, 50, 0, 0.0, p).reason == attack_reason_t::TOO_EARLY,
		      "big army waits for the earliest time");
		check(attack_decision(p.attack_anyway_after, 1, 0, 0.0, p).reason == attack_reason_t::TIMEOUT,
		      "single unit after the timeout");
	}

	// ---- objective and unit targets
	{
		std::vector<TargetCandidate> c{
			{1, 3.0, false, false, true},
			{2, 9.0, true, true, false},
			{3, 2.0, true, false, false},
			{4, 12.0, true, true, false},
			{5, 2.5, false, false, false},
		};
		auto o = choose_objective(c);
		check(o and c[*o].id == 2, "objective: nearest town center");
		c.erase(c.begin() + 1);
		c.erase(c.begin() + 2);
		o = choose_objective(c);
		check(o and c[*o].id == 3, "objective without town center: nearest enemy");
		check(not choose_objective({}).has_value(), "no objective without enemies");

		std::vector<TargetCandidate> u{
			{10, 3.0, false, false, true},
			{11, 1.5, false, false, false},
			{12, 0.5, true, false, false},
			{13, 7.0, false, false, true},
		};
		auto t = choose_unit_target(u, 6.0);
		check(t and u[*t].id == 10, "unit target: military before a closer villager (+2)");
		t = choose_unit_target(u, 2.0);
		check(t and u[*t].id == 11, "unit target: villager if no military in reach");
		t = choose_unit_target(u, 0.4);
		check(not t, "nothing in reach (buildings ignored)");
		// sweep: the target is always in reach and never a building
		Rng rng{7};
		for (int run = 0; run < 2000; ++run) {
			std::vector<TargetCandidate> r;
			for (int i = 0; i < 8; ++i) {
				r.push_back({static_cast<uint64_t>(i), rng.uniform() * 15.0, rng.uniform() < 0.3, false, rng.uniform() < 0.5});
			}
			double reach = rng.uniform() * 10.0;
			auto pick = choose_unit_target(r, reach);
			bool any = false;
			for (const auto &x : r) {
				any = any or (not x.building and x.distance <= reach);
			}
			check(pick.has_value() == any, "unit target exists iff a unit is in reach");
			if (pick) {
				check(not r[*pick].building and r[*pick].distance <= reach, "unit target in reach, no building");
			}
		}
	}

	// ---- population and production plan
	check(estimated_population_cap(1, 0) == 5 and estimated_population_cap(1, 3) == 20
	          and estimated_population_cap(2, 100) == 200,
	      "population cap 5 per town center/house, max 200");
	for (size_t villagers = 0; villagers <= 20; ++villagers) {
		for (size_t houses = 0; houses <= 4; ++houses) {
			for (size_t barracks = 0; barracks <= 1; ++barracks) {
				for (double wood : {0.0, 30.0, 200.0}) {
					for (double food : {0.0, 55.0, 200.0}) {
						EconomyView e;
						e.villagers = villagers;
						e.town_centers = 1;
						e.houses = houses;
						e.barracks = barracks;
						e.population = villagers;
						e.population_cap = estimated_population_cap(1, houses);
						e.stock = {food, wood, 100.0, 200.0};
						auto plan = plan_production(e, easy);
						std::set<plan_t> seen(plan.begin(), plan.end());
						check(seen.size() == plan.size(), "each wish once");
						std::array<double, 4> spent{};
						for (auto w : plan) {
							const auto &cost = w == plan_t::TRAIN_VILLAGER ? COST_VILLAGER
							                 : w == plan_t::TRAIN_MILITIA  ? COST_MILITIA
							                 : w == plan_t::BUILD_HOUSE    ? COST_HOUSE
							                                               : COST_BARRACKS;
							for (size_t i = 0; i < 4; ++i) {
								spent[i] += cost[i];
							}
						}
						check(affordable(e.stock, spent), "plan is affordable as a whole");
						if (seen.contains(plan_t::TRAIN_VILLAGER)) {
							check(villagers < 15 and e.population < e.population_cap, "villagers below 15 with room");
						}
						if (seen.contains(plan_t::TRAIN_MILITIA)) {
							check(barracks > 0 and e.population < e.population_cap, "militia needs barracks and room");
						}
						if (seen.contains(plan_t::BUILD_BARRACKS)) {
							check(barracks == 0 and villagers >= easy.barracks_after_villagers, "one barracks after 8 villagers");
						}
						if (seen.contains(plan_t::BUILD_HOUSE)) {
							check(e.population + easy.house_margin >= e.population_cap, "house only near the limit");
						}
						if (villagers > 0 and e.population + easy.house_margin >= e.population_cap and wood >= 30.0) {
							check(seen.contains(plan_t::BUILD_HOUSE), "house at the limit with wood");
						}
						if (villagers < 15 and e.population < e.population_cap and food >= 50.0) {
							check(seen.contains(plan_t::TRAIN_VILLAGER), "villager with food and room");
						}
					}
				}
			}
		}
	}

	// ---- building spots: deterministic, inside the ring, no duplicates
	for (uint64_t seed = 1; seed <= 50; ++seed) {
		Rng a{seed}, b{seed};
		auto s1 = building_spots(20.0, 30.0, 5.0, 10.0, 2.0, a);
		auto s2 = building_spots(20.0, 30.0, 5.0, 10.0, 2.0, b);
		check(s1 == s2, "same seed, same spots");
		check(s1.size() >= 30, "enough spots");
		std::set<std::pair<double, double>> uniq(s1.begin(), s1.end());
		check(uniq.size() == s1.size(), "no duplicate spots");
		for (const auto &[x, y] : s1) {
			double r = std::hypot(x - 20.0, y - 30.0);
			check(r >= 3.5 and r <= 11.5, "spot inside the ring");
			check(std::abs(x - std::floor(x)) < 1e-9, "even side: tile corner");
		}
		auto odd = building_spots(20.0, 30.0, 5.0, 6.0, 3.0, a);
		for (const auto &[x, y] : odd) {
			check(std::abs(x - std::floor(x) - 0.5) < 1e-9, "odd side: tile centre");
		}
	}

	// ---- log throttle
	{
		LogThrottle t{30.0};
		size_t due = 0;
		for (double now = 0.0; now < 300.0; now += 1.0) {
			due += t.due(now) ? 1 : 0;
		}
		check(due == 10, "throttle: once per 30 s over 300 s");
		check(t.take_suppressed() > 0 and t.take_suppressed() == 0, "suppressed counter resets");
	}

	// ---- rng
	{
		Rng a{42}, b{42}, c{43};
		bool same = true, differ = false;
		for (int i = 0; i < 100; ++i) {
			auto x = a.next();
			same = same and x == b.next();
			differ = differ or x != c.next();
			double u = a.uniform();
			b.uniform();
			c.uniform();
			check(u >= 0.0 and u < 1.0, "uniform in [0, 1)");
		}
		check(same and differ, "rng deterministic per seed");
	}

	std::cout << "ai rules check: " << checks << " checks, " << failures << " failures" << std::endl;
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
