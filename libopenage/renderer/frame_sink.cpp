// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "frame_sink.h"

#include <cmath>

#include "input/keys.h"


namespace openage::renderer {

// the sink uses the numeric values of input/keys.h (both are the values of Qt)
static_assert(SinkInputEvent::kLeftButton == input::mouse_button::LeftButton);
static_assert(SinkInputEvent::kRightButton == input::mouse_button::RightButton);
static_assert(SinkInputEvent::kMiddleButton == input::mouse_button::MiddleButton);
static_assert(SinkInputEvent::kKeyEscape == input::key::Key_Escape);
static_assert(SinkInputEvent::kKeyControl == input::key::Key_Control);
static_assert(SinkInputEvent::kControlModifier == input::modifier::ControlModifier);
static_assert(input::event_type::MouseButtonDblClick == 4);

bool to_window_event(const SinkInputEvent &in, WindowEvent &out) {
	out = WindowEvent{};
	out.modifiers = in.modifiers;
	out.buttons = in.buttons;
	out.x = in.x;
	out.y = in.y;
	out.global_x = in.x;
	out.global_y = in.y;

	switch (in.type) {
	case SinkInputEvent::kMouseMove:
		out.type = input::event_type::MouseMove;
		return true;
	case SinkInputEvent::kMouseDown:
		out.type = input::event_type::MouseButtonPress;
		out.button = in.button;
		return true;
	case SinkInputEvent::kMouseUp:
		out.type = input::event_type::MouseButtonRelease;
		out.button = in.button;
		return true;
	case SinkInputEvent::kMouseDoubleClick:
		out.type = input::event_type::MouseButtonDblClick;
		out.button = in.button;
		return true;
	case SinkInputEvent::kWheel:
		out.type = input::event_type::Wheel;
		out.angle_delta_y = static_cast<int>(std::lround(in.wheel * 120.0f));
		return out.angle_delta_y != 0;
	case SinkInputEvent::kKeyDown:
		out.type = input::event_type::KeyPress;
		out.key = in.key;
		return true;
	case SinkInputEvent::kKeyUp:
		out.type = input::event_type::KeyRelease;
		out.key = in.key;
		return true;
	default:
		// focus and leave have no window event equivalent
		return false;
	}
}

} // namespace openage::renderer
