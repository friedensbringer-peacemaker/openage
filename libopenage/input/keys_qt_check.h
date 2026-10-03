// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

/*
 * Compile time check (XR fork): the Qt-free input constants of input/keys.h
 * must have exactly the values of Qt. Only include this in builds with Qt.
 */

#include <QEvent>
#include <QtGlobal>

#include "input/keys.h"


namespace openage::input::qt_check {

// compare as int: the enums are of different types
constexpr bool same(int a, int b) {
	return a == b;
}

static_assert(same(event_type::NoEvent, QEvent::None));
static_assert(same(event_type::MouseButtonPress, QEvent::MouseButtonPress));
static_assert(same(event_type::MouseButtonRelease, QEvent::MouseButtonRelease));
static_assert(same(event_type::MouseButtonDblClick, QEvent::MouseButtonDblClick));
static_assert(same(event_type::MouseMove, QEvent::MouseMove));
static_assert(same(event_type::KeyPress, QEvent::KeyPress));
static_assert(same(event_type::KeyRelease, QEvent::KeyRelease));
static_assert(same(event_type::Wheel, QEvent::Wheel));

static_assert(same(mouse_button::NoButton, Qt::NoButton));
static_assert(same(mouse_button::LeftButton, Qt::LeftButton));
static_assert(same(mouse_button::RightButton, Qt::RightButton));
static_assert(same(mouse_button::MiddleButton, Qt::MiddleButton));
static_assert(same(mouse_button::BackButton, Qt::BackButton));
static_assert(same(mouse_button::ForwardButton, Qt::ForwardButton));

static_assert(same(modifier::NoModifier, Qt::NoModifier));
static_assert(same(modifier::ShiftModifier, Qt::ShiftModifier));
static_assert(same(modifier::ControlModifier, Qt::ControlModifier));
static_assert(same(modifier::AltModifier, Qt::AltModifier));
static_assert(same(modifier::MetaModifier, Qt::MetaModifier));
static_assert(same(modifier::KeypadModifier, Qt::KeypadModifier));
static_assert(same(modifier::GroupSwitchModifier, Qt::GroupSwitchModifier));

#define OPENAGE_INPUT_KEY_QT_CHECK(name, value)                               \
	static_assert(same(key::name, Qt::name), "input key differs from Qt: " #name); \
	static_assert(same(key::name, value));
OPENAGE_INPUT_KEYS(OPENAGE_INPUT_KEY_QT_CHECK)
#undef OPENAGE_INPUT_KEY_QT_CHECK

} // namespace openage::input::qt_check
