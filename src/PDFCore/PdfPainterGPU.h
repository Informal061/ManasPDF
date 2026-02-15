#pragma once
#include "IPdfPainter.h"
#include "PdfGradient.h"

#include <d2d1.h>
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wincodec.h>
#include <vector>
#include <map>
#include <stack>
#include <mutex>
#include <cfloat>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace pdf
{
    struct PdfPattern;

    // =====================================================
    // PdfPainterGPU - Full Direct2D GPU Rendering
    // OPTIMIZED: Text batching for Microsoft Print to PDF
    // =====================================================
    class PdfPainterGPU : public IPdfPainter
    {
    public:
        PdfPainterGPU(int width, int height, double scaleX, double scaleY);
        ~PdfPainterGPU() override;

        bool initialize();
        void beginDraw();
        void endDraw();
        bool hasEndDrawError() const;

        // Static factory management - call once at startup/shutdown
        static bool initFactories();
        static void cleanupFactories();

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

        std::vector<uint8_t> getBuffer() override;

        bool isGPU() const override { return true; }

        // ==================== PERFORMANCE OPTIMIZATION ====================
        void beginPage() override;
        void endPage() override;

        // ==================== TEXT BLOCK BATCHING (MuPDF-style) ====================
        void beginTextBlock() override;
        void endTextBlock() override;

        // ==================== CLIPPING LAYER (for Form XObjects) ====================
        void pushClipPath(const std::vector<PdfPathSegment>& clipPath, const PdfMatrix& clipCTM, bool evenOdd = false) override;
        void popClipPath() override;

        // ==================== SOFT MASK (SMask) ====================
        void pushSoftMask(const std::vector<uint8_t>& maskAlpha, int maskW, int maskH) override;
        void popSoftMask() override;

    private:
        int _w, _h;
        double _scaleX, _scaleY;
        bool _initialized = false;
        bool _inDraw = false;
        bool _inPageRender = false;  // Page rendering mode (batch mode)
        bool _endDrawFailed = false; // Track EndDraw failures

        // ==================== PATH BATCHING ====================
        struct BatchedFill {
            std::vector<PdfPathSegment> path;
            PdfMatrix ctm;
            bool evenOdd;
        };

        uint32_t _batchColor = 0;
        bool _batchEvenOdd = false;  // ✅ Track fill mode for batching
        std::vector<BatchedFill> _fillBatch;
        bool _hasBatchedFills = false;

        void flushFillBatch();
        void addToBatch(const std::vector<PdfPathSegment>& path,
            uint32_t color, const PdfMatrix& ctm, bool evenOdd);

        // ==================== TEXT BATCHING (OPTIMIZATION) ====================
        // Glyphs are collected into a single atlas bitmap and rendered in one call
        struct GlyphDrawCmd {
            float destX, destY;         // Destination position in device space (sub-pixel)
            int srcX, srcY;             // Position in atlas
            int width, height, pitch;   // Glyph dimensions (original bitmap size)
            int scaledWidth, scaledHeight; // Scaled dimensions for rendering
            double scaleX;              // Horizontal scale factor
            double scaleY;              // Vertical scale factor
            uint32_t color;             // Glyph color (for multi-color text blocks)
            std::vector<uint8_t> bitmap; // Copy of glyph bitmap (needed for atlas composition)
        };

        std::vector<GlyphDrawCmd> _glyphBatch;
        uint32_t _glyphBatchColor = 0;
        bool _hasGlyphBatch = false;
        float _glyphBatchMinX = FLT_MAX, _glyphBatchMinY = FLT_MAX;
        float _glyphBatchMaxX = -FLT_MAX, _glyphBatchMaxY = -FLT_MAX;

        // Text block mode (MuPDF-style): batch ALL glyphs between BT...ET
        bool _inTextBlock = false;

        // Flush threshold - flush when batch gets too large
        static constexpr size_t GLYPH_BATCH_MAX_COUNT = 1000;
        static constexpr int GLYPH_BATCH_MAX_AREA = 4096 * 1024; // Max atlas size

        void flushGlyphBatch();
        void addGlyphToBatch(const uint8_t* bitmap, int width, int height, int pitch,
            float destX, float destY, uint32_t color, double scaleX = 1.0, double scaleY = 1.0);

        // ==================== BRUSH CACHING ====================
        std::map<uint32_t, ID2D1SolidColorBrush*> _brushCache;
        ID2D1SolidColorBrush* getCachedBrush(uint32_t color);
        void clearBrushCache();

        // ==================== CLIP OPTIMIZATION TRACKING ====================
        // These are used for cleanup in destructor only
        bool _hasActiveClip = false;
        bool _activeClipIsRect = false;
        D2D1_RECT_F _activeClipRect = {};
        size_t _activeClipHash = 0;
        ID2D1Layer* _activeClipLayer = nullptr;
        ID2D1PathGeometry* _activeClipGeometry = nullptr;

        // ==================== CLIP LAYER STACK (for Form XObjects) ====================
        struct ClipLayerInfo {
            ID2D1Layer* layer = nullptr;
            ID2D1PathGeometry* geometry = nullptr;
        };
        std::stack<ClipLayerInfo> _clipLayerStack;

        // ==================== SOFT MASK LAYER STACK ====================
        struct SoftMaskLayerInfo {
            ID2D1Layer* layer = nullptr;
            ID2D1Bitmap* maskBitmap = nullptr;
            ID2D1BitmapBrush* maskBrush = nullptr;
        };
        std::stack<SoftMaskLayerInfo> _softMaskLayerStack;

        // Direct2D objects - factories are STATIC (shared across all instances)
        static ID2D1Factory1* s_d2dFactory;
        static IWICImagingFactory* s_wicFactory;
        static IDWriteFactory* s_dwFactory;
        static bool s_factoriesInitialized;
        static std::mutex s_factoryMutex;

        // Per-instance render target and bitmap
        ID2D1RenderTarget* _renderTarget = nullptr;
        ID2D1DeviceContext* _deviceContext = nullptr; // D2D1.1 for high-quality interpolation
        IWICBitmap* _wicBitmap = nullptr;

        // Rotation
        bool _hasRotate = false;
        D2D1_MATRIX_3X2_F _rotMatrix;

        // ==================== Helper Functions ====================
        D2D1_POINT_2F transformPoint(double x, double y, const PdfMatrix& ctm) const;
        D2D1_COLOR_F toD2DColor(uint32_t argb) const;

        // Path geometry with fill mode support
        // implicitClose: true for fills/clips (PDF spec: open subpaths implicitly closed)
        //                false for strokes (subpaths stay open unless explicit h operator)
        ID2D1PathGeometry* createPathGeometry(
            const std::vector<PdfPathSegment>& path,
            const PdfMatrix& ctm,
            bool evenOdd = false,
            bool implicitClose = true);

        // Gradient brush creation
        ID2D1Brush* createGradientBrush(
            const PdfGradient& gradient,
            const PdfMatrix& ctm,
            const PdfMatrix& gradientCTM);

        // Pattern brush creation  
        ID2D1Brush* createPatternBrush(const PdfPattern& pattern, const PdfMatrix& ctm);

        // Image helpers
        ID2D1Bitmap* createBitmapFromARGB(const std::vector<uint8_t>& argb, int w, int h);
        ID2D1Bitmap* createScaledBitmapFromARGB(const std::vector<uint8_t>& argb, int srcW, int srcH, int dstW, int dstH);
        void drawBitmapHighQuality(ID2D1Bitmap* bitmap, const D2D1_RECT_F& destRect, float opacity = 1.0f);

        // Legacy single glyph draw (used when not in batch mode)
        void drawGlyphBitmapColored(
            const uint8_t* bitmap, int width, int height, int pitch,
            float destX, float destY,
            uint8_t r, uint8_t g, uint8_t b,
            double scaleX = 1.0, double scaleY = 1.0);

        // ==================== TYPE3 FONT RENDERING ====================
        double drawTextType3(
            double x, double y,
            const std::string& raw,
            double fontSizePt,
            double advanceSizePt,
            uint32_t color,
            const PdfFontInfo* font,
            double charSpacing,
            double wordSpacing,
            double horizScale,
            double textAngle);

        // Type3 glyph cache: key = fontHash ^ glyphName hash
        struct Type3CachedGlyph {
            std::vector<uint8_t> alpha;  // grayscale alpha bitmap
            int width = 0;
            int height = 0;
            int bearingX = 0;  // left offset from glyph origin (glyph units)
            int bearingY = 0;  // top offset from baseline (glyph units)
            double bboxH = 0;  // bbox height in glyph units (for scale calculation)
        };
        std::map<size_t, Type3CachedGlyph> _type3Cache;
    };

} // namespace pdf