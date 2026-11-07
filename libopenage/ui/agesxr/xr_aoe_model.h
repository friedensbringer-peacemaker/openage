// xr.ages — Datenmodell der Spieloberfläche im AoE-II-Stil (docs/UI-SPEC-AOE.md). Reine Strukturen ohne GL und
// ohne Engine-Typen: obere Leiste (Rohstoffe, Einwohner, Zeitalter, Meldungsplakette, Uhr), Befehlsraster 5 × 3,
// Auswahlfeld (Einzel/Mehrfach, Warteschlange, Garnison), Minimap-Platzhalter, Meldungszeilen, Spielmenü mit
// Unterseiten. Kontextmenü und Sieg-Tafel nutzen ContextMenuModel/MatchBoardModel aus xr_game_ui.h.
// Gefüllt wird das Modell aus HudSample (native/app/aoe_feed.h) – am Desktop durch den Fork-Controller, auf der
// Quest ebenso, weil die Oberfläche im Engine-Bild liegt (E3). Die XR-Schicht bekommt nur UiFeedback (Haptik).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "xr_civs.h"
#include "xr_game_ui.h"
#include "xr_hud_model.h"

namespace agesxr {

// Feste Befehle des Rasters neben den Produktions-Codes der Engine (101–199 Ausbilden, 201–299 Bauen, 301–499
// Forschen; Katalog libopenage/gamestate/catalog.h).
constexpr int kAoeCmdCancelTraining = 900;   // = kHudCmdCancelTraining (hud_feed.h): letzten Auftrag abbrechen
constexpr int kAoeCmdCancelPlacement = 901;  // = kHudCmdCancelPlacement: Bauplatz-Modus verlassen
// Warteschlange: Klick auf Feld i = Auftrag i abbrechen (= kHudCmdCancelQueue0 + i aus hud_feed.h, 0.6.0-xr.0.11)
constexpr int kAoeCmdCancelQueue0 = 910;
constexpr int kAoeQueueCancelMax = 5;
constexpr int kAoeCmdAttack = 930;           // Zeile 3: Angriff (S3, bis dahin gesperrt)
constexpr int kAoeCmdHalt = 931;             // Halt (S3)
constexpr int kAoeCmdGarrison = 932;         // Garnison (S3)
constexpr int kAoeCmdUnload = 933;           // Alle ausladen (S3)
constexpr int kAoeCmdBack = 940;             // Zurück aus einem Untermenü (Feld 4,2)
constexpr int kAoeCmdPageNext = 941;         // W1: nächste Seite der Befehle (mehr als 10 Einträge)
// W4 Kampf (docs/aoe2-spec 06 §6.1–6.4, 08 §8.1/8.2): Befehle der Einheiten und Gebäude
constexpr int kAoeCmdPatrol = 950;           // Patrouille (Z), Ziel auf der Karte
constexpr int kAoeCmdGuard = 951;            // Wachen (X), Ziel: eigene Einheit/Gebäude
constexpr int kAoeCmdFollow = 952;           // Folgen (C), Ziel: Einheit
constexpr int kAoeCmdAttackMove = 953;       // Angriffsbewegung (Tab), Ziel auf der Karte
constexpr int kAoeCmdDelete = 954;           // Löschen (Entf), auf der Quest mit Rückfrage
constexpr int kAoeCmdStance0 = 955;          // + combat::stance_t: Aggressiv (A), Defensiv (D), Stellung halten (N), Kein Angriff (O)
constexpr int kAoeStanceCount = 4;
constexpr int kAoeCmdFormation0 = 960;       // + combat::formation_t − 1: Linie (Q), Kasten (W), Gestaffelt (E), Flanke (F)
constexpr int kAoeFormationCount = 4;
constexpr int kAoeCmdBell = 965;             // Dorfzentrum: Stadtglocke / Zurück an die Arbeit (B)
constexpr int kAoeCmdRepair = 973;           // W5: Dorfbewohner reparieren (R), Ziel: eigenes beschädigtes Gebäude
constexpr int kAoeCmdGateLock = 974;         // W5: Tor verriegeln / entriegeln (Platz 0, L)
// W6 Spezial (docs/aoe2-spec 05 §5.7–5.9, 06 §6.5, 08 §8.1/8.2): Mönche, Transporter, Tribock
constexpr int kAoeCmdConvert = 966;         // Mönch: Konvertieren (C), Ziel: gegnerische Einheit
constexpr int kAoeCmdHeal = 967;            // Mönch: Heilen (E), Ziel: eigene Einheit
constexpr int kAoeCmdUnloadAt = 968;        // Transportschiff: Entladen (L), Ziel: Ufer
constexpr int kAoeCmdPack = 969;            // Tribock: Packen (P)
constexpr int kAoeCmdUnpack = 970;          // Tribock: Entpacken (U)
constexpr int kAoeCmdDropRelic = 971;       // Mönch mit Reliquie: Reliquie ablegen
constexpr int kAoeCmdMarket0 = 980;         // W7: Markt, 980 + Ware·2 + 1 beim Verkaufen (Nahrung, Holz, Stein)
constexpr int kAoeCmdUnloadHere = 972;      // Rammbock/Schiff: Entladen hier (L)
// Sondertasten der Hotkeys (AoeCommand::hotkey): Entf und Tab
constexpr char kAoeKeyDelete = 0x7f;
constexpr char kAoeKeyTab = 0x09;
// Rückkanal-IDs (UiFeedback) der Warteschlangenfelder
constexpr int kAoeCmdQueueBase = 1000;

constexpr int kAoeGridCells = 15;
constexpr int kAoeOptionCells = 10;  // Zeilen 1–2: Angebot der Engine (mehr: Seiten, W1)
constexpr int kAoeQueueShown = 5;
constexpr int kAoeGarrisonMax = 10;
constexpr int kAoeMultiShown = 12;
constexpr int kAoeMessagesMax = 4;

// Hotkeys des Rasters (Spec §1.2): Q W E R T / A S D F G / Z X C V B.
constexpr const char kAoeHotkeys[kAoeGridCells + 1] = "QWERTASDFGZXCVB";
constexpr char aoeHotkey(int cell) { return cell >= 0 && cell < kAoeGridCells ? kAoeHotkeys[cell] : 0; }
constexpr int aoeCellOfHotkey(char key) {
    for (int i = 0; i < kAoeGridCells; ++i)
        if (kAoeHotkeys[i] == key) return i;
    return -1;
}

// Ein Feld des Befehlsrasters (id 0 = leer, kein Knopfrahmen).
struct AoeCommand {
    int id = 0;
    HudIcon icon = HudIcon::None;
    std::string label;   // Kurztext unter dem Symbol („Dorfbewohner“, „Haus“)
    bool enabled = true;
    std::string reason;  // Sperrgrund („nicht genug Holz (30)“), Zeile im Auswahlfeld
    char hotkey = 0;     // W4: eigene Taste (Buchstabe, kAoeKeyDelete, kAoeKeyTab); 0 = Taste des Rasterplatzes
    bool active = false; // W4: gewählter Zustand (Stellung, Formation, Glocke läutet) – gedrückt gezeichnet
    bool operator==(const AoeCommand& o) const {
        return id == o.id && icon == o.icon && label == o.label && enabled == o.enabled && reason == o.reason &&
               hotkey == o.hotkey && active == o.active;
    }
    bool operator!=(const AoeCommand& o) const { return !(*this == o); }
};

// Feld der Warteschlange, Garnison oder Mehrfachauswahl.
struct AoeSlot {
    std::string label;
    HudIcon icon = HudIcon::None;
    bool operator==(const AoeSlot& o) const { return label == o.label && icon == o.icon; }
    bool operator!=(const AoeSlot& o) const { return !(*this == o); }
};

// Bestellung (0.6.0-xr.0.11, prod::Order): laufende Ausbildung bzw. Bau eines eigenen Gebäudes.
struct AoeOrder {
    uint64_t entity = 0;          // Gebäude (Klick: Kamera dorthin, auswählen)
    HudIcon icon = HudIcon::None;
    std::string label;            // „Dorfbewohner“, bei Bau „Haus“
    std::string building;         // „Dorfzentrum“ (leer bei Bau)
    int count = 1;                // Aufträge dieser Art in der Warteschlange
    float remaining = -1.0f;      // Sekunden bis fertig, < 0 = unbekannt
    float progress = 0.0f;        // 0 … 1
    bool construction = false;
    bool operator==(const AoeOrder& o) const {
        return entity == o.entity && icon == o.icon && label == o.label && building == o.building && count == o.count &&
               remaining == o.remaining && progress == o.progress && construction == o.construction;
    }
    bool operator!=(const AoeOrder& o) const { return !(*this == o); }
};
constexpr int kAoeOrdersShown = 12;

// Spielstand-Platz (0.6.0-xr.0.11, gamestate::save::SlotInfo).
struct AoeSaveSlot {
    int slot = 0;                 // 1 … 5, Schnell, 0 = Automatisch
    std::string label;            // „Slot 2: Zufallskarte #3 · 12:34 · 2026-10-08 19:30“, „Slot 2: leer“
    bool exists = false;
    bool writable = true;         // Automatisch: nur laden
    bool operator==(const AoeSaveSlot& o) const {
        return slot == o.slot && label == o.label && exists == o.exists && writable == o.writable;
    }
    bool operator!=(const AoeSaveSlot& o) const { return !(*this == o); }
};
constexpr int kAoeSlotsMax = 7;

struct AoeSelection {
    int count = 0;                 // 0 = nichts gewählt
    std::string name;              // „Dorfbewohner“, bei Mehrfachauswahl Kopfzeile „3 Dorfbewohner“
    HudIcon icon = HudIcon::None;  // Porträt
    int hp = 0, hpMax = 0;         // hpMax ≤ 0 = kein Balken
    float progress = -1.0f;        // Bau/Ausbildung 0 … 1 statt HP, < 0 = keiner
    std::string detail;            // „Bau 30 %“, „wartet auf ein Haus“
    std::string stats;             // „Angriff 3 · Rüstung 0/0 · Reichw. 0“ (leer = keine Zeile)
    std::vector<AoeSlot> queue;    // Aufträge, erstes in Arbeit (alle; gezeigt werden kAoeQueueShown + „+n“)
    float queueProgress = 0.0f;
    int garrison = 0, garrisonMax = 0;  // Garnison (W4: belegt, Felder mit den Insassen)
    std::vector<AoeSlot> garrisonUnits; // W4: Insassen (Porträts)
    int arrows = 0;                     // W4: Pfeile je Salve (Gebäude mit Pfeilen), 0 = keine Zeile
    std::vector<AoeSlot> units;    // Mehrfachauswahl: Porträts (gezeigt kAoeMultiShown + „+n“)
    std::string blocked;           // Sperrgrund-Zeile („Haus: nicht genug Holz (30)“)
    bool multi() const { return count > 1 && !units.empty(); }
    bool operator==(const AoeSelection& o) const {
        return count == o.count && name == o.name && icon == o.icon && hp == o.hp && hpMax == o.hpMax &&
               progress == o.progress && detail == o.detail && stats == o.stats && queue == o.queue &&
               queueProgress == o.queueProgress && garrison == o.garrison && garrisonMax == o.garrisonMax &&
               garrisonUnits == o.garrisonUnits && arrows == o.arrows &&
               units == o.units && blocked == o.blocked;
    }
    bool operator!=(const AoeSelection& o) const { return !(*this == o); }
};

// Sicht einer neuen Karte (Seite „Neue Karte“), Reihenfolge = gamestate::vision_mode_t.
constexpr int kAoeVisionCount = 3;
constexpr const char* kAoeVisionLabels[kAoeVisionCount] = {"Normal", "Erkundet", "Alles sichtbar"};

// Minimap: Grundfarbe je Landschaft (Platzhalter), Kameraausschnitt in Kartenanteilen 0 … 1; live = echte Karte
// als eigene Textur unter der Leiste (xr_minimap.h), die Raute bleibt dann im Leistenbild durchsichtig.
struct AoeMinimap {
    int biome = 0;                      // kGameUiBiomeLabels-Index
    int tiles = 64;                     // Kantenlänge
    float camX = 0.5f, camY = 0.5f;     // Mitte des Kamerarahmens (0 … 1)
    float camW = 0.25f, camH = 0.15f;   // Breite/Höhe des Rahmens (0 … 1)
    bool live = false;
    bool operator==(const AoeMinimap& o) const {
        return biome == o.biome && tiles == o.tiles && camX == o.camX && camY == o.camY && camW == o.camW && camH == o.camH &&
               live == o.live;
    }
    bool operator!=(const AoeMinimap& o) const { return !(*this == o); }
};

// Meldungszeile über der unteren Leiste (Spec §1.5). kind: 0 Spiel (Spielerfarbe), 1 Warnung (gelb), 2 Gefahr (rot).
struct AoeMessage {
    std::string text;
    int kind = 0;
    double shownAt = 0.0;  // Sekunden (now) beim Einblenden; nach kMsgSeconds Fade 300 ms
    bool operator==(const AoeMessage& o) const { return text == o.text && kind == o.kind && shownAt == o.shownAt; }
    bool operator!=(const AoeMessage& o) const { return !(*this == o); }
};

// Spielmenü (Esc/F10/Y/Knopf „Menü“, Skizze 3) mit Unterseiten.
struct AoeMenuModel {
    enum Page { kMain = 0, kSettings, kNewMap, kSave, kLoad, kPageCount };
    bool open = false;
    int page = kMain;
    bool confirmSurrender = false;  // „Partie aufgeben“ scharf (erster Klick)
    int confirmSlot = -1;           // Slot-Seite: Zeile scharf („Überschreiben?“ / „Spiel verwerfen?“), −1 = keine
    std::vector<AoeSaveSlot> slot_list; // Spielstände (Seiten kSave/kLoad)
    double armedAt = 0.0;           // Zeit des Scharfschaltens (Frist kConfirmSeconds)
    // Einstellungen
    int speed = 1;                  // kGameSpeedLabels
    bool labels = true;             // Beschriftung der Knöpfe
    bool hotkeys = true;            // Hotkey-Buchstaben zeigen
    bool messages = true;           // Meldungszeilen
    // Neue Karte
    int biome = 0, size = 1, opponent = 1, seed = 1;
    int civ = kCivRandom, aiCiv = kCivRandom;  // W1: Kultur des Spielers und des Gegners (xr_civs.h, 0 = Zufall)
    int vision = 0;                 // Sicht (kAoeVisionLabels = gamestate::vision_mode_t)
    // Spielstand (0.6.0-xr.0.11, anderer Agent): verfügbar → Knöpfe aktiv, sonst „Speichern (folgt)“
    bool saveAvailable = false;
    bool loadAvailable = false;
    bool quest = false;             // Hinweis „App beenden: VR-Menü“ nur auf der Quest
    std::string mapInfo;            // „Karte #17 · Grasland 64 × 64 · 12:34 · Spiel angehalten“
    bool operator==(const AoeMenuModel& o) const {
        return open == o.open && page == o.page && confirmSurrender == o.confirmSurrender &&
               confirmSlot == o.confirmSlot && slot_list == o.slot_list && speed == o.speed &&
               labels == o.labels && hotkeys == o.hotkeys && messages == o.messages && biome == o.biome &&
               size == o.size && opponent == o.opponent && seed == o.seed && civ == o.civ && aiCiv == o.aiCiv &&
               vision == o.vision && saveAvailable == o.saveAvailable &&
               loadAvailable == o.loadAvailable && quest == o.quest && mapInfo == o.mapInfo;
    }
    bool operator!=(const AoeMenuModel& o) const { return !(*this == o); }
};

struct AoeModel {
    // obere Leiste
    int food = 0, wood = 0, gold = 0, stone = 0;
    int population = 0, populationMax = 0;  // wie HudModel (0 nur Zahl, < 0 „3/–“)
    bool housingAlert = false;              // „Mehr Häuser nötig“: Einwohner rot blinkend
    double gameSeconds = 0.0;
    bool paused = false;                    // Uhr zeigt „⏸“
    std::string age = "Dunkles Zeitalter";  // W1: Zeitalter des Spielers aus der Engine (HudProdState::ageLabel)
    int ageIndex = 1;                       // Plakette „I“ … „IV“
    std::string plakette;                   // Meldungsplakette (letzte Meldung, kPlaketteSeconds)
    HudStatus plaketteKind = HudStatus::kInfo;
    double plaketteAt = 0.0;
    // untere Leiste
    std::vector<AoeCommand> grid;           // kAoeGridCells (kürzer = Rest leer)
    // W1: Zeile beim Zielen je Feld (Kosten · Zeit · Wirkung, „Webstuhl – 50 Gold · 25 s · …“), leer = keine
    std::vector<std::string> tips;
    AoeSelection selection;
    AoeMinimap minimap;
    std::vector<AoeMessage> messages;       // älteste zuerst, höchstens kAoeMessagesMax
    std::vector<AoeOrder> orders;           // Bestellungen aller eigenen Gebäude (nach Restzeit), Auswahlfeld ohne Auswahl

