// xr.ages — füllt das HUD-Modell (native/xr/xr_hud_model.h) und verarbeitet Klicks auf die Befehlsknöpfe.
// Rein, ohne GL/OpenXR/Engine-Bibliothek (nur der abhängigkeitsfreie Fork-Header engine/hud_info.h); Host-getestet
// mit einer Mock-Engine (Folgen von HudSample) in tests/test_app_logic.cpp.
//
// Engine-Anbindung (0.6.0-xr.0.7): EngineHost fragt im eigenen Abfrage-Thread alle 250 ms die Engine ab
// (Engine::query_hud – Rohstoffe, eigene Einheiten, Auswahl mit HP, Spielzustand – und den Produktions-Schnappschuss
// prod::Production::snapshot – Einwohner/Grenze, Angebot, Warteschlange, Bauplatz-Modus, Meldungen) und legt das
// Ergebnis als HudSample ab; die XR-Schicht holt es je Frame (nur ein kurzer Mutex, nie ein Engine-Lock) und gibt
// es fill(). Klicks auf Knöpfe gehen über setCommandHandler an EngineHost::hudCommand (Produktion: Codes 101–106
// Ausbilden, 201–207 Bauplatz; kHudCmdCancel* = Warteschlange/Bauplatz abbrechen).
// Testbild-Modus (ohne Engine): fillPlaceholder() mit Beispiel-Befehlen wie in 0.6.0-xr.0.6.
#pragma once

#include <engine/hud_info.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "engine_status.h"
#include "xr_hud_model.h"

namespace agesxr {

// Befehls-IDs der Platzhalter-Knöpfe (Testbild-Modus).
enum HudCommand {
    kHudCmdVillager = 1,
    kHudCmdHouse,
    kHudCmdTower,
    kHudCmdAttack,
    kHudCmdStop,
};

// Eigene Befehle neben den Produktions-Codes der Engine (101–106, 201–207).
constexpr int kHudCmdCancelTraining = 900;   // letzte Einheit der Warteschlange abbrechen (Kosten zurück)
constexpr int kHudCmdCancelPlacement = 901;  // Bauplatz-Modus verlassen

// Produktions-Schnappschuss ohne Engine-Typen (EngineHost übersetzt prod::Snapshot hierher).
struct HudProdOption {
    int code = 0;           // für EngineHost::hudCommand
    std::string label;      // „Dorfbewohner“, „Haus“
    std::string icon;       // „villager“, „house“, „sword“, „bow“, „horse“, „hammer“, „tower“
    bool available = true;  // false = ausgegraut (reason sagt warum)
    std::string reason;
};

struct HudProdState {
    bool valid = false;                     // Produktions-Schnittstelle vorhanden
    std::array<double, 4> resources{};      // Nahrung, Holz, Gold, Stein
    int population = 0, populationCap = 0;  // Cap 0 = unbekannt
    int selectionCount = 0;
    std::string selectionLabel;             // deutscher Name der ersten gewählten Entity
    double construction = -1.0;             // Baufortschritt eines gewählten Fundaments 0 … 1, < 0 = keiner
    std::vector<HudProdOption> options;     // was die Auswahl ausbilden/bauen kann
    bool queue = false;                     // gewähltes Gebäude hat eine Warteschlange
    std::vector<std::string> queueItems;    // deutsche Namen, erstes = in Arbeit
    double queueProgress = 0.0;             // 0 … 1 des ersten Eintrags
    bool waitingForHousing = false;
    std::string placement;                  // Gebäude im Bauplatz-Modus (leer = keiner)
    std::string status;                     // letzte Meldung der Produktion
    HudStatus statusKind = HudStatus::kInfo;
    uint64_t statusSeq = 0;                 // zählt hoch; neue Nummer = neue Meldung
};

// Ein Abfrageergebnis (EngineHost::hudSample).
struct HudSample {
    bool valid = false;        // Engine läuft (sonst alles leer)
    double gameSeconds = 0.0;  // Spieluhr der Engine
    openage::engine::HudInfo info;
    HudProdState prod;
};

using HudMatch = openage::engine::hud_match_t;

class HudFeed {
public:
    using CommandHandler = std::function<void(int id, const std::string& label)>;
    static constexpr double kStatusSeconds = 3.0;
    static constexpr const char* kIdleStatus = "Wirtschaft folgt";  // nur Platzhalter (Testbild-Modus)
    static constexpr const char* kPlacementHint = "Bauplatz wählen – Trigger setzt, B bricht ab";
    static constexpr const char* kLoadingStatus = "Spiel wird aufgebaut …";

