// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <string>
#include <vector>

#include "util/path.h"


namespace openage::engine {

/**
 * Check that a root directory can start the engine (XR fork, same checks as
 * openage/game/main.py): cfg/ and assets/shaders exist, and assets/converted
 * contains the "engine" API modpack and all requested modpacks.
 *
 * Meant for embedders that show the problem as plain text (e.g. the VR
 * loading screen of the Quest app) instead of a failing engine start.
 *
 * @param root Root directory (contains assets/ and cfg/).
 * @param modpacks IDs of the modpacks that will be loaded.
 *
 * @return Empty string if everything is there, else a description of the problem.
 */
std::string check_root(const util::Path &root,
                       const std::vector<std::string> &modpacks);

/**
 * Same as check_root(const util::Path &, ...) for a plain directory in the
 * native file system.
 */
std::string check_root(const std::string &root_dir,
                       const std::vector<std::string> &modpacks);

} // namespace openage::engine
