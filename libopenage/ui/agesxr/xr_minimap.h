// xr.ages — Minimap im AoE-II-Layout (docs/UI-SPEC-AOE.md §1.2, Stufe S3): Projektion Karte ↔ Raute und
// CPU-Rasterung des Rautenbilds (Geländefarben, Nebel/Schwarz mit weichen Rändern, Ressourcen-Punkte dezent,
// Einheiten und Gebäude in Spielerfarbe, Signal-Ping). Kein GL, keine Engine: die Engine liefert Kachelfarben,
// Kachelzustände und Punkte (gamestate/vision/vision_snapshot.h), der Aufrufer lädt das Bild als eigene Textur
// unter die Leiste (Teil-Redraw: die Leiste wird für die Minimap nicht neu gezeichnet).
//
// Kartenanteile wie der Kamerarahmen (AoeMinimap::camX/camY): u entlang SE (u = se / Kacheln SE),
// v = 1 − ne / Kacheln NE. Raute um (cx, cy) mit Halbachsen hw, hh: x = cx + (u − v)·hw, y = cy + (u + v − 1)·hh –
// Kachel (ne = N, se = 0) liegt oben, (0, N) unten, (0, 0) links, (N, N) rechts (wie die Karte auf dem Bildschirm).
// Eigener Code (MIT), Herkunft PROVENANCE.md.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace agesxr {

// Punktarten in derselben Reihenfolge wie gamestate::vision::dot_kind_t (static_assert im Fork).
enum MinimapDotKind : uint8_t {
    kDotUnit = 0,
    kDotBuilding,
    kDotTree,
    kDotGold,
    kDotStone,
    kDotFood,
    kDotAnimal,
    kDotOther,
};

// Anzeige-Modus des Modi-Knopfs: alles, nur Militär (Einheiten mit Angriff, Gebäude), Wirtschaft (Ressourcen,
// Dorfbewohner, Gebäude).
enum MinimapMode : int { kMinimapNormal = 0, kMinimapMilitary = 1, kMinimapEconomy = 2, kMinimapModeCount = 3 };

struct MinimapDot {
    float ne = 0.0f, se = 0.0f;
    float radius = 0.0f;  // halbe Fußabdruck-Seite (Kacheln), 0 = Einheit
    uint8_t kind = kDotOther;
    uint8_t player = 0;
    bool fogged = false;    // zuletzt gesehen (im Nebel)
    bool military = false;  // Einheit mit Angriff (kein Dorfbewohner)
};

struct MinimapInput {
    int ne = 0, se = 0;                // Kacheln entlang NE / SE
    const uint32_t* terrain = nullptr; // ne·se RGBA (0xAABBGGRR), Index ne + se·Kacheln NE; nullptr = Grasgrün
    const uint8_t* fog = nullptr;      // Kachelzustände 0 unerkundet, 1 erkundet, 2 sichtbar; nullptr = alles sichtbar
    const MinimapDot* dots = nullptr;
    size_t dotCount = 0;
    uint64_t gaia = 255;               // neutraler Spieler (Ressourcen in eigenen Farben)
    int mode = kMinimapNormal;
    // Signal-Ping (Alt+Klick): Kartenanteile und Alter in Sekunden, < 0 = keiner
    float pingU = 0.0f, pingV = 0.0f, pingAge = -1.0f;
};

// Abbildung Kartenanteile ↔ Rautenpixel (Pixelmitte x + 0.5).
struct MinimapGeom {
    float cx = 0.0f, cy = 0.0f, hw = 1.0f, hh = 1.0f;
    void toPixel(float u, float v, float& x, float& y) const {
        x = cx + (u - v) * hw;
        y = cy + (u + v - 1.0f) * hh;
    }
    void toFrac(float x, float y, float& u, float& v) const {
        const float a = (x - cx) / hw, b = (y - cy) / hh;
        u = (a + b + 1.0f) * 0.5f;
        v = (b - a + 1.0f) * 0.5f;
    }
    // Abstand zum Rautenrand in Pixeln (negativ innen), dieselbe Formel wie die Rautenmaske der Leiste
    float edge(float x, float y) const {
        return (std::fabs(x - cx) / hw + std::fabs(y - cy) / hh - 1.0f) * std::min(hw, hh) * 0.7071f;
    }
};

