// xr.ages — gemeinsame CPU-Rasterung für VR-Menü und VR-HUD: Palette (Stardew-Optik aus xr.farm) und
// Formfunktionen (blend, roundRect, roundRectV, slab, woodSign, textShadow, focusFrame, fillSdf) auf einem
// RGBA-Puffer (Zeile 0 oben). Kein GL. Alle Funktionen achten auf den Clip-Streifen [clipY0, clipY1) des
// Teil-Redraws (Skill xr-vr-menu §8), damit ein Streifen pixelgleich zum vollen Redraw neu entsteht.
// Herkunft: Formfunktionen und Palette unverändert aus xr_menu.cpp (xr.farm, XRShell vr_menu.cpp, MIT) hierher
// verschoben, damit das HUD (xr_hud.*) dieselbe Optik ohne Kopie nutzt.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "text_raster.h"

namespace agesxr {

constexpr uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | (g << 8) | (b << 16) | (a << 24); }

// Palette (aus xr.farm, unverändert): lavendelgrauer Grabstein, dunkle Steinmulden, Holzschild,
// Rasengrün für Aktionen, cremeweiße Schrift mit dunklem Schatten. Eigene Vektor-Nachzeichnung, keine Spielgrafik.
namespace palette {
constexpr uint32_t kStoneRim = rgba(158, 160, 198, 255);    // heller Steinrand
constexpr uint32_t kStoneTop = rgba(96, 99, 138, 250);      // Tafel oben
constexpr uint32_t kStoneBottom = rgba(66, 68, 100, 250);   // Tafel unten
constexpr uint32_t kStoneDeep = rgba(36, 37, 56, 245);      // Mulden: Reiterleiste, Wertkapsel, Eingabefeld
constexpr uint32_t kSlabTop = rgba(118, 121, 162, 255);     // Steinplatte (Zeile, Knopf)
constexpr uint32_t kSlabBottom = rgba(86, 89, 128, 255);
constexpr uint32_t kSlabHoverTop = rgba(140, 143, 186, 255);
constexpr uint32_t kSlabHoverBottom = rgba(104, 107, 150, 255);
constexpr uint32_t kSlabLight = rgba(190, 192, 226, 150);   // Lichtkante oben
constexpr uint32_t kSlabShadow = rgba(24, 25, 40, 220);     // Schattenkante unten
constexpr uint32_t kLeaf = rgba(128, 214, 40, 255);         // Rasengrün (Aktionstext)
constexpr uint32_t kLeafTop = rgba(124, 206, 44, 255);      // grüne Platte (aktiver Reiter, Fertig)
constexpr uint32_t kLeafBottom = rgba(66, 138, 22, 255);
constexpr uint32_t kWoodTop = rgba(176, 118, 62, 255);      // Holzschild (Kopfzeile)
constexpr uint32_t kWoodBottom = rgba(124, 78, 38, 255);
constexpr uint32_t kWoodGrain = rgba(92, 56, 26, 120);
constexpr uint32_t kWoodEdge = rgba(64, 38, 16, 255);
constexpr uint32_t kNail = rgba(54, 50, 46, 255);
constexpr uint32_t kFocusColor = rgba(255, 214, 90, 255);   // Stick-Fokus: gelber Rahmen
constexpr uint32_t kText = rgba(250, 242, 214, 255);        // cremeweiß
constexpr uint32_t kTextDim = rgba(168, 170, 200, 255);
constexpr uint32_t kInk = rgba(20, 20, 32, 255);            // Schatten, dunkle Schrift
constexpr uint32_t kWarn = rgba(255, 112, 92, 255);         // Beenden-Schrift
constexpr uint32_t kWarnTop = rgba(214, 72, 52, 255);       // Beenden scharf: rote Platte
constexpr uint32_t kWarnBottom = rgba(150, 36, 26, 255);
constexpr uint32_t kLampOff = rgba(78, 80, 108, 255);       // Zustandslämpchen aus (grau)
constexpr uint32_t kKnob = rgba(240, 234, 210, 255);        // Schalterknopf (creme)
constexpr uint32_t kBarTrack = rgba(20, 20, 32, 150);       // Füllbalken: Spur
constexpr uint32_t kBarFill = rgba(128, 214, 40, 190);      // Füllbalken: Anteil (Rasengrün, gedämpft)
}  // namespace palette

