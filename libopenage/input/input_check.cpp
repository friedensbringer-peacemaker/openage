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
 * - lock order of the game controller: a binding that creates a gamestate
 *   event while an event handler calls back into the controller (like
 *   game.drag_select) must not deadlock (both threads meet inside their
 *   critical sections, deterministic)
 *
 * Exit code 0 if all checks pass.
 */

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "config.h"
#include "error/error.h"
#include "event/event.h"
#include "event/event_loop.h"
#include "event/evententity.h"
#include "event/eventhandler.h"
#include "event/state.h"
#include "input/controller/game/binding_context.h"
#include "input/controller/game/controller.h"
#include "input/event.h"
#include "input/keys.h"
#include "input/text_to_event.h"
#include "time/time.h"

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


/// Meeting point of the input thread and the simulation thread (lock order check).
struct Rendezvous {
	std::mutex mutex;
	std::condition_variable cv;
	/// input thread is inside Controller::process -> binding transform
	bool binding_entered = false;
	/// simulation thread is inside an event handler (event loop locked)
	bool handler_entered = false;
	bool input_done = false;
	bool simulation_done = false;

	void set(bool &flag) {
		{
			std::lock_guard<std::mutex> lock{this->mutex};
			flag = true;
		}
		this->cv.notify_all();
	}

	bool wait(const bool &flag, std::chrono::milliseconds limit) {
		std::unique_lock<std::mutex> lock{this->mutex};
		return this->cv.wait_for(lock, limit, [&flag] { return flag; });
	}
};

class LockCheckEntity : public event::EventEntity {
public:
	explicit LockCheckEntity(const std::shared_ptr<event::EventLoop> &loop) :
		EventEntity{loop} {}

	size_t id() const override {
		return 1;
	}

	std::string idstr() const override {
		return "lock-check";
	}
};

/// Like game.drag_select: runs under the event loop lock and calls back into the controller.
class LockCheckSelectHandler : public event::OnceEventHandler {
public:
	LockCheckSelectHandler(Rendezvous &meet, const std::shared_ptr<input::game::Controller> &controller) :
		OnceEventHandler{"check.select"},
		meet{meet},
		controller{controller} {}

	void setup_event(const std::shared_ptr<event::Event> & /*event*/,
	                 const std::shared_ptr<event::State> & /*state*/) override {}

	void invoke(event::EventLoop & /*loop*/,
	            const std::shared_ptr<event::EventEntity> & /*target*/,
	            const std::shared_ptr<event::State> & /*state*/,
	            const time::time_t & /*time*/,
	            const param_map & /*params*/) override {
		this->meet.set(this->meet.handler_entered);
		// the input thread is inside its binding now
		this->meet.wait(this->meet.binding_entered, std::chrono::milliseconds(2000));
		this->controller->set_selected({1, 2});
	}

	time::time_t predict_invoke_time(const std::shared_ptr<event::EventEntity> & /*target*/,
	                                 const std::shared_ptr<event::State> & /*state*/,
	                                 const time::time_t &at) override {
		return at;
	}

private:
	Rendezvous &meet;
	std::shared_ptr<input::game::Controller> controller;
};

/**
 * Input thread: Controller::process() runs a binding that creates a gamestate
 * event (locks the event loop). Simulation thread: EventLoop::reach_time()
 * runs a handler that calls Controller::set_selected(). Both threads meet
 * inside their critical sections, then take the other lock. Until the XR fork
 * fix, process() held the controller mutex during the binding: deadlock.
 */
void controller_lock_order() {
	using namespace std::chrono_literals;
	Rendezvous meet;

	auto loop = std::make_shared<event::EventLoop>();
	auto state = std::make_shared<event::State>(loop);
	auto entity = std::make_shared<LockCheckEntity>(loop);
	auto controller = std::make_shared<input::game::Controller>(std::unordered_set<size_t>{0}, 0);
	loop->add_event_handler(std::make_shared<LockCheckSelectHandler>(meet, controller));
	loop->create_event("check.select", entity, state, time::time_t::from_double(1));

	auto ctx = std::make_shared<input::game::BindingContext>();
	const input::Event release{input::event_class::MOUSE_BUTTON,
	                           input::mouse_button::LeftButton,
	                           input::modifier::NoModifier,
	                           input::event_type::MouseButtonRelease};
	bool created = false;
	input::game::binding_func_t transform = [&](const input::event_arguments & /*args*/,
	                                            const std::shared_ptr<input::game::Controller> ctrl) {
		meet.set(meet.binding_entered);
		// the simulation thread holds the event loop lock now
		meet.wait(meet.handler_entered, 2000ms);
		ctrl->get_controlled();
		auto ev = loop->create_event("check.select", entity, state, time::time_t::from_double(2));
		created = ev != nullptr;
		return ev;
	};
	ctx->bind(release, input::game::binding_action{input::game::forward_action_t::CLEAR, transform});

	std::thread simulation{[&] {
		loop->reach_time(time::time_t::from_double(1.5), state);
		meet.set(meet.simulation_done);
	}};
	std::thread input_thread{[&] {
		const input::event_arguments args{release, coord::input{0, 0}, coord::input_delta{0, 0}};
		controller->process(args, ctx);
		meet.set(meet.input_done);
	}};

	const bool simulation_ok = meet.wait(meet.simulation_done, 5000ms);
	const bool input_ok = meet.wait(meet.input_done, 5000ms);
	if (not simulation_ok or not input_ok) {
		// the threads are stuck in each other's locks and cannot be joined
		std::cerr << "FAIL: controller lock order: deadlock between Controller::process (binding -> "
		             "EventLoop::create_event) and an event handler (-> Controller::set_selected)\n"
		          << std::flush;
		std::_Exit(EXIT_FAILURE);
	}
	simulation.join();
	input_thread.join();

	check(meet.binding_entered and meet.handler_entered, "controller lock order: threads met");
	check(created, "controller lock order: binding created its event");
	check(controller->get_selected() == std::vector<gamestate::entity_id_t>{1, 2},
	      "controller lock order: selection from the event handler");
	std::cout << "controller lock order: binding and event handler ran concurrently without deadlock\n";
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
	controller_lock_order();

	std::cout << "input check: " << checks << " checks, " << failures << " failed\n";
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
