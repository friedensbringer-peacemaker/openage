// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "command.h"

#include <algorithm>

#include <eigen3/Eigen/Dense>

#include "log/log.h"
#include "log/message.h"

#include "gamestate/combat/combat_state.h"
#include "gamestate/component/internal/ownership.h"
#include "gamestate/component/types.h"
#include "gamestate/game_entity.h"
#include "gamestate/game_state.h"
#include "gamestate/pick.h"


namespace openage::gamestate::combat {

bool handle_command(const std::shared_ptr<GameState> &state,
                    const time::time_t &time,
                    component::command::command_t type,
                    std::vector<entity_id_t> &ids,
                    const coord::phys3 &target,
                    const openage::event::EventHandler::param_map &params) {
	using component::command::command_t;
	auto combat = state->get_combat();

	// dead (death animation) or removed entities take no commands
	std::erase_if(ids, [&](entity_id_t id) {
		return not state->get_game_entities().contains(id) or combat->is_dead(id);
	});
	if (ids.empty()) {
		return false;
	}

	if (type != command_t::MOVE and type != command_t::ATTACK) {
		// idle, gather, ...: stop attacking
		for (auto id : ids) {
			combat->cancel_attack(id, time);
		}
		return false;
	}

	// enemy: given (ATTACK), or under the cursor (enemy of the first commanded entity's owner)
	std::optional<entity_id_t> enemy;
	if (type == command_t::ATTACK and params.contains("target_entity")) {
		enemy = params.get<entity_id_t>("target_entity", 0);
	}
	else {
		auto owner = std::dynamic_pointer_cast<component::Ownership>(
			state->get_game_entity(ids.front())->get_component(component::component_t::OWNERSHIP));
		PickRequest request;
		request.terrain = target;
		if (params.contains("camera_matrix") and params.contains("pick_ndc")) {
			request.camera = params.get<Eigen::Matrix4f>("camera_matrix", Eigen::Matrix4f::Identity());
			request.ndc = params.get<Eigen::Vector2f>("pick_ndc", Eigen::Vector2f{0.0f, 0.0f});
			// a few pixels of slack (NDC spans 2 over the image)
			request.slack = 0.01;
		}
		auto picked = combat->pick_enemy(state, time, owner->get_owners().get(time), request);
		if (picked) {
			enemy = picked->get_id();
		}
	}

	if (not enemy) {
		// plain move: stop attacking
		for (auto id : ids) {
			combat->cancel_attack(id, time);
		}
		if (type == command_t::ATTACK) {
			log::log(INFO << "Attack command: no enemy at tile (" << target.ne.to_float() << ", "
			              << target.se.to_float() << ")");
			ids.clear();
		}
		return false;
	}

	size_t attacking = 0;
	std::erase_if(ids, [&](entity_id_t id) {
		if (combat->order_attack(state, id, *enemy, time)) {
			attacking += 1;
			return true;
		}
		combat->cancel_attack(id, time);
		// cannot attack: an explicit attack command does nothing, a right click moves
		return type == command_t::ATTACK;
	});
	log::log(INFO << "Attack command: " << attacking << " entities attack entity " << *enemy
	              << (ids.empty() ? std::string{} : ", " + std::to_string(ids.size()) + " move"));
	return true;
}

} // namespace openage::gamestate::combat
