// xr.ages — Spieloberfläche im Spielbild (Kontextmenü, Spielmenü, Sieg-/Niederlage-Tafel) auf einer
// fenstergroßen CPU-Leinwand. GL-frei, ohne Engine-Typen; Optik wie VR-Menü und VR-HUD (xr_canvas.h).
//
// Verwendung (Desktop-Engine, Fork libopenage/ui/; die Quest-App zeichnet stattdessen VR-Menü und VR-HUD):
//   GameUi ui; ui.init(); ui.resize(w, h);
//   ui.openContext(x, y, {{1, "Hierher bewegen"}, …});      // Rechtsklick-Halten
//   ui.menu().open = true;                                    // Esc
//   ui.onMove(x, y); auto r = ui.onClick(x, y);              // Maus (Fensterpixel, Zeile 0 oben)
//   const uint32_t* px = ui.pixels(); if (ui.version() != shown) upload(px, ui.dirtyY0(), ui.dirtyY1());
// Die Modelle (context(), menu(), board()) sind reine Daten; pixels() zeichnet nur neu, wenn sich etwas geändert hat
// (Modelle, Hover, Größe) und meldet die betroffenen Zeilen für einen Teil-Upload.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "text_raster.h"
#include "xr_canvas.h"

namespace agesxr {

struct GameUiItem {
    int id = 0;
    std::string label;
    bool enabled = true;
    bool operator==(const GameUiItem& o) const { return id == o.id && label == o.label && enabled == o.enabled; }
    bool operator!=(const GameUiItem& o) const { return !(*this == o); }
};

// Kontextmenü am Zeiger (Rechtsklick-Halten auf eine Auswahl).
struct ContextMenuModel {
    bool open = false;
    int x = 0, y = 0;  // Zeigerposition beim Öffnen (Fensterpixel); das Menü bleibt im Bild
    std::string title;  // z. B. „3 Dorfbewohner“
    std::vector<GameUiItem> items;
    bool operator==(const ContextMenuModel& o) const {
        return open == o.open && x == o.x && y == o.y && title == o.title && items == o.items;
    }
    bool operator!=(const ContextMenuModel& o) const { return !(*this == o); }
};

// Spielstand-Slot der Slot-Liste (0.6.0-xr.0.11): Nummer (0 = automatisch), sichtbarer Text, belegt.
struct GameUiSlot {
    int slot = 0;
    std::string label;  // „Slot 2: Zufallskarte #3 · 12:34 · 2026-10-08 19:30“
    bool exists = false;
    bool operator==(const GameUiSlot& o) const { return slot == o.slot && label == o.label && exists == o.exists; }
    bool operator!=(const GameUiSlot& o) const { return !(*this == o); }
};

// Spielmenü (Esc/F10). Werte sind Indizes in die Tabellen unten; GameUi ändert sie bei Klicks auf ◀ ▶ selbst.
struct GameMenuModel {
    enum View { kViewMain, kViewSave, kViewLoad };
    bool open = false;
    int view = kViewMain;             // Hauptseite oder Slot-Liste zum Speichern/Laden
    std::vector<GameUiSlot> slots;    // Slot-Liste (von der Engine gefüllt)
    int confirmSlot = -1;             // Slot mit offener Rückfrage (Überschreiben/Verwerfen), −1 = keine
    bool paused = false;    // Nutzerpause (Anzeige des Kippschalters)
    int speed = 1;          // kGameSpeedLabels
    int biome = 0;          // kBiomeLabels
    int size = 1;           // kMapSizeTiles
    int opponent = 1;       // kOpponentLabels
    int seed = 1;           // Kartennummer 1 … 9999
    bool confirmQuit = false;
    std::string mapInfo;    // laufende Karte, Hinweiszeile unter dem Titel
    bool operator==(const GameMenuModel& o) const {
        return open == o.open && view == o.view && slots == o.slots && confirmSlot == o.confirmSlot &&
               paused == o.paused && speed == o.speed && biome == o.biome && size == o.size &&
               opponent == o.opponent && seed == o.seed && confirmQuit == o.confirmQuit && mapInfo == o.mapInfo;
    }
    bool operator!=(const GameMenuModel& o) const { return !(*this == o); }
};

// Tafel in der Bildmitte (Sieg/Niederlage, Spiel angehalten).
struct MatchBoardModel {
    bool open = false;
    bool good = false;  // Sieg (grün) statt Niederlage (rot)
    std::string headline;
    std::vector<std::string> lines;
    bool operator==(const MatchBoardModel& o) const {
        return open == o.open && good == o.good && headline == o.headline && lines == o.lines;
    }
    bool operator!=(const MatchBoardModel& o) const { return !(*this == o); }
};

constexpr int kGameSpeedCount = 4;
constexpr const char* kGameSpeedLabels[kGameSpeedCount] = {"0,5×", "1×", "2×", "4×"};
constexpr double kGameSpeedValues[kGameSpeedCount] = {0.5, 1.0, 2.0, 4.0};
constexpr int kGameUiBiomeCount = 12;  // Reihenfolge = map_biome_t der Engine
constexpr const char* kGameUiBiomeLabels[kGameUiBiomeCount] = {
    "Grasland", "Steppe", "Hügel", "Wald", "Flüsse", "Küste", "Binnenmeer", "Inseln", "Goldrausch", "Wüste", "Winter", "Dschungel"};
constexpr int kGameUiSizeCount = 3;
constexpr int kGameUiSizeTiles[kGameUiSizeCount] = {48, 64, 96};
constexpr int kGameUiOpponentCount = 3;
constexpr const char* kGameUiOpponentLabels[kGameUiOpponentCount] = {"Aus", "Leicht", "Normal"};
constexpr int kGameUiSeedMin = 1, kGameUiSeedMax = 9999;

class GameUi {
public:
    enum class Action {
        kNone,
        kContextItem,     // id = GameUiItem::id
        kContextClosed,   // Klick neben das Kontextmenü
        kResume,          // „Weiter“ (Menü schließt sich)
        kTogglePause,     // Pause-Kippschalter (menu().paused schon umgeschaltet)
        kSpeedChanged,    // menu().speed geändert
        kNewMap,          // „Neue Karte starten“ mit biome/size/opponent/seed
        kQuit,            // „Ja“ in der Beenden-Rückfrage
        kShowSlots,       // „Speichern …“/„Laden …“: Slot-Liste geöffnet (menu().view), Port füllt menu().slots
        kSave,            // in Slot id speichern (bei belegtem Slot nach der Rückfrage)
        kLoad,            // Slot id laden (nach der Rückfrage „laufendes Spiel verwerfen?“)
    };
    struct Result {
        Action action = Action::kNone;
        int id = -1;
    };

