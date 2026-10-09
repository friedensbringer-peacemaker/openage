// xr.ages — Spieloberfläche im AoE-II-Stil, Implementierung. Siehe xr_aoe_ui.h und docs/UI-SPEC-AOE.md.
#include "xr_aoe_ui.h"

#include "xr_hud.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace agesxr {

using namespace aoelayout;
using namespace aoepal;

namespace {

constexpr int kAlignLeft = 0, kAlignCenter = 1, kAlignRight = 2;
constexpr int kIdMenuBtn = 5001, kIdMinimap = 5002, kIdCtxBase = 6000, kIdDlgBase = 7000, kIdBoardBase = 8000,
              kIdMultiBase = 9000, kIdGarrisonBase = 9500, kIdDlgLoad = 7999, kIdOrderBase = 9700;

int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

const char* ageNumeral(int index) {
    switch (index) {
    case 2: return "II";
    case 3: return "III";
    case 4: return "IV";
    default: return "I";
    }
}

}  // namespace

AoeUi::AoeUi() : mSkin(std::make_unique<VectorSkin>()) {}
AoeUi::~AoeUi() = default;

bool AoeUi::init() {
    mOk = mFont.load(false);
    if (!mFontBold.load(true)) mFontBold.load(false);
    if (mCanvas) mCanvas->setFonts(&mFont, &mFontBold);
    mDirty = true;
    return mOk;
}

void AoeUi::setSkin(std::unique_ptr<SkinProvider> skin) {
    if (skin) mSkin = std::move(skin);
    mDirty = true;
}

void AoeUi::resize(int width, int height) {
    width = std::max(width, 16);
    height = std::max(height, 16);
    if (mCanvas && mCanvas->width() == width && mCanvas->height() == height) return;
    mCanvas = std::make_unique<Canvas>(width, height);
    mCanvas->setFonts(&mFont, &mFontBold);
    mDirty = true;
}

float AoeUi::scale() const {
    if (!mCanvas) return 1.0f;
    const float s = std::min(static_cast<float>(mCanvas->height()) / static_cast<float>(kBaseH),
                             static_cast<float>(mCanvas->width()) / static_cast<float>(kBaseW));
    return std::clamp(s, 0.25f, 2.0f);
}

int AoeUi::px(float v) const { return static_cast<int>(std::lround(v * scale())); }

// ---- Modelle ------------------------------------------------------------------------------------

void AoeUi::openContext(int x, int y, const std::string& title, std::vector<GameUiItem> items) {
    if (static_cast<int>(items.size()) > kCtxMaxItems) items.resize(static_cast<size_t>(kCtxMaxItems));
    mContext.open = !items.empty();
    mContext.x = x;
    mContext.y = y;
    mContext.title = title;
    mContext.items = std::move(items);
    if (mContext.open && mFocusMode) setFocus({Hit::kCtxItem, 0});
}

void AoeUi::closeContext() {
    mContext = ContextMenuModel{};
    if (mFocus.kind == Hit::kCtxItem) {
        mFocus = {};
        mFocusMode = false;
    }
}

void AoeUi::openMenu(bool open, double now) {
    (void)now;
    mMenu.open = open;
    mMenu.page = AoeMenuModel::kMain;
    mMenu.confirmSurrender = false;
    mMenu.armedAt = 0.0;
    mFocus = {};
    mFocusMode = false;
    mPressed = {};
}

// ---- Layout -------------------------------------------------------------------------------------

AoeUi::Rect AoeUi::topBarRect() const { return r4(0, 0, width(), px(kTopH)); }

AoeUi::Rect AoeUi::topCellRect(int i) const {
    if (i < 0 || i >= kTopCells) return {};
    return r4(lx(static_cast<float>(topCellX(i))), 0, lx(static_cast<float>(topCellX(i) + kTopCellW)), px(kTopH));
}

AoeUi::Rect AoeUi::ageRect() const { return r4(lx(kAgeX), 0, std::min(lx(kAgeX + kAgeW), rx(kMsgX) - px(8)), px(kTopH)); }
AoeUi::Rect AoeUi::plaketteRect() const { return r4(rx(kMsgX), px(6), rx(kMsgX + kMsgW), px(kTopH - 6)); }
AoeUi::Rect AoeUi::clockRect() const { return r4(rx(kMsgX + kMsgW) + px(8), 0, rx(kClockRight), px(kTopH)); }
AoeUi::Rect AoeUi::menuButtonRect() const {
    return r4(rx(kMenuBtnX), px(4), rx(kMenuBtnX + kMenuBtnW), px(4 + kMenuBtnH));
}
AoeUi::Rect AoeUi::menuButtonHitRect() const { return r4(rx(kMenuBtnX), 0, rx(kMenuBtnX + kMenuBtnW), px(kMenuBtnHitH)); }
AoeUi::Rect AoeUi::barRect() const { return r4(0, height() - px(kBarH), width(), height()); }
AoeUi::Rect AoeUi::commandsRect() const { return r4(0, barRect().y0, lx(kCmdX1), height()); }
AoeUi::Rect AoeUi::selectionRect() const { return r4(lx(kSelX0), barRect().y0, rx(kSelX1), height()); }
AoeUi::Rect AoeUi::minimapFieldRect() const { return r4(rx(kMapX0), barRect().y0, width(), height()); }
AoeUi::Rect AoeUi::minimapRect() const {
    const int cx = rx(kMiniCx), cy = barRect().y0 + px(kMiniCy - kBarY);
    return r4(cx - px(kMiniW) / 2, cy - px(kMiniH) / 2, cx + px(kMiniW) / 2, cy + px(kMiniH) / 2);
}

AoeUi::Rect AoeUi::gridRect(int cell) const {
    if (cell < 0 || cell >= kAoeGridCells) return {};
    const int c = cell % kGridCols, r = cell / kGridCols;
    const int x0 = lx(static_cast<float>(gridX(c))), y0 = barRect().y0 + px(static_cast<float>(gridY(r) - kBarY));
    return r4(x0, y0, x0 + px(kBtn), y0 + px(kBtn));
}

AoeUi::Rect AoeUi::gridHitRect(int cell) const {
    const Rect r = gridRect(cell);
    if (r.empty()) return {};
    // lückenloses Raster: Zelle + halbe Lücke auf jeder Seite (bei 1080p 82 × 82)
    const int c = cell % kGridCols, row = cell / kGridCols;
    const int x0 = lx(static_cast<float>(gridX(c) - kHitPad)), x1 = lx(static_cast<float>(gridX(c + 1) - kHitPad));
    const int y0 = barRect().y0 + px(static_cast<float>(gridY(row) - kHitPad - kBarY));
    const int y1 = barRect().y0 + px(static_cast<float>(gridY(row + 1) - kHitPad - kBarY));
    return r4(x0, y0, x1, y1);
}

AoeUi::Rect AoeUi::portraitRect() const {
    const int y0 = barRect().y0 + px(kPortraitY - kBarY);
    return r4(lx(kPortraitX), y0, lx(kPortraitX) + px(kPortrait), y0 + px(kPortrait));
}

AoeUi::Rect AoeUi::hpRect() const {
    const int y0 = barRect().y0 + px(kHpY - kBarY);
    return r4(lx(kNameX), y0, lx(kNameX) + px(kHpW), y0 + px(kHpH));
}

AoeUi::Rect AoeUi::barBase(int x0, int y0, int x1, int y1) const {
    const int by = barRect().y0;
    return r4(lx(static_cast<float>(x0)), by + px(static_cast<float>(y0 - kBarY)), lx(static_cast<float>(x1)),
              by + px(static_cast<float>(y1 - kBarY)));
}

AoeUi::Rect AoeUi::slotBase(Hit kind, int i, int pad) const {
    int x = 0, y = 0;
    switch (kind) {
    case Hit::kQueue:
        if (i < 0 || i >= kAoeQueueShown) return {};
        x = queueX(i);
        y = kQueueY0;
        break;
    case Hit::kGarrison:
        if (i < 0 || i >= kGarrisonVisible) return {};
        x = garrisonX(i);
        y = kQueueY0;
        break;
    case Hit::kMulti:
    case Hit::kOrder:
        if (i < 0 || i >= kAoeMultiShown) return {};
        x = multiX(i);
        y = multiY(i);
        break;
    default: return {};
    }
    return barBase(x - pad, y - pad, x + kSlot + pad, y + kSlot + pad);
}

AoeUi::Rect AoeUi::queueRect(int i) const { return slotBase(Hit::kQueue, i, 0); }
AoeUi::Rect AoeUi::garrisonRect(int i) const { return slotBase(Hit::kGarrison, i, 0); }
AoeUi::Rect AoeUi::multiRect(int i) const { return slotBase(Hit::kMulti, i, 0); }
AoeUi::Rect AoeUi::orderRect(int i) const { return slotBase(Hit::kOrder, i, 0); }
AoeUi::Rect AoeUi::slotHitRect(Hit kind, int i) const { return slotBase(kind, i, kSlotHitPad); }

AoeUi::Rect AoeUi::messageRect(int i) const {
    const int n = static_cast<int>(mModel.messages.size());
    if (i < 0 || i >= n || i >= kAoeMessagesMax) return {};
    const AoeMessage& m = mModel.messages[static_cast<size_t>(n - 1 - i)];
    const int y1 = barRect().y0 - px(kBarY - kMsgBottomY) - i * px(kMsgRowH);
    const float tpx = kPxMsg * scale();
    int w = px(kMsgBarW + 16) + (mCanvas ? mCanvas->textWidth(m.text.c_str(), tpx, false) : px(200)) + px(16);
    w = std::min(w, px(kMsgW0));
    return r4(lx(kMsgX0), y1 - px(kMsgRowH - 2), lx(kMsgX0) + w, y1);
}

