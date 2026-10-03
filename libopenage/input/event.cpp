// Copyright 2015-2026 the openage authors. See copying.md for legal info.

#include "event.h"

#include <functional>
#include <utility>

#include "error/error.h"

namespace openage::input {

int event_class_hash::operator()(const event_class &c) const {
	return std::hash<int>()(static_cast<int>(c));
}


ClassCode::ClassCode(event_class cl, code_t code) :
	cl{cl},
	code{code} {}


std::vector<event_class> ClassCode::get_classes() const {
	std::vector<event_class> result;

	// use event_base to traverse up the class tree
	event_class c = this->cl;
	result.push_back(c);
	while (event_class_rel.count(c) > 0) {
		c = event_class_rel.at(c);
		result.push_back(c);
	}
	return result;
}


bool ClassCode::operator==(const ClassCode &other) const {
	return this->cl == other.cl && this->code == other.code;
}


bool ClassCode::is_subclass(const event_class &other) const {
	for (auto cl : this->get_classes()) {
		if (cl == other) {
			return true;
		}
	}
	return false;
}


int class_code_hash::operator()(const ClassCode &cc) const {
	return std::hash<int>()(static_cast<int>(cc.cl))
	       ^ std::hash<int>()(cc.code) * 3664657;
}


Event::Event(const renderer::WindowEvent &ev) :
	window_event{ev} {
	switch (ev.type) {
	case event_type::KeyPress:
	case event_type::KeyRelease:
		this->cc = ClassCode(event_class::KEYBOARD, ev.key);
		break;
	case event_type::MouseButtonPress:
	case event_type::MouseButtonRelease:
		this->cc = ClassCode(event_class::MOUSE_BUTTON, ev.button);
		break;
	case event_type::MouseButtonDblClick:
		this->cc = ClassCode(event_class::MOUSE_BUTTON_DBL, ev.button);
		break;
	case event_type::MouseMove:
		this->cc = ClassCode(event_class::MOUSE_MOVE, ev.button);
		break;
	case event_type::Wheel:
		if (ev.angle_delta_y > 0) {
			// forward
			this->cc = ClassCode(event_class::WHEEL, 1);
		}
		else {
			// backwards
			this->cc = ClassCode(event_class::WHEEL, -1);
		}
		break;
	// TODO: GUI events
	default:
		throw Error{MSG(err) << "Unrecognized input event type."};
	}
	this->mod_code = ev.modifiers;
	this->state = ev.type;
}

Event::Event(event_class cl, code_t code, modset_t mod, state_t state) :
	cc{cl, code},
	mod_code{mod},
	state{state},
	window_event{} {}


const renderer::WindowEvent &Event::get_window_event() const {
	return this->window_event;
}


bool Event::operator==(const Event &other) const {
	return this->cc == other.cc
	       && this->mod_code == other.mod_code
	       && this->state == other.state;
}


std::string Event::info() const {
	// TODO: human-readable info

	std::string result = "[Event: ";
	result += "class=" + std::to_string(static_cast<int>(this->cc.cl)) + ", ";
	result += "code=" + std::to_string(this->cc.code) + ", ";
	result += "modset=" + std::to_string(this->mod_code) + ", ";
	result += "state=" + std::to_string(this->state) + "]";
	return result;
}


int event_hash::operator()(const Event &e) const {
	return class_code_hash()(e.cc)
	       ^ std::hash<int>()(e.mod_code)
	       ^ std::hash<int>()(e.state);
}


} // namespace openage::input
