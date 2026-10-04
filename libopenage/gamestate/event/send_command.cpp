// Copyright 2023-2023 the openage authors. See copying.md for legal info.

#include "send_command.h"

#include <vector>

#include "coord/phys.h"
#include "gamestate/combat/command.h"
#include "gamestate/component/internal/command_queue.h"
#include "gamestate/component/internal/commands/gather.h"
#include "gamestate/component/internal/commands/idle.h"
#include "gamestate/component/internal/commands/move.h"
#include "gamestate/component/types.h"
#include "gamestate/econ.h"
#include "gamestate/game_entity.h"
#include "gamestate/game_state.h"
#include "gamestate/map.h"
#include "gamestate/types.h"
#include "log/log.h"


namespace openage::gamestate {
namespace component {
class CommandQueue;

namespace command {
class IdleCommand;
class MoveCommand;
} // namespace command
} // namespace component

namespace event {

Commander::Commander(const std::shared_ptr<openage::event::EventLoop> &loop) :
	openage::event::EventEntity{loop} {
}

size_t Commander::id() const {
	return 0;
}

std::string Commander::idstr() const {
	return "command_target";
}


SendCommandHandler::SendCommandHandler() :
	openage::event::OnceEventHandler{"game.send_command"} {
}

void SendCommandHandler::setup_event(const std::shared_ptr<openage::event::Event> & /* event */,
                                     const std::shared_ptr<openage::event::State> & /* state */) {
	// TODO
}

void SendCommandHandler::invoke(openage::event::EventLoop & /* loop */,
                                const std::shared_ptr<openage::event::EventEntity> & /* target */,
                                const std::shared_ptr<openage::event::State> &state,
                                const time::time_t &time,
                                const param_map &params) {
	auto gstate = std::dynamic_pointer_cast<openage::gamestate::GameState>(state);

	auto command_type = params.get("type", component::command::command_t::NONE);
	// XR fork: targets from input are plane hits; use the visible terrain point (hills)
	auto target = gstate->get_map()->pick_terrain(params.get("target", coord::phys3{0, 0, 0}));
	std::vector<gamestate::entity_id_t> ids = params.get("entity_ids",
	                                                     std::vector<gamestate::entity_id_t>{});
	// XR fork: INFO, embedders diagnose taps on headsets from the log
	log::log(INFO << "Command " << static_cast<int>(command_type) << " for " << ids.size()
	              << " entities, target tile (" << target.ne.to_float() << ", " << target.se.to_float() << ")");
	// XR fork: a right click on an enemy attacks it (attackers leave ids), dead entities
	// are dropped (gamestate/combat/command.h); priority: enemy, resource, ground
	bool enemy_picked = combat::handle_command(gstate, time, command_type, ids, target, params);

	// XR fork (economy): a right click (move) on a resource makes gatherers gather;
	// GATHER commands name the resource ("target_entity") or pick it at the target
	std::shared_ptr<GameEntity> gather_target;
	if (not enemy_picked
	    and (command_type == component::command::command_t::MOVE
	         or command_type == component::command::command_t::GATHER)) {
		auto picked = econ::pick_resource(gstate, params.get("target", coord::phys3{0, 0, 0}), time);
		if (params.contains("target_entity")) {
			picked = params.get<entity_id_t>("target_entity");
		}
		if (picked and gstate->get_game_entities().contains(*picked)) {
			gather_target = gstate->get_game_entity(*picked);
			log::log(INFO << "Command target: resource entity " << *picked);
		}
	}

	for (auto id : ids) {
		auto entity = gstate->get_game_entity(id);
		auto command_queue = std::dynamic_pointer_cast<component::CommandQueue>(
			entity->get_component(component::component_t::COMMANDQUEUE));

		// XR fork (economy)
		if (econ::can_gather_from(entity, gather_target)) {
			command_queue->add_command(
				time,
				std::make_shared<component::command::GatherCommand>(gather_target->get_id()));
			continue;
		}

		switch (command_type) {
		case component::command::command_t::IDLE:
			command_queue->add_command(time, std::make_shared<component::command::IdleCommand>());
			break;
		case component::command::command_t::MOVE:
			command_queue->add_command(
				time,
				std::make_shared<component::command::MoveCommand>(target));
			break;
		// XR fork (economy): units that cannot gather walk to the resource
		case component::command::command_t::GATHER:
			command_queue->add_command(
				time,
				std::make_shared<component::command::MoveCommand>(target));
			break;
		default:
			break;
		}
	}
}

time::time_t SendCommandHandler::predict_invoke_time(const std::shared_ptr<openage::event::EventEntity> & /* target */,
                                                     const std::shared_ptr<openage::event::State> & /* state */,
                                                     const time::time_t &at) {
	return at;
}

} // namespace event
} // namespace openage::gamestate