inline void minimapTileToFrac(float ne, float se, int tilesNe, int tilesSe, float& u, float& v) {
    u = se / static_cast<float>(std::max(tilesSe, 1));
    v = 1.0f - ne / static_cast<float>(std::max(tilesNe, 1));
}
inline void minimapFracToTile(float u, float v, int tilesNe, int tilesSe, float& ne, float& se) {
    se = u * static_cast<float>(std::max(tilesSe, 1));
    ne = (1.0f - v) * static_cast<float>(std::max(tilesNe, 1));
}

constexpr uint32_t mmRgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a = 255) { return r | (g << 8) | (b << 16) | (a << 24); }

// Helligkeit im Nebel (erkundet, nicht sichtbar): kaum abgedunkelt wie im Original (docs/aoe2-spec 13 §Nebel); gleicher
// Wert wie im Spielbild (Gelände-Shader 192/255, Welt-Pass FOG_TINT 0,75).
constexpr float kMinimapFogged = 0.75f;

// Spielerfarben wie die Sprites (world2d.frag.glsl): blau, rot, grün, gelb, türkis, lila, grau, orange.
inline uint32_t minimapPlayerColor(uint64_t player) {
    static constexpr std::array<uint32_t, 8> kColors{mmRgba(0, 76, 255),   mmRgba(255, 0, 0),   mmRgba(0, 255, 0),
                                                    mmRgba(255, 255, 0), mmRgba(0, 255, 255), mmRgba(255, 0, 255),
                                                    mmRgba(153, 153, 153), mmRgba(255, 128, 0)};
    return kColors[player % kColors.size()];
}

inline uint32_t mmScale(uint32_t c, float f) {
    auto ch = [&](int shift) {
        const float v = static_cast<float>((c >> shift) & 0xFFu) * f;
        return static_cast<uint32_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f) << shift;
    };
    return ch(0) | ch(8) | ch(16) | (c & 0xFF000000u);
}
inline uint32_t mmMix(uint32_t a, uint32_t b, float t) {
    auto ch = [&](int shift) {
        const float x = static_cast<float>((a >> shift) & 0xFFu), y = static_cast<float>((b >> shift) & 0xFFu);
        return static_cast<uint32_t>(std::clamp(x + (y - x) * t, 0.0f, 255.0f) + 0.5f) << shift;
    };
    return ch(0) | ch(8) | ch(16) | ch(24);
}

// Farbe eines Punkts, 0 = nicht zeichnen (Modus).
inline uint32_t minimapDotColor(const MinimapDot& d, const MinimapInput& in) {
    const bool gaia = d.player == in.gaia;
    uint32_t c = 0;
    switch (d.kind) {
    case kDotUnit:
    case kDotBuilding:
        if (in.mode == kMinimapMilitary && d.kind == kDotUnit && !d.military) return 0;
        if (in.mode == kMinimapEconomy && d.military) return 0;
        c = gaia ? mmRgba(230, 230, 230) : minimapPlayerColor(d.player);
        break;
    case kDotTree:
        if (in.mode == kMinimapMilitary) return 0;
        c = mmRgba(34, 68, 30);  // dezent: dunkles Waldgrün
        break;
    case kDotGold:
        if (in.mode == kMinimapMilitary) return 0;
        c = mmRgba(240, 204, 60);
        break;
    case kDotStone:
        if (in.mode == kMinimapMilitary) return 0;
        c = mmRgba(170, 170, 170);
        break;
    case kDotFood:
        if (in.mode == kMinimapMilitary) return 0;
        c = mmRgba(196, 92, 172);
        break;
    case kDotAnimal:
        if (in.mode == kMinimapMilitary) return 0;
        c = mmRgba(236, 214, 176);
        break;
    default:
        return 0;
    }
    return d.fogged ? mmScale(c, kMinimapFogged) : c;
}

