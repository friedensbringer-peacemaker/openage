// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <array>

#include <nyan/nyan.h>

#include "gamestate/component/api_component.h"
#include "gamestate/component/types.h"
#include "gamestate/resources.h"


namespace openage::gamestate::component {

/**
 * Building where units drop off gathered resources (XR fork, economy).
 */
class DropSite final : public APIComponent {
public:
	/**
	 * @param loop Event loop.
	 * @param ability DropSite ability.
	 * @param accepts Accepted resources (indexed by resource_t).
	 */
	DropSite(const std::shared_ptr<openage::event::EventLoop> &loop,
	         nyan::Object &ability,
	         const std::array<bool, RESOURCE_COUNT> &accepts);

	component_t get_type() const override;

	bool accepts(resource_t resource) const;

private:
	std::array<bool, RESOURCE_COUNT> accepted;

public:
	/// hitbox radius in tiles (footprint for walking next to it)
	double radius = 0.5;
};

} // namespace openage::gamestate::component
