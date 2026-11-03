// Copyright 2026-2026 the openage authors. See copying.md for legal info.

/*
 * Host test of the technology rules (XR fork W1).
 *
 *   openage-tech-check
 *
 * Parameter sweeps over the dependency-free parts of research, ages and civilisations:
 * - catalog: codes unique per kind and range (101 train, 201 build, 301 research),
 *   names unique per kind, German labels, hidden entries have a reason ("noch ohne
 *   Mechanik"), every civilisation once
 * - conditions: AND / OR / NOT / SUBSETMIN over every subset of facts (2^6 sweep),
 *   the data's Feudal/Castle/Imperial conditions, nested gates, reasons in German
 * - offer state matrix (age 1..4 x condition age x researched x queued x mechanic)
 * - ages from researched technologies, labels, "X ist zur Feudalzeit vorangeschritten."
 * - line upgrades: prefix of the line, membership (militia line, not the barracks)
 * - civilisation choice: fixed, random deterministic per seed and player, two random
 *   players differ (seed sweep 1..2000)
 * - training queue with research: no population slot, order of a mixed queue, cancel
 *   and contains()
 * - research rule of the computer opponent (villagers 0..40 x age x offers)
 * Exit code 0 if all checks pass. No engine dependencies.
 */

#include <cmath>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "gamestate/catalog.h"
#include "gamestate/production_math.h"
#include "gamestate/tech_math.h"

using namespace openage::gamestate;
using namespace openage::gamestate::tech;

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

const std::string GE = "hd_base.data.game_entity.generic.";
const std::string TE = "hd_base.data.tech.generic.";

std::string ge(const std::string &dir, const std::string &name) {
	return GE + dir + "." + dir + "." + name;
}

std::string te(const std::string &dir, const std::string &name) {
	return TE + dir + "." + dir + "." + name;
}

void check_catalog() {
	std::set<int> codes;
	std::set<std::pair<std::string, int>> names;
	size_t hidden = 0;
	for (const auto &e : prod::CATALOG) {
		const std::string name = e.name;
		check(codes.insert(e.code).second, "unique code " + name);
		check(names.insert({name, static_cast<int>(e.kind)}).second, "unique name per kind " + name);
		switch (e.kind) {
		case prod::entry_t::UNIT:
			check(e.code >= 101 and e.code <= 199 and not e.building, "unit code " + name);
			break;
		case prod::entry_t::BUILDING:
			check(e.code >= 201 and e.code <= 299 and e.building, "building code " + name);
			break;
		case prod::entry_t::TECH:
			check(e.code >= 301 and e.code <= 499 and not e.building, "tech code " + name);
			break;
		}
		check(std::string{e.label}.size() >= 3, "label " + name);
		check(std::string{e.icon}.size() >= 3, "icon " + name);
		if (e.missing != nullptr) {
			hidden += 1;
			check(std::string{e.missing}.rfind("noch ohne Mechanik", 0) == 0, "reason of hidden " + name);
		}
	}
	check(hidden >= 30 and hidden < prod::CATALOG_SIZE / 2, "some entries hidden, most offered");
	// what W1 offers and hides
	for (const char *offered : {"Villager", "Militia", "Knight", "Blacksmith", "University", "TownCenter", "Castle",
	                            "Monastery", "SiegeWorkshop"}) {
		check(prod::has_mechanic(offered), std::string{"offered "} + offered);
	}
	// XR fork W3: farms, docks, fishing ships and traps and the farm technologies have their mechanic
	for (const char *w3 : {"FishingShip", "Dock", "Farm", "FishingTrap"}) {
		check(prod::has_mechanic(w3), std::string{"offered (W3) "} + w3);
	}
	check(prod::has_mechanic("HorseCollar", true) and prod::has_mechanic("HeavyPlow", true), "farm technologies offered (W3)");
	for (const char *hidden_name : {"Monk", "Ram", "Galley", "TransportShip", "TradeCart", "Market", "Wonder"}) {
		check(not prod::has_mechanic(hidden_name), std::string{"hidden "} + hidden_name);
	}
	// XR fork W5: walls, gates and the fortified wall have their mechanic
	for (const char *w5 : {"PalisadeWall", "StoneWall", "StoneGate"}) {
		check(prod::has_mechanic(w5), std::string{"offered (W5) "} + w5);
	}
	check(prod::has_mechanic("FortifiedWall", true), "fortified wall offered (W5)");
	for (const char *t : {"Loom", "FeudalAge", "WheelBarrow", "HandCart", "Swordsman", "Crossbowman", "Forging"}) {
		check(prod::has_mechanic(t, true), std::string{"offered tech "} + t);
		check(not std::string{prod::catalog_entry(t, true)->effect}.empty(), std::string{"effect "} + t);
	}
	// XR fork W4: towers shoot and take a garrison, herbal medicine heals the garrison
	check(prod::has_mechanic("Tower") and prod::has_mechanic("BombardTower"), "W4: towers offered");
	for (const char *t : {"GuardTower", "Keep", "HerbalMedicine"}) {
		check(prod::has_mechanic(t, true), std::string{"W4: offered tech "} + t);
	}
	for (const char *t : {"Coinage", "Redemption", "CappedRam", "Careening"}) {
		check(not prod::has_mechanic(t, true), std::string{"hidden tech "} + t);
	}
	// civilisations
	std::set<std::string> civs;
	for (const auto &c : prod::CIVS) {
		check(civs.insert(c.name).second, std::string{"unique civ "} + c.name);
		check(prod::civ_index(c.name) >= 0 and prod::civ_label(c.name) == c.label, std::string{"civ lookup "} + c.name);
	}
	check(prod::civ_index("Gaia") < 0, "gaia is no civilisation");
	check(prod::civ_fqon("hd_base", prod::CIVS[0]) == "hd_base.data.civ.britons.britons.Britons", "civ fqon");
}

