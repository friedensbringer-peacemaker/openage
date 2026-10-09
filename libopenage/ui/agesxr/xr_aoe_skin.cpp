// xr.ages — Vektor-Skin der AoE-Oberfläche, Implementierung. Siehe xr_aoe_skin.h.
#include "xr_aoe_skin.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "xr_hud.h"

namespace agesxr {

using namespace aoepal;

namespace {

int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
int sc(float s, int v) { return std::max(1, static_cast<int>(std::lround(static_cast<float>(v) * s))); }

}  // namespace

VectorSkin::VectorSkin() {
    // splitmix-artiges Rauschen, deterministisch (nichts flimmert, Teil-Redraw bleibt pixelgleich)
    uint64_t z = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < kTile * kTile; ++i) {
        z += 0x9E3779B97F4A7C15ull;
        uint64_t x = z;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        x ^= x >> 31;
        mNoise[i] = static_cast<uint8_t>(x & 0xFF);
    }
}

uint32_t VectorSkin::lighten(uint32_t color, float factor) {
    uint32_t out = color & 0xFF000000u;
    for (int sh = 0; sh < 24; sh += 8) {
        const int ch = static_cast<int>((color >> sh) & 0xFF);
        int v = factor >= 0.0f ? ch + static_cast<int>((255 - ch) * factor) : ch + static_cast<int>(ch * factor);
        out |= static_cast<uint32_t>(clampi(v, 0, 255)) << sh;
    }
    return out;
}

void VectorSkin::frame(Canvas& c, const AoeRect& r, int width, uint32_t color) {
    if (r.empty() || width <= 0) return;
    c.fillRect(r.x0, r.y0, r.x1, r.y0 + width, color);
    c.fillRect(r.x0, r.y1 - width, r.x1, r.y1, color);
    c.fillRect(r.x0, r.y0 + width, r.x0 + width, r.y1 - width, color);
    c.fillRect(r.x1 - width, r.y0 + width, r.x1, r.y1 - width, color);
}

void VectorSkin::bevelRect(Canvas& c, const AoeRect& r, uint32_t fill, int bevel, bool sunken) {
    if (r.empty()) return;
    c.fillRect(r.x0, r.y0, r.x1, r.y1, fill);
    if (bevel <= 0) return;
    const uint32_t light = lighten(fill, 0.25f), dark = lighten(fill, -0.30f);
    const uint32_t tl = sunken ? dark : light, br = sunken ? light : dark;
    c.fillRect(r.x0, r.y0, r.x1, r.y0 + bevel, tl);
    c.fillRect(r.x0, r.y0, r.x0 + bevel, r.y1, tl);
    c.fillRect(r.x0, r.y1 - bevel, r.x1, r.y1, br);
    c.fillRect(r.x1 - bevel, r.y0, r.x1, r.y1, br);
}

void VectorSkin::parchment(Canvas& c, const AoeRect& r, uint32_t base) {
    if (mParchmentTile.empty() || mParchmentBase != base) {
        mParchmentTile.resize(static_cast<size_t>(kTile * kTile));
        for (int i = 0; i < kTile * kTile; ++i) {
            // Rauschen 4 %: −0,02 … +0,02 um die Grundfarbe
            const float n = (static_cast<float>(mNoise[i]) / 255.0f - 0.5f) * 0.04f;
            mParchmentTile[static_cast<size_t>(i)] = lighten(base, n) | 0xFF000000u;
        }
        mParchmentBase = base;
    }
    const int ya = std::max(r.y0, c.clipY0()), yb = std::min(r.y1, c.clipY1());
    const int xa = std::max(r.x0, 0), xb = std::min(r.x1, c.width());
    for (int y = ya; y < yb; ++y) {
        uint32_t* row = c.data() + static_cast<size_t>(y) * static_cast<size_t>(c.width());
        const uint32_t* tile = mParchmentTile.data() + static_cast<size_t>(y % kTile) * kTile;
        for (int x = xa; x < xb; ++x) row[x] = tile[x % kTile];
    }
}

