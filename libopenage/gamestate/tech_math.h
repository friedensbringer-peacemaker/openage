// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "gamestate/catalog.h"
#include "gamestate/resources.h"


namespace openage::gamestate::tech {

/**
 * Rules of the technologies (XR fork): unlock conditions of creatables and
 * researchables (nyan LogicElement trees), ages, the reasons shown for locked
 * buttons, which researchables a building shows, and the simple research rule of
 * the computer opponent.
 *
 * Behaviour follows the converted nyan data (conditions, costs, times, patches)
 * and the generally known AoE II rules (one age after the other, a technology is
 * researched once per player, a building trains or researches one thing at a
 * time in the order of its queue).
 *
 * This header has no engine dependencies (host test gamestate/tech_check.cpp).
 */

/// ages: 1 Dark, 2 Feudal, 3 Castle, 4 Imperial
constexpr int DARK_AGE = 1;
constexpr int MAX_AGE = 4;

/// age reached by a technology short name (0: no age technology)
inline int age_of_tech(const std::string &name) {
	if (name == "FeudalAge") {
		return 2;
	}
	if (name == "CastleAge") {
		return 3;
	}
	if (name == "ImperialAge") {
		return 4;
	}
	return 0;
}

/// short name of the technology that reaches an age (empty for the Dark Age)
inline const char *age_tech(int age) {
	switch (age) {
	case 2:
		return "FeudalAge";
	case 3:
		return "CastleAge";
	case 4:
		return "ImperialAge";
	default:
		return "";
	}
}

/// German name of an age ("Dunkles Zeitalter", "Feudalzeit", ...)
inline const char *age_label(int age) {
	switch (age) {
	case 2:
		return "Feudalzeit";
	case 3:
		return "Ritterzeit";
	case 4:
		return "Imperialzeit";
	default:
		return "Dunkles Zeitalter";
	}
}

/// "erreicht die Feudalzeit" (message of an age change)
inline std::string age_reached_text(const std::string &who, int age) {
	return who + " erreicht die " + age_label(age);
}

/// short name of a nyan object ("hd_base...loom.Loom" -> "Loom")
inline std::string short_of(const std::string &fqon) {
	auto dot = fqon.rfind('.');
	return dot == std::string::npos ? fqon : fqon.substr(dot + 1);
}

/**
 * Unlock condition (nyan engine.util.logic): a tree of gates over literals.
 * Unknown literal types are NEVER (locked), so that missing mechanics do not
 * unlock anything by accident.
 */
struct Condition {
	enum class kind_t {
		/// empty condition set: always true
		ALWAYS,
		/// unsupported literal
		NEVER,
		/// literal.TechResearched (ref = tech fqon)
		TECH,
		/// literal.GameEntityProgress, construction 100 % (ref = game entity fqon)
		BUILT,
		AND,
		OR,
		NOT,
		/// at least `min` inputs true
		SUBSETMIN,
	};
	kind_t kind = kind_t::ALWAYS;
	std::string ref{};
	size_t min = 1;
	std::vector<Condition> inputs{};
	/// unsupported type name (NEVER, for logs and the reason)
	std::string unsupported{};

