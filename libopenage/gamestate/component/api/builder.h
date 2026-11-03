// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include <nyan/nyan.h>

#include "gamestate/component/api_component.h"
#include "gamestate/component/types.h"
#include "gamestate/production_math.h"
#include "gamestate/resources.h"
#include "gamestate/tech_math.h"
#include "gamestate/types.h"
#include "time/time.h"


namespace openage::gamestate::component {

/**
 * Unit that places and constructs buildings (XR fork, production): the buildings
 * of its Create ability (placement mode Place), their build times from the
 * Construct ability, and the state of the current construction job
 * (walk next to the foundation -> build in steps -> idle when complete).
 *
 * Only accessed from the simulation thread.
 */
class Builder final : public APIComponent {
public:
	/// a building the unit can place
	struct Buildable {
		/// short name ("House")
		std::string name;
		/// nyan game entity
		nyan::fqon_t fqon;
		resource_amounts_t cost{};
		/// build time of one builder (seconds)
		double time = prod::DEFAULT_BUILD_TIME;
		/// hitbox radius of the building (tiles)
		double radius = 1.0;
		/// unlock condition (XR fork W1, CreatableGameEntity.condition)
		tech::Condition condition{};
	};

	enum class phase_t {
		NONE,
		/// walking to the foundation
		TO_SITE,
		/// building (one step at a time)
		BUILDING,
		/// XR fork W5: after a building was finished, looking for a foundation nearby (spec 04 §4.3, L33)
		SEARCH,
	};

	struct Job {
		phase_t phase = phase_t::NONE;
		/// foundation entity
		std::optional<entity_id_t> target{};
		/// duration of the current building step (seconds)
		double step = 0.0;
		/// failed walks in a row
		int failed_walks = 0;
		/// XR fork W5: repair a finished building instead of constructing it (spec 04 §4.5)
		bool repair = false;
		/// XR fork W5: HP repaired but not yet added (whole HP are applied) and costs owed
		double repair_hp = 0.0;
		std::array<double, 4> repair_owed{};
		/// XR fork W5: end of the search for a foundation (simulation seconds)
		double search_until = 0.0;
	};

	Builder(const std::shared_ptr<openage::event::EventLoop> &loop,
	        nyan::Object &ability,
	        std::vector<Buildable> &&buildables);

	component_t get_type() const override;

	const std::vector<Buildable> &get_buildables() const;

	/// replace the buildables (XR fork W1: a technology changed the data of the owner)
	void set_buildables(std::vector<Buildable> &&buildables);

	/// buildable by short name (nullptr if the unit cannot build it)
	const Buildable *find(const std::string &name) const;

	Job &get_job();

	/// construction animation (empty: none)
	std::string animation{};

	/// time of the last INFO log of this unit (throttling)
	time::time_t last_log{time::TIME_MIN};

	/// XR fork W5: wall foundations of a drag that this unit builds one after the other, nearest
	/// first (spec 04 §4.4); a build order for another foundation clears it
	std::vector<entity_id_t> wall_queue{};

private:
	std::vector<Buildable> buildables;
	Job job;
};

} // namespace openage::gamestate::component
