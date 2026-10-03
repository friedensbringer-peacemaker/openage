// Copyright 2026-2026 the openage authors. See copying.md for legal info.

/*
 * XR fork: the generated test list (testing/testlist.gen.cpp) names all tests
 * and demos of the full build. Builds without Qt or fonts leave out their
 * sources; these replacements keep the list linkable and report the reason.
 */

#include "config.h"

#include "error/error.h"


namespace openage {

#if !WITH_FONT || !WITH_QT
namespace {

[[noreturn]] void not_built(const char *name, const char *reason) {
	throw Error{MSG(err) << name << " is not available in this build (" << reason << ")."};
}

} // namespace
#endif

#if !WITH_FONT
namespace console::tests {
void render() {
	not_built("openage::console::tests::render", "OPENAGE_WITH_FONT=OFF");
}
void interactive() {
	not_built("openage::console::tests::interactive", "OPENAGE_WITH_FONT=OFF");
}
} // namespace console::tests

namespace renderer::tests {
void font() {
	not_built("openage::renderer::tests::font", "OPENAGE_WITH_FONT=OFF");
}
void font_manager() {
	not_built("openage::renderer::tests::font_manager", "OPENAGE_WITH_FONT=OFF");
}
} // namespace renderer::tests
#endif

#if !WITH_QT
namespace input::tests {
void action_demo() {
	not_built("openage::input::tests::action_demo", "OPENAGE_QT=OFF");
}
} // namespace input::tests
#endif

} // namespace openage