void check_conditions() {
	using k = Condition::kind_t;
	// sweep: every subset of six facts against the gates
	const std::vector<std::string> buildings{ge("mill", "Mill"), ge("lumber_camp", "LumberCamp"),
	                                         ge("dock", "Dock"), ge("barracks", "Barracks"),
	                                         ge("mining_camp", "MiningCamp")};
	const std::string feudal = te("feudal_age", "FeudalAge");
	std::vector<Condition> literals;
	for (const auto &b : buildings) {
		literals.push_back(Condition::built(b));
	}
	literals.push_back(Condition::tech(feudal));
	for (unsigned mask = 0; mask < 64; ++mask) {
		Facts f;
		size_t n_built = 0;
		for (size_t i = 0; i < buildings.size(); ++i) {
			if (mask & (1u << i)) {
				f.built.insert(buildings[i]);
				n_built += 1;
			}
		}
		const bool has_feudal = mask & 32u;
		if (has_feudal) {
			f.researched.insert(feudal);
		}
		const auto all = Condition::gate(k::AND, literals);
		const auto any = Condition::gate(k::OR, literals);
		const auto two = Condition::gate(k::SUBSETMIN, std::vector<Condition>(literals.begin(), literals.begin() + 5), 2);
		const auto neither = Condition::gate(k::NOT, {any});
		check(evaluate(all, f) == (mask == 63u), "AND mask " + std::to_string(mask));
		check(evaluate(any, f) == (mask != 0u), "OR mask " + std::to_string(mask));
		check(evaluate(two, f) == (n_built >= 2), "SUBSETMIN 2 mask " + std::to_string(mask));
		check(evaluate(neither, f) == (mask == 0u), "NOT mask " + std::to_string(mask));
		check(missing_text(two, f).empty() == (n_built >= 2), "reason iff locked " + std::to_string(mask));
		if (n_built < 2) {
			const auto text = missing_text(two, f);
			check(text.rfind("Benötigt ", 0) == 0, "reason starts with Benötigt: " + text);
			check(text.find(n_built == 1 ? "1 weitere von:" : "2 von:") != std::string::npos, "reason counts: " + text);
			check(text.find("Mühle") != std::string::npos or (mask & 1u), "reason names the mill: " + text);
		}
		// Castle Age of the data: Feudal Age AND 2 of 4 buildings
		const auto castle = all_of({Condition::tech(feudal), two});
		check(evaluate(castle, f) == (has_feudal and n_built >= 2), "castle condition " + std::to_string(mask));
		check(required_age(castle) == 2, "castle condition asks for the feudal age");
		if (not has_feudal) {
			check(missing_text(castle, f) == "Benötigt Feudalzeit", "age first: " + missing_text(castle, f));
		}
	}
	// Imperial Age of the data: Castle Age AND (2 of 4 OR castle) as SUBSETMIN 1 of {SUBSETMIN 2, castle}
	const std::string castle_age = te("castle_age", "CastleAge");
	const auto inner = Condition::gate(k::SUBSETMIN,
	                                   {Condition::built(ge("siege_workshop", "SiegeWorkshop")),
	                                    Condition::built(ge("monastery", "Monastery")),
	                                    Condition::built(ge("university", "University")),
	                                    Condition::built(ge("castle", "Castle"))},
	                                   2);
	const auto imperial = all_of({Condition::tech(castle_age),
	                              Condition::gate(k::SUBSETMIN, {inner, Condition::built(ge("castle", "Castle"))}, 1)});
	Facts f;
	f.researched.insert(castle_age);
	check(not evaluate(imperial, f), "imperial locked without buildings");
	check(missing_text(imperial, f).find("Burg") != std::string::npos, "imperial reason names the castle: " + missing_text(imperial, f));
	f.built.insert(ge("castle", "Castle"));
	check(evaluate(imperial, f), "imperial with a castle");
	f.built.clear();
	f.built.insert(ge("university", "University"));
	f.built.insert(ge("monastery", "Monastery"));
	check(evaluate(imperial, f), "imperial with university and monastery");
	check(required_age(imperial) == 3, "imperial condition asks for the castle age");
	// empty and unsupported conditions
	check(evaluate(Condition{}, Facts{}) and required_age(Condition{}) == 1, "empty = always");
	Condition never;
	never.kind = k::NEVER;
	check(not evaluate(never, Facts{}) and missing_text(never, Facts{}) == "Noch nicht verfügbar", "unsupported = locked");
	check(all_of({}).kind == k::ALWAYS and all_of({Condition::tech(feudal)}).kind == k::TECH, "all_of");
}