const VectorSkin::Mask& VectorSkin::iconMask(HudIcon icon, float scale, int offset) {
    uint32_t sbits;
    std::memcpy(&sbits, &scale, sizeof(sbits));
    const uint64_t key = (1ull << 62) | (static_cast<uint64_t>(icon) << 40) | (static_cast<uint64_t>(offset & 0xFF) << 32) | sbits;
    auto it = mMasks.find(key);
    if (it != mMasks.end()) return it->second;
    Mask m;
    const int half = static_cast<int>(std::ceil(20.0f * scale));
    m.x0 = -half;
    m.y0 = -half;
    m.w = 2 * half + 3;
    m.h = 2 * half + 3;
    m.cov.resize(static_cast<size_t>(m.w) * static_cast<size_t>(m.h));
    const float o = static_cast<float>(offset);
    for (int y = 0; y < m.h; ++y)
        for (int x = 0; x < m.w; ++x) {
            const float px = static_cast<float>(m.x0 + x) + 0.5f, py = static_cast<float>(m.y0 + y) + 0.5f;
            const float d = hudIconShape(icon, (px - o) / scale, (py - o) / scale) * scale;
            const float cov = std::clamp(0.5f - d, 0.0f, 1.0f);
            m.cov[static_cast<size_t>(y) * static_cast<size_t>(m.w) + static_cast<size_t>(x)] =
                static_cast<uint8_t>(std::min(cov, 1.0f) * 255.0f + 0.5f);
        }
    return mMasks.emplace(key, std::move(m)).first->second;
}

const VectorSkin::Mask& VectorSkin::diamondMask(int w, int h, int inset) {
    const uint64_t key = (2ull << 62) | (static_cast<uint64_t>(w & 0xFFFF) << 32) | (static_cast<uint64_t>(h & 0xFFFF) << 16) |
                         static_cast<uint64_t>(inset & 0xFFFF);
    auto it = mMasks.find(key);
    if (it != mMasks.end()) return it->second;
    Mask m;
    const float hw = static_cast<float>(w) * 0.5f, hh = static_cast<float>(h) * 0.5f;
    const int grow = std::max(0, -inset);
    m.x0 = -w / 2 - grow;
    m.y0 = -h / 2 - grow;
    m.w = w + 2 * grow + 1;
    m.h = h + 2 * grow + 1;
    m.cov.resize(static_cast<size_t>(m.w) * static_cast<size_t>(m.h));
    for (int y = 0; y < m.h; ++y)
        for (int x = 0; x < m.w; ++x) {
            const float dx = std::fabs(static_cast<float>(m.x0 + x) + 0.5f), dy = std::fabs(static_cast<float>(m.y0 + y) + 0.5f);
            const float d = (dx / hw + dy / hh - 1.0f) * std::min(hw, hh) * 0.7071f + static_cast<float>(inset);
            const float cov = std::clamp(0.5f - d, 0.0f, 1.0f);
            m.cov[static_cast<size_t>(y) * static_cast<size_t>(m.w) + static_cast<size_t>(x)] =
                static_cast<uint8_t>(cov * 255.0f + 0.5f);
        }
    return mMasks.emplace(key, std::move(m)).first->second;
}

uint32_t VectorSkin::biomeColor(int biome) {
    switch (biome) {
    case 9: return kMapDesert;              // Wüste
    case 10: return kMapWinter;             // Winter
    case 1: return rgba(0x8A, 0x8A, 0x3A, 255);   // Steppe
    case 5: case 6: case 7: return kMapWater;     // Küste, Binnenmeer, Inseln
    case 8: return rgba(0x9A, 0x8A, 0x4A, 255);   // Goldrausch
    case 11: return rgba(0x2E, 0x6A, 0x2A, 255);  // Dschungel
    case 3: return rgba(0x3A, 0x62, 0x2E, 255);   // Wald
    default: return kMapGrass;
    }
}