AoeUi::Rect AoeUi::contextRect() const {
    if (!mContext.open || !mCanvas) return {};
    const int w = px(kCtxW);
    const int h = px(kCtxPad) * 2 + (mContext.title.empty() ? 0 : px(kCtxTitle)) +
                  px(kCtxRow) * static_cast<int>(mContext.items.size());
    const int maxY = std::max(0, barRect().y0 - px(8) - h);  // nie unter der unteren Leiste
    const int x0 = clampi(mContext.x + px(6), 0, std::max(0, width() - w - px(kCtxShadow)));
    const int y0 = clampi(mContext.y - px(kCtxPad) - (mContext.title.empty() ? 0 : px(kCtxTitle)), px(kTopH), maxY);
    return r4(x0, y0, x0 + w, y0 + h);
}

AoeUi::Rect AoeUi::contextItemRect(int index) const {
    const Rect r = contextRect();
    if (r.empty() || index < 0 || index >= static_cast<int>(mContext.items.size())) return {};
    const int y0 = r.y0 + px(kCtxPad) + (mContext.title.empty() ? 0 : px(kCtxTitle)) + index * px(kCtxRow);
    return r4(r.x0 + px(kCtxPad), y0, r.x1 - px(kCtxPad), y0 + px(kCtxRow));
}

AoeUi::Rect AoeUi::dialogRect() const {
    if (!mMenu.open || !mCanvas) return {};
    const int w = px(mMenu.page == AoeMenuModel::kNewMap || slotPage(mMenu.page) ? kDlgWWide : kDlgW), h = px(kDlgH);
    const int x0 = (width() - w) / 2, y0 = std::max(px(kTopH + 8), (height() - h) / 2);
    return r4(x0, y0, x0 + w, y0 + h);
}

AoeUi::Rect AoeUi::dialogTitleRect() const {
    const Rect d = dialogRect();
    if (d.empty()) return {};
    const int w = px(kDlgTitleW), h = px(kDlgTitleH);
    return r4(d.cx() - w / 2, d.y0 - h / 3, d.cx() + w / 2, d.y0 - h / 3 + h);
}

int AoeUi::dialogRowCount() const {
    switch (mMenu.page) {
    case AoeMenuModel::kSettings: return kSetCount;
    case AoeMenuModel::kNewMap: return kNewCount;
    case AoeMenuModel::kSave:
    case AoeMenuModel::kLoad:
        return std::min(static_cast<int>(mMenu.slots.size()), kAoeSlotsMax) + 1;  // + „‹ Zurück“
    default: return kMainCount;
    }
}

AoeUi::Rect AoeUi::dialogRowRect(int row) const {
    const Rect d = dialogRect();
    if (d.empty() || row < 0 || row >= dialogRowCount()) return {};
    const bool slots = slotPage(mMenu.page);
    const int w = px(kDlgBtnW) + (mMenu.page == AoeMenuModel::kNewMap || slots ? px(kDlgWWide - kDlgW) : 0);
    // slot pages: 8 rows in the same dialog (pitch 64, rows 56 – still ≥ 48 px)
    const int pitch = slots ? px(64) : px(kDlgPitch);
    const int y0 = d.y0 + px(kDlgRowsY + 8) + row * pitch;
    Rect r = r4(d.cx() - w / 2, y0, d.cx() + w / 2, y0 + (slots ? px(56) : px(kDlgBtnH)));
    if (mMenu.page == AoeMenuModel::kMain && row == kMainSave && mMenu.loadAvailable) r.x1 = r.cx() - px(4);
    return r;
}

AoeUi::Rect AoeUi::dialogLoadRect() const {
    if (!mMenu.open || mMenu.page != AoeMenuModel::kMain || !mMenu.loadAvailable) return {};
    const Rect d = dialogRect();
    const int w = px(kDlgBtnW), y0 = d.y0 + px(kDlgRowsY + 8) + kMainSave * px(kDlgPitch);
    return r4(d.cx() + px(4), y0, d.cx() + w / 2, y0 + px(kDlgBtnH));
}

bool AoeUi::dialogRowIsValue(int row) const {
    if (mMenu.page == AoeMenuModel::kSettings) return row != kSetBack;
    if (mMenu.page == AoeMenuModel::kNewMap) return row <= kNewSeed;
    return false;
}

AoeUi::Rect AoeUi::dialogArrowRect(int row, bool right) const {
    const Rect r = dialogRowRect(row);
    if (r.empty() || !dialogRowIsValue(row)) return {};
    const int aw = px(kArrowW), ah = px(kArrowH), valueW = px(190);
    const int x1 = right ? r.x1 - px(8) : r.x1 - px(8) - aw - valueW;
    const int y0 = r.cy() - ah / 2;
    return r4(x1 - aw, y0, x1, y0 + ah);
}

bool AoeUi::dialogRowEnabled(int row) const {
    if (mMenu.page == AoeMenuModel::kMain) {
        if (row == kMainSave) return mMenu.saveAvailable;
        return true;
    }
    if (slotPage(mMenu.page) && row < dialogRowCount() - 1) {
        const AoeSaveSlot& s = mMenu.slots[static_cast<size_t>(row)];
        return mMenu.page == AoeMenuModel::kSave ? s.writable : s.exists;
    }
    return true;
}

std::string AoeUi::dialogRowLabel(int row) const {
    if (slotPage(mMenu.page)) {
        if (row >= dialogRowCount() - 1) return "‹ Zurück";
        if (row == mMenu.confirmSlot)
            return mMenu.page == AoeMenuModel::kSave ? "Überschreiben? Nochmal klicken" : "Spiel verwerfen? Nochmal klicken";
        return mMenu.slots[static_cast<size_t>(row)].label;
    }
    switch (mMenu.page) {
    case AoeMenuModel::kSettings:
        switch (row) {
        case kSetSpeed: return "Spieltempo";
        case kSetLabels: return "Beschriftung";
        case kSetHotkeys: return "Hotkeys zeigen";
        case kSetMessages: return "Meldungen";
        case kSetBack: return "‹ Zurück";
        default: return "";
        }
    case AoeMenuModel::kNewMap:
        switch (row) {
        case kNewBiome: return "Landschaft";
        case kNewSize: return "Kartengröße";
        case kNewOpponent: return "Gegner";
        case kNewSeed: return "Kartennummer";
        case kNewStart: return "Karte starten";
        case kNewBack: return "‹ Zurück";
        default: return "";
        }
    default:
        switch (row) {
        case kMainResume: return "Weiterspielen";
        case kMainSave: return mMenu.saveAvailable ? "Speichern …" : "Speichern (folgt)";
        case kMainSettings: return "Einstellungen ›";
        case kMainNewMap: return "Neue Karte ›";
        case kMainSurrender: return mMenu.confirmSurrender ? "Sicher? Nochmal klicken" : "Partie aufgeben";
        case kMainClose: return "Schließen";
        default: return "";
        }
    }
}

std::string AoeUi::dialogRowValue(int row) const {
    char buf[64];
    if (mMenu.page == AoeMenuModel::kSettings) {
        switch (row) {
        case kSetSpeed: return kGameSpeedLabels[clampi(mMenu.speed, 0, kGameSpeedCount - 1)];
        case kSetLabels: return mMenu.labels ? "An" : "Aus";
        case kSetHotkeys: return mMenu.hotkeys ? "An" : "Aus";
        case kSetMessages: return mMenu.messages ? "An" : "Aus";
        default: return "";
        }
    }
    if (mMenu.page == AoeMenuModel::kNewMap) {
        switch (row) {
        case kNewBiome: return kGameUiBiomeLabels[clampi(mMenu.biome, 0, kGameUiBiomeCount - 1)];
        case kNewSize: {
            const int t = kGameUiSizeTiles[clampi(mMenu.size, 0, kGameUiSizeCount - 1)];
            std::snprintf(buf, sizeof(buf), "%d × %d", t, t);
            return buf;
        }
        case kNewOpponent: return kGameUiOpponentLabels[clampi(mMenu.opponent, 0, kGameUiOpponentCount - 1)];
        case kNewSeed:
            std::snprintf(buf, sizeof(buf), "%d", mMenu.seed);
            return buf;
        default: return "";
        }
    }
    return "";
}

AoeUi::Rect AoeUi::boardRect() const {
    if (!mBoard.open || !mCanvas) return {};
    const int w = px(700);
    const int h = px(30) * 2 + px(60) + px(34) * static_cast<int>(mBoard.lines.size()) + px(kDlgBtnH + 24);
    const int x0 = (width() - w) / 2, y0 = std::max(px(kTopH + 8), (height() - h) / 2);
    return r4(x0, y0, x0 + w, y0 + h);
}

AoeUi::Rect AoeUi::boardButtonRect(int i) const {
    const Rect b = boardRect();
    if (b.empty() || i < 0 || i >= kBoardButtons) return {};
    const int w = px(300), gap = px(20);
    const int x0 = b.cx() - (2 * w + gap) / 2 + i * (w + gap);
    const int y1 = b.y1 - px(24);
    return r4(x0, y1 - px(kDlgBtnH), x0 + w, y1);
}

bool AoeUi::overBars(int x, int y) const {
    if (!mCanvas) return false;
    return topBarRect().contains(x, y) || barRect().contains(x, y) || menuButtonHitRect().contains(x, y);
}

// ---- Treffer ------------------------------------------------------------------------------------

