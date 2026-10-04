// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <memory>

#include "event/eventhandler.h"


namespace openage::gamestate::ai {

/**
 * Event handler of the computer opponent (XR fork): "game.ai_think", target
 * is the AiPlayer. The AI creates its events with this handler object, so
 * the event loop registers it on first use (no entry in
 * GameSimulation::init_event_handlers() needed).
 */
class AiEventHandler : public openage::event::OnceEventHandler {
public:
	AiEventHandler();
	~AiEventHandler() = default;

	void setup_event(const std::shared_ptr<openage::event::Event> &event,
	                 const std::shared_ptr<openage::event::State> &state) override;

	void invoke(openage::event::EventLoop &loop,
	            const std::shared_ptr<openage::event::EventEntity> &target,
	            const std::shared_ptr<openage::event::State> &state,
	            const time::time_t &time,
	            const param_map &params) override;

	time::time_t predict_invoke_time(const std::shared_ptr<openage::event::EventEntity> &target,
	                                 const std::shared_ptr<openage::event::State> &state,
	                                 const time::time_t &at) override;
};

} // namespace openage::gamestate::ai
