// Copyright 2026-2026 the openage authors. See copying.md for legal info.

/*
 * Host test of the mouse selection rules and the presenter frame statistics (XR fork).
 *
 *   openage-select-check
 *
 * Parameter sweeps over the dependency-free parts:
 * - click or drag: every press/release distance in -20..20 px per axis against
 *   the 6 px threshold (and other thresholds)
 * - next selection: replace/add, click toggles, rectangles add without
 *   duplicates, foreign entities only alone and never added, for selections
 *   of 0..6 entities and every hit
 * - on screen: NDC grid around [-1, 1]
 * - frame statistics: fps, averages, maximum, report period, reset
 * Exit code 0 if all checks pass. No engine dependencies.
 */

#include <cmath>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "gamestate/selection_rules.h"
#include "presenter/frame_stats.h"

using namespace openage::gamestate;

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, const std::string &what) {
	checks += 1;
	if (not ok) {
		failures += 1;
		if (failures <= 40) {
			std::cerr << "FAIL: " << what << std::endl;
		}
	}
}

bool near(double a, double b, double eps = 1e-6) {
	return std::abs(a - b) <= eps;
}

void check_click() {
	for (int threshold : {1, 4, 6, 10}) {
		for (int dx = -20; dx <= 20; ++dx) {
			for (int dy = -20; dy <= 20; ++dy) {
				bool expected = std::abs(dx) < threshold and std::abs(dy) < threshold;
				check(select::is_click(dx, dy, threshold) == expected,
				      "is_click(" + std::to_string(dx) + ", " + std::to_string(dy) + ", " + std::to_string(threshold) + ")");
			}
		}
	}
	// default threshold: 5 px still a click, 6 px a drag (both axes, both signs)
	check(select::is_click(5, -5) and not select::is_click(6, 0) and not select::is_click(0, -6), "default 6 px");
	check(select::is_click(0, 0), "no movement is a click");
}

void check_next_selection() {
	using ids = std::vector<int>;
	for (int n = 0; n <= 6; ++n) {
		ids current;
		for (int i = 0; i < n; ++i) {
			current.push_back(10 + i);
		}
		for (int hit = 0; hit < 20; ++hit) {
			ids hits{hit};
			bool selected = hit >= 10 and hit < 10 + n;
			std::string tag = "n=" + std::to_string(n) + " hit=" + std::to_string(hit);

			// replace: click on an own or foreign entity, click on the ground
			check(select::next_selection(current, false, hits, false, false, true) == hits, "click replaces " + tag);
			check(select::next_selection(current, false, hits, true, false, true) == hits, "foreign click alone " + tag);
			check(select::next_selection(current, false, ids{}, false, false, true).empty(), "ground clears " + tag);

			// shift + click toggles own entities
			auto added = select::next_selection(current, false, hits, false, true, true);
			if (selected) {
				check(added.size() == current.size() - 1, "shift click deselects " + tag);
				check(std::find(added.begin(), added.end(), hit) == added.end(), "deselected gone " + tag);
			}
			else {
				check(added.size() == current.size() + 1 and added.back() == hit, "shift click adds " + tag);
			}
			// shift + click on a foreign entity keeps the selection
			check(select::next_selection(current, false, hits, true, true, true) == current, "foreign not added " + tag);
			// shift + click on the ground keeps the selection
			check(select::next_selection(current, false, ids{}, false, true, true) == current, "shift ground keeps " + tag);
			// a foreign selection is replaced by own entities added with shift
			check(select::next_selection(ids{99}, true, hits, false, true, true) == hits, "foreign replaced " + tag);

			// shift + rectangle adds without duplicates and never removes
			ids rect{hit, hit + 1};
			auto merged = select::next_selection(current, false, rect, false, true, false);
			std::set<int> unique(merged.begin(), merged.end());
			check(unique.size() == merged.size(), "rectangle no duplicates " + tag);
			for (int id : current) {
				check(unique.contains(id), "rectangle keeps " + tag);
			}
			check(unique.contains(hit) and unique.contains(hit + 1), "rectangle adds " + tag);
			// rectangle without shift replaces
			check(select::next_selection(current, false, rect, false, false, false) == rect, "rectangle replaces " + tag);
		}
	}
}

void check_on_screen() {
	for (int ix = -15; ix <= 15; ++ix) {
		for (int iy = -15; iy <= 15; ++iy) {
			float x = ix * 0.1f;
			float y = iy * 0.1f;
			bool expected = std::abs(ix) <= 10 and std::abs(iy) <= 10;
			check(select::on_screen(x, y) == expected, "on_screen " + std::to_string(ix) + "," + std::to_string(iy));
		}
	}
}

void check_frame_stats() {
	using openage::presenter::FrameStats;
	for (int fps : {10, 30, 60, 72, 90, 144}) {
		FrameStats stats{5.0};
		double frame = 1.0 / fps;
		double elapsed = 0.0;
		size_t frames = 0;
		while (not stats.due(elapsed)) {
			// 10 % events, 30 % render, 60 % present; every 10th frame twice as long
			double scale = frames % 10 == 9 ? 2.0 : 1.0;
			stats.add(0.1 * frame * scale, 0.3 * frame * scale, 0.6 * frame * scale);
			elapsed += frame * scale;
			frames += 1;
		}
		std::string tag = std::to_string(fps) + " fps";
		check(stats.count() == frames, "frame count " + tag);
		check(near(stats.fps(elapsed), frames / elapsed), "fps " + tag);
		check(near(stats.avg_frame_ms(), 1000.0 * elapsed / frames, 1e-6), "average " + tag);
		auto line = stats.report(elapsed);
		check(line.find("fps over") != std::string::npos and line.find("present") != std::string::npos, "report " + tag);
		char max_ms[32];
		std::snprintf(max_ms, sizeof(max_ms), "max %.1f ms", 2000.0 * frame);
		check(line.find(max_ms) != std::string::npos, "maximum " + tag + ": " + line);
		stats.reset();
		check(stats.count() == 0 and not stats.due(10.0), "reset " + tag);
	}
	FrameStats empty{5.0};
	check(not empty.due(100.0) and empty.fps(0.0) == 0.0 and empty.avg_frame_ms() == 0.0, "no frames");
}

} // namespace

int main() {
	check_click();
	check_next_selection();
	check_on_screen();
	check_frame_stats();
	std::cout << "select check: " << checks << " checks, " << failures << " failures" << std::endl;
	return failures == 0 ? 0 : 1;
}
