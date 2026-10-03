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
 *                  [--gles] [--render-check <png>] [--shader-check]
 *
 * --gles selects an OpenGL ES 3.x context (like OPENAGE_GLES=1).
 * --render-check renders the game for --seconds (default 10) in a hidden
 * window, stores the final frame as PNG and exits. --shader-check compiles
 * and links all vertex/fragment shader pairs in assets/shaders and
 * assets/test/shaders in a hidden window and exits. Both never show a window.
 *
 * <dir> must contain assets/ (with shaders and converted/{engine,<modpack>})
 * and cfg/. The converted modpacks (including the "engine" API modpack) are
 * produced offline with `python -m openage convert` and
 * `python -m openage convert-export-api`.
 */

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "assets/mod_manager.h"
#include "engine/engine.h"
#include "error/error.h"
#include "log/log.h"
#include "renderer/gui/integration/public/gui_application_with_logger.h"
#include "renderer/renderer.h"
#include "renderer/resources/shader_source.h"
#include "renderer/resources/shader_template.h"
#include "renderer/window.h"
#include "util/fslike/directory.h"
#include "util/path.h"

namespace {

struct native_args {
	std::string root = ".";
	std::vector<std::string> modpacks;
	bool headless = false;
	bool check = false;
	bool gles = false;
	std::string render_check;
	bool shader_check = false;
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
		else if (arg == "--gles") {
			args.gles = true;
		}
		else if (arg == "--render-check") {
			args.render_check = value();
		}
		else if (arg == "--shader-check") {
			args.shader_check = true;
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

std::string read_file(const std::filesystem::path &file) {
	std::ifstream in{file};
	std::stringstream content;
	content << in.rdbuf();
	return content.str();
}

/**
 * Compile and link every vertex/fragment shader pair (X.vert.glsl + X.frag.glsl)
 * of assets/shaders and assets/test/shaders (with subfolders) in a hidden window.
 * Legacy shaders without #version (unused) are skipped, the shader template
 * of renderer demo 7 is filled with its snippets.
 *
 * @return Number of failed pairs.
 */
size_t shader_check(const std::filesystem::path &root,
                    const openage::util::Path &asset_root,
                    const openage::renderer::window_settings &settings) {
	using namespace openage;
	namespace fs = std::filesystem;

	renderer::gui::GuiApplicationWithLogger app{};
	auto window = renderer::Window::create("openage shader check", settings);
	auto renderer = window->make_renderer();

	// pairs used by the engine that don't share a name
	std::vector<std::pair<fs::path, fs::path>> pairs{
		{root / "assets/shaders/identity.vert.glsl", root / "assets/shaders/maptexture.frag.glsl"},
		{root / "assets/shaders/world3d.vert.glsl", root / "assets/shaders/world2d.frag.glsl"},
	};
	for (const auto &dir : {root / "assets/shaders", root / "assets/test/shaders"}) {
		std::vector<fs::path> frags;
		for (const auto &entry : fs::recursive_directory_iterator(dir)) {
			std::string name = entry.path().filename().string();
			if (name.size() > 10 and name.ends_with(".frag.glsl")) {
				frags.push_back(entry.path());
			}
		}
		std::sort(frags.begin(), frags.end());
		for (const auto &frag : frags) {
			std::string base = frag.filename().string();
			base.resize(base.size() - std::string{".frag.glsl"}.size());
			auto vert = frag.parent_path() / (base + ".vert.glsl");
			if (fs::exists(vert)) {
				pairs.emplace_back(vert, frag);
			}
		}
	}

	size_t ok = 0;
	size_t failed = 0;
	size_t skipped = 0;
	// shader templates (renderer demo 7) are filled with their snippets first
	const auto template_frag = root / "assets/test/shaders/demo_7_shader_command.frag.glsl";
	const auto template_dir = asset_root / "assets" / "test" / "shaders";

	for (const auto &[vert, frag] : pairs) {
		std::string vert_src = read_file(vert);
		std::string frag_src = read_file(frag);
		std::string name = vert.filename().string() + " + " + frag.filename().string();
		if (vert_src.find("#version") == std::string::npos
		    or frag_src.find("#version") == std::string::npos) {
			log::log(INFO << "shader check: skip " << name << " (legacy shader without #version)");
			skipped += 1;
			continue;
		}
		try {
			renderer::resources::ShaderSource frag_shader{renderer::resources::shader_lang_t::glsl,
			                                              renderer::resources::shader_stage_t::fragment,
			                                              std::move(frag_src)};
			if (frag == template_frag) {
				renderer::resources::ShaderTemplate frag_template{template_dir / frag.filename().string()};
				frag_template.load_snippets(template_dir / "demo_7_snippets");
				frag_shader = frag_template.generate_source();
				name += " (template)";
			}
			renderer->add_shader({
				renderer::resources::ShaderSource{renderer::resources::shader_lang_t::glsl,
			                                      renderer::resources::shader_stage_t::vertex,
			                                      std::move(vert_src)},
				frag_shader,
			});
			renderer->check_error();
			log::log(INFO << "shader check: ok   " << name);
			ok += 1;
		}
		catch (Error &err) {
			log::log(ERR << "shader check: FAIL " << name << ": " << err.what());
			failed += 1;
		}
	}

	log::log(INFO << "shader check: " << ok << " ok, " << failed << " failed, "
	              << skipped << " skipped");
	return failed;
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

		renderer::window_settings win_settings{};
		win_settings.width = args.width;
		win_settings.height = args.height;
		if (args.gles) {
			win_settings.backend = renderer::graphics_api_t::OPENGL_ES;
		}

		if (args.shader_check) {
			win_settings.visible = false;
			size_t failed = shader_check(args.root, root, win_settings);
			return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
		}

		check_modpacks(root, args.modpacks);
		if (args.check) {
			log::log(INFO << "check ok");
			return EXIT_SUCCESS;
		}

		const bool render_check = not args.render_check.empty();
		if (render_check) {
			if (args.headless) {
				throw Error{MSG(err) << "--render-check needs the renderer, not --headless"};
			}
			// the presenter stops the engine after storing the frame
			win_settings.visible = false;
			win_settings.capture_file = std::filesystem::absolute(args.render_check).string();
			win_settings.capture_delay = args.seconds > 0 ? args.seconds : 10;
		}

		auto mode = args.headless ? engine::Engine::mode::HEADLESS
		                          : engine::Engine::mode::FULL;
		engine::Engine engine{mode, root, args.modpacks, win_settings};

		std::jthread timer;
		if (args.seconds > 0 and not render_check) {
			timer = std::jthread{[&engine, seconds = args.seconds]() {
				std::this_thread::sleep_for(std::chrono::seconds(seconds));
				log::log(INFO << "--seconds reached, stopping engine");
				engine.stop();
			}};
		}

		engine.loop();
		log::log(INFO << "engine loop finished");

		if (render_check and not std::filesystem::exists(win_settings.capture_file)) {
			throw Error{MSG(err) << "render check: no frame stored to " << win_settings.capture_file};
		}
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
