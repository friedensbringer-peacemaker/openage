// xr.ages — geglättete Menüschrift (aus XRShell, MIT). Siehe text_raster.h.

#include "text_raster.h"

#include "stb_truetype.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>

#include "xr_log.h"

struct TextRaster::Font {
    stbtt_fontinfo info;
};

namespace {

// Nächster Unicode-Codepunkt aus UTF-8; ungültige Bytes werden übersprungen.
uint32_t nextCodepoint(const char*& s) {
    const auto* p = reinterpret_cast<const unsigned char*>(s);
    uint32_t cp = *p++;
    int extra = 0;
    if (cp >= 0xF0) { cp &= 0x07; extra = 3; }
    else if (cp >= 0xE0) { cp &= 0x0F; extra = 2; }
    else if (cp >= 0xC0) { cp &= 0x1F; extra = 1; }
    for (; extra > 0 && (*p & 0xC0) == 0x80; --extra) cp = (cp << 6) | (*p++ & 0x3F);
    s = reinterpret_cast<const char*>(p);
    return cp;
}

// Kandidaten in Suchreihenfolge: zuerst die Quest-Systemschrift, am PC danach Host-Schriften.
std::vector<std::string> fontCandidates(bool bold) {
    std::vector<std::string> fonts;
    if (bold) {
        fonts = {"/system/fonts/RobotoStatic-Bold.ttf", "/system/fonts/DroidSans-Bold.ttf"};
    }
    for (const char* p : {"/system/fonts/RobotoStatic-Regular.ttf", "/system/fonts/Roboto-Regular.ttf",
                          "/system/fonts/DroidSans.ttf"})
        fonts.emplace_back(p);
#ifndef __ANDROID__
    // Desktop-Vorschau und Host-Tests (nie auf der Quest): Umgebungsvariable, conda-Umgebung der
    // WSL-Werkzeuge (scripts/wsl/env.sh), Systemschriften von Linux und Windows.
    std::vector<std::string> host;
    if (const char* own = std::getenv(bold ? "XRAGES_FONT_BOLD" : "XRAGES_FONT")) host.emplace_back(own);
    if (bold) {
        host.emplace_back("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf");
        host.emplace_back("/usr/share/fonts/TTF/DejaVuSans-Bold.ttf");
        host.emplace_back("C:/Windows/Fonts/segoeuib.ttf");
        host.emplace_back("C:/Windows/Fonts/arialbd.ttf");
    }
    if (const char* prefix = std::getenv("CONDA_PREFIX")) host.emplace_back(std::string(prefix) + "/fonts/DejaVuSans.ttf");
    if (const char* home = std::getenv("HOME"))
        host.emplace_back(std::string(home) + "/xr-openage-build/tools/conda/fonts/DejaVuSans.ttf");
    host.emplace_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    host.emplace_back("/usr/share/fonts/TTF/DejaVuSans.ttf");
    host.emplace_back("C:/Windows/Fonts/segoeui.ttf");
    host.emplace_back("C:/Windows/Fonts/arial.ttf");
    fonts.insert(fonts.end(), host.begin(), host.end());
#endif
    return fonts;
}

} // namespace

TextRaster::TextRaster() = default;
TextRaster::~TextRaster() = default;

bool TextRaster::load(bool bold) {
    for (const std::string& path : fontCandidates(bold)) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) continue;
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        mData.resize(size > 0 ? static_cast<size_t>(size) : 0);
        const bool read = size > 0 && std::fread(mData.data(), 1, mData.size(), f) == mData.size();
        std::fclose(f);
        if (!read) continue;
        mFont = std::make_unique<Font>();
        if (stbtt_InitFont(&mFont->info, mData.data(),
                           stbtt_GetFontOffsetForIndex(mData.data(), 0))) {
            mOk = true;
            XLOGI("Menüschrift: %s", path.c_str());
            return true;
        }
    }
    XLOGI("Keine System-TTF gefunden — kein VR-Menü");
    mFont.reset();
    mData.clear();
    return false;
}

