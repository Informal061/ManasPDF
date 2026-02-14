#pragma once
// =====================================================
// IPdfPainter.h - GPU/CPU Rendering Interface
// =====================================================

#include <cstdint>
#include <vector>
#include <string>
#include "PdfPath.h"
#include "PdfGraphicsState.h"

namespace pdf
{
    struct PdfFontInfo;
    class PdfGradient;
    struct PdfPattern;

    class IPdfPainter
    {
    public:
        virtual ~IPdfPainter() = default;

        // Dimensions
        virtual int width() const = 0;
        virtual int height() const = 0;
        virtual double scaleX() const = 0;
        virtual double scaleY() const = 0;

        // Basic
        virtual void clear(uint32_t bgraColor) = 0;

        // Path Fill
        virtual void fillPath(
            const std::vector<PdfPathSegment>& path,
            uint32_t color,
            const PdfMatrix& ctm,
            bool evenOdd = false,
            const std::vector<PdfPathSegment>* clipPath = nullptr,
            const PdfMatrix* clipCTM = nullptr,
            bool clipEvenOdd = false) = 0;

        // Path Stroke
        virtual void strokePath(
            const std::vector<PdfPathSegment>& path,
            uint32_t color,
            double lineWidth,
            const PdfMatrix& ctm,
            int lineCap = 0,
            int lineJoin = 0,
            double miterLimit = 10.0) = 0;

        // Gradient Fill
        virtual void fillPathWithGradient(
            const std::vector<PdfPathSegment>& path,
            const PdfGradient& gradient,
            const PdfMatrix& ctm,
            const PdfMatrix& gradientCTM,
            bool evenOdd = false) = 0;

        // Pattern Fill
        virtual void fillPathWithPattern(
            const std::vector<PdfPathSegment>& path,
            const PdfPattern& pattern,
            const PdfMatrix& ctm,
            bool evenOdd = false) = 0;

        // Text Rendering
        virtual double drawTextFreeTypeRaw(
            double x, double y,
            const std::string& raw,
            double fontSizePt,
            double advanceSizePt,
            uint32_t color,
            const PdfFontInfo* font,
            double charSpacing,
            double wordSpacing,
            double horizScale,
            double textAngle = 0.0) = 0;  // Rotation angle in radians (page space)

        // Image Rendering
        virtual void drawImage(
            const std::vector<uint8_t>& argb,
            int imgW, int imgH,
            const PdfMatrix& ctm) = 0;

        virtual void drawImageWithClipRect(
            const std::vector<uint8_t>& argb,
            int imgW, int imgH,
            const PdfMatrix& ctm,
            int clipMinX, int clipMinY,
            int clipMaxX, int clipMaxY) = 0;

        virtual void drawImageClipped(
            const std::vector<uint8_t>& argb,
            int imgW, int imgH,
            const PdfMatrix& ctm,
            const std::vector<PdfPathSegment>& clipPath,
            const PdfMatrix& clipCTM,
            bool hasRectClip = false,
            double rectMinX = 0, double rectMinY = 0,
            double rectMaxX = 0, double rectMaxY = 0) = 0;

        // State
        virtual void setPageRotation(int degrees, double pageWPt, double pageHPt) = 0;

        // Output
        virtual std::vector<uint8_t> getBuffer() = 0;

        // Type Check
        virtual bool isGPU() const { return false; }

        // ==================== PERFORMANCE OPTIMIZATION ====================
        // Page rendering lifecycle - override for batching
        virtual void beginPage() {}  // Called before rendering starts
        virtual void endPage() {}    // Called after rendering completes

        // ==================== TEXT BLOCK BATCHING (MuPDF-style) ====================
        // Text blocks: BT...ET in PDF create a text block
        // All text within a block is batched and rendered in one call at ET
        virtual void beginTextBlock() {}  // Called at BT
        virtual void endTextBlock() {}    // Called at ET - flushes all batched text

        // ==================== CLIPPING LAYER (for Form XObjects) ====================
        // Push a clipping path that applies to all subsequent drawing
        // Used when entering Form XObjects that should be clipped by parent's path
        virtual void pushClipPath(const std::vector<PdfPathSegment>& clipPath, const PdfMatrix& clipCTM, bool evenOdd = false) {}
        virtual void popClipPath() {}
    };

} // namespace pdf