// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "aoe_ui_controller.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "gamestate/catalog.h"
#include "gamestate/production.h"
#include "input/keys.h"
#include "log/log.h"
#include "time/clock.h"


namespace openage::ui {

namespace {

/// the civilisation choice of the interface (agesxr/xr_civs.h) follows the catalog of the engine
constexpr bool civ_choices_match() {
	if (agesxr::kCivChoiceCount != static_cast<int>(gamestate::prod::CIVS.size()) + 1) {
		return false;
	}
	for (size_t i = 0; i < gamestate::prod::CIVS.size(); ++i) {
		const char *a = agesxr::kCivChoiceNames[i + 1];
		const char *b = gamestate::prod::CIVS[i].name;
		while (*a != 0 and *a == *b) {
			++a;
			++b;
		}
		if (*a != *b) {
			return false;
		}
	}
	return true;
}
static_assert(civ_choices_match(), "xr_civs.h and gamestate/catalog.h list the civilisations differently");

/// ids of the context menu items (same as the classic interface)
enum context_item_t : int {
	CTX_MOVE = 1,
	CTX_GATHER = 2,
	CTX_ATTACK = 3,
	CTX_BUILD = 4,
	CTX_RALLY = 5,
	CTX_CANCEL = 9,
};

agesxr::HudProdState prod_to_hud(const gamestate::prod::Snapshot &s) {
	using gamestate::prod::status_t;
	agesxr::HudProdState p;
	p.valid = true;
	for (size_t i = 0; i < p.resources.size() and i < s.resources.size(); ++i) {
		p.resources[i] = s.resources[i];
	}
	p.population = static_cast<int>(s.population);
	p.populationCap = static_cast<int>(s.population_cap);
	p.selectionCount = static_cast<int>(s.selection_count);
	p.selectionLabel = s.selection_label;
	p.construction = s.construction ? *s.construction : -1.0;
	p.selectionResource = s.selection_resource;
	for (const auto &o : s.options) {
		agesxr::HudProdOption h;
		h.code = o.code;
		h.label = o.label;
		h.icon = o.icon;
		h.available = o.available;
		h.reason = o.reason;
		// XR fork W1: research, costs, time and effect for the tooltip
		h.research = o.research;
		for (size_t i = 0; i < h.cost.size() and i < o.cost.size(); ++i) {
			h.cost[i] = o.cost[i];
		}
		h.time = o.time;
		h.effect = o.effect;
		p.options.push_back(h);
	}
	p.age = s.age;
	p.ageLabel = s.age_label;
	p.civLabel = s.civ_label;
	p.unitLabels = s.unit_labels;
	if (s.queue) {
		p.queue = true;
		for (const auto &item : s.queue->items) {
			p.queueItems.push_back(item.label.empty() ? item.id : item.label);
			p.queueIcons.push_back(item.icon);
		}
		p.queueProgress = s.queue->progress;
		p.waitingForHousing = s.queue->waiting_for_housing;
	}
	// all orders of the player (same conversion as the classic interface)
	for (const auto &o : s.orders) {
		agesxr::HudProdOrder h;
		h.entity = o.building;
		h.construction = o.construction;
		h.label = o.construction ? o.building_label : o.label;
		h.building = o.construction ? std::string{} : o.building_label;
		h.icon = o.icon;
		h.count = static_cast<int>(o.count);
		h.remaining = o.remaining;
		h.progress = o.progress;
		p.orders.push_back(h);
	}
	p.placement = s.placement;
	p.status = s.status;
	p.statusKind = s.status_kind == status_t::warn ? agesxr::HudStatus::kWarn
	               : s.status_kind == status_t::good ? agesxr::HudStatus::kGood
	                                                 : agesxr::HudStatus::kInfo;
	p.statusSeq = s.status_seq;
	return p;
}

int size_index(size_t tiles) {
	int best = 1;
	size_t best_diff = static_cast<size_t>(-1);
	for (int i = 0; i < agesxr::kGameUiSizeCount; ++i) {
		auto t = static_cast<size_t>(agesxr::kGameUiSizeTiles[i]);
		size_t diff = t > tiles ? t - tiles : tiles - t;
		if (diff < best_diff) {
			best_diff = diff;
			best = i;
		}
	}
	return best;
}

} // namespace


AoeUiController::AoeUiController(const gamestate::MapSettings &map,
                                 UiHooks hooks,
                                 AoeHooks aoe,
                                 const std::string &demo,
                                 bool quest) :
	map{map},
	hooks{std::move(hooks)},
	aoe{std::move(aoe)} {
	auto &menu = this->aoe_ui.menu();
	menu.biome = std::clamp(static_cast<int>(map.biome), 0, agesxr::kGameUiBiomeCount - 1);
	menu.size = size_index(map.size);
	menu.seed = std::clamp(static_cast<int>(map.seed), agesxr::kGameUiSeedMin, agesxr::kGameUiSeedMax);
	const bool ai_on = map.type == gamestate::map_type_t::RANDOM and map.ai.mode != gamestate::ai_mode_t::OFF;
	menu.opponent = not ai_on ? 0 : map.ai.difficulty == gamestate::ai_difficulty_t::NORMAL ? 2 : 1;
	// XR fork W1: civilisations of the last game (random until chosen)
	menu.civ = map.civs.size() > 0 ? agesxr::civChoiceIndex(map.civs[0]) : agesxr::kCivRandom;
	menu.aiCiv = map.civs.size() > 1 ? agesxr::civChoiceIndex(map.civs[1]) : agesxr::kCivRandom;
	menu.vision = map.type == gamestate::map_type_t::RANDOM
	                  ? std::clamp(static_cast<int>(map.vision), 0, agesxr::kAoeVisionCount - 1)
	                  : 0;
	menu.speed = 1;
	if (this->hooks.clock) {
		double speed = this->hooks.clock->get_speed().to_double();
		for (int i = 0; i < agesxr::kGameSpeedCount; ++i) {
			if (std::fabs(agesxr::kGameSpeedValues[i] - speed) < 0.01) {
				menu.speed = i;
			}
		}
	}
	menu.quest = quest;
	menu.saveAvailable = static_cast<bool>(this->hooks.save_slot) and static_cast<bool>(this->hooks.list_slots);
	menu.loadAvailable = static_cast<bool>(this->hooks.load_slot) and static_cast<bool>(this->hooks.list_slots);
	// on the Quest the symbols are generic without the game's skin: labels on (spec §1.2), hotkeys off
	menu.labels = true;
	menu.hotkeys = not quest;
	menu.mapInfo = this->map_info();

	std::istringstream in{demo};
	std::string item;
	while (std::getline(in, item, ',')) {
		auto at = item.find('@');
		if (at == std::string::npos) {
			continue;
		}
		try {
			this->demo.push_back({item.substr(0, at), std::stod(item.substr(at + 1)), false});
		}
		catch (std::exception &) {
			log::log(WARN << "UI: ignoring demo step '" << item << "'");
		}
	}
}

bool AoeUiController::init() {
	bool ok = this->aoe_ui.init();
	this->aoe_ui.resize(static_cast<int>(this->width), static_cast<int>(this->height));
	if (not ok) {
		log::log(WARN << "UI: no system font found (XRAGES_FONT), texts are not drawn");
	}
	log::log(INFO << "UI: AoE layout in the engine image (Esc/F10/Menü = game menu, F8 = focus, Q…B = commands, "
	              << "right button held = context menu)");
	return ok;
}

void AoeUiController::resize(size_t w, size_t h) {
	this->width = std::max<size_t>(w, 16);
	this->height = std::max<size_t>(h, 16);
	this->aoe_ui.resize(static_cast<int>(this->width), static_cast<int>(this->height));
	log::log(INFO << "UI: AoE layout " << this->width << "x" << this->height << ", bottom bar "
	              << this->aoe_ui.bottomBarPx() << " px, scale " << this->aoe_ui.scale());
}

// ---- data ---------------------------------------------------------------------------------

void AoeUiController::poll(double now) {
	agesxr::HudSample sample;
	sample.valid = true;
	if (this->hooks.clock) {
		sample.gameSeconds = this->hooks.clock->get_time().to_double();
	}
	if (this->hooks.query_hud) {
		sample.info = this->hooks.query_hud();
	}
	this->info = sample.info;
	if (this->hooks.production and sample.info.game) {
		sample.prod = prod_to_hud(this->hooks.production->snapshot());
	}
	// autosave (game time; not while paused, not twice per interval)
	if (sample.info.game and this->hooks.save_slot and not this->match_over
	    and this->autosave.due(sample.gameSeconds)) {
		this->save_slot(gamestate::save::AUTOSAVE_SLOT, "autosave");
	}
	// XR fork W4: the delete confirmation runs out after 3 s
	if (this->delete_armed_at >= 0.0 and now - this->delete_armed_at > 3.0) {
		this->delete_armed_at = -1.0;
		this->feed.setDeleteArmed(false);
	}
	this->feed.fill(sample, now);

	auto &model = this->aoe_ui.model();
	model = this->feed.model();
	model.paused = this->paused_sent;
	model.minimap.biome = std::clamp(static_cast<int>(this->map.biome), 0, agesxr::kGameUiBiomeCount - 1);
	model.minimap.tiles = static_cast<int>(this->map.size);
	float cx = 0.5f, cy = 0.5f, cw = 0.25f, ch = 0.15f;
	if (this->aoe.camera_view and this->aoe.camera_view(cx, cy, cw, ch)) {
		model.minimap.camX = cx;
		model.minimap.camY = cy;
		model.minimap.camW = cw;
		model.minimap.camH = ch;
	}
	// real minimap (XR fork, vision): own image under the bar, the diamond of the bar stays transparent
	model.minimap.live = this->minimap_live;
	this->update_minimap(now);

	const auto match = this->feed.match();
	if (match != agesxr::HudMatch::RUNNING and not this->match_over) {
		this->match_over = true;
		this->show_board(match == agesxr::HudMatch::VICTORY,
		                 agesxr::HudFeed::matchHeadline(match),
		                 {match == agesxr::HudMatch::VICTORY  ? "Alle gegnerischen Einheiten und Gebäude sind besiegt."
		                  : match == agesxr::HudMatch::DEFEAT ? "Keine eigenen Einheiten oder Gebäude mehr."
		                                                      : "Niemand ist übrig geblieben.",
		                  "Entschieden nach " + agesxr::hudTimeText(sample.info.decided_at) + " Spielzeit – das Spiel ist angehalten."});
	}
	this->apply_pause();
}

void AoeUiController::show_board(bool good, const std::string &headline, const std::vector<std::string> &lines) {
	auto &board = this->aoe_ui.board();
	board.open = true;
	board.good = good;
	board.headline = headline;
	board.lines = lines;
	this->match_over = true;
	log::log(INFO << "UI: match board '" << headline << "' shown, game paused");
	this->apply_pause();
}

void AoeUiController::apply_pause() {
	if (not this->hooks.clock) {
		return;
	}
	const bool wanted = this->menu_pause or this->match_over;
	if (wanted == this->paused_sent) {
		return;
	}
	this->paused_sent = wanted;
	if (wanted) {
		this->hooks.clock->pause();
	}
	else {
		this->hooks.clock->resume();
	}
	this->aoe_ui.model().paused = wanted;
	log::log(INFO << "UI: game clock " << (wanted ? "paused" : "resumed") << " (user 0, menu " << this->menu_pause
	              << ", match over " << this->match_over << ")");
}

void AoeUiController::update(double now) {
	this->last_now = now;
	if (this->start_time < 0.0) {
		this->start_time = now;
	}
	if (this->last_poll < 0.0 or now - this->last_poll >= POLL_SECONDS) {
		this->last_poll = now;
		this->poll(now);
	}
	if (this->right_held and not this->right_context and now - this->right_down_at >= HOLD_SECONDS) {
		this->right_context = this->open_context_menu(this->right_x, this->right_y);
		if (not this->right_context) {
			this->right_held = false;
		}
	}
	// signal ring on the minimap: redraw it more often while it runs
	if (this->ping_at >= 0.0 and now - this->ping_at < 3.2 and now - this->minimap_at >= 0.1) {
		this->minimap_force = true;
		this->update_minimap(now);
	}
	this->run_demo(now);
}

void AoeUiController::run_demo(double now) {
	for (auto &step : this->demo) {
		if (step.done or now - this->start_time < step.at) {
			continue;
		}
		step.done = true;
		if (step.what == "board") {
			this->show_board(true, "Sieg!",
			                 {"Alle gegnerischen Einheiten und Gebäude sind besiegt.",
			                  "Entschieden nach 12:34 Spielzeit – das Spiel ist angehalten."});
		}
		else if (step.what == "menu") {
			this->open_menu(true, now);
		}
		else if (step.what == "order0") {
			// first entry of the orders (as a click on it; same steps as the classic interface, 89-save-check.sh)
			const auto &orders = this->aoe_ui.model().orders;
			log::log(INFO << "UI: demo click on order 0 (" << orders.size() << " orders)");
			if (not orders.empty() and this->hooks.focus_entity) {
				this->hooks.focus_entity(orders.front().entity);
			}
		}
		else if (step.what == "save-menu" or step.what == "load-menu") {
			this->open_menu(true, now);
			auto &menu = this->aoe_ui.menu();
			menu.page = step.what == "save-menu" ? agesxr::AoeMenuModel::kSave : agesxr::AoeMenuModel::kLoad;
			menu.confirmSlot = -1;
			this->refresh_slots();
			this->aoe_ui.invalidate();
			log::log(INFO << "UI: demo slot list '" << step.what << "' with " << menu.slot_list.size() << " slots");
		}
		else if (step.what == "close-menu") {
			this->open_menu(false, now);
		}
		else if (step.what == "quicksave") {
			this->save_slot(gamestate::save::QUICK_SLOT, "demo");
		}
		else if (step.what == "restart") {
			auto settings = this->menu_map_settings();
			settings.seed += 1;
			log::log(INFO << "UI: demo restart with map seed " << settings.seed);
			if (this->hooks.restart) {
				this->hooks.restart(settings);
			}
		}
		else {
			log::log(WARN << "UI: unknown demo step '" << step.what << "'");
		}
	}
}

// ---- input --------------------------------------------------------------------------------

bool AoeUiController::on_event(const renderer::WindowEvent &ev, double now) {
	using namespace input;
	auto &ui = this->aoe_ui;
	switch (ev.type) {
	case event_type::MouseMove: {
		const int x = static_cast<int>(ev.x), y = static_cast<int>(ev.y);
		ui.onMove(x, y);
		// dragging on the minimap: the camera follows (at most 10 times a second)
		if (this->minimap_drag) {
			float du = 0.0f, dv = 0.0f;
			ui.minimapFrac(x, y, du, dv);
			if (now - this->minimap_drag_at >= 0.1 and this->aoe.minimap_jump) {
				this->minimap_drag_at = now;
				this->aoe.minimap_jump(du, dv);
			}
			return true;
		}
		if (ui.wantsMouse()) {
			return true;
		}
		// hovering the bars belongs to the interface (no edge scrolling), a drag passes
		return ui.overBars(x, y) and not this->left_held and not this->right_held;
	}
	case event_type::MouseButtonPress:
	case event_type::MouseButtonDblClick: {
		const int x = static_cast<int>(ev.x);
		const int y = static_cast<int>(ev.y);
		if (ev.button == mouse_button::LeftButton) {
			// Alt + click on the minimap: signal (ping) there (XR fork, vision)
			float pu = 0.0f, pv = 0.0f;
			if ((ev.modifiers & modifier::AltModifier) and not ui.wantsMouse() and this->aoe_ui.model().minimap.live
			    and ui.minimapFrac(x, y, pu, pv)) {
				this->ping_u = pu;
				this->ping_v = pv;
				this->ping_at = now;
				this->minimap_force = true;
				log::log(INFO << "UI: minimap signal at (" << pu << ", " << pv << ")");
				this->ui_click = true;
				return true;
			}
			if (ui.wantsMouse() or ui.overBars(x, y)) {
				this->handle(ui.onClick(x, y, now), now);
				this->ui_click = true;
				return true;
			}
			// XR fork W4: the target of patrol, guard, follow, attack move, garrison
			if (this->pending_target) {
				const auto type = *this->pending_target;
				this->pending_target.reset();
				log::log(INFO << "UI: " << this->pending_label << " target at pixel (" << x << ", " << y << ")");
				if (this->hooks.send_order) {
					this->hooks.send_order(type, x, y, 0);
				}
				this->last_poll = -1.0;
				this->ui_click = true;
				return true;
			}
			this->left_held = true;
			return false;
		}
		if (ev.button == mouse_button::RightButton and this->pending_target) {
			// right click ends the target choice
			log::log(INFO << "UI: " << this->pending_label << " cancelled");
			this->pending_target.reset();
			this->right_held = false;
			return true;
		}
		if (ev.button == mouse_button::RightButton) {
			// right click on the minimap: the selection moves there (XR fork, vision)
			float cu = 0.0f, cv = 0.0f;
			if (not ui.wantsMouse() and this->aoe_ui.model().minimap.live and ui.minimapFrac(x, y, cu, cv)) {
				log::log(INFO << "UI: minimap command at (" << cu << ", " << cv << ")");
				if (this->aoe.minimap_command) {
					this->aoe.minimap_command(cu, cv);
				}
				this->right_held = false;
				return true;
			}
			if (ui.context().open) {
				ui.closeContext();
				return true;
			}
			if (ui.wantsMouse() or ui.overBars(x, y)) {
				return true;
			}
			if (ev.modifiers & modifier::AltModifier) {
				this->right_context = this->open_context_menu(x, y);
				this->right_held = this->right_context;
				return this->right_context;
			}
			this->right_held = true;
			this->right_context = false;
			this->right_down_at = now;
			this->right_x = x;
			this->right_y = y;
			return true;
		}
		if (ev.button == mouse_button::MiddleButton) {
			if (ui.wantsMouse() or ui.overBars(x, y)) {
				return true;
			}
			return this->open_context_menu(x, y);
		}
		return ui.wantsMouse();
	}
	case event_type::MouseButtonRelease: {
		if (ev.button == mouse_button::LeftButton) {
			this->minimap_drag = false;
			this->left_held = false;
			if (this->ui_click) {
				this->ui_click = false;
				return true;
			}
			return ui.wantsMouse();
		}
		if (ev.button == mouse_button::RightButton) {
			if (this->right_held) {
				this->right_held = false;
				if (this->right_context) {
					this->right_context = false;
					return true;
				}
				return false;  // short press: the usual right click
			}
			return true;
		}
		if (ev.button == mouse_button::MiddleButton) {
			return true;
		}
		return ui.wantsMouse();
	}
	case event_type::Wheel:
		return ui.wantsMouse() or ui.overBars(static_cast<int>(ev.x), static_cast<int>(ev.y));
	case event_type::KeyPress: {
		const int k = ev.key;
		const bool modal = ui.menu().open or ui.board().open or ui.context().open;
		if (ev.auto_repeat) {
			// held arrows repeat the focus step (menus), everything else is swallowed while modal
			if (ui.focusMode() and (k == key::Key_Left or k == key::Key_Right or k == key::Key_Up or k == key::Key_Down)) {
				this->handle(ui.navigate(k == key::Key_Right ? 1 : k == key::Key_Left ? -1 : 0,
				                         k == key::Key_Down ? 1 : k == key::Key_Up ? -1 : 0, now),
				             now);
				return true;
			}
			return modal;
		}
		bool consumed = false;
		// minimap modes (docs/aoe2-spec 08 §8.4): Alt+N normal, Alt+C combat, Alt+R economy
		if ((ev.modifiers & modifier::AltModifier) and not modal and (k == 'N' or k == 'C' or k == 'R')) {
			this->minimap_mode = k == 'C' ? agesxr::kMinimapMilitary : k == 'R' ? agesxr::kMinimapEconomy : agesxr::kMinimapNormal;
			this->minimap_force = true;
			this->minimap_base.clear();
			log::log(INFO << "UI: minimap mode " << (k == 'C' ? "combat" : k == 'R' ? "economy" : "normal"));
			this->swallowed_keys.push_back(k);
			return true;
		}
		if ((k == key::Key_F5 or k == key::Key_F9) and not modal) {
			// quick save / quick load (0.6.0-xr.0.11)
			if (k == key::Key_F5) {
				this->save_slot(gamestate::save::QUICK_SLOT, "F5");
			}
			else {
				this->load_slot(gamestate::save::QUICK_SLOT, "F9");
			}
			this->swallowed_keys.push_back(k);
			return true;
		}
		// XR fork W4: Entf = delete, Tab = attack move (keys of the command grid)
		if ((k == key::Key_Delete or k == key::Key_Tab) and not modal) {
			if (now - this->last_poll > 0.05) {
				this->last_poll = now;
				this->poll(now);
			}
			const auto result = ui.onKey(k == key::Key_Delete ? agesxr::kAoeKeyDelete : agesxr::kAoeKeyTab, now);
			if (result.action != agesxr::AoeUi::Action::kNone) {
				// the keyboard asks no question (the Quest button does)
				if (k == key::Key_Delete) {
					this->delete_armed_at = now;
				}
				this->handle(result, now);
				this->swallowed_keys.push_back(k);
				return true;
			}
			return false;
		}
		if (k == key::Key_Escape and this->pending_target and not modal) {
			log::log(INFO << "UI: " << this->pending_label << " cancelled (Esc)");
			this->pending_target.reset();
			this->swallowed_keys.push_back(k);
			return true;
		}
		if (k == key::Key_Escape or k == key::Key_F10) {
			if (ui.context().open) {
				ui.closeContext();
			}
			else if (ui.menu().open) {
				if (ui.menu().page != agesxr::AoeMenuModel::kMain and k == key::Key_Escape) {
					ui.menu().page = agesxr::AoeMenuModel::kMain;  // Esc = one page back
				}
				else {
					this->open_menu(false, now);
				}
			}
			else if (ui.focusMode() and k == key::Key_Escape) {
				ui.focusGrid(false);  // B: focus off
			}
			else if (k == key::Key_Escape and this->hooks.production and this->hooks.production->placement_active()
			         and not ui.board().open) {
				return false;  // Esc ends the placement mode first (game binding)
			}
			else if (ui.board().open) {
				return true;
			}
			else {
				this->open_menu(true, now);
			}
			consumed = true;
		}
		else if (k == KEY_FOCUS) {
			if (not modal) {
				ui.focusGrid(not ui.focusMode());
				log::log(INFO << "UI: focus mode " << (ui.focusMode() ? "on" : "off"));
			}
			consumed = true;
		}
		else if (k == key::Key_Left or k == key::Key_Right or k == key::Key_Up or k == key::Key_Down) {
			if (ui.focusMode() or modal) {
				this->handle(ui.navigate(k == key::Key_Right ? 1 : k == key::Key_Left ? -1 : 0,
				                         k == key::Key_Down ? 1 : k == key::Key_Up ? -1 : 0, now),
				             now);
				consumed = true;
			}
		}
		else if (k == key::Key_Return or k == key::Key_Enter) {
			if (ui.focusMode()) {
				this->handle(ui.activate(now), now);
				consumed = true;
			}
		}
		else if (k >= 'A' and k <= 'Z' and not modal and not (ev.modifiers & modifier::ControlModifier)) {
			// XR fork W4: the grid follows the current selection (a key right after a selection)
			if (now - this->last_poll > 0.05) {
				this->last_poll = now;
				this->poll(now);
			}
			const auto result = ui.onKey(k, now);
			if (result.action != agesxr::AoeUi::Action::kNone) {
				this->handle(result, now);
				consumed = true;
			}
		}
		if (consumed) {
			this->swallowed_keys.push_back(k);
			return true;
		}
		return modal;
	}
	case event_type::KeyRelease: {
		auto it = std::find(this->swallowed_keys.begin(), this->swallowed_keys.end(), ev.key);
		if (it != this->swallowed_keys.end()) {
			this->swallowed_keys.erase(it);
			return true;
		}
		return ui.menu().open;
	}
	default:
		return false;
	}
}

void AoeUiController::open_menu(bool open, double now) {
	auto &ui = this->aoe_ui;
	if (ui.menu().open == open) {
		return;
	}
	ui.menu().mapInfo = this->map_info();
	ui.openMenu(open, now);
	if (open) {
		this->refresh_slots();
	}
	this->menu_pause = open;
	log::log(INFO << "UI: game menu " << (open ? "opened" : "closed"));
	this->apply_pause();
}

bool AoeUiController::open_context_menu(int x, int y) {
	if (not this->info.game or not this->info.first) {
		log::log(INFO << "UI: no context menu at pixel (" << x << ", " << y << "): nothing selected");
		return false;
	}
	const auto &first = *this->info.first;
	if (first.neutral_object) {
		log::log(INFO << "UI: no context menu: selection is not controllable");
		return false;
	}
	std::vector<agesxr::GameUiItem> items;
	if (first.building and not first.unit) {
		items.push_back({CTX_RALLY, "Sammelpunkt", true});
	}
	else {
		items.push_back({CTX_MOVE, "Hierher bewegen", true});
		if (first.villager) {
			items.push_back({CTX_GATHER, "Sammeln", true});
			items.push_back({CTX_BUILD, "Weiterbauen", true});
		}
		items.push_back({CTX_ATTACK, "Angreifen", true});
	}
	items.push_back({CTX_CANCEL, "Abbrechen", true});
	// "3 Dorfbewohner" (spec §1.3)
	const auto &sel = this->aoe_ui.model().selection;
	std::string title = sel.count > 1 ? sel.name : sel.name.empty() ? "Auswahl" : sel.name;
	this->context_x = x;
	this->context_y = y;
	this->aoe_ui.openContext(x, y, title, std::move(items));
	log::log(INFO << "UI: context menu at pixel (" << x << ", " << y << ") for '" << title << "'");
	return true;
}

void AoeUiController::context_command(int id) {
	ui_command_t type;
	const char *what;
	switch (id) {
	case CTX_MOVE:
	case CTX_RALLY:
		type = ui_command_t::MOVE_PLAIN;
		what = id == CTX_MOVE ? "move" : "rally point";
		break;
	case CTX_GATHER:
		type = ui_command_t::GATHER;
		what = "gather";
		break;
	case CTX_ATTACK:
		type = ui_command_t::ATTACK;
		what = "attack";
		break;
	case CTX_BUILD:
		type = ui_command_t::MOVE;
		what = "build";
		break;
	default:
		return;
	}
	log::log(INFO << "UI: context menu '" << what << "' at pixel (" << this->context_x << ", " << this->context_y << ")");
	if (this->hooks.send_command) {
		this->hooks.send_command(type, this->context_x, this->context_y);
	}
}

void AoeUiController::production_command(int code, const std::string &label) {
	if (code == agesxr::kAoeCmdPageNext) {
		// XR fork W1: more than 10 commands (buildings and technologies) are shown in pages
		this->feed.nextPage();
		log::log(INFO << "UI: command page " << (this->feed.page() + 1) << "/" << this->feed.pages());
		this->last_poll = -1.0;
		return;
	}
	if (this->combat_command(code, label)) {
		return;
	}
	if (not this->hooks.production) {
		return;
	}
	log::log(INFO << "UI: HUD button '" << label << "' (" << code << ")");
	if (code == agesxr::kAoeCmdCancelTraining) {
		this->hooks.production->cancel_training();
	}
	else if (code == agesxr::kAoeCmdCancelPlacement) {
		this->hooks.production->cancel_placement();
	}
	else if (code > 0 and code < agesxr::kAoeCmdCancelTraining) {
		this->hooks.production->command(code);
	}
	this->last_poll = -1.0;  // show the result at once
}

bool AoeUiController::combat_command(int code, const std::string &label) {
	using agesxr::kAoeCmdFormation0;
	using agesxr::kAoeCmdStance0;
	const double now = this->last_now;
	auto order = [&](ui_command_t type, int arg) {
		if (this->hooks.send_order) {
			this->hooks.send_order(type, 0, 0, arg);
		}
		this->last_poll = -1.0;
	};
	auto target = [&](ui_command_t type, const char *what) {
		this->pending_target = type;
		this->pending_label = what;
		const std::string hint = std::string{what} + ": Ziel auf der Karte wählen (Rechtsklick bricht ab)";
		agesxr::aoePushMessage(this->aoe_ui.model(), hint, 0, now);
		agesxr::aoePushMessage(this->feed.model(), hint, 0, now);
		log::log(INFO << "UI: " << what << " – waiting for a target");
	};
	if (code == agesxr::kAoeCmdPatrol) {
		target(ui_command_t::PATROL, "Patrouille");
	}
	else if (code == agesxr::kAoeCmdGuard) {
		target(ui_command_t::GUARD, "Wachen");
	}
	else if (code == agesxr::kAoeCmdFollow) {
		target(ui_command_t::FOLLOW, "Folgen");
	}
	else if (code == agesxr::kAoeCmdAttackMove) {
		target(ui_command_t::ATTACK_MOVE, "Angriffsbewegung");
	}
	else if (code == agesxr::kAoeCmdGarrison) {
		target(ui_command_t::GARRISON, "Garnison");
	}
	// XR fork W6: monks, carriers, trebuchets
	else if (code == agesxr::kAoeCmdConvert) {
		target(ui_command_t::CONVERT, "Konvertieren");
	}
	else if (code == agesxr::kAoeCmdHeal) {
		target(ui_command_t::HEAL, "Heilen");
	}
	else if (code == agesxr::kAoeCmdUnloadAt) {
		target(ui_command_t::UNLOAD, "Entladen");
	}
	else if (code == agesxr::kAoeCmdUnloadHere) {
		log::log(INFO << "UI: unload here");
		order(ui_command_t::UNLOAD_HERE, 1);
	}
	else if (code == agesxr::kAoeCmdPack or code == agesxr::kAoeCmdUnpack) {
		log::log(INFO << "UI: trebuchet " << (code == agesxr::kAoeCmdPack ? "pack" : "unpack"));
		order(code == agesxr::kAoeCmdPack ? ui_command_t::PACK : ui_command_t::UNPACK, 0);
	}
	else if (code == agesxr::kAoeCmdTributePage) {
		this->feed.toggleTribute();
		this->last_poll = -1.0;
	}
	else if (code == agesxr::kAoeCmdTributeAmount) {
		this->feed.nextTributeAmount();
		this->last_poll = -1.0;
	}
	else if (code == agesxr::kAoeCmdTributeTo) {
		this->feed.nextTributeReceiver();
		this->last_poll = -1.0;
	}
	else if (code >= agesxr::kAoeCmdTribute0 and code < agesxr::kAoeCmdTribute0 + 4) {
		const int resource = code - agesxr::kAoeCmdTribute0;
		log::log(INFO << "UI: tribute " << resource << " amount " << this->feed.tributeAmount() << " to player "
		              << this->feed.tributeReceiver());
		order(ui_command_t::MARKET, 100 + resource + 10 * this->feed.tributeAmount() + 100 * this->feed.tributeReceiver());
	}
	else if (code >= agesxr::kAoeCmdMarket0 and code < agesxr::kAoeCmdMarket0 + 6) {
		log::log(INFO << "UI: market " << (code - agesxr::kAoeCmdMarket0));
		order(ui_command_t::MARKET, code - agesxr::kAoeCmdMarket0);
	}
	else if (code == agesxr::kAoeCmdDropRelic) {
		log::log(INFO << "UI: drop relic");
		order(ui_command_t::DROP_RELIC, 0);
	}
	else if (code == agesxr::kAoeCmdHalt) {
		log::log(INFO << "UI: halt");
		order(ui_command_t::STOP, 0);
	}
	else if (code >= kAoeCmdStance0 and code < kAoeCmdStance0 + agesxr::kAoeStanceCount) {
		log::log(INFO << "UI: stance '" << label << "'");
		order(ui_command_t::STANCE, code - kAoeCmdStance0);
	}
	else if (code >= kAoeCmdFormation0 and code < kAoeCmdFormation0 + agesxr::kAoeFormationCount) {
		log::log(INFO << "UI: formation '" << label << "'");
		order(ui_command_t::FORMATION, code - kAoeCmdFormation0 + 1);
	}
	else if (code == agesxr::kAoeCmdUnload) {
		log::log(INFO << "UI: ungarrison all");
		order(ui_command_t::UNGARRISON, 0);
	}
	else if (code == agesxr::kAoeCmdBell) {
		const bool ringing = this->info.first and this->info.first->bell;
		log::log(INFO << "UI: town bell " << (ringing ? "off (back to work)" : "on"));
		order(ui_command_t::BELL, ringing ? 0 : 1);
	}
	// XR fork W5 (docs/aoe2-spec 04 §4.4/4.5, 08 §8.1/8.2)
	else if (code == agesxr::kAoeCmdRepair) {
		target(ui_command_t::REPAIR, "Reparieren");
	}
	else if (code == agesxr::kAoeCmdGateLock) {
		const bool locked = this->info.first and this->info.first->gate_locked;
		log::log(INFO << "UI: gate " << (locked ? "unlock" : "lock"));
		order(ui_command_t::GATE_LOCK, locked ? 0 : 1);
	}
	else if (code == agesxr::kAoeCmdDelete) {
		// on the Quest a second press within 3 s confirms (no keyboard, the trigger is quick)
		if (this->aoe_ui.menu().quest and this->delete_armed_at < 0.0) {
			this->delete_armed_at = now;
			this->feed.setDeleteArmed(true);
			agesxr::aoePushMessage(this->feed.model(), "Löschen: noch einmal drücken zum Bestätigen", 1, now);
			log::log(INFO << "UI: delete armed (3 s)");
			this->last_poll = -1.0;
			return true;
		}
		this->delete_armed_at = -1.0;
		this->feed.setDeleteArmed(false);
		log::log(INFO << "UI: delete the selection");
		order(ui_command_t::KILL, 0);
	}
	else {
		return false;
	}
	return true;
}

void AoeUiController::handle(const agesxr::AoeUi::Result &result, double now) {
	using Action = agesxr::AoeUi::Action;
	auto &ui = this->aoe_ui;
	auto &menu = ui.menu();
	switch (result.action) {
	case Action::kCommand:
		this->production_command(result.id, ui.model().cell(result.index).label);
		break;
	case Action::kBlocked: {
		// like the original: the click reports why (the engine answers with its own status message)
		const auto &cell = ui.model().cell(result.index);
		log::log(INFO << "UI: blocked button '" << cell.label << "' (" << result.id << "): " << cell.reason);
		if (result.id > 0 and result.id < agesxr::kAoeCmdCancelTraining and this->hooks.production) {
			this->hooks.production->command(result.id);
			this->last_poll = -1.0;
		}
		else {
			agesxr::aoePushMessage(ui.model(), cell.label + ": " + cell.reason, 1, now);
			agesxr::aoePushMessage(this->feed.model(), cell.label + ": " + cell.reason, 1, now);
		}
		break;
	}
	case Action::kQueueCancel:
		// exactly this entry (costs back), 0.6.0-xr.0.11
		log::log(INFO << "UI: queue slot " << result.index << " clicked: cancel this entry");
		if (this->hooks.production and result.index >= 0) {
			this->hooks.production->cancel_training_at(static_cast<size_t>(result.index));
			this->last_poll = -1.0;
		}
		break;
	case Action::kFocusOrder: {
		const auto &orders = ui.model().orders;
		if (result.index >= 0 and static_cast<size_t>(result.index) < orders.size()) {
			const auto &o = orders[static_cast<size_t>(result.index)];
			log::log(INFO << "UI: order '" << o.label << "' clicked, camera to entity " << o.entity);
			if (this->hooks.focus_entity) {
				this->hooks.focus_entity(o.entity);
			}
			this->last_poll = -1.0;
		}
		break;
	}
	case Action::kSelectUnit:
		if (result.index >= 0 and static_cast<size_t>(result.index) < this->info.selected.size() and this->aoe.select) {
			const uint64_t id = this->info.selected[static_cast<size_t>(result.index)];
			log::log(INFO << "UI: portrait " << result.index << " clicked: select entity " << id);
			this->aoe.select({id});
			this->last_poll = -1.0;
		}
		break;
	case Action::kContextItem:
		this->context_command(result.id);
		break;
	case Action::kMenuOpened:
		ui.menu().mapInfo = this->map_info();
		this->menu_pause = true;
		log::log(INFO << "UI: game menu opened (button)");
		this->apply_pause();
		break;
	case Action::kResume:
	case Action::kMenuClosed:
		this->menu_pause = false;
		log::log(INFO << "UI: game menu closed (" << (result.action == Action::kResume ? "Weiterspielen" : "Schließen") << ")");
		this->apply_pause();
		break;
	case Action::kSpeedChanged:
		if (this->hooks.clock) {
			this->hooks.clock->set_speed(time::speed_t::from_double(agesxr::kGameSpeedValues[menu.speed]));
		}
		log::log(INFO << "UI: game speed " << agesxr::kGameSpeedValues[menu.speed]);
		break;
	case Action::kSettingChanged:
		log::log(INFO << "UI: settings labels " << menu.labels << ", hotkeys " << menu.hotkeys << ", messages "
		              << menu.messages);
		break;
	case Action::kNewMap: {
		auto settings = this->menu_map_settings();
		log::log(INFO << "UI: new map requested: " << gamestate::map_biome_name(settings.biome) << " " << settings.size
		              << "x" << settings.size << " seed " << settings.seed << " ai "
		              << (settings.ai.mode == gamestate::ai_mode_t::OFF ? "off" : "on") << " civs "
		              << (settings.civs.size() > 0 ? settings.civs[0] : std::string{"-"}) << "/"
		              << (settings.civs.size() > 1 ? settings.civs[1] : std::string{"-"}));
		this->menu_pause = false;
		if (this->hooks.restart) {
			this->hooks.restart(settings);
		}
		break;
	}
	case Action::kSurrenderArmed:
		log::log(INFO << "UI: surrender armed (3 s)");
		break;
	case Action::kSurrender:
		log::log(INFO << "UI: match surrendered");
		this->menu_pause = false;
		this->show_board(false, "Niederlage", {"Du hast die Partie aufgegeben.", "Neue Karte: Knopf unten oder Spielmenü."});
		break;
	case Action::kSlotsShown:
		this->refresh_slots();
		log::log(INFO << "UI: slot list '" << (menu.page == agesxr::AoeMenuModel::kSave ? "save" : "load") << "' with "
		              << menu.slot_list.size() << " slots");
		break;
	case Action::kSlotArmed:
		log::log(INFO << "UI: slot " << result.id << " armed (3 s)");
		break;
	case Action::kSave:
		this->menu_pause = false;
		this->save_slot(result.id, "game menu");
		this->apply_pause();
		break;
	case Action::kLoad:
		this->menu_pause = false;
		this->load_slot(result.id, "game menu");
		this->apply_pause();
		break;
	case Action::kBoardNewMap:
		this->menu_pause = true;
		log::log(INFO << "UI: match board -> new map page");
		this->apply_pause();
		break;
	case Action::kBoardClosed:
		log::log(INFO << "UI: match board closed (game stays paused)");
		break;
	case Action::kMinimapJump:
		// S3 (XR fork, vision): the camera jumps there; holding the button and dragging follows (10 Hz)
		log::log(INFO << "UI: minimap jump to (" << result.fx << ", " << result.fy << ")");
		if (this->aoe.minimap_jump) {
			this->aoe.minimap_jump(result.fx, result.fy);
		}
		this->minimap_drag = true;
		this->minimap_drag_at = now;
		break;
	case Action::kContextClosed:
	case Action::kNone:
		break;
	}
}

// ---- save games ---------------------------------------------------------------------------

void AoeUiController::refresh_slots() {
	auto &menu = this->aoe_ui.menu();
	menu.slot_list.clear();
	if (not this->hooks.list_slots) {
		return;
	}
	for (const auto &s : this->hooks.list_slots()) {
		if (static_cast<int>(menu.slot_list.size()) >= agesxr::kAoeSlotsMax) {
			break;
		}
		agesxr::AoeSaveSlot slot;
		slot.slot = s.slot;
		slot.label = s.label();
		slot.exists = s.exists;
		slot.writable = s.slot != gamestate::save::AUTOSAVE_SLOT;
		menu.slot_list.push_back(slot);
	}
}

void AoeUiController::save_slot(int slot, const char *why) {
	if (not this->hooks.save_slot or not this->hooks.save_slot(slot)) {
		log::log(WARN << "UI: saving slot " << slot << " (" << why << ") not possible");
		if (this->hooks.production) {
			this->hooks.production->notify("Speichern nicht möglich", gamestate::prod::status_t::warn);
		}
		return;
	}
	log::log(INFO << "UI: save slot " << slot << " (" << why << ")");
	this->last_poll = -1.0;
}

void AoeUiController::load_slot(int slot, const char *why) {
	if (not this->hooks.load_slot) {
		return;
	}
	log::log(INFO << "UI: load slot " << slot << " (" << why << ")");
	auto error = this->hooks.load_slot(slot);
	if (not error.empty()) {
		log::log(WARN << "UI: load slot " << slot << " failed: " << error);
	}
	this->last_poll = -1.0;
}

// ---- map ----------------------------------------------------------------------------------

std::string AoeUiController::map_info() const {
	const std::string clock = agesxr::hudTimeText(this->aoe_ui.model().gameSeconds);
	if (this->map.type == gamestate::map_type_t::TEST) {
		return "Testkarte · " + clock + " · Spiel angehalten";
	}
	const int biome = std::clamp(static_cast<int>(this->map.biome), 0, agesxr::kGameUiBiomeCount - 1);
	char buf[160];
	std::snprintf(buf, sizeof(buf), "Karte #%u · %s %zu × %zu · %s · Spiel angehalten", this->map.seed,
	              agesxr::kGameUiBiomeLabels[biome], this->map.size, this->map.size, clock.c_str());
	return buf;
}

gamestate::MapSettings AoeUiController::menu_map_settings() const {
	const auto &menu = this->aoe_ui.menu();
	gamestate::MapSettings settings = this->map;
	settings.type = gamestate::map_type_t::RANDOM;
	settings.biome = static_cast<gamestate::map_biome_t>(menu.biome);
	settings.size = static_cast<size_t>(agesxr::kGameUiSizeTiles[std::clamp(menu.size, 0, agesxr::kGameUiSizeCount - 1)]);
	settings.seed = static_cast<uint32_t>(menu.seed);
	settings.view.reset();
	settings.skirmish = false;
	settings.ai.mode = menu.opponent == 0 ? gamestate::ai_mode_t::OFF : gamestate::ai_mode_t::ON;
	settings.ai.difficulty = menu.opponent == 2 ? gamestate::ai_difficulty_t::NORMAL : gamestate::ai_difficulty_t::EASY;
	// XR fork W1: civilisation of the player and of the opponent ("random": from the map seed)
	settings.civs = {agesxr::civChoiceName(menu.civ), agesxr::civChoiceName(menu.aiCiv)};
	settings.vision = static_cast<gamestate::vision_mode_t>(std::clamp(menu.vision, 0, agesxr::kAoeVisionCount - 1));
	return settings;
}

// ---- minimap (XR fork, gamestate/vision) ----------------------------------------------------

static_assert(agesxr::kDotUnit == static_cast<int>(gamestate::vision::dot_kind_t::UNIT)
                  and agesxr::kDotBuilding == static_cast<int>(gamestate::vision::dot_kind_t::BUILDING)
                  and agesxr::kDotTree == static_cast<int>(gamestate::vision::dot_kind_t::TREE)
                  and agesxr::kDotGold == static_cast<int>(gamestate::vision::dot_kind_t::GOLD)
                  and agesxr::kDotStone == static_cast<int>(gamestate::vision::dot_kind_t::STONE)
                  and agesxr::kDotFood == static_cast<int>(gamestate::vision::dot_kind_t::FOOD)
                  and agesxr::kDotAnimal == static_cast<int>(gamestate::vision::dot_kind_t::ANIMAL)
                  and agesxr::kDotOther == static_cast<int>(gamestate::vision::dot_kind_t::OTHER),
              "minimap dot kinds of the interface and the vision differ");

void AoeUiController::update_minimap(double now) {
	auto snapshot = this->aoe.minimap ? this->aoe.minimap() : nullptr;
	const bool live = snapshot and snapshot->ne_size > 0 and snapshot->se_size > 0;
	if (live != this->minimap_live) {
		this->minimap_live = live;
		this->aoe_ui.model().minimap.live = live;
		this->aoe_ui.invalidate();
		log::log(INFO << "UI: minimap " << (live ? "live" : "placeholder"));
	}
	if (not live) {
		return;
	}
	const auto rect = this->aoe_ui.minimapImageRect();
	const auto m = this->aoe_ui.minimapRect();
	const bool same_rect = rect.x0 == this->minimap_x0 and rect.y0 == this->minimap_y0 and rect.w() == this->minimap_w
	                       and rect.h() == this->minimap_h;
	if (snapshot->version == this->minimap_snapshot and same_rect and not this->minimap_force) {
		return;
	}
	const auto t0 = std::chrono::steady_clock::now();
	auto convert = [](const std::vector<gamestate::vision::MinimapDot> &from, std::vector<agesxr::MinimapDot> &to) {
		to.clear();
		to.reserve(from.size());
		for (const auto &d : from) {
			agesxr::MinimapDot dot;
			dot.ne = d.ne;
			dot.se = d.se;
			dot.radius = d.radius;
			dot.kind = static_cast<uint8_t>(d.kind);
			dot.player = d.player;
			dot.fogged = d.fogged;
			dot.military = d.military;
			to.push_back(dot);
		}
	};
	agesxr::MinimapInput in;
	in.ne = static_cast<int>(snapshot->ne_size);
	in.se = static_cast<int>(snapshot->se_size);
	const size_t tiles = snapshot->ne_size * snapshot->se_size;
	in.terrain = snapshot->terrain and snapshot->terrain->size() == tiles ? snapshot->terrain->data() : nullptr;
	in.fog = snapshot->tiles and snapshot->tiles->size() == tiles ? snapshot->tiles->data() : nullptr;
	in.gaia = snapshot->neutral_player;
	in.mode = this->minimap_mode;
	const float dot_px = std::max(2.0f, std::round(3.0f * this->aoe_ui.scale()));
	// ground x fog + static dots (trees, mines, buildings) only when the tile states, the static layer,
	// the colours or the size changed (partial redraw); moving units every time
	const void *terrain_key = snapshot->terrain ? static_cast<const void *>(snapshot->terrain.get()) : nullptr;
	const void *static_key = snapshot->static_dots ? static_cast<const void *>(snapshot->static_dots.get()) : nullptr;
	if (this->minimap_base.empty() or snapshot->tiles_version != this->minimap_base_tiles
	    or terrain_key != this->minimap_base_terrain or static_key != this->minimap_base_static
	    or m.w() != this->minimap_base_w or m.h() != this->minimap_base_h) {
		agesxr::rasterMinimapBase(in, m.w(), m.h(), this->minimap_base);
		if (snapshot->static_dots) {
			convert(*snapshot->static_dots, this->minimap_dots);
			in.dots = this->minimap_dots.data();
			in.dotCount = this->minimap_dots.size();
			agesxr::drawMinimapDots(in, m.w(), m.h(), dot_px, this->minimap_base);
		}
		this->minimap_base_tiles = snapshot->tiles_version;
		this->minimap_base_terrain = terrain_key;
		this->minimap_base_static = static_key;
		this->minimap_base_w = m.w();
		this->minimap_base_h = m.h();
		this->minimap_base_count += 1;
	}
	convert(snapshot->dots, this->minimap_dots);
	in.dots = this->minimap_dots.data();
	in.dotCount = this->minimap_dots.size();
	if (this->ping_at >= 0.0 and now - this->ping_at < 3.0) {
		in.pingU = this->ping_u;
		in.pingV = this->ping_v;
		in.pingAge = static_cast<float>(now - this->ping_at);
	}
	this->minimap_image = this->minimap_base;
	agesxr::drawMinimapDots(in, m.w(), m.h(), dot_px, this->minimap_image);
	this->minimap_x0 = rect.x0;
	this->minimap_y0 = rect.y0;
	this->minimap_w = rect.w();
	this->minimap_h = rect.h();
	this->minimap_snapshot = snapshot->version;
	this->minimap_force = false;
	this->minimap_at = now;
	this->minimap_seq += 1;

	// raster time (spec S3: at most 4 ms per update), logged every 10 s
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	this->minimap_ms_sum += ms;
	this->minimap_ms_max = std::max(this->minimap_ms_max, ms);
	this->minimap_count += 1;
	if (now - this->minimap_log_at >= 10.0) {
		log::log(INFO << "UI: minimap " << this->minimap_count << " images, avg "
		              << this->minimap_ms_sum / static_cast<double>(this->minimap_count) << " ms, max "
		              << this->minimap_ms_max << " ms (" << this->minimap_w << "x" << this->minimap_h << " px, "
		              << in.ne << "x" << in.se << " tiles, " << in.dotCount << " moving dots, fog "
		              << (in.fog ? gamestate::vision_mode_name(snapshot->mode) : "off") << ", ground layer "
		              << this->minimap_base_count << " times)");
		this->minimap_base_count = 0;
		this->minimap_log_at = now;
		this->minimap_ms_sum = 0.0;
		this->minimap_ms_max = 0.0;
		this->minimap_count = 0;
	}
}

const uint32_t *AoeUiController::minimap_pixels(int &x0, int &y0, int &w, int &h) const {
	if (not this->minimap_live or this->minimap_image.empty()
	    or this->minimap_image.size() != static_cast<size_t>(this->minimap_w) * static_cast<size_t>(this->minimap_h)) {
		x0 = y0 = w = h = 0;
		return nullptr;
	}
	x0 = this->minimap_x0;
	y0 = this->minimap_y0;
	w = this->minimap_w;
	h = this->minimap_h;
	return this->minimap_image.data();
}

uint32_t AoeUiController::minimap_version() const {
	return this->minimap_seq;
}

// ---- output -------------------------------------------------------------------------------

const uint32_t *AoeUiController::overlay_pixels() {
	const auto t0 = std::chrono::steady_clock::now();
	const uint32_t version = this->aoe_ui.version();
	const uint32_t *pixels = this->aoe_ui.pixels(this->last_now);
	if (this->aoe_ui.version() != version) {
		// redraw time of the interface (spec: < 1 ms per hover on the Quest), logged every 10 s
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		this->draw_ms_sum += ms;
		this->draw_ms_max = std::max(this->draw_ms_max, ms);
		this->draw_rows += this->aoe_ui.lastBandRows();
		++this->draw_count;
	}
	if (this->last_now - this->draw_log_at >= 10.0) {
		if (this->draw_count > 0) {
			log::log(INFO << "UI: redraws " << this->draw_count << ", avg " << this->draw_ms_sum / this->draw_count
			              << " ms, max " << this->draw_ms_max << " ms, rows " << this->draw_rows / this->draw_count
			              << " per redraw");
		}
		this->draw_log_at = this->last_now;
		this->draw_ms_sum = this->draw_ms_max = 0.0;
		this->draw_rows = this->draw_count = 0;
	}
	return pixels;
}

uint32_t AoeUiController::overlay_version() const {
	return this->aoe_ui.version();
}

UiController::Bands AoeUiController::overlay_band_rows() const {
	Bands rows;
	for (const auto &band : this->aoe_ui.lastBands()) {
		rows.emplace_back(band.y0, band.y1);
	}
	return rows;
}

int AoeUiController::overlay_width() const {
	return this->aoe_ui.width();
}

int AoeUiController::overlay_height() const {
	return this->aoe_ui.height();
}

bool AoeUiController::menu_open() const {
	return this->aoe_ui.menu().open;
}

bool AoeUiController::context_open() const {
	return this->aoe_ui.context().open;
}

bool AoeUiController::board_open() const {
	return this->aoe_ui.board().open;
}

bool AoeUiController::user_paused() const {
	return false;
}

int AoeUiController::camera_bottom_px() const {
	return this->aoe_ui.bottomBarPx();
}

UiFeedbackState AoeUiController::feedback() const {
	const auto &f = this->aoe_ui.feedback();
	return {f.hoverId, f.clickedId, f.clickSeq, f.armed, f.focusMode, f.menuOpen};
}

} // namespace openage::ui
