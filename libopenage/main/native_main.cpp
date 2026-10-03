// Copyright 2026 the openage authors. See copying.md for legal info.

/*
 * Native C++ entry point for openage (XR fork).
 *
 * The regular entry point is the Python package (`python -m openage game`),
 * which mounts assets/ and cfg/ as a Python union filesystem and then calls
 * run_game() through Cython. On platforms without a Python runtime (e.g. the
 * Meta Quest), this executable starts the engine directly:
 *
 *   openage-native --root <dir> [--modpack hd_base] [--headless]
 *                  [--seconds N] [--width W --height H] [--check]
 *
 * <dir> must contain assets/ (with shaders and converted/{engine,<modpack>})
 * and cfg/. The converted modpacks (including the "engine" API modpack) are
 * produced offline with `python -m openage convert` and
 * `python -m openage convert-export-api`.
 */

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "assets/mod_manager.h"
#include "engine/engine.h"
#include "error/error.h"
#include "log/log.h"
#include "renderer/window.h"
#include "util/fslike/directory.h"
#include "util/path.h"

namespace {

struct native_args {
	std::string root = ".";
	std::vector<std::string> modpacks;
	bool headless = false;
	bool check = false;
	int seconds = 0;
	size_t width = 1024;
	size_t height = 768;
};

void usage(const char *argv0) {
	std::cerr << "usage: " << argv0
	          << " --root <dir> [--modpack <id>]... [--headless] [--seconds <n>]"
	             " [--width <w> --height <h>] [--check]\n";
}

bool parse_args(int argc, char **argv, native_args &args) {
	for (int i = 1; i < argc; ++i) {
		std::string arg = argv[i];
		auto value = [&]() -> std::string {
			if (i + 1 >= argc) {
				throw std::runtime_error("missing value for " + arg);
			}
			return argv[++i];
		};

		if (arg == "--root") {
			args.root = value();
		}
		else if (arg == "--modpack") {
			args.modpacks.push_back(value());
		}
		else if (arg == "--headless") {
			args.headless = true;
		}
		else if (arg == "--check") {
			args.check = true;
		}
		else if (arg == "--seconds") {
			args.seconds = std::stoi(value());
		}
		else if (arg == "--width") {
			args.width = std::stoul(value());
		}
		else if (arg == "--height") {
			args.height = std::stoul(value());
		}
		else if (arg == "--help" or arg == "-h") {
			return false;
		}
		else {
			throw std::runtime_error("unknown argument: " + arg);
		}
	}
	if (args.modpacks.empty()) {
		args.modpacks.push_back("hd_base");
	}
	return true;
}

/**
 * Same checks as openage/game/main.py: the "engine" API modpack and all
 * requested modpacks must exist in assets/converted.
 */
void check_modpacks(const openage::util::Path &root,
                    const std::vector<std::string> &wanted) {
	using namespace openage;

	auto modpack_dir = root / "assets" / "converted";
	auto mods = assets::ModManager::enumerate_modpacks(modpack_dir);

	auto has = [&](const std::string &id) {
		return std::any_of(mods.begin(), mods.end(), [&](const assets::ModpackInfo &mod) {
			return mod.id == id;
		});
	};

	for (const auto &mod : mods) {
		log::log(INFO << "found modpack " << mod.id << " " << mod.versionstr);
	}

	if (not has("engine")) {
		throw Error{MSG(err) << "Modpack 'engine' not found in " << modpack_dir
		                     << ". Export it with 'python -m openage convert-export-api'."};
	}
	for (const auto &id : wanted) {
		if (not has(id)) {
			throw Error{MSG(err) << "Modpack '" << id << "' not found in " << modpack_dir};
		}
	}
	if (not (root / "assets" / "shaders").is_dir()) {
		throw Error{MSG(err) << "assets/shaders missing in " << root};
	}
}

} // namespace


int main(int argc, char **argv) {
	using namespace openage;

	native_args args;
	try {
		if (not parse_args(argc, argv, args)) {
			usage(argv[0]);
			return EXIT_SUCCESS;
		}
	}
	catch (std::exception &exc) {
		std::cerr << exc.what() << "\n";
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	log::set_level(log::level::info);

	try {
		// plain directory instead of the Python union filesystem
		util::Path root{std::make_shared<util::fslike::Directory>(args.root), {}};
		log::log(INFO << "openage-native, root " << root);

		check_modpacks(root, args.modpacks);
		if (args.check) {
			log::log(INFO << "check ok");
			return EXIT_SUCCESS;
		}

		renderer::window_settings win_settings{};
		win_settings.width = args.width;
		win_settings.height = args.height;

		auto mode = args.headless ? engine::Engine::mode::HEADLESS
		                          : engine::Engine::mode::FULL;
		engine::Engine engine{mode, root, args.modpacks, win_settings};

		std::jthread timer;
		if (args.seconds > 0) {
			timer = std::jthread{[&engine, seconds = args.seconds]() {
				std::this_thread::sleep_for(std::chrono::seconds(seconds));
				log::log(INFO << "--seconds reached, stopping engine");
				engine.stop();
			}};
		}

		engine.loop();
		log::log(INFO << "engine loop finished");
	}
	catch (Error &err) {
		std::cerr << "openage-native: " << err << std::endl;
		return EXIT_FAILURE;
	}
	catch (std::exception &exc) {
		std::cerr << "openage-native: " << exc.what() << std::endl;
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
