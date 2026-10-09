// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "gamestate/map_settings.h"
#include "gamestate/resources.h"


namespace openage::gamestate::save {

/**
 * Save game file format of the XR fork (gamestate/save_game.h writes and reads it).
 *
 * The file is plain text, one "key = value" line per field and one "[section]"
 * line per block, so it can be inspected and diffed. Unknown keys are ignored
 * (newer files with more fields still load), a greater format version is
 * rejected ("too new") instead of being guessed at.
 *
 * What is stored: the map settings (the map itself is generated again from
 * them, deterministically), the game time, the resources of every player, the
 * entities (type, owner, position, health, resource stock, carried resources,
 * construction progress, training queue, rally point, current order) and the
 * ids of generated map objects that are gone (gathered empty, destroyed).
 * Generated map objects of gaia that did not change are not stored (delta).
 *
 * This header has no engine dependencies, so embedders (the Quest app) can
 * list slots and read the header of a file, and a host test can check the
 * round trip without the engine.
 */

constexpr int FORMAT_VERSION = 1;

/// slots 1..SLOT_COUNT are chosen by the player, slot 0 is the autosave,
/// QUICK_SLOT the quick save (F5/F9)
constexpr int SLOT_COUNT = 5;
constexpr int AUTOSAVE_SLOT = 0;
constexpr int QUICK_SLOT = SLOT_COUNT + 1;

/// autosave interval (simulation seconds)
constexpr double AUTOSAVE_SECONDS = 300.0;

/// resources of one player
struct SavedPlayer {
	uint64_t id = 0;
	resource_amounts_t resources{};
};

/// one entry of a training queue
struct SavedQueueItem {
	std::string name;
	std::string fqon;
	resource_amounts_t cost{};
	/// training time (seconds)
	double time = 0.0;
	/// seconds left for the first item, negative while waiting for a population slot
	double remaining = -1.0;
};

/// one game entity
struct SavedEntity {
	uint64_t id = 0;
	/// nyan game entity (e.g. "hd_base.data.game_entity.generic.villager.villager.Villager")
	std::string fqon;
	uint64_t owner = 0;
	/// part of the generated map (same id after the map is generated again): updated in place
	bool generated = false;
	double ne = 0.0;
	double se = 0.0;
	double up = 0.0;
	/// facing (degrees, fixed point of the engine as double)
	double angle = 0.0;
	/// current health (Live attribute), nothing if the type has none
	std::optional<int64_t> health{};
	/// remaining amount of a resource spot (Harvestable)
	std::optional<double> harvest{};
	/// carried resources of a gatherer
	double carried = 0.0;
	int carried_type = 0;
	/// construction progress 0..1 of a foundation (nothing for finished buildings)
	std::optional<double> construction{};
	double build_time = 0.0;
	std::vector<SavedQueueItem> queue{};
	std::optional<std::array<double, 2>> rally{};
	/// current order: "", "move", "gather", "build", "attack"
	std::string order{};
	uint64_t order_target = 0;
	double order_ne = 0.0;
	double order_se = 0.0;
};

/// whole save game
struct SaveData {
	int version = FORMAT_VERSION;
	/// shown in the slot list
	std::string title;
	/// ISO date/time of the save ("2026-10-08 19:30")
	std::string created;
	/// what wrote the file (for diagnostics)
	std::string engine;
	/// simulation time (seconds)
	double game_time = 0.0;
	MapSettings map{};
	/// entity ids of the generated map objects: first..last (last < first: none)
	uint64_t generated_first = 1;
	uint64_t generated_last = 0;
	std::vector<SavedPlayer> players{};
	std::vector<SavedEntity> entities{};
	/// generated map objects that are gone
	std::vector<uint64_t> removed{};
};

// ---- helpers

inline std::string fmt_double(double v) {
	char buf[40];
	std::snprintf(buf, sizeof(buf), "%.17g", v);
	return buf;
}

inline std::string resources_text(const resource_amounts_t &r) {
	std::string out;
	for (size_t i = 0; i < RESOURCE_COUNT; ++i) {
		if (i > 0) {
			out += ' ';
		}
		out += fmt_double(r[i]);
	}
	return out;
}

inline bool parse_resources(const std::string &text, resource_amounts_t &r) {
	std::istringstream in{text};
	for (size_t i = 0; i < RESOURCE_COUNT; ++i) {
		if (not(in >> r[i])) {
			return false;
		}
	}
	return true;
}

inline std::string trim(const std::string &s) {
	size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos) {
		return {};
	}
	size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

/// "Zufallskarte #3, 64 × 64, Grasland" for slot lists and logs (German, like the HUD)
inline std::string map_text(const MapSettings &m) {
	if (m.type == map_type_t::TEST) {
		return "Testkarte";
	}
	static const char *names[] = {"Grasland", "Steppe", "Hügelland", "Wald", "Flüsse", "Küste", "Binnenmeer",
	                              "Inseln", "Goldrausch", "Wüste", "Winter", "Dschungel"};
	auto b = static_cast<size_t>(m.biome);
	const char *biome = b < sizeof(names) / sizeof(names[0]) ? names[b] : "?";
	char buf[128];
	std::snprintf(buf, sizeof(buf), "%s #%u, %zu × %zu, %s", m.skirmish ? "Gefecht" : "Zufallskarte",
	              static_cast<unsigned>(m.seed), m.size, m.size, biome);
	return buf;
}

/// "12:34" / "1:02:03"
inline std::string time_text(double seconds) {
	long t = std::isfinite(seconds) and seconds > 0.0 ? static_cast<long>(seconds) : 0L;
	char buf[32];
	if (t >= 3600) {
		std::snprintf(buf, sizeof(buf), "%ld:%02ld:%02ld", t / 3600, t / 60 % 60, t % 60);
	}
	else {
		std::snprintf(buf, sizeof(buf), "%ld:%02ld", t / 60, t % 60);
	}
	return buf;
}

/// local date and time "2026-10-08 19:30"
inline std::string now_text() {
	auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
	std::tm tm{};
#ifdef _WIN32
	localtime_s(&tm, &t);
#else
	localtime_r(&t, &tm);
#endif
	char buf[32];
	std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
	return buf;
}

// ---- serialization

/**
 * Write the save game as text (deterministic: the same data gives the same text).
 */
inline std::string serialize(const SaveData &d) {
	std::ostringstream out;
	out << "# xr.ages save game\n";
	out << "[save]\n";
	out << "version = " << d.version << "\n";
	out << "title = " << d.title << "\n";
	out << "created = " << d.created << "\n";
	out << "engine = " << d.engine << "\n";
	out << "game_time = " << fmt_double(d.game_time) << "\n";
	out << "generated_first = " << d.generated_first << "\n";
	out << "generated_last = " << d.generated_last << "\n";
	const MapSettings &m = d.map;
	out << "[map]\n";
	out << "type = " << (m.type == map_type_t::TEST ? "test" : "random") << "\n";
	out << "biome = " << map_biome_name(m.biome) << "\n";
	out << "seed = " << m.seed << "\n";
	out << "size = " << m.size << "\n";
	out << "max_trees = " << m.max_trees << "\n";
	out << "max_elevation = " << fmt_double(m.max_elevation) << "\n";
	out << "skirmish = " << (m.skirmish ? 1 : 0) << "\n";
	out << "ai_mode = " << (m.ai.mode == ai_mode_t::ON ? "on" : m.ai.mode == ai_mode_t::OFF ? "off" : "auto") << "\n";
	out << "ai_difficulty = " << (m.ai.difficulty == ai_difficulty_t::NORMAL ? "normal" : "easy") << "\n";
	if (m.ai.player) {
		out << "ai_player = " << *m.ai.player << "\n";
	}
	if (m.ai.first_attack) {
		out << "ai_first_attack = " << fmt_double(*m.ai.first_attack) << "\n";
	}
	if (m.ai.attack_size) {
		out << "ai_attack_size = " << *m.ai.attack_size << "\n";
	}
	out << "ai_seed = " << m.ai.seed << "\n";
	for (const auto &p : d.players) {
		out << "[player]\n";
		out << "id = " << p.id << "\n";
		out << "resources = " << resources_text(p.resources) << "\n";
	}
	for (const auto &e : d.entities) {
		out << "[entity]\n";
		out << "id = " << e.id << "\n";
		out << "fqon = " << e.fqon << "\n";
		out << "owner = " << e.owner << "\n";
		out << "generated = " << (e.generated ? 1 : 0) << "\n";
		out << "pos = " << fmt_double(e.ne) << " " << fmt_double(e.se) << " " << fmt_double(e.up) << "\n";
		out << "angle = " << fmt_double(e.angle) << "\n";
		if (e.health) {
			out << "health = " << *e.health << "\n";
		}
		if (e.harvest) {
			out << "harvest = " << fmt_double(*e.harvest) << "\n";
		}
		if (e.carried > 0.0) {
			out << "carried = " << e.carried_type << " " << fmt_double(e.carried) << "\n";
		}
		if (e.construction) {
			out << "construction = " << fmt_double(*e.construction) << " " << fmt_double(e.build_time) << "\n";
		}
		for (const auto &q : e.queue) {
			out << "queue = " << q.name << " " << q.fqon << " " << fmt_double(q.time) << " "
			    << fmt_double(q.remaining) << " " << resources_text(q.cost) << "\n";
		}
		if (e.rally) {
			out << "rally = " << fmt_double((*e.rally)[0]) << " " << fmt_double((*e.rally)[1]) << "\n";
		}
		if (not e.order.empty()) {
			out << "order = " << e.order << " " << e.order_target << " " << fmt_double(e.order_ne) << " "
			    << fmt_double(e.order_se) << "\n";
		}
	}
	if (not d.removed.empty()) {
		out << "[removed]\n";
		out << "ids =";
		for (auto id : d.removed) {
			out << " " << id;
		}
		out << "\n";
	}
	return out.str();
}

/**
 * Read a save game from text.
 *
 * @return false with an error message (German, for the HUD) if the text is no
 *         save game, too new or damaged.
 */
inline bool parse(const std::string &text, SaveData &d, std::string &error) {
	d = SaveData{};
	d.version = 0;
	std::istringstream in{text};
	std::string line;
	std::string section;
	bool has_map = false;
	bool has_type = false;
	size_t line_no = 0;
	auto fail = [&](const std::string &what) {
		error = "Spielstand beschädigt (Zeile " + std::to_string(line_no) + ": " + what + ")";
		return false;
	};
	while (std::getline(in, line)) {
		++line_no;
		line = trim(line);
		if (line.empty() or line[0] == '#') {
			continue;
		}
		if (line.front() == '[' and line.back() == ']') {
			section = line.substr(1, line.size() - 2);
			if (section == "player") {
				d.players.emplace_back();
			}
			else if (section == "entity") {
				d.entities.emplace_back();
			}
			else if (section == "map") {
				has_map = true;
			}
			continue;
		}
		auto eq = line.find('=');
		if (eq == std::string::npos) {
			return fail("kein Schlüssel");
		}
		const std::string key = trim(line.substr(0, eq));
		const std::string value = trim(line.substr(eq + 1));
		std::istringstream v{value};
		if (section == "save") {
			if (key == "version") {
				if (not(v >> d.version)) {
					return fail("Version");
				}
				if (d.version > FORMAT_VERSION) {
					error = "Spielstand stammt aus einer neueren Version (" + std::to_string(d.version) + ")";
					return false;
				}
				if (d.version < 1) {
					return fail("Version");
				}
			}
			else if (key == "title") {
				d.title = value;
			}
			else if (key == "created") {
				d.created = value;
			}
			else if (key == "engine") {
				d.engine = value;
			}
			else if (key == "game_time") {
				if (not(v >> d.game_time) or not std::isfinite(d.game_time) or d.game_time < 0.0) {
					return fail("Spielzeit");
				}
			}
			else if (key == "generated_first") {
				if (not(v >> d.generated_first)) {
					return fail("generated_first");
				}
			}
			else if (key == "generated_last") {
				if (not(v >> d.generated_last)) {
					return fail("generated_last");
				}
			}
		}
		else if (section == "map") {
			MapSettings &m = d.map;
			if (key == "type") {
				has_type = true;
				if (value == "test") {
					m.type = map_type_t::TEST;
				}
				else if (value == "random") {
					m.type = map_type_t::RANDOM;
				}
				else {
					return fail("Kartentyp");
				}
			}
			else if (key == "biome") {
				if (not map_biome_parse(value, m.biome)) {
					return fail("Landschaft");
				}
			}
			else if (key == "seed") {
				if (not(v >> m.seed)) {
					return fail("Kartennummer");
				}
			}
			else if (key == "size") {
				if (not(v >> m.size) or m.size < 16 or m.size > 1024) {
					return fail("Kartengröße");
				}
			}
			else if (key == "max_trees") {
				if (not(v >> m.max_trees)) {
					return fail("max_trees");
				}
			}
			else if (key == "max_elevation") {
				if (not(v >> m.max_elevation) or not std::isfinite(m.max_elevation)) {
					return fail("max_elevation");
				}
			}
			else if (key == "skirmish") {
				int s = 0;
				if (not(v >> s)) {
					return fail("skirmish");
				}
				m.skirmish = s != 0;
			}
			else if (key == "ai_mode") {
				m.ai.mode = value == "on" ? ai_mode_t::ON : value == "off" ? ai_mode_t::OFF : ai_mode_t::AUTO;
			}
			else if (key == "ai_difficulty") {
				m.ai.difficulty = value == "normal" ? ai_difficulty_t::NORMAL : ai_difficulty_t::EASY;
			}
			else if (key == "ai_player") {
				uint64_t p = 0;
				if (not(v >> p)) {
					return fail("ai_player");
				}
				m.ai.player = p;
			}
			else if (key == "ai_first_attack") {
				double s = 0.0;
				if (not(v >> s)) {
					return fail("ai_first_attack");
				}
				m.ai.first_attack = s;
			}
			else if (key == "ai_attack_size") {
				size_t s = 0;
				if (not(v >> s)) {
					return fail("ai_attack_size");
				}
				m.ai.attack_size = s;
			}
			else if (key == "ai_seed") {
				if (not(v >> m.ai.seed)) {
					return fail("ai_seed");
				}
			}
		}
		else if (section == "player") {
			SavedPlayer &p = d.players.back();
			if (key == "id") {
				if (not(v >> p.id)) {
					return fail("Spieler-ID");
				}
			}
			else if (key == "resources") {
				if (not parse_resources(value, p.resources)) {
					return fail("Rohstoffe");
				}
			}
		}
		else if (section == "entity") {
			SavedEntity &e = d.entities.back();
			if (key == "id") {
				if (not(v >> e.id)) {
					return fail("Entity-ID");
				}
			}
			else if (key == "fqon") {
				e.fqon = value;
			}
			else if (key == "owner") {
				if (not(v >> e.owner)) {
					return fail("Besitzer");
				}
			}
			else if (key == "generated") {
				int g = 0;
				if (not(v >> g)) {
					return fail("generated");
				}
				e.generated = g != 0;
			}
			else if (key == "pos") {
				if (not(v >> e.ne >> e.se >> e.up) or not std::isfinite(e.ne) or not std::isfinite(e.se)
				    or not std::isfinite(e.up)) {
					return fail("Position");
				}
			}
			else if (key == "angle") {
				if (not(v >> e.angle) or not std::isfinite(e.angle)) {
					return fail("Winkel");
				}
			}
			else if (key == "health") {
				int64_t h = 0;
				if (not(v >> h)) {
					return fail("Trefferpunkte");
				}
				e.health = h;
			}
			else if (key == "harvest") {
				double h = 0.0;
				if (not(v >> h) or not std::isfinite(h) or h < 0.0) {
					return fail("Vorrat");
				}
				e.harvest = h;
			}
			else if (key == "carried") {
				if (not(v >> e.carried_type >> e.carried) or not std::isfinite(e.carried)) {
					return fail("Ladung");
				}
			}
			else if (key == "construction") {
				double c = 0.0;
				if (not(v >> c >> e.build_time) or not std::isfinite(c) or c < 0.0 or c > 1.0) {
					return fail("Baufortschritt");
				}
				e.construction = c;
			}
			else if (key == "queue") {
				SavedQueueItem q;
				if (not(v >> q.name >> q.fqon >> q.time >> q.remaining)) {
					return fail("Warteschlange");
				}
				std::string rest;
				std::getline(v, rest);
				if (not parse_resources(trim(rest), q.cost)) {
					return fail("Warteschlange (Kosten)");
				}
				e.queue.push_back(q);
			}
			else if (key == "rally") {
				std::array<double, 2> r{};
				if (not(v >> r[0] >> r[1])) {
					return fail("Sammelpunkt");
				}
				e.rally = r;
			}
			else if (key == "order") {
				if (not(v >> e.order >> e.order_target >> e.order_ne >> e.order_se)) {
					return fail("Befehl");
				}
			}
		}
		else if (section == "removed") {
			if (key == "ids") {
				uint64_t id = 0;
				while (v >> id) {
					d.removed.push_back(id);
				}
			}
		}
		else {
			// unknown section: ignored (forward compatibility)
		}
	}
	if (d.version == 0) {
		error = "Keine Spielstand-Datei (Kopfzeile fehlt)";
		return false;
	}
	if (not has_map or not has_type) {
		error = "Spielstand beschädigt (Karte fehlt)";
		return false;
	}
	for (const auto &e : d.entities) {
		if (e.fqon.empty()) {
			error = "Spielstand beschädigt (Entity " + std::to_string(e.id) + " ohne Typ)";
			return false;
		}
	}
	return true;
}

/**
 * Canonical form of a save for comparisons: title, date and engine dropped,
 * entities that are not generated map objects get new ids in a sorted order
 * (loading creates them again with other ids), order targets are mapped,
 * players and removed ids sorted, times and amounts rounded to microseconds,
 * facings rounded to 0.01 degrees. Units with an order are compared without
 * their facing: loading gives the order again and the unit turns to its target
 * at once, before it moves.
 *
 * @param with_orders Keep the orders (false: loading gives them again, the
 *                    unit may not have started yet when the state is compared).
 */
inline SaveData canonical(const SaveData &d, bool with_orders = false) {
	SaveData c = d;
	c.title.clear();
	c.created.clear();
	c.engine.clear();
	c.map.load_file.clear();
	auto round6 = [](double v) { return std::round(v * 1e6) / 1e6; };
	c.game_time = round6(c.game_time);
	std::sort(c.players.begin(), c.players.end(), [](const SavedPlayer &a, const SavedPlayer &b) { return a.id < b.id; });
	std::sort(c.removed.begin(), c.removed.end());
	auto key = [](const SavedEntity &e) {
		return std::make_tuple(e.generated ? 0 : 1, e.generated ? e.id : 0, e.fqon, e.owner, e.ne, e.se, e.up, e.angle);
	};
	std::stable_sort(c.entities.begin(), c.entities.end(), [&](const SavedEntity &a, const SavedEntity &b) {
		return key(a) < key(b);
	});
	std::vector<std::pair<uint64_t, uint64_t>> id_map;
	uint64_t next = 1000000;
	for (auto &e : c.entities) {
		if (not e.generated) {
			id_map.emplace_back(e.id, next);
			e.id = next++;
		}
	}
	for (auto &e : c.entities) {
		for (auto &q : e.queue) {
			q.remaining = q.remaining < 0.0 ? -1.0 : round6(q.remaining);
			q.time = round6(q.time);
		}
		if (e.harvest) {
			e.harvest = round6(*e.harvest);
		}
		e.carried = round6(e.carried);
		if (e.construction) {
			e.construction = round6(*e.construction);
		}
		e.angle = e.order.empty() ? std::round(e.angle * 100.0) / 100.0 : 0.0;
		if (not with_orders) {
			e.order.clear();
			e.order_target = 0;
			e.order_ne = e.order_se = 0.0;
		}
		else {
			for (const auto &[from, to] : id_map) {
				if (e.order_target == from) {
					e.order_target = to;
				}
			}
		}
	}
	return c;
}

/**
 * Hash of the game state in a save (FNV-1a 64 of the canonical text), to
 * compare states before and after loading.
 */
inline uint64_t state_hash(const SaveData &d, bool with_orders = false) {
	const std::string text = serialize(canonical(d, with_orders));
	uint64_t h = 1469598103934665603ULL;
	for (unsigned char ch : text) {
		h ^= ch;
		h *= 1099511628211ULL;
	}
	return h;
}

// ---- slots

/// file names of a slot ("slot-3.save", "autosave.save") and its meta file
inline std::string slot_base(int slot) {
	return slot == AUTOSAVE_SLOT ? "autosave" : slot == QUICK_SLOT ? "quicksave" : "slot-" + std::to_string(slot);
}
inline std::filesystem::path slot_file(const std::filesystem::path &dir, int slot) {
	return dir / (slot_base(slot) + ".save");
}
inline std::filesystem::path slot_meta_file(const std::filesystem::path &dir, int slot) {
	return dir / (slot_base(slot) + ".meta");
}
inline bool slot_valid(int slot) {
	return slot >= 0 and slot <= QUICK_SLOT;
}

/// what the slot lists show
struct SlotInfo {
	int slot = 0;
	bool exists = false;
	std::string title;
	std::string created;
	std::string map;
	double game_time = 0.0;
	std::string file;
	/// "Slot 2: Zufallskarte #3 · 12:34 · 2026-10-08 19:30", "Slot 2: leer"
	std::string label() const {
		std::string name = slot == AUTOSAVE_SLOT ? "Automatisch"
		                   : slot == QUICK_SLOT  ? "Schnell (F5)"
		                                         : "Slot " + std::to_string(slot);
		if (not exists) {
			return name + ": leer";
		}
		return name + ": " + (map.empty() ? title : map) + " · " + time_text(game_time)
		       + (created.empty() ? "" : " · " + created);
	}
};

/// meta file text (small, read by the slot lists without parsing the save)
inline std::string meta_text(const SaveData &d) {
	std::ostringstream out;
	out << "title = " << d.title << "\n";
	out << "created = " << d.created << "\n";
	out << "map = " << map_text(d.map) << "\n";
	out << "game_time = " << fmt_double(d.game_time) << "\n";
	out << "version = " << d.version << "\n";
	return out.str();
}

inline bool read_text(const std::filesystem::path &file, std::string &text) {
	std::ifstream in{file, std::ios::binary};
	if (not in) {
		return false;
	}
	text.assign(std::istreambuf_iterator<char>(in), {});
	return true;
}

/// atomic write (tmp + rename)
inline bool write_text(const std::filesystem::path &file, const std::string &text, std::string &error) {
	std::error_code ec;
	std::filesystem::create_directories(file.parent_path(), ec);
	const std::filesystem::path tmp = file.string() + ".tmp";
	{
		std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
		if (not out) {
			error = "Datei nicht schreibbar: " + file.string();
			return false;
		}
		out << text;
		if (not out) {
			error = "Schreiben fehlgeschlagen: " + file.string();
			return false;
		}
	}
	std::filesystem::rename(tmp, file, ec);
	if (ec) {
		error = "Umbenennen fehlgeschlagen: " + file.string() + " (" + ec.message() + ")";
		return false;
	}
	return true;
}

/// write save and meta file of a slot
inline bool write_slot(const std::filesystem::path &dir, int slot, const SaveData &d, std::string &error) {
	if (not slot_valid(slot)) {
		error = "Ungültiger Slot " + std::to_string(slot);
		return false;
	}
	if (not write_text(slot_file(dir, slot), serialize(d), error)) {
		return false;
	}
	return write_text(slot_meta_file(dir, slot), meta_text(d), error);
}

/// read the meta file of a slot (exists = false if there is no save)
inline SlotInfo read_slot_info(const std::filesystem::path &dir, int slot) {
	SlotInfo info;
	info.slot = slot;
	const auto file = slot_file(dir, slot);
	std::error_code ec;
	info.exists = std::filesystem::exists(file, ec);
	info.file = file.string();
	if (not info.exists) {
		return info;
	}
	std::string text;
	if (read_text(slot_meta_file(dir, slot), text)) {
		std::istringstream in{text};
		std::string line;
		while (std::getline(in, line)) {
			auto eq = line.find('=');
			if (eq == std::string::npos) {
				continue;
			}
			const std::string key = trim(line.substr(0, eq));
			const std::string value = trim(line.substr(eq + 1));
			if (key == "title") {
				info.title = value;
			}
			else if (key == "created") {
				info.created = value;
			}
			else if (key == "map") {
				info.map = value;
			}
			else if (key == "game_time") {
				info.game_time = std::atof(value.c_str());
			}
		}
	}
	if (info.title.empty() and info.map.empty()) {
		info.title = "Spielstand";
	}
	return info;
}

/// all slots (1..SLOT_COUNT, then the quick save and the autosave)
inline std::vector<SlotInfo> list_slots(const std::filesystem::path &dir) {
	std::vector<SlotInfo> out;
	for (int slot = 1; slot <= SLOT_COUNT; ++slot) {
		out.push_back(read_slot_info(dir, slot));
	}
	out.push_back(read_slot_info(dir, QUICK_SLOT));
	out.push_back(read_slot_info(dir, AUTOSAVE_SLOT));
	return out;
}

/// read and parse a save file (false with a German error message)
inline bool read_save(const std::filesystem::path &file, SaveData &d, std::string &error) {
	std::string text;
	if (not read_text(file, text)) {
		error = "Spielstand nicht gefunden: " + file.filename().string();
		return false;
	}
	return parse(text, d, error);
}

/**
 * Autosave timer: due every AUTOSAVE_SECONDS of simulation time, not while the
 * clock stands still, never twice for the same interval.
 */
struct AutosaveTimer {
	double period = AUTOSAVE_SECONDS;
	double last = 0.0;
	/// the first call starts the interval (a loaded game is not saved again at once)
	bool started = false;

	/// true once per interval; game_seconds going backwards (new game) resets the timer
	bool due(double game_seconds) {
		if (not this->started) {
			this->started = std::isfinite(game_seconds);
			this->last = this->started and game_seconds > 0.0 ? game_seconds : 0.0;
			return false;
		}
		if (not std::isfinite(game_seconds) or game_seconds < this->last) {
			this->last = std::isfinite(game_seconds) and game_seconds > 0.0 ? game_seconds : 0.0;
			return false;
		}
		if (game_seconds - this->last >= this->period) {
			this->last = game_seconds;
			return true;
		}
		return false;
	}
	void reset(double game_seconds = 0.0) {
		this->last = std::isfinite(game_seconds) and game_seconds > 0.0 ? game_seconds : 0.0;
		this->started = true;
	}
};

} // namespace openage::gamestate::save