AoeUi::HitState AoeUi::hitTest(int x, int y) const {
    if (!mCanvas) return {};
    if (mBoard.open) {
        for (int i = 0; i < kBoardButtons; ++i)
            if (boardButtonRect(i).contains(x, y)) return {Hit::kBoardBtn, i};
        return {};
    }
    if (mMenu.open) {
        for (int row = 0; row < dialogRowCount(); ++row) {
            if (dialogRowIsValue(row)) {
                if (dialogArrowRect(row, false).grown(px(kArrowHitPad)).contains(x, y)) return {Hit::kDlgArrowL, row};
                if (dialogArrowRect(row, true).grown(px(kArrowHitPad)).contains(x, y)) return {Hit::kDlgArrowR, row};
            }
            const Rect r = dialogRowRect(row);
            if (r4(r.x0, r.y0 - px(3), r.x1, r.y1 + px(3)).contains(x, y)) return {Hit::kDlgRow, row};
        }
        if (dialogLoadRect().contains(x, y)) return {Hit::kDlgLoad, kMainSave};
        return {};
    }
    if (mContext.open) {
        for (int i = 0; i < static_cast<int>(mContext.items.size()); ++i)
            if (contextItemRect(i).contains(x, y)) return {Hit::kCtxItem, i};
        return {};
    }
    if (menuButtonHitRect().contains(x, y)) return {Hit::kMenuBtn, 0};
    if (barRect().contains(x, y)) {
        for (int i = 0; i < kAoeGridCells; ++i)
            if (gridHitRect(i).contains(x, y)) return {Hit::kGrid, i};
        const AoeSelection& sel = mModel.selection;
        if (sel.multi()) {
            const int n = std::min(static_cast<int>(sel.units.size()), kAoeMultiShown);
            for (int i = 0; i < n; ++i)
                if (slotHitRect(Hit::kMulti, i).contains(x, y)) return {Hit::kMulti, i};
        } else if (showsOrders()) {
            const int n = std::min(static_cast<int>(mModel.orders.size()), kAoeOrdersShown);
            for (int i = 0; i < n; ++i)
                if (slotHitRect(Hit::kOrder, i).contains(x, y)) return {Hit::kOrder, i};
        } else if (sel.count > 0) {
            const int nq = std::min(static_cast<int>(sel.queue.size()), kAoeQueueShown);
            for (int i = 0; i < nq; ++i)
                if (slotHitRect(Hit::kQueue, i).contains(x, y)) return {Hit::kQueue, i};
            const int ng = std::min(sel.garrison, kGarrisonVisible);
            for (int i = 0; i < ng; ++i)
                if (slotHitRect(Hit::kGarrison, i).contains(x, y)) return {Hit::kGarrison, i};
        }
        // Raute als Polygon
        const Rect m = minimapRect();
        if (!m.empty()) {
            const float dx = std::fabs(static_cast<float>(x) + 0.5f - static_cast<float>(m.cx()));
            const float dy = std::fabs(static_cast<float>(y) + 0.5f - static_cast<float>(m.cy()));
            if (dx / (static_cast<float>(m.w()) * 0.5f) + dy / (static_cast<float>(m.h()) * 0.5f) <= 1.0f)
                return {Hit::kMinimap, 0};
        }
    }
    return {};
}

int AoeUi::feedbackId(const HitState& h) const {
    switch (h.kind) {
    case Hit::kGrid: return mModel.cell(h.index).id;
    case Hit::kQueue: return kAoeCmdQueueBase + h.index;
    case Hit::kGarrison: return kIdGarrisonBase + h.index;
    case Hit::kMulti: return kIdMultiBase + h.index;
    case Hit::kOrder: return kIdOrderBase + h.index;
    case Hit::kMenuBtn: return kIdMenuBtn;
    case Hit::kMinimap: return kIdMinimap;
    case Hit::kCtxItem: return kIdCtxBase + h.index;
    case Hit::kDlgRow: case Hit::kDlgArrowL: case Hit::kDlgArrowR: return kIdDlgBase + h.index;
    case Hit::kDlgLoad: return kIdDlgLoad;
    case Hit::kBoardBtn: return kIdBoardBase + h.index;
    default: return 0;
    }
}

void AoeUi::onMove(int x, int y) {
    mHover = hitTest(x, y);
    if (mFocusMode && !mHover.none() && mHover.kind != Hit::kMinimap) mFocus = mHover;  // Fokus folgt dem Strahl
    mFeedback.hoverId = feedbackId(mHover);
}

void AoeUi::setPressed(const HitState& h, double now) {
    mPressed = h;
    mPressedUntil = now + kPressSeconds;
    ++mClickSeq;
    mFeedback.clickSeq = mClickSeq;
    mFeedback.clickedId = feedbackId(h);
}

AoeUi::Result AoeUi::stepValue(int row, int dir, double now) {
    (void)now;
    Result r;
    if (mMenu.page == AoeMenuModel::kSettings) {
        switch (row) {
        case kSetSpeed:
            mMenu.speed = clampi(mMenu.speed + dir, 0, kGameSpeedCount - 1);
            r.action = Action::kSpeedChanged;
            break;
        case kSetLabels: mMenu.labels = !mMenu.labels; r.action = Action::kSettingChanged; break;
        case kSetHotkeys: mMenu.hotkeys = !mMenu.hotkeys; r.action = Action::kSettingChanged; break;
        case kSetMessages: mMenu.messages = !mMenu.messages; r.action = Action::kSettingChanged; break;
        default: break;
        }
    } else if (mMenu.page == AoeMenuModel::kNewMap) {
        switch (row) {
        case kNewBiome: mMenu.biome = (mMenu.biome + dir + kGameUiBiomeCount) % kGameUiBiomeCount; break;
        case kNewSize: mMenu.size = clampi(mMenu.size + dir, 0, kGameUiSizeCount - 1); break;
        case kNewOpponent: mMenu.opponent = clampi(mMenu.opponent + dir, 0, kGameUiOpponentCount - 1); break;
        case kNewSeed: mMenu.seed = clampi(mMenu.seed + dir, kGameUiSeedMin, kGameUiSeedMax); break;
        default: break;
        }
    }
    return r;
}

AoeUi::Result AoeUi::dialogClick(const HitState& h, double now) {
    Result r;
    if (h.kind == Hit::kDlgArrowL || h.kind == Hit::kDlgArrowR) {
        setPressed(h, now);
        return stepValue(h.index, h.kind == Hit::kDlgArrowR ? 1 : -1, now);
    }
    if (h.kind == Hit::kDlgLoad) {
        setPressed(h, now);
        mMenu.page = AoeMenuModel::kLoad;
        mMenu.confirmSlot = -1;
        mMenu.confirmSurrender = false;
        r.action = Action::kSlotsShown;
        if (mFocusMode) mFocus = {Hit::kDlgRow, 0};
        return r;
    }
    if (h.kind == Hit::kDlgRow && slotPage(mMenu.page)) {
        const int row = h.index;
        if (row >= dialogRowCount() - 1) {  // ‹ Zurück
            setPressed(h, now);
            mMenu.page = AoeMenuModel::kMain;
            mMenu.confirmSlot = -1;
            if (mFocusMode) mFocus = {Hit::kDlgRow, kMainSave};
            return r;
        }
        if (!dialogRowEnabled(row)) return r;
        setPressed(h, now);
        const AoeSaveSlot& slot = mMenu.slots[static_cast<size_t>(row)];
        // belegten Slot überschreiben bzw. laden (laufendes Spiel geht verloren): zweiter Klick innerhalb 3 s
        const bool needsConfirm = mMenu.page == AoeMenuModel::kLoad || slot.exists;
        if (needsConfirm && mMenu.confirmSlot != row) {
            mMenu.confirmSlot = row;
            mMenu.armedAt = now;
            r.action = Action::kSlotArmed;
            r.id = slot.slot;
            return r;
        }
        r.action = mMenu.page == AoeMenuModel::kSave ? Action::kSave : Action::kLoad;
        r.id = slot.slot;
        mMenu.confirmSlot = -1;
        mMenu.open = false;
        mFocus = {};
        mFocusMode = false;
        return r;
    }
    if (h.kind != Hit::kDlgRow) {
        // Klick neben den Dialog: Rückfrage zurücknehmen, Dialog bleibt
        mMenu.confirmSurrender = false;
        return r;
    }
    const int row = h.index;
    if (!dialogRowEnabled(row)) return r;
    if (dialogRowIsValue(row)) {
        setPressed(h, now);
        return stepValue(row, 1, now);  // Klick auf die Zeile = eine Stufe weiter (Kippschalter)
    }
    setPressed(h, now);
    switch (mMenu.page) {
    case AoeMenuModel::kSettings:
        if (row == kSetBack) mMenu.page = AoeMenuModel::kMain;
        break;
    case AoeMenuModel::kNewMap:
        if (row == kNewBack) mMenu.page = AoeMenuModel::kMain;
        else if (row == kNewStart) {
            mMenu.open = false;
            r.action = Action::kNewMap;
        }
        break;
    default:
        switch (row) {
        case kMainResume:
            mMenu.open = false;
            mMenu.confirmSurrender = false;
            r.action = Action::kResume;
            break;
        case kMainSave:
            mMenu.page = AoeMenuModel::kSave;
            mMenu.confirmSlot = -1;
            mMenu.confirmSurrender = false;
            r.action = Action::kSlotsShown;
            break;
        case kMainSettings: mMenu.page = AoeMenuModel::kSettings; mMenu.confirmSurrender = false; break;
        case kMainNewMap: mMenu.page = AoeMenuModel::kNewMap; mMenu.confirmSurrender = false; break;
        case kMainSurrender:
            if (mMenu.confirmSurrender) {
                mMenu.confirmSurrender = false;
                mMenu.open = false;
                r.action = Action::kSurrender;
            } else {
                mMenu.confirmSurrender = true;
                mMenu.armedAt = now;
                r.action = Action::kSurrenderArmed;
            }
            break;
        case kMainClose:
            mMenu.open = false;
            mMenu.confirmSurrender = false;
            r.action = Action::kMenuClosed;
            break;
        default: break;
        }
    }
    if (!mMenu.open) {
        mFocus = {};
        mFocusMode = false;
    } else if (mFocusMode) {
        mFocus = {Hit::kDlgRow, 0};
    }
    return r;
}

