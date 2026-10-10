// xr.ages — Spieloberfläche im Spielbild, Implementierung. Siehe xr_game_ui.h.
#include "xr_game_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace agesxr {

using namespace palette;

namespace {

// Maße bei scale() = 1 (Fenster 1600 × 900).
constexpr float kCtxWidth = 300.0f, kCtxTitle = 40.0f, kCtxRow = 46.0f, kCtxPad = 10.0f, kCtxRadius = 14.0f;
constexpr float kMenuWidth = 640.0f, kMenuPad = 26.0f, kMenuTitle = 68.0f, kMenuInfo = 34.0f, kMenuRow = 56.0f;
constexpr float kMenuRowGap = 6.0f, kArrowW = 40.0f, kValueW = 190.0f, kConfirmW = 130.0f;
constexpr float kBoardWidth = 700.0f, kBoardPad = 30.0f, kBoardHead = 52.0f, kBoardLine = 32.0f;
constexpr float kTextPx = 24.0f, kTitlePx = 30.0f, kSmallPx = 20.0f;

constexpr uint32_t kDim = rgba(10, 10, 18, 110);  // Abdunkeln hinter Spielmenü und Tafel

const char* rowLabel(int row) {
    switch (row) {
    case GameUi::kRowResume: return "Weiter";
    case GameUi::kRowSave: return "Speichern …";
    case GameUi::kRowLoad: return "Laden …";
    case GameUi::kRowPause: return "Pause";
    case GameUi::kRowSpeed: return "Spieltempo";
    case GameUi::kRowBiome: return "Landschaft";
    case GameUi::kRowSize: return "Kartengröße";
    case GameUi::kRowOpponent: return "Gegner";
    case GameUi::kRowSeed: return "Kartennummer";
    case GameUi::kRowNewMap: return "Neue Karte starten";
    case GameUi::kRowQuit: return "Beenden";
    default: return "";
    }
}

bool isValueRow(int row) {
    return row == GameUi::kRowSpeed || row == GameUi::kRowBiome || row == GameUi::kRowSize ||
           row == GameUi::kRowOpponent || row == GameUi::kRowSeed;
}

bool isButtonRow(int row) {
    return row == GameUi::kRowResume || row == GameUi::kRowSave || row == GameUi::kRowLoad ||
           row == GameUi::kRowNewMap || row == GameUi::kRowQuit;
}

int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

}  // namespace

bool GameUi::init() {
    mOk = mFont.load(false);
    if (!mFontBold.load(true)) mFontBold.load(false);
    if (mCanvas) mCanvas->setFonts(&mFont, &mFontBold);
    mDirty = true;
    return mOk;
}

void GameUi::resize(int width, int height) {
    width = std::max(width, 16);
    height = std::max(height, 16);
    if (mCanvas && mCanvas->width() == width && mCanvas->height() == height) return;
    mCanvas = std::make_unique<Canvas>(width, height);
    mCanvas->setFonts(&mFont, &mFontBold);
    mDirty = true;
}

float GameUi::scale() const {
    if (!mCanvas) return 1.0f;
    const float s = std::min(static_cast<float>(mCanvas->width()) / 1600.0f, static_cast<float>(mCanvas->height()) / 900.0f);
    return std::clamp(s, 0.5f, 1.6f);
}

void GameUi::openContext(int x, int y, const std::string& title, std::vector<GameUiItem> items) {
    mContext.open = !items.empty();
    mContext.x = x;
    mContext.y = y;
    mContext.title = title;
    mContext.items = std::move(items);
}

// ---- Layout -------------------------------------------------------------------------------------

GameUi::Rect GameUi::contextRect() const {
    if (!mContext.open || !mCanvas) return {};
    const int w = px(kCtxWidth);
    const int h = px(kCtxPad) * 2 + (mContext.title.empty() ? 0 : px(kCtxTitle)) +
                  px(kCtxRow) * static_cast<int>(mContext.items.size());
    // am Zeiger, aber ganz im Bild
    int x0 = clampi(mContext.x + px(6.0f), 0, std::max(0, mCanvas->width() - w));
    int y0 = clampi(mContext.y - px(kCtxPad) - (mContext.title.empty() ? 0 : px(kCtxTitle)), 0,
                    std::max(0, mCanvas->height() - h));
    return {x0, y0, x0 + w, y0 + h};
}

