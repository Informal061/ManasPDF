#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <cmath>
#include <functional>
#include "PdfPath.h"
#include "PdfGraphicsState.h"
#include "PdfGradient.h"
#include "PdfDocument.h"
#include "IPdfPainter.h"

namespace pdf
{
    struct DPoint { double x, y; };
    struct IPoint { int x, y; };

    // Tiling Pattern Structure
    struct PdfPattern {
        std::vector<uint32_t> buffer;
        int width = 0;
        int height = 0;
        PdfMatrix matrix;       // Pattern's own /Matrix (pattern space → parent default CS)
        PdfMatrix defaultCtm;   // Parent content stream's initial CTM (default CS → device)
        int type = 1;
        int tilingType = 1;
        double xStep = 0;
        double yStep = 0;
        bool isUncolored = false;
        uint32_t baseColor = 0xFF000000;
    };

    // =====================================================
    // PdfPainter - CPU Software Rendering
    // Implements IPdfPainter interface
    // =====================================================
    class PdfPainter : public IPdfPainter
    {
    public:
        PdfPainter(int width, int height, double scaleX = 1.0, double scaleY = 1.0, int ssaa = 1);
        ~PdfPainter() override = default;

        // ==================== IPdfPainter Implementation ====================
        int width() const override { return _w; }
        int height() const override { return _h; }
        double scaleX() const override { return _scaleX; }
        double scaleY() const override { return _scaleY; }

        void clear(uint32_t bgraColor) override;

        void fillPath(
            const std::vector<PdfPathSegment>& path,
            uint32_t color,
            const PdfMatrix& ctm,
            bool evenOdd = false,
            const std::vector<PdfPathSegment>* clipPath = nullptr,
            const PdfMatrix* clipCTM = nullptr,
            bool clipEvenOdd = false) override;

        void strokePath(
            const std::vector<PdfPathSegment>& path,
            uint32_t color,
            double lineWidth,
            const PdfMatrix& ctm,
            int lineCap = 0,
            int lineJoin = 0,
            double miterLimit = 10.0) override;

        void fillPathWithGradient(
            const std::vector<PdfPathSegment>& path,
            const PdfGradient& gradient,
            const PdfMatrix& ctm,
            const PdfMatrix& gradientCTM,
            bool evenOdd = false) override;

        void fillPathWithPattern(
            const std::vector<PdfPathSegment>& path,
            const PdfPattern& pattern,
            const PdfMatrix& ctm,
            bool evenOdd = false) override;

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

        void drawImage(
            const std::vector<uint8_t>& argb,
            int imgW, int imgH,
            const PdfMatrix& ctm) override;

        void drawImageWithClipRect(
            const std::vector<uint8_t>& argb,
            int imgW, int imgH,
            const PdfMatrix& ctm,
            int clipMinX, int clipMinY,
            int clipMaxX, int clipMaxY) override;

        void drawImageClipped(
            const std::vector<uint8_t>& argb,
            int imgW, int imgH,
            const PdfMatrix& ctm,
            const std::vector<PdfPathSegment>& clipPath,
            const PdfMatrix& clipCTM,
            bool hasRectClip = false,
            double rectMinX = 0, double rectMinY = 0,
            double rectMaxX = 0, double rectMaxY = 0) override;

        void setPageRotation(int degrees, double pageWPt, double pageHPt) override;

        std::vector<uint8_t> getBuffer() override { return getDownsampledBuffer(); }

        bool isGPU() const override { return false; }

        // ==================== CPU-specific methods ====================
        std::vector<uint8_t> getDownsampledBuffer() const;
        bool getDownsampledBufferDirect(uint8_t* outBuffer, int outBufferSize) const;
        const std::vector<uint8_t>& getRawBuffer() const { return _buffer; }

        // Single CTM gradient overload (backwards compatibility)
        void fillPathWithGradient(
            const std::vector<PdfPathSegment>& path,
            const PdfGradient& gradient,
            const PdfMatrix& ctm,
            bool evenOdd);

        // Basic drawing
        void drawLine(double x1, double y1, double x2, double y2, uint32_t bgra);
        void fillRect(double x, double y, double w, double h, uint32_t color);
        void drawText(double x, double y, const std::wstring& text, double fontSizePt, uint32_t color);
        void drawGlyph(double x, double y, double w, double h, uint32_t bgra);

    private:
        int _ssaa;
        int _finalW, _finalH;
        int _w, _h;
        double _scaleX, _scaleY;
        std::vector<uint8_t> _buffer;

        bool _hasRotate = false;
        double _rotA = 1, _rotB = 0, _rotC = 0, _rotD = 1;
        double _rotTx = 0, _rotTy = 0;

        double mapY(double y) const;
        void applyRotate(double& x, double& y) const;
        void putPixel(int x, int y, uint32_t bgra);

        void rasterFillPolygon(const std::vector<IPoint>& poly, uint32_t color, bool evenOdd);
        void rasterFillPolygonPattern(const std::vector<IPoint>& poly, const PdfPattern& pattern, bool evenOdd);

        void strokeSubpath(
            const std::vector<DPoint>& pts,
            bool closed,
            uint32_t color,
            double lineWidthPx,
            int lineJoin,
            int lineCap,
            double miterLimit);

        void drawLineDevice(int x1, int y1, int x2, int y2, uint32_t color);
        void blendGray8ToBuffer(int dstX, int dstY, int w, int h, const uint8_t* src, int srcPitch, uint32_t color);

        // ==================== SMask Support ====================
    public:
        void pushSoftMask(const std::vector<uint8_t>& maskAlpha, int maskW, int maskH) override;
        void popSoftMask() override;

    private:
        std::vector<uint8_t> _smaskSavedBuffer;
        std::vector<uint8_t> _smaskAlpha;
        int _smaskW = 0;
        int _smaskH = 0;
        bool _hasSmask = false;
    };

} // namespace pdf
