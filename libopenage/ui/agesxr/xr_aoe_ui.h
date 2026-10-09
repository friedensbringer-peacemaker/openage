// xr.ages — Spieloberfläche im AoE-II-Stil auf einer fenstergroßen CPU-Leinwand (docs/UI-SPEC-AOE.md, Stufe S1):
// obere Leiste (Rohstoffe, Einwohner, Zeitalter, Meldungsplakette, Uhr, Knopf „Menü“), untere Leiste auf dem Spielbild
// (Befehlsraster 5 × 3 links, Auswahlfeld mittig, Minimap-Platzhalter rechts), Meldungszeilen, Kontextmenü am Zeiger,
// Spielmenü mit Unterseiten (Einstellungen, Neue Karte, Partie aufgeben mit 2-Klick-Bestätigung), Sieg-/Niederlage-Tafel.
// GL-frei, ohne Engine-Typen; Optik über SkinProvider (heute VectorSkin). Desktop und Quest zeichnen dieselbe Oberfläche
// im Engine-Bild (Fork libopenage/ui/aoe_ui_controller.*); die XR-Schicht bekommt nur UiFeedback (Haptik, Fokusmodus).
//
//   AoeUi ui; ui.init(); ui.resize(w, h);
//   ui.model() = …;  ui.openContext(x, y, "3 Dorfbewohner", items);  ui.openMenu(true, now);
//   ui.onMove(x, y); auto r = ui.onClick(x, y, now); r = ui.onKey('Q', now); r = ui.navigate(dx, dy, now);
//   const uint32_t* px = ui.pixels(now); if (ui.version() != shown) for (band : ui.lastBands()) upload(px, band);
// Teil-Redraw (P7): obere Leiste, Meldungen, untere Leiste, Kontextmenü und Dialog sind eigene Streifen; Hover/Fokus/
// Drückfarbe zeichnen nur die Zeilen des betroffenen Elements neu. Alle Maße aus xr_aoe_layout.h, skaliert mit
// scale() = min(H / 1080, W / 1920); rechts verankerte Elemente (Uhr, Menü-Knopf, Minimap) hängen am rechten Rand.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "text_raster.h"
#include "xr_aoe_layout.h"
#include "xr_aoe_model.h"
#include "xr_aoe_skin.h"
#include "xr_canvas.h"

namespace agesxr {

class AoeUi {
public:
    using Rect = AoeRect;

    enum class Action {
        kNone,
        kCommand,          // id = Befehl des Rasters (Produktions-Code oder kAoeCmd*)
        kBlocked,          // gesperrter Knopf mit Grund (id, index): wie im Original eine Meldung („Nicht genug Holz“)
        kQueueCancel,      // index = Auftrag der Warteschlange
        kSelectUnit,       // index = Einheit der Mehrfachauswahl
        kContextItem,      // id = GameUiItem::id
        kContextClosed,    // Klick neben das Kontextmenü
        kMenuOpened,       // Knopf „Menü“ (Spielmenü offen)
        kResume,           // „Weiterspielen“ (Menü zu, Menü-Pause aufheben)
        kMenuClosed,       // „Schließen“ (Menü zu, Nutzerpause bleibt)
        kSpeedChanged,     // menu().speed
        kSettingChanged,   // Beschriftung/Hotkeys/Meldungen
        kNewMap,           // „Karte starten“ mit biome/size/opponent/seed
        kSurrenderArmed,   // erster Klick auf „Partie aufgeben“ (Doppelpuls)
        kSurrender,        // zweiter Klick innerhalb der Frist
        kSave,             // „Speichern“ (nur mit menu().saveAvailable)
        kLoad,             // „Laden“ (nur mit menu().loadAvailable)
        kBoardNewMap,      // Sieg-Tafel „Neue Karte ›“ → Spielmenü-Seite Neue Karte
        kBoardClosed,      // Sieg-Tafel „Schließen“
        kMinimapJump,      // (S3) Klick auf die Raute: fx/fy = Kartenanteile
    };
    struct Result {
        Action action = Action::kNone;
        int id = -1;
        int index = -1;
        float fx = 0.0f, fy = 0.0f;
    };

    enum class Hit { kNone, kGrid, kQueue, kGarrison, kMulti, kMenuBtn, kMinimap, kCtxItem, kDlgRow, kDlgArrowL,
                     kDlgArrowR, kDlgLoad, kBoardBtn };
    struct HitState {
        Hit kind = Hit::kNone;
        int index = -1;
        bool operator==(const HitState& o) const { return kind == o.kind && index == o.index; }
        bool operator!=(const HitState& o) const { return !(*this == o); }
        bool none() const { return kind == Hit::kNone; }
    };