AoeUi::Result AoeUi::clickHit(const HitState& h, int x, int y, double now) {
    Result r;
    if (mBoard.open) {
        if (h.kind == Hit::kBoardBtn) {
            setPressed(h, now);
            mBoard = MatchBoardModel{};
            if (h.index == 0) {
                r.action = Action::kBoardNewMap;
                mMenu.open = true;
                mMenu.page = AoeMenuModel::kNewMap;
                mMenu.confirmSurrender = false;
            } else {
                r.action = Action::kBoardClosed;
            }
        }
        return r;
    }
    if (mMenu.open) return dialogClick(h, now);
    if (mContext.open) {
        if (h.kind == Hit::kCtxItem && mContext.items[static_cast<size_t>(h.index)].enabled) {
            setPressed(h, now);
            r.action = Action::kContextItem;
            r.id = mContext.items[static_cast<size_t>(h.index)].id;
            r.index = h.index;
        } else {
            r.action = Action::kContextClosed;
        }
        closeContext();
        mHover = {};
        return r;
    }
    switch (h.kind) {
    case Hit::kGrid: {
        const AoeCommand& c = mModel.cell(h.index);
        if (c.id == 0) return r;
        if (!c.enabled) {
            if (c.reason.empty()) return r;
            r.action = Action::kBlocked;  // keine Drückfarbe, aber Meldung (Aufrufer)
            r.id = c.id;
            r.index = h.index;
            return r;
        }
        setPressed(h, now);
        r.action = Action::kCommand;
        r.id = c.id;
        r.index = h.index;
        return r;
    }
    case Hit::kQueue:
        setPressed(h, now);
        r.action = Action::kQueueCancel;
        r.index = h.index;
        r.id = kAoeCmdQueueBase + h.index;
        return r;
    case Hit::kMulti:
        setPressed(h, now);
        r.action = Action::kSelectUnit;
        r.index = h.index;
        return r;
    case Hit::kGarrison:
        return r;  // S3
    case Hit::kOrder:
        setPressed(h, now);
        r.action = Action::kFocusOrder;
        r.index = h.index;
        return r;
    case Hit::kMenuBtn:
        setPressed(h, now);
        openMenu(true, now);
        r.action = Action::kMenuOpened;
        return r;
    case Hit::kMinimap: {
        // S3: Kamera springen – heute nur die Kartenanteile melden (Aufrufer ignoriert sie)
        const Rect m = minimapRect();
        const float u = (static_cast<float>(x - m.cx()) / (static_cast<float>(m.w()) * 0.5f) +
                         static_cast<float>(y - m.cy()) / (static_cast<float>(m.h()) * 0.5f) + 1.0f) * 0.5f;
        const float v = (static_cast<float>(y - m.cy()) / (static_cast<float>(m.h()) * 0.5f) -
                         static_cast<float>(x - m.cx()) / (static_cast<float>(m.w()) * 0.5f) + 1.0f) * 0.5f;
        r.action = Action::kMinimapJump;
        r.fx = std::clamp(u, 0.0f, 1.0f);
        r.fy = std::clamp(v, 0.0f, 1.0f);
        return r;
    }
    default:
        return r;
    }
}

AoeUi::Result AoeUi::onClick(int x, int y, double now) {
    mHover = hitTest(x, y);
    mFeedback.hoverId = feedbackId(mHover);
    const HitState h = mHover;
    if (mFocusMode && !h.none() && h.kind != Hit::kMinimap) mFocus = h;
    return clickHit(h, x, y, now);
}

AoeUi::Result AoeUi::onKey(int qtKey, double now) {
    Result r;
    if (qtKey == kKeyEscape) {
        if (mContext.open) {
            closeContext();
            r.action = Action::kContextClosed;
            return r;
        }
        if (mFocusMode) focusGrid(false);
        return r;
    }
    if (mMenu.open || mBoard.open || mContext.open) return r;
    if (qtKey >= 'A' && qtKey <= 'Z') {
        const int cell = aoeCellOfHotkey(static_cast<char>(qtKey));
        if (cell < 0) return r;
        const Rect g = gridRect(cell);
        return clickHit({Hit::kGrid, cell}, g.cx(), g.cy(), now);
    }
    return r;
}

// ---- Fokus (Stick/A/B) ----------------------------------------------------------------------------

bool AoeUi::isGridFocusable(const HitState& h) const { return h.kind == Hit::kGrid; }

std::vector<AoeUi::HitState> AoeUi::focusOrder() const {
    std::vector<HitState> out;
    if (mBoard.open) {
        for (int i = 0; i < kBoardButtons; ++i) out.push_back({Hit::kBoardBtn, i});
        return out;
    }
    if (mMenu.open) {
        for (int i = 0; i < dialogRowCount(); ++i) {
            if (!dialogRowEnabled(i)) continue;
            out.push_back({Hit::kDlgRow, i});
            if (mMenu.page == AoeMenuModel::kMain && i == kMainSave && mMenu.loadAvailable) out.push_back({Hit::kDlgLoad, i});
        }
        return out;
    }
    if (mContext.open) {
        for (int i = 0; i < static_cast<int>(mContext.items.size()); ++i) out.push_back({Hit::kCtxItem, i});
        return out;
    }
    // Raster (15) → Warteschlange (≤ 5) → Garnison (≤ 5 sichtbar) → Minimap → Menü-Knopf
    for (int i = 0; i < kAoeGridCells; ++i) out.push_back({Hit::kGrid, i});
    const AoeSelection& sel = mModel.selection;
    if (sel.multi()) {
        const int n = std::min(static_cast<int>(sel.units.size()), kAoeMultiShown);
        for (int i = 0; i < n; ++i) out.push_back({Hit::kMulti, i});
    } else if (showsOrders()) {
        const int n = std::min(static_cast<int>(mModel.orders.size()), kAoeOrdersShown);
        for (int i = 0; i < n; ++i) out.push_back({Hit::kOrder, i});
    } else if (sel.count > 0) {
        const int nq = std::min(static_cast<int>(sel.queue.size()), kAoeQueueShown);
        for (int i = 0; i < nq; ++i) out.push_back({Hit::kQueue, i});
        const int ng = std::min(sel.garrison, kGarrisonVisible);
        for (int i = 0; i < ng; ++i) out.push_back({Hit::kGarrison, i});
    }
    out.push_back({Hit::kMinimap, 0});
    out.push_back({Hit::kMenuBtn, 0});
    return out;
}

void AoeUi::setFocus(const HitState& h) { mFocus = h; }

void AoeUi::focusGrid(bool on) {
    mFocusMode = on;
    if (!on) {
        mFocus = {};
        return;
    }
    const std::vector<HitState> order = focusOrder();
    bool present = false;
    for (const HitState& h : order) present = present || h == mFocus;
    if (!present) mFocus = order.empty() ? HitState{} : order.front();
}

AoeUi::Result AoeUi::navigate(int dx, int dy, double now) {
    Result r;
    if (!mFocusMode) {
        if (dx == 0 && dy == 0) return r;
        focusGrid(true);
    }
    const std::vector<HitState> order = focusOrder();
    if (order.empty()) return r;
    int idx = -1;
    for (size_t i = 0; i < order.size(); ++i)
        if (order[i] == mFocus) idx = static_cast<int>(i);
    if (idx < 0) {
        mFocus = order.front();
        return r;
    }
    const int n = static_cast<int>(order.size());
    if (mMenu.open) {
        if (dx != 0 && mFocus.kind == Hit::kDlgRow && dialogRowIsValue(mFocus.index)) {
            setPressed({dx > 0 ? Hit::kDlgArrowR : Hit::kDlgArrowL, mFocus.index}, now);
            return stepValue(mFocus.index, dx > 0 ? 1 : -1, now);
        }
        if (dy != 0) mFocus = order[static_cast<size_t>(((idx + (dy > 0 ? 1 : -1)) % n + n) % n)];
        else if (dx != 0) mFocus = order[static_cast<size_t>(((idx + (dx > 0 ? 1 : -1)) % n + n) % n)];
        return r;
    }
    if (mContext.open || mBoard.open) {
        const int step = dy != 0 ? (dy > 0 ? 1 : -1) : (dx > 0 ? 1 : -1);
        mFocus = order[static_cast<size_t>(((idx + step) % n + n) % n)];
        return r;
    }
    // Leiste: im Raster 2D (Umlauf über den Rand ins nächste Element), sonst linear
    int step = 0;
    if (mFocus.kind == Hit::kGrid && dy != 0) {
        const int cell = mFocus.index, row = cell / kGridCols;
        const int nrow = row + (dy > 0 ? 1 : -1);
        if (nrow >= 0 && nrow < kGridRows) {
            mFocus = {Hit::kGrid, nrow * kGridCols + cell % kGridCols};
            return r;
        }
        step = dy > 0 ? kGridCols : -kGridCols;
        if (dy < 0) step = kAoeGridCells > idx ? -(idx + 1) : -1;  // oberer Rand: zum letzten Element
    } else {
        step = dy != 0 ? (dy > 0 ? 1 : -1) : (dx > 0 ? 1 : -1);
    }
    mFocus = order[static_cast<size_t>(((idx + step) % n + n) % n)];
    return r;
}

AoeUi::Result AoeUi::activate(double now) {
    if (!mFocusMode || mFocus.none()) return {};
    const Rect rr = rectOf(mFocus);
    return clickHit(mFocus, rr.cx(), rr.cy(), now);
}

// ---- Zeichnen: Streifen ---------------------------------------------------------------------------