GameUi::Rect GameUi::contextItemRect(int index) const {
    const Rect r = contextRect();
    if (r.empty() || index < 0 || index >= static_cast<int>(mContext.items.size())) return {};
    const int y0 = r.y0 + px(kCtxPad) + (mContext.title.empty() ? 0 : px(kCtxTitle)) + index * px(kCtxRow);
    return {r.x0 + px(kCtxPad), y0, r.x1 - px(kCtxPad), y0 + px(kCtxRow)};
}

GameUi::Rect GameUi::menuRect() const {
    if (!mMenu.open || !mCanvas) return {};
    const int w = px(kMenuWidth);
    const int h = px(kMenuPad) * 2 + px(kMenuTitle) + px(kMenuInfo) + kRowCount * px(kMenuRow + kMenuRowGap);
    const int x0 = (mCanvas->width() - w) / 2, y0 = std::max(0, (mCanvas->height() - h) / 2);
    return {x0, y0, x0 + w, y0 + h};
}

int GameUi::rowY0(int row) const {
    const Rect r = menuRect();
    return r.y0 + px(kMenuPad) + px(kMenuTitle) + px(kMenuInfo) + row * px(kMenuRow + kMenuRowGap);
}

GameUi::Rect GameUi::menuRowRect(int row) const {
    const Rect r = menuRect();
    if (r.empty() || row < 0 || row >= kRowCount) return {};
    const int y0 = rowY0(row);
    return {r.x0 + px(kMenuPad), y0, r.x1 - px(kMenuPad), y0 + px(kMenuRow)};
}

GameUi::Rect GameUi::menuArrowRect(int row, bool right) const {
    const Rect r = menuRowRect(row);
    if (r.empty() || !isValueRow(row)) return {};
    const int x1 = right ? r.x1 - px(8.0f) : r.x1 - px(8.0f) - px(kArrowW) - px(kValueW);
    return {x1 - px(kArrowW), r.y0 + px(6.0f), x1, r.y1 - px(6.0f)};
}

GameUi::Rect GameUi::menuConfirmRect(bool yes) const {
    const Rect r = menuRowRect(kRowQuit);
    if (r.empty() || !mMenu.confirmQuit) return {};
    const int x1 = yes ? r.x1 - px(8.0f) - px(kConfirmW) - px(10.0f) : r.x1 - px(8.0f);
    return {x1 - px(kConfirmW), r.y0 + px(6.0f), x1, r.y1 - px(6.0f)};
}

GameUi::Rect GameUi::boardRect() const {
    if (!mBoard.open || !mCanvas) return {};
    const int w = px(kBoardWidth);
    const int h = px(kBoardPad) * 2 + px(kBoardHead) + px(kBoardLine) * static_cast<int>(mBoard.lines.size()) + px(10.0f);
    const int x0 = (mCanvas->width() - w) / 2, y0 = std::max(0, (mCanvas->height() - h) / 2);
    return {x0, y0, x0 + w, y0 + h};
}

GameUi::Rect GameUi::unionRect(const Rect& a, const Rect& b) const {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}

GameUi::Rect GameUi::drawnRect() const {
    Rect r;
    if (mMenu.open || mBoard.open) return {0, 0, width(), height()};  // Abdunkeln über das ganze Bild
    r = unionRect(r, contextRect());
    return r;
}

// ---- Eingabe ------------------------------------------------------------------------------------

bool GameUi::slotEnabled(int index) const {
    if (index < 0 || index >= static_cast<int>(mMenu.slot_list.size())) return false;
    const GameUiSlot& s = mMenu.slot_list[static_cast<size_t>(index)];
    return mMenu.view == GameMenuModel::kViewSave ? s.slot != 0 : s.exists;
}