    // Zeilen des Spielmenüs je Seite (dialogRowRect(index))
    enum MainRow { kMainResume = 0, kMainSave, kMainSettings, kMainNewMap, kMainSurrender, kMainClose, kMainCount };
    enum SettingsRow { kSetSpeed = 0, kSetLabels, kSetHotkeys, kSetMessages, kSetBack, kSetCount };
    enum NewMapRow { kNewBiome = 0, kNewSize, kNewOpponent, kNewSeed, kNewStart, kNewBack, kNewCount };
    static constexpr int kBoardButtons = 2;  // Neue Karte ›, Schließen

    AoeUi();
    ~AoeUi();
    bool init();  // Schriften; false = keine Schrift (Formen und Logik funktionieren trotzdem)
    bool ok() const { return mOk; }
    void setSkin(std::unique_ptr<SkinProvider> skin);
    void resize(int width, int height);
    int width() const { return mCanvas ? mCanvas->width() : 0; }
    int height() const { return mCanvas ? mCanvas->height() : 0; }
    float scale() const;
    int px(float v) const;  // Basis-Maß → Fensterpixel

    AoeModel& model() { return mModel; }
    const AoeModel& model() const { return mModel; }
    AoeMenuModel& menu() { return mMenu; }
    const AoeMenuModel& menu() const { return mMenu; }
    ContextMenuModel& context() { return mContext; }
    const ContextMenuModel& context() const { return mContext; }
    MatchBoardModel& board() { return mBoard; }
    const MatchBoardModel& board() const { return mBoard; }

    void openContext(int x, int y, const std::string& title, std::vector<GameUiItem> items);
    void closeContext();
    // Spielmenü öffnen (Hauptseite, Bestätigung zurück) bzw. schließen; Fokus verfällt (P2).
    void openMenu(bool open, double now);
    bool wantsMouse() const { return mContext.open || mMenu.open || mBoard.open; }
    bool anyOpen() const { return wantsMouse(); }
    // Zeiger über einer Leiste oder dem Menü-Knopf: Klicks dort gehen nicht ans Spiel.
    bool overBars(int x, int y) const;
    int topBarPx() const { return px(aoelayout::kTopH); }
    int bottomBarPx() const { return px(aoelayout::kBarH); }  // Kamera-Grenze unten (Spec §1.4)

    void onMove(int x, int y);
    Result onClick(int x, int y, double now);  // Linksklick (Flanke)
    // Taste: 'A' … 'Z' = Hotkey des Rasters (nur ohne offenen Dialog), Escape schließt Kontextmenü/Fokus;
    // Esc/F10 fürs Spielmenü entscheidet der Aufrufer (Bauplatz-Modus hat Vorrang).
    Result onKey(int qtKey, double now);
    static constexpr int kKeyEscape = 0x01000000;

    // Stick/A/B-Bedienung (Spec §2.2): Fokus ins Raster (X), bewegen, auslösen (A), aufheben (B).
    bool focusMode() const { return mFocusMode; }
    void focusGrid(bool on);
    Result navigate(int dx, int dy, double now);  // dy +1 = runter, dx +1 = rechts
    Result activate(double now);                  // A
    HitState focus() const { return mFocus; }
    std::vector<HitState> focusOrder() const;     // Reihenfolge beim Weitergehen (Tests)

    // Pixel (RGBA, Zeile 0 oben, Fenstergröße). Zeichnet nur bei Änderung; version() steigt, lastBands() nennt die
    // neu gezeichneten Zeilenstreifen (sortiert, getrennt).
    const uint32_t* pixels(double now);
    uint32_t version() const { return mVersion; }
    struct Band {
        int y0 = 0, y1 = 0;
        bool empty() const { return y1 <= y0; }
    };
    const std::vector<Band>& lastBands() const { return mLastBands; }
    int lastBandRows() const;
    void invalidate() { mDirty = true; }
    const UiFeedback& feedback() const { return mFeedback; }