AoeUi::Rect AoeUi::rectOf(const HitState& h) const {
    switch (h.kind) {
    case Hit::kGrid: return gridRect(h.index);
    case Hit::kQueue: return queueRect(h.index);
    case Hit::kGarrison: return garrisonRect(h.index);
    case Hit::kMulti: return multiRect(h.index);
    case Hit::kOrder: return orderRect(h.index);
    case Hit::kMenuBtn: return menuButtonRect();
    case Hit::kMinimap: return minimapRect();
    case Hit::kCtxItem: return contextItemRect(h.index);
    case Hit::kDlgRow: return dialogRowRect(h.index);
    case Hit::kDlgArrowL: return dialogArrowRect(h.index, false);
    case Hit::kDlgArrowR: return dialogArrowRect(h.index, true);
    case Hit::kDlgLoad: return dialogLoadRect();
    case Hit::kBoardBtn: return boardButtonRect(h.index);
    default: return {};
    }
}

AoeUi::Rect AoeUi::bandOf(const HitState& h) const {
    Rect r = rectOf(h);
    if (r.empty()) return {};
    const int pad = px(kFocusFrame + 2);
    return r4(0, r.y0 - pad, width(), r.y1 + pad);
}

void AoeUi::markBand(int y0, int y1) {
    y0 = std::max(0, y0);
    y1 = std::min(height(), y1);
    if (y1 <= y0) return;
    mBands.push_back({y0, y1});
}

void AoeUi::markRect(const Rect& r) {
    if (!r.empty()) markBand(r.y0, r.y1);
}

int AoeUi::lastBandRows() const {
    int rows = 0;
    for (const Band& b : mLastBands) rows += b.y1 - b.y0;
    return rows;
}

const uint32_t* AoeUi::pixels(double now) {
    if (!mCanvas) return nullptr;
    mLastBands.clear();
    mBands.clear();
    // Fristen
    if (!mPressed.none() && now >= mPressedUntil) mPressed = {};
    if (mMenu.confirmSurrender && now - mMenu.armedAt >= kConfirmSeconds) mMenu.confirmSurrender = false;
    if (mMenu.confirmSlot >= 0 && now - mMenu.armedAt >= kConfirmSeconds) mMenu.confirmSlot = -1;
    aoeExpireMessages(mModel, now, kMsgSeconds, kFadeSeconds);
    const bool plakette = !mModel.plakette.empty() && now - mModel.plaketteAt < kPlaketteSeconds;
    bool fading = false;
    for (const AoeMessage& m : mModel.messages)
        fading = fading || (now - m.shownAt >= kMsgSeconds && now - m.shownAt < kMsgSeconds + kFadeSeconds);
    mFeedback.armed = mMenu.confirmSurrender || mMenu.confirmSlot >= 0;
    mFeedback.focusMode = mFocusMode;
    mFeedback.menuOpen = mMenu.open || mContext.open;

    const Rect ctxNow = contextRect(), dlgNow = dialogRect(), boardNow = boardRect();
    const bool overlayToggled = (mMenu.open != mShownMenu.open) || (mBoard.open != mShownBoard.open);
    if (mDirty || overlayToggled) {
        markBand(0, height());
    } else {
        // obere Leiste
        if (!mModel.sameTop(mShownModel) || plakette != mShownPlakette) markBand(0, px(kTopH));
        // Meldungen
        if (!mModel.sameMessages(mShownModel) || fading || mShownFading || mMenu.messages != mShownMenu.messages)
            markBand(barRect().y0 - px(kBarY - kBandMsgY0), barRect().y0);
        // untere Leiste
        if (!mModel.sameBar(mShownModel) || mMenu.labels != mShownMenu.labels || mMenu.hotkeys != mShownMenu.hotkeys)
            markRect(barRect());
        // Hover/Fokus/Drückfarbe: nur die Zeilen der betroffenen Elemente
        if (mHover != mShownHover) {
            markRect(bandOf(mShownHover));
            markRect(bandOf(mHover));
            // Sperrgrund des angezielten Knopfs steht im Auswahlfeld
            if (blockedOf(mHover) != blockedOf(mShownHover)) markRect(barRect());
        }
        if (mPressed != mShownPressed) { markRect(bandOf(mShownPressed)); markRect(bandOf(mPressed)); }
        if (mFocus != mShownFocus || mFocusMode != mShownFocusMode) { markRect(bandOf(mShownFocus)); markRect(bandOf(mFocus)); }
        // Kontextmenü, Dialog, Tafel: eigene Rechtecke (alt ∪ neu)
        if (mContext != mShownContext || ctxNow != mShownContextRect) { markRect(mShownContextRect.grown(px(kCtxShadow))); markRect(ctxNow.grown(px(kCtxShadow))); }
        if (mMenu != mShownMenu || dlgNow != mShownDialogRect) {
            if (!dlgNow.empty()) markBand(dlgNow.y0 - px(kDlgTitleH), dlgNow.y1);
            if (!mShownDialogRect.empty()) markBand(mShownDialogRect.y0 - px(kDlgTitleH), mShownDialogRect.y1);
        }
        if (mBoard != mShownBoard || boardNow != mShownBoardRect) { markRect(mShownBoardRect); markRect(boardNow); }
    }
    if (mBands.empty()) return mCanvas->data();

    // Streifen sortieren und verschmelzen
    std::sort(mBands.begin(), mBands.end(), [](const Band& a, const Band& b) { return a.y0 < b.y0; });
    std::vector<Band> merged;
    for (const Band& b : mBands) {
        if (!merged.empty() && b.y0 <= merged.back().y1) merged.back().y1 = std::max(merged.back().y1, b.y1);
        else merged.push_back(b);
    }
    for (const Band& b : merged) paintBand(b.y0, b.y1, now);
    mLastBands = merged;
    mShownModel = mModel;
    mShownMenu = mMenu;
    mShownContext = mContext;
    mShownBoard = mBoard;
    mShownHover = mHover;
    mShownPressed = mPressed;
    mShownFocus = mFocus;
    mShownFocusMode = mFocusMode;
    mShownContextRect = ctxNow;
    mShownDialogRect = dlgNow;
    mShownBoardRect = boardNow;
    mShownPlakette = plakette;
    mShownFading = fading;
    mDirty = false;
    ++mVersion;
    return mCanvas->data();
}

void AoeUi::paintBand(int y0, int y1, double now) {
    mCanvas->setClip(y0, y1);
    mCanvas->clearBand(y0, y1);
    drawAll(now);
    mCanvas->resetClip();
}

void AoeUi::drawAll(double now) {
    if (mCanvas->visible(0, px(kTopH))) drawTop(now);
    if (mMenu.messages && mCanvas->visible(barRect().y0 - px(kBarY - kBandMsgY0), barRect().y0)) drawMessages(now);
    if (mCanvas->visible(barRect().y0, height())) drawBar();
    if (mContext.open) drawContext();
    if (mMenu.open || mBoard.open) mSkin->drawPanel(*mCanvas, SkinPanel::kBackdrop, r4(0, 0, width(), height()), scale());
    if (mBoard.open) drawBoard();
    if (mMenu.open) drawDialog();
}

// ---- Zeichnen: Elemente ---------------------------------------------------------------------------

SkinState AoeUi::stateOf(const HitState& h, bool enabled) const {
    if (!enabled) return SkinState::kDisabled;
    if (h == mPressed) return SkinState::kPressed;
    if (h == mHover) return SkinState::kHover;
    return SkinState::kNormal;
}

std::string AoeUi::fitText(const std::string& s, float& pxSize, float minPx, int maxW, bool bold) const {
    if (!mCanvas || !mCanvas->hasFont()) return s;
    while (pxSize > minPx && mCanvas->textWidth(s.c_str(), pxSize, bold) > maxW) pxSize -= 1.0f;
    if (mCanvas->textWidth(s.c_str(), pxSize, bold) <= maxW) return s;
    std::string cut = s;
    while (!cut.empty()) {
        size_t n = cut.size() - 1;
        while (n > 0 && (static_cast<unsigned char>(cut[n]) & 0xC0) == 0x80) --n;
        cut.erase(n);
        const std::string t = cut + "…";
        if (mCanvas->textWidth(t.c_str(), pxSize, bold) <= maxW) return t;
    }
    return "…";
}

void AoeUi::textIn(const char* utf8, const Rect& r, float pxSize, uint32_t color, bool bold, bool shadow, int align) {
    if (r.empty()) return;
    const int w = mCanvas->textWidth(utf8, pxSize, bold);
    const int x = align == kAlignCenter ? r.cx() - w / 2 : align == kAlignRight ? r.x1 - w : r.x0;
    const int y = r.cy() - static_cast<int>(pxSize) / 2 - px(2);
    if (shadow) {
        mCanvas->text(utf8, x + 1, y + 1, pxSize, kShadow, bold);
        mCanvas->text(utf8, x, y, pxSize, color, bold);
    } else {
        mCanvas->text(utf8, x, y, pxSize, color, bold);
    }
}

