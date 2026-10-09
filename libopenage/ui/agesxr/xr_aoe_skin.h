// xr.ages — Skin-Schicht der AoE-Oberfläche (docs/UI-SPEC-AOE.md §3): AoeUi zeichnet alle Flächen, Knöpfe, Symbole
// und Balken über die Schnittstelle SkinProvider. Heute gibt es nur den Vektor-Skin (VectorSkin, Palette §3.3:
// Holz/Stein/Gold/Pergament/Tinte, eckige Flächen mit 2-px-Bevel, Pergament-Rauschen); der Spieldaten-Skin (S2,
// SkinSprites: Atlas aus der Nutzerkopie, Rechteck-Quellen je Element) setzt dieselbe Schnittstelle um.
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "xr_canvas.h"
#include "xr_hud_model.h"

namespace agesxr {

// Palette des Vektor-Skins (eigene Werte, Spec §3.3).
namespace aoepal {
constexpr uint32_t kWoodDark = rgba(0x3B, 0x26, 0x14, 255);
constexpr uint32_t kWoodMid = rgba(0x5C, 0x3A, 0x1E, 255);
constexpr uint32_t kWoodLight = rgba(0x8A, 0x5A, 0x2B, 255);
constexpr uint32_t kStoneDark = rgba(0x3A, 0x3A, 0x2E, 255);
constexpr uint32_t kStoneMid = rgba(0x4F, 0x4E, 0x3E, 255);
constexpr uint32_t kStoneLight = rgba(0x6E, 0x6C, 0x56, 255);
constexpr uint32_t kGold = rgba(0xC9, 0xA2, 0x4A, 255);
constexpr uint32_t kGoldLight = rgba(0xE8, 0xC8, 0x6A, 255);
constexpr uint32_t kGoldDark = rgba(0x8B, 0x6A, 0x22, 255);
constexpr uint32_t kParchment = rgba(0xE3, 0xCF, 0xA6, 255);
constexpr uint32_t kParchmentDark = rgba(0xC9, 0xAE, 0x7E, 255);
constexpr uint32_t kParchmentShadow = rgba(0xA6, 0x8A, 0x5C, 255);
constexpr uint32_t kInk = rgba(0x2B, 0x1D, 0x0E, 255);
constexpr uint32_t kWhite = rgba(0xFF, 0xFF, 0xFF, 255);
constexpr uint32_t kShadow = rgba(0, 0, 0, 204);  // Schrift-Schatten α 80 %
constexpr uint32_t kDim = rgba(0x8C, 0x8C, 0x8C, 255);
constexpr uint32_t kDisabled = rgba(0x4A, 0x4A, 0x42, 255);
constexpr uint32_t kFocus = rgba(0xDC, 0xCB, 0x70, 255);
constexpr uint32_t kHpGood = rgba(0x3F, 0xBF, 0x3F, 255);
constexpr uint32_t kHpMid = rgba(0xE0, 0xC0, 0x30, 255);
constexpr uint32_t kHpLow = rgba(0xD0, 0x30, 0x30, 255);
constexpr uint32_t kBarTrack = rgba(0x55, 0x55, 0x55, 255);
constexpr uint32_t kProgress = rgba(0xB2, 0x22, 0x22, 255);
constexpr uint32_t kDanger = rgba(0x8B, 0x2E, 0x1E, 255);
constexpr uint32_t kVictory = rgba(0x3F, 0xBF, 0x3F, 255);
constexpr uint32_t kCapsule = rgba(0x20, 0x20, 0x1A, 140);   // Meldungskapsel α 55 %
constexpr uint32_t kPlayerBlue = rgba(0x2E, 0x5A, 0xC8, 255);  // Meldungsbalken Spielerfarbe
constexpr uint32_t kWarnYellow = rgba(0xE0, 0xC0, 0x30, 255);
constexpr uint32_t kAlertRed = rgba(0xD0, 0x30, 0x30, 255);
constexpr uint32_t kMsgWarn = rgba(0xFF, 0xB0, 0xC0, 255);    // Plakette: rosa bei Warnung
constexpr uint32_t kMsgGood = rgba(0x9F, 0xE8, 0x7F, 255);
constexpr uint32_t kMapGrass = rgba(0x4A, 0x7A, 0x3A, 255);
constexpr uint32_t kMapWinter = rgba(0xC8, 0xD0, 0xD8, 255);
constexpr uint32_t kMapDesert = rgba(0xC8, 0xA8, 0x68, 255);
constexpr uint32_t kMapWater = rgba(0x2F, 0x5A, 0x8A, 255);
constexpr uint32_t kDimBackdrop = rgba(8, 6, 4, 110);  // Abdunkeln hinter Dialog/Tafel
// Rohstoff-Symbole
constexpr uint32_t kFoodColor = rgba(0xE0, 0x60, 0x48, 255);
constexpr uint32_t kWoodColor = rgba(0xC8, 0x90, 0x50, 255);
constexpr uint32_t kGoldColor = rgba(0xF4, 0xC8, 0x40, 255);
constexpr uint32_t kStoneColor = rgba(0xB8, 0xB8, 0xC0, 255);
}  // namespace aoepal

struct AoeRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool contains(int x, int y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
    bool empty() const { return x1 <= x0 || y1 <= y0; }
    int w() const { return x1 - x0; }
    int h() const { return y1 - y0; }
    int cx() const { return (x0 + x1) / 2; }
    int cy() const { return (y0 + y1) / 2; }
    AoeRect grown(int d) const { return {x0 - d, y0 - d, x1 + d, y1 + d}; }
    bool operator==(const AoeRect& o) const { return x0 == o.x0 && y0 == o.y0 && x1 == o.x1 && y1 == o.y1; }
    bool operator!=(const AoeRect& o) const { return !(*this == o); }
};

enum class SkinPanel {
    kTopBar,        // Holzband dunkel mit Goldkante unten
    kCommands,      // Steinfeld (Befehle)
    kSelection,     // Pergament (Auswahl)
    kMinimapField,  // Steinfeld mit Rautenrahmen
    kDialog,        // Pergament mit Holzrahmen + Goldlinie
    kContext,       // Pergament, Goldkante, Schlagschatten
    kTitlePlate,    // Holz mittel (Dialogtitel), Holz hell (Kontexttitel: kTitlePlateLight)
    kTitlePlateLight,
    kCapsule,       // dunkle Kapsel (Meldungszeile, Plakette)
    kPortrait,      // Porträtrahmen (Stein mit Goldrahmen)
    kValueCell,     // Wertzeile im Dialog (Pergament dunkel)
    kBackdrop,      // Abdunkeln hinter Dialog/Tafel
};

enum class SkinButton {
    kCommand,   // 76 × 76 Steinknopf
    kDialog,    // 480 × 64 Holz hell
    kDanger,    // „Partie aufgeben“ rot
    kArrow,     // ‹ › 64 × 48
    kMenuTop,   // Knopf „Menü“ oben rechts
    kSlot,      // 64 × 64 Feld (Warteschlange, Garnison, Mehrfachauswahl)
    kSlotEmpty, // leeres Feld (Pergament dunkel)
    kCtxItem,   // Kontextmenü-Eintrag
    kBoardBtn,  // Knopf der Sieg-Tafel
};

enum class SkinState { kNormal, kHover, kPressed, kDisabled };
enum class SkinBar { kHp, kProgress, kQueue };

class SkinProvider {
public:
    virtual ~SkinProvider() = default;
    // s = Skalierung (1 bei 1080p) für Rahmen- und Bevel-Stärken.
    virtual void drawPanel(Canvas& c, SkinPanel kind, const AoeRect& r, float s) = 0;
    virtual void drawButton(Canvas& c, SkinButton kind, const AoeRect& r, SkinState st, float s) = 0;
    // Symbol um (cx, cy), scale = Vergrößerung der 36-px-Symbolform (2 = Befehlsknopf, 3 = Porträt).
    virtual void drawIcon(Canvas& c, HudIcon icon, int cx, int cy, float scale, SkinState st) = 0;
    virtual void drawBar(Canvas& c, SkinBar kind, const AoeRect& r, float fraction, float s) = 0;
    // Stick-Fokus (3 px #DCCB70 außen) bzw. Hover-Goldrahmen (3 px) um ein Element.
    virtual void drawFocus(Canvas& c, const AoeRect& r, float s) = 0;
    virtual void drawHoverFrame(Canvas& c, const AoeRect& r, float s) = 0;
    // Minimap-Raute (Platzhalter): Grundfarbe je Landschaft mit Goldrand, Kamerarahmen weiß.
    virtual void drawMinimap(Canvas& c, int cx, int cy, int w, int h, int biome, float camX, float camY, float camW,
                             float camH, float s) = 0;
    // Schriftfarben je Untergrund
    virtual uint32_t inkOn(SkinPanel panel, SkinState st) const = 0;
    virtual bool shadowOn(SkinPanel panel) const = 0;  // Schatten unter der Schrift (Holz/Stein ja, Pergament nein)
};

// Eigene Vektor-Nachzeichnung im AoE-Stil (Spec §3.3): eckig, Bevel 2 px, Pergament-Rauschen 4 %, Holzmaserung.
class VectorSkin final : public SkinProvider {
public:
    VectorSkin();
    void drawPanel(Canvas& c, SkinPanel kind, const AoeRect& r, float s) override;
    void drawButton(Canvas& c, SkinButton kind, const AoeRect& r, SkinState st, float s) override;
    void drawIcon(Canvas& c, HudIcon icon, int cx, int cy, float scale, SkinState st) override;
    void drawBar(Canvas& c, SkinBar kind, const AoeRect& r, float fraction, float s) override;
    void drawFocus(Canvas& c, const AoeRect& r, float s) override;
    void drawHoverFrame(Canvas& c, const AoeRect& r, float s) override;
    void drawMinimap(Canvas& c, int cx, int cy, int w, int h, int biome, float camX, float camY, float camW,
                     float camH, float s) override;
    uint32_t inkOn(SkinPanel panel, SkinState st) const override;
    bool shadowOn(SkinPanel panel) const override;

