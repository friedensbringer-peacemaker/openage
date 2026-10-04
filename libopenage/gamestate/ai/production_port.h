// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "coord/phys.h"
#include "gamestate/types.h"
#include "time/time.h"


namespace openage::gamestate {
class GameState;

namespace prod {
class Production;
}

namespace ai {

/// population of a player
struct Population {
	size_t used = 0;
	size_t cap = 0;
};

/**
 * Training and construction for the computer opponent (XR fork).
 *
 * Two implementations (production_port.cpp): the bridge to the production of
 * the simulation (gamestate/production.h, prod::Production: train_for(),
 * place_for(), population_of(), queued_in(), is_finished()), and a stub
 * without any effect for games without production (host checks): the AI
 * still decides and logs what it would train or build.
 *
 * All methods are called from the simulation thread (AI think events).
 */
class ProductionPort {
public:
	virtual ~ProductionPort() = default;

	/// true if training and building have an effect
	virtual bool enabled() const = 0;

	/// short description for logs ("stub", "prod")
	virtual std::string name() const = 0;

	/**
	 * Queue a unit in a building of the player (costs are paid by the owner).
	 *
	 * @param unit Short name of the game entity ("Villager", "Militia").
	 *
	 * @return true if queued.
	 */
	virtual bool train(const std::shared_ptr<GameState> &state,
	                   player_id_t player,
	                   entity_id_t building,
	                   const std::string &unit,
	                   const time::time_t &time) = 0;

	/**
	 * Place a foundation and let villagers build it.
	 *
	 * @param building Short name of the game entity ("House", "Barracks").
	 * @param builders Villagers of the player.
	 * @param where Centre of the building on the ground (tiles).
	 *
	 * @return true if the foundation was placed.
	 */
	virtual bool build(const std::shared_ptr<GameState> &state,
	                   player_id_t player,
	                   const std::string &building,
	                   const std::vector<entity_id_t> &builders,
	                   const coord::phys3 &where,
	                   const time::time_t &time) = 0;

	/**
	 * Units queued in a building (0 without production).
	 */
	virtual size_t queued(const std::shared_ptr<GameState> &state,
	                      entity_id_t building,
	                      const time::time_t &time) = 0;

	/**
	 * Population and limit of the player; nothing: the AI estimates them
	 * from its units, town centers and houses (ai_rules.h).
	 */
	virtual std::optional<Population> population(const std::shared_ptr<GameState> &state,
	                                             player_id_t player,
	                                             const time::time_t &time) = 0;

	/**
	 * Building is a finished building (not a foundation).
	 */
	virtual bool is_complete(const std::shared_ptr<GameState> &state,
	                         entity_id_t building,
	                         const time::time_t &time) = 0;
};

/**
 * Production for the AI: the bridge to \p production, or the stub (no
 * effect) without it.
 */
std::shared_ptr<ProductionPort> make_production_port(const std::shared_ptr<prod::Production> &production = nullptr);

} // namespace ai
} // namespace openage::gamestate
