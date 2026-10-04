// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "events.h"

#include "event/event_loop.h"
#include "event/evententity.h"
#include "gamestate/ai/ai_player.h"
#include "gamestate/game_state.h"


namespace openage::gamestate::ai {

AiEventHandler::AiEventHandler() :
	OnceEventHandler{"game.ai_think"} {
}

void AiEventHandler::setup_event(const std::shared_ptr<openage::event::Event> & /* event */,
                                 const std::shared_ptr<openage::event::State> & /* state */) {
}

void AiEventHandler::invoke(openage::event::EventLoop & /* loop */,
                            const std::shared_ptr<openage::event::EventEntity> &target,
                            const std::shared_ptr<openage::event::State> &state,
                            const time::time_t &time,
                            const param_map & /* params */) {
	auto ai = std::dynamic_pointer_cast<AiPlayer>(target);
	auto gstate = std::dynamic_pointer_cast<GameState>(state);
	if (ai and gstate) {
		ai->think(gstate, time);
	}
}

time::time_t AiEventHandler::predict_invoke_time(const std::shared_ptr<openage::event::EventEntity> & /* target */,
                                                 const std::shared_ptr<openage::event::State> & /* state */,
                                                 const time::time_t &at) {
	return at;
}

} // namespace openage::gamestate::ai