    // Bausteine (auch für Tests): Fläche mit Lichtkante oben/links und Schattenkante unten/rechts.
    static void bevelRect(Canvas& c, const AoeRect& r, uint32_t fill, int bevel, bool sunken = false);
    static void frame(Canvas& c, const AoeRect& r, int width, uint32_t color);  // Rahmen innen
    static uint32_t lighten(uint32_t color, float factor);  // factor > 0 heller, < 0 dunkler (−0,3 = −30 %)
    static uint32_t biomeColor(int biome);
    void parchment(Canvas& c, const AoeRect& r, uint32_t base);  // Rauschen aus der Kachel

private:
    // Abdeckungsmaske relativ zu einem Ankerpixel (Teil-Redraw: einmal rastern, danach nur einmischen).
    struct Mask {
        int x0 = 0, y0 = 0, w = 0, h = 0;
        std::vector<uint8_t> cov;
    };
    const Mask& iconMask(HudIcon icon, float scale, int offset);
    const Mask& diamondMask(int w, int h, int inset);

    static constexpr int kTile = 64;
    uint8_t mNoise[kTile * kTile];  // 0 … 255, vorberechnet (deterministisch)
    uint32_t mParchmentBase = 0;    // Grundfarbe der zwischengespeicherten Pergament-Kachel
    std::vector<uint32_t> mParchmentTile;
    std::unordered_map<uint64_t, Mask> mMasks;
};

}  // namespace agesxr