void AoeUi::drawTop(double now) {
    const float s = scale();
    const Rect bar = topBarRect();
    mSkin->drawPanel(*mCanvas, SkinPanel::kTopBar, bar, s);
    const HudIcon icons[kTopCells] = {HudIcon::Food, HudIcon::Wood, HudIcon::Gold, HudIcon::Stone, HudIcon::Population};
    const std::string values[kTopCells] = {hudAmountText(mModel.food), hudAmountText(mModel.wood), hudAmountText(mModel.gold),
                                           hudAmountText(mModel.stone), hudPopulationText(mModel.population, mModel.populationMax)};
    const uint32_t ink = mSkin->inkOn(SkinPanel::kTopBar, SkinState::kNormal);
    for (int i = 0; i < kTopCells; ++i) {
        const Rect c = topCellRect(i);
        if (i > 0) mCanvas->fillRect(c.x0 - px(8), px(8), c.x0 - px(8) + px(2), bar.y1 - px(8), rgba(0xC9, 0xA2, 0x4A, 153));
        // Symbolrahmen 34 × 30 wie das Original: kleine Platte, darauf das Vektorsymbol
        const Rect ic = r4(c.x0, bar.cy() - px(kTopCellIconH) / 2, c.x0 + px(kTopCellIconW), bar.cy() + px(kTopCellIconH) / 2);
        VectorSkin::bevelRect(*mCanvas, ic, kStoneDark, std::max(1, px(1)));
        const uint32_t tint = i == 0 ? kFoodColor : i == 1 ? kWoodColor : i == 2 ? kGoldColor : i == 3 ? kStoneColor : kWhite;
        mCanvas->fillSdf(ic.x0, ic.y0, ic.x1, ic.y1, tint, [&](float x, float y) {
            return hudIconShape(icons[i], (x - static_cast<float>(ic.cx())) / (0.75f * s), (y - static_cast<float>(ic.cy())) / (0.75f * s)) * 0.75f * s;
        });
        float fpx = kPxNumber * s;
        const std::string v = fitText(values[i], fpx, kPxMin * s, c.w() - px(kTopCellTextDx) - px(8), true);
        const bool alert = i == 4 && mModel.housingAlert;
        textIn(v.c_str(), r4(c.x0 + px(kTopCellTextDx), c.y0, c.x1, c.y1), fpx, alert ? kAlertRed : ink, true, true, kAlignLeft);
    }
    // Zeitalter: Plakette + Text
    const Rect age = ageRect();
    if (age.w() > px(60)) {
        const Rect pl = r4(age.x0, bar.cy() - px(16), age.x0 + px(40), bar.cy() + px(16));
        mSkin->drawPanel(*mCanvas, SkinPanel::kPortrait, pl, s);
        textIn(ageNumeral(mModel.ageIndex), pl, kPxHotkey * s + px(4), kInk, true, false, kAlignCenter);
        float apx = kPxAge * s;
        const std::string t = fitText(mModel.age, apx, kPxMin * s, age.w() - px(52), true);
        textIn(t.c_str(), r4(pl.x1 + px(12), age.y0, age.x1, age.y1), apx, ink, true, true, kAlignLeft);
    }
    // Meldungsplakette (8 s)
    if (!mModel.plakette.empty() && now - mModel.plaketteAt < kPlaketteSeconds) {
        const Rect p = plaketteRect();
        mSkin->drawPanel(*mCanvas, SkinPanel::kCapsule, p, s);
        VectorSkin::frame(*mCanvas, p, std::max(1, px(1)), kGoldDark);
        float ppx = kPxPlakette * s;
        const std::string t = fitText(mModel.plakette, ppx, kPxMin * s, p.w() - px(16), true);
        const uint32_t col = mModel.plaketteKind == HudStatus::kWarn ? kMsgWarn : mModel.plaketteKind == HudStatus::kGood ? kMsgGood : kWhite;
        textIn(t.c_str(), p, ppx, col, true, true, kAlignCenter);
    }
    // Uhr (Pause: zwei Balken links davon)
    const Rect ck = clockRect();
    textIn(hudTimeText(mModel.gameSeconds).c_str(), ck, kPxNumber * s, ink, true, true, kAlignRight);
    if (mModel.paused) {
        const int tw = mCanvas->textWidth(hudTimeText(mModel.gameSeconds).c_str(), kPxNumber * s, true);
        const int bx = ck.x1 - tw - px(22), by = ck.cy();
        mCanvas->fillRect(bx, by - px(9), bx + px(5), by + px(9), ink);
        mCanvas->fillRect(bx + px(9), by - px(9), bx + px(14), by + px(9), ink);
    }
    // Knopf „Menü“
    const HitState mb{Hit::kMenuBtn, 0};
    const Rect m = menuButtonRect();
    const SkinState st = stateOf(mb, true);
    mSkin->drawButton(*mCanvas, SkinButton::kMenuTop, m, st, s);
    textIn("Menü", m, kPxMenuBtn * s, kWhite, true, true, kAlignCenter);
    if (mFocusMode && mFocus == mb) mSkin->drawFocus(*mCanvas, m, s);
}

void AoeUi::drawMessages(double now) {
    const float s = scale();
    const int n = std::min(static_cast<int>(mModel.messages.size()), kAoeMessagesMax);
    for (int i = 0; i < n; ++i) {
        const AoeMessage& m = mModel.messages[static_cast<size_t>(static_cast<int>(mModel.messages.size()) - 1 - i)];
        const float a = aoeMessageAlpha(m, now, kMsgSeconds, kFadeSeconds);
        if (a <= 0.0f) continue;
        const Rect r = messageRect(i);
        if (r.empty() || !mCanvas->visible(r.y0, r.y1)) continue;
        auto withAlpha = [a](uint32_t c) {
            const uint32_t al = static_cast<uint32_t>(static_cast<float>(c >> 24) * a);
            return (c & 0x00FFFFFFu) | (al << 24);
        };
        mCanvas->roundRect(r.x0, r.y0, r.x1, r.y1, px(6), withAlpha(kCapsule));
        const uint32_t bar = m.kind == 2 ? kAlertRed : m.kind == 1 ? kWarnYellow : kPlayerBlue;
        mCanvas->fillRect(r.x0, r.y0, r.x0 + px(kMsgBarW), r.y1, withAlpha(bar));
        float mpx = kPxMsg * s;
        const std::string t = fitText(m.text, mpx, kPxMin * s, r.w() - px(kMsgBarW + 24), false);
        textIn(t.c_str(), r4(r.x0 + px(kMsgBarW + 12), r.y0, r.x1, r.y1), mpx, withAlpha(kWhite), false, a >= 0.99f, kAlignLeft);
    }
}

void AoeUi::drawBar() {
    const float s = scale();
    mSkin->drawPanel(*mCanvas, SkinPanel::kCommands, commandsRect(), s);
    mSkin->drawPanel(*mCanvas, SkinPanel::kSelection, selectionRect(), s);
    mSkin->drawPanel(*mCanvas, SkinPanel::kMinimapField, minimapFieldRect(), s);
    drawGrid();
    drawSelection();
    drawMinimap();
}

void AoeUi::drawGrid() {
    const float s = scale();
    for (int i = 0; i < kAoeGridCells; ++i) {
        const AoeCommand& c = mModel.cell(i);
        const Rect r = gridRect(i);
        if (!mCanvas->visible(r.y0 - px(6), r.y1 + px(6))) continue;
        const HitState h{Hit::kGrid, i};
        if (c.id == 0) {
            if (mFocusMode && mFocus == h) mSkin->drawFocus(*mCanvas, r, s);
            continue;
        }
        const SkinState st = stateOf(h, c.enabled);
        mSkin->drawButton(*mCanvas, SkinButton::kCommand, r, st, s);
        const int iconCy = mMenu.labels ? r.y0 + px(32) : r.cy();
        mSkin->drawIcon(*mCanvas, c.icon, r.cx(), iconCy, static_cast<float>(kIconScaleBtn) * s * 0.7f, st);
        if (mMenu.hotkeys) {
            const char hk[2] = {aoeHotkey(i), 0};
            textIn(hk, r4(r.x1 - px(22), r.y0 + px(2), r.x1 - px(4), r.y0 + px(20)), kPxHotkey * s,
                   st == SkinState::kDisabled ? kDim : kGoldLight, true, true, kAlignRight);
        }
        if (mMenu.labels) {
            float lpx = kPxBtnLabel * s;
            const std::string t = fitText(c.label, lpx, kPxMin * s * 0.8f, r.w() - px(6), false);
            textIn(t.c_str(), r4(r.x0, r.y1 - px(20), r.x1, r.y1 - px(3)), lpx, st == SkinState::kDisabled ? kDim : kWhite,
                   false, true, kAlignCenter);
        }
        if (mFocusMode && mFocus == h) mSkin->drawFocus(*mCanvas, r, s);
    }
}

void AoeUi::drawSlotRow(const std::vector<AoeSlot>& slots, int shown, int x0, int y0, Hit kind, float progressFirst,
                        bool enabled, int maxX) {
    const float s = scale();
    const int n = static_cast<int>(slots.size());
    const int vis = std::min(n, shown);
    for (int i = 0; i < vis; ++i) {
        const Rect r = kind == Hit::kQueue ? queueRect(i) : kind == Hit::kGarrison ? garrisonRect(i) : multiRect(i);
        if (r.empty()) continue;
        const HitState h{kind, i};
        const SkinState st = stateOf(h, enabled);
        mSkin->drawButton(*mCanvas, SkinButton::kSlot, r, st, s);
        mSkin->drawIcon(*mCanvas, slots[static_cast<size_t>(i)].icon, r.cx(), r.cy() - (i == 0 && progressFirst >= 0.0f ? px(4) : 0), s * 1.1f, st);
        if (i == 0 && progressFirst >= 0.0f)
            mSkin->drawBar(*mCanvas, SkinBar::kQueue, r4(r.x0 + px(2), r.y1 - px(12), r.x1 - px(2), r.y1 - px(2)), progressFirst, s);
        if (mFocusMode && mFocus == h) mSkin->drawFocus(*mCanvas, r, s);
    }
    // leere Felder bis shown
    for (int i = vis; i < shown; ++i) {
        const Rect r = kind == Hit::kQueue ? queueRect(i) : kind == Hit::kGarrison ? garrisonRect(i) : multiRect(i);
        if (!r.empty()) mSkin->drawButton(*mCanvas, SkinButton::kSlotEmpty, r, SkinState::kNormal, s);
    }
    if (n > shown) {
        const Rect last = kind == Hit::kQueue ? queueRect(shown - 1) : kind == Hit::kGarrison ? garrisonRect(shown - 1) : multiRect(shown - 1);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "+%d", n - shown);
        textIn(buf, r4(last.x1 + px(8), last.y0, std::min(last.x1 + px(70), maxX), last.y1), kPxStats * s, kInk, true, false, kAlignLeft);
    }
    (void)x0;
    (void)y0;
}

