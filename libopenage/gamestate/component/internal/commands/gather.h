// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include "coord/phys.h"
#include "gamestate/component/internal/commands/base_command.h"
#include "gamestate/component/internal/commands/types.h"
#include "gamestate/types.h"


namespace openage::gamestate::component::command {

/**
 * Gather from a resource entity (XR fork, economy).
 */
class GatherCommand : public Command {
public:
	/**
	 * Creates a new gather command.
	 *
	 * @param target Resource entity (tree, mine, bush).
	 */
	GatherCommand(entity_id_t target);
	virtual ~GatherCommand() = default;

	inline command_t get_type() const override {
		return command_t::GATHER;
	}

	/**
	 * Get the resource entity.
	 *
	 * @return Entity ID.
	 */
	entity_id_t get_target() const;

private:
	const entity_id_t target;
};

} // namespace openage::gamestate::component::command