    // Engine-Anbindung: Befehl an die Engine geben (nil = Platzhalter-Meldung in der Statuszeile).
    void setCommandHandler(CommandHandler h) { mHandler = std::move(h); }

    // Neue Engine (Kartenwechsel): Meldungsnummern und Spielzustand beginnen von vorn.
    void reset() {
        mLastSeq = 0;
        mMatch = HudMatch::RUNNING;
        mStatusUntil = 0.0;
        mModel = HudModel{};
    }

    // Platzhalter-Daten (Testbild-Modus ohne Engine). gameSeconds = Spieluhr.
    void fillPlaceholder(double gameSeconds, double now) {
        mModel.food = mModel.wood = mModel.gold = mModel.stone = 0;
        mModel.population = 0;
        mModel.populationMax = 0;
        mModel.gameSeconds = gameSeconds;
        mModel.selection = HudSelection{};
        if (mModel.buttons.empty()) mModel.buttons = placeholderButtons();
        if (now >= mStatusUntil) {
            mModel.status = kIdleStatus;
            mModel.statusKind = HudStatus::kInfo;
        }
    }

    // Werte der Engine (HudSample aus EngineHost) ins Modell.
    void fill(const HudSample& s, double now) {
        const bool game = s.valid && s.info.game;
        mModel.gameSeconds = s.valid && std::isfinite(s.gameSeconds) && s.gameSeconds > 0.0 ? s.gameSeconds : 0.0;
        const HudProdState& p = s.prod;
        const std::array<double, 4>& r = game && p.valid ? p.resources : s.info.resources;
        mModel.food = amount(game ? r[0] : 0.0);
        mModel.wood = amount(game ? r[1] : 0.0);
        mModel.gold = amount(game ? r[2] : 0.0);
        mModel.stone = amount(game ? r[3] : 0.0);
        // Einwohner: aus der Produktion (mit Grenze), sonst eigene Einheiten aus dem Kampf, Grenze „–“.
        if (game && p.valid) {
            mModel.population = p.population;
            mModel.populationMax = p.populationCap > 0 ? p.populationCap : -1;
        } else {
            mModel.population = game && s.info.units ? static_cast<int>(*s.info.units) : 0;
            mModel.populationMax = -1;
        }
        mModel.selection = game ? selectionOf(s) : HudSelection{};
        mModel.buttons = game ? buttonsOf(p) : std::vector<HudButton>{};

        // Spielzustand (nur Wechsel zählen; nach Sieg/Niederlage bleibt es dabei, bis reset()).
        if (game && s.info.match != HudMatch::RUNNING) mMatch = s.info.match;
        // Neue Meldung der Produktion → 3 s
        if (game && p.valid && p.statusSeq != mLastSeq) {
            mLastSeq = p.statusSeq;
            if (!p.status.empty()) setStatus(p.status, p.statusKind, now);
        }
        if (mMatch != HudMatch::RUNNING) {
            mModel.status = matchHeadline(mMatch);
            mModel.statusKind = mMatch == HudMatch::VICTORY ? HudStatus::kGood
                                : mMatch == HudMatch::DEFEAT ? HudStatus::kWarn
                                                             : HudStatus::kInfo;
        } else if (now >= mStatusUntil) {
            if (!game) mModel.status = s.valid ? kLoadingStatus : "";
            else if (p.valid && !p.placement.empty()) mModel.status = kPlacementHint;
            else mModel.status.clear();
            mModel.statusKind = HudStatus::kInfo;
        }
    }

    // Klick auf Knopf index (VrHud::pointer). Gibt die Befehls-ID zurück (−1 = ungültig/ausgegraut).
    int click(int index, double now) {
        if (index < 0 || index >= static_cast<int>(mModel.buttons.size())) return -1;
        const HudButton& b = mModel.buttons[static_cast<size_t>(index)];
        if (!b.enabled) return -1;
        if (mHandler) mHandler(b.id, b.label);
        else setStatus(b.label + ": folgt mit der Wirtschaft", HudStatus::kInfo, now);
        return b.id;
    }

    // Meldung für kStatusSeconds (danach zurück auf die Ruhemeldung).
    void setStatus(const std::string& text, HudStatus kind, double now, double seconds = kStatusSeconds) {
        mModel.status = text;
        mModel.statusKind = kind;
        mStatusUntil = now + seconds;
    }