    int shownSecond() const { return gameSeconds > 0.0 ? static_cast<int>(gameSeconds) : 0; }
    const AoeCommand& cell(int i) const {
        static const AoeCommand kEmpty;
        return i >= 0 && i < static_cast<int>(grid.size()) ? grid[static_cast<size_t>(i)] : kEmpty;
    }
    const std::string& tip(int i) const {
        static const std::string kNone;
        return i >= 0 && i < static_cast<int>(tips.size()) ? tips[static_cast<size_t>(i)] : kNone;
    }
    bool sameTop(const AoeModel& o) const {
        return food == o.food && wood == o.wood && gold == o.gold && stone == o.stone && population == o.population &&
               populationMax == o.populationMax && housingAlert == o.housingAlert && shownSecond() == o.shownSecond() &&
               paused == o.paused && age == o.age && ageIndex == o.ageIndex && plakette == o.plakette &&
               plaketteKind == o.plaketteKind && plaketteAt == o.plaketteAt;
    }
    bool sameBar(const AoeModel& o) const {
        return grid == o.grid && tips == o.tips && selection == o.selection && minimap == o.minimap && orders == o.orders;
    }
    bool sameMessages(const AoeModel& o) const { return messages == o.messages; }
};

// Rückkanal für die XR-Schicht (Spec §2.4): Haptik nur bei clickSeq-Wechsel, nie bei Hover.
struct UiFeedback {
    int hoverId = 0;        // Fläche unter dem Zeiger (0 = keine)
    int clickedId = 0;      // zuletzt geklickte Fläche
    uint32_t clickSeq = 0;  // zählt je Klick auf eine Fläche hoch
    bool armed = false;     // „Partie aufgeben“ scharf (Doppelpuls beim Wechsel auf true)
    bool focusMode = false; // Stick-Fokus aktiv (Stick bewegt den Fokus statt der Kamera)
    bool menuOpen = false;  // Spiel- oder Kontextmenü offen
};

// Oberflächen-Stil (window_settings::ui_style im Fork): AoE-Layout (Standard) oder die bisherige HUD-Leiste.
enum class UiStyle { kAoe, kClassic };
inline UiStyle uiStyleOf(const std::string& name) { return name == "classic" ? UiStyle::kClassic : UiStyle::kAoe; }

// Symbol zu einem Produktions-Code (Engine: production_math.h KNOWN) bzw. Hinweis-Text.
inline HudIcon aoeIconFor(int code, const std::string& hint) {
    switch (code) {
    case 101: return HudIcon::Villager;
    case 102: case 103: return HudIcon::Sword;
    case 104: case 105: return HudIcon::Bow;
    case 106: return HudIcon::Horse;
    case 201: return HudIcon::House;
    case 202: return HudIcon::Mill;
    case 203: return HudIcon::LumberCamp;
    case 204: return HudIcon::MiningCamp;
    case 205: return HudIcon::Barracks;
    case 206: return HudIcon::Range;
    case 207: return HudIcon::Stable;
    case kAoeCmdAttack: return HudIcon::Sword;
    case kAoeCmdHalt: return HudIcon::Halt;
    case kAoeCmdGarrison: return HudIcon::Garrison;
    case kAoeCmdUnload: return HudIcon::Unload;
    case kAoeCmdCancelTraining: case kAoeCmdCancelPlacement: return HudIcon::Cancel;
    case kAoeCmdBack: return HudIcon::Back;
    case kAoeCmdPageNext: return HudIcon::Next;
    default: break;
    }
    if (code >= 301 && code <= 303) return HudIcon::AgeUp;  // Feudal-/Ritter-/Imperialzeit
    if (hint == "villager") return HudIcon::Villager;
    if (hint == "monk") return HudIcon::Monk;
    if (hint == "trebuchet") return HudIcon::Trebuchet;
    if (hint == "house") return HudIcon::House;
    if (hint == "sword") return HudIcon::Sword;
    if (hint == "bow") return HudIcon::Bow;
    if (hint == "horse") return HudIcon::Horse;
    if (hint == "tower") return HudIcon::Tower;
    // W1: Hinweise des Katalogs (libopenage/gamestate/catalog.h)
    if (hint == "mill") return HudIcon::Mill;
    if (hint == "lumber") return HudIcon::LumberCamp;
    if (hint == "mining") return HudIcon::MiningCamp;
    if (hint == "barracks") return HudIcon::Barracks;
    if (hint == "range") return HudIcon::Range;
    if (hint == "stable") return HudIcon::Stable;
    if (hint == "wall") return HudIcon::Wall;
    if (hint == "tech") return HudIcon::Tech;
    if (hint == "age") return HudIcon::AgeUp;
    // W3: Farm, Fischreuse, „Feld neu einsäen“ (Nahrungssymbol), Hafen (Gebäude), Schiffe (Hammer)
    if (hint == "farm" || hint == "trap") return HudIcon::Food;
    if (hint == "dock") return HudIcon::House;
    return HudIcon::Hammer;
}

// Symbol zu einem deutschen Namen der Auswahl (Porträt, Warteschlange).
inline HudIcon aoeIconForName(const std::string& name, bool building, bool villager, bool ranged, bool mounted) {
    if (villager || name.rfind("Dorfbewohner", 0) == 0) return HudIcon::Villager;
    if (name.rfind("Mönch", 0) == 0) return HudIcon::Monk;
    if (name.rfind("Tribock", 0) == 0) return HudIcon::Trebuchet;
    if (name == "Mühle") return HudIcon::Mill;
    if (name == "Holzfällerlager") return HudIcon::LumberCamp;
    if (name == "Bergbaulager") return HudIcon::MiningCamp;
    if (name == "Kaserne") return HudIcon::Barracks;
    if (name == "Bogenschießstand" || name == "Schießanlage") return HudIcon::Range;
    if (name == "Stall") return HudIcon::Stable;
    if (name == "Palisade" || name == "Steinmauer") return HudIcon::Wall;
    if (name == "Wachturm" || name == "Burg" || name == "Außenposten") return HudIcon::Tower;
    if (name == "Schaf" || name == "Truthahn") return HudIcon::Sheep;
    if (name == "Baum" || name == "Wald") return HudIcon::Tree;
    if (building) return HudIcon::House;
    if (ranged) return HudIcon::Bow;
    if (mounted) return HudIcon::Horse;
    return HudIcon::Sword;
}

// Abbrechen-Feld (Platz 14): Bauplatz verlassen, letzten Auftrag abbrechen oder gesperrt.
inline AoeCommand aoeCancelCell(bool cancelTraining, bool cancelPlacement) {
    if (cancelPlacement) return {kAoeCmdCancelPlacement, HudIcon::Cancel, "Abbrechen", true, ""};
    if (cancelTraining) return {kAoeCmdCancelTraining, HudIcon::Cancel, "Abbrechen", true, ""};
    return {kAoeCmdCancelTraining, HudIcon::Cancel, "Abbrechen", false, "nichts abzubrechen"};
}

// Zeile 3 des Rasters ohne passende Auswahl: feste Aktionen gesperrt (Angriffsbewegung · Halt · Garnison ·
// Alle raus) und Abbrechen. W4: mit Einheiten oder Gebäuden belegt aoeCombatGrid() die Felder.
inline void aoeFixedRow(AoeModel& m, bool cancelTraining, bool cancelPlacement) {
    if (m.grid.size() < static_cast<size_t>(kAoeGridCells)) m.grid.resize(static_cast<size_t>(kAoeGridCells));
    const int base = 10;
    m.grid[base + 0] = {kAoeCmdAttackMove, HudIcon::Sword, "Angriffsbew.", false, "nur für Militäreinheiten"};
    m.grid[base + 1] = {kAoeCmdHalt, HudIcon::Halt, "Halt", false, "keine Einheit gewählt"};
    m.grid[base + 2] = {kAoeCmdGarrison, HudIcon::Garrison, "Garnison", false, "keine Einheit gewählt"};
    m.grid[base + 3] = {kAoeCmdUnload, HudIcon::Unload, "Alle raus", false, "kein Gebäude mit Garnison"};
    m.grid[base + 4] = aoeCancelCell(cancelTraining, cancelPlacement);
}

// W4: Kampfzustand der Auswahl für das Befehlsraster (aus HudEntity, native/app/aoe_feed.h).
struct AoeCombat {
    bool military = false;      // erste Auswahl ist eine Militäreinheit
    int militaryCount = 0;      // Militäreinheiten in der Auswahl (Formationen ab 2)
    bool stances = false, formations = false;
    int stance = 0;             // combat::stance_t: 0 aggressiv, 1 defensiv, 2 Stellung halten, 3 kein Angriff
    int formation = 0;          // combat::formation_t: 0 keine, 1 Linie, 2 Kasten, 3 gestaffelt, 4 Flanke
    bool villager = false;
    bool building = false;
    bool townCenter = false;
    bool bell = false;          // Glocke läutet
    int garrison = 0, garrisonMax = 0;
    bool deleteArmed = false;   // Quest: „Löschen“ wartet auf die Bestätigung
    bool gate = false;          // W5: Tor (Befehl Verriegeln/Entriegeln)
    bool gateLocked = false;
    // W6: Mönch (Glaube 0–100, trägt Reliquie), Transporter (Ladung/Plätze, Schiff), Tribock
    bool monk = false, heals = false, relic = false;
    int faith = 0;
    int cargo = 0, cargoMax = 0;
    bool transport = false;
    bool packable = false, unpacked = false, packing = false;
    // W7: fertiger Markt, Gold je Los (Nahrung, Holz, Stein)
    bool market = false;
    long long marketBuy[3] = {0, 0, 0}, marketSell[3] = {0, 0, 0};
    bool marketCanBuy[3] = {false, false, false}, marketCanSell[3] = {false, false, false};
};

// W6: Befehle der Spezialeinheiten (Spec 08 §8.1: Mönch Konvertieren · Heilen auf 0/1, mit Reliquie
// Reliquie ablegen auf 0, Transport Entladen auf 0, Packen/Entpacken auf 4; Tasten §8.2: C Konvertieren,
// E Heilen, L Entladen, P Packen, U Entpacken). true = das Raster wurde belegt.
inline bool aoeSpecialGrid(AoeModel& m, const AoeCombat& c, const AoeCommand& del) {
    if (c.monk) {
        m.grid.assign(static_cast<size_t>(kAoeGridCells), AoeCommand{});
        if (c.relic) {
            m.grid[0] = {kAoeCmdDropRelic, HudIcon::Back, "Reliquie ablegen", true, ""};
        }
        else {
            const bool full = c.faith >= 100;
            m.grid[0] = {kAoeCmdConvert, HudIcon::Emblem, "Konvertieren", full,
                         full ? "" : "Glaube " + std::to_string(c.faith) + "/100", 'C'};
            m.grid[1] = {kAoeCmdHeal, HudIcon::Tech, "Heilen", c.heals, c.heals ? "" : "kann nicht heilen", 'E'};
        }
        m.grid[3] = del;
        m.grid[4] = {kAoeCmdGarrison, HudIcon::Garrison, "Garnison", true, "", 'G'};
        m.grid[9] = {kAoeCmdHalt, HudIcon::Halt, "Halt", true, "", 'S'};
        return true;
    }
    if (c.transport) {
        m.grid.assign(static_cast<size_t>(kAoeGridCells), AoeCommand{});
        const bool any = c.cargo > 0;
        m.grid[0] = {kAoeCmdUnloadAt, HudIcon::Unload, "Entladen", any, any ? "" : "niemand an Bord", 'L'};
        m.grid[3] = del;
        m.grid[4] = {kAoeCmdUnloadHere, HudIcon::Unload, "Hier raus", any, any ? "" : "niemand an Bord"};
        m.grid[9] = {kAoeCmdHalt, HudIcon::Halt, "Halt", true, "", 'S'};
        return true;
    }
    if (c.packable) {
        m.grid.assign(static_cast<size_t>(kAoeGridCells), AoeCommand{});
        m.grid[3] = del;
        m.grid[4] = c.unpacked ? AoeCommand{kAoeCmdPack, HudIcon::Hammer, "Packen", !c.packing, c.packing ? "baut gerade um" : "", 'P'}
                               : AoeCommand{kAoeCmdUnpack, HudIcon::Hammer, "Entpacken", !c.packing, c.packing ? "baut gerade um" : "", 'U'};
        m.grid[9] = {kAoeCmdHalt, HudIcon::Halt, "Halt", true, "", 'S'};
        return true;
    }
    return false;
}

// Befehlsraster der Einheiten/Gebäude (Spec 08 §8.1, Tasten des klassischen Profils §8.2):
//   Militär: Patrouille Z · Wachen X · Folgen C · Löschen Entf · Garnison G / Aggressiv A · Defensiv D ·
//            Stellung halten N · Kein Angriff O · Halt S / Linie Q · Kasten W · Gestaffelt E · Flanke F ·
//            Angriffsbewegung Tab (Formationen ab zwei Militäreinheiten)
//   Dorfbewohner: Zeilen 1–2 = Bauen (Engine), Zeile 3: Garnison Z · Halt X · Löschen Entf · Reparieren R (W5) ·
//            Abbrechen
//   Gebäude: Zeilen 1–2 = Ausbilden/Forschen, Zeile 3: Glocke B (Dorfzentrum) · – · Löschen Entf ·
//            Alle raus · Abbrechen; Tor (W5): Platz 0 Verriegeln/Entriegeln L
// true = das Raster wurde belegt (sonst bleibt aoeFixedRow).
inline bool aoeCombatGrid(AoeModel& m, const AoeCombat& c, bool cancelTraining, bool cancelPlacement) {
    if (m.grid.size() < static_cast<size_t>(kAoeGridCells)) m.grid.resize(static_cast<size_t>(kAoeGridCells));
    const AoeCommand del = c.deleteArmed ? AoeCommand{kAoeCmdDelete, HudIcon::Cancel, "Sicher?", true, "", kAoeKeyDelete, true}
                                         : AoeCommand{kAoeCmdDelete, HudIcon::Cancel, "Löschen", true, "", kAoeKeyDelete};
    if (!c.building && !c.villager && aoeSpecialGrid(m, c, del)) return true;
    if (c.military && !c.building && !c.villager) {
        m.grid.assign(static_cast<size_t>(kAoeGridCells), AoeCommand{});
        m.grid[0] = {kAoeCmdPatrol, HudIcon::Next, "Patrouille", true, "", 'Z'};
        m.grid[1] = {kAoeCmdGuard, HudIcon::Tower, "Wachen", true, "", 'X'};
        m.grid[2] = {kAoeCmdFollow, HudIcon::Horse, "Folgen", true, "", 'C'};
        m.grid[3] = del;
        m.grid[4] = {kAoeCmdGarrison, HudIcon::Garrison, "Garnison", true, "", 'G'};
        static const char* const kStanceLabels[kAoeStanceCount] = {"Aggressiv", "Defensiv", "Halten", "Kein Angriff"};
        static const char kStanceKeys[kAoeStanceCount] = {'A', 'D', 'N', 'O'};
        static const HudIcon kStanceIcons[kAoeStanceCount] = {HudIcon::Sword, HudIcon::Wall, HudIcon::Halt, HudIcon::Stop};
        for (int i = 0; i < kAoeStanceCount; ++i) {
            AoeCommand s{kAoeCmdStance0 + i, kStanceIcons[i], kStanceLabels[i], c.stances, c.stances ? "" : "keine Stellungen",
                         kStanceKeys[i], c.stances && c.stance == i};
            m.grid[static_cast<size_t>(5 + i)] = s;
        }
        m.grid[9] = {kAoeCmdHalt, HudIcon::Halt, "Halt", true, "", 'S'};
        static const char* const kFormationLabels[kAoeFormationCount] = {"Linie", "Kasten", "Gestaffelt", "Flanke"};
        static const char kFormationKeys[kAoeFormationCount] = {'Q', 'W', 'E', 'F'};
        const bool formations = c.formations && c.militaryCount >= 2;
        for (int i = 0; i < kAoeFormationCount; ++i) {
            AoeCommand f{kAoeCmdFormation0 + i, HudIcon::Emblem, kFormationLabels[i], formations,
                         formations ? "" : "nur mit mehreren Militäreinheiten", kFormationKeys[i],
                         formations && c.formation == i + 1};
            m.grid[static_cast<size_t>(10 + i)] = f;
        }
        m.grid[14] = {kAoeCmdAttackMove, HudIcon::Sword, "Angriffsbew.", true, "", kAoeKeyTab};
        if (c.cargoMax > 0) {
            // W6: Rammbock mit Infanterie (Spec 06 §6.5) – Garnisonsplatz wird „Entladen“
            m.grid[4] = {kAoeCmdUnloadHere, HudIcon::Unload, "Entladen", c.cargo > 0, c.cargo > 0 ? "" : "niemand drin", 'L'};
        }
        return true;
    }
    if (c.villager) {
        // Tasten des Rasterplatzes (Z, X): G/S gehören der Seitentaste und dem Bauangebot
        m.grid[10] = {kAoeCmdGarrison, HudIcon::Garrison, "Garnison", true, ""};
        m.grid[11] = {kAoeCmdHalt, HudIcon::Halt, "Halt", true, ""};
        m.grid[12] = del;
        // W5 (Spec 08 §8.2): Reparieren R – danach ein eigenes beschädigtes Gebäude anklicken
        m.grid[13] = {kAoeCmdRepair, HudIcon::Hammer, "Reparieren", true, "", 'R'};
        m.grid[14] = aoeCancelCell(cancelTraining, cancelPlacement);
        return true;
    }
    if (c.building) {
        if (c.gate) {
            // W5 (Spec 08 §8.1 Platz 0): Tor verriegeln/entriegeln
            m.grid[0] = c.gateLocked ? AoeCommand{kAoeCmdGateLock, HudIcon::Wall, "Entriegeln", true, "", 'L', true}
                                     : AoeCommand{kAoeCmdGateLock, HudIcon::Wall, "Verriegeln", true, "", 'L'};
        }
        if (c.market) {
            // W7 (Spec 02 §2.7): obere Zeile Kaufen, zweite Zeile Verkaufen, je Nahrung, Holz, Stein
            static const HudIcon kGoodIcons[3] = {HudIcon::Sheep, HudIcon::Tree, HudIcon::MiningCamp};
            for (int i = 0; i < 3; ++i) {
                m.grid[static_cast<size_t>(i)] = {kAoeCmdMarket0 + i * 2, kGoodIcons[i], "Kauf " + std::to_string(c.marketBuy[i]),
                                                  c.marketCanBuy[i], c.marketCanBuy[i] ? "" : "zu wenig Gold"};
                m.grid[static_cast<size_t>(5 + i)] = {kAoeCmdMarket0 + i * 2 + 1, kGoodIcons[i], "Verk. " + std::to_string(c.marketSell[i]),
                                                      c.marketCanSell[i], c.marketCanSell[i] ? "" : "zu wenig Vorrat"};
            }
        }
        if (c.townCenter) {
            m.grid[10] = c.bell ? AoeCommand{kAoeCmdBell, HudIcon::Villager, "Zur Arbeit", true, "", 'B', true}
                                : AoeCommand{kAoeCmdBell, HudIcon::Age, "Stadtglocke", true, "", 'B'};
        }
        else {
            m.grid[10] = AoeCommand{};
        }
        m.grid[11] = AoeCommand{};
        m.grid[12] = del;
        m.grid[13] = c.garrisonMax > 0 ? AoeCommand{kAoeCmdUnload, HudIcon::Unload, "Alle raus", c.garrison > 0,
                                                    c.garrison > 0 ? "" : "niemand in der Garnison"}
                                       : AoeCommand{};
        m.grid[14] = aoeCancelCell(cancelTraining, cancelPlacement);
        return true;
    }
    return false;
}

// W4: Taste eines Feldes (eigene Taste, sonst die des Rasterplatzes) und ihr Text („Entf“, „Tab“, „Q“).
inline char aoeKeyOf(const AoeModel& m, int cell) {
    const char own = m.cell(cell).hotkey;
    if (own != 0) return own;
    // die Taste des Platzes gilt nicht, wenn ein anderes Feld sie als eigene Taste hat
    const char key = aoeHotkey(cell);
    for (int i = 0; i < kAoeGridCells; ++i)
        if (i != cell && m.cell(i).id != 0 && m.cell(i).hotkey == key) return 0;
    return key;
}
inline std::string aoeKeyText(char key) {
    if (key == kAoeKeyDelete) return "Entf";
    if (key == kAoeKeyTab) return "Tab";
    return key == 0 ? std::string{} : std::string(1, key);
}
// W4: Feld zu einer Taste – eigene Tasten zuerst, dann die Plätze, deren Taste kein anderes Feld beansprucht.
inline int aoeCellOfKey(const AoeModel& m, char key) {
    for (int i = 0; i < kAoeGridCells; ++i)
        if (m.cell(i).id != 0 && m.cell(i).hotkey == key) return i;
    const int cell = aoeCellOfHotkey(key);
    if (cell < 0 || m.cell(cell).hotkey != 0) return -1;
    return cell;
}

// Meldung anhängen (älteste fällt heraus); gleiche Meldung hintereinander nur einmal.
inline void aoePushMessage(AoeModel& m, const std::string& text, int kind, double now) {
    if (text.empty()) return;
    if (!m.messages.empty() && m.messages.back().text == text && now - m.messages.back().shownAt < 1.0) return;
    m.messages.push_back({text, kind, now});
    while (m.messages.size() > static_cast<size_t>(kAoeMessagesMax)) m.messages.erase(m.messages.begin());
}

// Abgelaufene Meldungen entfernen (nach kMsgSeconds + Fade). true = etwas entfernt.
inline bool aoeExpireMessages(AoeModel& m, double now, double seconds, double fade) {
    bool changed = false;
    while (!m.messages.empty() && now - m.messages.front().shownAt >= seconds + fade) {
        m.messages.erase(m.messages.begin());
        changed = true;
    }
    return changed;
}

// Deckkraft einer Meldung 0 … 1 (voll bis seconds, dann linear in fade auf 0).
inline float aoeMessageAlpha(const AoeMessage& msg, double now, double seconds, double fade) {
    const double age = now - msg.shownAt;
    if (age < seconds) return 1.0f;
    if (fade <= 0.0 || age >= seconds + fade) return 0.0f;
    return static_cast<float>(1.0 - (age - seconds) / fade);
}

}  // namespace agesxr
