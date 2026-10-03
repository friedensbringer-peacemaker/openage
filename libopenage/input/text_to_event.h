// Copyright 2016-2023 the openage authors. See copying.md for legal info.

#pragma once

#include <string>

#include "input/event.h"

namespace openage::input {

/**
 * Convert a string to an event.
 *
 * Accepts the portable format of QKeySequence ("Ctrl+X", "Shift+F5", "Esc")
 * and blank-separated modifiers as in cfg/keybinds.oac ("Ctrl x"), without
 * using Qt. Key codes and modifiers have the values of Qt (input/keys.h).
 *
 * TODO: Mouse/Wheel/GUI events
 *
 * @throws if the string is not a valid event.
 */
Event text_to_event(const std::string &event_str);

} // namespace openage::input
