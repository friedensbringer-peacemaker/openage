// xr.ages — füllt das Modell der AoE-Oberfläche (native/xr/xr_aoe_model.h) aus einem HudSample (Engine::query_hud +
// Produktions-Schnappschuss), wie HudFeed das VR-HUD füllt. Rein, ohne GL/Engine-Bibliothek (nur der abhängigkeitsfreie
// Fork-Header engine/hud_info.h); host-getestet in tests/test_app_logic.cpp (testAoeFeed). Der Fork-Controller
// (libopenage/ui/aoe_ui_controller.cpp) ruft fill() alle 250 ms im Presenter-Thread auf – am Desktop und auf der Quest.
//
// Belegung des Befehlsrasters (Spec §1.2): Zeile 1–2 = Angebot der Engine (Ausbilden 101–106, Bauen 201–207) in
// der Reihenfolge der Engine, Zeile 3 = feste Aktionen (aoeFixedRow: Angriff · Halt · Garnison · Alle raus ·
// Abbrechen; die ersten vier bis S3 gesperrt mit Grund).
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
        mModel = AoeModel{};
        mMatch = HudMatch::RUNNING;
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
        aoeExpireMessages(m, now, aoelayout::kMsgSeconds, aoelayout::kFadeSeconds);

        // Befehlsraster
        m.grid.assign(static_cast<size_t>(kAoeGridCells), AoeCommand{});
        if (game && p.valid) {
            int cell = 0;
            for (const HudProdOption& o : p.options) {
                if (cell >= 10) break;  // Zeilen 1–2
                AoeCommand c;
                c.id = o.code;
                c.icon = aoeIconFor(o.code, o.icon);
                c.label = o.label;
                c.enabled = o.available;
                c.reason = o.reason;
                m.grid[static_cast<size_t>(cell++)] = c;
            }
        }
        aoeFixedRow(m, game && p.valid && p.queue && !p.queueItems.empty(), game && p.valid && !p.placement.empty());

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
                sel.garrisonMax = e.building ? garrisonCapacity(e.name) : 0;
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
        return sel;
    }

    AoeModel mModel;
    uint64_t mLastSeq = 0;
    HudMatch mMatch = HudMatch::RUNNING;
};

}  // namespace agesxr
