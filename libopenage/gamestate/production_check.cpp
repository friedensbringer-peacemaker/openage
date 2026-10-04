// Copyright 2026-2026 the openage authors. See copying.md for legal info.

/*
 * Host test of the production rules (XR fork).
 *
 *   openage-prod-check
 *
 * Parameter sweeps over the dependency-free parts of production and construction:
 * - offered units and buildings: codes unique, lookup by name and code, German labels
 * - footprints: side per hitbox radius, snapped anchors, tile count = side^2 for every
 *   click position, footprint contains the clicked tile's neighbourhood
 * - placement check: priority outside > water > blocked > occupied, sweep over a
 *   footprint with one bad tile at every position
 * - costs: missing resource and message
 * - training queue: max 5, start/finish times for chains, population slots (waits at
 *   the limit, continues when housing is free), cancel/refund, progress
 * - population limit and construction progress (one and several builders)
 * - scenario of the desktop check: town centre trains 2 villagers (food -100,
 *   50 s), a house adds 5 population after 25 s of work
 * Exit code 0 if all checks pass. No engine dependencies.
 */

#include <cmath>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "gamestate/production_math.h"
#include "gamestate/resources.h"

using namespace openage::gamestate;
using namespace openage::gamestate::prod;

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

bool near(double a, double b, double eps = 1e-9) {
	return std::abs(a - b) <= eps;
}

void check_known() {
	std::set<int> codes;
	std::set<std::string> names;
	for (const auto &entry : KNOWN) {
		check(codes.insert(entry.code).second, std::string{"unique code "} + entry.name);
		check(names.insert(entry.name).second, std::string{"unique name "} + entry.name);
		check(known(entry.name) == &entry, std::string{"lookup by name "} + entry.name);
		check(known(entry.code) == &entry, std::string{"lookup by code "} + entry.name);
		check(entry.building == (entry.code >= 200), std::string{"code range "} + entry.name);
		check(std::string{entry.label}.size() > 2, std::string{"label "} + entry.name);
	}
	check(known("EagleWarrior") == nullptr, "eagle warrior not offered");
	check(known(999) == nullptr, "unknown code");
	check(label_of("Villager") == "Dorfbewohner", "label villager");
	check(label_of("TownCenter") == "Dorfzentrum", "label town centre");
	check(label_of("SiegeWorkshop") == "Siege Workshop", "camel case split");
	check(short_name("hd_base.data.game_entity.generic.house.house.House") == "House", "short name");
	check(display_order("Villager") < display_order("Militia"), "order units");
	check(display_order("House") < display_order("Barracks"), "order buildings");
	check(display_order("Wonder") == KNOWN.size(), "unknown last");
}

void check_footprints() {
	check(footprint_side(0.5) == 1 and footprint_side(1.0) == 2 and footprint_side(1.5) == 3
	          and footprint_side(2.0) == 4 and footprint_side(0.0) == 1,
	      "footprint sides");
	for (double radius : {0.5, 1.0, 1.5, 2.0, 2.5}) {
		const long side = footprint_side(radius);
		for (int i = 0; i <= 40; ++i) {
			for (int j = 0; j <= 40; ++j) {
				double ne = 10.0 + i * 0.05;
				double se = 7.0 + j * 0.05;
				auto [ane, ase] = snap_anchor(ne, se, radius);
				auto tiles = building_tiles(ane, ase, radius);
				std::string what = "radius " + std::to_string(radius) + " click " + std::to_string(ne) + "," + std::to_string(se);
				check(static_cast<long>(tiles.size()) == side * side, "tile count " + what);
				// anchor within half a tile of the click
				check(std::abs(ane - ne) <= 0.5 + 1e-9 and std::abs(ase - se) <= 0.5 + 1e-9, "anchor near click " + what);
				// tiles form a square around the anchor
				long x0 = tiles.front().ne, x1 = x0, y0 = tiles.front().se, y1 = y0;
				for (const auto &t : tiles) {
					x0 = std::min(x0, t.ne);
					x1 = std::max(x1, t.ne);
					y0 = std::min(y0, t.se);
					y1 = std::max(y1, t.se);
				}
				check(x1 - x0 + 1 == side and y1 - y0 + 1 == side, "square " + what);
				check(near(0.5 * (x0 + x1 + 1), ane) and near(0.5 * (y0 + y1 + 1), ase), "centred " + what);
				// the reach test of the economy covers the ring around the footprint
				check(econ::in_reach(x0 - 0.5, y0 - 0.5, ane, ase, radius), "reach corner " + what);
				check(not econ::in_reach(x0 - 1.5, y0 + 0.5, ane, ase, radius), "no reach two tiles away " + what);
			}
		}
	}
}