// Kachelzustand als Helligkeit 0 … 1 (bilinear zwischen den Kachelmitten: weiche Ränder wie im Spielbild).
inline float minimapFogAt(const MinimapInput& in, float ne, float se) {
    if (!in.fog || in.ne <= 0 || in.se <= 0) return 1.0f;
    const float x = ne - 0.5f, y = se - 0.5f;
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
    auto at = [&](int tx, int ty) {
        tx = std::clamp(tx, 0, in.ne - 1);
        ty = std::clamp(ty, 0, in.se - 1);
        const uint8_t s = in.fog[static_cast<size_t>(tx) + static_cast<size_t>(ty) * static_cast<size_t>(in.ne)];
        return s >= 2 ? 1.0f : s == 1 ? kMinimapFogged : 0.0f;
    };
    const float a = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * fx;
    const float b = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * fx;
    return a + (b - a) * fy;
}


// Geometrie des Rautenbilds: (w + 1) × (h + 1) Pixel, Mitte (w/2, h/2) wie die Rautenmaske der Leiste.
inline MinimapGeom minimapImageGeom(int w, int h) {
    MinimapGeom g;
    g.cx = static_cast<float>(std::max(w, 2) / 2);
    g.cy = static_cast<float>(std::max(h, 2) / 2);
    g.hw = static_cast<float>(std::max(w, 2)) * 0.5f;
    g.hh = static_cast<float>(std::max(h, 2)) * 0.5f;
    return g;
}

