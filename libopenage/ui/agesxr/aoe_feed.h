// xr.ages — füllt das Modell der AoE-Oberfläche (native/xr/xr_aoe_model.h) aus einem HudSample (Engine::query_hud +
// Produktions-Schnappschuss), wie HudFeed das VR-HUD füllt. Rein, ohne GL/Engine-Bibliothek (nur der abhängigkeitsfreie
// Fork-Header engine/hud_info.h); host-getestet in tests/test_app_logic.cpp (testAoeFeed). Der Fork-Controller
// (libopenage/ui/aoe_ui_controller.cpp) ruft fill() alle 250 ms im Presenter-Thread auf – am Desktop und auf der Quest.
//
// Belegung des Befehlsrasters (Spec §1.2): Zeile 1–2 = Angebot der Engine (Ausbilden 101–199, Bauen 201–299, Forschen
// 301–499, W1) in der Reihenfolge der Engine, bei mehr als 10 Einträgen Seiten zu 9 + „Seite n/m“; Zeile 3 = feste Aktionen (aoeFixedRow: Angriff · Halt · Garnison · Alle raus ·
// Abbrechen); W4: Militär, Dorfbewohner und Gebäude belegen das Raster mit Kampfbefehlen (aoeCombatGrid).
#pragma once

#include <engine/hud_info.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "hud_feed.h"
#include "xr_aoe_layout.h"
#include "xr_aoe_model.h"

namespace agesxr {

class AoeFeed {
public:
    // Neue Engine: Meldungsnummern von vorn.
    void reset() {
        mLastSeq = 0;
        mPage = 0;
        mPages = 1;
        mPageKey.clear();
        mModel = AoeModel{};
        mMatch = HudMatch::RUNNING;
        mDeleteArmed = false;
    }

