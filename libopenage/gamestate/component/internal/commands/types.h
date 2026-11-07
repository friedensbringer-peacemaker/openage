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

	// XR fork W5 (docs/aoe2-spec 04 §4.4/4.5), handled in event/send_command.cpp
	/// villagers repair the own damaged building under the cursor
	REPAIR,
	/// gates: lock ("combat_arg" = 1) or unlock (0)
	GATE_LOCK,
	// XR fork W6 (special::SpecialState, docs/aoe2-spec 05 §5.7–5.9, 06 §6.5), handled in special::handle_command
	/// monks: convert the enemy under the cursor
	CONVERT,
	/// monks: heal the own unit under the cursor
	HEAL,
	/// trebuchets: pack (move) / unpack (shoot)
	PACK,
	UNPACK,
	/// monks: put the carried relic down
	DROP_RELIC,
	/// carriers: unload at the target point ("combat_arg" = 1: here at once)
	UNLOAD,
	/// market: "combat_arg" = good * 2 + (1 when selling), goods as in special::MARKET_GOODS
	MARKET,
};

} // namespace openage::gamestate::component::command
