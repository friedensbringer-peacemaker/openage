// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "build.h"


namespace openage::gamestate::component::command {

BuildCommand::BuildCommand(entity_id_t target) :
	target{target} {}

entity_id_t BuildCommand::get_target() const {
	return this->target;
}

} // namespace openage::gamestate::component::command