void check_offer_matrix() {
	const std::string feudal = te("feudal_age", "FeudalAge");
	const std::string castle = te("castle_age", "CastleAge");
	const std::string range = ge("archery_range", "ArcheryRange");
	for (int age = 1; age <= 4; ++age) {
		for (int needs = 1; needs <= 4; ++needs) {
			Condition c = needs == 1 ? Condition{} : Condition::tech(needs == 2 ? feudal : needs == 3 ? castle : te("imperial_age", "ImperialAge"));
			c = all_of({c, Condition::built(range)});
			for (int bits = 0; bits < 16; ++bits) {
				const bool researched = bits & 1, queued = bits & 2, mechanic = bits & 4, built = bits & 8;
				Facts f;
				if (age >= 2) f.researched.insert(feudal);
				if (age >= 3) f.researched.insert(castle);
				if (age >= 4) f.researched.insert(te("imperial_age", "ImperialAge"));
				if (built) f.built.insert(range);
				auto o = offer_state(c, f, age, researched, queued, mechanic);
				offer_t want;
				if (not mechanic or researched or needs > age) {
					want = offer_t::HIDDEN;
				}
				else if (queued or not built) {
					want = offer_t::LOCKED;
				}
				else {
					want = offer_t::AVAILABLE;
				}
				check(o == want, "offer age " + std::to_string(age) + " needs " + std::to_string(needs) + " bits "
				                     + std::to_string(bits));
			}
		}
	}
	// the next age technology is shown in the age before it
	check(offer_state(Condition{}, Facts{}, 1, false, false, true) == offer_t::AVAILABLE, "feudal age offered in the dark age");
	check(offer_state(Condition::tech(feudal), Facts{}, 1, false, false, true) == offer_t::HIDDEN, "castle age hidden in the dark age");
}

