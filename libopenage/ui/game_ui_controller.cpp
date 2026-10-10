// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "game_ui_controller.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "gamestate/production.h"
#include "input/keys.h"
#include "log/log.h"
#include "time/clock.h"


namespace openage::ui {

namespace {

/// ids of the context menu items
enum context_item_t : int {
	CTX_MOVE = 1,
	CTX_GATHER = 2,
	CTX_ATTACK = 3,
	CTX_BUILD = 4,
	CTX_RALLY = 5,
	CTX_CANCEL = 9,
};

agesxr::HudProdState to_hud(const gamestate::prod::Snapshot &s) {
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


GameUiController::GameUiController(const gamestate::MapSettings &map, UiHooks hooks, const std::string &demo) :
	map{map},
	hooks{std::move(hooks)} {
	auto &menu = this->ui.menu();
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
	menu.mapInfo = this->map_info();
	this->feed.setCommandHandler([this](int code, const std::string &label) {
		this->production_command(code, label);
	});
	this->feed.setFocusHandler([this](uint64_t id, const std::string &label) {
		log::log(INFO << "UI: order '" << label << "' clicked, camera to entity " << id);
		if (this->hooks.focus_entity) {
			this->hooks.focus_entity(id);
		}
	});

	// "what@seconds,what@seconds"
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

bool GameUiController::init() {
	bool hud_ok = this->hud.init();
	bool ui_ok = this->ui.init();
	this->ui.resize(static_cast<int>(this->width), static_cast<int>(this->height));
	if (not hud_ok or not ui_ok) {
		log::log(WARN << "UI: no system font found (XRAGES_FONT), texts are not drawn");
	}
	log::log(INFO << "UI: game interface in the engine image (Esc/F10 = game menu, right button held = context menu)");
	return hud_ok and ui_ok;
}

void GameUiController::resize(size_t w, size_t h) {
	this->width = std::max<size_t>(w, 16);
	this->height = std::max<size_t>(h, 16);
	this->ui.resize(static_cast<int>(this->width), static_cast<int>(this->height));
}

// ---- geometry -----------------------------------------------------------------------------

void GameUiController::hud_screen_rect(int &x0, int &y0, int &x1, int &y1) const {
	const double scale = static_cast<double>(this->width) / hud_texture_width();
	x0 = 0;
	y0 = 0;
	x1 = static_cast<int>(this->width);
	y1 = static_cast<int>(std::lround(hud_texture_height() * scale));
}

bool GameUiController::over_hud(double x, double y) const {
	int x0, y0, x1, y1;
	this->hud_screen_rect(x0, y0, x1, y1);
	return x >= x0 and x < x1 and y >= y0 and y < y1;
}

void GameUiController::hud_coords(double x, double y, float &hx, float &hy) const {
	const double scale = static_cast<double>(this->width) / hud_texture_width();
	hx = static_cast<float>(x / scale);
	hy = static_cast<float>(y / scale);
}

// ---- data ---------------------------------------------------------------------------------

void GameUiController::poll(double now) {
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
		sample.prod = to_hud(this->hooks.production->snapshot());
	}
	this->feed.fill(sample, now);
	// autosave (game time; not while paused, not twice per interval)
	if (sample.info.game and this->hooks.save_slot and this->autosave.due(sample.gameSeconds)) {
		this->save_slot(gamestate::save::AUTOSAVE_SLOT, "autosave");
	}

	auto &board = this->ui.board();
	const bool over = this->feed.matchOver();
	if (over and not board.open) {
		const auto match = this->feed.match();
		board.open = true;
		board.good = match == agesxr::HudMatch::VICTORY;
		board.headline = agesxr::HudFeed::matchHeadline(match);
		board.lines = {
			match == agesxr::HudMatch::VICTORY  ? "Alle gegnerischen Einheiten und Gebäude sind besiegt."
			: match == agesxr::HudMatch::DEFEAT ? "Keine eigenen Einheiten oder Gebäude mehr."
			                                    : "Niemand ist übrig geblieben.",
			"Entschieden nach " + agesxr::hudTimeText(sample.info.decided_at) + " Spielzeit – das Spiel ist angehalten.",
			"Esc: Spielmenü – Neue Karte starten oder Beenden",
		};
		log::log(INFO << "UI: match board '" << board.headline << "' shown, game paused");
	}
	this->apply_pause();
}

void GameUiController::apply_pause() {
	if (not this->hooks.clock) {
		return;
	}
	const bool wanted = this->ui.menu().paused or this->menu_pause or this->ui.board().open;
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
	log::log(INFO << "UI: game clock " << (wanted ? "paused" : "resumed") << " (user " << this->ui.menu().paused
	              << ", menu " << this->menu_pause << ", match over " << this->ui.board().open << ")");
}

void GameUiController::update(double now) {
	if (this->start_time < 0.0) {
		this->start_time = now;
	}
	if (this->last_poll < 0.0 or now - this->last_poll >= POLL_SECONDS) {
		this->last_poll = now;
		this->poll(now);
	}
	// right button held long enough: context menu at the press position
	if (this->right_held and not this->right_context and now - this->right_down_at >= HOLD_SECONDS) {
		this->right_context = this->open_context_menu(this->right_x, this->right_y);
		if (not this->right_context) {
			// nothing to offer: the release becomes the usual right click
			this->right_held = false;
		}
	}
	this->run_demo(now);
}

void GameUiController::run_demo(double now) {
	for (auto &step : this->demo) {
		if (step.done or now - this->start_time < step.at) {
			continue;
		}
		step.done = true;
		if (step.what == "board") {
			auto &board = this->ui.board();
			board.open = true;
			board.good = true;
			board.headline = "Sieg!";
			board.lines = {"Alle gegnerischen Einheiten und Gebäude sind besiegt.",
			               "Entschieden nach 12:34 Spielzeit – das Spiel ist angehalten.",
			               "Esc: Spielmenü – Neue Karte starten oder Beenden"};
			log::log(INFO << "UI: demo match board shown");
			this->apply_pause();
		}
		else if (step.what == "menu") {
			this->open_menu(true);
			log::log(INFO << "UI: demo game menu opened");
		}
		else if (step.what == "order0") {
			// first entry of the orders bar, like a click on it
			log::log(INFO << "UI: demo click on order 0 (" << this->feed.model().orders.size() << " orders)");
			this->feed.clickHit(agesxr::VrHud::kHitOrderBase, now);
		}
		else if (step.what == "save-menu" or step.what == "load-menu") {
			this->open_menu(true);
			this->ui.menu().view = step.what == "save-menu" ? agesxr::GameMenuModel::kViewSave
			                                                : agesxr::GameMenuModel::kViewLoad;
			this->refresh_slots();
			log::log(INFO << "UI: demo slot list '" << step.what << "' with " << this->ui.menu().slot_list.size() << " slots");
		}
		else if (step.what == "close-menu") {
			this->open_menu(false);
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

bool GameUiController::on_event(const renderer::WindowEvent &ev, double now) {
	using namespace input;
	switch (ev.type) {
	case event_type::MouseMove: {
		if (this->ui.wantsMouse()) {
			this->ui.onMove(static_cast<int>(ev.x), static_cast<int>(ev.y));
			return true;
		}
		float hx, hy;
		this->hud_coords(ev.x, ev.y, hx, hy);
		const bool over = this->over_hud(ev.x, ev.y);
		this->hud.pointer(over ? hx : -1.0f, over ? hy : -1.0f, false, this->feed.model(), now);
		// hovering the bar belongs to the interface (no edge scrolling), a drag passes
		return over and not this->left_held and not this->right_held;
	}
	case event_type::MouseButtonPress:
	case event_type::MouseButtonDblClick: {
		const int x = static_cast<int>(ev.x);
		const int y = static_cast<int>(ev.y);
		if (ev.button == mouse_button::LeftButton) {
			if (this->ui.wantsMouse()) {
				this->menu_action(this->ui.onClick(x, y));
				this->hud_click = true;
				return true;
			}
			if (this->over_hud(ev.x, ev.y)) {
				float hx, hy;
				this->hud_coords(ev.x, ev.y, hx, hy);
				int index = this->hud.pointer(hx, hy, true, this->feed.model(), now);
				if (index >= 0) {
					this->feed.clickHit(index, now);
				}
				this->hud_click = true;
				return true;
			}
			this->left_held = true;
			return false;
		}
		if (ev.button == mouse_button::RightButton) {
			if (this->ui.wantsMouse()) {
				// a right click closes the context menu, the game menu stays
				this->ui.closeContext();
				return true;
			}
			if (this->over_hud(ev.x, ev.y)) {
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
			// held back until the release: short = right click, long = context menu
			return true;
		}
		if (ev.button == mouse_button::MiddleButton) {
			if (this->ui.wantsMouse() or this->over_hud(ev.x, ev.y)) {
				return true;
			}
			return this->open_context_menu(x, y);
		}
		return this->ui.wantsMouse();
	}
	case event_type::MouseButtonRelease: {
		if (ev.button == mouse_button::LeftButton) {
			this->left_held = false;
			if (this->hud_click) {
				this->hud_click = false;
				return true;
			}
			return this->ui.wantsMouse();
		}
		if (ev.button == mouse_button::RightButton) {
			if (this->right_held) {
				this->right_held = false;
				if (this->right_context) {
					this->right_context = false;
					return true;
				}
				// short press: the usual right click (move / gather / attack)
				return false;
			}
			return true;
		}
		if (ev.button == mouse_button::MiddleButton) {
			return true;
		}
		return this->ui.wantsMouse();
	}
	case event_type::Wheel:
		return this->ui.wantsMouse();
	case event_type::KeyPress: {
		if (ev.auto_repeat) {
			return this->ui.menu().open;
		}
		if ((ev.key == key::Key_F5 or ev.key == key::Key_F9) and not this->ui.menu().open) {
			if (ev.key == key::Key_F5) {
				this->save_slot(gamestate::save::QUICK_SLOT, "F5");
			}
			else {
				this->load_slot(gamestate::save::QUICK_SLOT, "F9");
			}
			this->swallowed_keys.push_back(ev.key);
			return true;
		}
		if (ev.key == key::Key_Escape or ev.key == key::Key_F10) {
			if (this->ui.context().open) {
				this->ui.closeContext();
			}
			else if (this->ui.menu().open) {
				this->open_menu(false);
			}
			else if (ev.key == key::Key_Escape and this->hooks.production
			         and this->hooks.production->placement_active() and not this->ui.board().open) {
				// Esc ends the placement mode first (game binding)
				return false;
			}
			else {
				this->open_menu(true);
			}
			this->swallowed_keys.push_back(ev.key);
			return true;
		}
		return this->ui.menu().open;
	}
	case event_type::KeyRelease: {
		auto it = std::find(this->swallowed_keys.begin(), this->swallowed_keys.end(), ev.key);
		if (it != this->swallowed_keys.end()) {
			this->swallowed_keys.erase(it);
			return true;
		}
		return this->ui.menu().open;
	}
	default:
		return false;
	}
}

void GameUiController::open_menu(bool open) {
	auto &menu = this->ui.menu();
	if (menu.open == open) {
		return;
	}
	menu.open = open;
	menu.confirmQuit = false;
	menu.view = agesxr::GameMenuModel::kViewMain;
	menu.confirmSlot = -1;
	menu.mapInfo = this->map_info();
	if (open) {
		this->refresh_slots();
	}
	this->menu_pause = open;
	log::log(INFO << "UI: game menu " << (open ? "opened" : "closed"));
	this->apply_pause();
}

bool GameUiController::open_context_menu(int x, int y) {
	if (not this->info.game or not this->info.first) {
		log::log(INFO << "UI: no context menu at pixel (" << x << ", " << y << "): nothing selected");
		return false;
	}
	const auto &first = *this->info.first;
	std::vector<agesxr::GameUiItem> items;
	if (first.neutral_object) {
		log::log(INFO << "UI: no context menu: selection is not controllable");
		return false;
	}
	if (first.building and not first.unit) {
		items.push_back({CTX_RALLY, "Sammelpunkt hierher", true});
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
	const auto &sel = this->feed.model().selection;
	std::string title = sel.count > 1 ? std::to_string(sel.count) + " × " + sel.kind : sel.kind;
	this->context_x = x;
	this->context_y = y;
	this->ui.openContext(x, y, title, std::move(items));
	log::log(INFO << "UI: context menu at pixel (" << x << ", " << y << ") for '" << title << "'");
	return true;
}

void GameUiController::context_command(int id) {
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
	log::log(INFO << "UI: context menu '" << what << "' at pixel (" << this->context_x << ", "
	              << this->context_y << ")");
	if (this->hooks.send_command) {
		this->hooks.send_command(type, this->context_x, this->context_y);
	}
}

void GameUiController::menu_action(agesxr::GameUi::Result result) {
	using Action = agesxr::GameUi::Action;
	auto &menu = this->ui.menu();
	switch (result.action) {
	case Action::kContextItem:
		this->context_command(result.id);
		break;
	case Action::kResume:
		this->menu_pause = false;
		log::log(INFO << "UI: game menu closed (Weiter)");
		this->apply_pause();
		break;
	case Action::kTogglePause:
		log::log(INFO << "UI: user pause " << (menu.paused ? "on" : "off"));
		this->apply_pause();
		break;
	case Action::kSpeedChanged:
		if (this->hooks.clock) {
			this->hooks.clock->set_speed(time::speed_t::from_double(agesxr::kGameSpeedValues[menu.speed]));
		}
		log::log(INFO << "UI: game speed " << agesxr::kGameSpeedValues[menu.speed]);
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
	case Action::kShowSlots:
		this->refresh_slots();
		log::log(INFO << "UI: slot list '" << (menu.view == agesxr::GameMenuModel::kViewSave ? "save" : "load") << "'");
		break;
	case Action::kSave:
		this->save_slot(result.id, "game menu");
		this->open_menu(false);
		break;
	case Action::kLoad:
		this->load_slot(result.id, "game menu");
		this->open_menu(false);
		break;
	case Action::kQuit:
		log::log(INFO << "UI: quit confirmed");
		if (this->hooks.quit) {
			this->hooks.quit();
		}
		break;
	default:
		break;
	}
}

void GameUiController::refresh_slots() {
	auto &menu = this->ui.menu();
	menu.slot_list.clear();
	if (not this->hooks.list_slots) {
		return;
	}
	for (const auto &s : this->hooks.list_slots()) {
		menu.slot_list.push_back({s.slot, s.label(), s.exists});
	}
}

void GameUiController::save_slot(int slot, const char *why) {
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

void GameUiController::load_slot(int slot, const char *why) {
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

void GameUiController::production_command(int code, const std::string &label) {
	if (not this->hooks.production) {
		return;
	}
	log::log(INFO << "UI: HUD button '" << label << "' (" << code << ")");
	if (code == agesxr::kHudCmdCancelTraining) {
		this->hooks.production->cancel_training();
	}
	else if (agesxr::isCancelQueueCommand(code)) {
		this->hooks.production->cancel_training_at(static_cast<size_t>(code - agesxr::kHudCmdCancelQueue0));
	}
	else if (code == agesxr::kHudCmdCancelPlacement) {
		this->hooks.production->cancel_placement();
	}
	else {
		this->hooks.production->command(code);
	}
	// show the result at once
	this->last_poll = -1.0;
}

// ---- map ----------------------------------------------------------------------------------

std::string GameUiController::map_info() const {
	if (this->map.type == gamestate::map_type_t::TEST) {
		return "Testkarte";
	}
	const int biome = std::clamp(static_cast<int>(this->map.biome), 0, agesxr::kGameUiBiomeCount - 1);
	const char *opponent = this->map.ai.mode == gamestate::ai_mode_t::OFF ? "ohne Gegner"
	                       : this->map.ai.difficulty == gamestate::ai_difficulty_t::NORMAL ? "Gegner normal"
	                                                                                        : "Gegner leicht";
	char buf[160];
	std::snprintf(buf, sizeof(buf), "%s #%u, %zu × %zu, %s, %s", this->map.skirmish ? "Gefecht" : "Zufallskarte",
	              this->map.seed, this->map.size, this->map.size, agesxr::kGameUiBiomeLabels[biome], opponent);
	return buf;
}

gamestate::MapSettings GameUiController::menu_map_settings() const {
	const auto &menu = this->ui.menu();
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

const uint32_t *GameUiController::hud_pixels(double now) {
	return this->hud.pixels(this->feed.model(), now);
}

uint32_t GameUiController::hud_version() const {
	return this->hud.version();
}

const std::vector<agesxr::VrHud::DirtyBand> &GameUiController::hud_bands() const {
	return this->hud.lastBands();
}

const uint32_t *GameUiController::overlay_pixels() {
	return this->ui.pixels();
}

uint32_t GameUiController::overlay_version() const {
	return this->ui.version();
}

int GameUiController::overlay_dirty_y0() const {
	return this->ui.dirtyY0();
}

int GameUiController::overlay_dirty_y1() const {
	return this->ui.dirtyY1();
}

int GameUiController::overlay_width() const {
	return this->ui.width();
}

int GameUiController::overlay_height() const {
	return this->ui.height();
}

bool GameUiController::menu_open() const {
	return this->ui.menu().open;
}

bool GameUiController::context_open() const {
	return this->ui.context().open;
}

bool GameUiController::board_open() const {
	return this->ui.board().open;
}

bool GameUiController::user_paused() const {
	return this->ui.menu().paused;
}

const agesxr::HudModel &GameUiController::hud_model() const {
	return this->feed.model();
}

} // namespace openage::ui