std::string AoeUi::blockedOf(const HitState& h) const {
    if (h.kind != Hit::kGrid) return {};
    const AoeCommand& c = mModel.cell(h.index);
    if (c.id == 0 || c.enabled || c.reason.empty()) return {};
    return c.label + ": " + c.reason;
}

void AoeUi::drawSelection() {
    const float s = scale();
    const Rect field = selectionRect();
    AoeSelection sel = mModel.selection;
    const std::string hoverBlocked = blockedOf(mHover);
    if (!hoverBlocked.empty()) sel.blocked = hoverBlocked;
    const uint32_t ink = mSkin->inkOn(SkinPanel::kSelection, SkinState::kNormal);
    if (sel.count <= 0 && !mModel.orders.empty()) {
        // Bestellungen (0.6.0-xr.0.11) wie Porträts der Mehrfachauswahl: Symbol, Fortschritt, Anzahl; Klick = Gebäude
        const int n = std::min(static_cast<int>(mModel.orders.size()), kAoeOrdersShown);
        for (int i = 0; i < n; ++i) {
            const AoeOrder& o = mModel.orders[static_cast<size_t>(i)];
            const Rect r = orderRect(i);
            const HitState h{Hit::kOrder, i};
            const SkinState st = stateOf(h, true);
            mSkin->drawButton(*mCanvas, SkinButton::kSlot, r, st, s);
            mSkin->drawIcon(*mCanvas, o.icon == HudIcon::None ? HudIcon::Hammer : o.icon, r.cx(), r.cy() - px(4), s * 1.1f, st);
            mSkin->drawBar(*mCanvas, SkinBar::kQueue, r4(r.x0 + px(2), r.y1 - px(12), r.x1 - px(2), r.y1 - px(2)), o.progress, s);
            if (o.count > 1) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%d", o.count);
                textIn(buf, r4(r.x1 - px(24), r.y0 + px(2), r.x1 - px(4), r.y0 + px(22)), kPxHotkey * s, kGoldLight, true, true,
                       kAlignRight);
            }
            if (mFocusMode && mFocus == h) mSkin->drawFocus(*mCanvas, r, s);
        }
        // Kopfzeile und Erklärung zur angezielten bzw. ersten Bestellung
        const int hx = lx(static_cast<float>(multiX(kMultiCols - 1))) + px(kSlot + 24);
        const int textX1 = field.x1 - px(16);
        char head[48];
        std::snprintf(head, sizeof(head), "Bestellungen (%d)", static_cast<int>(mModel.orders.size()));
        float npx = kPxName * s;
        const std::string ht = fitText(head, npx, kPxMin * s, textX1 - hx, true);
        textIn(ht.c_str(), r4(hx, field.y0 + px(kMultiY0 - kBarY), textX1, field.y0 + px(kMultiY0 - kBarY + kSlot)), npx,
               ink, true, false, kAlignLeft);
        const int shown = mHover.kind == Hit::kOrder ? mHover.index : 0;
        if (shown >= 0 && shown < n) {
            const AoeOrder& o = mModel.orders[static_cast<size_t>(shown)];
            char line[160];
            if (o.remaining >= 0.0f)
                std::snprintf(line, sizeof(line), "%s%s%s · noch %d s", o.label.c_str(), o.building.empty() ? "" : " – ",
                              o.building.c_str(), static_cast<int>(o.remaining + 0.5f));
            else
                std::snprintf(line, sizeof(line), "%s%s%s", o.label.c_str(), o.building.empty() ? "" : " – ", o.building.c_str());
            float lpx = kPxStats * s;
            const std::string lt = fitText(line, lpx, kPxMin * s, textX1 - hx, false);
            textIn(lt.c_str(), r4(hx, field.y0 + px(kMultiY0 + kMultiPitch - kBarY), textX1,
                                  field.y0 + px(kMultiY0 + kMultiPitch + kSlot - kBarY)),
                   lpx, ink, false, false, kAlignLeft);
        }
        return;
    }
    if (sel.count <= 0) {
        // gedimmtes Wappen in der Mitte
        mSkin->drawIcon(*mCanvas, HudIcon::Emblem, field.cx(), field.cy(), 3.0f * s, SkinState::kDisabled);
        if (!sel.blocked.empty()) {
            float bpx = kPxStats * s;
            const std::string b = fitText(sel.blocked, bpx, kPxMin * s, field.w() - px(32), false);
            textIn(b.c_str(), r4(field.x0, field.y1 - px(40), field.x1, field.y1 - px(12)), bpx, kDanger, false, false, kAlignCenter);
        }
        return;
    }
    const int barY0 = field.y0;
    if (sel.multi()) {
        drawSlotRow(sel.units, kAoeMultiShown, 0, 0, Hit::kMulti, -1.0f, true, field.x1 - px(16));
        float npx = kPxName * s;
        const int hx = lx(static_cast<float>(multiX(kMultiCols - 1))) + px(kSlot + 24);
        const std::string t = fitText(sel.name, npx, kPxMin * s, field.x1 - px(16) - hx, true);
        textIn(t.c_str(), r4(hx, barY0 + px(kMultiY0 - kBarY), field.x1 - px(16), barY0 + px(kMultiY0 - kBarY + kSlot)), npx, ink, true, false, kAlignLeft);
        if (!sel.blocked.empty()) {
            float bpx = kPxStats * s;
            const std::string b = fitText(sel.blocked, bpx, kPxMin * s, field.x1 - px(16) - hx, false);
            textIn(b.c_str(), r4(hx, barY0 + px(kMultiY0 + kMultiPitch - kBarY), field.x1 - px(16), barY0 + px(kMultiY0 + kMultiPitch + kSlot - kBarY)), bpx, kDanger, false, false, kAlignLeft);
        }
        return;
    }
    // Einzelauswahl: Porträt, Name, HP/Fortschritt, Werte
    const Rect pr = portraitRect();
    mSkin->drawPanel(*mCanvas, SkinPanel::kPortrait, pr, s);
    mSkin->drawIcon(*mCanvas, sel.icon == HudIcon::None ? HudIcon::Villager : sel.icon, pr.cx(), pr.cy(), static_cast<float>(kIconScalePortrait) * s * 0.85f, SkinState::kNormal);
    const int textX1 = rx(kSelX1) - px(16);
    float npx = kPxName * s;
    std::string name = sel.name.empty() ? "Einheit" : sel.name;
    if (sel.count > 1) name = std::to_string(sel.count) + " × " + name;
    name = fitText(name, npx, kPxMin * s, textX1 - lx(kNameX), true);
    textIn(name.c_str(), r4(lx(kNameX), barY0 + px(kNameY - kBarY), textX1, barY0 + px(kNameY - kBarY) + px(36)), npx, ink, true, false, kAlignLeft);
    const Rect hp = hpRect();
    if (sel.progress >= 0.0f) {
        mSkin->drawBar(*mCanvas, SkinBar::kProgress, hp, sel.progress, s);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d %%", static_cast<int>(std::lround(100.0f * std::clamp(sel.progress, 0.0f, 1.0f))));
        textIn(buf, r4(hp.x1 + px(12), hp.y0 - px(6), textX1, hp.y1 + px(6)), kPxHp * s, ink, true, false, kAlignLeft);
    } else if (sel.hpMax > 0) {
        mSkin->drawBar(*mCanvas, SkinBar::kHp, hp, static_cast<float>(sel.hp) / static_cast<float>(sel.hpMax), s);
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%d / %d", std::max(sel.hp, 0), sel.hpMax);
        textIn(buf, r4(hp.x1 + px(12), hp.y0 - px(6), textX1, hp.y1 + px(6)), kPxHp * s, ink, true, false, kAlignLeft);
    }
    // Werte-Zeile bzw. Detail (Bau 30 %), darunter Sperrgrund
    const Rect statsR = r4(lx(kNameX), barY0 + px(kStatsY - kBarY), textX1, barY0 + px(kStatsY - kBarY) + px(26));
    const std::string line = !sel.detail.empty() ? sel.detail : sel.stats;
    if (!line.empty()) {
        float spx = kPxStats * s;
        const std::string t = fitText(line, spx, kPxMin * s, statsR.w(), false);
        textIn(t.c_str(), statsR, spx, ink, false, false, kAlignLeft);
    }
    if (!sel.blocked.empty()) {
        const Rect br = r4(statsR.x0, statsR.y0 + px(26), statsR.x1, statsR.y1 + px(26));
        float bpx = kPxStats * s;
        const std::string t = fitText(sel.blocked, bpx, kPxMin * s, br.w(), false);
        textIn(t.c_str(), br, bpx, kDanger, false, false, kAlignLeft);
    }
    // Warteschlange
    const int labelY = barY0 + px(kQueueLabelY - kBarY);
    if (!sel.queue.empty()) {
        textIn("Warteschlange", r4(lx(kQueueX0), labelY - px(12), lx(kQueueX0) + px(300), labelY + px(12)), kPxHeading * s, ink, true, false, kAlignLeft);
        drawSlotRow(sel.queue, kAoeQueueShown, 0, 0, Hit::kQueue, sel.queueProgress, true, lx(kGarrisonX0) - px(8));
    }
    // Garnison (S3: Platzhalter ausgegraut)
    if (sel.garrisonMax > 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "Garnison %d/%d", sel.garrison, sel.garrisonMax);
        textIn(buf, r4(lx(kGarrisonX0), labelY - px(12), lx(kGarrisonX0) + px(300), labelY + px(12)), kPxHeading * s, kDim, true, false, kAlignLeft);
        std::vector<AoeSlot> empty;
        drawSlotRow(empty, kGarrisonVisible, 0, 0, Hit::kGarrison, -1.0f, false, textX1);
    }
}

