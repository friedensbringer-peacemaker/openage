// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <memory>
#include <string>

#include "event/eventhandler.h"


namespace openage {

namespace event {
class EventLoop;
} // namespace event

namespace gamestate::combat {

/**
 * Event handler of the combat state (XR fork). The target of all events is
 * the combat state of the game state; "entity" is the entity id parameter.
 *
 *  - game.combat_tick: one attack step of an attacker
 *  - game.combat_scan: periodic auto attack scan and victory check
 *  - game.combat_remove: remove a dead entity after its death animation
 */
class CombatEventHandler : public openage::event::OnceEventHandler {
public:
	enum class kind {
		TICK,
		SCAN,
		REMOVE,
	};

	explicit CombatEventHandler(kind what);
	~CombatEventHandler() = default;

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

private:
	kind what;
};

/**
 * Register the combat event handlers (game simulation, host checks).
 */
void add_event_handlers(const std::shared_ptr<openage::event::EventLoop> &loop);

} // namespace gamestate::combat
} // namespace openage
