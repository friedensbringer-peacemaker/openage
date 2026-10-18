// xr.ages — Kulturwahl (W1): Auswahl im Spielmenü „Neue Karte“ (Desktop und Quest) und in den Einstellungen der Quest
// (map_civ/ai_civ). Index 0 = Zufall, 1 … 18 = Kulturen in der Reihenfolge des Fork-Katalogs
// (libopenage/gamestate/catalog.h, prod::CIVS – der Fork-Controller prüft die Übereinstimmung). Kurznamen sind die
// nyan-Namen der PlayerSetup-Objekte im konvertierten Modpack. Rein, ohne Abhängigkeiten.
#pragma once

#include <string>

namespace agesxr {

constexpr int kCivChoiceCount = 19;  // Zufall + 18 Kulturen
constexpr int kCivRandom = 0;

constexpr const char* kCivChoiceNames[kCivChoiceCount] = {
    "random",   "Britons", "Franks",  "Goths",  "Teutons", "Japanese", "Chinese", "Byzantines", "Persians", "Saracens",
    "Turks",    "Vikings", "Mongols", "Celts",  "Spanish", "Aztecs",   "Mayans",  "Huns",       "Koreans",
};

constexpr const char* kCivChoiceLabels[kCivChoiceCount] = {
    "Zufall",  "Briten",   "Franken",  "Goten",  "Teutonen", "Japaner", "Chinesen", "Byzantiner", "Perser", "Sarazenen",
    "Türken",  "Wikinger", "Mongolen", "Kelten", "Spanier",  "Azteken", "Maya",     "Hunnen",     "Koreaner",
};

inline int civChoiceClamp(int i) { return i >= 0 && i < kCivChoiceCount ? i : kCivRandom; }
inline const char* civChoiceName(int i) { return kCivChoiceNames[civChoiceClamp(i)]; }
inline const char* civChoiceLabel(int i) { return kCivChoiceLabels[civChoiceClamp(i)]; }

// Kurzname („Britons“, „random“, leer) → Index (unbekannt/leer/„random“ = Zufall)
inline int civChoiceIndex(const std::string& name) {
    for (int i = 1; i < kCivChoiceCount; ++i)
        if (name == kCivChoiceNames[i]) return i;
    return kCivRandom;
}

}  // namespace agesxr