    HudMatch match() const { return mMatch; }
    bool matchOver() const { return mMatch != HudMatch::RUNNING; }
    const HudModel& model() const { return mModel; }
    HudModel& model() { return mModel; }

    static std::vector<HudButton> placeholderButtons() {
        return {{kHudCmdVillager, HudIcon::Villager, "Dorfbewohner", true},
                {kHudCmdHouse, HudIcon::House, "Haus bauen", true},
                {kHudCmdTower, HudIcon::Tower, "Turm bauen", true},
                {kHudCmdAttack, HudIcon::Sword, "Angriff", true},
                {kHudCmdStop, HudIcon::Stop, "Halt", true}};
    }

    static const char* matchHeadline(HudMatch m) {
        return m == HudMatch::VICTORY ? "Sieg!" : m == HudMatch::DEFEAT ? "Niederlage" : m == HudMatch::DRAW ? "Unentschieden" : "";
    }

    // Zeilen der Meldung im Stil des VR-Ladebildschirms (Überschrift = matchHeadline groß).
    static std::vector<LoaderLine> matchLines(HudMatch m, double decidedAt, bool newMapAvailable) {
        std::vector<LoaderLine> out;
        out.push_back({0, m == HudMatch::VICTORY  ? "Alle gegnerischen Einheiten und Gebäude sind besiegt."
                          : m == HudMatch::DEFEAT ? "Keine eigenen Einheiten oder Gebäude mehr."
                                                  : "Niemand ist übrig geblieben."});
        out.push_back({5, "Entschieden nach " + hudTimeText(decidedAt) + " Spielzeit – das Spiel ist angehalten."});
        out.push_back({5, newMapAvailable ? "Neues Spiel: Menütaste → Spiel → Neue Karte"
                                          : "Neues Spiel: Menütaste → Spiel → Karte wechseln"});
        out.push_back({5, "Beenden: Menütaste → Beenden"});
        return out;
    }

    // nyan-Kurzname („Villager“, „TownCenter“) → deutscher Name und Symbol; unbekannte Namen in Wörtern
    // („SiegeWorkshop“ → „Siege Workshop“).
    static std::pair<std::string, HudIcon> unitName(const openage::engine::HudEntity& e) {
        struct Entry {
            const char* id;
            const char* name;
        };
        static const Entry kNames[] = {
            {"Villager", "Dorfbewohner"}, {"VillagerMale", "Dorfbewohner"}, {"VillagerFemale", "Dorfbewohnerin"},
            {"Militia", "Milizsoldat"}, {"ManAtArms", "Schwertkämpfer"}, {"Spearman", "Speerträger"},
            {"Archer", "Bogenschütze"}, {"Skirmisher", "Plänkler"}, {"Knight", "Ritter"},
            {"ScoutCavalry", "Späher"}, {"Monk", "Mönch"}, {"TradeCart", "Handelskarren"},
            {"TownCenter", "Dorfzentrum"}, {"House", "Haus"}, {"Barracks", "Kaserne"},
            {"ArcheryRange", "Schießanlage"}, {"Stable", "Stall"}, {"Mill", "Mühle"},
            {"LumberCamp", "Holzfällerlager"}, {"MiningCamp", "Bergbaulager"}, {"Farm", "Farm"},
            {"Market", "Markt"}, {"Blacksmith", "Schmiede"}, {"Dock", "Hafen"}, {"Castle", "Burg"},
            {"Monastery", "Kloster"}, {"University", "Universität"}, {"Outpost", "Außenposten"},
            {"Tower", "Wachturm"}, {"WatchTower", "Wachturm"}, {"PalisadeWall", "Palisade"},
            {"StoneWall", "Steinmauer"}, {"Sheep", "Schaf"}, {"Turkey", "Truthahn"}, {"Deer", "Hirsch"},
            {"Boar", "Wildschwein"},
        };
        std::string name;
        for (const Entry& n : kNames)
            if (e.name == n.id) name = n.name;
        if (name.empty()) {
            for (size_t i = 0; i < e.name.size(); ++i) {
                const char c = e.name[i];
                if (i > 0 && c >= 'A' && c <= 'Z') name += ' ';
                name += c;
            }
            if (name.empty()) name = e.building ? "Gebäude" : "Einheit";
        }
        HudIcon icon = HudIcon::None;
        const auto has = [&](const char* part) { return e.name.find(part) != std::string::npos; };
        if (e.neutral_object) icon = HudIcon::None;
        else if (e.villager) icon = HudIcon::Villager;
        else if (e.building) icon = has("Tower") || has("Castle") || has("Outpost") ? HudIcon::Tower : HudIcon::House;
        else if (e.ranged) icon = HudIcon::Bow;
        else if (has("Knight") || has("Cavalry") || has("Scout") || has("Camel") || has("Cataphract") ||
                 has("Rider") || has("Elephant"))
            icon = HudIcon::Horse;
        else if (e.unit) icon = HudIcon::Sword;
        return {name, icon};
    }

