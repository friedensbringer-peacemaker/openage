// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <nyan/nyan.h>

#include "gamestate/component/api_component.h"
#include "gamestate/component/types.h"
#include "gamestate/production_math.h"


namespace openage::gamestate::component {

/**
 * Building that is constructed by villagers (XR fork, production).
 *
 * Buildings of the map start complete. A foundation placed by a player starts
 * at progress 0, blocks its tiles and has no drop site until it is complete.
 *
 * Only accessed from the simulation thread.
 */
class Constructable final : public APIComponent {
public:
	Constructable(const std::shared_ptr<openage::event::EventLoop> &loop,
	              nyan::Object &ability);

	component_t get_type() const override;

	/// progress 0..1
	double get_progress() const;

	/// true if construction is done
	bool is_complete() const;

	/// make this a foundation (progress 0) with a build time
	void start_foundation(double build_time);

	/**
	 * Add the work of one builder.
	 *
	 * @return true if this step completed the building.
	 */
	bool add_work(double seconds);

	double get_build_time() const;

	/// population space provided when complete (ProvideContingent)
	double population = 0.0;

	/// hitbox radius in tiles
	double radius = 0.5;

	/// animation while under construction (empty: none)
	std::string construct_animation{};

	/// health attribute (nyan fqon, empty: none) and its maximum; the health of a
	/// foundation rises with the construction progress (production.h)
	nyan::fqon_t health_attribute{};
	int64_t max_health = 0;

	/// drop site of the finished building (taken away while it is a foundation)
	std::shared_ptr<Component> pending_drop_site{};

private:
	double progress;
	double build_time;
};

} // namespace openage::gamestate::component
