// xr.ages — Datenmodell des VR-HUD (schmale Schnittstelle Engine → HUD). Reine Struktur ohne GL, ohne Engine-Typen.
//
// Die App füllt das Modell einmal pro Frame (oder wenn sich etwas ändert) und übergibt es VrHud::pixels(); das HUD
// zeichnet nur neu, was sich gegenüber dem gezeigten Stand geändert hat (Teil-Redraw je Leiste).
//
// Engine-Anbindung (später, Wirtschaft/Kampf im Fork): nur noch das Füllen, z. B.
//     const auto r = gamestate.get_resources(player);      // Wirtschaft
//     model.food = r.food; model.wood = r.wood; model.gold = r.gold; model.stone = r.stone;
//     model.population = r.population; model.populationMax = r.population_max;
//     model.gameSeconds = engine.gameSeconds();             // Spieluhr (Pause/Tempo schon eingerechnet)
//     model.selection = {count, "Dorfbewohner", HudIcon::Villager, hp, hpMax};   // Auswahl (Einzel: HP)
//     model.buttons = {{kCmdTrainVillager, HudIcon::Villager, "Dorfbewohner", canAfford}, …};  // ≤ 8 Befehle
//     model.status = "Nicht genug Holz"; model.statusKind = HudStatus::kWarn;     // Meldung
// Ein Klick auf einen Knopf liefert VrHud::pointer() als Knopfindex; die App gibt buttons[i].id an die Engine.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace agesxr {

// Vektor-Symbole des HUD (eigene Formen, keine Spielgrafik). Neue Symbole hinten anhängen.
enum class HudIcon : uint8_t {
    None,
    Food, Wood, Gold, Stone,  // Rohstoffe
    Population, Clock,         // Einwohner, Spielzeit
    Villager, House, Sword, Bow, Horse, Tower, Hammer, Stop,  // Auswahl und Befehle
    // AoE-Layout (xr_aoe_ui): weitere Gebäude, feste Aktionen, Schmuck (hudIconShape in xr_hud.cpp)
    Mill, LumberCamp, MiningCamp, Barracks, Range, Stable, Wall,
    Halt, Garrison, Unload, Cancel, Back, Emblem, Age, Sheep, Tree,
    Count,
};

enum class HudStatus : uint8_t { kInfo, kWarn, kGood };  // Plakette: Stein, Rot, Grün

struct HudButton {
    int id = 0;                    // Befehls-ID für die Engine (frei wählbar, eindeutig je Leiste)
    HudIcon icon = HudIcon::None;
    std::string label;             // Kurztext unter dem Symbol, z. B. „Dorfbewohner“, „Haus bauen“
    bool enabled = true;           // false = ausgegraut (z. B. nicht genug Holz), Klick wirkungslos
    bool operator==(const HudButton& o) const {
        return id == o.id && icon == o.icon && label == o.label && enabled == o.enabled;
    }
    bool operator!=(const HudButton& o) const { return !(*this == o); }
};

struct HudSelection {
    int count = 0;                 // 0 = nichts ausgewählt
    std::string kind;              // Art: „Dorfbewohner“, „Haus“, „gemischt“
    HudIcon icon = HudIcon::None;
    int hp = 0, hpMax = 0;         // Einzelauswahl: Trefferpunkte; hpMax ≤ 0 = kein Balken
    // Fortschritt (0.6.0-xr.0.7): Ausbildung/Bau 0 … 1 statt des HP-Balkens, < 0 = keiner; detail = Zeile darunter
    // („Dorfbewohner +1 · 45 %“, „Wartet auf ein Haus“, „Bau 30 %“), ersetzt dann die HP-Zahlen.
    float progress = -1.0f;
    std::string detail;
    bool operator==(const HudSelection& o) const {
        return count == o.count && kind == o.kind && icon == o.icon && hp == o.hp && hpMax == o.hpMax &&
               progress == o.progress && detail == o.detail;
    }
    bool operator!=(const HudSelection& o) const { return !(*this == o); }
};

struct HudModel {
    static constexpr int kMaxButtons = 8;

    // Ressourcenleiste
    int food = 0, wood = 0, gold = 0, stone = 0;
    int population = 0, populationMax = 0;  // „3/5“; populationMax 0 → nur die Zahl, < 0 → „3/–“ (Grenze unbekannt)
    double gameSeconds = 0.0;               // Spielzeit; angezeigt auf ganze Sekunden („12:34“, „1:02:03“)
    std::string status;                     // Statuszeile („Nicht genug Holz“, „Sieg!“); leer = keine Plakette
    HudStatus statusKind = HudStatus::kInfo;
    // Auswahlfeld und Befehlsleiste
    HudSelection selection;
    std::vector<HudButton> buttons;         // höchstens kMaxButtons, weitere werden nicht gezeigt

    int shownSecond() const { return std::isfinite(gameSeconds) && gameSeconds > 0.0 ? static_cast<int>(gameSeconds) : 0; }
    // Obere Leiste (Ressourcen, Einwohner, Zeit, Status) gleich wie gezeigt?
    bool sameTop(const HudModel& o) const {
        return food == o.food && wood == o.wood && gold == o.gold && stone == o.stone && population == o.population &&
               populationMax == o.populationMax && shownSecond() == o.shownSecond() && status == o.status &&
               statusKind == o.statusKind;
    }
    // Untere Leiste (Auswahl, Befehle) gleich wie gezeigt?
    bool sameBottom(const HudModel& o) const { return selection == o.selection && buttons == o.buttons; }
};

// Anzeigehilfen (Host-getestet).
// Spielzeit: „0:00“ … „59:59“, ab einer Stunde „1:02:03“; negativ/NaN → „0:00“.
inline std::string hudTimeText(double seconds) {
    const long t = std::isfinite(seconds) && seconds > 0.0 ? static_cast<long>(seconds) : 0L;
    char buf[32];
    if (t >= 3600) std::snprintf(buf, sizeof(buf), "%ld:%02ld:%02ld", t / 3600, t / 60 % 60, t % 60);
    else std::snprintf(buf, sizeof(buf), "%ld:%02ld", t / 60, t % 60);
    return buf;
}
// Rohstoffmenge: bis 99 999 ausgeschrieben, darüber „123k“; negativ → 0.
inline std::string hudAmountText(int v) {
    char buf[16];
    if (v < 0) v = 0;
    if (v >= 100000) std::snprintf(buf, sizeof(buf), "%dk", v / 1000);
    else std::snprintf(buf, sizeof(buf), "%d", v);
    return buf;
}
// Einwohner „3/5“, „3“ ohne Obergrenze, „3/–“ bei unbekannter Grenze (max < 0).
inline std::string hudPopulationText(int pop, int max) {
    char buf[32];
    if (pop < 0) pop = 0;
    if (max > 0) std::snprintf(buf, sizeof(buf), "%d/%d", pop, max);
    else if (max < 0) std::snprintf(buf, sizeof(buf), "%d/–", pop);
    else std::snprintf(buf, sizeof(buf), "%d", pop);
    return buf;
}

}  // namespace agesxr