void VectorSkin::drawPanel(Canvas& c, SkinPanel kind, const AoeRect& r, float s) {
    if (r.empty()) return;
    const int b2 = sc(s, 2), b3 = sc(s, 3), b4 = sc(s, 4);
    switch (kind) {
    case SkinPanel::kTopBar:
        c.fillRect(r.x0, r.y0, r.x1, r.y1, kWoodMid);
        // Holzmaserung: 3 waagerechte Linien α 15 % je 16 px
        for (int y = r.y0 + sc(s, 8); y < r.y1 - b2; y += sc(s, 16)) c.fillRect(r.x0, y, r.x1, y + 1, rgba(0, 0, 0, 38));
        c.fillRect(r.x0, r.y1 - b2, r.x1, r.y1, kGold);
        break;
    case SkinPanel::kCommands:
    case SkinPanel::kMinimapField:
        c.fillRect(r.x0, r.y0, r.x1, r.y1, kStoneMid);
        for (int y = r.y0 + sc(s, 20); y < r.y1; y += sc(s, 40)) c.fillRect(r.x0, y, r.x1, y + 1, rgba(0, 0, 0, 25));
        frame(c, r, b3, kGold);
        break;
    case SkinPanel::kSelection:
        parchment(c, r, kParchment);
        frame(c, r, b3, kGold);
        break;
    case SkinPanel::kDialog:
        parchment(c, r, kParchment);
        frame(c, r, b4, kWoodDark);
        frame(c, {r.x0 + b4, r.y0 + b4, r.x1 - b4, r.y1 - b4}, b2, kGold);
        break;
    case SkinPanel::kContext: {
        // Schlagschatten 8 px α 40 %
        const int sh = sc(s, 8);
        c.fillRect(r.x0 + sh, r.y0 + sh, r.x1 + sh, r.y1 + sh, rgba(0, 0, 0, 102));
        parchment(c, r, kParchment);
        frame(c, r, b3, kGold);
        break;
    }
    case SkinPanel::kTitlePlate:
    case SkinPanel::kTitlePlateLight:
        bevelRect(c, r, kind == SkinPanel::kTitlePlate ? kWoodMid : kWoodLight, b2);
        frame(c, r, b2, kGold);
        break;
    case SkinPanel::kCapsule:
        c.roundRect(r.x0, r.y0, r.x1, r.y1, sc(s, 6), kCapsule);
        break;
    case SkinPanel::kPortrait:
        c.fillRect(r.x0, r.y0, r.x1, r.y1, kStoneLight);
        frame(c, r, b3, kGold);
        break;
    case SkinPanel::kValueCell:
        bevelRect(c, r, kParchmentDark, b2, true);
        break;
    case SkinPanel::kBackdrop:
        c.fillRect(r.x0, r.y0, r.x1, r.y1, kDimBackdrop);
        break;
    }
}

void VectorSkin::drawButton(Canvas& c, SkinButton kind, const AoeRect& r, SkinState st, float s) {
    if (r.empty()) return;
    const int b2 = sc(s, 2), b1 = 1;
    uint32_t fill;
    switch (kind) {
    case SkinButton::kCommand: fill = kStoneLight; break;
    case SkinButton::kDialog: fill = kWoodLight; break;
    case SkinButton::kDanger: fill = kDanger; break;
    case SkinButton::kArrow: fill = kWoodLight; break;
    case SkinButton::kMenuTop: fill = kWoodLight; break;
    case SkinButton::kSlot: fill = kStoneLight; break;
    case SkinButton::kSlotEmpty: fill = kParchmentDark; break;
    case SkinButton::kCtxItem: fill = kParchment; break;
    case SkinButton::kBoardBtn: fill = kWoodLight; break;
    default: fill = kStoneLight; break;
    }
    if (st == SkinState::kDisabled) fill = kind == SkinButton::kSlotEmpty ? kParchmentDark : kDisabled;
    else if (st == SkinState::kHover) fill = kind == SkinButton::kCtxItem ? kParchmentShadow : lighten(fill, 0.12f);
    else if (st == SkinState::kPressed) fill = lighten(fill, -0.15f);
    const bool sunken = st == SkinState::kPressed || kind == SkinButton::kSlotEmpty;
    bevelRect(c, r, fill, kind == SkinButton::kCtxItem ? b1 : b2, sunken);
    if (kind == SkinButton::kCtxItem) frame(c, r, b1, kGoldDark);
    else if (kind != SkinButton::kSlotEmpty) frame(c, r, b1, st == SkinState::kDisabled ? kStoneDark : kGoldDark);
    if (st == SkinState::kPressed) frame(c, {r.x0 + b2, r.y0 + b2, r.x1 - b2, r.y1 - b2}, b1, rgba(0, 0, 0, 255));
    if (st == SkinState::kHover && kind != SkinButton::kCtxItem) drawHoverFrame(c, r, s);
}

void VectorSkin::drawIcon(Canvas& c, HudIcon icon, int cx, int cy, float scale, SkinState st) {
    if (icon == HudIcon::None) return;
    const uint32_t color = st == SkinState::kDisabled ? rgba(0xB0, 0xB0, 0xB0, 128) : kWhite;
    for (int pass = st == SkinState::kDisabled ? 1 : 0; pass < 2; ++pass) {
        const int off = pass == 0 ? std::max(1, static_cast<int>(scale)) : 0;
        const Mask& m = iconMask(icon, scale, off);
        c.blendMask(cx + m.x0, cy + m.y0, m.w, m.h, m.cov.data(), pass == 0 ? kShadow : color);
    }
}

