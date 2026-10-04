// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include "gamestate/component/internal/commands/base_command.h"
#include "gamestate/component/internal/commands/types.h"
#include "gamestate/types.h"


namespace openage::gamestate::component::command {

/**
 * Command for attacking a game entity (XR fork).
 *
 * Not put into the command queue (the activity graphs of the converted
 * modpacks have no attack node): the send command handler hands it to the
 * combat state, which walks into range with move commands and attacks.
 */
class AttackCommand : public Command {
public:
	/**
	 * Create a new attack command.
	 *
	 * @param target ID of the game entity to attack.
	 */
	AttackCommand(entity_id_t target);
	virtual ~AttackCommand() = default;

	inline command_t get_type() const override {
		return command_t::ATTACK;
	}

	/**
	 * Get the ID of the game entity to attack.
	 *
	 * @return Target entity ID.
	 */
	entity_id_t get_target() const;

private:
	/**
	 * Target game entity.
	 */
	entity_id_t target;
};

} // namespace openage::gamestate::component::command
