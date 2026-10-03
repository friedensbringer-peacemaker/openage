// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "drop_site.h"


namespace openage::gamestate::component {

DropSite::DropSite(const std::shared_ptr<openage::event::EventLoop> &loop,
                   nyan::Object &ability,
                   const std::array<bool, RESOURCE_COUNT> &accepts) :
	APIComponent{loop, ability},
	accepted{accepts} {}

component_t DropSite::get_type() const {
	return component_t::DROP_SITE;
}

bool DropSite::accepts(resource_t resource) const {
	auto idx = static_cast<size_t>(resource);
	return idx < RESOURCE_COUNT and this->accepted[idx];
}

} // namespace openage::gamestate::component