GameUi::HoverState GameUi::hitTest(int x, int y) const {
    if (mMenu.open && mMenu.view != GameMenuModel::kViewMain) {
        for (int i = 0; i < static_cast<int>(mMenu.slot_list.size()); ++i)
            if (slotEnabled(i) && menuRowRect(i).contains(x, y)) return {Hover::kSlotRow, i};
        if (menuRowRect(slotBackRow()).contains(x, y)) return {Hover::kSlotBack, slotBackRow()};
        return {};
    }
    if (mMenu.open) {
        if (mMenu.confirmQuit) {
            if (menuConfirmRect(true).contains(x, y)) return {Hover::kConfirmYes, kRowQuit};
            if (menuConfirmRect(false).contains(x, y)) return {Hover::kConfirmNo, kRowQuit};
        }
        for (int row = 0; row < kRowCount; ++row) {
            if (isValueRow(row)) {
                if (menuArrowRect(row, false).contains(x, y)) return {Hover::kMenuArrowL, row};
                if (menuArrowRect(row, true).contains(x, y)) return {Hover::kMenuArrowR, row};
            }
            if (row == kRowQuit && mMenu.confirmQuit) continue;
            if ((isButtonRow(row) || row == kRowPause) && menuRowRect(row).contains(x, y)) return {Hover::kMenuRow, row};
        }
        return {};
    }
    if (mContext.open) {
        for (int i = 0; i < static_cast<int>(mContext.items.size()); ++i)
            if (mContext.items[static_cast<size_t>(i)].enabled && contextItemRect(i).contains(x, y))
                return {Hover::kContextItem, i};
    }
    return {};
}

void GameUi::onMove(int x, int y) { mHover = hitTest(x, y); }

GameUi::Result GameUi::onClick(int x, int y) {
    mHover = hitTest(x, y);
    Result r;
    if (mMenu.open) {
        const HoverState h = mHover;
        switch (h.kind) {
        case Hover::kSlotRow: {
            const GameUiSlot& s = mMenu.slot_list[static_cast<size_t>(h.index)];
            const bool save = mMenu.view == GameMenuModel::kViewSave;
            // belegter Slot (Speichern) bzw. jeder Slot (Laden): erst Rückfrage, zweiter Klick führt aus
            if ((!save || s.exists) && mMenu.confirmSlot != s.slot) {
                mMenu.confirmSlot = s.slot;
                return r;
            }
            r.action = save ? Action::kSave : Action::kLoad;
            r.id = s.slot;
            mMenu.view = GameMenuModel::kViewMain;
            mMenu.confirmSlot = -1;
            return r;
        }
        case Hover::kSlotBack:
            mMenu.view = GameMenuModel::kViewMain;
            mMenu.confirmSlot = -1;
            return r;
        case Hover::kConfirmYes:
            r.action = Action::kQuit;
            return r;
        case Hover::kConfirmNo:
            mMenu.confirmQuit = false;
            return r;
        case Hover::kMenuArrowL:
        case Hover::kMenuArrowR: {
            const int d = h.kind == Hover::kMenuArrowR ? 1 : -1;
            switch (h.index) {
            case kRowSpeed:
                mMenu.speed = clampi(mMenu.speed + d, 0, kGameSpeedCount - 1);
                r.action = Action::kSpeedChanged;
                break;
            case kRowBiome: mMenu.biome = (mMenu.biome + d + kGameUiBiomeCount) % kGameUiBiomeCount; break;
            case kRowSize: mMenu.size = clampi(mMenu.size + d, 0, kGameUiSizeCount - 1); break;
            case kRowOpponent: mMenu.opponent = clampi(mMenu.opponent + d, 0, kGameUiOpponentCount - 1); break;
            case kRowSeed: mMenu.seed = clampi(mMenu.seed + d, kGameUiSeedMin, kGameUiSeedMax); break;
            default: break;
            }
            return r;
        }
        case Hover::kMenuRow:
            switch (h.index) {
            case kRowResume:
                mMenu.open = false;
                mMenu.confirmQuit = false;
                r.action = Action::kResume;
                break;
            case kRowSave:
            case kRowLoad:
                mMenu.view = h.index == kRowSave ? GameMenuModel::kViewSave : GameMenuModel::kViewLoad;
                mMenu.confirmSlot = -1;
                mMenu.confirmQuit = false;
                r.action = Action::kShowSlots;
                break;
            case kRowPause:
                mMenu.paused = !mMenu.paused;
                r.action = Action::kTogglePause;
                break;
            case kRowNewMap:
                mMenu.open = false;
                mMenu.confirmQuit = false;
                r.action = Action::kNewMap;
                break;
            case kRowQuit: mMenu.confirmQuit = true; break;
            default: break;
            }
            return r;
        default:
            // Klick neben die Tafel: Rückfragen zurücknehmen, Menü bleibt offen
            if (!menuRect().contains(x, y)) {
                mMenu.confirmQuit = false;
                mMenu.confirmSlot = -1;
            }
            return r;
        }
    }
    if (mContext.open) {
        if (mHover.kind == Hover::kContextItem) {
            r.action = Action::kContextItem;
            r.id = mContext.items[static_cast<size_t>(mHover.index)].id;
        } else {
            r.action = Action::kContextClosed;
        }
        closeContext();
        mHover = {};
        return r;
    }
    return r;
}