	static Condition tech(const std::string &fqon) {
		Condition c;
		c.kind = kind_t::TECH;
		c.ref = fqon;
		return c;
	}
	static Condition built(const std::string &fqon) {
		Condition c;
		c.kind = kind_t::BUILT;
		c.ref = fqon;
		return c;
	}
	static Condition gate(kind_t kind, std::vector<Condition> inputs, size_t min = 1) {
		Condition c;
		c.kind = kind;
		c.inputs = std::move(inputs);
		c.min = min;
		return c;
	}
};

/// what a player has: researched technologies and (ever) completed buildings (fqons)
struct Facts {
	std::set<std::string> researched{};
	std::set<std::string> built{};
};

/// the condition holds
inline bool evaluate(const Condition &c, const Facts &facts) {
	using k = Condition::kind_t;
	switch (c.kind) {
	case k::ALWAYS:
		return true;
	case k::NEVER:
		return false;
	case k::TECH:
		return facts.researched.contains(c.ref);
	case k::BUILT:
		return facts.built.contains(c.ref);
	case k::AND:
		return std::all_of(c.inputs.begin(), c.inputs.end(), [&](const Condition &i) { return evaluate(i, facts); });
	case k::OR:
		return std::any_of(c.inputs.begin(), c.inputs.end(), [&](const Condition &i) { return evaluate(i, facts); });
	case k::NOT:
		return c.inputs.empty() or not evaluate(c.inputs.front(), facts);
	case k::SUBSETMIN: {
		size_t n = 0;
		for (const auto &i : c.inputs) {
			n += evaluate(i, facts) ? 1 : 0;
		}
		return n >= c.min;
	}
	default:
		return false;
	}
}

/// conjunction of a set of conditions (CreatableGameEntity.condition is a set: all must hold)
inline Condition all_of(std::vector<Condition> set) {
	if (set.empty()) {
		return Condition{};
	}
	if (set.size() == 1) {
		return std::move(set.front());
	}
	return Condition::gate(Condition::kind_t::AND, std::move(set));
}

/**
 * Age a condition asks for: the highest age technology that must be researched
 * (literals in AND chains; OR/SUBSETMIN/NOT branches do not count). 1 if none.
 */
inline int required_age(const Condition &c) {
	using k = Condition::kind_t;
	switch (c.kind) {
	case k::TECH:
		return std::max(DARK_AGE, age_of_tech(short_of(c.ref)));
	case k::AND: {
		int age = DARK_AGE;
		for (const auto &i : c.inputs) {
			age = std::max(age, required_age(i));
		}
		return age;
	}
	default:
		return DARK_AGE;
	}
}

/// German label of a nyan object short name for reasons (catalog, else the name)
inline std::string label_for(const std::string &fqon, bool tech) {
	const auto name = short_of(fqon);
	if (tech) {
		if (int age = age_of_tech(name)) {
			return age_label(age);
		}
	}
	if (const auto *entry = prod::catalog_entry(name, tech)) {
		return entry->label;
	}
	return name;
}

/**
 * Why a condition does not hold, in German for the HUD ("Benötigt Feudalzeit",
 * "Benötigt Bogenschießstand", "Benötigt 2 von: Mühle, Holzfällerlager, …").
 * Empty if it holds.
 */
inline std::string missing_text(const Condition &c, const Facts &facts) {
	using k = Condition::kind_t;
	if (evaluate(c, facts)) {
		return {};
	}
	switch (c.kind) {
	case k::NEVER:
		return "Noch nicht verfügbar";
	case k::TECH:
		return "Benötigt " + label_for(c.ref, true);
	case k::BUILT:
		return "Benötigt " + label_for(c.ref, false);
	case k::AND:
		// ages first: they are what the player works towards
		for (const auto &i : c.inputs) {
			if (i.kind == k::TECH and not evaluate(i, facts) and age_of_tech(short_of(i.ref)) > 0) {
				return missing_text(i, facts);
			}
		}
		for (const auto &i : c.inputs) {
			if (not evaluate(i, facts)) {
				return missing_text(i, facts);
			}
		}
		return {};
	case k::OR:
	case k::SUBSETMIN: {
		std::vector<std::string> names;
		size_t have = 0;
		for (const auto &i : c.inputs) {
			if (evaluate(i, facts)) {
				have += 1;
				continue;
			}
			if (i.kind == k::TECH or i.kind == k::BUILT) {
				names.push_back(label_for(i.ref, i.kind == k::TECH));
			}
			else {
				// nested gate (Imperial Age: castle or two buildings)
				names.push_back(missing_text(i, facts).substr(std::string{"Benötigt "}.size()));
			}
		}
		const size_t need = c.kind == k::OR ? 1 : c.min;
		std::string out = "Benötigt ";
		if (need > have + 1 or c.kind == k::SUBSETMIN) {
			out += std::to_string(need - std::min(need, have)) + (have > 0 ? " weitere" : "") + " von: ";
		}
		for (size_t i = 0; i < names.size(); ++i) {
			out += (i > 0 ? (i + 1 == names.size() and c.kind == k::OR ? " oder " : ", ") : "") + names[i];
		}
		return out;
	}
	case k::NOT:
		return "Nicht mehr verfügbar";
	default:
		return "Noch nicht verfügbar";
	}
}

/// how a creatable or researchable shows up in the command grid
enum class offer_t {
	/// not shown (later age, already researched, no mechanic)
	HIDDEN,
	/// shown, greyed out with a reason
	LOCKED,
	/// shown and usable (resources permitting)
	AVAILABLE,
};

/**
 * Visibility of an entry for a player in an age (AoE II: entries of later ages
 * stay hidden, the next age technology is always shown; what is missing apart
 * from the age is shown greyed out with its reason).
 *
 * @param researched The technology is researched (researchables only).
 * @param in_progress The technology is queued somewhere (researchables only).
 */
inline offer_t offer_state(const Condition &condition, const Facts &facts, int age, bool researched, bool in_progress,
                           bool mechanic) {
	if (not mechanic or researched) {
		return offer_t::HIDDEN;
	}
	if (required_age(condition) > age) {
		return offer_t::HIDDEN;
	}
	if (in_progress) {
		return offer_t::LOCKED;
	}
	return evaluate(condition, facts) ? offer_t::AVAILABLE : offer_t::LOCKED;
}

/**
 * Highest age of a player from its researched technologies (fqons).
 */
inline int age_of(const Facts &facts) {
	int age = DARK_AGE;
	for (const auto &t : facts.researched) {
		age = std::max(age, age_of_tech(short_of(t)));
	}
	return age;
}

/**
 * The game entity line a line upgrade belongs to (XR fork, converter layout): line
 * upgrades are technologies in the directory of the line's game entity
 * ("...game_entity.generic.militia.longswordsman.Longswordsman" belongs to the
 * entities "...game_entity.generic.militia.militia.*"). Returns the prefix of the
 * line entity ("...game_entity.generic.militia.militia."), empty for other techs.
 */
inline std::string line_prefix(const std::string &tech_fqon) {
	const std::string marker = ".game_entity.generic.";
	auto at = tech_fqon.find(marker);
	if (at == std::string::npos) {
		return {};
	}
	auto dir_start = at + marker.size();
	auto dir_end = tech_fqon.find('.', dir_start);
	if (dir_end == std::string::npos) {
		return {};
	}
	const auto dir = tech_fqon.substr(dir_start, dir_end - dir_start);
	return tech_fqon.substr(0, dir_end) + "." + dir + ".";
}

/// the game entity belongs to the line of a line upgrade
inline bool in_line(const std::string &entity_fqon, const std::string &tech_fqon) {
	auto prefix = line_prefix(tech_fqon);
	return not prefix.empty() and entity_fqon.compare(0, prefix.size(), prefix) == 0
	       and entity_fqon.find('.', prefix.size()) == std::string::npos;
}

// ---- research rule of the computer opponent ----------------------------------------------

/// what the computer opponent knows when it decides about research
struct AiResearchView {
	int age = DARK_AGE;
	size_t villagers = 0;
	resource_amounts_t stock{};
	/// short names of technologies the town centre offers now (available, not researched)
	std::vector<std::string> offered{};
	/// a research runs or waits in the town centre
	bool researching = false;
};

/// thresholds of the research rule (the AI trains 15 villagers, ai_rules.h; without farms and hunting (W3) the
/// food of the start is short, so the AI saves for the Feudal Age from feudal_villagers on)
struct AiResearchParams {
	/// Loom from this many villagers on
	size_t loom_villagers = 10;
	/// Feudal Age from this many villagers on
	size_t feudal_villagers = 10;
	/// Wheelbarrow line from this many villagers on (after the age-up)
	size_t wheelbarrow_villagers = 12;
};

/**
 * Simple research rule of the computer opponent (XR fork): Loom, the Feudal Age,
 * then the Wheelbarrow line (Wheelbarrow, Hand Cart), each once the villager count
 * is reached and the town centre offers it; one research at a time (it waits in the
 * queue of the town centre behind queued villagers, like in AoE II).
 *
 * @return Short name of the technology to research, empty: nothing.
 */
inline std::string ai_research_choice(const AiResearchView &v, const AiResearchParams &p = {}) {
	if (v.researching) {
		return {};
	}
	auto offered = [&](const char *name) {
		return std::find(v.offered.begin(), v.offered.end(), name) != v.offered.end();
	};
	if (v.villagers >= p.feudal_villagers and v.age == DARK_AGE and offered("FeudalAge")) {
		return "FeudalAge";
	}
	if (v.villagers >= p.loom_villagers and offered("Loom")) {
		return "Loom";
	}
	if (v.villagers >= p.wheelbarrow_villagers and v.age >= 2) {
		if (offered("WheelBarrow")) {
			return "WheelBarrow";
		}
		if (offered("HandCart")) {
			return "HandCart";
		}
	}
	return {};
}

} // namespace openage::gamestate::tech
