// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nyan/nyan.h>

#include "gamestate/resources.h"
#include "gamestate/tech_math.h"
#include "gamestate/types.h"
#include "time/time.h"


namespace openage {
namespace event {
class EventLoop;
}

namespace gamestate {
class GameEntity;
class GameState;

/**
 * Technologies of the XR fork (W1): research, the effect of technologies (nyan
 * patches on the view of a player), ages, line upgrades and civilisations
 * (PlayerSetup). The rules without engine types are in tech_math.h.
 *
 * Patches: Tech.updates and PlayerSetup.game_setup are applied as one nyan
 * transaction on the view of the player. Every value that the engine read from
 * nyan and keeps (combat values, gather rates and capacities, creatables and
 * their costs and times, build times, population space, animations) is read
 * again for the entities of that player (refresh), so that running units change
 * too: more maximum health raises the current health (AoE II), a line upgrade
 * turns the units into the next stage (the line shares one game entity in the
 * converted data), buildings show the graphics of the new age.
 *
 * All methods run in the simulation thread.
 */
namespace tech {

/// a technology offered by a building (Research ability of the owner's view)
struct Researchable {
	/// short name ("Loom")
	std::string name;
	/// nyan Tech object
	nyan::fqon_t fqon;
	resource_amounts_t cost{};
	/// research time (seconds)
	double time = 0.0;
	Condition condition{};
};

/// technology state of a player
struct PlayerTech {
	/// civilisation short name ("Britons"), empty: none (generic data)
	std::string civ{};
	/// researched technologies in the order of research (fqons)
	std::vector<nyan::fqon_t> order{};
	/// researched technologies and (ever) completed buildings
	Facts facts{};
	int age = DARK_AGE;
};

/// something the HUD should report (age reached, technology researched)
struct Message {
	player_id_t player = 0;
	/// researched technology (short name)
	std::string tech;
	/// age reached (0: none)
	int age = 0;
};

/**
 * Read a condition set (CreatableGameEntity.condition, ResearchableTech.condition).
 */
Condition read_condition_set(const std::shared_ptr<nyan::View> &view, const nyan::set_t &set);

class TechState {
public:
	TechState() = default;
	~TechState() = default;

	/// state of a player (created on first use)
	PlayerTech &player(player_id_t id);
	/// state of a player, nullptr if it has none
	const PlayerTech *find(player_id_t id) const;
	/// players with a state
	std::vector<player_id_t> players() const;

	/// counts up whenever a player's data changes (caches of the HUD)
	uint64_t version() const;

	/**
	 * Apply the civilisation of a player (PlayerSetup game_setup patches: bonuses,
	 * tech tree, architecture; starting resources are added). Before the entities
	 * of the player exist (new game), or with a refresh of them.
	 *
	 * @param civ Short name of the civilisation ("Britons").
	 * @return false if the data has no such civilisation.
	 */
	bool apply_civ(const std::shared_ptr<GameState> &state,
	               const std::shared_ptr<openage::event::EventLoop> &loop,
	               player_id_t player,
	               const std::string &civ,
	               const time::time_t &time);

	/**
	 * A technology is researched: apply its patches, record it, refresh the
	 * entities of the player, report it (\p announce).
	 */
	bool research_done(const std::shared_ptr<GameState> &state,
	                   const std::shared_ptr<openage::event::EventLoop> &loop,
	                   player_id_t player,
	                   const nyan::fqon_t &tech,
	                   const time::time_t &time,
	                   bool announce = true);

	/// a building of the player is complete (GameEntityProgress conditions)
	void building_done(player_id_t player, const nyan::fqon_t &building);

	/**
	 * Technologies a building of the player offers (Research ability of the
	 * owner's view, cached until the player's data changes).
	 */
	const std::vector<Researchable> &researchables(const std::shared_ptr<GameState> &state,
	                                               player_id_t player,
	                                               const nyan::fqon_t &building);

	/// the technology is researched by the player
	bool is_researched(player_id_t player, const nyan::fqon_t &tech) const;

	/**
	 * German label of a unit of the player: the stage of its line (a line upgrade
	 * changes the name, "Milizsoldat" -> "Schwertkämpfer"), else the catalog label.
	 */
	std::string unit_label(player_id_t player, const nyan::fqon_t &entity) const;

	/// messages since the last call (Production shows them)
	std::vector<Message> take_messages();

private:
	/// apply patch wrappers (engine.util.patch.Patch) to the view of the player
	size_t apply_patches(const std::shared_ptr<GameState> &state,
	                     player_id_t player,
	                     const std::vector<nyan::Object> &wrappers,
	                     const std::string &what);

	/// read again what the engine keeps from nyan, for every entity of the player
	void refresh_player(const std::shared_ptr<GameState> &state,
	                    const std::shared_ptr<openage::event::EventLoop> &loop,
	                    player_id_t player,
	                    const std::map<nyan::fqon_t, std::vector<std::string>> &animations_before,
	                    bool proportional,
	                    const time::time_t &time);

	/// animation paths of the abilities of the entity types of a player
	std::map<nyan::fqon_t, std::vector<std::string>> animations_of(const std::shared_ptr<GameState> &state,
	                                                              player_id_t player,
	                                                              const time::time_t &time) const;

	std::map<player_id_t, PlayerTech> states;
	std::map<std::pair<player_id_t, nyan::fqon_t>, std::vector<Researchable>> research_cache;
	std::vector<Message> messages;
	uint64_t data_version = 0;
	/// order of the next nyan transaction (strictly increasing)
	uint64_t next_order = 1;
};

} // namespace tech
} // namespace gamestate
} // namespace openage