void check_placement_rules() {
	auto tiles = building_tiles(10.5, 10.5, 1.5);
	check(tiles.size() == 9, "3x3 footprint");
	check(check_placement(tiles, [](const econ::tile_pos &) { return tile_state_t::FREE; }) == placement_t::OK,
	      "all free");
	const tile_state_t states[] = {tile_state_t::OUTSIDE, tile_state_t::WATER, tile_state_t::BLOCKED, tile_state_t::OCCUPIED};
	const placement_t results[] = {placement_t::OUTSIDE, placement_t::WATER, placement_t::BLOCKED, placement_t::OCCUPIED};
	for (size_t s = 0; s < 4; ++s) {
		for (size_t bad = 0; bad < tiles.size(); ++bad) {
			auto result = check_placement(tiles, [&](const econ::tile_pos &t) {
				return t == tiles[bad] ? states[s] : tile_state_t::FREE;
			});
			check(result == results[s], "single bad tile " + std::to_string(s) + "/" + std::to_string(bad));
		}
	}
	// priority: outside > water > blocked > occupied, independent of the tile order
	for (size_t a = 0; a < 4; ++a) {
		for (size_t b = 0; b < 4; ++b) {
			auto result = check_placement(tiles, [&](const econ::tile_pos &t) {
				if (t == tiles[0]) {
					return states[a];
				}
				if (t == tiles[8]) {
					return states[b];
				}
				return tile_state_t::FREE;
			});
			check(result == results[std::min(a, b)], "priority " + std::to_string(a) + "," + std::to_string(b));
		}
	}
	check(std::string{placement_message(placement_t::WATER)}.find("Wasser") != std::string::npos, "water message");
	check(std::string{placement_message(placement_t::OK)}.empty(), "ok message empty");
}

void check_costs() {
	resource_amounts_t stock{200, 200, 100, 200};
	check(not missing_resource(stock, {50, 0, 0, 0}), "villager affordable");
	check(missing_resource(stock, {0, 0, 101, 0}) == resource_t::GOLD, "gold missing");
	check(missing_resource({0, 10, 0, 0}, {0, 30, 0, 0}) == resource_t::WOOD, "wood missing");
	check(missing_message(resource_t::WOOD) == "Nicht genug Holz", "message wood");
	check(missing_message(resource_t::FOOD) == "Nicht genug Nahrung", "message food");
	check(not missing_resource(stock, stock), "exact amount affordable");
}

TrainQueue::Item villager() {
	TrainQueue::Item item;
	item.name = "Villager";
	item.cost = {50, 0, 0, 0};
	item.time = 25.0;
	return item;
}

void check_queue() {
	TrainQueue queue;
	for (size_t i = 0; i < MAX_QUEUE; ++i) {
		check(queue.push(villager()), "push " + std::to_string(i));
	}
	check(not queue.push(villager()), "queue full at 5");
	check(queue.size() == MAX_QUEUE, "size 5");

	// chain: one item every 25 s, sweep over the update interval
	for (double dt : {0.001, 0.1, 0.25, 1.0, 7.0, 30.0}) {
		TrainQueue q;
		for (int i = 0; i < 3; ++i) {
			q.push(villager());
		}
		size_t free = 10;
		std::vector<double> done_at;
		for (double t = 0.0; t <= 200.0 and q.size() > 0; t += dt) {
			auto done = q.advance(t, free);
			for (const auto &item : done) {
				done_at.push_back(item.started + item.time);
			}
		}
		std::string what = "chain dt " + std::to_string(dt);
		check(done_at.size() == 3, "3 done " + what);
		if (done_at.size() == 3) {
			check(near(done_at[0], 25.0, 1e-6) and near(done_at[1], 50.0, 1e-6) and near(done_at[2], 75.0, 1e-6),
			      "finish times 25/50/75 " + what);
		}
		check(free == 7, "3 slots used " + what);
	}

	// population limit: 1 free slot, 2 items -> the second waits
	{
		TrainQueue q;
		q.push(villager());
		q.push(villager());
		size_t free = 1;
		auto done = q.advance(0.0, free);
		check(done.empty() and free == 0 and q.reserved() == 1, "first starts");
		check(near(q.progress(12.5), 0.5), "progress half");
		free = 0;
		done = q.advance(25.0, free);
		check(done.size() == 1 and q.is_waiting() and q.reserved() == 0, "second waits for housing");
		free = 0;
		done = q.advance(40.0, free);
		check(done.empty() and q.is_waiting(), "still waiting");
		free = 5;
		done = q.advance(41.0, free);
		check(done.empty() and not q.is_waiting() and free == 4 and q.reserved() == 1, "starts when housing free");
		done = q.advance(66.0, free);
		check(done.size() == 1 and near(done[0].started, 41.0), "second done after 25 s");
		check(q.size() == 0 and not q.is_waiting(), "empty");
	}

	// cancel: last item, refund by the caller
	{
		TrainQueue q;
		q.push(villager());
		q.push(villager());
		size_t free = 5;
		q.advance(0.0, free);
		auto item = q.cancel_last();
		check(item and item->cost[0] == 50 and q.size() == 1 and q.reserved() == 1, "cancel last");
		item = q.cancel_last();
		check(item and q.size() == 0 and q.reserved() == 0, "cancel running");
		check(not q.cancel_last(), "cancel empty");
		check(near(q.progress(10.0), 0.0), "no progress when empty");
	}
}