// ---- Zeichnen -----------------------------------------------------------------------------------

const uint32_t* GameUi::pixels() {
    if (!mCanvas) return nullptr;
    const bool changed = mDirty || mContext != mShownContext || mMenu != mShownMenu || mBoard != mShownBoard ||
                         mHover != mShownHover;
    if (!changed) {
        mDirtyY0 = mDirtyY1 = 0;
        return mCanvas->data();
    }
    const Rect now = drawnRect();
    const Rect band = mDirty ? Rect{0, 0, width(), height()} : unionRect(mShownRect, now);
    mCanvas->resetClip();
    if (!band.empty()) {
        mCanvas->setClip(band.y0, band.y1);
        mCanvas->clearBand(band.y0, band.y1);
        drawAll();
        mCanvas->resetClip();
    }
    mDirtyY0 = band.empty() ? 0 : band.y0;
    mDirtyY1 = band.empty() ? 0 : band.y1;
    mShownContext = mContext;
    mShownMenu = mMenu;
    mShownBoard = mBoard;
    mShownHover = mHover;
    mShownRect = now;
    mDirty = false;
    ++mVersion;
    return mCanvas->data();
}

void GameUi::drawAll() {
    if (mMenu.open || mBoard.open) mCanvas->fillRect(0, 0, width(), height(), kDim);
    if (mBoard.open) drawBoard();
    if (mMenu.open) drawMenu();
    if (mContext.open) drawContext();
}

void GameUi::drawContext() {
    const Rect r = contextRect();
    if (r.empty()) return;
    const int rad = px(kCtxRadius);
    mCanvas->roundRect(r.x0, r.y0, r.x1, r.y1, rad, kStoneRim);
    mCanvas->roundRectV(r.x0 + 3, r.y0 + 3, r.x1 - 3, r.y1 - 3, rad - 3, kStoneTop, kStoneBottom);
    const float textPx = kTextPx * scale();
    if (!mContext.title.empty()) {
        const int ty = r.y0 + px(kCtxPad);
        mCanvas->woodSign(r.x0 + px(kCtxPad), ty, r.x1 - px(kCtxPad), ty + px(kCtxTitle) - px(6.0f), px(10.0f), false);
        const int tw = mCanvas->textWidth(mContext.title.c_str(), textPx * 0.9f, true);
        mCanvas->textShadow(mContext.title.c_str(), (r.x0 + r.x1 - tw) / 2, ty + px(5.0f), textPx * 0.9f, kText, true);
    }
    for (int i = 0; i < static_cast<int>(mContext.items.size()); ++i) {
        const GameUiItem& it = mContext.items[static_cast<size_t>(i)];
        const Rect ir = contextItemRect(i);
        const bool hot = mHover.kind == Hover::kContextItem && mHover.index == i;
        const int gap = px(3.0f);
        if (it.enabled) {
            mCanvas->slab(ir.x0, ir.y0 + gap, ir.x1, ir.y1 - gap, px(10.0f), hot ? kSlabHoverTop : kSlabTop,
                          hot ? kSlabHoverBottom : kSlabBottom);
        } else {
            mCanvas->roundRect(ir.x0, ir.y0 + gap, ir.x1, ir.y1 - gap, px(10.0f), kStoneDeep);
        }
        mCanvas->textShadow(it.label.c_str(), ir.x0 + px(16.0f), ir.y0 + (ir.y1 - ir.y0 - static_cast<int>(textPx)) / 2,
                            textPx, it.enabled ? (hot ? kLeaf : kText) : kTextDim, false);
    }
}

