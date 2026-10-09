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

#include "xr_game_ui.h"
#include "xr_hud_model.h"

namespace agesxr {

// Feste Befehle des Rasters neben den Produktions-Codes der Engine (101–106 Ausbilden, 201–207 Bauen).
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
// Rückkanal-IDs (UiFeedback) der Warteschlangenfelder
constexpr int kAoeCmdQueueBase = 1000;

constexpr int kAoeGridCells = 15;
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
    bool operator==(const AoeCommand& o) const {
        return id == o.id && icon == o.icon && label == o.label && enabled == o.enabled && reason == o.reason;
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
    int garrison = 0, garrisonMax = 0;  // Garnison (S3: Felder, bis dahin ausgegraut)
    std::vector<AoeSlot> units;    // Mehrfachauswahl: Porträts (gezeigt kAoeMultiShown + „+n“)
    std::string blocked;           // Sperrgrund-Zeile („Haus: nicht genug Holz (30)“)
    bool multi() const { return count > 1 && !units.empty(); }
    bool operator==(const AoeSelection& o) const {
        return count == o.count && name == o.name && icon == o.icon && hp == o.hp && hpMax == o.hpMax &&
               progress == o.progress && detail == o.detail && stats == o.stats && queue == o.queue &&
               queueProgress == o.queueProgress && garrison == o.garrison && garrisonMax == o.garrisonMax &&
               units == o.units && blocked == o.blocked;
    }
    bool operator!=(const AoeSelection& o) const { return !(*this == o); }
};

// Minimap-Platzhalter (S3: echte Karte): Grundfarbe je Landschaft, Kameraausschnitt in Kartenanteilen 0 … 1.
struct AoeMinimap {
    int biome = 0;                      // kGameUiBiomeLabels-Index
    int tiles = 64;                     // Kantenlänge
    float camX = 0.5f, camY = 0.5f;     // Mitte des Kamerarahmens (0 … 1)
    float camW = 0.25f, camH = 0.15f;   // Breite/Höhe des Rahmens (0 … 1)
    bool operator==(const AoeMinimap& o) const {
        return biome == o.biome && tiles == o.tiles && camX == o.camX && camY == o.camY && camW == o.camW && camH == o.camH;
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
    std::vector<AoeSaveSlot> slots; // Spielstände (Seiten kSave/kLoad)
    double armedAt = 0.0;           // Zeit des Scharfschaltens (Frist kConfirmSeconds)
    // Einstellungen
    int speed = 1;                  // kGameSpeedLabels
    bool labels = true;             // Beschriftung der Knöpfe
    bool hotkeys = true;            // Hotkey-Buchstaben zeigen
    bool messages = true;           // Meldungszeilen
    // Neue Karte
    int biome = 0, size = 1, opponent = 1, seed = 1;
    // Spielstand (0.6.0-xr.0.11, anderer Agent): verfügbar → Knöpfe aktiv, sonst „Speichern (folgt)“
    bool saveAvailable = false;
    bool loadAvailable = false;
    bool quest = false;             // Hinweis „App beenden: VR-Menü“ nur auf der Quest
    std::string mapInfo;            // „Karte #17 · Grasland 64 × 64 · 12:34 · Spiel angehalten“
    bool operator==(const AoeMenuModel& o) const {
        return open == o.open && page == o.page && confirmSurrender == o.confirmSurrender &&
               confirmSlot == o.confirmSlot && slots == o.slots && speed == o.speed &&
               labels == o.labels && hotkeys == o.hotkeys && messages == o.messages && biome == o.biome &&
               size == o.size && opponent == o.opponent && seed == o.seed && saveAvailable == o.saveAvailable &&
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
    std::string age = "Dunkles Zeitalter";  // fest bis Zeitalter (S3)
    int ageIndex = 1;                       // Plakette „I“ … „IV“
    std::string plakette;                   // Meldungsplakette (letzte Meldung, kPlaketteSeconds)
    HudStatus plaketteKind = HudStatus::kInfo;
    double plaketteAt = 0.0;
    // untere Leiste
    std::vector<AoeCommand> grid;           // kAoeGridCells (kürzer = Rest leer)
    AoeSelection selection;
    AoeMinimap minimap;
    std::vector<AoeMessage> messages;       // älteste zuerst, höchstens kAoeMessagesMax
    std::vector<AoeOrder> orders;           // Bestellungen aller eigenen Gebäude (nach Restzeit), Auswahlfeld ohne Auswahl

    int shownSecond() const { return gameSeconds > 0.0 ? static_cast<int>(gameSeconds) : 0; }
    const AoeCommand& cell(int i) const {
        static const AoeCommand kEmpty;
        return i >= 0 && i < static_cast<int>(grid.size()) ? grid[static_cast<size_t>(i)] : kEmpty;
    }
    bool sameTop(const AoeModel& o) const {
        return food == o.food && wood == o.wood && gold == o.gold && stone == o.stone && population == o.population &&
               populationMax == o.populationMax && housingAlert == o.housingAlert && shownSecond() == o.shownSecond() &&
               paused == o.paused && age == o.age && ageIndex == o.ageIndex && plakette == o.plakette &&
               plaketteKind == o.plaketteKind && plaketteAt == o.plaketteAt;
    }
    bool sameBar(const AoeModel& o) const {
        return grid == o.grid && selection == o.selection && minimap == o.minimap && orders == o.orders;
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
    default: break;
    }
    if (hint == "villager") return HudIcon::Villager;
    if (hint == "house") return HudIcon::House;
    if (hint == "sword") return HudIcon::Sword;
    if (hint == "bow") return HudIcon::Bow;
    if (hint == "horse") return HudIcon::Horse;
    if (hint == "tower") return HudIcon::Tower;
    return HudIcon::Hammer;
}

// Symbol zu einem deutschen Namen der Auswahl (Porträt, Warteschlange).
inline HudIcon aoeIconForName(const std::string& name, bool building, bool villager, bool ranged, bool mounted) {
    if (villager || name.rfind("Dorfbewohner", 0) == 0) return HudIcon::Villager;
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

// Zeile 3 des Rasters: feste Aktionen (Angriff · Halt · Garnison · Alle ausladen · Abbrechen).
inline void aoeFixedRow(AoeModel& m, bool cancelTraining, bool cancelPlacement) {
    if (m.grid.size() < static_cast<size_t>(kAoeGridCells)) m.grid.resize(static_cast<size_t>(kAoeGridCells));
    const int base = 10;
    m.grid[base + 0] = {kAoeCmdAttack, HudIcon::Sword, "Angriff", false, "kommt mit S3"};
    m.grid[base + 1] = {kAoeCmdHalt, HudIcon::Halt, "Halt", false, "kommt mit S3"};
    m.grid[base + 2] = {kAoeCmdGarrison, HudIcon::Garrison, "Garnison", false, "kommt mit S3"};
    m.grid[base + 3] = {kAoeCmdUnload, HudIcon::Unload, "Alle raus", false, "kommt mit S3"};
    if (cancelPlacement) m.grid[base + 4] = {kAoeCmdCancelPlacement, HudIcon::Cancel, "Abbrechen", true, ""};
    else if (cancelTraining) m.grid[base + 4] = {kAoeCmdCancelTraining, HudIcon::Cancel, "Abbrechen", true, ""};
    else m.grid[base + 4] = {kAoeCmdCancelTraining, HudIcon::Cancel, "Abbrechen", false, "nichts abzubrechen"};
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