void check_ages() {
	check(age_of_tech("FeudalAge") == 2 and age_of_tech("CastleAge") == 3 and age_of_tech("ImperialAge") == 4
	          and age_of_tech("Loom") == 0,
	      "age of technologies");
	for (int age = 1; age <= 4; ++age) {
		check(std::string{age_label(age)}.size() > 5, "age label " + std::to_string(age));
		if (age > 1) {
			check(age_of_tech(age_tech(age)) == age, "age tech round trip " + std::to_string(age));
		}
	}
	check(std::string{age_label(1)} == "Dunkles Zeitalter" and std::string{age_label(2)} == "Feudalzeit"
	          and std::string{age_label(3)} == "Ritterzeit" and std::string{age_label(4)} == "Imperialzeit",
	      "German age names");
	check(age_reached_text("Spieler 2 (Franken)", 2) == "Spieler 2 (Franken) ist zur Feudalzeit vorangeschritten.", "age message");
	Facts f;
	check(age_of(f) == 1, "dark age without techs");
	f.researched.insert(te("loom", "Loom"));
	check(age_of(f) == 1, "loom is no age");
	f.researched.insert(te("castle_age", "CastleAge"));
	check(age_of(f) == 3, "castle age");
}

void check_lines() {
	const std::string ls = GE + "militia.longswordsman.Longswordsman";
	check(line_prefix(ls) == GE + "militia.militia.", "line prefix of the long swordsman");
	check(line_prefix(te("loom", "Loom")).empty(), "loom has no line");
	check(in_line(ge("militia", "Militia"), ls), "militia is in the line");
	check(not in_line(ge("barracks", "Barracks"), ls), "barracks is not");
	check(not in_line(GE + "militia.militia.Militia.Idle", ls), "abilities are not entities");
	check(in_line(ge("archer", "Archer"), GE + "archer.crossbowman.Crossbowman"), "archer line");
	check(short_of(ls) == "Longswordsman" and short_of("Loom") == "Loom", "short names");
}

void check_civ_choice() {
	check(prod::pick_civ("Britons", 7, 0) == "Britons" and prod::pick_civ("Huns", 7, 1) == "Huns", "fixed civ");
	check(prod::civ_index(prod::pick_civ("Atlantis", 7, 0)) >= 0, "unknown civ: random");
	std::set<std::string> seen;
	size_t different = 0;
	for (uint32_t seed = 1; seed <= 2000; ++seed) {
		auto a = prod::pick_civ("random", seed, 0);
		auto b = prod::pick_civ("", seed, 1);
		check(prod::civ_index(a) >= 0 and prod::civ_index(b) >= 0, "random civ valid");
		check(a == prod::pick_civ("random", seed, 0), "random civ deterministic");
		different += a != b ? 1 : 0;
		seen.insert(a);
	}
	check(different == 2000, "two random players differ");
	check(seen.size() == prod::CIVS.size(), "every civilisation comes up");
}

