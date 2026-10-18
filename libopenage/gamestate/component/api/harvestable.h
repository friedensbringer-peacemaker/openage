// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <nyan/nyan.h>

#include <optional>
#include <string>

#include "gamestate/component/api_component.h"
#include "gamestate/component/types.h"
#include "gamestate/food_math.h"
#include "gamestate/resources.h"
#include "gamestate/types.h"


namespace openage::gamestate::component {

/**
 * Resource spot of a game entity (tree, mine, bush) that units can gather from
 * (XR fork, economy). The amount starts at the resource spot's starting amount
 * and only decreases; the entity is removed from the world when it is empty.
 *
 * Only accessed from the simulation thread.
 */
class Harvestable final : public APIComponent {
public:
	/**
	 * @param loop Event loop.
	 * @param ability Harvestable ability.
	 * @param resource Resource type of the resource spot.
	 * @param amount Starting amount.
	 */
	Harvestable(const std::shared_ptr<openage::event::EventLoop> &loop,
	            nyan::Object &ability,
	            resource_t resource,
	            double amount);

	component_t get_type() const override;

	resource_t get_resource() const;

	/// remaining amount
	double get_amount() const;

	/// starting amount (XR fork, save games: only changed spots are stored)
	double get_start_amount() const;

	/// set the remaining amount (XR fork, loading a save game)
	void set_amount(double amount);

	/**
	 * Take up to \p amount from the spot.
	 *
	 * @return Amount actually taken (less if the spot runs empty).
	 */
	double take(double amount);

	/// true once the spot is empty (and the entity was removed)
	bool is_depleted() const;

	/// mark as removed from the world
	void set_depleted();

	/// XR fork W3: refill (a farm is finished again) and take it as the new starting amount
	void refill(double amount);

private:
	resource_t resource;
	double amount;
	double start_amount;
	bool depleted;

public:
	/// hitbox radius in tiles (footprint for walking next to it)
	double radius = 0.5;
	/// hitbox height (scene up units, for picking)
	double height = 1.0;

	// ---- XR fork W3 (food): how it is gathered, decay, worker
	/// nyan ResourceSpot of the data (the Gather.targets of a gatherer name it)
	std::string spot{};
	/// kind of food source (animal, farm, fish, other)
	food::source_t kind = food::source_t::OTHER;
	/// food lost per second once the animal is dead (ResourceSpot.decay_rate)
	double decay_rate = 0.0;
	/// the animal is dead (a carcass): gatherable, decays
	bool dead = false;
	/// farm / trap: the worker that holds it (one each, spec 02 §2.6)
	std::optional<entity_id_t> worker{};
};

} // namespace openage::gamestate::component
