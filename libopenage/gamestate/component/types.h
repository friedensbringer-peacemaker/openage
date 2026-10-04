// Copyright 2021-2023 the openage authors. See copying.md for legal info.

#pragma once


namespace openage::gamestate::component {

/**
 * Types of components.
 */
enum class component_t {
	// Internal
	POSITION,
	COMMANDQUEUE,
	OWNERSHIP,
	ACTIVITY,

	// API
	IDLE,
	TURN,
	MOVE,
	SELECTABLE,
	LIVE,

	// API, economy (XR fork)
	GATHER,
	HARVESTABLE,
	DROP_SITE,

	// API, production (XR fork)
	PRODUCTION_QUEUE,
	CONSTRUCTABLE,
	BUILDER,
};

} // namespace openage::gamestate::component
