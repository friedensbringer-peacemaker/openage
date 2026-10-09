// xr.ages — VR-HUD, Implementierung. Siehe xr_hud.h.
#include "xr_hud.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace agesxr {

using namespace hudlayout;
using namespace palette;
using namespace sdf;

namespace {

// Symbolfarben (Rohstoffe wie gewohnt eingefärbt, Befehle cremeweiß wie die Schrift).
constexpr uint32_t kFoodColor = rgba(232, 84, 66, 255);
constexpr uint32_t kWoodColor = rgba(226, 176, 112, 255);
constexpr uint32_t kGoldColor = rgba(252, 206, 64, 255);
constexpr uint32_t kStoneColor = rgba(196, 198, 210, 255);
constexpr uint32_t kSlotEmpty = rgba(36, 37, 56, 110);      // leerer Befehlsplatz (gedämpfte Mulde)
constexpr uint32_t kHpGood = rgba(110, 206, 60, 255);
constexpr uint32_t kHpMid = rgba(240, 196, 60, 255);
constexpr uint32_t kHpLow = rgba(226, 70, 52, 255);
constexpr uint32_t kSeparator = rgba(64, 38, 16, 150);
constexpr uint32_t kProgress = rgba(90, 170, 240, 255);   // Ausbildung/Bau (blau, unterscheidbar vom HP-Balken)

uint32_t resourceColor(HudIcon i) {
    switch (i) {
    case HudIcon::Food: return kFoodColor;
    case HudIcon::Wood: return kWoodColor;
    case HudIcon::Gold: return kGoldColor;
    case HudIcon::Stone: return kStoneColor;
    default: return kText;
    }
}

}  // namespace

