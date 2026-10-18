// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <array>
#include <optional>
#include <string>
#include <unordered_map>

#include <nyan/nyan.h>

#include "coord/phys.h"
#include "gamestate/component/api_component.h"
#include "gamestate/component/types.h"
#include "gamestate/food_math.h"
#include "gamestate/resources.h"
#include "gamestate/types.h"
#include "time/time.h"


namespace openage::gamestate::component {

/**
 * Gathering of a unit (XR fork, economy): all Gather abilities of the entity
 * (one per resource) plus the state of the current gather job
 * (walk to the resource -> gather until full -> walk to a drop site -> drop off -> back).
 *
 * Only accessed from the simulation thread.
 */
class Gather final : public APIComponent {
public:
	/// gather ability values for one resource
	struct Skill {
		bool available = false;
		/// resource units per second
		double rate = 0.0;
		/// carry capacity
		double capacity = 10.0;
		/// gather animation (empty: none)
		std::string animation{};
		/// walk animation with the load (Move override of the container's carry
		/// progress, empty: none), shown on the way to the drop site
		std::string carry_animation{};
		/// fraction of the capacity from which the carry animation is shown
		/// (Progress.left_boundary / 100, AoE: 0.2)
		double carry_from = 0.2;
	};

	enum class phase_t {
		NONE,
		/// walking to the resource
		TO_RESOURCE,
		/// gathering (one chunk per step)
		GATHERING,
		/// walking to the drop site
		TO_DROP_SITE,
		/// XR fork W3: killing the animal before its food can be gathered (one strike per step)
		HUNTING,
	};

	/// state of the gather job
	struct Job {
		phase_t phase = phase_t::NONE;
		/// resource entity
		std::optional<entity_id_t> target{};
		/// resource type of the job
		resource_t resource = resource_t::FOOD;
		/// last known position of the target (for searching the next one)
		coord::phys3 target_pos{0, 0, 0};
		/// drop site entity while walking there
		std::optional<entity_id_t> drop_site{};
		/// amount of the current gather chunk
		double chunk = 0.0;
		/// failed walks in a row (no path)
		int failed_walks = 0;
		/// XR fork W3: resource spot (nyan) and kind of the target; the search for the next
		/// target prefers the same spot (berries after berries, farms after farms)
		std::string spot{};
		food::source_t kind = food::source_t::OTHER;
	};

	Gather(const std::shared_ptr<openage::event::EventLoop> &loop,
	       nyan::Object &ability);

	component_t get_type() const override;

	/// set the values of the Gather ability for a resource
	void set_skill(resource_t resource, const Skill &skill);

	const Skill &get_skill(resource_t resource) const;

	/**
	 * XR fork W3: values of the Gather ability whose targets name the resource spot (a villager
	 * gathers berries, hunts, farms and herds with different rates and capacities); spots that
	 * no ability names use the first ability of the resource.
	 */
	void set_spot_skill(const std::string &spot, const Skill &skill);
	const Skill &get_skill(resource_t resource, const std::string &spot) const;
	/// an ability of the unit names the spot in its targets
	bool names_spot(const std::string &spot) const;
	/// all spot skills (XR fork W1 + W3: copied into the running unit after a technology)
	const std::unordered_map<std::string, Skill> &get_spot_skills() const {
		return this->spot_skills;
	}

	/// true if the unit can gather this resource
	bool can_gather(resource_t resource) const;

	Job &get_job();

	/// amount and type of the carried resource
	double get_carried() const;
	resource_t get_carried_type() const;
	void set_carried(resource_t type, double amount);

	/// time of the last INFO log of this unit (throttling)
	time::time_t last_log{time::TIME_MIN};

	/// XR fork W3: the unit moves on water (fishing ship): it uses drop sites on water only
	bool ship = false;

private:
	std::array<Skill, RESOURCE_COUNT> skills;
	std::unordered_map<std::string, Skill> spot_skills;
	Job job;
	double carried;
	resource_t carried_type;
};

} // namespace openage::gamestate::component
