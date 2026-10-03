// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "check_root.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <sstream>

#include "assets/mod_manager.h"
#include "error/error.h"
#include "log/log.h"
#include "util/fslike/directory.h"


namespace openage::engine {

std::string check_root(const util::Path &root,
                       const std::vector<std::string> &modpacks) {
	std::ostringstream problem;
	try {
		if (not (root / "cfg").is_dir()) {
			problem << "cfg/ missing in " << root;
			return problem.str();
		}
		if (not (root / "assets" / "shaders").is_dir()) {
			problem << "assets/shaders missing in " << root;
			return problem.str();
		}

		auto modpack_dir = root / "assets" / "converted";
		auto mods = assets::ModManager::enumerate_modpacks(modpack_dir);
		for (const auto &mod : mods) {
			log::log(INFO << "found modpack " << mod.id << " " << mod.versionstr);
		}

		auto has = [&](const std::string &id) {
			return std::any_of(mods.begin(), mods.end(), [&](const assets::ModpackInfo &mod) {
				return mod.id == id;
			});
		};

		if (not has("engine")) {
			problem << "Modpack 'engine' not found in " << modpack_dir
			        << ". Export it with 'python -m openage convert-export-api'.";
			return problem.str();
		}
		for (const auto &id : modpacks) {
			if (not has(id)) {
				problem << "Modpack '" << id << "' not found in " << modpack_dir;
				return problem.str();
			}
		}
	}
	catch (Error &err) {
		return err.msg.text;
	}
	catch (std::exception &exc) {
		return exc.what();
	}
	return {};
}

std::string check_root(const std::string &root_dir,
                       const std::vector<std::string> &modpacks) {
	util::Path root{std::make_shared<util::fslike::Directory>(root_dir), {}};
	return check_root(root, modpacks);
}

} // namespace openage::engine