int TextRaster::width(const char* utf8, float px) const {
    if (!mOk) return 0;
    const float scale = stbtt_ScaleForPixelHeight(&mFont->info, px);
    float x = 0.0f;
    int prev = 0;
    for (const char* s = utf8; *s;) {
        const int glyph = stbtt_FindGlyphIndex(&mFont->info, static_cast<int>(nextCodepoint(s)));
        if (prev) x += scale * stbtt_GetGlyphKernAdvance(&mFont->info, prev, glyph);
        int advance = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&mFont->info, glyph, &advance, &lsb);
        x += scale * advance;
        prev = glyph;
    }
    return static_cast<int>(std::ceil(x));
}

void TextRaster::draw(uint32_t* pixels, int texW, int texH, const char* utf8,
                      int x, int y, float px, uint32_t color, int clipY0, int clipY1) const {
    if (!mOk) return;
    const int yMin = std::max(clipY0, 0), yMax = clipY1 < 0 ? texH : std::min(clipY1, texH);
    const float scale = stbtt_ScaleForPixelHeight(&mFont->info, px);
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&mFont->info, &ascent, &descent, &gap);
    const int baseline = y + static_cast<int>(std::lround(ascent * scale));
    const uint32_t cr = color & 0xFF, cg = (color >> 8) & 0xFF, cb = (color >> 16) & 0xFF;
    const uint32_t ca = (color >> 24) & 0xFF;

    float pen = static_cast<float>(x);
    int prev = 0;
    for (const char* s = utf8; *s;) {
        const int glyph = stbtt_FindGlyphIndex(&mFont->info, static_cast<int>(nextCodepoint(s)));
        if (prev) pen += scale * stbtt_GetGlyphKernAdvance(&mFont->info, prev, glyph);
        int advance = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&mFont->info, glyph, &advance, &lsb);

        // Viertelpixel-Versatz reicht optisch; so lässt sich die Glyphe zwischenspeichern.
        const int quarter = static_cast<int>((pen - std::floor(pen)) * 4.0f) & 3;
        const Glyph& cached = glyphBitmap(glyph, scale, quarter);
        const int w = cached.w, h = cached.h, xoff = cached.xoff, yoff = cached.yoff;
        const unsigned char* bitmap = cached.pixels.data();
        const int gx = static_cast<int>(std::floor(pen)) + xoff, gy = baseline + yoff;
        for (int row = 0; row < h; ++row) {
            const int ty = gy + row;
            if (ty < yMin || ty >= yMax) continue;
            for (int col = 0; col < w; ++col) {
                const int tx = gx + col;
                if (tx < 0 || tx >= texW) continue;
                const uint32_t cov = bitmap[row * w + col] * ca / 255;
                if (!cov) continue;
                uint32_t& dst = pixels[ty * texW + tx];
                const uint32_t inv = 255 - cov;
                const uint32_t r = (cr * cov + (dst & 0xFF) * inv) / 255;
                const uint32_t g = (cg * cov + ((dst >> 8) & 0xFF) * inv) / 255;
                const uint32_t b = (cb * cov + ((dst >> 16) & 0xFF) * inv) / 255;
                const uint32_t a = std::max(cov, (dst >> 24) & 0xFF);
                dst = r | (g << 8) | (b << 16) | (a << 24);
            }
        }
        pen += scale * advance;
        prev = glyph;
    }
}

// Gerasterte Glyphe aus dem Cache (Schlüssel: Glyphe, Größe, Viertelpixel-Versatz).
const TextRaster::Glyph& TextRaster::glyphBitmap(int glyph, float scale, int quarter) const {
    const uint64_t key = (static_cast<uint64_t>(glyph) << 32) |
                         (static_cast<uint64_t>(std::lround(scale * 100000.0f)) << 2) |
                         static_cast<uint64_t>(quarter);
    auto it = mGlyphs.find(key);
    if (it != mGlyphs.end()) return it->second;
    Glyph g;
    unsigned char* bitmap = stbtt_GetGlyphBitmapSubpixel(&mFont->info, scale, scale,
                                                         static_cast<float>(quarter) * 0.25f, 0.0f,
                                                         glyph, &g.w, &g.h, &g.xoff, &g.yoff);
    if (bitmap) {
        g.pixels.assign(bitmap, bitmap + static_cast<size_t>(g.w) * g.h);
        stbtt_FreeBitmap(bitmap, nullptr);
    }
    return mGlyphs.emplace(key, std::move(g)).first->second;
}
