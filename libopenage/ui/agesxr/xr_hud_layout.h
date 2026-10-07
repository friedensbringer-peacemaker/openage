// xr.ages — Layout-Raster des VR-HUD (Texturpixel, Zeile 0 oben). Einzige Quelle für alle Maße: HUD, Host-Tests,
// Desktop-Vorschau und XR-Schicht rechnen hiermit (Skill xr-vr-menu P5: ein Raster, keine Einzelwerte).
//
// Aufbau (1536 × 208, Steintafel wie das Menü):
//   oben  (kTopY0 … kTopY1)  Holzschild: Nahrung · Holz · Gold · Stein · Einwohner · Spielzeit | Statusplakette
//   unten (kBotY0 … kBotY1)  Auswahlfeld (Symbol, Anzahl/Art, HP-Balken) | 8 Befehlsknöpfe (Symbol + Kurztext)
// Breite in der Welt: Anteil der Fensterbreite (kWorldWidthFraction); 1536 px auf ~1,8 m ≈ 1,2 mm/px, Schrift
// 26–34 px ≈ 3–4 cm hoch bei 1,8 m Abstand.
#pragma once

namespace agesxr::hudlayout {

constexpr int kWidth = 1536;
constexpr int kHeight = 208;

// Tafel
constexpr int kRadius = 30;
// Obere Leiste (Holzschild)
constexpr int kTopY0 = 10, kTopY1 = 78;
constexpr int kSignX0 = 12, kSignX1 = kWidth - 12;
constexpr int kCellX0 = 52;          // erste Zelle
constexpr int kCellW = 168;          // Zellenbreite (6 Zellen: 4 Rohstoffe, Einwohner, Zeit)
constexpr int kCells = 6;
constexpr int kCellIconDx = 20;      // Symbolmitte relativ zum Zellenanfang
constexpr int kCellTextDx = 46;      // Zahl relativ zum Zellenanfang
constexpr int kStatusX0 = kCellX0 + kCells * kCellW + 16, kStatusX1 = kWidth - 44;
// Untere Leiste
constexpr int kBotY0 = 88, kBotY1 = kHeight - 12;
constexpr int kSelX0 = 16, kSelX1 = 420;               // Auswahlfeld
constexpr int kHpX0 = kSelX0 + 20, kHpX1 = kSelX1 - 20;  // HP-Balken
constexpr int kHpY0 = kBotY0 + 54, kHpY1 = kBotY0 + 66;
constexpr int kBtnX0 = 436;                             // erster Befehlsknopf
constexpr int kBtnPitch = 135;
constexpr int kBtnW = 127;
constexpr int kBtnCount = 8;
constexpr int kBtnIconDy = 36;                          // Symbolmitte unter der Knopfoberkante
constexpr int kBtnTextDy = 70;                          // Oberkante des Kurztexts
constexpr int kHitPad = 4;                              // Trefferzonen etwas größer als gezeichnet

// Teil-Redraw: zwei Streifen (volle Breite → ein glTexSubImage2D je Streifen).
constexpr int kTopBandY0 = 0, kTopBandY1 = (kTopY1 + kBotY0) / 2;
constexpr int kBotBandY0 = kTopBandY1, kBotBandY1 = kHeight;

constexpr int cellX(int i) { return kCellX0 + i * kCellW; }
constexpr int buttonX0(int i) { return kBtnX0 + i * kBtnPitch; }
constexpr int buttonX1(int i) { return buttonX0(i) + kBtnW; }
constexpr int kTopCenterY = (kTopY0 + kTopY1) / 2;

// Lage am Spielfenster (Meter bzw. Anteil): Leiste oberhalb, mittig, wandert/skaliert mit dem Fenster.
constexpr float kWorldWidthFraction = 0.9f;   // HUD-Breite = Anteil der Fensterbreite (Sehne beim Zylinder)
constexpr float kWorldGap = 0.10f;            // m zwischen Bildoberkante und HUD-Unterkante (über den Eck-Anfassern)

static_assert(buttonX1(kBtnCount - 1) <= kWidth - 16, "Befehlsknöpfe passen nicht in die Breite");
static_assert(kStatusX1 - kStatusX0 >= 300, "Statusplakette zu schmal");
static_assert(kHpY1 < kBotY1, "HP-Balken außerhalb des Auswahlfelds");

}  // namespace agesxr::hudlayout
