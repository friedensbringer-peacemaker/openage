// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "gather.h"


namespace openage::gamestate::component {

Gather::Gather(const std::shared_ptr<openage::event::EventLoop> &loop,
               nyan::Object &ability) :
	APIComponent{loop, ability},
	skills{},
	job{},
	carried{0.0},
	carried_type{resource_t::FOOD} {}

component_t Gather::get_type() const {
	return component_t::GATHER;
}

void Gather::set_skill(resource_t resource, const Skill &skill) {
	auto idx = static_cast<size_t>(resource);
	if (idx < RESOURCE_COUNT) {
		this->skills[idx] = skill;
	}
}

const Gather::Skill &Gather::get_skill(resource_t resource) const {
	return this->skills.at(static_cast<size_t>(resource));
}

bool Gather::can_gather(resource_t resource) const {
	auto idx = static_cast<size_t>(resource);
	return idx < RESOURCE_COUNT and this->skills[idx].available;
}

Gather::Job &Gather::get_job() {
	return this->job;
}

double Gather::get_carried() const {
	return this->carried;
}

resource_t Gather::get_carried_type() const {
	return this->carried_type;
}

void Gather::set_carried(resource_t type, double amount) {
	this->carried_type = type;
	this->carried = amount;
}

} // namespace openage::gamestate::component
