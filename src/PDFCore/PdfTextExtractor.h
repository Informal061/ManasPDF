#pragma once
// =====================================================
// PdfTextExtractor.h - Text Extraction & Selection Support
// =====================================================
// Koordinat sistemi: "bitmap-at-zoom1" pixel cinsinden
//   pixel = page_point * (96.0 / 72.0)
// C# tarafında: mouse_pos / zoom ile eşleşir.
// =====================================================

#include <cstdint>
#include <vector>
#include <string>
#include <map>
#include <cmath>

#include "IPdfPainter.h"
#include "PdfDocument.h"

namespace pdf
{
    static constexpr double TEXT_DPI = 96.0;
    static constexpr double TEXT_PT_TO_PX = TEXT_DPI / 72.0;  // 1.33333

    // =====================================================
    // Interop Struct - C# (24 byte, pack=1)
    // =====================================================
#pragma pack(push, 1)
    struct PdfTextGlyphExport
    {
        uint32_t unicode;
        float x;          // bitmap pixel (zoom=1)
        float y;          // bitmap pixel (zoom=1), top-left origin
        float width;      // bitmap pixel (zoom=1)
        float height;     // bitmap pixel (zoom=1)
        float fontSize;   // bitmap pixel (zoom=1)
    };
#pragma pack(pop)

    struct PdfTextGlyphInfo
    {
        uint32_t unicode = 0;
        double bitmapX = 0;
        double bitmapY = 0;
        double width = 0;
        double height = 0;
        double fontSize = 0;
        bool isSpace = false;
        bool isNewLine = false;
    };

    // =====================================================
    // PdfTextCollectorPainter - IPdfPainter impl
    // =====================================================
    class PdfTextCollectorPainter : public IPdfPainter
    {
    public:
        PdfTextCollectorPainter(double pageWPt, double pageHPt);

        const std::vector<PdfTextGlyphInfo>& getGlyphs() const { return _glyphs; }
        int getGlyphCount() const { return (int)_glyphs.size(); }
        void sortGlyphs();
        void exportGlyphs(std::vector<PdfTextGlyphExport>& out) const;

        // IPdfPainter
        int width() const override { return _pixW; }
        int height() const override { return _pixH; }
        double scaleX() const override { return TEXT_PT_TO_PX; }
        double scaleY() const override { return TEXT_PT_TO_PX; }

        void clear(uint32_t) override {}
        void fillPath(const std::vector<PdfPathSegment>&, uint32_t, const PdfMatrix&,
            bool, const std::vector<PdfPathSegment>*, const PdfMatrix*, bool) override {
        }
        void strokePath(const std::vector<PdfPathSegment>&, uint32_t, double,
            const PdfMatrix&, int, int, double) override {
        }
        void fillPathWithGradient(const std::vector<PdfPathSegment>&, const PdfGradient&,
            const PdfMatrix&, const PdfMatrix&, bool) override {
        }
        void fillPathWithPattern(const std::vector<PdfPathSegment>&, const PdfPattern&,
            const PdfMatrix&, bool) override {
        }

        double drawTextFreeTypeRaw(
            double x, double y,
            const std::string& raw,
            double fontSizePt,
            double advanceSizePt,
            uint32_t color,
            const PdfFontInfo* font,
            double charSpacing,
            double wordSpacing,
            double horizScale,
            double textAngle = 0.0) override;

        void drawImage(const std::vector<uint8_t>&, int, int, const PdfMatrix&) override {}
        void drawImageWithClipRect(const std::vector<uint8_t>&, int, int,
            const PdfMatrix&, int, int, int, int) override {
        }
        void drawImageClipped(const std::vector<uint8_t>&, int, int,
            const PdfMatrix&, const std::vector<PdfPathSegment>&,
            const PdfMatrix&, bool, double, double, double, double) override {
        }

        void setPageRotation(int degrees, double pageWPt, double pageHPt) override;
        std::vector<uint8_t> getBuffer() override { return {}; }
        bool isGPU() const override { return false; }
        void beginPage() override {}
        void endPage() override {}
        void beginTextBlock() override {}
        void endTextBlock() override {}
        void pushClipPath(const std::vector<PdfPathSegment>&, const PdfMatrix&, bool evenOdd = false) override {}
        void popClipPath() override {}

    private:
        double _pageWPt, _pageHPt;
        int _pixW, _pixH;     // bitmap pixel at zoom=1

        bool _hasRotate = false;
        int _rotation = 0;    // 0, 90, 180, 270

        std::vector<PdfTextGlyphInfo> _glyphs;

        // page point -> bitmap pixel (zoom=1)
        void toBitmapPx(double pageX, double pageY,
            double& outPx, double& outPy) const;
    };

    // =====================================================
    // PdfTextExtractor
    // =====================================================
    class PdfTextExtractor
    {
    public:
        int extractPage(PdfDocument& doc, int pageIndex);
        const std::vector<PdfTextGlyphExport>& getPageGlyphs(int pageIndex) const;
        int getGlyphCount(int pageIndex) const;
        bool hasPage(int pageIndex) const;
        void clearPage(int pageIndex);
        void clearAll();
    private:
        std::map<int, std::vector<PdfTextGlyphExport>> _cache;
    };

} // namespace pdf