    void fill(const HudSample& s, double now) {
        AoeModel& m = mModel;
        const bool game = s.valid && s.info.game;
        const HudProdState& p = s.prod;
        m.gameSeconds = s.valid && std::isfinite(s.gameSeconds) && s.gameSeconds > 0.0 ? s.gameSeconds : 0.0;
        const std::array<double, 4>& r = game && p.valid ? p.resources : s.info.resources;
        m.food = amount(game ? r[0] : 0.0);
        m.wood = amount(game ? r[1] : 0.0);
        m.gold = amount(game ? r[2] : 0.0);
        m.stone = amount(game ? r[3] : 0.0);
        if (game && p.valid) {
            m.population = p.population;
            m.populationMax = p.populationCap > 0 ? p.populationCap : -1;
        } else {
            m.population = game && s.info.units ? static_cast<int>(*s.info.units) : 0;
            m.populationMax = -1;
        }
        m.housingAlert = game && p.valid && p.queue && p.waitingForHousing;
        // W1: Zeitalter des Spielers (Plakette I … IV und Text)
        m.ageIndex = game && p.valid ? std::clamp(p.age, 1, 4) : 1;
        m.age = game && p.valid && !p.ageLabel.empty() ? p.ageLabel : std::string{"Dunkles Zeitalter"};
        aoeExpireMessages(m, now, aoelayout::kMsgSeconds, aoelayout::kFadeSeconds);

        // Befehlsraster: Zeilen 1–2 = Angebot der Engine; mehr als 10 Einträge (W1: Gebäude + Forschung) → Seiten
        // zu 9 Einträgen, Feld 10 (G) = „Seite n/m“ (blättert weiter)
        m.grid.assign(static_cast<size_t>(kAoeGridCells), AoeCommand{});
        m.tips.assign(static_cast<size_t>(kAoeGridCells), std::string{});
        mPages = 1;
        if (game && p.valid) {
            const int n = static_cast<int>(p.options.size());
            const bool paged = n > kAoeOptionCells;
            const int per = paged ? kAoeOptionCells - 1 : kAoeOptionCells;
            mPages = paged ? (n + per - 1) / per : 1;
            const std::string key = p.selectionLabel + "|" + std::to_string(p.selectionCount);
            if (key != mPageKey) {
                mPageKey = key;
                mPage = 0;
            }
            if (mPage >= mPages) mPage = 0;
            int cell = 0;
            for (int i = mPage * per; i < n && cell < per; ++i) {
                const HudProdOption& o = p.options[static_cast<size_t>(i)];
                AoeCommand c;
                c.id = o.code;
                c.icon = aoeIconFor(o.code, o.icon);
                c.label = o.label;
                c.enabled = o.available;
                c.reason = o.reason;
                m.tips[static_cast<size_t>(cell)] = tipOf(o);
                m.grid[static_cast<size_t>(cell++)] = c;
            }
            if (paged) {
                AoeCommand c;
                c.id = kAoeCmdPageNext;
                c.icon = HudIcon::Next;
                c.label = "Seite " + std::to_string(mPage + 1) + "/" + std::to_string(mPages);
                m.tips[static_cast<size_t>(kAoeOptionCells - 1)] =
                    "Weitere Befehle: weiter zu Seite " + std::to_string((mPage + 1) % mPages + 1) + " von " + std::to_string(mPages);
                m.grid[static_cast<size_t>(kAoeOptionCells - 1)] = c;
            }
        }
        const bool cancelTraining = game && p.valid && p.queue && !p.queueItems.empty();
        const bool cancelPlacement = game && p.valid && !p.placement.empty();
        aoeFixedRow(m, cancelTraining, cancelPlacement);
        // W4: Befehle der Einheiten und Gebäude (Stellungen, Formationen, Patrouille, Garnison, Glocke)
        if (game && s.info.first && !s.info.first->neutral_object && s.info.first->alive) {
            const auto& e = *s.info.first;
            AoeCombat c;
            c.military = e.military;
            c.militaryCount = static_cast<int>(s.info.military_selected);
            c.stances = e.stances;
            c.formations = e.formations;
            c.stance = e.stance;
            c.formation = e.formation;
            c.villager = e.villager;
            c.building = e.building && !e.unit;
            c.townCenter = e.town_center;
            c.bell = e.bell;
            c.garrison = e.garrison;
            c.garrisonMax = e.garrison_max;
            c.deleteArmed = mDeleteArmed;
            c.gate = e.gate;  // W5: Tor verriegeln/entriegeln
            c.gateLocked = e.gate_locked;
            if (aoeCombatGrid(m, c, cancelTraining, cancelPlacement) && c.military && !c.villager && !c.building) {
                m.tips.assign(static_cast<size_t>(kAoeGridCells), std::string{});
            }
        }

        m.selection = game ? selectionOf(s) : AoeSelection{};
        // Bestellungen aller eigenen Gebäude (0.6.0-xr.0.11), Restzeit auf ganze Sekunden (weniger Neuzeichnen)
        m.orders.clear();
        if (game && p.valid) {
            for (const HudProdOrder& o : p.orders) {
                AoeOrder a;
                a.entity = o.entity;
                a.icon = o.construction ? aoeIconForName(o.label, true, false, false, false) : aoeIconFor(0, o.icon);
                a.label = o.label;
                a.building = o.building;
                a.count = o.count;
                a.remaining = std::isfinite(o.remaining) && o.remaining >= 0.0 ? std::ceil(static_cast<float>(o.remaining)) : -1.0f;
                a.progress = static_cast<float>(std::clamp(std::isfinite(o.progress) ? o.progress : 0.0, 0.0, 1.0));
                a.progress = std::round(a.progress * 50.0f) / 50.0f;  // 2-%-Schritte
                a.construction = o.construction;
                m.orders.push_back(a);
            }
        }

        // Meldungen der Produktion: Plakette oben (8 s) + Zeile über der Leiste
        if (game && p.valid && p.statusSeq != mLastSeq) {
            mLastSeq = p.statusSeq;
            if (!p.status.empty()) {
                m.plakette = p.status;
                m.plaketteKind = p.statusKind;
                m.plaketteAt = now;
                aoePushMessage(m, p.status, p.statusKind == HudStatus::kWarn ? 1 : 0, now);
            }
        }
        if (game && p.valid && !p.placement.empty() && m.plakette != HudFeed::kPlacementHint) {
            m.plakette = HudFeed::kPlacementHint;
            m.plaketteKind = HudStatus::kInfo;
            m.plaketteAt = now;
        }
        // Spielende
        if (game && s.info.match != HudMatch::RUNNING && mMatch == HudMatch::RUNNING) {
            mMatch = s.info.match;
            aoePushMessage(m, HudFeed::matchHeadline(mMatch), mMatch == HudMatch::DEFEAT ? 2 : 0, now);
        }
    }

