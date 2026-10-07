// xr.ages — geglättete Menüschrift aus der System-TTF des Geräts (stb_truetype).
// Übernommen aus XR-Ports-Platform (XRShell, MIT) src/text_raster.h.
//
// Keine Schriftdatei im Repo: Android/Quest bringt Roboto bzw. DroidSans unter
// /system/fonts mit. Fehlt beides, meldet load() false; dann gibt es
// kein VR-Menü (XR läuft ohne Menü weiter). Text ist UTF-8 (Umlaute, ß).
// Am PC (Host-Tests, Desktop-Vorschau; nur außerhalb von __ANDROID__) wird zusätzlich nach DejaVuSans
// (conda/WSL, /usr/share/fonts) bzw. Segoe UI/Arial (Windows) gesucht; XRAGES_FONT/XRAGES_FONT_BOLD
// geben eine Datei vor.

#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

class TextRaster {
public:
    TextRaster();
    ~TextRaster();

    bool load(bool bold = false); // System-TTF suchen und öffnen (bold: fetter Schnitt)
    bool ok() const { return mOk; }

    // Breite in Pixeln bei Schriftgröße px (Zeilenhöhe).
    int width(const char* utf8, float px) const;
    // Text in einen RGBA-Puffer (R,G,B,A je Byte) mischen; y = Oberkante der Zeile.
    // clipY0/clipY1: nur Pixelzeilen in [clipY0, clipY1) schreiben (Teil-Redraw des Menüs; -1 = bis texH).
    void draw(uint32_t* pixels, int texW, int texH, const char* utf8,
              int x, int y, float px, uint32_t color, int clipY0 = 0, int clipY1 = -1) const;

private:
    struct Font;
    std::unique_ptr<Font> mFont;
    std::vector<unsigned char> mData;
    bool mOk = false;
    struct Glyph { int w = 0, h = 0, xoff = 0, yoff = 0; std::vector<unsigned char> pixels; };
    const Glyph& glyphBitmap(int glyph, float scale, int quarter) const;
    mutable std::unordered_map<uint64_t, Glyph> mGlyphs;   // Raster-Cache (Menü zeichnet oft neu)
};
