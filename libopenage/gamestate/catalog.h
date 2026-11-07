// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>


namespace openage::gamestate::prod {

/**
 * Catalog of the XR fork: every unit, building and technology of the converted
 * AoE II data with its German label, a stable HUD command code, an icon hint and
 * whether the engine has the mechanic it needs.
 *
 * What a building offers (Create/Research) comes from the nyan data at runtime;
 * the catalog only names the entries, orders them and hides those without
 * mechanic ("noch ohne Mechanik", `missing` != nullptr) until a later round
 * (docs/PLAN-NEUFASSUNG-AOE2.md W3–W8) adds it: set `missing` to nullptr there.
 * Numbers (costs, times, effects) are never stored here, they come from the data.
 *
 * Codes: 101–199 train, 201–299 build, 301–499 research. They are stable (HUD
 * buttons, tests); new entries get new codes.
 *
 * This header has no engine dependencies (host test gamestate/tech_check.cpp).
 */

enum class entry_t {
	UNIT,
	BUILDING,
	TECH,
};

struct CatalogEntry {
	/// last part of the nyan object name ("Villager", "FeudalAge")
	const char *name;
	/// German label for the HUD
	const char *label;
	/// HUD command code (stable)
	int code;
	/// true: built by villagers (placement mode)
	bool building;
	/// icon hint ("villager", "house", "sword", "bow", "horse", "hammer", "tower", "tech", "age")
	const char *icon;
	/// UNIT, BUILDING or TECH
	entry_t kind;
	/// nullptr: the mechanic exists; else why it is hidden ("noch ohne Mechanik: …")
	const char *missing;
	/// short German effect for the tooltip (techs; may be empty)
	const char *effect;
};

namespace detail {
constexpr entry_t U = entry_t::UNIT;
constexpr entry_t B = entry_t::BUILDING;
constexpr entry_t T = entry_t::TECH;
constexpr const char *TRADE = "noch ohne Mechanik: Markt/Handel (W7)";
constexpr const char *WONDER = "noch ohne Mechanik: Wunder/Reliquien (W8)";
} // namespace detail

// clang-format off
constexpr CatalogEntry CATALOG[] = {
	// ---- units (Create in buildings)
	{"Villager", "Dorfbewohner", 101, false, "villager", detail::U, nullptr, ""},
	{"Militia", "Milizsoldat", 102, false, "sword", detail::U, nullptr, ""},
	{"Spearman", "Speerkämpfer", 103, false, "sword", detail::U, nullptr, ""},
	{"Archer", "Bogenschütze", 104, false, "bow", detail::U, nullptr, ""},
	{"Skirmisher", "Plänkler", 105, false, "bow", detail::U, nullptr, ""},
	{"ScoutCavalry", "Späher", 106, false, "horse", detail::U, nullptr, ""},
	{"Knight", "Ritter", 107, false, "horse", detail::U, nullptr, ""},
	{"CamelRider", "Kamelreiter", 108, false, "horse", detail::U, nullptr, ""},
	{"HorseArcher", "Berittener Bogenschütze", 109, false, "bow", detail::U, nullptr, ""},
	{"HandCannoneer", "Handkanonier", 110, false, "bow", detail::U, nullptr, ""},
	{"EagleWarrior", "Adlerkrieger", 111, false, "sword", detail::U, nullptr, ""},
	{"Longbowman", "Langbogenschütze", 112, false, "bow", detail::U, nullptr, ""},
	{"ThrowingAxeman", "Axtwerfer", 113, false, "sword", detail::U, nullptr, ""},
	{"Huscarl", "Huskarl", 114, false, "sword", detail::U, nullptr, ""},
	{"Samurai", "Samurai", 115, false, "sword", detail::U, nullptr, ""},
	{"ChuKoNu", "Chu Ko Nu", 116, false, "bow", detail::U, nullptr, ""},
	{"Cataphract", "Kataphrakt", 117, false, "horse", detail::U, nullptr, ""},
	{"WoadRaider", "Waidkrieger", 118, false, "sword", detail::U, nullptr, ""},
	{"Berserk", "Berserker", 119, false, "sword", detail::U, nullptr, ""},
	{"TeutonicKnight", "Deutschordensritter", 120, false, "sword", detail::U, nullptr, ""},
	{"Mangudai", "Mangudai", 121, false, "bow", detail::U, nullptr, ""},
	{"WarElephant", "Kriegselefant", 122, false, "horse", detail::U, nullptr, ""},
	{"Mameluke", "Mameluck", 123, false, "horse", detail::U, nullptr, ""},
	{"Janissary", "Janitschar", 124, false, "bow", detail::U, nullptr, ""},
	{"Conquistador", "Konquistador", 125, false, "horse", detail::U, nullptr, ""},
	{"JaguarWarrior", "Jaguarkrieger", 126, false, "sword", detail::U, nullptr, ""},
	{"PlumedArcher", "Federbogenschütze", 127, false, "bow", detail::U, nullptr, ""},
	{"Tarkan", "Tarkan", 128, false, "horse", detail::U, nullptr, ""},
	{"WarWaggon", "Kriegswagen", 129, false, "horse", detail::U, nullptr, ""},
	{"Monk", "Mönch", 140, false, "monk", detail::U, nullptr, ""},
	{"Missionary", "Missionar", 141, false, "horse", detail::U, nullptr, ""},
	{"Ram", "Rammbock", 142, false, "hammer", detail::U, nullptr, ""},
	{"Mangonel", "Mangonel", 143, false, "hammer", detail::U, nullptr, ""},
	{"Scorpion", "Skorpion", 144, false, "hammer", detail::U, nullptr, ""},
	{"BombardCannon", "Bombardenkanone", 145, false, "hammer", detail::U, nullptr, ""},
	{"Trebuchet", "Tribock", 146, false, "trebuchet", detail::U, nullptr, ""},
	{"Petard", "Petarde", 147, false, "hammer", detail::U, nullptr, ""},
	{"FishingShip", "Fischerboot", 150, false, "ship", detail::U, nullptr, ""},
	{"TransportShip", "Transportschiff", 151, false, "hammer", detail::U, nullptr, ""},
	{"TradeCog", "Handelskogge", 152, false, "hammer", detail::U, nullptr, ""},
	{"Galley", "Galeere", 153, false, "hammer", detail::U, nullptr, ""},
	{"FireTrireme", "Feuerschiff", 154, false, "hammer", detail::U, nullptr, ""},
	{"DemolitionShip", "Sprengschiff", 155, false, "hammer", detail::U, nullptr, ""},
	{"CannonGalleon", "Kanonengaleone", 156, false, "hammer", detail::U, nullptr, ""},
	{"Longboat", "Langboot", 157, false, "hammer", detail::U, nullptr, ""},
	{"TurtleShip", "Schildkrötenschiff", 158, false, "hammer", detail::U, nullptr, ""},
	{"TradeCart", "Handelskarren", 159, false, "horse", detail::U, detail::TRADE, ""},
	// ---- buildings (Create of villagers, placement mode)
	{"House", "Haus", 201, true, "house", detail::B, nullptr, ""},
	{"Mill", "Mühle", 202, true, "mill", detail::B, nullptr, ""},
	{"LumberCamp", "Holzfällerlager", 203, true, "lumber", detail::B, nullptr, ""},
	{"MiningCamp", "Bergbaulager", 204, true, "mining", detail::B, nullptr, ""},
	{"Barracks", "Kaserne", 205, true, "barracks", detail::B, nullptr, ""},
	{"ArcheryRange", "Bogenschießstand", 206, true, "range", detail::B, nullptr, ""},
	{"Stable", "Stall", 207, true, "stable", detail::B, nullptr, ""},
	{"Blacksmith", "Schmiede", 208, true, "hammer", detail::B, nullptr, ""},
	{"University", "Universität", 209, true, "tech", detail::B, nullptr, ""},
	{"Outpost", "Außenposten", 210, true, "tower", detail::B, nullptr, ""},
	{"Tower", "Wachturm", 211, true, "tower", detail::B, nullptr, ""},
	{"TownCenter", "Dorfzentrum", 212, true, "house", detail::B, nullptr, ""},
	// buildings without own mechanic yet; needed as requirement of the Imperial Age (2 of them)
	{"Monastery", "Kloster", 213, true, "house", detail::B, nullptr, ""},
	{"SiegeWorkshop", "Belagerungswerkstatt", 214, true, "hammer", detail::B, nullptr, ""},
	{"Castle", "Burg", 215, true, "tower", detail::B, nullptr, ""},
	{"BombardTower", "Bombardenturm", 216, true, "tower", detail::B, nullptr, ""},
	{"Market", "Markt", 220, true, "house", detail::B, nullptr, ""},
	{"Dock", "Hafen", 221, true, "dock", detail::B, nullptr, ""},
	{"Farm", "Farm", 222, true, "farm", detail::B, nullptr, ""},
	{"FishingTrap", "Fischreuse", 223, true, "trap", detail::B, nullptr, ""},
	{"PalisadeWall", "Palisade", 224, true, "wall", detail::B, nullptr, ""},
	{"StoneWall", "Steinmauer", 225, true, "wall", detail::B, nullptr, ""},
	{"StoneGate", "Tor", 226, true, "wall", detail::B, nullptr, ""},
	{"Wonder", "Weltwunder", 227, true, "house", detail::B, detail::WONDER, ""},
	// ---- technologies: ages (Town Center)
	{"FeudalAge", "Feudalzeit", 301, false, "age", detail::T, nullptr, "Neues Zeitalter: neue Einheiten, Gebäude und Techniken"},
	{"CastleAge", "Ritterzeit", 302, false, "age", detail::T, nullptr, "Neues Zeitalter: neue Einheiten, Gebäude und Techniken"},
	{"ImperialAge", "Imperialzeit", 303, false, "age", detail::T, nullptr, "Neues Zeitalter: neue Einheiten, Gebäude und Techniken"},
	// Town Center
	{"Loom", "Webstuhl", 304, false, "tech", detail::T, nullptr, "Dorfbewohner: mehr LP und Rüstung"},
	{"WheelBarrow", "Schubkarre", 305, false, "tech", detail::T, nullptr, "Dorfbewohner schneller"},
	{"HandCart", "Handkarren", 306, false, "tech", detail::T, nullptr, "Dorfbewohner noch schneller"},
	{"TownWatch", "Stadtwache", 307, false, "tech", detail::T, nullptr, "Gebäude: mehr Sichtweite"},
	{"TownPatrol", "Stadtpatrouille", 308, false, "tech", detail::T, nullptr, "Gebäude: noch mehr Sichtweite"},
	// Lumber Camp, Mining Camp
	{"DoubleBitAxe", "Doppelaxt", 310, false, "tech", detail::T, nullptr, "Holzfäller sammeln schneller"},
	{"BowSaw", "Bügelsäge", 311, false, "tech", detail::T, nullptr, "Holzfäller sammeln schneller"},
	{"TwoManSaw", "Zweimannsäge", 312, false, "tech", detail::T, nullptr, "Holzfäller sammeln schneller"},
	{"GoldMining", "Goldbergbau", 313, false, "tech", detail::T, nullptr, "Goldsammler schneller"},
	{"GoldShaftMining", "Goldschachtbau", 314, false, "tech", detail::T, nullptr, "Goldsammler schneller"},
	{"StoneMining", "Steinbergbau", 315, false, "tech", detail::T, nullptr, "Steinsammler schneller"},
	{"StoneShaftMining", "Steinschachtbau", 316, false, "tech", detail::T, nullptr, "Steinsammler schneller"},
	// Mill (farms)
	{"HorseCollar", "Kummet", 317, false, "tech", detail::T, nullptr, "Farmen: mehr Nahrung"},
	{"HeavyPlow", "Schwerer Pflug", 318, false, "tech", detail::T, nullptr, "Farmen: mehr Nahrung"},
	{"CropRotation", "Fruchtwechsel", 319, false, "tech", detail::T, nullptr, "Farmen: mehr Nahrung"},
	// Blacksmith
	{"Forging", "Schmieden", 320, false, "tech", detail::T, nullptr, "Nahkampf: mehr Angriff"},
	{"IronCasting", "Eisenguss", 321, false, "tech", detail::T, nullptr, "Nahkampf: mehr Angriff"},
	{"BlastFurnace", "Hochofen", 322, false, "tech", detail::T, nullptr, "Nahkampf: mehr Angriff"},
	{"ScaleMailArmor", "Schuppenpanzer", 323, false, "tech", detail::T, nullptr, "Infanterie: mehr Rüstung"},
	{"ChainMailArmor", "Kettenhemd", 324, false, "tech", detail::T, nullptr, "Infanterie: mehr Rüstung"},
	{"PlateMailArmor", "Plattenpanzer", 325, false, "tech", detail::T, nullptr, "Infanterie: mehr Rüstung"},
	{"ScaleBardingArmor", "Schuppenrossharnisch", 326, false, "tech", detail::T, nullptr, "Kavallerie: mehr Rüstung"},
	{"ChainBardingArmor", "Kettenrossharnisch", 327, false, "tech", detail::T, nullptr, "Kavallerie: mehr Rüstung"},
	{"PlateBardingArmor", "Plattenrossharnisch", 328, false, "tech", detail::T, nullptr, "Kavallerie: mehr Rüstung"},
	{"Fletching", "Befiederung", 329, false, "tech", detail::T, nullptr, "Fernkampf: mehr Angriff und Reichweite"},
	{"BodkinArrow", "Ahlspitzpfeil", 330, false, "tech", detail::T, nullptr, "Fernkampf: mehr Angriff und Reichweite"},
	{"Bracer", "Armschutz", 331, false, "tech", detail::T, nullptr, "Fernkampf: mehr Angriff und Reichweite"},
	{"PaddedArcherArmor", "Gepolsterte Schützenrüstung", 332, false, "tech", detail::T, nullptr, "Schützen: mehr Rüstung"},
	{"LeatherArcherArmor", "Lederne Schützenrüstung", 333, false, "tech", detail::T, nullptr, "Schützen: mehr Rüstung"},
	{"RingArcherArmor", "Ringpanzer der Schützen", 334, false, "tech", detail::T, nullptr, "Schützen: mehr Rüstung"},
	// University
	{"Masonry", "Steinmetzkunst", 335, false, "tech", detail::T, nullptr, "Gebäude: mehr LP und Rüstung"},
	{"Architecture", "Architektur", 336, false, "tech", detail::T, nullptr, "Gebäude: mehr LP und Rüstung"},
	{"Ballistics", "Ballistik", 337, false, "tech", detail::T, nullptr, "Geschosse zielen vor"},
	{"Chemistry", "Chemie", 338, false, "tech", detail::T, nullptr, "Fernkampf: mehr Angriff"},
	{"GuardTower", "Wehrturm", 339, false, "tech", detail::T, nullptr, "Wachtürme stärker"},
	{"Keep", "Bergfried", 340, false, "tech", detail::T, nullptr, "Wehrtürme stärker"},
	{"BombardTower", "Bombardenturm", 341, false, "tech", detail::T, nullptr, "Bombardenturm baubar"},
	{"HeatedShot", "Glühende Geschosse", 342, false, "tech", detail::T, nullptr, "Türme stärker gegen Schiffe"},
	{"MurderHoles", "Pechnasen", 343, false, "tech", detail::T, nullptr, "Türme ohne Mindestreichweite"},
	{"TreadmillCrane", "Tretkran", 344, false, "tech", detail::T, nullptr, "Dorfbewohner bauen schneller"},
	{"SiegeEngineers", "Belagerungsingenieure", 345, false, "tech", detail::T, nullptr, "Belagerung: mehr Reichweite"},
	{"FortifiedWall", "Befestigte Mauer", 346, false, "tech", detail::T, nullptr, "Mauern stärker"},
	// Barracks
	{"Swordsman", "Schwertkämpfer", 350, false, "sword", detail::T, nullptr, "Milizsoldaten werden Schwertkämpfer"},
	{"Longswordsman", "Langschwertkämpfer", 351, false, "sword", detail::T, nullptr, "Schwertkämpfer werden Langschwertkämpfer"},
	{"TwoHandedSwordsman", "Zweihandschwertkämpfer", 352, false, "sword", detail::T, nullptr, "Aufwertung der Schwertkämpfer"},
	{"Champion", "Champion", 353, false, "sword", detail::T, nullptr, "Aufwertung der Schwertkämpfer"},
	{"Pikeman", "Pikenier", 354, false, "sword", detail::T, nullptr, "Speerkämpfer werden Pikeniere"},
	{"Helbardier", "Hellebardier", 355, false, "sword", detail::T, nullptr, "Pikeniere werden Hellebardiere"},
	{"EliteEagleWarrior", "Elite-Adlerkrieger", 356, false, "sword", detail::T, nullptr, "Aufwertung der Adlerkrieger"},
	{"Squires", "Knappen", 357, false, "tech", detail::T, nullptr, "Infanterie schneller"},
	{"Tracking", "Fährtenlesen", 358, false, "tech", detail::T, nullptr, "Infanterie: mehr Sichtweite"},
	// Archery Range
	{"Crossbowman", "Armbrustschütze", 360, false, "bow", detail::T, nullptr, "Bogenschützen werden Armbrustschützen"},
	{"Arbalest", "Arbalestier", 361, false, "bow", detail::T, nullptr, "Armbrustschützen werden Arbalestiere"},
	{"EliteSkirmisher", "Elite-Plänkler", 362, false, "bow", detail::T, nullptr, "Aufwertung der Plänkler"},
	{"HeavyCavalryArcher", "Schwerer ber. Bogenschütze", 363, false, "bow", detail::T, nullptr, "Aufwertung der berittenen Bogenschützen"},
	{"ThumbRing", "Daumenring", 364, false, "tech", detail::T, nullptr, "Schützen schießen schneller und genauer"},
	{"ParthianTactics", "Partherschuss", 365, false, "tech", detail::T, nullptr, "Berittene Schützen: mehr Rüstung und Angriff"},
	// Stable
	{"LightCavalry", "Leichte Kavallerie", 366, false, "horse", detail::T, nullptr, "Späher werden Leichte Kavallerie"},
	{"Hussar", "Husar", 367, false, "horse", detail::T, nullptr, "Leichte Kavallerie wird Husar"},
	{"Chevalier", "Kavalier", 368, false, "horse", detail::T, nullptr, "Ritter werden Kavaliere"},
	{"Paladin", "Paladin", 369, false, "horse", detail::T, nullptr, "Kavaliere werden Paladine"},
	{"HeavyCamelRider", "Schwerer Kamelreiter", 370, false, "horse", detail::T, nullptr, "Aufwertung der Kamelreiter"},
	{"Bloodlines", "Blutlinien", 371, false, "tech", detail::T, nullptr, "Berittene: mehr LP"},
	{"Husbandry", "Tierhaltung", 372, false, "tech", detail::T, nullptr, "Berittene schneller"},
	// Castle and unique technologies
	{"Conscription", "Wehrpflicht", 375, false, "tech", detail::T, nullptr, "Militär schneller ausgebildet"},
	{"Hoardings", "Wehrgänge", 376, false, "tech", detail::T, nullptr, "Burgen: mehr LP"},
	{"Sappers", "Pioniere", 377, false, "tech", detail::T, nullptr, "Dorfbewohner stärker gegen Gebäude"},
	{"Spies", "Spionage", 378, false, "tech", detail::T, detail::TRADE, "Sicht der Gegner"},
	{"Crenellations", "Zinnen", 379, false, "tech", detail::T, nullptr, "Burgen: mehr Reichweite"},
	{"Yeoman", "Freisassen", 380, false, "tech", detail::T, nullptr, "Schützen: mehr Reichweite"},
	{"ElDorado", "El Dorado", 381, false, "tech", detail::T, nullptr, "Adlerkrieger: mehr LP"},
	{"FurorCeltica", "Furor Celtica", 382, false, "tech", detail::T, nullptr, "Belagerung: mehr LP"},
	{"Rocketry", "Raketentechnik", 383, false, "tech", detail::T, nullptr, "Chu Ko Nu: mehr Angriff"},
	{"Logistica", "Logistica", 384, false, "tech", detail::T, nullptr, "Kataphrakte: Flächenschaden"},
	{"Kataparuto", "Kataparuto", 385, false, "tech", detail::T, nullptr, "Triböcke schneller"},
	{"Mahouts", "Mahuts", 386, false, "tech", detail::T, nullptr, "Kriegselefanten schneller"},
	{"Zealotry", "Zelotentum", 387, false, "tech", detail::T, nullptr, "Kamelreiter: mehr LP"},
	{"Artillery", "Artillerie", 388, false, "tech", detail::T, nullptr, "Kanonen: mehr Reichweite"},
	{"Berserkergang", "Berserkergang", 389, false, "tech", detail::T, nullptr, "Berserker heilen schneller"},
	{"Anarchy", "Anarchie", 390, false, "tech", detail::T, nullptr, "Huskarle in der Kaserne"},
	{"Perfusion", "Perfusion", 391, false, "tech", detail::T, nullptr, "Kaserne bildet schneller aus"},
	{"Atheism", "Atheismus", 392, false, "tech", detail::T, detail::WONDER, "Wunder- und Reliquiensieg dauern länger"},
	{"GarlandWars", "Blumenkriege", 393, false, "tech", detail::T, nullptr, "Infanterie: mehr Angriff"},
	{"Shinkichon", "Shinkichon", 394, false, "tech", detail::T, nullptr, "Mangonels: mehr Reichweite"},
	{"BeardedAxe", "Bartaxt", 395, false, "tech", detail::T, nullptr, "Axtwerfer: mehr Reichweite"},
	{"Supremacy", "Überlegenheit", 396, false, "tech", detail::T, nullptr, "Dorfbewohner kämpfen besser"},
	// elite unique units
	{"EliteLongbowman", "Elite-Langbogenschütze", 400, false, "bow", detail::T, nullptr, "Aufwertung der Langbogenschützen"},
	{"EliteThrowingAxeman", "Elite-Axtwerfer", 401, false, "sword", detail::T, nullptr, "Aufwertung der Axtwerfer"},
	{"EliteHuscarl", "Elite-Huskarl", 402, false, "sword", detail::T, nullptr, "Aufwertung der Huskarle"},
	{"EliteSamurai", "Elite-Samurai", 403, false, "sword", detail::T, nullptr, "Aufwertung der Samurai"},
	{"EliteChoKoNu", "Elite-Chu Ko Nu", 404, false, "bow", detail::T, nullptr, "Aufwertung der Chu Ko Nu"},
	{"EliteCataphract", "Elite-Kataphrakt", 405, false, "horse", detail::T, nullptr, "Aufwertung der Kataphrakte"},
	{"EliteWoadRaider", "Elite-Waidkrieger", 406, false, "sword", detail::T, nullptr, "Aufwertung der Waidkrieger"},
	{"EliteBerserk", "Elite-Berserker", 407, false, "sword", detail::T, nullptr, "Aufwertung der Berserker"},
	{"EliteTeutonicKnight", "Elite-Deutschordensritter", 408, false, "sword", detail::T, nullptr, "Aufwertung der Deutschordensritter"},
	{"EliteMangudai", "Elite-Mangudai", 409, false, "bow", detail::T, nullptr, "Aufwertung der Mangudai"},
	{"EliteWarElephant", "Elite-Kriegselefant", 410, false, "horse", detail::T, nullptr, "Aufwertung der Kriegselefanten"},
	{"EliteMameluke", "Elite-Mameluck", 411, false, "horse", detail::T, nullptr, "Aufwertung der Mamelucken"},
	{"EliteJanissary", "Elite-Janitschar", 412, false, "bow", detail::T, nullptr, "Aufwertung der Janitscharen"},
	{"EliteConquistador", "Elite-Konquistador", 413, false, "horse", detail::T, nullptr, "Aufwertung der Konquistadoren"},
	{"EliteJaguarWarrior", "Elite-Jaguarkrieger", 414, false, "sword", detail::T, nullptr, "Aufwertung der Jaguarkrieger"},
	{"ElitePlumedArcher", "Elite-Federbogenschütze", 415, false, "bow", detail::T, nullptr, "Aufwertung der Federbogenschützen"},
	{"EliteTarkan", "Elite-Tarkan", 416, false, "horse", detail::T, nullptr, "Aufwertung der Tarkane"},
	{"EliteWarWaggon", "Elite-Kriegswagen", 417, false, "horse", detail::T, nullptr, "Aufwertung der Kriegswagen"},
	// siege workshop, dock, market, monastery: hidden with their units/buildings
	{"CappedRam", "Schwerer Rammbock", 420, false, "hammer", detail::T, nullptr, ""},
	{"SiegeRam", "Belagerungsramme", 421, false, "hammer", detail::T, nullptr, ""},
	{"Onager", "Onager", 422, false, "hammer", detail::T, nullptr, ""},
	{"SiegeOnager", "Belagerungsonager", 423, false, "hammer", detail::T, nullptr, ""},
	{"HeavyScorpion", "Schwerer Skorpion", 424, false, "hammer", detail::T, nullptr, ""},
	{"WarGalley", "Kriegsgaleere", 425, false, "hammer", detail::T, nullptr, ""},
	{"Galleon", "Galeone", 426, false, "hammer", detail::T, nullptr, ""},
	{"EliteWarGalley", "Elite-Kriegsgaleere", 427, false, "hammer", detail::T, nullptr, ""},
	{"FastFireShip", "Schnelles Feuerschiff", 428, false, "hammer", detail::T, nullptr, ""},
	{"HeavyDemolitionShip", "Schweres Sprengschiff", 429, false, "hammer", detail::T, nullptr, ""},
	{"CannonGalleon", "Kanonengaleone", 430, false, "hammer", detail::T, nullptr, ""},
	{"EliteCannonGalleon", "Elite-Kanonengaleone", 431, false, "hammer", detail::T, nullptr, ""},
	{"EliteLongboat", "Elite-Langboot", 432, false, "hammer", detail::T, nullptr, ""},
	{"EliteTurtleShip", "Elite-Schildkrötenschiff", 433, false, "hammer", detail::T, nullptr, ""},
	{"Careening", "Kielholen", 434, false, "tech", detail::T, nullptr, ""},
	{"DryDock", "Trockendock", 435, false, "tech", detail::T, nullptr, ""},
	{"Shipwright", "Schiffsbaumeister", 436, false, "tech", detail::T, nullptr, ""},
	{"Cartography", "Kartografie", 437, false, "tech", detail::T, detail::TRADE, ""},
	{"Coinage", "Münzprägung", 438, false, "tech", detail::T, nullptr, "Tribut-Gebühr 20 %"},
	{"Banking", "Bankwesen", 439, false, "tech", detail::T, nullptr, "Tribut ohne Gebühr"},
	{"Caravan", "Karawane", 440, false, "tech", detail::T, detail::TRADE, ""},
	{"Guilds", "Gilden", 441, false, "tech", detail::T, nullptr, "Marktgebühr 15 %"},
	{"Redemption", "Erlösung", 442, false, "tech", detail::T, nullptr, "Mönche konvertieren Gebäude und Belagerung"},
	{"Atonement", "Sühne", 443, false, "tech", detail::T, nullptr, "Mönche konvertieren Mönche"},
	{"HerbalMedicine", "Kräuterheilkunde", 444, false, "tech", detail::T, nullptr, "Garnison heilt 4× schneller"},
	{"Heresy", "Ketzerei", 445, false, "tech", detail::T, nullptr, "Konvertierte eigene Einheiten sterben"},
	{"Sanctity", "Heiligkeit", 446, false, "tech", detail::T, nullptr, "Mönche: mehr LP"},
	{"Fervor", "Inbrunst", 447, false, "tech", detail::T, nullptr, "Mönche: schneller"},
	{"Faith", "Glaube", 448, false, "tech", detail::T, nullptr, "Mehr Widerstand gegen Konvertierung"},
	{"Illumination", "Erleuchtung", 449, false, "tech", detail::T, nullptr, "Glaube lädt schneller"},
	{"BlockPrinting", "Buchdruck", 450, false, "tech", detail::T, nullptr, "Mönche: mehr Reichweite"},
	{"Theocracy", "Theokratie", 451, false, "tech", detail::T, nullptr, "Nur der erfolgreiche Mönch verliert Glauben"},
};
// clang-format on

/// labels of other game entities that show up in the selection (never offered)
constexpr std::pair<const char *, const char *> OTHER_LABELS[] = {
	{"Sheep", "Schaf"},
	{"Turkey", "Truthahn"},
	{"Deer", "Hirsch"},
	{"Boar", "Wildschwein"},
	{"BerryBush", "Beerenstrauch"},
	{"GoldMine", "Goldmine"},
	{"StoneMine", "Steinbruch"},
	{"Relic", "Reliquie"},
	{"ForestTree", "Baum"},
	{"Conifer", "Nadelbaum"},
	{"OakTree", "Eiche"},
	{"PalmTree", "Palme"},
};

/// number of catalog entries
constexpr size_t CATALOG_SIZE = sizeof(CATALOG) / sizeof(CATALOG[0]);

/**
 * XR fork W3: commands of buildings that are no game entity or technology (codes 501–599,
 * stable like the catalog codes).
 */
/// mill: "Feld neu einsäen" – queue a farm reseed (costs of a farm, spec 13 L14); the queued
/// reseeds follow the research items in the training queue of the mill (cancel_training_at)
constexpr int CMD_RESEED_FARM = 501;

/**
 * Entry by short name (nullptr if not in the catalog). A few names exist as game
 * entity and as technology ("BombardTower", "CannonGalleon"): \p tech chooses,
 * the other kind is the fallback.
 */
inline const CatalogEntry *catalog_entry(const std::string &name, bool tech = false) {
	const CatalogEntry *any = nullptr;
	for (const auto &entry : CATALOG) {
		if (name == entry.name) {
			if ((entry.kind == entry_t::TECH) == tech) {
				return &entry;
			}
			any = any == nullptr ? &entry : any;
		}
	}
	return any;
}

/// entry by HUD code (nullptr if unknown)
inline const CatalogEntry *catalog_entry(int code) {
	for (const auto &entry : CATALOG) {
		if (code == entry.code) {
			return &entry;
		}
	}
	return nullptr;
}

/**
 * The entry may be offered: it has its mechanic. Unknown names (other data
 * sets) are offered, so that everything of the data shows up.
 */
inline bool has_mechanic(const std::string &name, bool tech = false) {
	const auto *entry = catalog_entry(name, tech);
	return entry == nullptr or entry->missing == nullptr;
}

/// civilisations of the AoE II data (PlayerSetup objects without Gaia), German names
struct CivEntry {
	/// last part of the PlayerSetup object ("Britons")
	const char *name;
	/// directory under civ/ ("britons")
	const char *dir;
	const char *label;
};

constexpr std::array<CivEntry, 18> CIVS{{
	{"Britons", "britons", "Briten"},
	{"Franks", "franks", "Franken"},
	{"Goths", "goths", "Goten"},
	{"Teutons", "teutons", "Teutonen"},
	{"Japanese", "japanese", "Japaner"},
	{"Chinese", "chinese", "Chinesen"},
	{"Byzantines", "byzantines", "Byzantiner"},
	{"Persians", "persians", "Perser"},
	{"Saracens", "saracens", "Sarazenen"},
	{"Turks", "turks", "Türken"},
	{"Vikings", "vikings", "Wikinger"},
	{"Mongols", "mongols", "Mongolen"},
	{"Celts", "celts", "Kelten"},
	{"Spanish", "spanish", "Spanier"},
	{"Aztecs", "aztecs", "Azteken"},
	{"Mayans", "mayans", "Maya"},
	{"Huns", "huns", "Hunnen"},
	{"Koreans", "koreans", "Koreaner"},
}};

/// index of a civilisation by its short name (-1 if unknown)
inline int civ_index(const std::string &name) {
	for (size_t i = 0; i < CIVS.size(); ++i) {
		if (name == CIVS[i].name) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

/// German label of a civilisation short name (the name itself if unknown)
inline std::string civ_label(const std::string &name) {
	int i = civ_index(name);
	return i < 0 ? name : std::string{CIVS[static_cast<size_t>(i)].label};
}

/// nyan object of a civilisation in a modpack ("hd_base.data.civ.britons.britons.Britons")
inline std::string civ_fqon(const std::string &modpack, const CivEntry &civ) {
	return modpack + ".data.civ." + civ.dir + "." + civ.dir + "." + civ.name;
}

/**
 * Civilisation of a player for a new game: the chosen one, or a random one
 * from the seed (deterministic: same seed and player = same civilisation;
 * players get different ones while there are enough).
 *
 * @param chosen Short name, empty or "random" for a random civilisation.
 */
inline std::string pick_civ(const std::string &chosen, uint32_t seed, size_t player) {
	if (not chosen.empty() and chosen != "random" and civ_index(chosen) >= 0) {
		return chosen;
	}
	// splitmix of the seed: stable across platforms
	uint64_t z = static_cast<uint64_t>(seed) + 0x9E3779B97F4A7C15ULL;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	z ^= z >> 31;
	// offset by player (7 is coprime to 18): random players differ for the same seed
	size_t index = (static_cast<size_t>(z % CIVS.size()) + player * 7) % CIVS.size();
	return CIVS[index].name;
}

} // namespace openage::gamestate::prod