    // Kampfwerte als Zeile („Angriff 3 · Rüstung 0/0 · Reichw. 0“); leer ohne Werte.
    static std::string statsLine(const openage::engine::HudEntity& e) {
        if (!e.has_stats || e.neutral_object) return {};
        char buf[96];
        const long range = std::lround(std::max(0.0, e.range));
        std::snprintf(buf, sizeof(buf), "Angriff %lld · Rüstung %lld/%lld · Reichw. %ld", static_cast<long long>(e.attack),
                      static_cast<long long>(e.armor_melee), static_cast<long long>(e.armor_pierce), range);
        return buf;
    }

    // Garnisonsplätze je Gebäude (S3: Felder ausgegraut, nur die Kapazität wird angezeigt).
    static int garrisonCapacity(const std::string& nyanName) {
        if (nyanName == "TownCenter") return 15;
        if (nyanName == "Castle") return 20;
        if (nyanName.find("Tower") != std::string::npos) return 5;
        return 0;
    }

    // W4: „Löschen“ wartet auf die Bestätigung (Quest: zweiter Druck löscht), wirkt beim nächsten fill().
    void setDeleteArmed(bool armed) { mDeleteArmed = armed; }
    bool deleteArmed() const { return mDeleteArmed; }

    // W1: nächste Seite des Befehlsrasters (Knopf kAoeCmdPageNext); wirkt beim nächsten fill().
    void nextPage() { mPage = mPages > 1 ? (mPage + 1) % mPages : 0; }
    int page() const { return mPage; }
    int pages() const { return mPages; }

    // W1: Zeile beim Zielen: „Webstuhl – 50 Gold · 25 s · Dorfbewohner: mehr LP und Rüstung“.
    static std::string tipOf(const HudProdOption& o) {
        static const char* kNames[4] = {"Nahrung", "Holz", "Gold", "Stein"};
        std::string costs;
        for (size_t i = 0; i < o.cost.size(); ++i) {
            if (!(o.cost[i] > 0.0)) continue;
            if (!costs.empty()) costs += ", ";
            costs += std::to_string(static_cast<long>(std::lround(o.cost[i]))) + " " + kNames[i];
        }
        std::string out = o.label;
        if (!costs.empty()) out += " – " + costs;
        if (o.time > 0.0) out += (costs.empty() ? " – " : " · ") + std::to_string(static_cast<long>(std::lround(o.time))) + " s";
        if (!o.effect.empty()) out += " · " + o.effect;
        return out;
    }

    const AoeModel& model() const { return mModel; }
    AoeModel& model() { return mModel; }
    HudMatch match() const { return mMatch; }

private:
    static int amount(double v) {
        if (!std::isfinite(v) || v <= 0.0) return 0;
        return v >= 2.0e9 ? 2000000000 : static_cast<int>(std::floor(v));
    }

    static HudIcon iconOf(const openage::engine::HudEntity& e, const std::string& name) {
        return aoeIconForName(name, e.building, e.villager, e.ranged, e.mounted);
    }

