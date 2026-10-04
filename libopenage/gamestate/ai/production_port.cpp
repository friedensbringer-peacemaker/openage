// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "production_port.h"

#include "log/log.h"
#include "log/message.h"


namespace openage::gamestate::ai {

namespace {

/**
 * Production stub: nothing is trained or built, the AI logs its wishes.
 */
class StubProduction final : public ProductionPort {
public:
	bool enabled() const override {
		return false;
	}

	std::string name() const override {
		return "stub";
	}

	bool train(const std::shared_ptr<GameState> & /* state */,
	           player_id_t player,
	           entity_id_t building,
	           const std::string &unit,
	           const time::time_t &time) override {
		log::log(DBG << "AI P" << player << ": (stub) would train " << unit << " in entity " << building
		             << " at t=" << time.to_double());
		return false;
	}

	bool build(const std::shared_ptr<GameState> & /* state */,
	           player_id_t player,
	           const std::string &building,
	           const std::vector<entity_id_t> &builders,
	           const coord::phys3 &where,
	           const time::time_t &time) override {
		log::log(DBG << "AI P" << player << ": (stub) would build " << building << " at ("
		             << where.ne.to_double() << ", " << where.se.to_double() << ") with "
		             << builders.size() << " villagers at t=" << time.to_double());
		return false;
	}

	size_t queued(const std::shared_ptr<GameState> & /* state */,
	              entity_id_t /* building */,
	              const time::time_t & /* time */) override {
		return 0;
	}

	std::optional<Population> population(const std::shared_ptr<GameState> & /* state */,
	                                     player_id_t /* player */,
	                                     const time::time_t & /* time */) override {
		return std::nullopt;
	}

	bool is_complete(const std::shared_ptr<GameState> & /* state */,
	                 entity_id_t /* building */,
	                 const time::time_t & /* time */) override {
		// no foundations without production: every building stands finished
		return true;
	}
};

} // namespace


/*
 * After the merge with xr-prod (gamestate/production.h, namespace prod):
 *
 * 1. Add a class ProdProduction : ProductionPort in this file under
 *    #ifdef OPENAGE_AI_PRODUCTION and return it from make_production_port()
 *    (set the definition in gamestate/ai/CMakeLists.txt). It needs the
 *    prod::Production object of the simulation (GameSimulation::get_production()),
 *    so make_production_port() gets that pointer as parameter; Game::start_ai()
 *    (game.cpp, block "ai (XR fork)") passes it through.
 * 2. train(): prod::Production::train_in(building, unit) - it queues in the
 *    given building and spends the resources of the building's owner (works for
 *    any player). Note: it also sets the HUD status message of the human
 *    player; add a quiet variant (e.g. a flag on the request) to keep AI
 *    messages out of the HUD.
 * 3. build(): prod::Production::place() is bound to the HUD player and its
 *    selection. Add a per-player variant, e.g.
 *      void Production::place_for(player_id_t player, const std::vector<entity_id_t> &builders,
 *                                 const std::string &id, const coord::phys3 &ground_hit);
 *    (same code as Request::kind_t::PLACE with "own" = builders of that player)
 *    and call it. The placement check (check_placement) rejects bad spots; the
 *    AI tries the next candidate spot on its next decision.
 * 4. queued(): component::ProductionQueue::get_queue().size() of the building.
 * 5. population(): the private count_population() of production.cpp as public
 *    function, e.g. prod::population_of(state, player, time) -> {units + training, limit}.
 * 6. is_complete(): the Constructable component (is_complete() in production.cpp).
 *
 * Production::update() runs every simulation loop iteration, requests are
 * executed there (one loop iteration later than the AI decision).
 */
std::shared_ptr<ProductionPort> make_production_port() {
	return std::make_shared<StubProduction>();
}

} // namespace openage::gamestate::ai
