// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "constructable.h"


namespace openage::gamestate::component {

Constructable::Constructable(const std::shared_ptr<openage::event::EventLoop> &loop,
                             nyan::Object &ability) :
	APIComponent{loop, ability},
	progress{1.0},
	build_time{prod::DEFAULT_BUILD_TIME} {}

component_t Constructable::get_type() const {
	return component_t::CONSTRUCTABLE;
}

double Constructable::get_progress() const {
	return this->progress;
}

bool Constructable::is_complete() const {
	return this->progress >= 1.0;
}

void Constructable::start_foundation(double build_time) {
	this->progress = 0.0;
	this->build_time = build_time > 0.0 ? build_time : prod::DEFAULT_BUILD_TIME;
}

bool Constructable::add_work(double seconds) {
	if (this->is_complete()) {
		return false;
	}
	this->progress = prod::build_progress(this->progress, seconds, this->build_time);
	return this->is_complete();
}

double Constructable::get_build_time() const {
	return this->build_time;
}

} // namespace openage::gamestate::component
