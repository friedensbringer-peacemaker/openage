// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "production_queue.h"


namespace openage::gamestate::component {

ProductionQueue::ProductionQueue(const std::shared_ptr<openage::event::EventLoop> &loop,
                                 nyan::Object &ability,
                                 std::vector<Creatable> &&creatables) :
	APIComponent{loop, ability},
	creatables{std::move(creatables)},
	queue{} {}

component_t ProductionQueue::get_type() const {
	return component_t::PRODUCTION_QUEUE;
}

const std::vector<ProductionQueue::Creatable> &ProductionQueue::get_creatables() const {
	return this->creatables;
}

const ProductionQueue::Creatable *ProductionQueue::find(const std::string &name) const {
	for (const auto &creatable : this->creatables) {
		if (creatable.name == name) {
			return &creatable;
		}
	}
	return nullptr;
}

prod::TrainQueue &ProductionQueue::get_queue() {
	return this->queue;
}

} // namespace openage::gamestate::component