void check_queue() {
	using prod::TrainQueue;
	TrainQueue q;
	TrainQueue::Item villager{"Villager", ge("villager", "Villager"), {50, 0, 0, 0}, 25.0, -1.0, false};
	TrainQueue::Item loom{"Loom", te("loom", "Loom"), {0, 0, 50, 0}, 25.0, -1.0, true};
	check(q.push(villager) and q.push(loom) and q.push(villager), "mixed queue");
	check(q.contains(loom.fqon) and not q.contains(te("wheel_barrow", "WheelBarrow")), "contains");
	// no free population slot: the villager waits, research does not need one
	size_t free = 0;
	auto done = q.advance(0.0, free);
	check(done.empty() and q.is_waiting() and q.reserved() == 0, "villager waits for a slot");
	// AoE II: a click on a villager cancels the hindmost villager (index 2), then the waiting one
	auto cancelled = q.cancel_at(0);
	check(cancelled and cancelled->name == "Villager" and q.size() == 2 and q.get_items()[1].research,
	      "cancel takes the hindmost villager");
	cancelled = q.cancel_at(0);
	check(cancelled and cancelled->name == "Villager" and q.size() == 1, "cancel the waiting villager");
	free = 0;
	done = q.advance(1.0, free);
	check(done.empty() and not q.is_waiting() and q.reserved() == 0, "research starts without a slot");
	done = q.advance(26.0, free);
	check(done.size() == 1 and done[0].research and done[0].name == "Loom", "research done after its time");
	check(not q.contains(loom.fqon), "research left the queue");
	// sweep: research time and slot counts, order of a queue villager, research, villager
	for (int slots = 0; slots <= 3; ++slots) {
		for (double rt : {1.0, 25.0, 130.0}) {
			TrainQueue s;
			auto r = loom;
			r.time = rt;
			s.push(villager);
			s.push(r);
			s.push(villager);
			size_t f = static_cast<size_t>(slots);
			std::vector<std::string> order;
			for (double t = 0.0; t < 400.0; t += 0.5) {
				for (const auto &item : s.advance(t, f)) {
					order.push_back(item.name);
				}
			}
			if (slots >= 2) {
				check(order == std::vector<std::string>{"Villager", "Loom", "Villager"}, "order with slots");
			}
			else if (slots == 1) {
				check(order == std::vector<std::string>{"Villager", "Loom"}, "second villager waits");
			}
			else {
				check(order.empty(), "first villager blocks the queue");
			}
		}
	}
}

void check_ai_rule() {
	for (size_t villagers = 0; villagers <= 40; ++villagers) {
		for (int age = 1; age <= 2; ++age) {
			for (int mask = 0; mask < 16; ++mask) {
				AiResearchView v;
				v.villagers = villagers;
				v.age = age;
				v.researching = mask & 8;
				if (mask & 1) v.offered.push_back("Loom");
				if (mask & 2) v.offered.push_back(age == 1 ? "FeudalAge" : "CastleAge");
				if (mask & 4) v.offered.push_back("WheelBarrow");
				auto c = ai_research_choice(v);
				std::string want;
				if (not v.researching) {
					if (age == 1 and villagers >= 10 and (mask & 2)) {
						want = "FeudalAge";
					}
					else if (villagers >= 10 and (mask & 1)) {
						want = "Loom";
					}
					else if (age >= 2 and villagers >= 12 and (mask & 4)) {
						want = "WheelBarrow";
					}
				}
				check(c == want, "ai research v" + std::to_string(villagers) + " age " + std::to_string(age) + " mask "
				                     + std::to_string(mask) + ": " + c);
			}
		}
	}
	AiResearchView v;
	v.villagers = 30;
	v.age = 2;
	v.offered = {"HandCart"};
	check(ai_research_choice(v) == "HandCart", "hand cart after the wheelbarrow");
}

} // namespace

int main() {
	check_catalog();
	check_conditions();
	check_offer_matrix();
	check_ages();
	check_lines();
	check_civ_choice();
	check_queue();
	check_ai_rule();
	std::cout << "tech check: " << checks << " checks, " << failures << " failures" << std::endl;
	return failures == 0 ? 0 : 1;
}
