// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include "gamestate/component/internal/commands/base_command.h"
#include "gamestate/component/internal/commands/types.h"
#include "gamestate/types.h"


namespace openage::gamestate::component::command {

/**
 * Construct a building (XR fork, production): walk next to the foundation and build it.
 */
class BuildCommand : public Command {
public:
	/**
	 * Creates a new build command.
	 *
	 * @param target Foundation entity.
	 */
	BuildCommand(entity_id_t target);
	virtual ~BuildCommand() = default;

	inline command_t get_type() const override {
		return command_t::BUILD;
	}

	/**
	 * Get the foundation entity.
	 *
	 * @return Entity ID.
	 */
	entity_id_t get_target() const;

private:
	const entity_id_t target;
};

} // namespace openage::gamestate::component::command