void AoeUi::drawMinimap() {
    const Rect m = minimapRect();
    if (m.empty() || !mCanvas->visible(m.y0 - px(4), m.y1 + px(4))) return;
    const AoeMinimap& mm = mModel.minimap;
    mSkin->drawMinimap(*mCanvas, m.cx(), m.cy(), m.w(), m.h(), mm.biome, mm.camX, mm.camY, mm.camW, mm.camH, scale());
    const HitState h{Hit::kMinimap, 0};
    if (mFocusMode && mFocus == h) mSkin->drawFocus(*mCanvas, m, scale());
}

void AoeUi::drawContext() {
    const Rect r = contextRect();
    if (r.empty() || !mCanvas->visible(r.y0, r.y1 + px(kCtxShadow))) return;
    const float s = scale();
    mSkin->drawPanel(*mCanvas, SkinPanel::kContext, r, s);
    if (!mContext.title.empty()) {
        const Rect t = r4(r.x0 + px(kCtxPad), r.y0 + px(kCtxPad), r.x1 - px(kCtxPad), r.y0 + px(kCtxPad) + px(kCtxTitle) - px(4));
        mSkin->drawPanel(*mCanvas, SkinPanel::kTitlePlateLight, t, s);
        float tpx = kPxCtx * s;
        const std::string tt = fitText(mContext.title, tpx, kPxMin * s, t.w() - px(16), true);
        textIn(tt.c_str(), t, tpx, kWhite, true, true, kAlignCenter);
    }
    for (int i = 0; i < static_cast<int>(mContext.items.size()); ++i) {
        const GameUiItem& it = mContext.items[static_cast<size_t>(i)];
        const Rect ir = contextItemRect(i);
        const HitState h{Hit::kCtxItem, i};
        const SkinState st = stateOf(h, it.enabled);
        const Rect inner = r4(ir.x0, ir.y0 + px(2), ir.x1, ir.y1 - px(2));
        mSkin->drawButton(*mCanvas, SkinButton::kCtxItem, inner, st, s);
        float ipx = kPxCtx * s;
        const std::string t = fitText(it.label, ipx, kPxMin * s, inner.w() - px(24), false);
        textIn(t.c_str(), r4(inner.x0 + px(12), inner.y0, inner.x1, inner.y1), ipx, mSkin->inkOn(SkinPanel::kContext, st), false, false, kAlignLeft);
        if (mFocusMode && mFocus == h) mSkin->drawFocus(*mCanvas, inner, s);
    }
}

void AoeUi::drawArrow(const Rect& r, bool right, SkinState st) {
    if (r.empty()) return;
    mSkin->drawButton(*mCanvas, SkinButton::kArrow, r, st, scale());
    const float cx = static_cast<float>(r.cx()), cy = static_cast<float>(r.cy()), h = static_cast<float>(r.h()) * 0.22f;
    const float v[6] = {right ? cx + h : cx - h, cy, right ? cx - h * 0.7f : cx + h * 0.7f, cy - h,
                        right ? cx - h * 0.7f : cx + h * 0.7f, cy + h};
    mCanvas->fillSdf(r.x0, r.y0, r.x1, r.y1, st == SkinState::kDisabled ? kDim : kWhite, [&](float x, float y) { return sdf::triangle(x, y, v); });
}

void AoeUi::drawDialog() {
    const Rect d = dialogRect();
    if (d.empty()) return;
    const float s = scale();
    mSkin->drawPanel(*mCanvas, SkinPanel::kDialog, d, s);
    const Rect t = dialogTitleRect();
    mSkin->drawPanel(*mCanvas, SkinPanel::kTitlePlate, t, s);
    const char* title = mMenu.page == AoeMenuModel::kSettings ? "Einstellungen"
                        : mMenu.page == AoeMenuModel::kNewMap ? "Neue Karte"
                        : mMenu.page == AoeMenuModel::kSave   ? "Speichern"
                        : mMenu.page == AoeMenuModel::kLoad   ? "Laden"
                                                              : "Spielmenü";
    textIn(title, t, kPxDialogTitle * s, kWhite, true, true, kAlignCenter);
    // Hinweiszeile
    std::string info = mMenu.page == AoeMenuModel::kNewMap ? "Startet die Partie neu (laufende Partie geht verloren)"
                       : mMenu.page == AoeMenuModel::kSettings ? "Spieltempo, Beschriftung, Hotkeys, Meldungen"
                       : mMenu.page == AoeMenuModel::kSave     ? "Platz wählen – belegte Plätze werden überschrieben"
                       : mMenu.page == AoeMenuModel::kLoad     ? "Spielstand wählen – die laufende Partie geht verloren"
                       : mMenu.mapInfo;
    if (!info.empty()) {
        float ipx = kPxDialogInfo * s;
        info = fitText(info, ipx, kPxMin * s, d.w() - px(40), false);
        textIn(info.c_str(), r4(d.x0, d.y0 + px(kDlgInfoY), d.x1, d.y0 + px(kDlgInfoY + 30)), ipx, kInk, false, false, kAlignCenter);
    }
    for (int row = 0; row < dialogRowCount(); ++row) {
        const Rect r = dialogRowRect(row);
        const HitState h{Hit::kDlgRow, row};
        const bool enabled = dialogRowEnabled(row);
        const SkinState st = stateOf(h, enabled);
        const std::string label = dialogRowLabel(row);
        if (dialogRowIsValue(row)) {
            mSkin->drawPanel(*mCanvas, SkinPanel::kValueCell, r, s);
            if (st == SkinState::kHover) mSkin->drawHoverFrame(*mCanvas, r, s);
            textIn(label.c_str(), r4(r.x0 + px(20), r.y0, r.x1, r.y1), kPxDialogValue * s, kInk, true, false, kAlignLeft);
            const Rect al = dialogArrowRect(row, false), ar = dialogArrowRect(row, true);
            drawArrow(al, false, stateOf({Hit::kDlgArrowL, row}, true));
            drawArrow(ar, true, stateOf({Hit::kDlgArrowR, row}, true));
            float vpx = kPxDialogValue * s;
            const std::string v = fitText(dialogRowValue(row), vpx, kPxMin * s, ar.x0 - al.x1 - px(8), true);
            textIn(v.c_str(), r4(al.x1, r.y0, ar.x0, r.y1), vpx, kInk, true, false, kAlignCenter);
        } else {
            const bool danger = (mMenu.page == AoeMenuModel::kMain && row == kMainSurrender) ||
                                (slotPage(mMenu.page) && row == mMenu.confirmSlot);
            mSkin->drawButton(*mCanvas, danger ? SkinButton::kDanger : SkinButton::kDialog, r, st, s);
            float bpx = kPxDialogBtn * s;
            const std::string l = fitText(label, bpx, kPxMin * s, r.w() - px(24), true);
            textIn(l.c_str(), r, bpx, enabled ? kWhite : kDim, true, true, kAlignCenter);
            if (mMenu.page == AoeMenuModel::kMain && row == kMainSave && mMenu.loadAvailable) {
                const Rect lr = dialogLoadRect();
                const SkinState lst = stateOf({Hit::kDlgLoad, row}, true);
                mSkin->drawButton(*mCanvas, SkinButton::kDialog, lr, lst, s);
                textIn("Laden", lr, bpx, kWhite, true, true, kAlignCenter);
                if (mFocusMode && mFocus == HitState{Hit::kDlgLoad, row}) mSkin->drawFocus(*mCanvas, lr, s);
            }
        }
        if (mFocusMode && mFocus == h) mSkin->drawFocus(*mCanvas, r, s);
    }
    if (mMenu.quest && mMenu.page == AoeMenuModel::kMain) {
        textIn("App beenden: VR-Menü (linke Menütaste)", r4(d.x0, d.y0 + px(kDlgHintY), d.x1, d.y0 + px(kDlgHintY + 26)),
               kPxDialogHint * s, kInk, false, false, kAlignCenter);
    }
}

void AoeUi::drawBoard() {
    const Rect b = boardRect();
    if (b.empty()) return;
    const float s = scale();
    mSkin->drawPanel(*mCanvas, SkinPanel::kDialog, b, s);
    const Rect head = r4(b.x0 + px(30), b.y0 + px(30), b.x1 - px(30), b.y0 + px(30 + 60));
    VectorSkin::bevelRect(*mCanvas, head, mBoard.good ? kVictory : kDanger, px(2));
    textIn(mBoard.headline.c_str(), head, (kPxDialogTitle + 6.0f) * s, kWhite, true, true, kAlignCenter);
    int y = head.y1 + px(10);
    for (const std::string& line : mBoard.lines) {
        float lpx = kPxCtx * s;
        const std::string t = fitText(line, lpx, kPxMin * s, b.w() - px(60), false);
        textIn(t.c_str(), r4(b.x0, y, b.x1, y + px(34)), lpx, kInk, false, false, kAlignCenter);
        y += px(34);
    }
    const char* labels[kBoardButtons] = {"Neue Karte ›", "Schließen"};
    for (int i = 0; i < kBoardButtons; ++i) {
        const Rect r = boardButtonRect(i);
        const HitState h{Hit::kBoardBtn, i};
        mSkin->drawButton(*mCanvas, SkinButton::kBoardBtn, r, stateOf(h, true), s);
        textIn(labels[i], r, kPxDialogBtn * s, kWhite, true, true, kAlignCenter);
        if (mFocusMode && mFocus == h) mSkin->drawFocus(*mCanvas, r, s);
    }
}

}  // namespace agesxr