// Abstandsfunktion des Symbols in Symbolkoordinaten (≈ −16 … +16, +y unten wie im Bild), negativ = innen.
// Auch die AoE-Oberfläche (xr_aoe_ui.cpp) zeichnet ihre Vektorsymbole hiermit.
float hudIconShape(HudIcon icon, float px, float py) {
    switch (icon) {
    case HudIcon::Food:  // Apfel mit Stiel und Blatt
        return std::min({lengthOf(px, py - 3.0f) - 11.0f, segment(px, py, 0, -8, 1, -14, 1.6f),
                         segment(px, py, 2, -11, 8, -14, 2.4f)});
    case HudIcon::Wood:  // zwei gestapelte Stämme mit Jahresring
        return std::min({box(px, py - 5.5f, 14.0f, 5.0f, 5.0f), box(px, py + 6.0f, 14.0f, 5.0f, 5.0f)});
    case HudIcon::Gold:  // Münzstapel
        return std::min({lengthOf(px + 7.0f, py - 5.0f) - 7.0f, lengthOf(px - 7.0f, py - 5.0f) - 7.0f,
                         lengthOf(px, py + 6.0f) - 7.0f});
    case HudIcon::Stone:  // Steinblock mit Brocken obenauf
        return std::min(box(px, py - 4.0f, 13.0f, 8.0f, 4.0f), lengthOf(px + 3.0f, py + 6.0f) - 7.5f);
    case HudIcon::Population:
    case HudIcon::Villager: {  // Kopf + Oberkörper (Dorfbewohner mit Hut)
        float d = std::min(lengthOf(px, py + 8.0f) - 5.5f,
                           std::max(box(px, py - 8.0f, 10.0f, 7.0f, 6.0f), -(py - 1.0f)));
        if (icon == HudIcon::Villager) {
            static const float hat[6] = {-10.0f, -12.0f, 10.0f, -12.0f, 0.0f, -18.0f};
            d = std::min(d, triangle(px, py, hat));
        }
        return d;
    }
    case HudIcon::Clock:
        return std::min({ring(px, py, 12.0f, 3.0f), segment(px, py, 0, 0, 0, -7, 1.6f), segment(px, py, 0, 0, 6, 0, 1.6f)});
    case HudIcon::House: {
        static const float roof[6] = {-15.0f, -1.0f, 15.0f, -1.0f, 0.0f, -15.0f};
        const float body = std::max(box(px, py - 7.0f, 10.5f, 8.0f, 1.0f), -box(px, py - 10.0f, 3.0f, 5.0f, 1.0f));
        return std::min(triangle(px, py, roof), body);
    }
    case HudIcon::Sword:
        return std::min({segment(px, py, -9, 9, 11, -11, 2.4f), segment(px, py, -11, 3, -3, 11, 2.0f),
                         segment(px, py, -9, 9, -14, 14, 2.6f)});
    case HudIcon::Bow:  // Bogen (Halbkreis) mit Sehne und Pfeil
        return std::min({std::max(ring(px + 5.0f, py, 14.0f, 3.0f), -(px + 5.0f)), segment(px, py, -5, -14, -5, 14, 1.0f),
                         segment(px, py, -12, 0, 12, 0, 1.4f)});
    case HudIcon::Horse: {  // Pferdekopf
        static const float neck[6] = {-9.0f, 14.0f, 5.0f, 14.0f, 1.0f, -10.0f};
        static const float head[6] = {0.0f, -12.0f, 14.0f, -2.0f, 7.0f, 3.0f};
        return std::min({triangle(px, py, neck) - 1.5f, triangle(px, py, head) - 1.5f, segment(px, py, 0, -12, -2, -17, 2.0f)});
    }
    case HudIcon::Tower:
        return std::min({box(px, py - 3.0f, 8.0f, 11.0f, 1.0f), box(px + 6.0f, py + 11.0f, 2.5f, 3.0f, 0.5f),
                         box(px, py + 11.0f, 2.5f, 3.0f, 0.5f), box(px - 6.0f, py + 11.0f, 2.5f, 3.0f, 0.5f)});
    case HudIcon::Hammer:
        return std::min(segment(px, py, -10, 11, 3, -2, 2.2f), segment(px, py, -2, -11, 10, 1, 4.2f));
    case HudIcon::Stop:
        return std::min(segment(px, py, -9, -9, 9, 9, 3.0f), segment(px, py, -9, 9, 9, -9, 3.0f));
    case HudIcon::Mill: {  // Windmühle: Turm + vier Flügel
        static const float body[6] = {-6.0f, 14.0f, 6.0f, 14.0f, 0.0f, -4.0f};
        return std::min({triangle(px, py, body) - 1.0f, segment(px, py, -12, -14, 12, 10, 1.8f),
                         segment(px, py, 12, -14, -12, 10, 1.8f)});
    }
    case HudIcon::LumberCamp:  // Stamm mit Axt
        return std::min({box(px, py + 8.0f, 14.0f, 5.0f, 4.0f), segment(px, py, -4, 2, 8, -12, 2.0f),
                         box(px + 9.0f, py - 12.0f, 5.0f, 3.5f, 1.0f)});
    case HudIcon::MiningCamp:  // Spitzhacke über Stein
        return std::min({segment(px, py, -10, 12, 8, -8, 2.0f), segment(px, py, 0, -14, 14, -2, 2.6f),
                         lengthOf(px + 8.0f, py + 10.0f) - 5.0f});
    case HudIcon::Barracks: {  // Haus mit Fahne
        static const float roof[6] = {-15.0f, 2.0f, 15.0f, 2.0f, 0.0f, -8.0f};
        static const float flag[6] = {2.0f, -17.0f, 12.0f, -14.0f, 2.0f, -11.0f};
        return std::min({triangle(px, py, roof), box(px, py + 8.0f, 11.0f, 6.0f, 1.0f),
                         segment(px, py, 1, -17, 1, -8, 1.2f), triangle(px, py, flag)});
    }
    case HudIcon::Range:  // Zielscheibe mit Pfeil
        return std::min({ring(px, py, 13.0f, 3.0f), ring(px, py, 6.0f, 3.0f), lengthOf(px, py) - 2.0f,
                         segment(px, py, 4, -4, 15, -15, 1.6f)});
    case HudIcon::Stable:  // Hufeisen
        return std::max(ring(px, py - 1.0f, 11.0f, 5.0f), -(py + 6.0f));
    case HudIcon::Wall:  // Mauer aus Steinen
        return std::min({box(px, py - 8.0f, 15.0f, 3.5f, 1.0f), box(px - 8.0f, py, 6.5f, 3.5f, 1.0f),
                         box(px + 8.0f, py, 6.5f, 3.5f, 1.0f), box(px, py + 8.0f, 15.0f, 3.5f, 1.0f)});
    case HudIcon::Halt: {  // Achteck (Stoppschild) mit Balken
        const float a = std::fabs(px), b = std::fabs(py);
        const float oct = std::max(std::max(a, b), (a + b) * 0.7071f) - 13.0f;
        return std::max(oct, -box(px, py, 8.0f, 2.0f, 0.5f));
    }
    case HudIcon::Garrison: {  // Haus mit Pfeil hinein
        static const float roof[6] = {-15.0f, -2.0f, 15.0f, -2.0f, 0.0f, -15.0f};
        const float body = std::max(box(px, py + 6.0f, 11.0f, 7.0f, 1.0f), -box(px, py + 7.0f, 4.5f, 6.0f, 0.5f));
        static const float head[6] = {0.0f, 2.0f, 0.0f, 12.0f, -7.0f, 7.0f};
        return std::min({triangle(px, py, roof), body, segment(px, py, -14, 7, 0, 7, 1.6f), triangle(px, py, head)});
    }
    case HudIcon::Unload: {  // Tür mit Pfeil heraus
        static const float head[6] = {14.0f, 0.0f, 5.0f, -7.0f, 5.0f, 7.0f};
        return std::min({std::max(box(px - 6.0f, py, 8.0f, 13.0f, 1.0f), -box(px - 6.0f, py, 5.0f, 10.0f, 0.5f)),
                         segment(px, py, -4, 0, 8, 0, 1.8f), triangle(px, py, head)});
    }
    case HudIcon::Cancel:  // Kreis mit Kreuz
        return std::min({ring(px, py, 13.0f, 3.0f), segment(px, py, -6, -6, 6, 6, 2.2f),
                         segment(px, py, -6, 6, 6, -6, 2.2f)});
    case HudIcon::Back: {  // Pfeil nach links
        static const float head[6] = {-14.0f, 0.0f, -2.0f, -10.0f, -2.0f, 10.0f};
        return std::min(triangle(px, py, head), box(px + 6.0f, py, 8.0f, 3.5f, 1.0f));
    }
    case HudIcon::Emblem: {  // Wappenschild
        const float top = box(px, py - 4.0f, 13.0f, 8.0f, 2.0f);
        static const float tip[6] = {-13.0f, 2.0f, 13.0f, 2.0f, 0.0f, 16.0f};
        return std::min(top, triangle(px, py, tip));
    }
    case HudIcon::Age:  // Plakette „I“
        return std::min(box(px, py, 2.5f, 11.0f, 0.5f),
                        std::min(box(px, py - 10.0f, 7.0f, 2.0f, 0.5f), box(px, py + 10.0f, 7.0f, 2.0f, 0.5f)));
    case HudIcon::Sheep:  // Schaf
        return std::min({box(px - 2.0f, py, 11.0f, 7.0f, 6.0f), lengthOf(px - 12.0f, py - 3.0f) - 4.5f,
                         box(px - 6.0f, py + 10.0f, 1.5f, 4.0f, 0.5f), box(px + 4.0f, py + 10.0f, 1.5f, 4.0f, 0.5f)});
    case HudIcon::Tree: {  // Nadelbaum
        static const float crown[6] = {-13.0f, 6.0f, 13.0f, 6.0f, 0.0f, -16.0f};
        return std::min(triangle(px, py, crown), box(px, py + 11.0f, 2.5f, 5.0f, 0.5f));
    }
    case HudIcon::None:
    case HudIcon::Count: break;
    }
    return 1e9f;
}