// Grundbild: Gelände × Nebel (weiche Ränder), außerhalb der Raute durchsichtig, innen deckend bis an den Rand – die
// ausgestanzte Raute des Leistenbilds darüber glättet die Kante (zusammen überall volle Deckkraft).
// Hängt nur von Kachelfarben, Kachelzuständen und Größe ab – der Aufrufer behält es, solange sich diese nicht ändern
// (Teil-Redraw), und zeichnet die Punkte auf eine Kopie (drawMinimapDots).
inline void rasterMinimapBase(const MinimapInput& in, int w, int h, std::vector<uint32_t>& out) {
    const int W = std::max(w, 2) + 1, H = std::max(h, 2) + 1;
    out.assign(static_cast<size_t>(W) * static_cast<size_t>(H), 0u);
    const MinimapGeom g = minimapImageGeom(w, h);
    const int nNe = std::max(in.ne, 1), nSe = std::max(in.se, 1);
    const uint32_t grass = mmRgba(74, 122, 58);
    // Festkomma 16.16 in Kacheln: je Pixel nach rechts wächst se um nSe/(2·hw), ne um nNe/(2·hw)
    // (toFrac ist linear in x: du = 1/(2·hw), dv = −1/(2·hw), ne = (1 − v)·nNe)
    const double fx_one = 65536.0;
    const int32_t dSe = static_cast<int32_t>(std::lround(0.5 / g.hw * nSe * fx_one));
    const int32_t dNe = static_cast<int32_t>(std::lround(0.5 / g.hw * nNe * fx_one));
    const int32_t maxSe = nSe * 65536 - 1, maxNe = nNe * 65536 - 1;
    // Helligkeit je Kachelzustand (0 … 256): unerkundet schwarz, erkundet halb, sichtbar voll
    auto bright = [&](int tx, int ty) -> int32_t {
        tx = std::clamp(tx, 0, nNe - 1);
        ty = std::clamp(ty, 0, nSe - 1);
        const uint8_t s = in.fog[static_cast<size_t>(tx) + static_cast<size_t>(ty) * static_cast<size_t>(nNe)];
        return s >= 2 ? 256 : s == 1 ? 192 : 0;
    };
    for (int y = 0; y < H; ++y) {
        const float py = static_cast<float>(y) + 0.5f;
        const float ry = 1.0f - std::fabs(py - g.cy) / g.hh;  // halbe Zeilenbreite in Anteilen von hw
        if (ry < -2.0f / g.hw) continue;
        const int xa = std::max(0, static_cast<int>(std::floor(g.cx - ry * g.hw - 2.0f)));
        const int xb = std::min(W, static_cast<int>(std::ceil(g.cx + ry * g.hw + 2.0f)));
        float u0, v0;
        g.toFrac(static_cast<float>(xa) + 0.5f, py, u0, v0);
        int32_t se = static_cast<int32_t>(std::lround(static_cast<double>(u0) * nSe * fx_one));
        int32_t ne = static_cast<int32_t>(std::lround((1.0 - static_cast<double>(v0)) * nNe * fx_one));
        uint32_t* row = out.data() + static_cast<size_t>(y) * static_cast<size_t>(W);
        for (int x = xa; x < xb; ++x, se += dSe, ne += dNe) {
            // Rand: das Bild ist innen deckend, die ausgestanzte Leiste darüber glättet die Kante
            if (g.edge(static_cast<float>(x) + 0.5f, py) >= 0.5f) continue;
            const int32_t cse = std::clamp(se, 0, maxSe), cne = std::clamp(ne, 0, maxNe);
            const int tn = cne >> 16, ts = cse >> 16;
            uint32_t c = in.terrain ? in.terrain[static_cast<size_t>(tn) + static_cast<size_t>(ts) * static_cast<size_t>(nNe)]
                                    : grass;
            if (in.fog) {
                // bilinear zwischen den Kachelmitten (weiche Ränder), Gewichte 0 … 256
                const int32_t X = cne - 32768, Y = cse - 32768;
                const int x0 = X >> 16, y0 = Y >> 16;
                const int32_t fxw = (X >> 8) & 0xFF, fyw = (Y >> 8) & 0xFF;
                const int32_t a = bright(x0, y0) * (256 - fxw) + bright(x0 + 1, y0) * fxw;
                const int32_t b = bright(x0, y0 + 1) * (256 - fxw) + bright(x0 + 1, y0 + 1) * fxw;
                const uint32_t f = static_cast<uint32_t>((a * (256 - fyw) + b * fyw) >> 16);  // 0 … 256
                const uint32_t r = ((c & 0xFFu) * f) >> 8, gg = (((c >> 8) & 0xFFu) * f) >> 8, bb = (((c >> 16) & 0xFFu) * f) >> 8;
                c = r | (gg << 8) | (bb << 16);
            }
            row[x] = (c & 0x00FFFFFFu) | 0xFF000000u;
        }
    }
}

