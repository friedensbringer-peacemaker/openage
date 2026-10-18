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

namespace {

PickRequest make_request(const coord::phys3 &target, const openage::event::EventHandler::param_map &params) {
	PickRequest request;
	request.terrain = target;
	if (params.contains("camera_matrix") and params.contains("pick_ndc")) {
		request.camera = params.get<Eigen::Matrix4f>("camera_matrix", Eigen::Matrix4f::Identity());
		request.ndc = params.get<Eigen::Vector2f>("pick_ndc", Eigen::Vector2f{0.0f, 0.0f});
		// a few pixels of slack (NDC spans 2 over the image)
		request.slack = 0.01;
	}
	return request;
}

player_id_t owner_of(const std::shared_ptr<GameState> &state, entity_id_t id, const time::time_t &time) {
	auto owner = std::dynamic_pointer_cast<component::Ownership>(
		state->get_game_entity(id)->get_component(component::component_t::OWNERSHIP));
	return owner->get_owners().get(time);
}

const char *command_name(component::command::command_t type) {
	using component::command::command_t;
	switch (type) {
	case command_t::PATROL:
		return "patrol";
	case command_t::GUARD:
		return "guard";
	case command_t::FOLLOW:
		return "follow";
	case command_t::ATTACK_MOVE:
		return "attack move";
	case command_t::GARRISON:
		return "garrison";
	case command_t::UNGARRISON:
		return "ungarrison";
	case command_t::STOP:
		return "stop";
	case command_t::STANCE:
		return "stance";
	case command_t::FORMATION:
		return "formation";
	case command_t::KILL:
		return "delete";
	case command_t::BELL:
		return "town bell";
	default:
		return "?";
	}
}

/**
 * Commands of round W4 (docs/aoe2-spec 06 §6.1/6.3, 07 §7.5): they never fall
 * through to the move/gather handling of the caller.
 */
bool handle_w4(const std::shared_ptr<GameState> &state,
               const time::time_t &time,
               component::command::command_t type,
               std::vector<entity_id_t> &ids,
               const coord::phys3 &target,
               const openage::event::EventHandler::param_map &params) {
	using component::command::command_t;
	auto combat = state->get_combat();
	const int arg = params.get<int>("combat_arg", 0);
	size_t done = 0;
	std::string detail;
	switch (type) {
	case command_t::STOP:
		for (auto id : ids) {
			combat->stop(state, id, time);
			done += 1;
		}
		break;
	case command_t::STANCE: {
		const auto stance = static_cast<stance_t>(std::clamp(arg, 0, static_cast<int>(STANCE_COUNT) - 1));
		for (auto id : ids) {
			done += combat->set_stance(id, stance) ? 1 : 0;
		}
		detail = to_string(stance);
		break;
	}
	case command_t::FORMATION: {
		const auto formation = static_cast<formation_t>(std::clamp(arg, 0, static_cast<int>(formation_t::FLANK)));
		for (auto id : ids) {
			done += combat->set_formation(id, formation) ? 1 : 0;
		}
		detail = to_string(formation);
		break;
	}
	case command_t::KILL:
		for (auto id : ids) {
			combat->kill(state, id, time);
			done += 1;
		}
		break;
	case command_t::UNGARRISON:
		for (auto id : ids) {
			if (not combat->occupants(id).empty()) {
				combat->ungarrison_all(state, id, time);
				done += 1;
			}
		}
		break;
	case command_t::BELL:
		for (auto id : ids) {
			done += combat->ring_bell(state, id, arg != 0, time) ? 1 : 0;
		}
		detail = arg != 0 ? "on" : "off";
		break;
	case command_t::PATROL:
		for (auto id : ids) {
			done += combat->order_patrol(state, id, target, time) ? 1 : 0;
		}
		break;
	case command_t::ATTACK_MOVE:
		for (auto id : ids) {
			done += combat->order_attack_move(state, id, target, time) ? 1 : 0;
		}
		break;
	case command_t::GUARD:
	case command_t::FOLLOW: {
		auto entity = params.contains("target_entity")
		                  ? (state->get_game_entities().contains(params.get<entity_id_t>("target_entity", 0))
		                         ? state->get_game_entity(params.get<entity_id_t>("target_entity", 0))
		                         : nullptr)
		                  : combat->pick_unit(state, time, make_request(target, params), ids.size() == 1 ? ids.front() : 0);
		if (entity == nullptr) {
			detail = "nothing under the cursor";
			break;
		}
		for (auto id : ids) {
			done += (type == command_t::GUARD ? combat->order_guard(state, id, entity->get_id(), time)
			                                  : combat->order_follow(state, id, entity->get_id(), time))
			            ? 1
			            : 0;
		}
		detail = "entity " + std::to_string(entity->get_id());
		break;
	}
	case command_t::GARRISON: {
		std::shared_ptr<GameEntity> building;
		if (params.contains("target_entity")) {
			auto id = params.get<entity_id_t>("target_entity", 0);
			if (state->get_game_entities().contains(id)) {
				building = state->get_game_entity(id);
			}
		}
		else {
			building = combat->pick_shelter(state, time, owner_of(state, ids.front(), time), make_request(target, params));
		}
		if (building == nullptr) {
			detail = "no own building with garrison places under the cursor";
			break;
		}
		for (auto id : ids) {
			done += combat->order_garrison(state, id, building->get_id(), time) ? 1 : 0;
		}
		detail = "building " + std::to_string(building->get_id());
		break;
	}
	default:
		return false;
	}
	log::log(INFO << "Combat command " << command_name(type) << (detail.empty() ? std::string{} : " (" + detail + ")")
	              << ": " << done << " of " << ids.size() << " entities");
	ids.clear();
	return true;
}

} // namespace