bool VrHud::init() {
    mOk = mFont.load(false);
    if (!mFontBold.load(true)) mFontBold.load(false);
    mCanvas.setFonts(&mFont, &mFontBold);
    mDirty = true;
    return mOk;
}

void VrHud::setDiagnostic(bool on) {
    if (mDiagnostic == on) return;
    mDiagnostic = on;
    mDirty = true;
}

int VrHud::buttonAt(float x, float y, int buttonCount) {
    const int n = std::min(buttonCount, kBtnCount);
    if (!(y >= static_cast<float>(kBotY0 - kHitPad) && y < static_cast<float>(kBotY1 + kHitPad))) return -1;
    if (!(x >= static_cast<float>(kBtnX0 - kHitPad))) return -1;
    for (int i = 0; i < n; ++i)
        if (x >= static_cast<float>(buttonX0(i) - kHitPad) && x < static_cast<float>(buttonX1(i) + kHitPad)) return i;
    return -1;
}

int VrHud::pointer(float x, float y, bool clickEdge, const HudModel& model, double now) {
    const int n = mDiagnostic ? 0 : static_cast<int>(model.buttons.size());
    mHover = x >= 0.0f && y >= 0.0f ? buttonAt(x, y, n) : -1;
    if (!clickEdge || mHover < 0 || !model.buttons[static_cast<size_t>(mHover)].enabled) return -1;
    mPressed = mHover;
    mPressedUntil = now + kPressSeconds;
    return mHover;
}

