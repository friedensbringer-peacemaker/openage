// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "aoe_ui_controller.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "gamestate/production.h"
#include "input/keys.h"
#include "log/log.h"
#include "time/clock.h"


namespace openage::ui {

namespace {

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
	for (const auto &o : s.options) {
		p.options.push_back({o.code, o.label, o.icon, o.available, o.reason});
	}
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
			if (ui.wantsMouse() or ui.overBars(x, y)) {
				this->handle(ui.onClick(x, y, now), now);
				this->ui_click = true;
				return true;
			}
			this->left_held = true;
			return false;
		}
		if (ev.button == mouse_button::RightButton) {
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
		              << (settings.ai.mode == gamestate::ai_mode_t::OFF ? "off" : "on"));
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
		log::log(INFO << "UI: minimap click at (" << result.fx << ", " << result.fy << ") (jump follows with S3)");
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
	return settings;
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
