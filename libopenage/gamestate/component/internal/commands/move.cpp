// Copyright 2023-2026 the openage authors. See copying.md for legal info.

#include "move.h"


namespace openage::gamestate::component::command {

MoveCommand::MoveCommand(const coord::phys3 &target, std::optional<double> max_speed) :
	target{target},
	max_speed{max_speed} {}

const coord::phys3 &MoveCommand::get_target() const {
	return this->target;
}

const std::optional<double> &MoveCommand::get_max_speed() const {
	return this->max_speed;
}

} // namespace openage::gamestate::component::command