    static AoeSelection selectionOf(const HudSample& s) {
        AoeSelection sel;
        const HudProdState& p = s.prod;
        sel.count = static_cast<int>(s.info.selected.size());
        if (sel.count == 0 && p.valid) sel.count = p.selectionCount;
        if (sel.count <= 0) return AoeSelection{};
        if (s.info.first) {
            const auto& e = *s.info.first;
            const auto named = HudFeed::unitName(e);
            sel.name = named.first;
            sel.icon = iconOf(e, named.first);
            if (sel.count == 1 && e.alive && e.max_health > 0) {
                sel.hp = static_cast<int>(e.health);
                sel.hpMax = static_cast<int>(e.max_health);
            }
            if (sel.count == 1) {
                sel.stats = statsLine(e);
                // W4: Plätze und Insassen aus der Engine (Daten), sonst die bekannten Plätze
                sel.garrisonMax = e.building ? (e.garrison_max > 0 ? e.garrison_max : garrisonCapacity(e.name)) : 0;
                sel.garrison = e.garrison;
                sel.arrows = e.building ? e.arrows : 0;
                for (const auto& name : e.garrison_names) {
                    openage::engine::HudEntity g;
                    g.name = name;
                    g.unit = true;
                    g.villager = name.rfind("Villager", 0) == 0;
                    const auto gn = HudFeed::unitName(g);
                    sel.garrisonUnits.push_back({gn.first, aoeIconForName(gn.first, false, g.villager, name.find("Archer") != std::string::npos, false)});
                }
            }
        }
        if (p.valid && !p.selectionLabel.empty() && sel.count == 1) sel.name = p.selectionLabel;
        if (sel.count > 1) {
            // Mehrfachauswahl: Porträts aus den nyan-Namen, Kopfzeile „3 Dorfbewohner“ bzw. „7 Einheiten (gemischt)“
            bool same = true;
            std::string firstName;
            for (size_t i = 0; i < s.info.selected_names.size(); ++i) {
                openage::engine::HudEntity e;
                e.name = s.info.selected_names[i];
                e.unit = true;
                e.villager = e.name.rfind("Villager", 0) == 0;
                const auto named = HudFeed::unitName(e);
                std::string label = named.first;
                if (label == "Dorfbewohnerin") label = "Dorfbewohner";
                // W1: aufgewertete Linien heißen nach ihrer Stufe („Militia“ → „Schwertkämpfer“)
                for (const auto& [nyanName, upgraded] : p.unitLabels)
                    if (nyanName == e.name) label = upgraded;
                if (i == 0) firstName = label;
                same = same && label == firstName;
                const auto has = [&](const char* part) { return e.name.find(part) != std::string::npos; };
                sel.units.push_back({label, aoeIconForName(label, false, e.villager,
                                                           has("Archer") || has("Skirmisher") || has("Crossbow"),
                                                           has("Knight") || has("Scout") || has("Cavalry"))});
            }
            if (sel.units.empty()) sel.units.push_back({sel.name, sel.icon});
            sel.name = std::to_string(sel.count) + " " + (same && !firstName.empty() ? firstName : "Einheiten (gemischt)");
            return sel;
        }
        if (p.valid && p.queue && !p.queueItems.empty()) {
            for (size_t i = 0; i < p.queueItems.size(); ++i) {
                // Symbol-Hinweis der Engine (0.6.0-xr.0.11), sonst nach dem Namen
                const std::string& q = p.queueItems[i];
                const bool hint = i < p.queueIcons.size() && !p.queueIcons[i].empty();
                sel.queue.push_back({q, hint ? aoeIconFor(0, p.queueIcons[i]) : aoeIconForName(q, false, false, false, false)});
            }
            sel.queueProgress = static_cast<float>(std::clamp(p.queueProgress, 0.0, 1.0));
            if (p.waitingForHousing) sel.detail = p.queueItems.front() + " – wartet auf ein Haus";
        } else if (p.valid && p.construction >= 0.0) {
            const double f = std::clamp(p.construction, 0.0, 1.0);
            sel.progress = static_cast<float>(f);
            char buf[48];
            std::snprintf(buf, sizeof(buf), "Bau %d %%", static_cast<int>(std::lround(100.0 * f)));
            sel.detail = buf;
        }
        // W3: Vorrat von Feld, Tier, Kadaver, Fisch (auch Gaia: Schaf, Hirsch, Baum, Mine)
        if (sel.detail.empty() && p.valid && !p.selectionResource.empty()) sel.detail = p.selectionResource;
        return sel;
    }

    AoeModel mModel;
    uint64_t mLastSeq = 0;
    int mPage = 0, mPages = 1;   // W1: Seite des Befehlsrasters
    std::string mPageKey;        // Auswahl, für die die Seite gilt (neue Auswahl = Seite 1)
    bool mDeleteArmed = false;   // W4: Löschen-Rückfrage (Quest)
    HudMatch mMatch = HudMatch::RUNNING;
};

}  // namespace agesxr
