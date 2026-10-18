// Copyright 2023-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <optional>

#include "coord/phys.h"
#include "gamestate/component/internal/commands/base_command.h"
#include "gamestate/component/internal/commands/types.h"


namespace openage::gamestate::component::command {

/**
 * Command for moving to a target position.
 */
class MoveCommand : public Command {
public:
	/**
	 * Creates a new move command.
	 *
	 * @param target Target position coordinates.
	 * @param max_speed XR fork W4: speed limit (tiles/s) of a formation moving with
	 *                  its slowest member; nothing = the speed of the unit.
	 */
	MoveCommand(const coord::phys3 &target, std::optional<double> max_speed = std::nullopt);
	virtual ~MoveCommand() = default;

	inline command_t get_type() const override {
		return command_t::MOVE;
	}

	/**
	 * Get the target position.
	 *
	 * @return Target position coordinates.
	 */
	const coord::phys3 &get_target() const;

	/**
	 * Speed limit of the move (XR fork W4, formations), nothing = none.
	 */
	const std::optional<double> &get_max_speed() const;

private:
	/**
	 * Target position.
	 */
	const coord::phys3 target;

	/**
	 * Speed limit (XR fork W4).
	 */
	const std::optional<double> max_speed;
};

} // namespace openage::gamestate::component::command