int VrHud::lastBandRows() const {
    int rows = 0;
    for (const DirtyBand& b : mLastBands) rows += b.y1 - b.y0;
    return rows;
}

const uint32_t* VrHud::pixels(const HudModel& model, double now) {
    mLastBands.clear();
    if (mPressed >= 0 && now >= mPressedUntil) mPressed = -1;
    const int n = std::min(static_cast<int>(model.buttons.size()), kBtnCount);
    if (mHover >= n) mHover = -1;
    if (mPressed >= n) mPressed = -1;
    const bool top = !model.sameTop(mShown);
    const bool bottom = !model.sameBottom(mShown) || mHover != mShownHover || mPressed != mShownPressed;
    if (mDirty) {
        paintBand(model, 0, kHeight);
        mLastBands.push_back({0, kHeight});
    } else if (top || bottom) {
        if (top) {
            paintBand(model, kTopBandY0, kTopBandY1);
            mLastBands.push_back({kTopBandY0, kTopBandY1});
        }
        if (bottom) {
            paintBand(model, kBotBandY0, kBotBandY1);
            mLastBands.push_back({kBotBandY0, kBotBandY1});
        }
    } else {
        return mCanvas.data();
    }
    mDirty = false;
    mShown = model;
    mShownHover = mHover;
    mShownPressed = mPressed;
    ++mVersion;
    return mCanvas.data();
}

void VrHud::paintBand(const HudModel& m, int y0, int y1) {
    mCanvas.setClip(y0, y1);
    mCanvas.clearBand(y0, y1);
    drawScene(m);
    mCanvas.resetClip();
}

std::string VrHud::fitText(const std::string& s, float& px, float minPx, int maxW, bool bold) const {
    while (px > minPx && mCanvas.textWidth(s.c_str(), px, bold) > maxW) px -= 1.0f;
    if (mCanvas.textWidth(s.c_str(), px, bold) <= maxW) return s;
    std::string cut = s;
    while (!cut.empty()) {
        size_t n = cut.size() - 1;  // ganzes UTF-8-Zeichen entfernen
        while (n > 0 && (static_cast<unsigned char>(cut[n]) & 0xC0) == 0x80) --n;
        cut.erase(n);
        const std::string t = cut + "…";
        if (mCanvas.textWidth(t.c_str(), px, bold) <= maxW) return t;
    }
    return "…";
}

void VrHud::drawIcon(HudIcon icon, int cx, int cy, float scale, uint32_t color, bool shadow) {
    if (icon == HudIcon::None) return;
    const int half = static_cast<int>(std::ceil(20.0f * scale));
    for (int pass = shadow ? 0 : 1; pass < 2; ++pass) {
        const float ox = static_cast<float>(cx + (pass == 0 ? 2 : 0)), oy = static_cast<float>(cy + (pass == 0 ? 2 : 0));
        const uint32_t c = pass == 0 ? (kInk & 0x00FFFFFFu) | 0xB0000000u : color;
        mCanvas.fillSdf(cx - half, cy - half, cx + half + 3, cy + half + 3, c, [&](float x, float y) {
            return hudIconShape(icon, (x - ox) / scale, (y - oy) / scale) * scale;  // Bildkoordinaten: +y unten
        });
    }
}

