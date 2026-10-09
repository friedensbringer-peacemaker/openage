// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nyan/nyan.h>

#include "coord/phys.h"
#include "gamestate/component/api_component.h"
#include "gamestate/component/types.h"
#include "gamestate/production_math.h"
#include "gamestate/resources.h"
#include "gamestate/types.h"


namespace openage::gamestate::component {

/**
 * Training of units in a building (XR fork, production): the creatable units of
 * the Create ability and a training queue (gamestate/production_math.h).
 *
 * Only accessed from the simulation thread.
 */
class ProductionQueue final : public APIComponent {
public:
	/// a unit the building can train
	struct Creatable {
		/// short name ("Villager")
		std::string name;
		/// nyan game entity
		nyan::fqon_t fqon;
		resource_amounts_t cost{};
		/// training time (seconds)
		double time = prod::DEFAULT_TRAIN_TIME;
	};

	ProductionQueue(const std::shared_ptr<openage::event::EventLoop> &loop,
	                nyan::Object &ability,
	                std::vector<Creatable> &&creatables);

	component_t get_type() const override;

	const std::vector<Creatable> &get_creatables() const;

	/// creatable by short name (nullptr if the building cannot train it)
	const Creatable *find(const std::string &name) const;

	prod::TrainQueue &get_queue();

	/// rally point of new units (none: they stay next to the building)
	std::optional<coord::phys3> rally_point{};

	/// what the rally point is on (XR fork): new villagers gather a resource, units
	/// walk to an own entity, military attacks an enemy; the ground otherwise
	enum class rally_t {
		GROUND,
		RESOURCE,
		ENTITY,
		ENEMY,
	};
	struct RallyTarget {
		rally_t kind = rally_t::GROUND;
		entity_id_t entity = 0;
		/// resource type of a RESOURCE target (the next spot of this type if it is empty)
		resource_t resource = resource_t::FOOD;
	};
	std::optional<RallyTarget> rally_target{};

private:
	std::vector<Creatable> creatables;
	prod::TrainQueue queue;
};

} // namespace openage::gamestate::component
