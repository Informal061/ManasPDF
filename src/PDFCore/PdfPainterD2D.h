#pragma once

#include <d2d1.h>
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wincodec.h>
#include <vector>
#include <map>
#include <stack>
#include <cstdint>
#include <string>
#include <memory>

#include "PdfPath.h"
#include "PdfGraphicsState.h"
#include "PdfGradient.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace pdf
{
    struct PdfFontInfo;  // Forward declaration

    // ============================================
    // GPU-ACCELERATED PDF PAINTER using Direct2D
    // ============================================
    class PdfPainterD2D
    {
    public:
        PdfPainterD2D(int width, int height, double scaleX, double scaleY);
        ~PdfPainterD2D();

        bool initialize();
        void beginDraw();
        void endDraw();
        void clear(uint32_t bgraColor);

        // ==================== PATH OPERATIONS ====================
        void fillPath(const std::vector<PdfPathSegment>& path, uint32_t color,
            const PdfMatrix& ctm, bool evenOdd = false);
        void strokePath(const std::vector<PdfPathSegment>& path, uint32_t color,
            double lineWidth, const PdfMatrix& ctm,
            int lineCap, int lineJoin, double miterLimit,
            const std::vector<double>& dashArray = {}, double dashPhase = 0);

        // ==================== TEXT RENDERING ====================
        double drawText(const std::string& text, double x, double y,
            const PdfFontInfo* font, double fontSize,
            uint32_t color, const PdfMatrix& ctm,
            double charSpacing = 0, double wordSpacing = 0,
            double horizScale = 100);

        double drawTextRaw(const std::vector<uint8_t>& raw, double x, double y,
            const PdfFontInfo* font, double fontSize,
            uint32_t color, const PdfMatrix& ctm,
            double charSpacing = 0, double wordSpacing = 0,
            double horizScale = 100);

        // ==================== IMAGE RENDERING ====================
        void drawImage(const std::vector<uint8_t>& argb, int imgW, int imgH,
            const PdfMatrix& ctm);
        void drawImageWithMask(const std::vector<uint8_t>& argb, int imgW, int imgH,
            const std::vector<uint8_t>& mask, int maskW, int maskH,
            const PdfMatrix& ctm);

        // ==================== GRADIENT RENDERING ====================
        void fillPathWithGradient(const std::vector<PdfPathSegment>& path,
            const PdfGradient& gradient,
            const PdfMatrix& ctm, bool evenOdd = false);

        // ==================== CLIPPING ====================
        void pushClip(const std::vector<PdfPathSegment>& path,
            const PdfMatrix& ctm, bool evenOdd = false);
        void popClip();

        // ==================== STATE MANAGEMENT ====================
        void saveState();
        void restoreState();
        void setPageRotation(int degrees, double pageWPt, double pageHPt);

        // ==================== OUTPUT ====================
        std::vector<uint8_t> getBuffer();
        bool uploadFromCPUBuffer(const std::vector<uint8_t>& cpuBuffer);

        // ==================== GETTERS ====================
        int width() const { return _w; }
        int height() const { return _h; }
        double scaleX() const { return _scaleX; }
        double scaleY() const { return _scaleY; }
        bool isInitialized() const { return _initialized; }

    private:
        int _w, _h;
        double _scaleX, _scaleY;
        bool _initialized = false;
        bool _inDraw = false;

        // Direct2D objects
        ID2D1Factory1* _d2dFactory = nullptr;
        ID2D1RenderTarget* _renderTarget = nullptr;
        IWICImagingFactory* _wicFactory = nullptr;
        IDWriteFactory* _dwFactory = nullptr;
        IWICBitmap* _wicBitmap = nullptr;

        // Font cache for DirectWrite
        std::map<std::string, IDWriteFontFace*> _fontFaceCache;
        IDWriteFontFace* getOrCreateFontFace(const PdfFontInfo* font);

        // Clip stack
        std::stack<ID2D1Layer*> _clipLayers;

        // Rotation
        bool _hasRotate = false;
        D2D1_MATRIX_3X2_F _rotMatrix;

        // Helper functions
        D2D1_MATRIX_3X2_F toD2DMatrix(const PdfMatrix& m) const;
        D2D1_COLOR_F toD2DColor(uint32_t argb) const;
        ID2D1PathGeometry* createPathGeometry(const std::vector<PdfPathSegment>& path,
            const PdfMatrix& ctm);
        D2D1_POINT_2F transformPoint(double x, double y, const PdfMatrix& ctm) const;

        // Brush creation
        ID2D1Brush* createGradientBrush(const PdfGradient& gradient, const PdfMatrix& ctm);
    };

} // namespace pdf
