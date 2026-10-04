// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "harvestable.h"

#include <algorithm>


namespace openage::gamestate::component {

Harvestable::Harvestable(const std::shared_ptr<openage::event::EventLoop> &loop,
                         nyan::Object &ability,
                         resource_t resource,
                         double amount) :
	APIComponent{loop, ability},
	resource{resource},
	amount{amount},
	depleted{false} {}

component_t Harvestable::get_type() const {
	return component_t::HARVESTABLE;
}

resource_t Harvestable::get_resource() const {
	return this->resource;
}

double Harvestable::get_amount() const {
	return this->amount;
}

double Harvestable::take(double amount) {
	double taken = std::clamp(amount, 0.0, this->amount);
	this->amount -= taken;
	if (this->amount < 1e-9) {
		this->amount = 0.0;
	}
	return taken;
}

bool Harvestable::is_depleted() const {
	return this->depleted or this->amount <= 0.0;
}

void Harvestable::set_depleted() {
	this->depleted = true;
	this->amount = 0.0;
}

} // namespace openage::gamestate::component