void VrHud::drawScene(const HudModel& m) {
    if (mDiagnostic) {
        // Diagnosezeile (0.6.0-xr.0.12): schmale Tafel, nur der Statustext (fps, Engine-Zeit, Abfrage-Latenz).
        mCanvas.roundRect(0, 0, kWidth, kDiagHeight, 20, kStoneRim);
        mCanvas.roundRectV(4, 4, kWidth - 4, kDiagHeight - 4, 16, kStoneTop, kStoneBottom);
        float px = kStatusPx;
        const std::string s = fitText(m.status.empty() ? "Diagnose" : m.status, px, 18.0f, kWidth - 48, false);
        mCanvas.textShadow(s.c_str(), (kWidth - mCanvas.textWidth(s.c_str(), px)) / 2,
                           (kDiagHeight - static_cast<int>(px)) / 2 - 2, px, kText);
        return;
    }
    // Steintafel wie das Menü (heller Rand, Verlauf).
    mCanvas.roundRect(0, 0, kWidth, kHeight, kRadius, kStoneRim);
    mCanvas.roundRectV(4, 4, kWidth - 4, kHeight - 4, kRadius - 4, kStoneTop, kStoneBottom);
    if (mCanvas.visible(kTopBandY0, kTopBandY1)) drawTop(m);
    if (mCanvas.visible(kBotBandY0, kBotBandY1)) drawBottom(m);
}

void VrHud::drawTop(const HudModel& m) {
    mCanvas.woodSign(kSignX0, kTopY0, kSignX1, kTopY1, 18);
    const HudIcon icons[kCells] = {HudIcon::Food, HudIcon::Wood, HudIcon::Gold, HudIcon::Stone, HudIcon::Population,
                                   HudIcon::Clock};
    const std::string values[kCells] = {hudAmountText(m.food),  hudAmountText(m.wood),
                                        hudAmountText(m.gold),  hudAmountText(m.stone),
                                        hudPopulationText(m.population, m.populationMax), hudTimeText(m.gameSeconds)};
    for (int i = 0; i < kCells; ++i) {
        const int x = cellX(i);
        if (i > 0) mCanvas.fillRect(x - 10, kTopY0 + 16, x - 8, kTopY1 - 16, kSeparator);
        drawIcon(icons[i], x + kCellIconDx, kTopCenterY, 0.95f, resourceColor(icons[i]));
        float px = kNumberPx;
        const std::string v = fitText(values[i], px, 22.0f, kCellW - kCellTextDx - 14, true);
        mCanvas.textShadow(v.c_str(), x + kCellTextDx, kTopCenterY - static_cast<int>(px) / 2 - 3, px, kText, true);
    }
    if (m.status.empty()) return;
    const uint32_t top = m.statusKind == HudStatus::kWarn ? kWarnTop : m.statusKind == HudStatus::kGood ? kLeafTop : kSlabTop;
    const uint32_t bottom =
        m.statusKind == HudStatus::kWarn ? kWarnBottom : m.statusKind == HudStatus::kGood ? kLeafBottom : kSlabBottom;
    const int y0 = kTopY0 + 12, y1 = kTopY1 - 14;
    mCanvas.slab(kStatusX0, y0, kStatusX1, y1, 16, top, bottom);
    float px = kStatusPx;
    const std::string s = fitText(m.status, px, 18.0f, kStatusX1 - kStatusX0 - 28, true);
    mCanvas.textShadow(s.c_str(), (kStatusX0 + kStatusX1 - mCanvas.textWidth(s.c_str(), px, true)) / 2,
                       (y0 + y1 - static_cast<int>(px)) / 2 - 2, px, kText, true);
}

