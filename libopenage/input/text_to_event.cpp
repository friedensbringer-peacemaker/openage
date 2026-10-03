// Copyright 2016-2026 the openage authors. See copying.md for legal info.

#include "text_to_event.h"

#include <array>
#include <cctype>
#include <string_view>
#include <vector>

#include "error/error.h"
#include "input/keys.h"


namespace openage::input {

namespace {

/**
 * Key names, compatible with the portable text format of QKeySequence
 * (case-insensitive), plus a few longer aliases.
 */
struct key_name {
	std::string_view name;
	int code;
};

constexpr std::array<key_name, 37> key_names{{
	{"Space", key::Key_Space},
	{"Esc", key::Key_Escape},
	{"Escape", key::Key_Escape},
	{"Tab", key::Key_Tab},
	{"Backtab", key::Key_Backtab},
	{"Backspace", key::Key_Backspace},
	{"Return", key::Key_Return},
	{"Enter", key::Key_Enter},
	{"Ins", key::Key_Insert},
	{"Insert", key::Key_Insert},
	{"Del", key::Key_Delete},
	{"Delete", key::Key_Delete},
	{"Pause", key::Key_Pause},
	{"Print", key::Key_Print},
	{"SysReq", key::Key_SysReq},
	{"Clear", key::Key_Clear},
	{"Home", key::Key_Home},
	{"End", key::Key_End},
	{"Left", key::Key_Left},
	{"Up", key::Key_Up},
	{"Right", key::Key_Right},
	{"Down", key::Key_Down},
	{"PgUp", key::Key_PageUp},
	{"PageUp", key::Key_PageUp},
	{"PgDown", key::Key_PageDown},
	{"PageDown", key::Key_PageDown},
	{"Shift", key::Key_Shift},
	{"Control", key::Key_Control},
	{"Meta", key::Key_Meta},
	{"Alt", key::Key_Alt},
	{"CapsLock", key::Key_CapsLock},
	{"NumLock", key::Key_NumLock},
	{"ScrollLock", key::Key_ScrollLock},
	{"Menu", key::Key_Menu},
	{"Help", key::Key_Help},
	{"Plus", key::Key_Plus},
	{"Minus", key::Key_Minus},
}};

struct modifier_name {
	std::string_view name;
	int mod;
};

constexpr std::array<modifier_name, 6> modifier_names{{
	{"Ctrl", modifier::ControlModifier},
	{"Control", modifier::ControlModifier},
	{"Shift", modifier::ShiftModifier},
	{"Alt", modifier::AltModifier},
	{"Meta", modifier::MetaModifier},
	{"Num", modifier::KeypadModifier},
}};

bool equal_nocase(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (size_t i = 0; i < a.size(); ++i) {
		if (std::tolower(static_cast<unsigned char>(a[i]))
		    != std::tolower(static_cast<unsigned char>(b[i]))) {
			return false;
		}
	}
	return true;
}

/**
 * Decode a string that consists of exactly one UTF-8 encoded code point.
 *
 * @return Code point, or -1 if the string is not a single valid code point.
 */
int single_codepoint(std::string_view str) {
	if (str.empty()) {
		return -1;
	}
	auto byte = [&](size_t i) {
		return static_cast<unsigned char>(str[i]);
	};

	size_t len;
	int cp;
	if (byte(0) < 0x80) {
		len = 1;
		cp = byte(0);
	}
	else if ((byte(0) & 0xe0) == 0xc0) {
		len = 2;
		cp = byte(0) & 0x1f;
	}
	else if ((byte(0) & 0xf0) == 0xe0) {
		len = 3;
		cp = byte(0) & 0x0f;
	}
	else if ((byte(0) & 0xf8) == 0xf0) {
		len = 4;
		cp = byte(0) & 0x07;
	}
	else {
		return -1;
	}

	if (str.size() != len) {
		return -1;
	}
	for (size_t i = 1; i < len; ++i) {
		if ((byte(i) & 0xc0) != 0x80) {
			return -1;
		}
		cp = (cp << 6) | (byte(i) & 0x3f);
	}
	return cp;
}

/**
 * Key code of a single key name ("A", "F5", "Esc", "`", "ä" ...).
 *
 * @return Key code, or -1 if the name is unknown.
 */
int parse_key(std::string_view name) {
	for (const auto &entry : key_names) {
		if (equal_nocase(name, entry.name)) {
			return entry.code;
		}
	}

	// function keys F1 .. F35
	if (name.size() >= 2 and name.size() <= 3 and (name[0] == 'F' or name[0] == 'f')) {
		int number = 0;
		bool digits = true;
		for (size_t i = 1; i < name.size(); ++i) {
			if (not std::isdigit(static_cast<unsigned char>(name[i]))) {
				digits = false;
				break;
			}
			number = number * 10 + (name[i] - '0');
		}
		if (digits and number >= 1 and number <= 35 and name[1] != '0') {
			return key::Key_F1 + number - 1;
		}
	}

	// single printable character: the key code is the upper case code point
	int cp = single_codepoint(name);
	if (cp < 0x20 or cp == 0x7f) {
		return -1;
	}
	if (cp >= 'a' and cp <= 'z') {
		return cp - 'a' + 'A';
	}
	// Latin-1 lower case letters (except division sign and y with diaeresis)
	if (cp >= 0xe0 and cp <= 0xfe and cp != 0xf7) {
		return cp - 0x20;
	}
	return cp;
}

} // namespace


Event text_to_event(const std::string &event_str) {
	std::string_view str{event_str};

	// trim surrounding whitespace
	while (not str.empty() and std::isspace(static_cast<unsigned char>(str.front()))) {
		str.remove_prefix(1);
	}
	while (not str.empty() and std::isspace(static_cast<unsigned char>(str.back()))) {
		str.remove_suffix(1);
	}
	if (str.empty()) {
		throw Error{MSG(err) << "Invalid event string: key sequence is empty"};
	}

	// split into parts at '+' (QKeySequence format, "Ctrl+X")
	// or at blanks (config format, "Ctrl x"). A '+' that is the
	// last character is the plus key ("Ctrl++", "+").
	std::vector<std::string_view> parts;
	size_t start = 0;
	for (size_t i = 0; i < str.size(); ++i) {
		char c = str[i];
		bool sep = (c == '+' and i > start) or c == ' ';
		if (sep) {
			if (i > start) {
				parts.push_back(str.substr(start, i - start));
			}
			start = i + 1;
		}
	}
	if (start < str.size()) {
		parts.push_back(str.substr(start));
	}
	if (parts.empty()) {
		throw Error{MSG(err) << "Invalid event string: '" << event_str << "'"};
	}

	int mods = modifier::NoModifier;
	for (size_t i = 0; i + 1 < parts.size(); ++i) {
		bool found = false;
		for (const auto &entry : modifier_names) {
			if (equal_nocase(parts[i], entry.name)) {
				mods |= entry.mod;
				found = true;
				break;
			}
		}
		if (not found) {
			throw Error{MSG(err) << "Invalid event string: unknown modifier '"
			                     << parts[i] << "' in '" << event_str << "'"};
		}
	}

	int code = parse_key(parts.back());
	if (code < 0) {
		throw Error{MSG(err) << "Invalid event string: unknown key '"
		                     << parts.back() << "' in '" << event_str << "'"};
	}

	return Event(event_class::KEYBOARD,
	             code,
	             mods,
	             event_type::KeyRelease // TODO: configurable?
	);
}

} // namespace openage::input