void GameUi::drawArrow(const Rect& r, bool right, bool hot) {
    if (r.empty()) return;
    mCanvas->slab(r.x0, r.y0, r.x1, r.y1, px(8.0f), hot ? kLeafTop : kSlabTop, hot ? kLeafBottom : kSlabBottom);
    const float cx = (r.x0 + r.x1) * 0.5f, cy = (r.y0 + r.y1) * 0.5f, h = (r.y1 - r.y0) * 0.22f;
    const float v[6] = {right ? cx + h : cx - h, cy, right ? cx - h * 0.7f : cx + h * 0.7f, cy - h,
                        right ? cx - h * 0.7f : cx + h * 0.7f, cy + h};
    mCanvas->fillSdf(r.x0, r.y0, r.x1, r.y1, kText, [&](float x, float y) { return sdf::triangle(x, y, v); });
}

void GameUi::drawSlots() {
    const Rect r = menuRect();
    const float s = scale();
    const int rad = px(28.0f);
    const bool save = mMenu.view == GameMenuModel::kViewSave;
    mCanvas->roundRect(r.x0, r.y0, r.x1, r.y1, rad, kStoneRim);
    mCanvas->roundRectV(r.x0 + 4, r.y0 + 4, r.x1 - 4, r.y1 - 4, rad - 4, kStoneTop, kStoneBottom);
    const int ty = r.y0 + px(kMenuPad);
    mCanvas->woodSign(r.x0 + px(kMenuPad), ty, r.x1 - px(kMenuPad), ty + px(kMenuTitle) - px(8.0f), px(14.0f), true);
    const char* title = save ? "Spiel speichern" : "Spiel laden";
    const float titlePx = kTitlePx * s;
    mCanvas->textShadow(title, (r.x0 + r.x1 - mCanvas->textWidth(title, titlePx, true)) / 2, ty + px(12.0f), titlePx, kText, true);
    const char* info = save ? "Slot wählen – belegter Slot fragt vor dem Überschreiben" : "Slot wählen – das laufende Spiel wird verworfen";
    const float ipx = kSmallPx * s;
    mCanvas->textShadow(info, (r.x0 + r.x1 - mCanvas->textWidth(info, ipx, false)) / 2, ty + px(kMenuTitle) + px(4.0f), ipx,
                        kTextDim, false);
    const float textPx = kTextPx * s * 0.86f;
    for (int i = 0; i <= slotBackRow(); ++i) {
        const Rect rr = menuRowRect(i);
        if (rr.empty()) continue;
        const int textY = rr.y0 + (rr.y1 - rr.y0 - static_cast<int>(textPx)) / 2;
        if (i == slotBackRow()) {
            const bool hot = mHover.kind == Hover::kSlotBack;
            mCanvas->slab(rr.x0, rr.y0, rr.x1, rr.y1, px(12.0f), hot ? kLeafTop : kSlabTop, hot ? kLeafBottom : kSlabBottom);
            const char* back = "‹ Zurück";
            mCanvas->textShadow(back, (rr.x0 + rr.x1 - mCanvas->textWidth(back, textPx, true)) / 2, textY, textPx, kText, true);
            continue;
        }
        const GameUiSlot& slot = mMenu.slot_list[static_cast<size_t>(i)];
        const bool enabled = slotEnabled(i);
        const bool hot = mHover.kind == Hover::kSlotRow && mHover.index == i;
        const bool confirm = mMenu.confirmSlot == slot.slot;
        if (confirm) mCanvas->slab(rr.x0, rr.y0, rr.x1, rr.y1, px(12.0f), kWarnTop, kWarnBottom);
        else if (!enabled) mCanvas->roundRect(rr.x0, rr.y0, rr.x1, rr.y1, px(12.0f), kStoneDeep);
        else mCanvas->slab(rr.x0, rr.y0, rr.x1, rr.y1, px(12.0f), hot ? kSlabHoverTop : kSlabTop, hot ? kSlabHoverBottom : kSlabBottom);
        std::string text = confirm ? (save ? "Überschreiben? Nochmal klicken" : "Laufendes Spiel verwerfen? Nochmal klicken")
                                   : slot.label;
        float tpx = textPx;
        const int maxW = rr.x1 - rr.x0 - px(32.0f);
        while (tpx > 12.0f && mCanvas->textWidth(text.c_str(), tpx, false) > maxW) tpx -= 1.0f;
        mCanvas->textShadow(text.c_str(), rr.x0 + px(16.0f), rr.y0 + (rr.y1 - rr.y0 - static_cast<int>(tpx)) / 2, tpx,
                            !enabled ? kTextDim : hot && !confirm ? kLeaf : kText, confirm);
    }
}

