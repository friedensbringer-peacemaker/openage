// xr.ages — Layout-Raster des VR-HUD (Texturpixel, Zeile 0 oben). Einzige Quelle für alle Maße: HUD, Host-Tests,
// Desktop-Vorschau und XR-Schicht rechnen hiermit (Skill xr-vr-menu P5: ein Raster, keine Einzelwerte).
//
// Aufbau (1536 × 274, Steintafel wie das Menü):
//   oben   (kTopY0 … kTopY1)  Holzschild: Nahrung · Holz · Gold · Stein · Einwohner · Spielzeit | Statusplakette
//   mitte  (kBotY0 … kBotY1)  Auswahlfeld (Symbol, Anzahl/Art, Warteschlange als Symbolreihe, HP-/Fortschrittsbalken)
//                             | 8 Befehlsknöpfe (Symbol + Kurztext)
//   unten  (kOrdY0 … kOrdY1)  „Bestellungen“: alle laufenden Ausbildungen/Bauten eigener Gebäude (0.6.0-xr.0.11),
//                             bis zu 8 Einträge (Symbol, Was ×Anzahl, Gebäude · Restzeit, Fortschritt); Klick springt hin
// Breite in der Welt: Anteil der Fensterbreite (kWorldWidthFraction); 1536 px auf ~1,8 m ≈ 1,2 mm/px, Schrift
// 26–34 px ≈ 3–4 cm hoch bei 1,8 m Abstand.
#pragma once

namespace agesxr::hudlayout {

constexpr int kWidth = 1536;
constexpr int kHeight = 274;

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
// Mittlere Leiste (Auswahl, Befehle)
constexpr int kBotY0 = 88, kBotY1 = 196;
constexpr int kSelX0 = 16, kSelX1 = 420;               // Auswahlfeld
constexpr int kHpX0 = kSelX0 + 20, kHpX1 = kSelX1 - 20;  // HP-/Fortschrittsbalken
constexpr int kHpY0 = kBotY0 + 54, kHpY1 = kBotY0 + 66;
// Warteschlange des gewählten Gebäudes: bis zu 5 kleine Symbole rechts in der Titelzeile des Auswahlfelds
constexpr int kQueueCount = 5;
constexpr int kQueuePitch = 34;
constexpr int kQueueSize = 26;                          // Kästchen (Trefferzone = Kästchen + kHitPad)
constexpr int kQueueX0 = kSelX1 - 14 - kQueueCount * kQueuePitch;
constexpr int kQueueY0 = kBotY0 + 14, kQueueY1 = kQueueY0 + kQueueSize;
constexpr int kBtnX0 = 436;                             // erster Befehlsknopf
constexpr int kBtnPitch = 135;
constexpr int kBtnW = 127;
constexpr int kBtnCount = 8;
constexpr int kBtnIconDy = 36;                          // Symbolmitte unter der Knopfoberkante
constexpr int kBtnTextDy = 70;                          // Oberkante des Kurztexts
constexpr int kHitPad = 4;                              // Trefferzonen etwas größer als gezeichnet
// Untere Leiste „Bestellungen“
constexpr int kOrdY0 = 212, kOrdY1 = kHeight - 12;
constexpr int kOrdLabelX = 24;                          // Beschriftung „Bestellungen“ links
constexpr int kOrdX0 = 186;                             // erster Eintrag
constexpr int kOrdPitch = 166;
constexpr int kOrdW = 156;
constexpr int kOrdCount = 8;
constexpr int kOrdIconDx = 18;                          // Symbolmitte relativ zum Eintragsanfang
constexpr int kOrdTextDx = 38;                          // Text relativ zum Eintragsanfang
constexpr int kOrdBarH = 4;                             // Fortschrittsbalken am unteren Rand des Eintrags

// Teil-Redraw: drei Streifen (volle Breite → ein glTexSubImage2D je Streifen).
constexpr int kTopBandY0 = 0, kTopBandY1 = (kTopY1 + kBotY0) / 2;
constexpr int kBotBandY0 = kTopBandY1, kBotBandY1 = (kBotY1 + kOrdY0) / 2;
constexpr int kOrdBandY0 = kBotBandY1, kOrdBandY1 = kHeight;

constexpr int cellX(int i) { return kCellX0 + i * kCellW; }
constexpr int buttonX0(int i) { return kBtnX0 + i * kBtnPitch; }
constexpr int buttonX1(int i) { return buttonX0(i) + kBtnW; }
constexpr int queueX0(int i) { return kQueueX0 + i * kQueuePitch; }
constexpr int queueX1(int i) { return queueX0(i) + kQueueSize; }
constexpr int orderX0(int i) { return kOrdX0 + i * kOrdPitch; }
constexpr int orderX1(int i) { return orderX0(i) + kOrdW; }
constexpr int kTopCenterY = (kTopY0 + kTopY1) / 2;

// Lage am Spielfenster (Meter bzw. Anteil): Leiste oberhalb, mittig, wandert/skaliert mit dem Fenster.
constexpr float kWorldWidthFraction = 0.9f;   // HUD-Breite = Anteil der Fensterbreite (Sehne beim Zylinder)
constexpr float kWorldGap = 0.10f;            // m zwischen Bildoberkante und HUD-Unterkante (über den Eck-Anfassern)

static_assert(buttonX1(kBtnCount - 1) <= kWidth - 16, "Befehlsknöpfe passen nicht in die Breite");
static_assert(orderX1(kOrdCount - 1) <= kWidth - 16, "Bestellungen passen nicht in die Breite");
static_assert(kStatusX1 - kStatusX0 >= 300, "Statusplakette zu schmal");
static_assert(kHpY1 < kBotY1, "HP-Balken außerhalb des Auswahlfelds");
static_assert(kQueueX0 > kSelX0 + 64 + 120, "Warteschlange lässt der Art-Beschriftung zu wenig Platz");
static_assert(kQueueY1 < kHpY0, "Warteschlange überdeckt den Balken");
static_assert(kBotY1 + 2 * kHitPad <= kOrdY0, "Bestellungen überlappen die Befehlsknöpfe");
static_assert(kOrdY1 <= kHeight - 8, "Bestellungen reichen über die Tafel");

}  // namespace agesxr::hudlayout
