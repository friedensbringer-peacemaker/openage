// Copyright 2023-2026 the openage authors. See copying.md for legal info.

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

	// economy (XR fork)
	GATHER,

	// production (XR fork)
	BUILD,

	/// XR fork: attack a game entity (combat::CombatState)
	ATTACK,

	// XR fork W4 (combat::CombatState, docs/aoe2-spec 06 §6.1/6.3), handled in combat::handle_command
	/// patrol between the current position and the target point
	PATROL,
	/// guard the own entity under the cursor
	GUARD,
	/// follow the entity under the cursor
	FOLLOW,
	/// walk to the target point and attack every enemy on the way
	ATTACK_MOVE,
	/// enter the own building under the cursor
	GARRISON,
	/// buildings: let the whole garrison out
	UNGARRISON,
	/// stop: drop orders and targets, stand
	STOP,
	/// set the stance ("combat_arg" = combat::stance_t)
	STANCE,
	/// set the formation ("combat_arg" = combat::formation_t)
	FORMATION,
	/// delete the own entities (they die, button "Löschen")
	KILL,
	/// town centers: town bell on ("combat_arg" = 1) or back to work (0)
	BELL,
};

} // namespace openage::gamestate::component::command