void VectorSkin::drawBar(Canvas& c, SkinBar kind, const AoeRect& r, float fraction, float s) {
    if (r.empty()) return;
    const float f = std::clamp(fraction, 0.0f, 1.0f);
    c.fillRect(r.x0, r.y0, r.x1, r.y1, kBarTrack);
    const int fill = r.x0 + static_cast<int>(std::lround(f * static_cast<float>(r.w())));
    uint32_t color = kProgress;
    if (kind == SkinBar::kHp) color = f >= 0.5f ? kHpGood : f >= 0.25f ? kHpMid : kHpLow;
    if (fill > r.x0) c.fillRect(r.x0, r.y0, fill, r.y1, color);
    frame(c, r, sc(s, 1), kInk);
}

void VectorSkin::drawFocus(Canvas& c, const AoeRect& r, float s) {
    const int w = sc(s, 3);
    frame(c, r.grown(w), w, kFocus);
}

void VectorSkin::drawHoverFrame(Canvas& c, const AoeRect& r, float s) {
    frame(c, r, sc(s, 3), kGold);
}

void VectorSkin::drawMinimap(Canvas& c, int cx, int cy, int w, int h, int biome, float camX, float camY, float camW,
                             float camH, float s) {
    const float hw = static_cast<float>(w) * 0.5f, hh = static_cast<float>(h) * 0.5f;
    const float fcx = static_cast<float>(cx), fcy = static_cast<float>(cy);
    // Raute (|dx|/hw + |dy|/hh ≤ 1) als zwischengespeicherte Maske: Goldrand, dann Landschaftsfarbe
    const int gold = sc(s, 3);
    const Mask& outer = diamondMask(w, h, -gold);
    c.blendMask(cx + outer.x0, cy + outer.y0, outer.w, outer.h, outer.cov.data(), kGold);
    const Mask& inner = diamondMask(w, h, 0);
    c.blendMask(cx + inner.x0, cy + inner.y0, inner.w, inner.h, inner.cov.data(), biomeColor(biome));
    // Kameratrapez: Kartenanteile (u entlang NE-Achse, v entlang SE-Achse) auf die Raute abgebildet
    auto toPixel = [&](float u, float v, float& x, float& y) {
        x = fcx + (u - v) * hw;
        y = fcy + (u + v - 1.0f) * hh;
    };
    const float u0 = std::clamp(camX - camW * 0.5f, 0.0f, 1.0f), u1 = std::clamp(camX + camW * 0.5f, 0.0f, 1.0f);
    const float v0 = std::clamp(camY - camH * 0.5f, 0.0f, 1.0f), v1 = std::clamp(camY + camH * 0.5f, 0.0f, 1.0f);
    float px[4], py[4];
    toPixel(u0, v0, px[0], py[0]);
    toPixel(u1, v0, px[1], py[1]);
    toPixel(u1, v1, px[2], py[2]);
    toPixel(u0, v1, px[3], py[3]);
    const float lw = std::max(1.0f, 2.0f * s);
    int minx = c.width(), miny = c.height(), maxx = 0, maxy = 0;
    for (int i = 0; i < 4; ++i) {
        minx = std::min(minx, static_cast<int>(px[i]) - 3);
        maxx = std::max(maxx, static_cast<int>(px[i]) + 4);
        miny = std::min(miny, static_cast<int>(py[i]) - 3);
        maxy = std::max(maxy, static_cast<int>(py[i]) + 4);
    }
    c.fillSdf(minx, miny, maxx, maxy, kWhite, [&](float x, float y) {
        float d = 1e9f;
        for (int i = 0; i < 4; ++i) {
            const int j = (i + 1) % 4;
            d = std::min(d, sdf::segment(x, y, px[i], py[i], px[j], py[j], lw * 0.5f));
        }
        return d;
    });
}

uint32_t VectorSkin::inkOn(SkinPanel panel, SkinState st) const {
    if (st == SkinState::kDisabled) return kDim;
    switch (panel) {
    case SkinPanel::kSelection:
    case SkinPanel::kDialog:
    case SkinPanel::kContext:
    case SkinPanel::kValueCell:
        return kInk;
    default:
        return kWhite;
    }
}

bool VectorSkin::shadowOn(SkinPanel panel) const {
    return !(panel == SkinPanel::kSelection || panel == SkinPanel::kDialog || panel == SkinPanel::kContext ||
             panel == SkinPanel::kValueCell);
}

}  // namespace agesxr