void check_population_and_building() {
	check(population_cap(0.0) == 0 and population_cap(5.0) == 5 and population_cap(10.0) == 10, "cap");
	check(population_cap(1000.0) == POPULATION_LIMIT, "cap 200");
	check(population_cap(-3.0) == 0, "negative cap");

	// one builder: 25 steps of 1 s complete a house (25 s)
	for (double step : {0.05, 0.1, 0.5, 1.0}) {
		double p = 0.0;
		double t = 0.0;
		while (p < 1.0 and t < 100.0) {
			p = build_progress(p, step, 25.0);
			t += step;
		}
		check(near(t, 25.0, step + 1e-6), "house after 25 s, step " + std::to_string(step));
	}
	// two builders: twice as fast
	double p = 0.0;
	for (int i = 0; i < 25; ++i) {
		p = build_progress(p, 0.5, 25.0);
		p = build_progress(p, 0.5, 25.0);
	}
	check(p >= 1.0, "two builders 12.5 s");
	check(build_progress(0.9, 100.0, 25.0) == 1.0, "clamped");
	check(build_progress(0.2, 1.0, 0.0) == 1.0, "no build time");
	// health of a foundation: 1 .. max, rising with the progress
	long long last = 0;
	for (int i = 0; i <= 100; ++i) {
		auto hp = construction_health(900, i / 100.0);
		check(hp >= 1 and hp <= 900 and hp >= last, "health rises " + std::to_string(i));
		last = hp;
	}
	check(construction_health(900, 0.0) == 1 and construction_health(900, 1.0) == 900, "health 1 .. 900");
	check(construction_health(900, 0.5) == 450 and construction_health(0, 0.5) == 0, "health half, none");
}

/// the desktop check (scripts/wsl/83-prod-check.sh) as numbers
void check_scenario() {
	ResourceStock stock;
	resource_amounts_t villager_cost{50, 0, 0, 0};
	resource_amounts_t house_cost{0, 30, 0, 0};
	TrainQueue tc;
	for (int i = 0; i < 2; ++i) {
		check(stock.spend(villager_cost), "pay villager");
		auto item = villager();
		tc.push(item);
	}
	check(near(stock.get(resource_t::FOOD), 100.0), "food 200 -> 100");
	// 3 villagers, town centre +5
	size_t units = 3;
	double provided = 5.0;
	size_t spawned = 0;
	for (double t = 0.0; t <= 60.0; t += 0.1) {
		size_t used = units + tc.reserved();
		size_t free = population_cap(provided) > used ? population_cap(provided) - used : 0;
		spawned += tc.advance(t, free).size();
		units = 3 + spawned;
	}
	check(spawned == 2 and units == 5, "2 villagers after 50 s");
	check(stock.spend(house_cost) and near(stock.get(resource_t::WOOD), 170.0), "house wood 200 -> 170");
	double progress = 0.0;
	for (int i = 0; i < 25; ++i) {
		progress = build_progress(progress, 1.0, 25.0);
	}
	if (progress >= 1.0) {
		provided += 5.0;
	}
	check(population_cap(provided) == 10, "population limit 5 -> 10");
}

} // namespace

int main() {
	check_known();
	check_footprints();
	check_placement_rules();
	check_costs();
	check_queue();
	check_population_and_building();
	check_scenario();
	std::cout << "prod check: " << checks << " checks, " << failures << " failures" << std::endl;
	return failures == 0 ? 0 : 1;
}
