// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "production_port.h"

#include "gamestate/game_entity.h"
#include "gamestate/game_state.h"
#include "gamestate/production.h"
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


/**
 * Bridge to the production of the simulation (gamestate/production.h).
 * Requests are executed in Production::update() of the next simulation loop
 * iteration (before the next AI decision); rejected placements are logged
 * there and the AI tries another spot later.
 */
class ProdProduction final : public ProductionPort {
public:
	explicit ProdProduction(const std::shared_ptr<prod::Production> &production) :
		production{production} {}

	bool enabled() const override {
		return true;
	}

	std::string name() const override {
		return "prod";
	}

	bool train(const std::shared_ptr<GameState> & /* state */,
	           player_id_t player,
	           entity_id_t building,
	           const std::string &unit,
	           const time::time_t & /* time */) override {
		this->production->train_for(player, building, unit);
		return true;
	}

	bool build(const std::shared_ptr<GameState> & /* state */,
	           player_id_t player,
	           const std::string &building,
	           const std::vector<entity_id_t> &builders,
	           const coord::phys3 &where,
	           const time::time_t & /* time */) override {
		this->production->place_for(player, builders, building, where);
		return true;
	}

	size_t queued(const std::shared_ptr<GameState> &state,
	              entity_id_t building,
	              const time::time_t & /* time */) override {
		const auto &entities = state->get_game_entities();
		auto it = entities.find(building);
		return it == entities.end() ? 0 : prod::queued_in(it->second);
	}

	std::optional<Population> population(const std::shared_ptr<GameState> &state,
	                                     player_id_t player,
	                                     const time::time_t &time) override {
		auto pop = prod::population_of(state, player, time);
		return Population{pop.used, pop.cap};
	}

	bool is_complete(const std::shared_ptr<GameState> &state,
	                 entity_id_t building,
	                 const time::time_t & /* time */) override {
		const auto &entities = state->get_game_entities();
		auto it = entities.find(building);
		return it != entities.end() and prod::is_finished(it->second);
	}

private:
	std::shared_ptr<prod::Production> production;
};

} // namespace


std::shared_ptr<ProductionPort> make_production_port(const std::shared_ptr<prod::Production> &production) {
	if (production) {
		return std::make_shared<ProdProduction>(production);
	}
	return std::make_shared<StubProduction>();
}

} // namespace openage::gamestate::ai
