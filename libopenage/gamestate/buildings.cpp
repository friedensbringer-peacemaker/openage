// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "buildings.h"

#include <algorithm>
#include <cmath>

#include "log/log.h"
#include "log/message.h"

#include "gamestate/combat/combat_state.h"
#include "gamestate/game_state.h"
#include "gamestate/map.h"


namespace openage::gamestate::building {

namespace {

/// sample distance along a path when looking for gates (tiles)
constexpr double CLIP_STEP = 0.25;

std::vector<coord::tile> to_tiles(const std::vector<tile_t> &tiles) {
	std::vector<coord::tile> out;
	out.reserve(tiles.size());
	for (const auto &[x, y] : tiles) {
		out.push_back(coord::tile{static_cast<coord::tile_t>(x), static_cast<coord::tile_t>(y)});
	}
	return out;
}

} // namespace


void BuildingState::start(const std::shared_ptr<GameState> &state) {
	if (this->started) {
		return;
	}
	this->started = true;
	auto combat = state->get_combat();
	if (combat == nullptr) {
		return;
	}
	std::weak_ptr<BuildingState> weak = this->shared_from_this();
	// the gate leaves the game: its 4 tiles are freed (not the square of its 4x1 hitbox)
	combat->set_footprint_hook([weak](entity_id_t id) -> std::optional<std::vector<coord::tile>> {
		auto self = weak.lock();
		if (self == nullptr) {
			return std::nullopt;
		}
		auto tiles = self->footprint(id);
		self->forget(id);
		return tiles;
	});
}

double BuildingState::build_speed(player_id_t player) const {
	auto it = this->speed.find(player);
	return it == this->speed.end() ? 1.0 : it->second;
}

void BuildingState::set_build_speed(player_id_t player, double k) {
	this->speed[player] = k;
	log::log(INFO << "Buildings: build speed of player " << player << " is " << k);
}

void BuildingState::add_gate(entity_id_t id, const Gate &gate) {
	this->forget(id);
	this->gate_list[id] = gate;
	for (const auto &[x, y] : gate.tiles()) {
		this->gate_tiles_map[{x, y}] = id;
	}
	this->publish();
	log::log(INFO << "Buildings: gate " << id << " of player " << gate.owner << " at tile (" << gate.anchor.first << ", "
	              << gate.anchor.second << "), direction " << static_cast<int>(gate.dir)
	              << (gate.locked ? ", locked" : "") << (gate.finished ? ", finished" : ", foundation"));
}

void BuildingState::gate_finished(const std::shared_ptr<GameState> &state, entity_id_t id, const time::time_t &time) {
	auto it = this->gate_list.find(id);
	if (it == this->gate_list.end()) {
		return;
	}
	it->second.finished = true;
	if (not it->second.locked) {
		// open: the pathfinder routes through it, foreign paths are cut (clip_path)
		state->get_map()->unblock_tiles(to_tiles(it->second.tiles()), time);
	}
	log::log(INFO << "Buildings: gate " << id << " finished, " << (it->second.locked ? "locked" : "open"));
}

const Gate *BuildingState::gate(entity_id_t id) const {
	auto it = this->gate_list.find(id);
	return it == this->gate_list.end() ? nullptr : &it->second;
}

const std::map<entity_id_t, Gate> &BuildingState::gates() const {
	return this->gate_list;
}

bool BuildingState::set_locked(const std::shared_ptr<GameState> &state,
                               entity_id_t id,
                               player_id_t player,
                               bool locked,
                               const time::time_t &time) {
	auto it = this->gate_list.find(id);
	if (it == this->gate_list.end() or it->second.owner != player) {
		return false;
	}
	auto &gate = it->second;
	if (gate.locked == locked) {
		return true;
	}
	gate.locked = locked;
	if (gate.finished) {
		auto map = state->get_map();
		const auto tiles = to_tiles(gate.tiles());
		if (locked) {
			for (const auto &t : tiles) {
				map->block_tile(t, time);
			}
		}
		else {
			map->unblock_tiles(tiles, time);
		}
	}
	this->publish();
	log::log(INFO << "Buildings: gate " << id << " of player " << player << (locked ? " locked" : " unlocked"));
	return true;
}

bool BuildingState::blocks(player_id_t mover, long ne, long se) const {
	auto it = this->gate_tiles_map.find({ne, se});
	if (it == this->gate_tiles_map.end()) {
		return false;
	}
	const auto &gate = this->gate_list.at(it->second);
	// own units pass (no alliances in this engine yet: allies are the own player), others not
	return not gate_passable(gate.owner == mover, gate.locked);
}

std::vector<coord::phys3> BuildingState::clip_path(player_id_t mover, const std::vector<coord::phys3> &waypoints) const {
	if (this->gate_tiles_map.empty() or waypoints.size() < 2) {
		return waypoints;
	}
	std::vector<coord::phys3> out;
	out.push_back(waypoints.front());
	bool leaving = true;  // samples at the start on a gate tile (the unit stands there) are allowed
	for (size_t i = 1; i < waypoints.size(); ++i) {
		const auto &a = waypoints[i - 1];
		const auto &b = waypoints[i];
		const double ane = a.ne.to_double();
		const double ase = a.se.to_double();
		const double dne = b.ne.to_double() - ane;
		const double dse = b.se.to_double() - ase;
		const double length = std::hypot(dne, dse);
		const int samples = std::max(1, static_cast<int>(std::ceil(length / CLIP_STEP)));
		std::optional<coord::phys3> last_free;
		for (int s = 0; s <= samples; ++s) {
			const double f = static_cast<double>(s) / samples;
			const double ne = ane + f * dne;
			const double se = ase + f * dse;
			const bool blocked = this->blocks(mover, static_cast<long>(std::floor(ne)), static_cast<long>(std::floor(se)));
			if (blocked and not leaving) {
				if (last_free) {
					out.push_back(*last_free);
				}
				log::log(DBG << "Buildings: path of player " << mover << " cut in front of a gate at ("
				             << ne << ", " << se << ")");
				return out;
			}
			if (not blocked) {
				leaving = false;
				const double up = a.up.to_double() + f * (b.up.to_double() - a.up.to_double());
				last_free = coord::phys3{coord::phys_t{ne}, coord::phys_t{se}, coord::phys_t{up}};
			}
		}
		out.push_back(b);
	}
	return out;
}

std::optional<std::vector<coord::tile>> BuildingState::footprint(entity_id_t id) const {
	auto it = this->gate_list.find(id);
	if (it == this->gate_list.end()) {
		return std::nullopt;
	}
	return to_tiles(it->second.tiles());
}

std::optional<bool> BuildingState::is_locked_shared(entity_id_t id) const {
	std::lock_guard<std::mutex> lock{this->shared_mutex};
	auto it = this->locked_shared.find(id);
	if (it == this->locked_shared.end()) {
		return std::nullopt;
	}
	return it->second;
}

void BuildingState::forget(entity_id_t id) {
	auto it = this->gate_list.find(id);
	if (it == this->gate_list.end()) {
		return;
	}
	for (const auto &[x, y] : it->second.tiles()) {
		auto t = this->gate_tiles_map.find({x, y});
		if (t != this->gate_tiles_map.end() and t->second == id) {
			this->gate_tiles_map.erase(t);
		}
	}
	this->gate_list.erase(it);
	this->publish();
}

void BuildingState::post(player_id_t player, const std::string &text, bool warn) {
	this->messages.push_back(Message{player, text, warn});
	if (this->messages.size() > 32) {
		this->messages.erase(this->messages.begin());
	}
}

std::vector<Message> BuildingState::take_messages() {
	std::vector<Message> out;
	out.swap(this->messages);
	return out;
}

void BuildingState::publish() {
	std::lock_guard<std::mutex> lock{this->shared_mutex};
	this->locked_shared.clear();
	for (const auto &[id, gate] : this->gate_list) {
		this->locked_shared[id] = gate.locked;
	}
}

} // namespace openage::gamestate::building
