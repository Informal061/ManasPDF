// =====================================================
// PdfTextExtractor.cpp
// =====================================================

#include "pch.h"
#include "PdfTextExtractor.h"
#include "PdfContentParser.h"
#include "PdfDebug.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pdf
{
    // =====================================================
    // Helpers
    // =====================================================

    // WinAnsi (CP1252) encoding tablosu - PdfContentParser.cpp ile aynı
    static const uint16_t WinAnsi[256] =
    {
        /* 0–31 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,

        /* 32–63 */
        32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,
        48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,

        /* 64–95 */
        64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,
        80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,

        /* 96–127 */
        96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,
        112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,

        /* 128–159 (WinAnsi special) */
        0x20AC,0,0x201A,0x0192,0x201E,0x2026,0x2020,0x2021,
        0x02C6,0x2030,0x0160,0x2039,0x0152,0,0x017D,0,
        0,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,
        0x02DC,0x2122,0x0161,0x203A,0x0153,0,0x017E,0,

        /* 160–191 */
        160,0x00A1,0x00A2,0x00A3,0x00A4,0x00A5,0x00A6,0x00A7,
        0x00A8,0x00A9,0x00AA,0x00AB,0x00AC,0x00AD,0x00AE,0x00AF,
        0x00B0,0x00B1,0x00B2,0x00B3,0x00B4,0x00B5,0x00B6,0x00B7,
        0x00B8,0x00B9,0x00BA,0x00BB,0x00BC,0x00BD,0x00BE,0x00BF,

        /* 192–223 */
        0x00C0,0x00C1,0x00C2,0x00C3,0x00C4,0x00C5,0x00C6,0x00C7,
        0x00C8,0x00C9,0x00CA,0x00CB,0x00CC,0x00CD,0x00CE,0x00CF,
        0x00D0,0x00D1,0x00D2,0x00D3,0x00D4,0x00D5,0x00D6,0x00D7,
        0x00D8,0x00D9,0x00DA,0x00DB,0x00DC,0x00DD,0x00DE,0x00DF,

        /* 224–255 */
        0x00E0,0x00E1,0x00E2,0x00E3,0x00E4,0x00E5,0x00E6,0x00E7,
        0x00E8,0x00E9,0x00EA,0x00EB,0x00EC,0x00ED,0x00EE,0x00EF,
        0x00F0,0x00F1,0x00F2,0x00F3,0x00F4,0x00F5,0x00F6,0x00F7,
        0x00F8,0x00F9,0x00FA,0x00FB,0x00FC,0x00FD,0x00FE,0x00FF
    };

    static inline bool isCidFont(const PdfFontInfo* f)
    {
        if (!f) return false;
        return f->isCidFont ||
            f->encoding == "/Identity-H" ||
            f->encoding == "/Identity-V";
    }

    static int getW1000(const PdfFontInfo* f, int code)
    {
        if (!f) return 500;
        if (isCidFont(f))
        {
            auto it = f->cidWidths.find((uint16_t)code);
            if (it != f->cidWidths.end()) return it->second;
            return (f->cidDefaultWidth > 0) ? f->cidDefaultWidth : 1000;
        }
        if (f->hasWidths &&
            code >= f->firstChar &&
            code < f->firstChar + (int)f->widths.size())
        {
            int w = f->widths[code - f->firstChar];
            if (w > 0) return w;
        }
        return (f->missingWidth > 0) ? f->missingWidth : 500;
    }

    static uint32_t toUnicode(const PdfFontInfo* f, int code, bool cid)
    {
        if (!f) return (uint32_t)code;

        // CID font için cidToUnicode kullan
        if (cid)
        {
            auto it = f->cidToUnicode.find((uint16_t)code);
            if (it != f->cidToUnicode.end()) return it->second;
            return (code >= 0x20 && code <= 0xFFFF) ? (uint32_t)code : 0xFFFD;
        }

        // Simple font için - PdfContentParser.cpp ile aynı mantık
        if (code < 0 || code >= 256)
            return (uint32_t)code;

        // 1) Önce hasSimpleMap kontrolü - ToUnicode CMap varsa kullan
        if (f->hasSimpleMap && f->codeToUnicode[code] != 0)
        {
            return f->codeToUnicode[code];
        }

        // 2) hasSimpleMap yoksa encoding'e göre fallback
        if (f->encoding == "/WinAnsiEncoding" || f->encoding.empty())
        {
            return WinAnsi[code];
        }
        else if (f->encoding == "/MacRomanEncoding")
        {
            // MacRoman için basit fallback - ASCII range
            return (uint32_t)code;
        }

        // 3) Son fallback - raw code
        return (uint32_t)code;
    }


    // =====================================================
    // PdfTextCollectorPainter
    // =====================================================

    PdfTextCollectorPainter::PdfTextCollectorPainter(
        double pageWPt, double pageHPt)
        : _pageWPt(pageWPt)
        , _pageHPt(pageHPt)
    {
        // Bitmap pixel boyutu zoom=1 icin (aynen PdfCore.cpp'deki gibi)
        _pixW = (int)std::llround(pageWPt * TEXT_PT_TO_PX);
        _pixH = (int)std::llround(pageHPt * TEXT_PT_TO_PX);
        _glyphs.reserve(4096);
    }

    void PdfTextCollectorPainter::setPageRotation(
        int degrees, double pageWPt, double pageHPt)
    {
        // pageWPt ve pageHPt zaten rotation sonrası değerler
        // (getPageSize rotation'ı hesaba katıyor)
        // CTM de rotation'ı içeriyor, o yüzden text koordinatları
        // zaten doğru page space'te geliyor.
        _pageWPt = pageWPt;
        _pageHPt = pageHPt;
        _rotation = degrees;

        // Final bitmap boyutları
        _pixW = (int)std::llround(pageWPt * TEXT_PT_TO_PX);
        _pixH = (int)std::llround(pageHPt * TEXT_PT_TO_PX);

        _hasRotate = (degrees != 0);
    }

    void PdfTextCollectorPainter::toBitmapPx(
        double pageX, double pageY,
        double& outPx, double& outPy) const
    {
        // ÖNEMLİ: PdfContentParser, text koordinatlarını CTM ile dönüştürüp
        // page space'e çeviriyor. Yani buraya gelen (pageX, pageY) zaten
        // rotation uygulanmış koordinatlar!
        //
        // getPageSize() de rotation sonrası boyut döndürüyor.
        // Dolayısıyla rotation'a göre ayrı işlem yapmamıza GEREK YOK.
        //
        // Tek yapmamız gereken: PDF koordinat sisteminden (sol-alt orijin)
        // bitmap koordinat sistemine (sol-üst orijin) Y-flip yapmak.
        //
        // Formül:
        //   bitmapX = pageX * scale
        //   bitmapY = (pageHeight - pageY) * scale

        double bx = pageX * TEXT_PT_TO_PX;
        double by = (_pageHPt - pageY) * TEXT_PT_TO_PX;

        outPx = bx;
        outPy = by;
    }


    // =====================================================
    // drawTextFreeTypeRaw - glyph toplama
    // =====================================================
    double PdfTextCollectorPainter::drawTextFreeTypeRaw(
        double x, double y,
        const std::string& raw,
        double fontSizePt,
        double advanceSizePt,
        uint32_t /*color*/,
        const PdfFontInfo* font,
        double charSpacing,
        double wordSpacing,
        double horizScale,
        double textAngle)
    {
        if (!font || raw.empty() || fontSizePt < 0.1)
            return 0.0;

        // fontSizePt, charSpacing, wordSpacing hepsi "effective" degerler
        // (fontSize * tmScale * ctmScale tarafindan PdfContentParser'da hesaplandi)
        // Bunlar page-space point cinsinden.

        const bool cidMode = isCidFont(font);
        const double hScale = horizScale / 100.0;
        const double S = TEXT_PT_TO_PX; // point -> pixel cevirici

        // Font yuksekligi pixel olarak
        double fontPx = fontSizePt * S;

        // Text rotation support
        double cosA = std::cos(textAngle);
        double sinA = std::sin(textAngle);
        bool hasTextRotation = (std::abs(textAngle) > 0.001);

        // Kalem pozisyonu page space'te
        double penPageX = x;
        double penPageY = y;  // Y tracking for rotated text

        auto addGlyph = [&](int code, uint32_t uni) {
            double glyphW_pt;
            if (font && font->isType3) {
                double fmA = std::abs(font->type3FontMatrix.a);
                if (fmA < 1e-10) fmA = 0.001;
                glyphW_pt = getW1000(font, code) * fmA * advanceSizePt;
            } else {
                glyphW_pt = (getW1000(font, code) / 1000.0) * advanceSizePt;
            }
            double advance_pt = glyphW_pt + charSpacing;
            if (code == 32 || uni == 32)
                advance_pt += wordSpacing;
            advance_pt *= hScale;

            // Bitmap pixel pozisyon
            double bx, by;
            toBitmapPx(penPageX, hasTextRotation ? penPageY : y, bx, by);

            PdfTextGlyphInfo g;
            g.unicode = uni;
            g.bitmapX = bx;
            g.bitmapY = by - fontPx;  // baseline'dan uste (glyph top)
            g.width = std::abs(advance_pt) * S;
            g.height = fontPx;
            g.fontSize = fontPx;
            g.isSpace = (uni == 32 || uni == 0x00A0);
            _glyphs.push_back(g);

            if (hasTextRotation) {
                penPageX += advance_pt * cosA;
                penPageY += advance_pt * sinA;
            } else {
                penPageX += advance_pt;
            }
            };

        if (cidMode)
        {
            for (size_t i = 0; i + 1 < raw.size(); i += 2)
            {
                int code = ((unsigned char)raw[i] << 8) | (unsigned char)raw[i + 1];
                addGlyph(code, toUnicode(font, code, true));
            }
        }
        else
        {
            for (unsigned char c : raw)
            {
                addGlyph((int)c, toUnicode(font, (int)c, false));
            }
        }

        if (hasTextRotation) {
            double dx = penPageX - x;
            double dy = penPageY - y;
            return std::sqrt(dx * dx + dy * dy);
        }
        return penPageX - x; // toplam advance (page point)
    }


    // =====================================================
    // Sort & Export
    // =====================================================
    void PdfTextCollectorPainter::sortGlyphs()
    {
        // NOT: Glyph'leri SIRALAMAYIN!
        // PDF'deki orijinal sırayı koruyoruz.
        // Sıralama yaparsak farklı text block'lar karışır.
        // Kullanıcı mouse ile seçerken spatial hit-test yapıyoruz,
        // sıralama gereksiz.

        // Sadece satir sonlarini isaretle (okuma sirasina gore)
        if (_glyphs.empty()) return;

        for (size_t i = 1; i < _glyphs.size(); i++)
        {
            double tol = std::min(_glyphs[i - 1].height, _glyphs[i].height) * 0.5;
            // Y farkı büyükse veya X geri gittiyse yeni satır
            if (std::abs(_glyphs[i].bitmapY - _glyphs[i - 1].bitmapY) > tol ||
                _glyphs[i].bitmapX < _glyphs[i - 1].bitmapX - 50)
            {
                _glyphs[i - 1].isNewLine = true;
            }
        }
        _glyphs.back().isNewLine = true;
    }

    void PdfTextCollectorPainter::exportGlyphs(
        std::vector<PdfTextGlyphExport>& out) const
    {
        out.resize(_glyphs.size());
        for (size_t i = 0; i < _glyphs.size(); i++)
        {
            auto& g = _glyphs[i];
            out[i].unicode = g.unicode;
            out[i].x = (float)g.bitmapX;
            out[i].y = (float)g.bitmapY;
            out[i].width = (float)g.width;
            out[i].height = (float)g.height;
            out[i].fontSize = (float)g.fontSize;
        }
    }


    // =====================================================
    // PdfTextExtractor
    // =====================================================
    static const std::vector<PdfTextGlyphExport> s_empty;

    int PdfTextExtractor::extractPage(PdfDocument& doc, int pageIndex)
    {
        double wPt = 0, hPt = 0;
        if (!doc.getPageSize(pageIndex, wPt, hPt))
            return -1;

        int rotation = doc.getPageRotate(pageIndex);

        // DEBUG: Sayfa boyutlarını logla
        LogDebug("[TextExtract] Page %d: size=%.1fx%.1f pt, rotation=%d",
            pageIndex, wPt, hPt, rotation);

        PdfTextCollectorPainter collector(wPt, hPt);
        collector.setPageRotation(rotation, wPt, hPt);

        // Sayfa icerigini al
        std::vector<uint8_t> content;
        if (!doc.getPageContentsBytes(pageIndex, content))
            return 0;

        // Font bilgilerini al
        std::map<std::string, PdfFontInfo> fonts;
        doc.getPageFonts(pageIndex, fonts);

        // Width tablolari icin FreeType hazirla
        // (bazi CID fontlarda width = 0 ise FreeType'dan alinir)
        for (auto& kv : fonts)
        {
            if (!kv.second.ftReady && !kv.second.fontProgram.empty())
                doc.prepareFreeTypeFont(kv.second);
        }

        // Resource stack
        std::vector<std::shared_ptr<PdfDictionary>> resStack;
        doc.getPageResources(pageIndex, resStack);
        std::reverse(resStack.begin(), resStack.end());

        // Parse
        // ÖNEMLİ: Rotation için initial CTM ayarla (renderPageToPainter ile aynı)
        // Content stream rotation-öncesi (raw) koordinatlarda yazılmış.
        // CTM ile rotation sonrası koordinatlara dönüştürülüyor.
        double rawW = 0, rawH = 0;
        doc.getRawPageSize(pageIndex, rawW, rawH);

        PdfGraphicsState gs;
        gs.ctm = PdfMatrix(); // Identity default

        if (rotation == 90) {
            gs.ctm.a = 0;  gs.ctm.b = -1;
            gs.ctm.c = 1;  gs.ctm.d = 0;
            gs.ctm.e = 0;  gs.ctm.f = rawW;
        }
        else if (rotation == 180) {
            gs.ctm.a = -1; gs.ctm.b = 0;
            gs.ctm.c = 0;  gs.ctm.d = -1;
            gs.ctm.e = rawW; gs.ctm.f = rawH;
        }
        else if (rotation == 270) {
            gs.ctm.a = 0;  gs.ctm.b = 1;
            gs.ctm.c = -1; gs.ctm.d = 0;
            gs.ctm.e = rawH; gs.ctm.f = 0;
        }

        PdfContentParser parser(content, &collector, &doc,
            pageIndex, &fonts, gs, resStack);
        parser.parse();

        collector.sortGlyphs();
        collector.exportGlyphs(_cache[pageIndex]);

        return (int)_cache[pageIndex].size();
    }

    const std::vector<PdfTextGlyphExport>& PdfTextExtractor::getPageGlyphs(int pageIndex) const
    {
        auto it = _cache.find(pageIndex);
        return (it != _cache.end()) ? it->second : s_empty;
    }

    int PdfTextExtractor::getGlyphCount(int pageIndex) const
    {
        auto it = _cache.find(pageIndex);
        return (it != _cache.end()) ? (int)it->second.size() : 0;
    }

    bool PdfTextExtractor::hasPage(int pageIndex) const
    {
        return _cache.find(pageIndex) != _cache.end();
    }

    void PdfTextExtractor::clearPage(int pageIndex) { _cache.erase(pageIndex); }
    void PdfTextExtractor::clearAll() { _cache.clear(); }

} // namespace pdf