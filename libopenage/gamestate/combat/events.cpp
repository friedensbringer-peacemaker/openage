// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "events.h"

#include "event/event_loop.h"
#include "event/evententity.h"
#include "gamestate/combat/combat_state.h"
#include "gamestate/game_state.h"
#include "gamestate/types.h"


namespace openage::gamestate::combat {

namespace {

const char *handler_name(CombatEventHandler::kind what) {
	switch (what) {
	case CombatEventHandler::kind::TICK:
		return "game.combat_tick";
	case CombatEventHandler::kind::SCAN:
		return "game.combat_scan";
	case CombatEventHandler::kind::REMOVE:
	default:
		return "game.combat_remove";
	}
}

} // namespace


CombatEventHandler::CombatEventHandler(kind what) :
	OnceEventHandler{handler_name(what)},
	what{what} {
}

void CombatEventHandler::setup_event(const std::shared_ptr<openage::event::Event> & /* event */,
                                     const std::shared_ptr<openage::event::State> & /* state */) {
}

void CombatEventHandler::invoke(openage::event::EventLoop & /* loop */,
                                const std::shared_ptr<openage::event::EventEntity> & /* target */,
                                const std::shared_ptr<openage::event::State> &state,
                                const time::time_t &time,
                                const param_map &params) {
	auto gstate = std::dynamic_pointer_cast<GameState>(state);
	auto combat = gstate->get_combat();
	auto id = params.get<entity_id_t>("entity", 0);
	switch (this->what) {
	case kind::TICK:
		combat->tick(gstate, id, time);
		break;
	case kind::SCAN:
		combat->scan(gstate, time);
		break;
	case kind::REMOVE:
		combat->remove(gstate, id, time);
		break;
	}
}

time::time_t CombatEventHandler::predict_invoke_time(const std::shared_ptr<openage::event::EventEntity> & /* target */,
                                                     const std::shared_ptr<openage::event::State> & /* state */,
                                                     const time::time_t &at) {
	return at;
}

void add_event_handlers(const std::shared_ptr<openage::event::EventLoop> &loop) {
	for (auto what : {CombatEventHandler::kind::TICK, CombatEventHandler::kind::SCAN, CombatEventHandler::kind::REMOVE}) {
		loop->add_event_handler(std::make_shared<CombatEventHandler>(what));
	}
}

} // namespace openage::gamestate::combat