// Punkte (Ressourcen dezent, Gebäude als Quadrat ihres Fußabdrucks, Einheiten mindestens dotPx groß) und Signal-Ping
// auf ein Grundbild derselben Größe zeichnen.
inline void drawMinimapDots(const MinimapInput& in, int w, int h, float dotPx, std::vector<uint32_t>& img) {
    const int W = std::max(w, 2) + 1, H = std::max(h, 2) + 1;
    if (img.size() != static_cast<size_t>(W) * static_cast<size_t>(H)) return;
    const MinimapGeom g = minimapImageGeom(w, h);
    const int nNe = std::max(in.ne, 1), nSe = std::max(in.se, 1);
    const float tileW = 2.0f * g.hw / static_cast<float>(std::max(nNe, nSe));  // Rautenbreite einer Kachel
    auto plot = [&](float fx, float fy, float half, uint32_t color, float alpha) {
        const int x0 = static_cast<int>(std::floor(fx - half)), x1 = static_cast<int>(std::ceil(fx + half));
        const int y0 = static_cast<int>(std::floor(fy - half)), y1 = static_cast<int>(std::ceil(fy + half));
        for (int y = std::max(y0, 0); y < std::min(y1, H); ++y) {
            for (int x = std::max(x0, 0); x < std::min(x1, W); ++x) {
                uint32_t& dst = img[static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x)];
                const uint32_t da = dst >> 24;
                if (da == 0) continue;  // nur innerhalb der Raute
                // Abdeckung des Quadrats im Pixel (Kanten geglättet)
                const float ox = std::clamp(std::min(static_cast<float>(x) + 1.0f, fx + half) - std::max(static_cast<float>(x), fx - half), 0.0f, 1.0f);
                const float oy = std::clamp(std::min(static_cast<float>(y) + 1.0f, fy + half) - std::max(static_cast<float>(y), fy - half), 0.0f, 1.0f);
                const float t = ox * oy * alpha;
                if (t <= 0.0f) continue;
                dst = (mmMix(dst, color, t) & 0x00FFFFFFu) | (da << 24);
            }
        }
    };
    for (size_t i = 0; i < in.dotCount; ++i) {
        const MinimapDot& d = in.dots[i];
        const uint32_t color = minimapDotColor(d, in);
        if (color == 0) continue;
        float u, v, x, y;
        minimapTileToFrac(d.ne, d.se, nNe, nSe, u, v);
        g.toPixel(u, v, x, y);
        float half = 0.5f * dotPx;
        float alpha = 1.0f;
        switch (d.kind) {
        case kDotBuilding:
            half = std::max(0.75f * dotPx, 0.5f * tileW * std::max(d.radius, 0.5f) * 1.2f);
            break;
        case kDotTree:
            half = std::max(0.5f, 0.35f * tileW);
            alpha = 0.55f;
            break;
        case kDotGold:
        case kDotStone:
        case kDotFood:
            half = std::max(0.5f * dotPx, 0.4f * tileW);
            alpha = 0.9f;
            break;
        default:
            break;
        }
        plot(x, y, half, color, alpha);
    }
    // Signal-Ping: Ring, der sich 3 s lang jede Sekunde neu ausbreitet
    if (in.pingAge >= 0.0f && in.pingAge < 3.0f) {
        float x, y;
        g.toPixel(in.pingU, in.pingV, x, y);
        const float t = std::fmod(in.pingAge, 1.0f);
        const float r = (0.3f + 1.7f * t) * 4.0f * dotPx;
        const float fade = 1.0f - in.pingAge / 3.0f;
        const int x0 = static_cast<int>(x - r - 3), x1 = static_cast<int>(x + r + 3);
        const int y0 = static_cast<int>(y - r - 3), y1 = static_cast<int>(y + r + 3);
        for (int py = std::max(y0, 0); py < std::min(y1, H); ++py) {
            for (int px = std::max(x0, 0); px < std::min(x1, W); ++px) {
                uint32_t& dst = img[static_cast<size_t>(py) * static_cast<size_t>(W) + static_cast<size_t>(px)];
                const uint32_t da = dst >> 24;
                if (da == 0) continue;
                const float dx = static_cast<float>(px) + 0.5f - x, dy = (static_cast<float>(py) + 0.5f - y) * 2.0f;
                const float dd = std::fabs(std::sqrt(dx * dx + dy * dy) - r) - std::max(1.0f, 0.6f * dotPx);
                const float cov = std::clamp(0.5f - dd, 0.0f, 1.0f) * fade;
                if (cov > 0.0f) dst = (mmMix(dst, mmRgba(255, 255, 255), cov) & 0x00FFFFFFu) | (da << 24);
            }
        }
    }
}

// Ganzes Bild in einem Schritt (Tests, Vorschau): Grundbild + Punkte.
inline void rasterMinimap(const MinimapInput& in, int w, int h, float dotPx, std::vector<uint32_t>& out) {
    rasterMinimapBase(in, w, h, out);
    drawMinimapDots(in, w, h, dotPx, out);
}

}  // namespace agesxr