void VrHud::drawBottom(const HudModel& m) {
    // Auswahlfeld: Steinmulde mit Symbol, Anzahl/Art und HP-Balken (Einzelauswahl).
    mCanvas.roundRect(kSelX0, kBotY0, kSelX1, kBotY1, 16, kStoneDeep);
    const HudSelection& sel = m.selection;
    if (sel.count <= 0) {
        const char* none = "Nichts ausgewählt";
        mCanvas.textShadow(none, (kSelX0 + kSelX1 - mCanvas.textWidth(none, kLabelPx + 2.0f)) / 2,
                           (kBotY0 + kBotY1) / 2 - 14, kLabelPx + 2.0f, kTextDim);
    } else {
        drawIcon(sel.icon == HudIcon::None ? HudIcon::Villager : sel.icon, kSelX0 + 34, kBotY0 + 28, 1.0f, kText);
        std::string kind = sel.kind.empty() ? "Einheit" : sel.kind;
        if (sel.count > 1) kind = std::to_string(sel.count) + " × " + kind;
        float px = kKindPx;
        const int tx = kSelX0 + 64;
        kind = fitText(kind, px, 20.0f, kSelX1 - 14 - tx, true);
        mCanvas.textShadow(kind.c_str(), tx, kBotY0 + 28 - static_cast<int>(px) / 2 - 3, px, kText, true);
        if (sel.progress >= 0.0f) {
            // Ausbildung/Bau: blauer Fortschrittsbalken, darunter die Zeile detail (statt der HP-Zahlen).
            const float f = std::clamp(sel.progress, 0.0f, 1.0f);
            mCanvas.roundRect(kHpX0, kHpY0, kHpX1, kHpY1, 6, kBarTrack);
            const int fill = kHpX0 + static_cast<int>(std::lround(f * static_cast<float>(kHpX1 - kHpX0)));
            if (fill > kHpX0 + 1) mCanvas.roundRect(kHpX0, kHpY0, fill, kHpY1, 6, kProgress);
        } else if (sel.hpMax > 0) {
            const float f = std::clamp(static_cast<float>(sel.hp) / static_cast<float>(sel.hpMax), 0.0f, 1.0f);
            mCanvas.roundRect(kHpX0, kHpY0, kHpX1, kHpY1, 6, kBarTrack);
            const int fill = kHpX0 + static_cast<int>(std::lround(f * static_cast<float>(kHpX1 - kHpX0)));
            if (fill > kHpX0 + 1)
                mCanvas.roundRect(kHpX0, kHpY0, fill, kHpY1, 6, f > 0.5f ? kHpGood : f > 0.25f ? kHpMid : kHpLow);
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%d / %d", std::max(sel.hp, 0), sel.hpMax);
            if (sel.detail.empty()) mCanvas.textShadow(buf, kHpX0, kHpY1 + 6, kLabelPx, kTextDim);
        }
        if (!sel.detail.empty()) {
            float dpx = kLabelPx;
            const std::string d = fitText(sel.detail, dpx, 16.0f, kHpX1 - kHpX0, false);
            mCanvas.textShadow(d.c_str(), kHpX0, kHpY1 + 6, dpx, kTextDim);
        }
    }
    // Befehlsleiste: 8 Plätze, belegte als Steinplatte mit Symbol + Kurztext.
    const int n = std::min(static_cast<int>(m.buttons.size()), kBtnCount);
    for (int i = 0; i < kBtnCount; ++i) {
        const int x0 = buttonX0(i), x1 = buttonX1(i);
        if (i >= n) {
            mCanvas.roundRect(x0, kBotY0, x1, kBotY1, 14, kSlotEmpty);
            continue;
        }
        const HudButton& b = m.buttons[static_cast<size_t>(i)];
        const bool pressed = i == mPressed, hover = i == mHover && b.enabled;
        if (pressed) mCanvas.slab(x0, kBotY0, x1, kBotY1, 14, kLeafTop, kLeafBottom);
        else if (hover) mCanvas.slab(x0, kBotY0, x1, kBotY1, 14, kSlabHoverTop, kSlabHoverBottom);
        else mCanvas.slab(x0, kBotY0, x1, kBotY1, 14, kSlabTop, kSlabBottom);
        const uint32_t tone = b.enabled ? kText : kTextDim;
        drawIcon(b.icon, (x0 + x1) / 2, kBotY0 + kBtnIconDy, 1.1f, b.enabled ? (hover && !pressed ? kLeaf : tone) : kTextDim);
        float px = kLabelPx;
        const std::string label = fitText(b.label, px, 16.0f, kBtnW - 10, false);
        mCanvas.textShadow(label.c_str(), (x0 + x1 - mCanvas.textWidth(label.c_str(), px)) / 2, kBotY0 + kBtnTextDy, px,
                           tone);
    }
}

}  // namespace agesxr
