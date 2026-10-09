// xr.ages — Layout-Raster der Spieloberfläche im AoE-II-Stil (docs/UI-SPEC-AOE.md §4). Alle Maße in Pixeln bei
// 1920 × 1080 (Basis); AoeUi skaliert mit s = Fensterhöhe / 1080, links verankerte Elemente wachsen von links,
// rechts verankerte (Uhr, Knopf „Menü“, Minimap) von rechts, Auswahlfeld und Zeitalter-Plakette füllen dazwischen.
// Einzige Quelle für alle Maße: Oberfläche, Host-Tests, Vorschau und Replays rechnen hiermit (xr-vr-menu P5).
#pragma once

namespace agesxr::aoelayout {

constexpr int kBaseW = 1920, kBaseH = 1080;

// Obere Leiste
constexpr int kTopH = 48, kTopCellW = 200, kTopCellX0 = 16, kTopCells = 5;  // Nahrung Holz Gold Stein Einwohner
constexpr int kAgeX = 1016, kAgeW = 340, kMsgX = 1376, kMsgW = 250, kClockRight = 1724;
constexpr int kMenuBtnX = 1744, kMenuBtnW = 168, kMenuBtnH = 40, kMenuBtnHitH = 48;
constexpr int kTopCellIconW = 34, kTopCellIconH = 30, kTopCellTextDx = 48;

// Untere Leiste (liegt auf dem Spielbild, Variante A)
constexpr int kBarH = 264, kBarY = kBaseH - kBarH;  // 816
constexpr int kCmdX0 = 0, kCmdX1 = 440, kSelX0 = 440, kSelX1 = 1440, kMapX0 = 1440, kMapX1 = 1920;
constexpr int kBtn = 76, kBtnGap = 6, kBtnPitch = kBtn + kBtnGap, kGridCols = 5, kGridRows = 3;
constexpr int kGridX0 = 18, kGridY0 = kBarY + 12, kHitPad = 3;
constexpr int kIconPx = 36, kIconScaleBtn = 2, kIconScalePortrait = 3;
constexpr int kPortraitX = 456, kPortraitY = 832, kPortrait = 128;
constexpr int kNameX = 604, kNameY = 838, kHpX = 604, kHpY = 880, kHpW = 300, kHpH = 16, kStatsY = 912;
constexpr int kQueueLabelY = 978, kQueueX0 = 456, kQueueY0 = 1002, kSlot = 64, kSlotGap = 8, kQueueSlots = 5;
constexpr int kGarrisonX0 = 1000, kGarrisonSlots = 10, kMultiCols = 6, kMultiRows = 2, kMultiY0 = 840;
constexpr int kMultiX0 = 456, kMultiPitch = kSlot + kSlotGap;
constexpr int kMiniW = 440, kMiniH = 220, kMiniCx = 1680, kMiniCy = 948;
constexpr int kSlotHitPad = kSlotGap / 2;  // Slot-Trefferzone 72 × 72 (Feld + Lücke/2)

// Kontextmenü, Dialog, Meldungen
constexpr int kCtxW = 320, kCtxTitle = 44, kCtxRow = 56, kCtxPad = 8, kCtxMaxItems = 6, kCtxShadow = 8;
constexpr int kDlgW = 560, kDlgWWide = 680, kDlgH = 640, kDlgBtnW = 480, kDlgBtnH = 64, kDlgPitch = 76;
constexpr int kDlgTitleW = 360, kDlgTitleH = 56, kDlgRowsY = 96, kArrowW = 64, kArrowH = 48, kArrowHitPad = 8;
constexpr int kDlgFrame = 4, kDlgInfoY = 60, kDlgHintY = kDlgH - 40;
constexpr int kMsgRows = 4, kMsgRowH = 28, kMsgBottomY = 800, kMsgSeconds = 8, kMsgX0 = 16, kMsgW0 = 720;
constexpr int kMsgBarW = 8;

// Schrift (px bei Basis)
constexpr float kPxNumber = 30, kPxName = 32, kPxDialogBtn = 28, kPxAge = 26, kPxCtx = 24, kPxStats = 22,
                kPxMsg = 20, kPxHeading = 20, kPxHotkey = 16, kPxBtnLabel = 15, kPxDialogInfo = 20, kPxDialogHint = 18,
                kPxDialogTitle = 30, kPxDialogValue = 26, kPxMenuBtn = 26, kPxPlakette = 24, kPxHp = 22;
constexpr float kPxMin = 15.0f;  // keine Schrift darunter

// Zustände
constexpr double kPressSeconds = 0.18;    // Drückfarbe nach dem Klick
constexpr double kFadeSeconds = 0.30;     // Ausblenden einer Meldung
constexpr double kConfirmSeconds = 3.0;   // „Partie aufgeben“ scharf
constexpr double kPlaketteSeconds = 8.0;  // Meldungsplakette oben
constexpr int kFocusFrame = 3, kHoverFrame = 3;

constexpr int gridX(int c) { return kGridX0 + c * kBtnPitch; }
constexpr int gridY(int r) { return kGridY0 + r * kBtnPitch; }
constexpr int queueX(int i) { return kQueueX0 + i * (kSlot + kSlotGap); }
// Garnison: 10 Plätze im Modell, 5 sichtbare Felder + „+n“ (eine Reihe; zwei Reihen stießen an die Werte-Zeile)
constexpr int kGarrisonVisible = 5;
constexpr int garrisonX(int i) { return kGarrisonX0 + i * (kSlot + kSlotGap); }
constexpr int multiX(int i) { return kMultiX0 + (i % kMultiCols) * kMultiPitch; }
constexpr int multiY(int i) { return kMultiY0 + (i / kMultiCols) * kMultiPitch; }
constexpr int topCellX(int i) { return kTopCellX0 + i * kTopCellW; }

// Teil-Redraw-Streifen (volle Breite): obere Leiste, Meldungen, untere Leiste; Kontextmenü/Dialog eigene Rechtecke.
constexpr int kBandTopY0 = 0, kBandTopY1 = kTopH;
constexpr int kBandMsgY0 = kMsgBottomY - kMsgRows * kMsgRowH - 8, kBandMsgY1 = kBarY;
constexpr int kBandBarY0 = kBarY, kBandBarY1 = kBaseH;

static_assert(gridX(kGridCols - 1) + kBtn <= kCmdX1 - 18, "Raster passt nicht ins Befehlsfeld");
static_assert(gridY(kGridRows - 1) + kBtn <= kBaseH - 12, "Raster passt nicht in die Leiste");
static_assert(kQueueX0 + kQueueSlots * (kSlot + kSlotGap) < kGarrisonX0, "Warteschlange stößt an die Garnison");
static_assert(kGridX0 - kHitPad >= 0 && kBtnPitch == kBtn + 2 * kHitPad, "Trefferzonen lückenlos");
static_assert(kMenuBtnX + kMenuBtnW <= kBaseW - 8, "Menü-Knopf ragt aus dem Bild");
static_assert(kMsgX + kMsgW <= kClockRight - 90, "Meldungsplakette stößt an die Uhr");
static_assert(kMultiX0 + kMultiCols * kMultiPitch <= kSelX1 - 100, "Mehrfachauswahl zu breit");
// Mehrfachauswahl ersetzt Werte/Warteschlange (zwei Reihen à 64 + Lücke)
static_assert(kMultiY0 + kMultiRows * kMultiPitch <= kBaseH - 12, "Mehrfachauswahl ragt aus der Leiste");
static_assert(garrisonX(kGarrisonVisible - 1) + kSlot + 60 <= kSelX1 - 16, "Garnison + „+n“ ragt aus dem Auswahlfeld");
static_assert(kMiniCx - kMiniW / 2 >= kMapX0 + 8 && kMiniCx + kMiniW / 2 <= kMapX1 - 8, "Raute passt nicht ins Feld");
static_assert(kBandMsgY0 > kBandTopY1 && kBandMsgY1 == kBandBarY0, "Streifen überlappen");
static_assert(kDlgH <= kBaseH && kDlgWWide <= kBaseW, "Dialog größer als das Bild");

}  // namespace agesxr::aoelayout
