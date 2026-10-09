// Copyright 2026-2026 the openage authors. See copying.md for legal info.

/**
 * Host test of the save game format (XR fork, gamestate/save_format.h), without
 * the engine: round trip of all fields (parameter sweep), deterministic text
 * and state hash, damaged/too new/foreign files are rejected with a message
 * instead of crashing, slot files and meta files, slot labels, autosave timer.
 *
 *   openage-save-format-check
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "gamestate/save_format.h"

using namespace openage::gamestate;
using namespace openage::gamestate::save;

namespace {

int failures = 0;
size_t checks = 0;

#define CHECK(cond, ...)                               \
	do {                                               \
		++checks;                                      \
		if (not(cond)) {                               \
			++failures;                                \
			std::printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
			std::printf(__VA_ARGS__);                  \
			std::printf("\n");                         \
		}                                              \
	} while (0)

SaveData sample(uint32_t seed, size_t entities) {
	SaveData d;
	d.title = "Slot 2: Zufallskarte #" + std::to_string(seed);
	d.created = "2026-10-08 19:30";
	d.engine = "openage test";
	d.game_time = 123.456 + seed;
	d.map.type = map_type_t::RANDOM;
	d.map.seed = seed;
	d.map.size = 64;
	d.map.biome = static_cast<map_biome_t>(seed % static_cast<uint32_t>(map_biome_t::COUNT));
	d.map.skirmish = seed % 2 == 1;
	d.map.ai.mode = seed % 3 == 0 ? ai_mode_t::ON : seed % 3 == 1 ? ai_mode_t::OFF : ai_mode_t::AUTO;
	d.map.ai.difficulty = seed % 2 == 0 ? ai_difficulty_t::NORMAL : ai_difficulty_t::EASY;
	if (seed % 4 == 0) {
		d.map.ai.first_attack = 480.5;
		d.map.ai.attack_size = 7;
		d.map.ai.player = 1;
	}
	d.map.ai.seed = seed * 7;
	d.map.max_trees = 600;
	d.map.max_elevation = 4.0f;
	d.generated_first = 2;
	d.generated_last = 2 + entities;
	for (uint64_t p = 0; p < 3; ++p) {
		SavedPlayer sp;
		sp.id = p;
		sp.resources = {200.0 + p, 150.25, 100.0 / 3.0, 0.0};
		d.players.push_back(sp);
	}
	for (size_t i = 0; i < entities; ++i) {
		SavedEntity e;
		e.id = 2 + i;
		e.fqon = i % 2 == 0 ? "hd_base.data.game_entity.generic.villager.villager.Villager"
		                    : "hd_base.data.game_entity.generic.town_center.town_center.TownCenter";
		e.owner = i % 3;
		e.generated = i < entities / 2;
		e.ne = 10.0 + i * 0.015625;
		e.se = 20.5 - i * 0.25;
		e.up = i % 5 == 0 ? 0.0 : 1.0 / 3.0;
		e.angle = (i * 37) % 360 + 0.5;
		if (i % 3 != 2) {
			e.health = static_cast<int64_t>(25 + i);
		}
		if (i % 4 == 1) {
			e.harvest = 99.5 - i;
		}
		if (i % 5 == 2) {
			e.carried = 7.5;
			e.carried_type = 1;
		}
		if (i % 7 == 3) {
			e.construction = 0.25;
			e.build_time = 35.0;
		}
		if (i % 2 == 1) {
			for (size_t q = 0; q < i % 4; ++q) {
				SavedQueueItem item;
				item.name = "Villager";
				item.fqon = "hd_base.data.game_entity.generic.villager.villager.Villager";
				item.cost = {50.0, 0.0, 0.0, 0.0};
				item.time = 25.0;
				item.remaining = q == 0 ? 12.75 : -1.0;
				e.queue.push_back(item);
			}
			e.rally = std::array<double, 2>{12.0, 13.5};
		}
		switch (i % 6) {
		case 0:
			e.order = "gather";
			e.order_target = 40;
			break;
		case 1:
			e.order = "build";
			e.order_target = 41;
			break;
		case 2:
			e.order = "attack";
			e.order_target = 42;
			break;
		case 3:
			e.order = "move";
			e.order_ne = 5.5;
			e.order_se = 6.25;
			break;
		default:
			break;
		}
		d.entities.push_back(e);
	}
	for (size_t i = 0; i < entities / 3; ++i) {
		d.removed.push_back(100 + i);
	}
	return d;
}

bool same(const SaveData &a, const SaveData &b) {
	return serialize(a) == serialize(b);
}

void check_roundtrip() {
	std::printf("== round trip\n");
	size_t total = 0;
	for (uint32_t seed = 1; seed <= 24; ++seed) {
		for (size_t n : {0u, 1u, 7u, 40u}) {
			SaveData d = sample(seed, n);
			std::string text = serialize(d);
			SaveData back;
			std::string error;
			bool ok = parse(text, back, error);
			CHECK(ok, "parse seed %u n %zu: %s", seed, n, error.c_str());
			CHECK(same(d, back), "round trip seed %u n %zu differs", seed, n);
			CHECK(serialize(back) == text, "serialize(parse(text)) == text (seed %u n %zu)", seed, n);
			CHECK(state_hash(d) == state_hash(back), "hash equal (seed %u n %zu)", seed, n);
			CHECK(back.map.load_file.empty(), "load_file never stored");
			// title/date do not change the state hash, a resource does
			SaveData other = d;
			other.title = "x";
			other.created = "y";
			CHECK(state_hash(other) == state_hash(d), "hash ignores title/date");
			if (not d.players.empty()) {
				other.players[0].resources[0] += 1.0;
				CHECK(state_hash(other) != state_hash(d), "hash sees resources");
			}
			total += n;
		}
	}
	std::printf("  %zu entities round-tripped\n", total);
	// field by field for one entity
	SaveData d = sample(4, 12);
	SaveData back;
	std::string error;
	CHECK(parse(serialize(d), back, error), "parse");
	CHECK(back.entities.size() == 12 and back.players.size() == 3 and back.removed.size() == 4, "counts");
	const SavedEntity &e = back.entities[3];
	CHECK(e.id == 5 and e.owner == 0 and e.generated and e.order == "move" and e.order_ne == 5.5, "entity 3");
	CHECK(back.entities[1].queue.size() == 1 and back.entities[1].queue[0].remaining == 12.75
	          and back.entities[1].queue[0].cost[0] == 50.0 and back.entities[1].rally
	          and (*back.entities[1].rally)[1] == 13.5,
	      "queue and rally");
	CHECK(back.entities[1].health and *back.entities[1].health == 26 and back.entities[1].harvest
	          and *back.entities[1].harvest == 98.5,
	      "health/harvest");
	CHECK(back.entities[3].construction and *back.entities[3].construction == 0.25 and back.entities[3].build_time == 35.0,
	      "construction");
	CHECK(back.entities[2].carried == 7.5 and back.entities[2].carried_type == 1, "carried");
	CHECK(back.map.ai.first_attack and *back.map.ai.first_attack == 480.5 and back.map.ai.attack_size
	          and *back.map.ai.attack_size == 7 and back.map.ai.player and *back.map.ai.player == 1,
	      "ai settings");
	CHECK(back.map.type == map_type_t::RANDOM and back.map.seed == 4 and back.map.size == 64, "map");
	CHECK(std::fabs(back.game_time - d.game_time) < 1e-12, "game time exact");
}

void check_damaged() {
	std::printf("== damaged files\n");
	SaveData d = sample(3, 5);
	const std::string text = serialize(d);
	SaveData back;
	std::string error;
	// empty, foreign, truncated, too new, bad numbers
	CHECK(not parse("", back, error) and not error.empty(), "empty rejected: %s", error.c_str());
	CHECK(not parse("hello world\nfoo", back, error), "foreign text rejected: %s", error.c_str());
	CHECK(not parse("[save]\nversion = 1\n", back, error) and error.find("Karte") != std::string::npos,
	      "no map rejected: %s", error.c_str());
	CHECK(not parse("[save]\nversion = 99\n[map]\ntype = random\n", back, error)
	          and error.find("neueren") != std::string::npos,
	      "too new rejected: %s", error.c_str());
	CHECK(not parse("[save]\nversion = 1\ngame_time = nan\n[map]\ntype = random\n", back, error),
	      "nan time rejected: %s", error.c_str());
	CHECK(not parse("[save]\nversion = 1\n[map]\ntype = moon\n", back, error), "bad map type rejected");
	CHECK(not parse("[save]\nversion = 1\n[map]\ntype = random\nsize = 7\n", back, error), "bad size rejected");
	CHECK(not parse("[save]\nversion = 1\n[map]\ntype = random\n[entity]\nid = 3\n", back, error)
	          and error.find("ohne Typ") != std::string::npos,
	      "entity without type rejected: %s", error.c_str());
	// every truncation of the file either parses (valid prefix) or fails with a message, never crashes
	size_t parsed = 0;
	size_t rejected = 0;
	for (size_t cut = 0; cut < text.size(); cut += 7) {
		SaveData t;
		std::string err;
		if (parse(text.substr(0, cut), t, err)) {
			++parsed;
		}
		else {
			++rejected;
			CHECK(not err.empty(), "truncated at %zu: message", cut);
		}
	}
	std::printf("  truncations: %zu parse as a prefix, %zu rejected\n", parsed, rejected);
	// unknown keys and sections are ignored
	CHECK(parse(text + "[future]\nthing = 1\n[save]\nnew_key = 2\n", back, error) and same(back, d),
	      "unknown keys ignored: %s", error.c_str());
	// garbage line
	CHECK(not parse(text + "garbage without equals\n", back, error) and error.find("Zeile") != std::string::npos,
	      "garbage line rejected with line number: %s", error.c_str());
	// windows line endings
	std::string crlf;
	for (char c : text) {
		if (c == '\n') {
			crlf += "\r\n";
		}
		else {
			crlf += c;
		}
	}
	CHECK(parse(crlf, back, error) and same(back, d), "CRLF accepted");
}

void check_slots() {
	std::printf("== slots\n");
	const auto dir = std::filesystem::temp_directory_path() / "xr-ages-save-format-check";
	std::filesystem::remove_all(dir);
	auto slots = list_slots(dir);
	CHECK(slots.size() == static_cast<size_t>(SLOT_COUNT) + 2, "slot count %zu", slots.size());
	CHECK(slots[SLOT_COUNT].slot == QUICK_SLOT and slot_file(dir, QUICK_SLOT).filename() == "quicksave.save",
	      "quick slot");
	for (const auto &s : slots) {
		CHECK(not s.exists, "slot %d empty", s.slot);
		CHECK(s.label().find("leer") != std::string::npos, "label %s", s.label().c_str());
	}
	CHECK(slots.back().slot == AUTOSAVE_SLOT and slots.back().label().rfind("Automatisch", 0) == 0, "autosave last");
	CHECK(slot_file(dir, 3).filename() == "slot-3.save" and slot_meta_file(dir, 0).filename() == "autosave.meta",
	      "file names");
	CHECK(slot_valid(0) and slot_valid(5) and slot_valid(QUICK_SLOT) and not slot_valid(QUICK_SLOT + 1)
	          and not slot_valid(-1),
	      "slot_valid");
	std::string error;
	SaveData d = sample(5, 9);
	d.game_time = 754.0;
	CHECK(write_slot(dir, 2, d, error), "write slot 2: %s", error.c_str());
	CHECK(write_slot(dir, AUTOSAVE_SLOT, d, error), "write autosave: %s", error.c_str());
	CHECK(not write_slot(dir, 9, d, error), "slot 9 refused");
	CHECK(write_slot(dir, QUICK_SLOT, d, error) and read_slot_info(dir, QUICK_SLOT).exists, "quick save");
	auto info = read_slot_info(dir, 2);
	CHECK(info.exists and info.title == d.title and info.created == d.created and info.game_time == 754.0,
	      "meta read: %s", info.label().c_str());
	CHECK(info.label().find("12:34") != std::string::npos and info.label().find("2026-10-08") != std::string::npos,
	      "label with time and date: %s", info.label().c_str());
	CHECK(info.map.find("#5") != std::string::npos, "label map: %s", info.map.c_str());
	SaveData back;
	CHECK(read_save(slot_file(dir, 2), back, error) and same(back, d), "read_save slot 2");
	CHECK(not read_save(slot_file(dir, 4), back, error) and error.find("nicht gefunden") != std::string::npos,
	      "missing slot: %s", error.c_str());
	// damaged file on disk
	std::string junk = "[save]\nversion = 1\n[map]\ntype = random\nsize = 1\n";
	CHECK(write_text(slot_file(dir, 4), junk, error), "write junk");
	CHECK(not read_save(slot_file(dir, 4), back, error), "junk rejected: %s", error.c_str());
	// meta missing: list still works
	std::filesystem::remove(slot_meta_file(dir, 2));
	info = read_slot_info(dir, 2);
	CHECK(info.exists and info.title == "Spielstand", "meta missing -> default title");
	std::filesystem::remove_all(dir);
	CHECK(map_text(d.map).find("Gefecht #5") == 0, "map text: %s", map_text(d.map).c_str());
	MapSettings test;
	CHECK(map_text(test) == "Testkarte", "test map text");
	CHECK(time_text(3723.0) == "1:02:03" and time_text(59.9) == "0:59" and time_text(-1.0) == "0:00", "time text");
}

void check_autosave() {
	std::printf("== autosave timer\n");
	AutosaveTimer t;
	CHECK(t.period == AUTOSAVE_SECONDS and AUTOSAVE_SECONDS == 300.0, "5 min");
	size_t fired = 0;
	for (double s = 0.0; s <= 1800.0; s += 0.25) {
		if (t.due(s)) {
			++fired;
			CHECK(std::fmod(s, 300.0) == 0.0, "fires on the interval (%.2f)", s);
		}
	}
	CHECK(fired == 6, "6 autosaves in 30 min (%zu)", fired);
	// paused clock: same time again -> nothing
	CHECK(not t.due(1800.0) and not t.due(1800.0), "no repeat while paused");
	// a loaded game (first call at 1000 s) is not saved at once
	AutosaveTimer l;
	CHECK(not l.due(1000.0) and not l.due(1200.0) and l.due(1300.0), "loaded game: first autosave 300 s later");
	// new game: time goes back -> timer restarts
	CHECK(not t.due(3.0), "reset on a new game");
	CHECK(not t.due(299.0) and t.due(303.0), "next after 300 s of the new game");
	CHECK(not t.due(NAN), "nan ignored");
	AutosaveTimer u;
	u.period = 10.0;
	fired = 0;
	for (double s = 0.0; s < 100.0; s += 3.0) {
		fired += u.due(s) ? 1 : 0;
	}
	CHECK(fired == 8, "custom period: first call starts the interval (%zu)", fired);
}

} // namespace

int main() {
	check_roundtrip();
	check_damaged();
	check_slots();
	check_autosave();
	std::printf("save format check: %zu checks, %d failures\n", checks, failures);
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
