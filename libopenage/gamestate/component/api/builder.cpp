// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "builder.h"


namespace openage::gamestate::component {

Builder::Builder(const std::shared_ptr<openage::event::EventLoop> &loop,
                 nyan::Object &ability,
                 std::vector<Buildable> &&buildables) :
	APIComponent{loop, ability},
	buildables{std::move(buildables)},
	job{} {}

component_t Builder::get_type() const {
	return component_t::BUILDER;
}

const std::vector<Builder::Buildable> &Builder::get_buildables() const {
	return this->buildables;
}

const Builder::Buildable *Builder::find(const std::string &name) const {
	for (const auto &buildable : this->buildables) {
		if (buildable.name == name) {
			return &buildable;
		}
	}
	return nullptr;
}

Builder::Job &Builder::get_job() {
	return this->job;
}

} // namespace openage::gamestate::component