bool handle_command(const std::shared_ptr<GameState> &state,
                    const time::time_t &time,
                    component::command::command_t type,
                    std::vector<entity_id_t> &ids,
                    const coord::phys3 &target,
                    const openage::event::EventHandler::param_map &params) {
	using component::command::command_t;
	auto combat = state->get_combat();
	combat->note_time(time);

	// dead (death animation), removed or garrisoned entities take no commands
	std::erase_if(ids, [&](entity_id_t id) {
		return not state->get_game_entities().contains(id) or combat->is_dead(id) or combat->garrisoned_in(id).has_value();
	});
	if (ids.empty()) {
		return false;
	}

	// XR fork W4: stances, formations, patrol, guard, follow, attack move, garrison, stop, delete, bell
	if (handle_w4(state, time, type, ids, target, params)) {
		return true;
	}

	if (type != command_t::MOVE and type != command_t::ATTACK) {
		// idle (also the plain move of the context menu), gather, build: stop attacking, orders end
		for (auto id : ids) {
			combat->player_moved(id, type == command_t::IDLE ? std::optional<coord::phys3>{target} : std::nullopt, time);
		}
		return false;
	}

	// enemy: given (ATTACK), or under the cursor (enemy of the first commanded entity's owner)
	std::optional<entity_id_t> enemy;
	if (type == command_t::ATTACK and params.contains("target_entity")) {
		enemy = params.get<entity_id_t>("target_entity", 0);
	}
	else {
		auto picked = combat->pick_enemy(state, time, owner_of(state, ids.front(), time), make_request(target, params));
		if (picked) {
			enemy = picked->get_id();
		}
	}

	if (not enemy) {
		if (type == command_t::ATTACK) {
			for (auto id : ids) {
				combat->player_moved(id, std::nullopt, time);
			}
			log::log(INFO << "Attack command: no enemy at tile (" << target.ne.to_float() << ", "
			              << target.se.to_float() << ")");
			ids.clear();
			return false;
		}
		// XR fork W4: military units on an own building with garrison places go inside
		// (villagers keep their economy meaning: drop off, build)
		if (type == command_t::MOVE and not params.get<bool>("plain", false)) {
			auto building = combat->pick_shelter(state, time, owner_of(state, ids.front(), time), make_request(target, params));
			if (building) {
				size_t inside = 0;
				std::erase_if(ids, [&](entity_id_t id) {
					auto stats = combat->get_stats(id);
					if (stats and stats->military() and combat->order_garrison(state, id, building->get_id(), time)) {
						inside += 1;
						return true;
					}
					return false;
				});
				if (inside > 0) {
					log::log(INFO << "Combat command garrison (right click): " << inside << " units into building "
					              << building->get_id());
				}
			}
		}
		// XR fork W4: several military units with a formation move in it (07 §7.5)
		auto moved = combat->formation_move(state, ids, target, time);
		std::erase_if(ids, [&](entity_id_t id) { return std::find(moved.begin(), moved.end(), id) != moved.end(); });
		// plain move: stop attacking, the destination is the new post
		for (auto id : ids) {
			combat->player_moved(id, target, time);
		}
		return false;
	}

	size_t attacking = 0;
	std::erase_if(ids, [&](entity_id_t id) {
		if (combat->order_attack(state, id, *enemy, time)) {
			attacking += 1;
			return true;
		}
		combat->player_moved(id, target, time);
		// cannot attack: an explicit attack command does nothing, a right click moves
		return type == command_t::ATTACK;
	});
	log::log(INFO << "Attack command: " << attacking << " entities attack entity " << *enemy
	              << (ids.empty() ? std::string{} : ", " + std::to_string(ids.size()) + " move"));
	return true;
}

} // namespace openage::gamestate::combat