    // Symbol-Hinweis der Produktion → HUD-Symbol (unbekannt = Hammer).
    static HudIcon iconFor(const std::string& hint) {
        if (hint == "villager") return HudIcon::Villager;
        if (hint == "house") return HudIcon::House;
        if (hint == "sword") return HudIcon::Sword;
        if (hint == "bow") return HudIcon::Bow;
        if (hint == "horse") return HudIcon::Horse;
        if (hint == "tower") return HudIcon::Tower;
        return HudIcon::Hammer;
    }

private:
    static int amount(double v) {
        if (!std::isfinite(v) || v <= 0.0) return 0;
        return v >= 2.0e9 ? 2000000000 : static_cast<int>(std::floor(v));
    }

    static HudSelection selectionOf(const HudSample& s) {
        HudSelection sel;
        const HudProdState& p = s.prod;
        sel.count = static_cast<int>(s.info.selected.size());
        if (sel.count == 0 && p.valid) sel.count = p.selectionCount;
        if (sel.count <= 0) return HudSelection{};
        if (s.info.first) {
            const auto named = unitName(*s.info.first);
            sel.kind = named.first;
            sel.icon = named.second;
            if (sel.count == 1 && s.info.first->alive && s.info.first->max_health > 0) {
                sel.hp = static_cast<int>(s.info.first->health);
                sel.hpMax = static_cast<int>(s.info.first->max_health);
            }
        }
        if (p.valid && !p.selectionLabel.empty()) sel.kind = p.selectionLabel;
        if (p.valid && p.queue && !p.queueItems.empty()) {
            sel.progress = static_cast<float>(std::clamp(p.queueProgress, 0.0, 1.0));
            char buf[160];
            const int more = static_cast<int>(p.queueItems.size()) - 1;
            if (p.waitingForHousing)
                std::snprintf(buf, sizeof(buf), "%s%s – wartet auf ein Haus", p.queueItems[0].c_str(),
                              more > 0 ? (" +" + std::to_string(more)).c_str() : "");
            else
                std::snprintf(buf, sizeof(buf), "%s%s · %d %%", p.queueItems[0].c_str(),
                              more > 0 ? (" +" + std::to_string(more)).c_str() : "",
                              static_cast<int>(std::lround(100.0 * std::clamp(p.queueProgress, 0.0, 1.0))));
            sel.detail = buf;
        } else if (p.valid && p.construction >= 0.0) {
            const double f = std::clamp(p.construction, 0.0, 1.0);
            sel.progress = static_cast<float>(f);
            char buf[48];
            std::snprintf(buf, sizeof(buf), "Bau %d %%", static_cast<int>(std::lround(100.0 * f)));
            sel.detail = buf;
        }
        return sel;
    }

    static std::vector<HudButton> buttonsOf(const HudProdState& p) {
        std::vector<HudButton> out;
        if (!p.valid) return out;
        const bool placing = !p.placement.empty();
        const bool cancelTraining = !placing && p.queue && !p.queueItems.empty();
        const size_t room = static_cast<size_t>(HudModel::kMaxButtons) - (placing || cancelTraining ? 1 : 0);
        for (const HudProdOption& o : p.options) {
            if (out.size() >= room) break;
            out.push_back({o.code, iconFor(o.icon), o.label, o.available});
        }
        if (placing) out.push_back({kHudCmdCancelPlacement, HudIcon::Stop, "Bau abbrechen", true});
        else if (cancelTraining) out.push_back({kHudCmdCancelTraining, HudIcon::Stop, "Abbrechen", true});
        return out;
    }

    HudModel mModel;
    CommandHandler mHandler;
    double mStatusUntil = 0.0;
    uint64_t mLastSeq = 0;
    HudMatch mMatch = HudMatch::RUNNING;
};

}  // namespace agesxr