void GameUi::drawMenu() {
    const Rect r = menuRect();
    if (r.empty()) return;
    if (mMenu.view != GameMenuModel::kViewMain) {
        drawSlots();
        return;
    }
    const float s = scale();
    const int rad = px(28.0f);
    mCanvas->roundRect(r.x0, r.y0, r.x1, r.y1, rad, kStoneRim);
    mCanvas->roundRectV(r.x0 + 4, r.y0 + 4, r.x1 - 4, r.y1 - 4, rad - 4, kStoneTop, kStoneBottom);
    // Titel auf Holzschild
    const int ty = r.y0 + px(kMenuPad);
    mCanvas->woodSign(r.x0 + px(kMenuPad), ty, r.x1 - px(kMenuPad), ty + px(kMenuTitle) - px(8.0f), px(14.0f), true);
    const char* title = "Spielmenü";
    const float titlePx = kTitlePx * s;
    const int tw = mCanvas->textWidth(title, titlePx, true);
    mCanvas->textShadow(title, (r.x0 + r.x1 - tw) / 2, ty + px(12.0f), titlePx, kText, true);
    // Hinweiszeile: laufende Karte
    if (!mMenu.mapInfo.empty()) {
        const float ipx = kSmallPx * s;
        const int iw = mCanvas->textWidth(mMenu.mapInfo.c_str(), ipx, false);
        mCanvas->textShadow(mMenu.mapInfo.c_str(), (r.x0 + r.x1 - iw) / 2, ty + px(kMenuTitle) + px(4.0f), ipx, kTextDim, false);
    }
    const float textPx = kTextPx * s;
    for (int row = 0; row < kRowCount; ++row) {
        const Rect rr = menuRowRect(row);
        const bool hot = mHover.index == row && (mHover.kind == Hover::kMenuRow);
        const int textY = rr.y0 + (rr.y1 - rr.y0 - static_cast<int>(textPx)) / 2;
        if (isButtonRow(row)) {
            if (row == kRowQuit && mMenu.confirmQuit) {
                mCanvas->roundRect(rr.x0, rr.y0, rr.x1, rr.y1, px(12.0f), kStoneDeep);
                mCanvas->textShadow("Wirklich beenden?", rr.x0 + px(20.0f), textY, textPx, kWarn, true);
                const Rect yes = menuConfirmRect(true), no = menuConfirmRect(false);
                const bool hotYes = mHover.kind == Hover::kConfirmYes, hotNo = mHover.kind == Hover::kConfirmNo;
                mCanvas->slab(yes.x0, yes.y0, yes.x1, yes.y1, px(10.0f), hotYes ? kWarnTop : kSlabTop, hotYes ? kWarnBottom : kSlabBottom);
                mCanvas->slab(no.x0, no.y0, no.x1, no.y1, px(10.0f), hotNo ? kLeafTop : kSlabTop, hotNo ? kLeafBottom : kSlabBottom);
                const int yw = mCanvas->textWidth("Ja", textPx, true), nw = mCanvas->textWidth("Nein", textPx, true);
                mCanvas->textShadow("Ja", (yes.x0 + yes.x1 - yw) / 2, textY, textPx, kText, true);
                mCanvas->textShadow("Nein", (no.x0 + no.x1 - nw) / 2, textY, textPx, kText, true);
                continue;
            }
            const bool quit = row == kRowQuit;
            const uint32_t top = quit ? (hot ? kWarnTop : kSlabTop) : (hot ? kLeafTop : kSlabTop);
            const uint32_t bottom = quit ? (hot ? kWarnBottom : kSlabBottom) : (hot ? kLeafBottom : kSlabBottom);
            mCanvas->slab(rr.x0, rr.y0, rr.x1, rr.y1, px(12.0f), top, bottom);
            const int w = mCanvas->textWidth(rowLabel(row), textPx, true);
            mCanvas->textShadow(rowLabel(row), (rr.x0 + rr.x1 - w) / 2, textY, textPx, quit && !hot ? kWarn : kText, true);
            continue;
        }
        // Wertzeile: Steinplatte, Beschriftung links
        mCanvas->slab(rr.x0, rr.y0, rr.x1, rr.y1, px(12.0f), hot ? kSlabHoverTop : kSlabTop, hot ? kSlabHoverBottom : kSlabBottom);
        mCanvas->textShadow(rowLabel(row), rr.x0 + px(20.0f), textY, textPx, kText, false);
        if (row == kRowPause) {
            // Kippschalter rechts
            const int kw = px(74.0f), kh = px(32.0f);
            const int kx1 = rr.x1 - px(14.0f), kx0 = kx1 - kw, ky0 = (rr.y0 + rr.y1 - kh) / 2;
            mCanvas->roundRect(kx0, ky0, kx1, ky0 + kh, kh / 2, mMenu.paused ? kLeafBottom : kLampOff);
            const int kr = kh / 2 - px(3.0f);
            const int kcx = mMenu.paused ? kx1 - kh / 2 : kx0 + kh / 2;
            mCanvas->circle(kcx, ky0 + kh / 2, kr, kKnob);
            const char* state = mMenu.paused ? "An" : "Aus";
            const int sw = mCanvas->textWidth(state, textPx, false);
            mCanvas->textShadow(state, kx0 - px(14.0f) - sw, textY, textPx, kTextDim, false);
            continue;
        }
        // Wert mit Pfeilen
        char value[64];
        switch (row) {
        case kRowSpeed: std::snprintf(value, sizeof(value), "%s", kGameSpeedLabels[mMenu.speed]); break;
        case kRowBiome: std::snprintf(value, sizeof(value), "%s", kGameUiBiomeLabels[mMenu.biome]); break;
        case kRowSize: std::snprintf(value, sizeof(value), "%d × %d", kGameUiSizeTiles[mMenu.size], kGameUiSizeTiles[mMenu.size]); break;
        case kRowOpponent: std::snprintf(value, sizeof(value), "%s", kGameUiOpponentLabels[mMenu.opponent]); break;
        case kRowSeed: std::snprintf(value, sizeof(value), "#%d", mMenu.seed); break;
        default: value[0] = 0; break;
        }
        const Rect al = menuArrowRect(row, false), ar = menuArrowRect(row, true);
        drawArrow(al, false, mHover.kind == Hover::kMenuArrowL && mHover.index == row);
        drawArrow(ar, true, mHover.kind == Hover::kMenuArrowR && mHover.index == row);
        mCanvas->roundRect(al.x1 + px(4.0f), al.y0, ar.x0 - px(4.0f), al.y1, px(8.0f), kStoneDeep);
        const int vw = mCanvas->textWidth(value, textPx, false);
        mCanvas->textShadow(value, (al.x1 + ar.x0 - vw) / 2, textY, textPx, kText, false);
    }
}

