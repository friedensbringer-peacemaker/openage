// Copyright 2026-2026 the openage authors. See copying.md for legal info.

/*
 * Host test of the Qt-free input constants and text parser (XR fork).
 *
 *   openage-input-check [cfg/keybinds.oac]
 *
 * - every key of input/keys.h is parsed back from its name
 *   (with Qt: the name Qt gives the key, and the result is compared with
 *   QKeySequence::fromString; the values of all constants are compared with
 *   Qt at compile time in input/keys_qt_check.h)
 * - modifier combinations (sweep over Ctrl/Shift/Alt/Meta/Num)
 * - fixed cases incl. config format ("Ctrl x") and invalid strings
 * - all keyboard bindings of cfg/keybinds.oac are parsed
 *
 * Exit code 0 if all checks pass.
 */

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "config.h"
#include "error/error.h"
#include "input/keys.h"
#include "input/text_to_event.h"

#if WITH_QT
	#include <QKeySequence>
	#include <QString>

	#include "input/keys_qt_check.h"
#endif


namespace {

using namespace openage;

size_t checks = 0;
size_t failures = 0;

void check(bool ok, const std::string &what) {
	checks += 1;
	if (not ok) {
		failures += 1;
		std::cerr << "FAIL: " << what << "\n";
	}
}

/// Parse a string; returns false if it throws.
bool parse(const std::string &str, int &code, int &mods) {
	try {
		auto ev = input::text_to_event(str);
		code = ev.cc.code;
		mods = ev.mod_code;
		return ev.cc.cl == input::event_class::KEYBOARD
		       and ev.state == input::event_type::KeyRelease;
	}
	catch (Error &) {
		return false;
	}
}

void expect(const std::string &str, int code, int mods) {
	int got_code = 0;
	int got_mods = 0;
	bool ok = parse(str, got_code, got_mods);
	std::ostringstream what;
	what << "'" << str << "' -> " << std::hex << "code 0x" << got_code << " mods 0x" << got_mods
	     << ", expected code 0x" << code << " mods 0x" << mods;
	check(ok and got_code == code and got_mods == mods, what.str());
}

void expect_invalid(const std::string &str) {
	int code = 0;
	int mods = 0;
	check(not parse(str, code, mods), "'" + str + "' should be rejected");
}

struct named_key {
	const char *name;
	int code;
};

const std::vector<named_key> all_keys{
#define OPENAGE_INPUT_KEY_ENTRY(name, value) {#name, value},
	OPENAGE_INPUT_KEYS(OPENAGE_INPUT_KEY_ENTRY)
#undef OPENAGE_INPUT_KEY_ENTRY
};

constexpr int mod_bits[] = {
	input::modifier::ControlModifier,
	input::modifier::ShiftModifier,
	input::modifier::AltModifier,
	input::modifier::MetaModifier,
	input::modifier::KeypadModifier,
};

void fixed_cases() {
	using namespace input;
	expect("Ctrl+X", key::Key_X, modifier::ControlModifier);
	expect("ctrl+x", key::Key_X, modifier::ControlModifier);
	expect("Ctrl x", key::Key_X, modifier::ControlModifier);
	expect("Shift Escape", key::Key_Escape, modifier::ShiftModifier);
	expect("Shift+Esc", key::Key_Escape, modifier::ShiftModifier);
	expect("Ctrl+Shift+Alt+Meta+F5", key::Key_F5,
	       modifier::ControlModifier | modifier::ShiftModifier | modifier::AltModifier | modifier::MetaModifier);
	expect("Num+5", key::Key_5, modifier::KeypadModifier);
	expect("Return", key::Key_Return, modifier::NoModifier);
	expect("Space", key::Key_Space, modifier::NoModifier);
	expect("Delete", key::Key_Delete, modifier::NoModifier);
	expect("Del", key::Key_Delete, modifier::NoModifier);
	expect("PgDown", key::Key_PageDown, modifier::NoModifier);
	expect("F1", key::Key_F1, modifier::NoModifier);
	expect("F35", key::Key_F35, modifier::NoModifier);
	expect("`", key::Key_QuoteLeft, modifier::NoModifier);
	expect("+", key::Key_Plus, modifier::NoModifier);
	expect("Ctrl++", key::Key_Plus, modifier::ControlModifier);
	expect("a", key::Key_A, modifier::NoModifier);
	expect("1", key::Key_1, modifier::NoModifier);
	expect("\xc3\xa4", 0xc4, modifier::NoModifier); // a with diaeresis -> upper case code point
	expect("  Up  ", key::Key_Up, modifier::NoModifier);

	expect_invalid("");
	expect_invalid("   ");
	expect_invalid("F0");
	expect_invalid("F36");
	expect_invalid("Foo");
	expect_invalid("Hyper+X");
	expect_invalid("Ctrl+Foo");
}

void all_named_keys() {
	// every key of keys.h can be written as its name without "Key_"
	// for the keys that have a parser name or are a single character
	size_t parsed = 0;
	for (const auto &key : all_keys) {
		std::string name = std::string{key.name}.substr(4);
		int code = 0;
		int mods = 0;
		if (parse(name, code, mods) and code == key.code) {
			parsed += 1;
		}
	}
	std::cout << "keys parsed from their constant name: " << parsed << "/" << all_keys.size() << "\n";
}

#if WITH_QT
/// Compare with Qt: names of all keys, modifier sweep, QKeySequence::fromString.
void compare_with_qt() {
	size_t compared = 0;
	size_t qt_agreed = 0;
	size_t skipped = 0;
	for (const auto &key : all_keys) {
		std::string name = QKeySequence(key.code).toString(QKeySequence::PortableText).toStdString();
		auto qt_own = QKeySequence::fromString(QString::fromStdString(name), QKeySequence::PortableText);
		bool qt_round_trip = qt_own.count() == 1 and qt_own[0].toCombined() == key.code;
		if (name.empty() or name.find(' ') != std::string::npos or key.code == input::key::Key_unknown
		    or (not qt_round_trip and name != ",")) {
			// Qt has no portable name for the key (Super/Hyper keys get a
			// meaningless character), or a name with blanks that the config
			// format (blank as separator) can't express
			std::cout << "skipped " << key.name << " (Qt name '" << name << "')\n";
			skipped += 1;
			continue;
		}

		for (int combo = 0; combo < (1 << 5); ++combo) {
			int mods = 0;
			for (int bit = 0; bit < 5; ++bit) {
				if (combo & (1 << bit)) {
					mods |= mod_bits[bit];
				}
			}
			QKeySequence qseq{QKeyCombination::fromCombined(mods | key.code)};
			std::string str = qseq.toString(QKeySequence::PortableText).toStdString();

			// our parser must give the key and modifiers that Qt named
			expect(str, key.code, mods);
			compared += 1;

			// and agree with Qt's parser where Qt can parse its own string
			// (not e.g. for "," which separates key sequences in Qt)
			auto qt_parsed = QKeySequence::fromString(QString::fromStdString(str), QKeySequence::PortableText);
			if (qt_parsed.count() == 1) {
				int code = 0;
				int parsed_mods = 0;
				parse(str, code, parsed_mods);
				check(qt_parsed[0].toCombined() == (parsed_mods | code),
				      "QKeySequence::fromString('" + str + "') differs");
				qt_agreed += 1;
			}
		}
	}
	std::cout << "compared with QKeySequence: " << compared << " key/modifier combinations of "
	          << (all_keys.size() - skipped) << " keys (" << skipped << " keys without parsable Qt name), "
	          << qt_agreed << " also equal to QKeySequence::fromString\n";
}
#endif

void keybinds_file(const std::string &file) {
	std::ifstream in{file};
	if (not in) {
		check(false, "cannot open " + file);
		return;
	}

	size_t parsed = 0;
	std::string line;
	while (std::getline(in, line)) {
		std::istringstream words{line};
		std::string cmd;
		std::string action;
		words >> cmd >> action;
		if (cmd != "set") {
			continue;
		}
		std::string binding;
		std::getline(words, binding);
		if (binding.find("MOUSE") != std::string::npos or binding.find("WHEEL") != std::string::npos) {
			continue; // mouse/wheel bindings are not keyboard events
		}
		int code = 0;
		int mods = 0;
		check(parse(binding, code, mods), "keybinds: " + action + " '" + binding + "'");
		parsed += 1;
	}
	std::cout << "keyboard bindings parsed from " << file << ": " << parsed << "\n";
}

} // namespace


int main(int argc, char **argv) {
	fixed_cases();
	all_named_keys();
#if WITH_QT
	compare_with_qt();
#else
	std::cout << "built without Qt: constants not compared with Qt at run time\n";
#endif
	if (argc > 1) {
		keybinds_file(argv[1]);
	}

	std::cout << "input check: " << checks << " checks, " << failures << " failed\n";
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