    // Layout in Fensterpixeln (Tests, Replays, Fork-Controller).
    Rect topBarRect() const;
    Rect topCellRect(int i) const;      // 0 … 4
    Rect ageRect() const;
    Rect plaketteRect() const;
    Rect clockRect() const;
    Rect menuButtonRect() const;
    Rect menuButtonHitRect() const;
    Rect barRect() const;               // untere Leiste
    Rect commandsRect() const;
    Rect selectionRect() const;
    Rect minimapFieldRect() const;
    Rect minimapRect() const;           // Hülle der Raute
    Rect gridRect(int cell) const;      // Knopf 76 × 76
    Rect gridHitRect(int cell) const;   // Trefferzone 82 × 82 (lückenlos)
    Rect portraitRect() const;
    Rect hpRect() const;
    Rect queueRect(int i) const;        // 0 … kAoeQueueShown − 1
    Rect garrisonRect(int i) const;     // 0 … kGarrisonVisible − 1
    Rect multiRect(int i) const;        // 0 … kAoeMultiShown − 1
    // Trefferzone eines Feldes (Hit::kQueue/kGarrison/kMulti): Feld + halbe Lücke, aus Basis-Kanten gerundet, damit
    // Nachbarn lückenlos und ohne Überlappung aneinanderstoßen.
    Rect slotHitRect(Hit kind, int i) const;
    Rect messageRect(int i) const;      // i = 0 unterste (neueste) Zeile
    Rect contextRect() const;
    Rect contextItemRect(int index) const;
    Rect dialogRect() const;
    int dialogRowCount() const;
    Rect dialogRowRect(int row) const;
    Rect dialogArrowRect(int row, bool right) const;
    Rect dialogLoadRect() const;        // „Laden“ rechts neben „Speichern“ (nur mit loadAvailable)
    Rect dialogTitleRect() const;
    Rect boardRect() const;
    Rect boardButtonRect(int i) const;
    bool dialogRowIsValue(int row) const;
    bool dialogRowEnabled(int row) const;
    std::string dialogRowLabel(int row) const;
    std::string dialogRowValue(int row) const;
    HitState hitTest(int x, int y) const;
    int feedbackId(const HitState& h) const;
    // Text in maxW einpassen (erst kleiner bis minPx, dann „…“), UTF-8-sicher.
    std::string fitText(const std::string& s, float& pxSize, float minPx, int maxW, bool bold) const;

private:
    enum class Group { kBar, kContext, kDialog, kBoard };
    int lx(float v) const { return px(v); }
    int rx(float v) const { return width() - px(static_cast<float>(aoelayout::kBaseW) - v); }
    Rect r4(int x0, int y0, int x1, int y1) const { return {x0, y0, x1, y1}; }
    // Rechteck in Basis-Koordinaten der unteren Leiste (y ≥ kBarY, links verankert) → Fensterpixel
    Rect barBase(int x0, int y0, int x1, int y1) const;
    Rect slotBase(Hit kind, int i, int pad) const;
    Rect rectOf(const HitState& h) const;   // gezeichnetes Rechteck eines Treffers (Fokus-/Hover-Streifen)
    Rect bandOf(const HitState& h) const;   // Zeilen, die ein Hover-/Fokuswechsel neu zeichnen muss
    void markBand(int y0, int y1);
    void markRect(const Rect& r);
    void paintBand(int y0, int y1, double now);
    void drawAll(double now);
    void drawTop(double now);
    void drawBar();
    void drawGrid();
    void drawSelection();
    void drawMinimap();
    void drawMessages(double now);
    void drawContext();
    void drawDialog();
    void drawBoard();
    void drawArrow(const Rect& r, bool right, SkinState st);
    void drawSlotRow(const std::vector<AoeSlot>& slots, int shown, int x0, int y0, Hit kind, float progressFirst,
                     bool enabled, int maxX);
    void textIn(const char* utf8, const Rect& r, float pxSize, uint32_t color, bool bold, bool shadow, int align);
    SkinState stateOf(const HitState& h, bool enabled) const;
    std::string blockedOf(const HitState& h) const;  // „Haus: nicht genug Holz (30)“ für gesperrte Rasterknöpfe
    Result clickHit(const HitState& h, int x, int y, double now);
    Result dialogClick(const HitState& h, double now);
    Result stepValue(int row, int dir, double now);
    void setPressed(const HitState& h, double now);
    bool isGridFocusable(const HitState& h) const;
    void setFocus(const HitState& h);

    bool mOk = false;
    TextRaster mFont, mFontBold;
    std::unique_ptr<Canvas> mCanvas;
    std::unique_ptr<SkinProvider> mSkin;
    AoeModel mModel, mShownModel;
    AoeMenuModel mMenu, mShownMenu;
    ContextMenuModel mContext, mShownContext;
    MatchBoardModel mBoard, mShownBoard;
    HitState mHover, mShownHover, mPressed, mShownPressed, mFocus, mShownFocus;
    bool mFocusMode = false, mShownFocusMode = false;
    double mPressedUntil = 0.0;
    Rect mShownContextRect, mShownDialogRect, mShownBoardRect;
    bool mShownPlakette = false;
    bool mShownFading = false;
    bool mDirty = true;
    uint32_t mVersion = 0;
    uint32_t mClickSeq = 0;
    std::vector<Band> mBands, mLastBands;
    UiFeedback mFeedback;
    int mShownMenuBtnState = -1;
};

}  // namespace agesxr