void GameUi::drawBoard() {
    const Rect r = boardRect();
    if (r.empty()) return;
    const float s = scale();
    const int rad = px(28.0f);
    mCanvas->roundRect(r.x0, r.y0, r.x1, r.y1, rad, kStoneRim);
    mCanvas->roundRectV(r.x0 + 4, r.y0 + 4, r.x1 - 4, r.y1 - 4, rad - 4, kStoneTop, kStoneBottom);
    const int hy = r.y0 + px(kBoardPad);
    mCanvas->roundRectV(r.x0 + px(kBoardPad), hy, r.x1 - px(kBoardPad), hy + px(kBoardHead) - px(6.0f), px(12.0f),
                        mBoard.good ? kLeafTop : kWarnTop, mBoard.good ? kLeafBottom : kWarnBottom);
    const float hpx = (kTitlePx + 6.0f) * s;
    const int hw = mCanvas->textWidth(mBoard.headline.c_str(), hpx, true);
    mCanvas->textShadow(mBoard.headline.c_str(), (r.x0 + r.x1 - hw) / 2, hy + px(6.0f), hpx, kText, true);
    const float lpx = kTextPx * s;
    int y = hy + px(kBoardHead) + px(6.0f);
    for (const std::string& line : mBoard.lines) {
        const int lw = mCanvas->textWidth(line.c_str(), lpx, false);
        mCanvas->textShadow(line.c_str(), (r.x0 + r.x1 - lw) / 2, y, lpx, kText, false);
        y += px(kBoardLine);
    }
}

}  // namespace agesxr
