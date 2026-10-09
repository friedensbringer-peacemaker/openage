// xr.ages — VR-HUD: schlanke, immer sichtbare Leiste über dem Spielfenster (eigene Tafel neben dem Menü).
//
// Inhalt aus HudModel (xr_hud_model.h): Ressourcenleiste (Nahrung, Holz, Gold, Stein mit Vektor-Symbolen,
// Einwohner „3/5“, Spielzeit), Statusplakette, Auswahlfeld (Anzahl/Art, HP-Balken bei Einzelauswahl) und
// Befehlsleiste mit bis zu 8 Knöpfen (Symbol + Kurztext), per Strahl/Trigger klickbar.
// Optik wie das VR-Menü (Stardew-Look: Steintafel, Holzschild, Steinplatten, cremeweiße Schrift mit Schatten) über
// die gemeinsame Leinwand (xr_canvas.h). CPU-gerastert, keine GL-Aufrufe: die XR-Schicht lädt die Pixel nur bei
// version()-Wechsel hoch, und zwar nur die Streifen aus lastBands() (Teil-Redraw: obere bzw. untere Leiste).
#pragma once

#include <cstdint>
#include <vector>

#include "text_raster.h"
#include "xr_canvas.h"
#include "xr_hud_layout.h"
#include "xr_hud_model.h"

namespace agesxr {

// Abstandsfunktion eines Vektorsymbols (Symbolkoordinaten ≈ −16 … +16, +y unten), negativ = innen; gemeinsame
// Formen für VR-HUD und AoE-Oberfläche (xr_aoe_ui).
float hudIconShape(HudIcon icon, float px, float py);

class VrHud {
public:
    static constexpr int kWidth = hudlayout::kWidth;
    static constexpr int kHeight = hudlayout::kHeight;
    static constexpr double kPressSeconds = 0.18;  // Knopf leuchtet nach dem Klick grün
    static constexpr int kDiagHeight = 64;         // Diagnosemodus: nur diese Zeilen sind belegt

    bool init();  // false: keine System-Schrift (Logik und Formen funktionieren trotzdem, z. B. in Host-Tests)
    bool ok() const { return mOk; }

    // Strahl auf dem HUD in Texturpixeln (x/y < 0 = kein Treffer). clickEdge = Trigger gedrückt (Flanke).
    // Rückgabe: Index des geklickten Knopfs in model.buttons (nur aktive), sonst −1.
    int pointer(float x, float y, bool clickEdge, const HudModel& model, double now);
    int hoverButton() const { return mHover; }
    // Knopf unter (x, y) ohne Zustand (−1 = keiner); berücksichtigt nur vorhandene Knöpfe.
    static int buttonAt(float x, float y, int buttonCount);

    // Pixel (RGBA, Zeile 0 oben); zeichnet nur, was sich gegenüber dem gezeigten Stand geändert hat.
    const uint32_t* pixels(const HudModel& model, double now);
    uint32_t version() const { return mVersion; }
    struct DirtyBand {
        int y0 = 0, y1 = 0;
        bool empty() const { return y1 <= y0; }
    };
    const std::vector<DirtyBand>& lastBands() const { return mLastBands; }
    int lastBandRows() const;
    void invalidate() { mDirty = true; }  // nächster pixels() zeichnet alles (z. B. nach GL-Kontextverlust)

    // Diagnosemodus (0.6.0-xr.0.12, Spieloberfläche liegt im Engine-Bild): eine Zeile mit model.status, keine
    // Knöpfe; die XR-Schicht zeigt nur die oberen contentHeight() Zeilen.
    void setDiagnostic(bool on);
    bool diagnostic() const { return mDiagnostic; }
    int contentHeight() const { return mDiagnostic ? kDiagHeight : kHeight; }

    int measureText(const char* utf8, float px, bool bold = false) const { return mCanvas.textWidth(utf8, px, bold); }
    static constexpr float kNumberPx = 34.0f, kLabelPx = 22.0f, kKindPx = 28.0f, kStatusPx = 26.0f;

private:
    void paintBand(const HudModel& m, int y0, int y1);
    void drawScene(const HudModel& m);
    void drawTop(const HudModel& m);
    void drawBottom(const HudModel& m);
    void drawIcon(HudIcon icon, int cx, int cy, float scale, uint32_t color, bool shadow = true);
    // Text in maxW einpassen: erst verkleinern (bis minPx), dann mit „…“ kürzen (UTF-8-sicher).
    std::string fitText(const std::string& s, float& px, float minPx, int maxW, bool bold) const;

    bool mOk = false;
    bool mDiagnostic = false;
    TextRaster mFont, mFontBold;
    Canvas mCanvas{kWidth, kHeight};
    uint32_t mVersion = 1;
    bool mDirty = true;
    std::vector<DirtyBand> mLastBands;
    HudModel mShown;
    int mHover = -1, mShownHover = -1;
    int mPressed = -1, mShownPressed = -1;
    double mPressedUntil = 0.0;
};

}  // namespace agesxr