// Abstandsfunktionen für Vektorsymbole (negativ = innen).
namespace sdf {
inline float lengthOf(float x, float y) { return std::sqrt(x * x + y * y); }
inline float segment(float px, float py, float ax, float ay, float bx, float by, float r) {
    const float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    const float t = std::clamp((wx * vx + wy * vy) / (vx * vx + vy * vy), 0.0f, 1.0f);
    return lengthOf(wx - t * vx, wy - t * vy) - r;
}
inline float ring(float px, float py, float radius, float width) {
    return std::fabs(lengthOf(px, py) - radius) - width * 0.5f;
}
// Konvexes Dreieck (Ecken in beliebiger Reihenfolge): größter Kantenabstand.
inline float triangle(float px, float py, const float (&v)[6]) {
    float d = -1e9f;
    const float cx = (v[0] + v[2] + v[4]) / 3.0f, cy = (v[1] + v[3] + v[5]) / 3.0f;
    for (int i = 0; i < 3; ++i) {
        const float ax = v[2 * i], ay = v[2 * i + 1], bx = v[(2 * i + 2) % 6], by = v[(2 * i + 3) % 6];
        float nx = by - ay, ny = ax - bx;
        const float len = lengthOf(nx, ny);
        nx /= len;
        ny /= len;
        if ((cx - ax) * nx + (cy - ay) * ny > 0.0f) { nx = -nx; ny = -ny; }  // Normale nach außen
        d = std::max(d, (px - ax) * nx + (py - ay) * ny);
    }
    return d;
}
// Achsparalleles Rechteck um (0,0) mit Halbmaßen hx, hy und Eckradius r.
inline float box(float px, float py, float hx, float hy, float r) {
    const float qx = std::fabs(px) - (hx - r), qy = std::fabs(py) - (hy - r);
    const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    return lengthOf(ox, oy) + std::min(std::max(qx, qy), 0.0f) - r;
}
}  // namespace sdf

// RGBA-Leinwand (je Pixel R,G,B,A als Bytes = 0xAABBGGRR) mit den Formfunktionen der Stardew-Optik.
class Canvas {
public:
    Canvas(int width, int height) : mW(width), mH(height), mPixels(static_cast<size_t>(width) * height, 0u),
                                    mClipY1(height) {}

    int width() const { return mW; }
    int height() const { return mH; }
    uint32_t* data() { return mPixels.data(); }
    const uint32_t* data() const { return mPixels.data(); }
    void setFonts(const TextRaster* regular, const TextRaster* bold) {
        mFont = regular;
        mBold = bold;
    }

    // Teil-Redraw: nur Pixelzeilen in [y0, y1) schreiben.
    void setClip(int y0, int y1) {
        mClipY0 = std::max(0, y0);
        mClipY1 = std::min(mH, y1);
    }
    void resetClip() { setClip(0, mH); }
    int clipY0() const { return mClipY0; }
    int clipY1() const { return mClipY1; }
    bool visible(int y0, int y1) const { return y0 < mClipY1 && y1 > mClipY0; }
    void clearBand(int y0, int y1);  // Zeilen leeren (durchsichtig)

    void blend(int x, int y, uint32_t color, float cov);
    void roundRect(int x0, int y0, int x1, int y1, int r, uint32_t color);
    void roundRectV(int x0, int y0, int x1, int y1, int r, uint32_t top, uint32_t bottom);  // senkrechter Verlauf
    void slab(int x0, int y0, int x1, int y1, int r, uint32_t top, uint32_t bottom);        // Steinplatte mit Kanten
    void woodSign(int x0, int y0, int x1, int y1, int r, bool nails = true);                // Holzschild
    void circle(int cx, int cy, int r, uint32_t color) { roundRect(cx - r, cy - r, cx + r, cy + r, r, color); }
    void fillRect(int x0, int y0, int x1, int y1, uint32_t color);
    // Abdeckungsmaske (w × h Bytes, 0 … 255) mit Farbe einmischen; Ergebnis wie fillSdf mit derselben Abdeckung.
    void blendMask(int x0, int y0, int w, int h, const uint8_t* coverage, uint32_t color);
    void focusFrame(int x0, int y0, int x1, int y1, int r);
    void text(const char* utf8, int x, int y, float px, uint32_t color, bool bold = false);
    void textShadow(const char* utf8, int x, int y, float px, uint32_t color, bool bold = false);
    int textWidth(const char* utf8, float px, bool bold = false) const;
    bool hasFont() const { return mFont && mFont->ok(); }

    // Beliebige Form über eine Abstandsfunktion (Pixelmitte absolut, negativ = innen), Kante 1 px geglättet.
    template <class Sdf>
    void fillSdf(int x0, int y0, int x1, int y1, uint32_t color, Sdf sdfFn) {
        for (int y = std::max(y0, mClipY0); y < std::min(y1, mClipY1); ++y)
            for (int x = std::max(x0, 0); x < std::min(x1, mW); ++x) {
                const float cov = std::clamp(0.5f - sdfFn(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f),
                                             0.0f, 1.0f);
                if (cov > 0.0f) blend(x, y, color, cov);
            }
    }

private:
    const TextRaster& fontFor(bool bold) const { return bold && mBold && mBold->ok() ? *mBold : *mFont; }

    int mW, mH;
    std::vector<uint32_t> mPixels;
    int mClipY0 = 0, mClipY1;
    const TextRaster* mFont = nullptr;
    const TextRaster* mBold = nullptr;
};

}  // namespace agesxr
