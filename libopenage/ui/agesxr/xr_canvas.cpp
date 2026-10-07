// xr.ages — gemeinsame CPU-Rasterung, Implementierung. Siehe xr_canvas.h.
// Formfunktionen unverändert aus xr_menu.cpp (xr.farm, MIT); einzige Änderung: Puffer/Breite/Clip als Member.
#include "xr_canvas.h"

#include <cstddef>

namespace agesxr {

using namespace palette;

void Canvas::clearBand(int y0, int y1) {
    y0 = std::clamp(y0, 0, mH);
    y1 = std::clamp(y1, 0, mH);
    if (y1 <= y0) return;
    std::fill(mPixels.begin() + static_cast<std::ptrdiff_t>(y0) * mW,
              mPixels.begin() + static_cast<std::ptrdiff_t>(y1) * mW, 0u);
}

void Canvas::blend(int x, int y, uint32_t color, float cov) {
    if (x < 0 || y < mClipY0 || x >= mW || y >= mClipY1 || cov <= 0.0f) return;
    const uint32_t c = static_cast<uint32_t>(std::min(cov, 1.0f) * 255.0f + 0.5f);
    const uint32_t a = ((color >> 24) & 0xFF) * c / 255;
    if (a == 0) return;
    uint32_t& dst = mPixels[static_cast<size_t>(y) * static_cast<size_t>(mW) + static_cast<size_t>(x)];
    if (a == 255) { dst = color | 0xFF000000u; return; }
    const uint32_t inv = 255 - a;
    const uint32_t r = ((color & 0xFF) * a + (dst & 0xFF) * inv) / 255;
    const uint32_t g = (((color >> 8) & 0xFF) * a + ((dst >> 8) & 0xFF) * inv) / 255;
    const uint32_t b = (((color >> 16) & 0xFF) * a + ((dst >> 16) & 0xFF) * inv) / 255;
    const uint32_t outA = a + ((dst >> 24) & 0xFF) * inv / 255;
    dst = r | (g << 8) | (b << 16) | (std::min(outA, 255u) << 24);
}

// Abgerundetes Rechteck, Kante geglättet; Abstand nur in den vier Eckquadraten berechnet.
void Canvas::roundRect(int x0, int y0, int x1, int y1, int r, uint32_t color) {
    r = std::max(0, std::min(r, std::min(x1 - x0, y1 - y0) / 2));
    const int ya = std::max(y0, mClipY0), yb = std::min(y1, mClipY1);
    const int xa = std::max(x0, 0), xb = std::min(x1, mW);
    for (int y = ya; y < yb; ++y) {
        const bool cornerRow = y < y0 + r || y >= y1 - r;
        for (int x = xa; x < xb; ++x) {
            float cov = 1.0f;
            if (cornerRow && (x < x0 + r || x >= x1 - r)) {
                const float cx = static_cast<float>(x < x0 + r ? x0 + r : x1 - 1 - r);
                const float cy = static_cast<float>(y < y0 + r ? y0 + r : y1 - 1 - r);
                const float dx = static_cast<float>(x) - cx, dy = static_cast<float>(y) - cy;
                cov = std::min(std::max(static_cast<float>(r) + 0.5f - std::sqrt(dx * dx + dy * dy), 0.0f), 1.0f);
            }
            blend(x, y, color, cov);
        }
    }
}

void Canvas::fillRect(int x0, int y0, int x1, int y1, uint32_t color) {
    for (int y = std::max(y0, mClipY0); y < std::min(y1, mClipY1); ++y)
        for (int x = std::max(x0, 0); x < std::min(x1, mW); ++x) blend(x, y, color, 1.0f);
}

// Fokusrahmen (Stick-Bedienung): geglätteter Ring, 4 px breit, direkt außerhalb des abgerundeten Elements.
void Canvas::focusFrame(int x0, int y0, int x1, int y1, int r) {
    const float w = 4.0f;
    const float cx = 0.5f * static_cast<float>(x0 + x1), cy = 0.5f * static_cast<float>(y0 + y1);
    const float hx = 0.5f * static_cast<float>(x1 - x0), hy = 0.5f * static_cast<float>(y1 - y0);
    const float rad = std::min(static_cast<float>(r), std::min(hx, hy));
    const int pad = static_cast<int>(w) + 2;
    for (int y = std::max(y0 - pad, mClipY0); y < std::min(y1 + pad, mClipY1); ++y) {
        for (int x = std::max(x0 - pad, 0); x < std::min(x1 + pad, mW); ++x) {
            const float qx = std::fabs(static_cast<float>(x) + 0.5f - cx) - (hx - rad);
            const float qy = std::fabs(static_cast<float>(y) + 0.5f - cy) - (hy - rad);
            const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
            const float d = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - rad;
            const float cov = std::min(std::clamp(d + 0.5f, 0.0f, 1.0f), std::clamp(w + 0.5f - d, 0.0f, 1.0f));
            if (cov > 0.0f) blend(x, y, kFocusColor, cov);
        }
    }
}

void Canvas::text(const char* utf8, int x, int y, float px, uint32_t color, bool bold) {
    // Zeile liegt sicher in [y − px, y + 2·px): außerhalb des Streifens gar nicht erst rastern.
    if (!mFont || !visible(y - static_cast<int>(px), y + 2 * static_cast<int>(px))) return;
    fontFor(bold).draw(mPixels.data(), mW, mH, utf8, x, y, px, color, mClipY0, mClipY1);
}

int Canvas::textWidth(const char* utf8, float px, bool bold) const {
    return mFont ? fontFor(bold).width(utf8, px) : 0;
}

// Schrift mit dunklem Schlagschatten (2 px nach rechts unten) wie die Beschriftungen im Spiel.
void Canvas::textShadow(const char* utf8, int x, int y, float px, uint32_t color, bool bold) {
    text(utf8, x + 2, y + 2, px, (kInk & 0x00FFFFFFu) | 0xC0000000u, bold);
    text(utf8, x, y, px, color, bold);
}

// Abgerundetes Rechteck mit senkrechtem Farbverlauf (je Zeile gemischt, Kanten wie roundRect geglättet).
void Canvas::roundRectV(int x0, int y0, int x1, int y1, int r, uint32_t top, uint32_t bottom) {
    const int h = std::max(1, y1 - y0 - 1);
    auto mix = [](uint32_t a, uint32_t b, int t, int n) {
        uint32_t out = 0;
        for (int sh = 0; sh < 32; sh += 8) {
            const int ca = static_cast<int>((a >> sh) & 0xFF), cb = static_cast<int>((b >> sh) & 0xFF);
            out |= static_cast<uint32_t>(ca + (cb - ca) * t / n) << sh;
        }
        return out;
    };
    r = std::max(0, std::min(r, std::min(x1 - x0, y1 - y0) / 2));
    for (int y = std::max(y0, mClipY0); y < std::min(y1, mClipY1); ++y) {
        const uint32_t color = mix(top, bottom, y - y0, h);
        const bool cornerRow = y < y0 + r || y >= y1 - r;
        for (int x = std::max(x0, 0); x < std::min(x1, mW); ++x) {
            float cov = 1.0f;
            if (cornerRow && (x < x0 + r || x >= x1 - r)) {
                const float cx = static_cast<float>(x < x0 + r ? x0 + r : x1 - 1 - r);
                const float cy = static_cast<float>(y < y0 + r ? y0 + r : y1 - 1 - r);
                const float dx = static_cast<float>(x) - cx, dy = static_cast<float>(y) - cy;
                cov = std::min(std::max(static_cast<float>(r) + 0.5f - std::sqrt(dx * dx + dy * dy), 0.0f), 1.0f);
            }
            blend(x, y, color, cov);
        }
    }
}

// Steinplatte: Schattenkante unten, Verlaufsfläche, schmale Lichtkante oben.
void Canvas::slab(int x0, int y0, int x1, int y1, int r, uint32_t top, uint32_t bottom) {
    roundRect(x0, y0 + 3, x1, y1 + 3, r, kSlabShadow);
    roundRectV(x0, y0, x1, y1, r, top, bottom);
    roundRectV(x0 + r / 2, y0 + 2, x1 - r / 2, y0 + 5, 2, kSlabLight, (kSlabLight & 0x00FFFFFFu));
}

// Holzschild wie die Hängeschilder im Hauptmenü: Verlauf, waagrechte Maserung, dunkle Kante, zwei Nägel.
void Canvas::woodSign(int x0, int y0, int x1, int y1, int r, bool nails) {
    roundRect(x0, y0, x1, y1, r, kWoodEdge);
    roundRectV(x0 + 3, y0 + 3, x1 - 3, y1 - 3, std::max(0, r - 3), kWoodTop, kWoodBottom);
    // Maserung: leicht gewellte, 2 px dicke Linien – fest im Raster, damit nichts flimmert.
    for (int line = 0; line < 4; ++line) {
        const int base = y0 + 14 + line * (y1 - y0 - 24) / 4;
        for (int x = x0 + r; x < x1 - r; ++x) {
            const float wave = 2.0f * std::sin(static_cast<float>(x) * 0.011f + static_cast<float>(line) * 1.7f);
            const int y = base + static_cast<int>(wave);
            blend(x, y, kWoodGrain, 1.0f);
            blend(x, y + 1, kWoodGrain, 0.6f);
        }
    }
    if (!nails) return;
    for (int side = 0; side < 2; ++side) {
        const int cx = side == 0 ? x0 + 20 : x1 - 20, cy = (y0 + y1) / 2;
        circle(cx, cy, 7, kNail);
        circle(cx - 2, cy - 2, 2, kSlabLight);
    }
}

}  // namespace agesxr
