// Copyright 2023-2023 the openage authors. See copying.md for legal info.

#pragma once


namespace openage::gamestate::component::command {

/**
 * Command types.
 */
enum class command_t {
	NONE,

	CUSTOM,
	IDLE,
	MOVE,
	/// XR fork: attack a game entity (combat::CombatState)
	ATTACK,
};

} // namespace openage::gamestate::component::command