    bool init();  // Schriften; false = keine Schrift (Formen funktionieren trotzdem, Host-Tests)
    bool ok() const { return mOk; }
    void resize(int width, int height);
    int width() const { return mCanvas ? mCanvas->width() : 0; }
    int height() const { return mCanvas ? mCanvas->height() : 0; }

    ContextMenuModel& context() { return mContext; }
    GameMenuModel& menu() { return mMenu; }
    MatchBoardModel& board() { return mBoard; }
    const ContextMenuModel& context() const { return mContext; }
    const GameMenuModel& menu() const { return mMenu; }
    const MatchBoardModel& board() const { return mBoard; }

    void openContext(int x, int y, const std::string& title, std::vector<GameUiItem> items);
    void closeContext() { mContext = ContextMenuModel{}; }
    // Mausereignisse gehören der Oberfläche (nicht dem Spiel), solange ein Menü offen ist.
    bool wantsMouse() const { return mContext.open || mMenu.open; }
    bool anyOpen() const { return mContext.open || mMenu.open || mBoard.open; }

    void onMove(int x, int y);
    Result onClick(int x, int y);  // Linksklick (Flanke)

    // Pixel (RGBA, Zeile 0 oben, Fenstergröße). Zeichnet nur bei Änderungen neu; dann steigt version() und
    // dirtyY0()/dirtyY1() nennen die Zeilen, die neu hochgeladen werden müssen.
    const uint32_t* pixels();
    uint32_t version() const { return mVersion; }
    int dirtyY0() const { return mDirtyY0; }
    int dirtyY1() const { return mDirtyY1; }
    void invalidate() { mDirty = true; }

    // Layout (Fensterpixel) – für Tests und Treffer. Skaliert mit der Fenstergröße (scale()).
    float scale() const;
    struct Rect {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        bool contains(int x, int y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
        bool empty() const { return x1 <= x0 || y1 <= y0; }
    };
    Rect contextRect() const;
    Rect contextItemRect(int index) const;
    Rect menuRect() const;
    // Zeilen des Spielmenüs (Index = kRow*): Zeile, linker Pfeil, rechter Pfeil, Knopf „Ja“/„Nein“ der Rückfrage.
    enum Row { kRowResume = 0, kRowSave, kRowLoad, kRowPause, kRowSpeed, kRowBiome, kRowSize, kRowOpponent, kRowSeed,
               kRowNewMap, kRowQuit, kRowCount };
    // Slot-Liste: Zeile i = menu().slots[i], danach „Zurück“ (slotBackRow()).
    int slotBackRow() const { return static_cast<int>(mMenu.slots.size()); }
    bool slotEnabled(int index) const;  // Speichern: alle außer automatisch; Laden: nur belegte
    Rect menuRowRect(int row) const;
    Rect menuArrowRect(int row, bool right) const;
    Rect menuConfirmRect(bool yes) const;
    Rect boardRect() const;

private:
    enum class Hover { kNone, kContextItem, kMenuRow, kMenuArrowL, kMenuArrowR, kConfirmYes, kConfirmNo, kSlotRow, kSlotBack };
    struct HoverState {
        Hover kind = Hover::kNone;
        int index = -1;
        bool operator==(const HoverState& o) const { return kind == o.kind && index == o.index; }
        bool operator!=(const HoverState& o) const { return !(*this == o); }
    };
    HoverState hitTest(int x, int y) const;
    void drawAll();
    void drawContext();
    void drawMenu();
    void drawSlots();
    void drawBoard();
    void drawArrow(const Rect& r, bool right, bool hot);
    int rowY0(int row) const;
    int px(float v) const { return static_cast<int>(v * scale() + 0.5f); }
    Rect unionRect(const Rect& a, const Rect& b) const;
    Rect drawnRect() const;  // Vereinigung aller sichtbaren Elemente

    bool mOk = false;
    TextRaster mFont, mFontBold;
    std::unique_ptr<Canvas> mCanvas;
    ContextMenuModel mContext;
    GameMenuModel mMenu;
    MatchBoardModel mBoard;
    ContextMenuModel mShownContext;
    GameMenuModel mShownMenu;
    MatchBoardModel mShownBoard;
    HoverState mHover, mShownHover;
    Rect mShownRect;
    bool mDirty = true;
    uint32_t mVersion = 0;
    int mDirtyY0 = 0, mDirtyY1 = 0;
};

}  // namespace agesxr
