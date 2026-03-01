#include "pch.h"
#include "PdfPainterGPU.h"
#include "PdfDocument.h"
#include "PdfPainter.h"  // PdfPattern
#include "PdfContentParser.h"  // Type3 CharProc rendering
#include "GlyphCache.h"  // CPU GlyphCache - shared with GPU
#include "PdfDebug.h"    // LogDebug
#include <algorithm>
#include <cmath>

#include <ft2build.h>
#include FT_FREETYPE_H

extern FT_Library g_ftLib;

// WinAnsi encoding table
static const uint16_t WinAnsiGPU[256] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,
    48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,
    64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,
    80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,
    96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,
    112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,
    0x20AC,0,0x201A,0x0192,0x201E,0x2026,0x2020,0x2021,
    0x02C6,0x2030,0x0160,0x2039,0x0152,0,0x017D,0,
    0,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,
    0x02DC,0x2122,0x0161,0x203A,0x0153,0,0x017E,0,
    160,0x00A1,0x00A2,0x00A3,0x00A4,0x00A5,0x00A6,0x00A7,
    0x00A8,0x00A9,0x00AA,0x00AB,0x00AC,0x00AD,0x00AE,0x00AF,
    0x00B0,0x00B1,0x00B2,0x00B3,0x00B4,0x00B5,0x00B6,0x00B7,
    0x00B8,0x00B9,0x00BA,0x00BB,0x00BC,0x00BD,0x00BE,0x00BF,
    0x00C0,0x00C1,0x00C2,0x00C3,0x00C4,0x00C5,0x00C6,0x00C7,
    0x00C8,0x00C9,0x00CA,0x00CB,0x00CC,0x00CD,0x00CE,0x00CF,
    0x00D0,0x00D1,0x00D2,0x00D3,0x00D4,0x00D5,0x00D6,0x00D7,
    0x00D8,0x00D9,0x00DA,0x00DB,0x00DC,0x00DD,0x00DE,0x00DF,
    0x00E0,0x00E1,0x00E2,0x00E3,0x00E4,0x00E5,0x00E6,0x00E7,
    0x00E8,0x00E9,0x00EA,0x00EB,0x00EC,0x00ED,0x00EE,0x00EF,
    0x00F0,0x00F1,0x00F2,0x00F3,0x00F4,0x00F5,0x00F6,0x00F7,
    0x00F8,0x00F9,0x00FA,0x00FB,0x00FC,0x00FD,0x00FE,0x00FF
};

static inline uint32_t FixTurkishGPU(uint32_t uni)
{
    switch (uni)
    {
    case 0xDD: return 0x0130; // Ý -> İ
    case 0xDE: return 0x015E; // Þ -> Ş
    case 0xF0: return 0x011F; // ð -> ğ
    case 0xFD: return 0x0131; // ý -> ı
    case 0xFE: return 0x015F; // þ -> ş
    case 0xD0: return 0x011E; // Ð -> Ğ
    default:   return uni;
    }
}

static inline bool isCidFontActiveGPU(const pdf::PdfFontInfo* f) {
    if (!f) return false;
    if (f->isCidFont) return true;
    if (f->encoding == "/Identity-H" || f->encoding == "/Identity-V") return true;
    return false;
}

static inline int getWidth1000ForCodeGPU(const pdf::PdfFontInfo* f, int code) {
    if (!f) return 0;
    if (f->isCidFont || f->encoding == "/Identity-H" || f->encoding == "/Identity-V") {
        auto it = f->cidWidths.find((uint16_t)code);
        if (it != f->cidWidths.end()) return it->second;
        // CID not in width table - use cidDefaultWidth (PDF spec)
        if (f->cidDefaultWidth == 1000) return 0;  // Signal: use FreeType
        return f->cidDefaultWidth;
    }
    int w = f->missingWidth;
    if (w <= 0) w = 500;
    if (f->hasWidths && code >= f->firstChar && code < f->firstChar + (int)f->widths.size()) {
        int idx = code - f->firstChar;
        int ww = f->widths[idx];
        if (ww > 0) w = ww;
    }
    return w;
}

namespace pdf
{
    // ============================================
    // STATIC FACTORY MEMBERS
    // ============================================
    ID2D1Factory1* PdfPainterGPU::s_d2dFactory = nullptr;
    IWICImagingFactory* PdfPainterGPU::s_wicFactory = nullptr;
    IDWriteFactory* PdfPainterGPU::s_dwFactory = nullptr;
    bool PdfPainterGPU::s_factoriesInitialized = false;
    std::mutex PdfPainterGPU::s_factoryMutex;

    bool PdfPainterGPU::initFactories()
    {
        std::lock_guard<std::mutex> lock(s_factoryMutex);
        if (s_factoriesInitialized) return true;

        HRESULT hr;

        hr = D2D1CreateFactory(
            D2D1_FACTORY_TYPE_MULTI_THREADED,
            __uuidof(ID2D1Factory1),
            (void**)&s_d2dFactory
        );
        if (FAILED(hr)) return false;

        hr = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&s_wicFactory)
        );
        if (FAILED(hr)) return false;

        hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(&s_dwFactory)
        );
        if (FAILED(hr)) return false;

        s_factoriesInitialized = true;
        return true;
    }

    void PdfPainterGPU::cleanupFactories()
    {
        std::lock_guard<std::mutex> lock(s_factoryMutex);
        if (s_dwFactory) { s_dwFactory->Release(); s_dwFactory = nullptr; }
        if (s_wicFactory) { s_wicFactory->Release(); s_wicFactory = nullptr; }
        if (s_d2dFactory) { s_d2dFactory->Release(); s_d2dFactory = nullptr; }
        s_factoriesInitialized = false;
    }

    // ============================================
    // CONSTRUCTOR / DESTRUCTOR
    // ============================================

    PdfPainterGPU::PdfPainterGPU(int width, int height, double scaleX, double scaleY)
        : _w(width), _h(height), _scaleX(scaleX), _scaleY(scaleY)
    {
        _rotMatrix = D2D1::IdentityMatrix();
        _inTextBlock = false;

        // Initialize clip tracking
        _hasActiveClip = false;
        _activeClipIsRect = false;
        _activeClipHash = 0;
        _activeClipLayer = nullptr;
        _activeClipGeometry = nullptr;

        // Reserve space for glyph batch to avoid repeated allocations
        _glyphBatch.reserve(512);
    }

    PdfPainterGPU::~PdfPainterGPU()
    {
        // Flush any pending batches first
        if (_renderTarget && _inDraw)
        {
            if (_hasGlyphBatch) {
                try { flushGlyphBatch(); } catch (...) {}
            }
            if (_hasBatchedFills) {
                try { flushFillBatch(); } catch (...) {}
            }
        }

        // Pop ALL remaining soft mask layers from D2D render target
        while (!_softMaskLayerStack.empty() && _renderTarget)
        {
            _renderTarget->PopLayer();
            SoftMaskLayerInfo smInfo = _softMaskLayerStack.top();
            _softMaskLayerStack.pop();
            if (smInfo.layer) smInfo.layer->Release();
            if (smInfo.maskBrush) smInfo.maskBrush->Release();
            if (smInfo.maskBitmap) smInfo.maskBitmap->Release();
        }

        // Pop ALL remaining clip layers from D2D render target
        // This is critical - D2D crashes if EndDraw is called with layers still pushed
        while (!_clipLayerStack.empty() && _renderTarget)
        {
            _renderTarget->PopLayer();
            ClipLayerInfo info = _clipLayerStack.top();
            _clipLayerStack.pop();
            if (info.layer) info.layer->Release();
            if (info.geometry) info.geometry->Release();
        }

        // End any active draw session
        if (_inDraw && _renderTarget)
        {
            HRESULT hr = _renderTarget->EndDraw();
            (void)hr; // EndDraw failure in destructor is non-fatal
            _inDraw = false;
        }

        // Clear all batches
        _glyphBatch.clear();
        _glyphBatch.shrink_to_fit();
        _fillBatch.clear();
        _fillBatch.shrink_to_fit();
        _hasGlyphBatch = false;
        _hasBatchedFills = false;

        // Clear active clip resources (legacy per-operation clip)
        if (_activeClipLayer) { _activeClipLayer->Release(); _activeClipLayer = nullptr; }
        if (_activeClipGeometry) { _activeClipGeometry->Release(); _activeClipGeometry = nullptr; }

        // Clear brush cache before releasing render target
        clearBrushCache();

        if (_deviceContext) { _deviceContext->Release(); _deviceContext = nullptr; }
        if (_renderTarget) { _renderTarget->Release(); _renderTarget = nullptr; }
        if (_wicBitmap) { _wicBitmap->Release(); _wicBitmap = nullptr; }
        // NOTE: Factories are STATIC - do NOT release them here
        // They are shared across all PdfPainterGPU instances
        // Call cleanupFactories() at program exit
    }

    // ============================================
    // INITIALIZATION
    // ============================================

    bool PdfPainterGPU::initialize()
    {
        if (_initialized) return true;

        // Ensure static factories are ready (thread-safe, one-time init)
        if (!initFactories()) return false;

        HRESULT hr;

        LogDebug("GPU init: creating WIC bitmap %d x %d (%.1f MB)", _w, _h, (double)_w * _h * 4 / (1024.0 * 1024.0));

        // Create WIC Bitmap (per-instance, page-size dependent)
        // WICBitmapCacheOnLoad = contiguous layout (stride == w*4, faster getBuffer)
        hr = s_wicFactory->CreateBitmap(
            _w, _h,
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapCacheOnLoad,
            &_wicBitmap
        );
        if (FAILED(hr))
        {
            LogDebug("ERROR: WIC CreateBitmap failed hr=0x%08X for %d x %d", (unsigned)hr, _w, _h);
            return false;
        }

        // Create D2D Render Target from WIC Bitmap (per-instance)
        D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            0, 0,
            D2D1_RENDER_TARGET_USAGE_NONE,
            D2D1_FEATURE_LEVEL_DEFAULT
        );

        hr = s_d2dFactory->CreateWicBitmapRenderTarget(_wicBitmap, rtProps, &_renderTarget);
        if (FAILED(hr))
        {
            LogDebug("ERROR: D2D CreateWicBitmapRenderTarget failed hr=0x%08X for %d x %d", (unsigned)hr, _w, _h);
            return false;
        }

        // High quality rendering
        _renderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        _renderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

        // Try to get ID2D1DeviceContext for high-quality image interpolation
        hr = _renderTarget->QueryInterface(__uuidof(ID2D1DeviceContext), (void**)&_deviceContext);
        if (SUCCEEDED(hr) && _deviceContext)
        {
            LogDebug("GPU init: ID2D1DeviceContext available (high-quality interpolation enabled)");
        }
        else
        {
            _deviceContext = nullptr;
            LogDebug("GPU init: ID2D1DeviceContext not available (using bilinear interpolation)");
        }

        _initialized = true;
        return true;
    }

    void PdfPainterGPU::beginDraw()
    {
        if (_renderTarget && !_inDraw)
        {
            _renderTarget->BeginDraw();
            _inDraw = true;
        }
    }

    void PdfPainterGPU::endDraw()
    {
        if (_renderTarget && _inDraw)
        {
            HRESULT hr = _renderTarget->EndDraw();
            _inDraw = false;

            if (FAILED(hr))
            {
                LogDebug("WARNING: EndDraw failed hr=0x%08X (D2DERR_RECREATE_TARGET=0x%08X)", (unsigned)hr, (unsigned)D2DERR_RECREATE_TARGET);
                _endDrawFailed = true;
            }
        }
    }

    // ============================================
    // HELPER FUNCTIONS
    // ============================================

    D2D1_POINT_2F PdfPainterGPU::transformPoint(double x, double y, const PdfMatrix& ctm) const
    {
        // PDF -> Device space
        double tx = ctm.a * x + ctm.c * y + ctm.e;
        double ty = ctm.b * x + ctm.d * y + ctm.f;

        // Scale
        tx *= _scaleX;
        ty *= _scaleY;

        // Y flip
        ty = _h - ty;

        // ===== SAFETY: Check for NaN/Infinity =====
        // These can crash Direct2D!
        if (!std::isfinite(tx)) tx = 0.0;
        if (!std::isfinite(ty)) ty = 0.0;

        // Clamp to reasonable range to prevent GPU issues
        const double MAX_COORD = 1e7;
        tx = std::max(-MAX_COORD, std::min(MAX_COORD, tx));
        ty = std::max(-MAX_COORD, std::min(MAX_COORD, ty));

        return D2D1::Point2F((float)tx, (float)ty);
    }

    D2D1_COLOR_F PdfPainterGPU::toD2DColor(uint32_t argb) const
    {
        float a = ((argb >> 24) & 0xFF) / 255.0f;
        float r = ((argb >> 16) & 0xFF) / 255.0f;
        float g = ((argb >> 8) & 0xFF) / 255.0f;
        float b = (argb & 0xFF) / 255.0f;
        return D2D1::ColorF(r, g, b, a);
    }

    ID2D1PathGeometry* PdfPainterGPU::createPathGeometry(
        const std::vector<PdfPathSegment>& path,
        const PdfMatrix& ctm,
        bool evenOdd,
        bool implicitClose)
    {
        if (!s_d2dFactory || path.empty()) return nullptr;

        ID2D1PathGeometry* geometry = nullptr;
        HRESULT hr = s_d2dFactory->CreatePathGeometry(&geometry);
        if (FAILED(hr) || !geometry) return nullptr;

        ID2D1GeometrySink* sink = nullptr;
        hr = geometry->Open(&sink);
        if (FAILED(hr) || !sink)
        {
            geometry->Release();
            return nullptr;
        }

        // Set fill mode based on evenOdd parameter
        sink->SetFillMode(evenOdd ? D2D1_FILL_MODE_ALTERNATE : D2D1_FILL_MODE_WINDING);

        // PDF spec: For fill and clip operations, open subpaths are implicitly closed.
        // For strokes, subpaths remain open unless closed with explicit 'h' operator.
        D2D1_FIGURE_END openEnd = implicitClose ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN;

        bool figureStarted = false;

        for (const auto& seg : path)
        {
            switch (seg.type)
            {
            case PdfPathSegment::MoveTo:
                if (figureStarted)
                    sink->EndFigure(openEnd);
                {
                    D2D1_POINT_2F pt = transformPoint(seg.x, seg.y, ctm);
                    sink->BeginFigure(pt, D2D1_FIGURE_BEGIN_FILLED);
                }
                figureStarted = true;
                break;

            case PdfPathSegment::LineTo:
                if (figureStarted)
                {
                    D2D1_POINT_2F pt = transformPoint(seg.x, seg.y, ctm);
                    sink->AddLine(pt);
                }
                break;

            case PdfPathSegment::CurveTo:
                if (figureStarted)
                {
                    D2D1_BEZIER_SEGMENT bezier;
                    bezier.point1 = transformPoint(seg.x1, seg.y1, ctm);
                    bezier.point2 = transformPoint(seg.x2, seg.y2, ctm);
                    bezier.point3 = transformPoint(seg.x3, seg.y3, ctm);
                    sink->AddBezier(bezier);
                }
                break;

            case PdfPathSegment::Close:
                if (figureStarted)
                {
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    figureStarted = false;
                }
                break;
            }
        }

        if (figureStarted)
            sink->EndFigure(openEnd);

        hr = sink->Close();
        sink->Release();

        if (FAILED(hr))
        {
            geometry->Release();
            return nullptr;
        }

        return geometry;
    }

    // ============================================
    // CLEAR
    // ============================================

    void PdfPainterGPU::clear(uint32_t bgraColor)
    {
        if (!_renderTarget) return;

        bool wasInDraw = _inDraw;
        if (!_inDraw) beginDraw();

        _renderTarget->Clear(toD2DColor(bgraColor));

        if (!wasInDraw) endDraw();
    }

    // ============================================
    // PATH FILL - WITH PERFORMANCE OPTIMIZATIONS
    // ============================================

    // ============================================
    // HELPER: Check if path is an axis-aligned rectangle
    // Returns true and fills rect if it's a simple rectangle
    // ============================================
    static bool isAxisAlignedRect(
        const std::vector<PdfPathSegment>& path,
        const PdfMatrix& ctm,
        double scaleX, double scaleY, int h,
        D2D1_RECT_F& outRect)
    {
        // A rectangle has: MoveTo, 3x LineTo, Close (or 4x LineTo back to start)
        if (path.size() < 4 || path.size() > 6) return false;

        // Check structure: must be MoveTo followed by LineTo's
        if (path[0].type != PdfPathSegment::MoveTo) return false;

        // Collect all points
        std::vector<D2D1_POINT_2F> points;
        points.reserve(5);

        for (const auto& seg : path)
        {
            double tx, ty;
            switch (seg.type)
            {
            case PdfPathSegment::MoveTo:
            case PdfPathSegment::LineTo:
                tx = ctm.a * seg.x + ctm.c * seg.y + ctm.e;
                ty = ctm.b * seg.x + ctm.d * seg.y + ctm.f;
                tx *= scaleX;
                ty = h - ty * scaleY;

                // Safety: Check for NaN/Infinity
                if (!std::isfinite(tx) || !std::isfinite(ty))
                    return false;  // Can't be a valid rectangle

                points.push_back(D2D1::Point2F((float)tx, (float)ty));
                break;
            case PdfPathSegment::Close:
                // Closing the path - ignore
                break;
            case PdfPathSegment::CurveTo:
                // Has curves - not a simple rectangle
                return false;
            }
        }

        // Need exactly 4 unique points for a rectangle
        if (points.size() < 4) return false;

        // Check if it forms an axis-aligned rectangle
        // Get bounding box
        float minX = points[0].x, maxX = points[0].x;
        float minY = points[0].y, maxY = points[0].y;

        for (const auto& pt : points)
        {
            minX = std::min(minX, pt.x);
            maxX = std::max(maxX, pt.x);
            minY = std::min(minY, pt.y);
            maxY = std::max(maxY, pt.y);
        }

        // Check if all points are on the rectangle edges
        const float epsilon = 1.0f;  // Allow 1 pixel tolerance
        for (const auto& pt : points)
        {
            bool onVerticalEdge = (std::abs(pt.x - minX) < epsilon || std::abs(pt.x - maxX) < epsilon);
            bool onHorizontalEdge = (std::abs(pt.y - minY) < epsilon || std::abs(pt.y - maxY) < epsilon);

            if (!onVerticalEdge && !onHorizontalEdge)
                return false;
        }

        // It's a rectangle!
        outRect = D2D1::RectF(minX, minY, maxX, maxY);
        return true;
    }

    // ============================================
    // HELPER: Check if a bounding box is entirely inside a clip rect
    // If so, we can skip clipping entirely!
    // ============================================
    static bool isBBoxInsideClip(
        double fillMinX, double fillMinY, double fillMaxX, double fillMaxY,
        const D2D1_RECT_F& clipRect)
    {
        return (fillMinX >= clipRect.left - 1.0f &&
            fillMinY >= clipRect.top - 1.0f &&
            fillMaxX <= clipRect.right + 1.0f &&
            fillMaxY <= clipRect.bottom + 1.0f);
    }

    void PdfPainterGPU::fillPath(
        const std::vector<PdfPathSegment>& path,
        uint32_t color,
        const PdfMatrix& ctm,
        bool evenOdd,
        const std::vector<PdfPathSegment>* clipPath,
        const PdfMatrix* clipCTM,
        bool clipEvenOdd)
    {
        if (!_renderTarget || path.empty()) return;

        // ===== OPTIMIZATION 1: Calculate bounding box & skip tiny/outside paths =====
        double minX = 1e30, minY = 1e30, maxX = -1e30, maxY = -1e30;
        for (const auto& seg : path)
        {
            D2D1_POINT_2F pt;
            switch (seg.type)
            {
            case PdfPathSegment::MoveTo:
            case PdfPathSegment::LineTo:
                pt = transformPoint(seg.x, seg.y, ctm);
                minX = std::min(minX, (double)pt.x);
                minY = std::min(minY, (double)pt.y);
                maxX = std::max(maxX, (double)pt.x);
                maxY = std::max(maxY, (double)pt.y);
                break;
            case PdfPathSegment::CurveTo:
                pt = transformPoint(seg.x1, seg.y1, ctm);
                minX = std::min(minX, (double)pt.x); minY = std::min(minY, (double)pt.y);
                maxX = std::max(maxX, (double)pt.x); maxY = std::max(maxY, (double)pt.y);
                pt = transformPoint(seg.x2, seg.y2, ctm);
                minX = std::min(minX, (double)pt.x); minY = std::min(minY, (double)pt.y);
                maxX = std::max(maxX, (double)pt.x); maxY = std::max(maxY, (double)pt.y);
                pt = transformPoint(seg.x3, seg.y3, ctm);
                minX = std::min(minX, (double)pt.x); minY = std::min(minY, (double)pt.y);
                maxX = std::max(maxX, (double)pt.x); maxY = std::max(maxY, (double)pt.y);
                break;
            default:
                break;
            }
        }

        double bboxWidth = maxX - minX;
        double bboxHeight = maxY - minY;

        // Skip paths smaller than 0.5 pixel in both dimensions
        if (bboxWidth < 0.5 && bboxHeight < 0.5)
            return;

        // Skip paths completely outside viewport
        if (maxX < 0 || maxY < 0 || minX > _w || minY > _h)
            return;

        // ===== CLIP ANALYSIS =====
        bool hasClip = (clipPath && !clipPath->empty() && clipCTM);
        bool clipIsRect = false;
        D2D1_RECT_F clipRect = {};
        bool fillInsideClip = false;

        if (hasClip)
        {
            clipIsRect = isAxisAlignedRect(*clipPath, *clipCTM, _scaleX, _scaleY, _h, clipRect);

            // ===== OPTIMIZATION 2: If fill is entirely inside clip rect, skip clipping! =====
            if (clipIsRect)
            {
                fillInsideClip = isBBoxInsideClip(minX, minY, maxX, maxY, clipRect);
                if (fillInsideClip)
                {
                    hasClip = false;  // No need to clip!
                }
            }
        }

        // ===== BATCHING: If no clipping needed, use batch =====
        if (_inPageRender && !hasClip)
        {
            addToBatch(path, color, ctm, evenOdd);
            return;
        }

        // ===== NON-BATCHED RENDERING (with clipping) =====
        if (_inPageRender && hasClip)
        {
            flushFillBatch();
        }

        ID2D1PathGeometry* geometry = createPathGeometry(path, ctm, evenOdd);
        if (!geometry) return;

        // Safety check for render target
        if (!_renderTarget)
        {
            geometry->Release();
            return;
        }

        ID2D1SolidColorBrush* brush = nullptr;
        bool useCachedBrush = _inPageRender;

        if (useCachedBrush)
        {
            brush = getCachedBrush(color);
        }
        else
        {
            HRESULT hr = _renderTarget->CreateSolidColorBrush(toD2DColor(color), &brush);
            if (FAILED(hr)) brush = nullptr;
        }

        if (brush)
        {
            bool wasInDraw = _inDraw;
            if (!_inDraw) beginDraw();

            // ===== CLIPPING =====
            bool usedAxisAlignedClip = false;
            ID2D1Layer* clipLayer = nullptr;
            ID2D1PathGeometry* clipGeometry = nullptr;

            if (hasClip && _renderTarget)
            {
                if (clipIsRect)
                {
                    // FAST PATH: Use axis-aligned clip
                    _renderTarget->PushAxisAlignedClip(clipRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                    usedAxisAlignedClip = true;
                }
                else
                {
                    // SLOW PATH: Use layer-based clipping for complex paths
                    clipGeometry = createPathGeometry(*clipPath, *clipCTM, clipEvenOdd);
                    if (clipGeometry)
                    {
                        HRESULT hr = _renderTarget->CreateLayer(&clipLayer);
                        if (SUCCEEDED(hr) && clipLayer)
                        {
                            _renderTarget->PushLayer(
                                D2D1::LayerParameters(D2D1::InfiniteRect(), clipGeometry),
                                clipLayer
                            );
                        }
                    }
                }
            }

            if (_renderTarget)
                _renderTarget->FillGeometry(geometry, brush);

            // Pop clipping
            if (usedAxisAlignedClip && _renderTarget)
            {
                _renderTarget->PopAxisAlignedClip();
            }
            else if (clipLayer && _renderTarget)
            {
                _renderTarget->PopLayer();
                clipLayer->Release();
            }

            if (clipGeometry)
                clipGeometry->Release();

            if (!wasInDraw) endDraw();

            // Only release if not cached
            if (!useCachedBrush)
                brush->Release();
        }

        geometry->Release();
    }

    // ============================================
    // PATH STROKE - WITH PERFORMANCE OPTIMIZATIONS
    // ============================================

    void PdfPainterGPU::strokePath(
        const std::vector<PdfPathSegment>& path,
        uint32_t color,
        double lineWidth,
        const PdfMatrix& ctm,
        int lineCap,
        int lineJoin,
        double miterLimit)
    {
        if (!_renderTarget || path.empty()) return;

        // ===== OPTIMIZATION: Skip paths outside viewport =====
        double minX = 1e30, minY = 1e30, maxX = -1e30, maxY = -1e30;
        for (const auto& seg : path)
        {
            D2D1_POINT_2F pt;
            switch (seg.type)
            {
            case PdfPathSegment::MoveTo:
            case PdfPathSegment::LineTo:
                pt = transformPoint(seg.x, seg.y, ctm);
                minX = std::min(minX, (double)pt.x);
                minY = std::min(minY, (double)pt.y);
                maxX = std::max(maxX, (double)pt.x);
                maxY = std::max(maxY, (double)pt.y);
                break;
            case PdfPathSegment::CurveTo:
                pt = transformPoint(seg.x3, seg.y3, ctm);
                minX = std::min(minX, (double)pt.x); minY = std::min(minY, (double)pt.y);
                maxX = std::max(maxX, (double)pt.x); maxY = std::max(maxY, (double)pt.y);
                break;
            default:
                break;
            }
        }

        // Skip if completely outside viewport (with stroke width margin)
        double margin = lineWidth * _scaleX + 5;
        if (maxX < -margin || maxY < -margin || minX > _w + margin || minY > _h + margin)
            return;

        // ===== END OPTIMIZATION =====

        // Flush pending fill batch before stroke (render order matters)
        if (_inPageRender)
            flushFillBatch();

        // Flush pending glyph batch before stroke
        if (_hasGlyphBatch)
            flushGlyphBatch();

        ID2D1PathGeometry* geometry = createPathGeometry(path, ctm, false, false);  // implicitClose=false for strokes
        if (!geometry) return;

        // Safety check for render target
        if (!_renderTarget)
        {
            geometry->Release();
            return;
        }

        // Use cached brush if in page render mode
        ID2D1SolidColorBrush* brush = nullptr;
        bool useCachedBrush = _inPageRender;

        if (useCachedBrush)
        {
            brush = getCachedBrush(color);
        }
        else
        {
            HRESULT hr = _renderTarget->CreateSolidColorBrush(toD2DColor(color), &brush);
            if (FAILED(hr)) brush = nullptr;
        }

        if (brush)
        {
            // Create stroke style
            D2D1_STROKE_STYLE_PROPERTIES strokeProps = {};

            // Map PDF line cap to D2D
            switch (lineCap) {
            case 0: strokeProps.startCap = strokeProps.endCap = D2D1_CAP_STYLE_FLAT; break;
            case 1: strokeProps.startCap = strokeProps.endCap = D2D1_CAP_STYLE_ROUND; break;
            case 2: strokeProps.startCap = strokeProps.endCap = D2D1_CAP_STYLE_SQUARE; break;
            default: strokeProps.startCap = strokeProps.endCap = D2D1_CAP_STYLE_FLAT; break;
            }
            strokeProps.dashCap = strokeProps.startCap;

            // Map PDF line join to D2D
            switch (lineJoin) {
            case 0: strokeProps.lineJoin = D2D1_LINE_JOIN_MITER; break;
            case 1: strokeProps.lineJoin = D2D1_LINE_JOIN_ROUND; break;
            case 2: strokeProps.lineJoin = D2D1_LINE_JOIN_BEVEL; break;
            default: strokeProps.lineJoin = D2D1_LINE_JOIN_MITER; break;
            }

            strokeProps.miterLimit = (float)miterLimit;
            strokeProps.dashStyle = D2D1_DASH_STYLE_SOLID;

            ID2D1StrokeStyle* strokeStyle = nullptr;
            s_d2dFactory->CreateStrokeStyle(strokeProps, nullptr, 0, &strokeStyle);

            bool wasInDraw = _inDraw;
            if (!_inDraw) beginDraw();

            // Calculate stroke width in device space
            float strokeWidth = (float)(lineWidth * _scaleX);
            if (strokeWidth < 0.5f) strokeWidth = 0.5f;

            _renderTarget->DrawGeometry(geometry, brush, strokeWidth, strokeStyle);

            if (!wasInDraw) endDraw();

            if (strokeStyle) strokeStyle->Release();

            // Only release if not cached
            if (!useCachedBrush)
                brush->Release();
        }

        geometry->Release();
    }

    // ============================================
    // GRADIENT FILL
    // ============================================

    ID2D1Brush* PdfPainterGPU::createGradientBrush(
        const PdfGradient& gradient,
        const PdfMatrix& ctm,
        const PdfMatrix& gradientCTM)
    {
        if (!_renderTarget) return nullptr;

        // Use evaluateColor for smooth gradient (same as CPU)
        const int numStops = 256;
        std::vector<D2D1_GRADIENT_STOP> d2dStops(numStops);

        for (int i = 0; i < numStops; ++i)
        {
            float t = i / (float)(numStops - 1);

            double rgb[3];
            gradient.evaluateColor(t, rgb);

            float r = (float)std::clamp(rgb[0], 0.0, 1.0);
            float g = (float)std::clamp(rgb[1], 0.0, 1.0);
            float b = (float)std::clamp(rgb[2], 0.0, 1.0);

            d2dStops[i].position = t;
            d2dStops[i].color = D2D1::ColorF(r, g, b, 1.0f);
        }

        ID2D1GradientStopCollection* stopCollection = nullptr;
        HRESULT hr = _renderTarget->CreateGradientStopCollection(
            d2dStops.data(), numStops,
            D2D1_GAMMA_1_0,  // Linear gamma (PDF compatible)
            D2D1_EXTEND_MODE_CLAMP,
            &stopCollection
        );
        if (FAILED(hr)) return nullptr;

        ID2D1Brush* brush = nullptr;

        if (gradient.type == 2)  // Axial (Linear)
        {
            D2D1_POINT_2F start = transformPoint(gradient.x0, gradient.y0, gradientCTM);
            D2D1_POINT_2F end = transformPoint(gradient.x1, gradient.y1, gradientCTM);

            ID2D1LinearGradientBrush* linearBrush = nullptr;
            hr = _renderTarget->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(start, end),
                stopCollection,
                &linearBrush
            );
            brush = linearBrush;
        }
        else if (gradient.type == 3)  // Radial
        {
            D2D1_POINT_2F center = transformPoint(gradient.x0, gradient.y0, gradientCTM);
            float radius = (float)(gradient.r1 * _scaleX);

            ID2D1RadialGradientBrush* radialBrush = nullptr;
            hr = _renderTarget->CreateRadialGradientBrush(
                D2D1::RadialGradientBrushProperties(center, D2D1::Point2F(0, 0), radius, radius),
                stopCollection,
                &radialBrush
            );
            brush = radialBrush;
        }

        stopCollection->Release();
        return brush;
    }

    void PdfPainterGPU::fillPathWithGradient(
        const std::vector<PdfPathSegment>& path,
        const PdfGradient& gradient,
        const PdfMatrix& ctm,
        const PdfMatrix& gradientCTM,
        bool evenOdd)
    {
        if (!_renderTarget || path.empty()) return;

        // Flush pending batches
        if (_inPageRender)
            flushFillBatch();
        if (_hasGlyphBatch)
            flushGlyphBatch();

        ID2D1PathGeometry* geometry = createPathGeometry(path, ctm, evenOdd);
        if (!geometry) return;

        ID2D1Brush* brush = createGradientBrush(gradient, ctm, gradientCTM);
        if (!brush)
        {
            geometry->Release();
            return;
        }

        bool wasInDraw = _inDraw;
        if (!_inDraw) beginDraw();

        _renderTarget->FillGeometry(geometry, brush);

        if (!wasInDraw) endDraw();

        brush->Release();
        geometry->Release();
    }

    // ============================================
    // PATTERN FILL
    // ============================================

    ID2D1Brush* PdfPainterGPU::createPatternBrush(const PdfPattern& pattern, const PdfMatrix& ctm)
    {
        if (!_renderTarget || pattern.buffer.empty() || pattern.width <= 0 || pattern.height <= 0)
            return nullptr;

        // Pattern buffer is already BGRA with premultiplied alpha from PdfPainter (CPU tile renderer).
        // Just reinterpret uint32_t buffer as raw bytes - no need to re-premultiply.
        const uint8_t* bgra = reinterpret_cast<const uint8_t*>(pattern.buffer.data());

        ID2D1Bitmap* bitmap = nullptr;
        HRESULT hr = _renderTarget->CreateBitmap(
            D2D1::SizeU(pattern.width, pattern.height),
            bgra,
            pattern.width * 4,
            D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
            ),
            &bitmap
        );
        if (FAILED(hr)) return nullptr;

        ID2D1BitmapBrush* brush = nullptr;
        hr = _renderTarget->CreateBitmapBrush(
            bitmap,
            D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP),
            &brush
        );

        bitmap->Release();

        if (FAILED(hr)) return nullptr;

        // Compose brush transform: bitmap → pattern space → device space
        // Chain: bitmap_Y_flip * pattern.matrix * defaultCtm * painter_scale_flip
        //
        // 1. Bitmap Y-flip: PdfPainter renders with Y-flip (PDF Y-up → pixel Y-down)
        //    M_flip = (1, 0, 0, -1, 0, pH)  where pH = pattern.height
        // 2. Pattern matrix: pattern space → parent default coordinate system
        //    M_pat = pattern.matrix
        // 3. Default CTM: parent default CS → initial page CS
        //    M_ctm = pattern.defaultCtm
        // 4. Painter scale + Y-flip: page CS → device pixels
        //    M_dev = (_scaleX, 0, 0, -_scaleY, 0, _h)
        //
        // Combined: M_flip * M_pat * M_ctm * M_dev
        // For M_flip * M_pat:  (a, b, -c, -d, pH*c+e, pH*d+f) where a,b,c,d,e,f from M_pat
        // Then * M_ctm, then * M_dev
        //
        // Simplified: compute MC = M_pat * M_ctm, then final = M_flip(MC) * M_dev

        // Step 1: MC = pattern.matrix * defaultCtm (2D affine multiply)
        const auto& P = pattern.matrix;
        const auto& C = pattern.defaultCtm;
        double MCa = P.a * C.a + P.b * C.c;
        double MCb = P.a * C.b + P.b * C.d;
        double MCc = P.c * C.a + P.d * C.c;
        double MCd = P.c * C.b + P.d * C.d;
        double MCe = P.e * C.a + P.f * C.c + C.e;
        double MCf = P.e * C.b + P.f * C.d + C.f;

        // Step 2: Apply Y-flip (bitmap→pattern): negate c,d rows, add pH offset
        double pH = (double)pattern.height;
        double Fa = MCa;
        double Fb = MCb;
        double Fc = -MCc;
        double Fd = -MCd;
        double Fe = pH * MCc + MCe;
        double Ff = pH * MCd + MCf;

        // Step 3: Apply painter scale + Y-flip: (sx, 0, 0, -sy, 0, _h)
        double sx = _scaleX;
        double sy = _scaleY;
        D2D1_MATRIX_3X2_F transform = D2D1::Matrix3x2F(
            (float)(Fa * sx),   (float)(-Fb * sy),
            (float)(Fc * sx),   (float)(-Fd * sy),
            (float)(Fe * sx),   (float)(-Ff * sy + _h)
        );
        brush->SetTransform(transform);

        return brush;
    }

    void PdfPainterGPU::fillPathWithPattern(
        const std::vector<PdfPathSegment>& path,
        const PdfPattern& pattern,
        const PdfMatrix& ctm,
        bool evenOdd)
    {
        if (!_renderTarget || path.empty()) return;

        // Flush pending batches
        if (_inPageRender)
            flushFillBatch();
        if (_hasGlyphBatch)
            flushGlyphBatch();

        ID2D1PathGeometry* geometry = createPathGeometry(path, ctm, evenOdd);
        if (!geometry) return;

        ID2D1Brush* brush = createPatternBrush(pattern, ctm);
        if (!brush)
        {
            // Fallback to solid color
            ID2D1SolidColorBrush* solidBrush = nullptr;
            _renderTarget->CreateSolidColorBrush(toD2DColor(pattern.baseColor), &solidBrush);
            if (solidBrush)
            {
                bool wasInDraw = _inDraw;
                if (!_inDraw) beginDraw();
                _renderTarget->FillGeometry(geometry, solidBrush);
                if (!wasInDraw) endDraw();
                solidBrush->Release();
            }
            geometry->Release();
            return;
        }

        bool wasInDraw = _inDraw;
        if (!_inDraw) beginDraw();

        _renderTarget->FillGeometry(geometry, brush);

        if (!wasInDraw) endDraw();

        brush->Release();
        geometry->Release();
    }

    // ============================================
    // TEXT RENDERING - GLYPH BATCHING (OPTIMIZED)
    // ============================================

    void PdfPainterGPU::addGlyphToBatch(
        const uint8_t* bitmap, int width, int height, int pitch,
        float destX, float destY, uint32_t color, double scaleX, double scaleY)
    {
        if (!bitmap || width <= 0 || height <= 0) return;

        // Calculate scaled dimensions (minimum 1px to prevent glyph dropout)
        int scaledWidth = std::max(1, (int)std::round(width * scaleX));
        int scaledHeight = std::max(1, (int)std::round(height * scaleY));

        // Viewport culling - skip glyphs completely outside (use scaled size)
        if (destX + scaledWidth < 0 || destY + scaledHeight < 0 || destX > _w || destY > _h)
            return;

        // Safety: Limit glyph size to prevent memory issues
        if (width > 1024 || height > 1024) return;

        // If any scale != 1.0, render individually for better quality
        // Atlas-based batching doesn't support per-glyph scaling
        if (std::abs(scaleX - 1.0) > 0.01 || std::abs(scaleY - 1.0) > 0.01)
        {
            // Flush any pending batch first
            if (_hasGlyphBatch)
                flushGlyphBatch();

            // Create individual bitmap and draw with scaling
            std::vector<uint8_t> bgra(width * height * 4);
            uint8_t r = (color >> 16) & 0xFF;
            uint8_t g = (color >> 8) & 0xFF;
            uint8_t b = color & 0xFF;

            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    uint8_t alpha = bitmap[y * pitch + x];
                    size_t idx = (y * width + x) * 4;
                    bgra[idx + 0] = (uint8_t)((b * alpha) / 255);
                    bgra[idx + 1] = (uint8_t)((g * alpha) / 255);
                    bgra[idx + 2] = (uint8_t)((r * alpha) / 255);
                    bgra[idx + 3] = alpha;
                }
            }

            ID2D1Bitmap* d2dBitmap = nullptr;
            HRESULT hr = _renderTarget->CreateBitmap(
                D2D1::SizeU(width, height),
                bgra.data(),
                width * 4,
                D2D1::BitmapProperties(
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
                ),
                &d2dBitmap
            );

            if (SUCCEEDED(hr) && d2dBitmap)
            {
                // Use float positions for sub-pixel accuracy
                float sw = (float)(width * scaleX);
                float sh = (float)(height * scaleY);
                D2D1_RECT_F destRect = D2D1::RectF(
                    destX, destY,
                    destX + sw, destY + sh
                );
                _renderTarget->DrawBitmap(d2dBitmap, destRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                d2dBitmap->Release();
            }
            return;
        }

        // Normal batching for scale == 1.0
        // In text block mode (MuPDF-style): DON'T flush on color change
        // Instead, flush entire batch at endTextBlock()
        // This is crucial for Microsoft Print to PDF which uses many Tj commands
        if (!_inTextBlock)
        {
            // Outside text block: flush on color change (original behavior)
            if (_hasGlyphBatch && color != _glyphBatchColor)
            {
                flushGlyphBatch();
            }
        }
        // In text block mode: continue batching regardless of color
        // Color will be handled per-glyph in the atlas

        // Add glyph to batch
        GlyphDrawCmd cmd;
        cmd.destX = destX;
        cmd.destY = destY;
        cmd.width = width;
        cmd.height = height;
        cmd.pitch = pitch;
        cmd.scaledWidth = scaledWidth;
        cmd.scaledHeight = scaledHeight;
        cmd.scaleX = scaleX;
        cmd.scaleY = scaleY;
        cmd.color = color;  // Store color per-glyph for multi-color text blocks

        // Copy bitmap data (GlyphCache may evict entries)
        size_t bitmapSize = (size_t)height * (size_t)pitch;
        try {
            cmd.bitmap.resize(bitmapSize);
        }
        catch (const std::bad_alloc&) {
            flushGlyphBatch();  // Try to free memory
            return;
        }
        memcpy(cmd.bitmap.data(), bitmap, bitmapSize);

        _glyphBatch.push_back(std::move(cmd));
        _glyphBatchColor = color;
        _hasGlyphBatch = true;

        // Update bounding box (use scaled dimensions)
        _glyphBatchMinX = std::min(_glyphBatchMinX, destX);
        _glyphBatchMinY = std::min(_glyphBatchMinY, destY);
        _glyphBatchMaxX = std::max(_glyphBatchMaxX, destX + (float)scaledWidth);
        _glyphBatchMaxY = std::max(_glyphBatchMaxY, destY + (float)scaledHeight);

        // In text block mode: use larger batch limits
        size_t maxCount = _inTextBlock ? 2000 : GLYPH_BATCH_MAX_COUNT;
        int maxArea = _inTextBlock ? (4096 * 1024) : GLYPH_BATCH_MAX_AREA;

        // Flush if batch gets too large
        int area = (int)((_glyphBatchMaxX - _glyphBatchMinX) * (_glyphBatchMaxY - _glyphBatchMinY));
        if (_glyphBatch.size() >= maxCount || area > maxArea)
        {
            flushGlyphBatch();
        }
    }

    void PdfPainterGPU::flushGlyphBatch()
    {
        if (!_hasGlyphBatch || _glyphBatch.empty() || !_renderTarget)
            return;

        // Calculate atlas dimensions (round float bounds to integer for pixel buffer)
        // Use floor for min and ceil for max to ensure all glyphs fit
        int atlasOriginX = (int)std::floor(_glyphBatchMinX);
        int atlasOriginY = (int)std::floor(_glyphBatchMinY);
        int atlasWidth = (int)std::ceil(_glyphBatchMaxX) - atlasOriginX;
        int atlasHeight = (int)std::ceil(_glyphBatchMaxY) - atlasOriginY;

        if (atlasWidth <= 0 || atlasHeight <= 0)
        {
            _glyphBatch.clear();
            _hasGlyphBatch = false;
            return;
        }

        // Clamp to reasonable size
        atlasWidth = std::min(atlasWidth, 4096);
        atlasHeight = std::min(atlasHeight, 2048);

        // Safety: Check for reasonable memory allocation
        size_t atlasSize = (size_t)atlasWidth * (size_t)atlasHeight * 4;
        if (atlasSize > 64 * 1024 * 1024)  // 64MB limit for glyph atlas
        {
            _glyphBatch.clear();
            _hasGlyphBatch = false;
            return;
        }

        // Create atlas buffer (BGRA, premultiplied alpha)
        std::vector<uint8_t> atlas;
        try {
            atlas.resize(atlasSize, 0);
        }
        catch (const std::bad_alloc&) {
            _glyphBatch.clear();
            _hasGlyphBatch = false;
            return;  // Memory allocation failed
        }

        // Composite all glyphs into atlas - each glyph uses its own color
        for (const auto& cmd : _glyphBatch)
        {
            // Extract color components for this glyph
            uint8_t colorR = (cmd.color >> 16) & 0xFF;
            uint8_t colorG = (cmd.color >> 8) & 0xFF;
            uint8_t colorB = cmd.color & 0xFF;

            int offsetX = (int)std::round(cmd.destX) - atlasOriginX;
            int offsetY = (int)std::round(cmd.destY) - atlasOriginY;

            for (int y = 0; y < cmd.height; ++y)
            {
                int atlasY = offsetY + y;
                if (atlasY < 0 || atlasY >= atlasHeight) continue;

                for (int x = 0; x < cmd.width; ++x)
                {
                    int atlasX = offsetX + x;
                    if (atlasX < 0 || atlasX >= atlasWidth) continue;

                    uint8_t alpha = cmd.bitmap[y * cmd.pitch + x];
                    if (alpha == 0) continue;

                    size_t idx = (atlasY * atlasWidth + atlasX) * 4;

                    // Alpha blending with existing content (for overlapping glyphs)
                    uint8_t existingAlpha = atlas[idx + 3];
                    if (existingAlpha == 0)
                    {
                        // No existing content - just write
                        atlas[idx + 0] = (uint8_t)((colorB * alpha) / 255);  // B
                        atlas[idx + 1] = (uint8_t)((colorG * alpha) / 255);  // G
                        atlas[idx + 2] = (uint8_t)((colorR * alpha) / 255);  // R
                        atlas[idx + 3] = alpha;                               // A
                    }
                    else
                    {
                        // Blend with existing (over operator for premultiplied alpha)
                        uint8_t srcB = (uint8_t)((colorB * alpha) / 255);
                        uint8_t srcG = (uint8_t)((colorG * alpha) / 255);
                        uint8_t srcR = (uint8_t)((colorR * alpha) / 255);

                        float srcA = alpha / 255.0f;
                        float dstA = existingAlpha / 255.0f;
                        float outA = srcA + dstA * (1.0f - srcA);

                        if (outA > 0)
                        {
                            atlas[idx + 0] = (uint8_t)(srcB + atlas[idx + 0] * (1.0f - srcA));
                            atlas[idx + 1] = (uint8_t)(srcG + atlas[idx + 1] * (1.0f - srcA));
                            atlas[idx + 2] = (uint8_t)(srcR + atlas[idx + 2] * (1.0f - srcA));
                            atlas[idx + 3] = (uint8_t)(outA * 255);
                        }
                    }
                }
            }
        }

        // Create D2D bitmap from atlas
        ID2D1Bitmap* d2dBitmap = nullptr;
        HRESULT hr = _renderTarget->CreateBitmap(
            D2D1::SizeU(atlasWidth, atlasHeight),
            atlas.data(),
            atlasWidth * 4,
            D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
            ),
            &d2dBitmap
        );

        if (SUCCEEDED(hr) && d2dBitmap)
        {
            D2D1_RECT_F destRect = D2D1::RectF(
                (float)atlasOriginX, (float)atlasOriginY,
                (float)(atlasOriginX + atlasWidth), (float)(atlasOriginY + atlasHeight)
            );
            _renderTarget->DrawBitmap(d2dBitmap, destRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            d2dBitmap->Release();
        }

        // Clear batch
        _glyphBatch.clear();
        _hasGlyphBatch = false;
        _glyphBatchMinX = FLT_MAX;
        _glyphBatchMinY = FLT_MAX;
        _glyphBatchMaxX = -FLT_MAX;
        _glyphBatchMaxY = -FLT_MAX;
    }

    // Legacy single glyph draw (used when not in batch mode)
    void PdfPainterGPU::drawGlyphBitmapColored(
        const uint8_t* bitmap, int width, int height, int pitch,
        float destX, float destY,
        uint8_t r, uint8_t g, uint8_t b,
        double scaleX, double scaleY)
    {
        if (!_renderTarget || !bitmap || width <= 0 || height <= 0) return;

        // Calculate scaled dimensions (minimum 1px to prevent glyph dropout)
        int scaledWidth = std::max(1, (int)std::round(width * scaleX));
        int scaledHeight = std::max(1, (int)std::round(height * scaleY));

        // In page render mode, use batching
        if (_inPageRender)
        {
            uint32_t color = 0xFF000000 | (r << 16) | (g << 8) | b;
            addGlyphToBatch(bitmap, width, height, pitch, destX, destY, color, scaleX, scaleY);
            return;
        }

        // Legacy path - create individual bitmap
        std::vector<uint8_t> bgra(width * height * 4);
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                uint8_t alpha = bitmap[y * pitch + x];
                size_t idx = (y * width + x) * 4;

                // Premultiplied alpha
                bgra[idx + 0] = (uint8_t)((b * alpha) / 255);
                bgra[idx + 1] = (uint8_t)((g * alpha) / 255);
                bgra[idx + 2] = (uint8_t)((r * alpha) / 255);
                bgra[idx + 3] = alpha;
            }
        }

        ID2D1Bitmap* d2dBitmap = nullptr;
        HRESULT hr = _renderTarget->CreateBitmap(
            D2D1::SizeU(width, height),
            bgra.data(),
            width * 4,
            D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
            ),
            &d2dBitmap
        );

        if (SUCCEEDED(hr) && d2dBitmap)
        {
            // Draw with scaling and sub-pixel positioning
            float sw = (float)(width * scaleX);
            float sh = (float)(height * scaleY);
            D2D1_RECT_F destRect = D2D1::RectF(
                destX, destY,
                destX + sw, destY + sh
            );
            _renderTarget->DrawBitmap(d2dBitmap, destRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            d2dBitmap->Release();
        }
    }

    double PdfPainterGPU::drawTextFreeTypeRaw(
        double x, double y,
        const std::string& raw,
        double fontSizePt,
        double advanceSizePt,
        uint32_t color,
        const PdfFontInfo* font,
        double charSpacing,
        double wordSpacing,
        double horizScale,
        double textAngle)
    {
        // Type3 font branch: render CharProc streams instead of FreeType glyphs
        if (font && font->isType3) {
            return drawTextType3(x, y, raw, fontSizePt, advanceSizePt,
                color, font, charSpacing, wordSpacing, horizScale, textAngle);
        }

        // Early return checks
        if (!font || !font->ftReady || !font->ftFace) {
            return 0.0;
        }
        if (!_renderTarget || raw.empty()) return 0;

        // In text block mode (MuPDF-style): DON'T flush fill batch
        // Text will be rendered after all fills in the block
        if (_inPageRender && !_inTextBlock)
            flushFillBatch();

        // ═══════════════════════════════════════════════════════════════
        // TEXT ROTATION: For rotated text matrices (e.g. [0 1 -1 0 x y])
        // Apply a D2D transform around the text starting position.
        // All glyph rendering proceeds as if horizontal; D2D handles rotation.
        // ═══════════════════════════════════════════════════════════════
        bool hasTextRotation = (std::abs(textAngle) > 0.001);
        D2D1_MATRIX_3X2_F origTransform = D2D1::Matrix3x2F::Identity();

        if (hasTextRotation && _renderTarget) {
            // Flush any pending glyph batch before changing transform
            if (_hasGlyphBatch) flushGlyphBatch();

            _renderTarget->GetTransform(&origTransform);

            // Text starting position in device space
            float cx = (float)(x * _scaleX);
            float cy = (float)(_h - y * _scaleY);

            // Convert page-space angle to device-space angle
            // Device space Y is inverted (pointing down), so negate the angle
            float angleDeg = (float)(-textAngle * 180.0 / 3.14159265358979);

            D2D1_MATRIX_3X2_F rotMatrix = D2D1::Matrix3x2F::Rotation(angleDeg, D2D1::Point2F(cx, cy));
            _renderTarget->SetTransform(rotMatrix * origTransform);
        }

        bool wasInDraw = _inDraw;
        if (!_inDraw) beginDraw();

        FT_Face face = font->ftFace;

        // Font size in pixels (Y-scale for glyph height)
        double pxSize = fontSizePt * _scaleY;

        // Minimum pixel size for readable text
        const int MIN_PIXEL_SIZE = 4;
        const int MAX_PIXEL_SIZE = 512;

        int pixelSize = (int)std::round(pxSize);
        pixelSize = std::max(MIN_PIXEL_SIZE, std::min(pixelSize, MAX_PIXEL_SIZE));

        // Scale correction: if we render at different size than requested
        double scaleCorrection = pxSize / (double)pixelSize;

        // Snap to 1.0 if within 5% — uses sharp pixel-perfect atlas path
        // instead of blurry bilinear-scaled individual path.
        // Max error per glyph: ~0.8px at 16px, barely visible.
        if (std::abs(scaleCorrection - 1.0) < 0.05)
            scaleCorrection = 1.0;

        // Horizontal compression: ratio of X-scale to Y-scale
        // For non-uniform text matrices like [7.2 0 0 8]:
        //   fontSizePt ∝ Y-scale (8), advanceSizePt ∝ X-scale (7.2)
        //   horzCompress = 7.2/8 = 0.9 -> glyph bitmap is 90% as wide
        // For uniform matrices: horzCompress = 1.0 (no change)
        double horzCompress = 1.0;
        if (fontSizePt > 0.001)
            horzCompress = advanceSizePt / fontSizePt;

        // Starting position (device space)
        double penX = x * _scaleX;
        double penY = _h - y * _scaleY;

        // ═══════════════════════════════════════════════════════════════
        // BASELINE SNAPPING: Yuvarlama farkından kaynaklanan dikey kayma
        // düzeltmesi. penY (baseline) bir kez integer'a yuvarlanır,
        // böylece aynı satırdaki TÜM glyph'ler aynı Y seviyesine sahip olur.
        // bearingY offset'leri bu sabit baseline'a göre hesaplanır.
        // ═══════════════════════════════════════════════════════════════
        double baselineY = std::round(penY);

        double totalAdvance = 0;

        // Lambda for advance calculation (uses advanceSizePt for horizontal advance)
        auto getAdvancePx = [&](int code) -> double {
            int w1000 = getWidth1000ForCodeGPU(font, code);
            if (w1000 <= 0) w1000 = 500;
            double advPt = (w1000 / 1000.0) * advanceSizePt;
            advPt += charSpacing;
            if (code == 32) advPt += wordSpacing;
            advPt *= (horizScale / 100.0);
            return advPt * _scaleX;
            };

        // Extract color components for tinting
        uint8_t colorR = (color >> 16) & 0xFF;
        uint8_t colorG = (color >> 8) & 0xFF;
        uint8_t colorB = color & 0xFF;

        const bool cidMode = isCidFontActiveGPU(font);
        size_t fontHash = font->fontHash > 0 ? font->fontHash : reinterpret_cast<size_t>(face);

        if (cidMode)
        {
            // CID Font mode (2-byte codes)
            for (size_t i = 0; i + 1 < raw.size(); i += 2)
            {
                int cid = ((unsigned char)raw[i] << 8) | (unsigned char)raw[i + 1];
                FT_UInt gid = 0;

                // ToUnicode -> FT_Get_Char_Index for system fonts
                bool usedToUnicode = false;
                if (font->fontProgram.empty() && !font->cidToUnicode.empty()) {
                    auto it = font->cidToUnicode.find((uint16_t)cid);
                    if (it != font->cidToUnicode.end() && it->second != 0) {
                        gid = FT_Get_Char_Index(face, (FT_ULong)it->second);
                        usedToUnicode = true;
                    }
                }

                // CIDToGIDMap fallback
                if (!usedToUnicode) {
                    if (font->hasCidToGidMap) {
                        if (font->cidToGidIdentity) gid = (FT_UInt)cid;
                        else gid = (cid >= 0 && (size_t)cid < font->cidToGid.size())
                            ? (FT_UInt)font->cidToGid[cid] : (FT_UInt)cid;
                    }
                    else gid = (FT_UInt)cid;
                }

                // CID→GID ile glif bulunamadıysa ToUnicode → charmap fallback
                if (gid == 0 && !font->cidToUnicode.empty()) {
                    auto it = font->cidToUnicode.find((uint16_t)cid);
                    if (it != font->cidToUnicode.end() && it->second != 0) {
                        FT_UInt uniGid = FT_Get_Char_Index(face, (FT_ULong)it->second);
                        if (uniGid > 0) {
                            gid = uniGid;
                        }
                    }
                }

                double advPx = getAdvancePx(cid);

                // Use CPU GlyphCache for glyph rendering
                if (gid != 0) {
                    const CachedGlyph* cached = GlyphCache::instance().getOrRender(face, fontHash, gid, pixelSize);

                    if (cached && !cached->bitmap.empty()) {
                        bool useFreeTypeWidth = font->cidWidths.empty();
                        if (useFreeTypeWidth) {
                            double ftAdvPx = cached->advanceX * scaleCorrection;
                            // FreeType advance is based on fontSizePt (Y-scale), correct to X-scale
                            if (fontSizePt > 0.001)
                                ftAdvPx *= (advanceSizePt / fontSizePt);
                            ftAdvPx += charSpacing * _scaleX;
                            if (cid == 32) ftAdvPx += wordSpacing * _scaleX;
                            ftAdvPx *= (horizScale / 100.0);
                            advPx = ftAdvPx;
                        }

                        // Apply scale correction to bearing for proper positioning
                        // bearingX is also horizontally compressed for non-uniform text matrices
                        double scaledBearingX = cached->bearingX * scaleCorrection * horzCompress;
                        double scaledBearingY = cached->bearingY * scaleCorrection;

                        drawGlyphBitmapColored(
                            cached->bitmap.data(), cached->width, cached->height, cached->pitch,
                            (float)(penX + scaledBearingX),
                            (float)(baselineY - scaledBearingY),
                            colorR, colorG, colorB,
                            scaleCorrection * horzCompress,  // scaleX: includes horizontal compression
                            scaleCorrection);                // scaleY: no horizontal compression
                    }
                }

                penX += advPx;
                totalAdvance += advPx;
            }
        }
        else
        {
            // Simple font mode (1-byte codes)
            for (unsigned char c : raw)
            {
                int code = (int)c;
                FT_UInt gid = 0;

                // codeToGid table - with bounds check (256 element array)
                if (font->hasCodeToGid && code >= 0 && code < 256 && font->codeToGid[code] > 0) {
                    gid = font->codeToGid[code];
                }

                // Fallback: Unicode lookup
                if (gid == 0) {
                    uint32_t uni = 0;
                    // codeToUnicode - with bounds check (256 element array)
                    if (font->hasSimpleMap && c < 256 && font->codeToUnicode[c] != 0) {
                        uni = font->codeToUnicode[c];
                        uni = FixTurkishGPU(uni);
                    }
                    else if (c < 256) {
                        uni = WinAnsiGPU[c];
                        uni = FixTurkishGPU(uni);
                    }

                    if (uni != 0 && face->num_charmaps > 0) {
                        for (int cm = 0; cm < face->num_charmaps && gid == 0; cm++) {
                            FT_Set_Charmap(face, face->charmaps[cm]);
                            gid = FT_Get_Char_Index(face, (FT_ULong)uni);
                        }
                    }

                    // Direct code fallback
                    if (gid == 0 && face->num_charmaps > 0) {
                        for (int cm = 0; cm < face->num_charmaps && gid == 0; cm++) {
                            FT_Set_Charmap(face, face->charmaps[cm]);
                            gid = FT_Get_Char_Index(face, (FT_ULong)code);
                        }
                    }

                    // Symbolic TrueType cmap: 0xF000 + code (PDF spec)
                    if (gid == 0 && face->num_charmaps > 0) {
                        for (int cm = 0; cm < face->num_charmaps && gid == 0; cm++) {
                            FT_Set_Charmap(face, face->charmaps[cm]);
                            gid = FT_Get_Char_Index(face, (FT_ULong)(0xF000 + code));
                        }
                    }
                }

                double advPx = getAdvancePx(code);

                // Use CPU GlyphCache
                if (gid != 0) {
                    const CachedGlyph* cached = GlyphCache::instance().getOrRender(face, fontHash, gid, pixelSize);

                    if (cached && !cached->bitmap.empty()) {
                        bool useFreeTypeWidth = !font->hasWidths;
                        if (useFreeTypeWidth) {
                            double ftAdvPx = cached->advanceX * scaleCorrection;
                            // FreeType advance is based on fontSizePt (Y-scale), correct to X-scale
                            if (fontSizePt > 0.001)
                                ftAdvPx *= (advanceSizePt / fontSizePt);
                            ftAdvPx += charSpacing * _scaleX;
                            if (code == 32) ftAdvPx += wordSpacing * _scaleX;
                            ftAdvPx *= (horizScale / 100.0);
                            advPx = ftAdvPx;
                        }

                        // Apply scale correction to bearing for proper positioning
                        // bearingX is also horizontally compressed for non-uniform text matrices
                        double scaledBearingX = cached->bearingX * scaleCorrection * horzCompress;
                        double scaledBearingY = cached->bearingY * scaleCorrection;

                        drawGlyphBitmapColored(
                            cached->bitmap.data(), cached->width, cached->height, cached->pitch,
                            (float)(penX + scaledBearingX),
                            (float)(baselineY - scaledBearingY),
                            colorR, colorG, colorB,
                            scaleCorrection * horzCompress,  // scaleX: includes horizontal compression
                            scaleCorrection);                // scaleY: no horizontal compression
                    }
                }

                penX += advPx;
                totalAdvance += advPx;
            }
        }

        // Restore original transform if we applied text rotation
        if (hasTextRotation && _renderTarget) {
            // Flush glyph batch while rotation transform is active
            // so glyphs are drawn with the correct rotation
            if (_hasGlyphBatch) flushGlyphBatch();
            _renderTarget->SetTransform(origTransform);
        }

        if (!wasInDraw) endDraw();

        return totalAdvance / _scaleX;
    }

    // ============================================
    // TYPE3 FONT RENDERING
    // ============================================

    // Pre-parse d1 operator from CharProc stream to get bounding box
    static bool parseD1FromStream(const std::vector<uint8_t>& stream,
        double& wx, double& wy,
        double& llx, double& lly, double& urx, double& ury)
    {
        // Simple scanner: look for "d1" operator preceded by 6 numbers
        // Format: wx wy llx lly urx ury d1
        std::vector<double> nums;
        size_t pos = 0;
        size_t len = stream.size();

        while (pos < len)
        {
            // Skip whitespace
            while (pos < len && (stream[pos] == ' ' || stream[pos] == '\n' ||
                   stream[pos] == '\r' || stream[pos] == '\t'))
                pos++;
            if (pos >= len) break;

            // Check for d0 or d1
            if (pos + 1 < len && stream[pos] == 'd' && stream[pos + 1] == '1')
            {
                // Verify it's a word boundary
                bool wordEnd = (pos + 2 >= len || stream[pos + 2] == ' ' ||
                    stream[pos + 2] == '\n' || stream[pos + 2] == '\r');
                if (wordEnd && nums.size() >= 6)
                {
                    size_t base = nums.size() - 6;
                    wx = nums[base];     wy = nums[base + 1];
                    llx = nums[base + 2]; lly = nums[base + 3];
                    urx = nums[base + 4]; ury = nums[base + 5];
                    return true;
                }
            }
            if (pos + 1 < len && stream[pos] == 'd' && stream[pos + 1] == '0')
            {
                bool wordEnd = (pos + 2 >= len || stream[pos + 2] == ' ' ||
                    stream[pos + 2] == '\n' || stream[pos + 2] == '\r');
                if (wordEnd && nums.size() >= 2)
                {
                    size_t base = nums.size() - 2;
                    wx = nums[base]; wy = nums[base + 1];
                    llx = lly = urx = ury = 0;
                    return true;
                }
            }

            // Try to parse a number
            if ((stream[pos] >= '0' && stream[pos] <= '9') ||
                stream[pos] == '-' || stream[pos] == '.' || stream[pos] == '+')
            {
                size_t start = pos;
                if (stream[pos] == '-' || stream[pos] == '+') pos++;
                while (pos < len && ((stream[pos] >= '0' && stream[pos] <= '9') || stream[pos] == '.'))
                    pos++;
                std::string numStr((const char*)&stream[start], pos - start);
                try { nums.push_back(std::stod(numStr)); }
                catch (...) { nums.push_back(0); }
            }
            else
            {
                // Skip non-number token (operator name, etc.)
                while (pos < len && stream[pos] != ' ' && stream[pos] != '\n' &&
                       stream[pos] != '\r' && stream[pos] != '\t')
                    pos++;
            }
        }
        return false;
    }

    double PdfPainterGPU::drawTextType3(
        double x, double y,
        const std::string& raw,
        double fontSizePt,
        double advanceSizePt,
        uint32_t color,
        const PdfFontInfo* font,
        double charSpacing,
        double wordSpacing,
        double horizScale,
        double textAngle)
    {
        if (!font || !font->isType3 || raw.empty()) return 0.0;
        if (!_renderTarget) return 0.0;

        static int t3DebugCount = 0;
        if (t3DebugCount < 20) {
            LogDebug("[Type3] drawTextType3: font='%s' raw=%zu fontSize=%.2f advSize=%.2f scaleXY=(%.4f,%.4f) WxH=%dx%d charProcs=%zu",
                font->baseFont.c_str(), raw.size(), fontSizePt, advanceSizePt,
                _scaleX, _scaleY, _w, _h, font->type3CharProcs.size());
            LogDebug("[Type3] FontMatrix: [%.8f %.8f %.8f %.8f]", font->type3FontMatrix.a, font->type3FontMatrix.b,
                font->type3FontMatrix.c, font->type3FontMatrix.d);
            t3DebugCount++;
        }

        // In text block mode: DON'T flush fill batch
        if (_inPageRender && !_inTextBlock)
            flushFillBatch();

        // Text rotation setup (same as FreeType path)
        bool hasTextRotation = (std::abs(textAngle) > 0.001);
        D2D1_MATRIX_3X2_F origTransform = D2D1::Matrix3x2F::Identity();
        if (hasTextRotation && _renderTarget) {
            if (_hasGlyphBatch) flushGlyphBatch();
            _renderTarget->GetTransform(&origTransform);
            float cx = (float)(x * _scaleX);
            float cy = (float)(_h - y * _scaleY);
            float angleDeg = (float)(-textAngle * 180.0 / 3.14159265358979);
            D2D1_MATRIX_3X2_F rotMatrix = D2D1::Matrix3x2F::Rotation(angleDeg, D2D1::Point2F(cx, cy));
            _renderTarget->SetTransform(rotMatrix * origTransform);
        }

        bool wasInDraw = _inDraw;
        if (!_inDraw) beginDraw();

        // Type3 FontMatrix: maps glyph space → text space
        // Typically [0.001 0 0 0.001 0 0] or [0.00048828127 0 0 -0.00048828127 0 0]
        const PdfMatrix& fm = font->type3FontMatrix;
        double fmScaleX = std::abs(fm.a);
        double fmScaleY = std::abs(fm.d);
        if (fmScaleX < 1e-10) fmScaleX = 0.001;
        if (fmScaleY < 1e-10) fmScaleY = 0.001;
        bool fmFlipY = (fm.d < 0); // Chrome Type3 fonts often have negative d

        // Starting position (device space)
        double penX = x * _scaleX;
        double penY = _h - y * _scaleY;
        double baselineY = std::round(penY);

        double totalAdvance = 0;

        // Extract color components
        uint8_t colorR = (color >> 16) & 0xFF;
        uint8_t colorG = (color >> 8) & 0xFF;
        uint8_t colorB = color & 0xFF;

        // Process each character
        for (unsigned char c : raw)
        {
            int code = (int)c;

            // Get glyph name from encoding
            std::string glyphName;
            if (code >= 0 && code < 256 && !font->codeToGlyphName[code].empty()) {
                glyphName = font->codeToGlyphName[code];
            }

            static int t3GlyphDebug = 0;
            if (t3GlyphDebug < 30) {
                LogDebug("[Type3] code=0x%02X glyphName='%s' found=%d",
                    code, glyphName.c_str(),
                    !glyphName.empty() && font->type3CharProcs.count(glyphName) > 0);
                t3GlyphDebug++;
            }

            // Calculate advance from width table
            double advPx = 0;
            {
                int glyphWidth = 0;
                if (font->hasWidths && code >= font->firstChar &&
                    code < font->firstChar + (int)font->widths.size()) {
                    glyphWidth = font->widths[code - font->firstChar];
                }
                if (glyphWidth <= 0) glyphWidth = font->missingWidth;
                if (glyphWidth <= 0) glyphWidth = (int)std::round(1.0 / fmScaleX * 0.5); // half em default

                // Type3 widths are in glyph space; multiply by FontMatrix to get text space
                // (Standard fonts use /1000, but Type3 uses fontMatrix.a which may differ)
                double advPt = glyphWidth * fmScaleX * advanceSizePt;
                advPt += charSpacing;
                if (code == 32) advPt += wordSpacing;
                advPt *= (horizScale / 100.0);
                advPx = advPt * _scaleX;
            }

            // Look up CharProc
            if (!glyphName.empty())
            {
                auto it = font->type3CharProcs.find(glyphName);
                if (it != font->type3CharProcs.end() && !it->second.empty())
                {
                    const auto& charProcData = it->second;

                    // Separate X and Y pixel-per-unit (non-uniform text matrix support)
                    double ppuX = fmScaleX * advanceSizePt * _scaleX;  // glyph unit → device px (X)
                    double ppuY = fmScaleY * fontSizePt * _scaleY;     // glyph unit → device px (Y)

                    // Pre-parse d1/d0 to get bounding box
                    double wx = 0, wy = 0, llx = 0, lly = 0, urx = 0, ury = 0;
                    parseD1FromStream(charProcData, wx, wy, llx, lly, urx, ury);

                    double bboxMinX = std::min(llx, urx);
                    double bboxMaxX = std::max(llx, urx);
                    double bboxMinY = std::min(lly, ury);
                    double bboxMaxY = std::max(lly, ury);
                    double bboxW = bboxMaxX - bboxMinX;
                    double bboxH = bboxMaxY - bboxMinY;

                    if (bboxW < 1) { bboxMinX = 0; bboxMaxX = wx > 0 ? wx : 2048; bboxW = bboxMaxX - bboxMinX; }
                    if (bboxH < 1) { bboxMinY = -2048; bboxMaxY = 0; bboxH = 2048; }

                    // Render at actual display size for pixel-perfect quality
                    double finalDevW = bboxW * ppuX;
                    double finalDevH = bboxH * ppuY;
                    int bmpW = std::clamp((int)std::ceil(finalDevW), 4, 512);
                    int bmpH = std::clamp((int)std::ceil(finalDevH), 8, 512);

                    // Quantize to multiples of 4 to reduce cache entries across zoom levels
                    bmpW = ((bmpW + 3) / 4) * 4;
                    bmpH = ((bmpH + 3) / 4) * 4;

                    // Cache key: fontHash ^ glyphName ^ quantized size
                    size_t cacheKey = font->fontHash;
                    cacheKey ^= std::hash<std::string>{}(glyphName) + 0x9e3779b9 + (cacheKey << 6) + (cacheKey >> 2);
                    cacheKey ^= ((size_t)bmpH << 16) | (size_t)bmpW;

                    auto cacheIt = _type3Cache.find(cacheKey);
                    if (cacheIt == _type3Cache.end())
                    {
                        static int t3RenderDebug = 0;
                        if (t3RenderDebug < 15) {
                            LogDebug("[Type3] Rendering '%s': wx=%.0f bbox=[%.0f,%.0f,%.0f,%.0f] bmp=%dx%d fmFlipY=%d ppuXY=(%.6f,%.6f) devWH=(%.1f,%.1f)",
                                glyphName.c_str(), wx, bboxMinX, bboxMinY, bboxMaxX, bboxMaxY, bmpW, bmpH, (int)fmFlipY, ppuX, ppuY, finalDevW, finalDevH);
                        }

                        // SSAA=2 for anti-aliased edges (one-time cost per glyph, doesn't affect GPU frame rate)
                        PdfPainter cpuPainter(bmpW, bmpH, 1.0, 1.0, 2);
                        cpuPainter.clear(0xFF000000); // Black background

                        // CTM: glyph space → bitmap coordinates
                        double sX = (double)bmpW / bboxW;
                        double sY = (double)bmpH / bboxH;

                        PdfMatrix glyphCTM;
                        glyphCTM.a = sX;
                        glyphCTM.b = 0;
                        glyphCTM.c = 0;

                        if (fmFlipY) {
                            glyphCTM.d = -sY;
                            glyphCTM.f = bboxMaxY * sY;
                        } else {
                            glyphCTM.d = sY;
                            glyphCTM.f = -bboxMinY * sY;
                        }
                        glyphCTM.e = -bboxMinX * sX;

                        PdfGraphicsState childGs;
                        childGs.ctm = glyphCTM;
                        childGs.fillColor[0] = 1.0;
                        childGs.fillColor[1] = 1.0;
                        childGs.fillColor[2] = 1.0;

                        std::vector<std::shared_ptr<PdfDictionary>> resStack;
                        if (font->type3Resources)
                            resStack.push_back(font->type3Resources);

                        std::map<std::string, PdfFontInfo> charProcFonts;

                        PdfContentParser charParser(
                            charProcData,
                            &cpuPainter,
                            nullptr,
                            -1,
                            &charProcFonts,
                            childGs,
                            resStack
                        );
                        charParser.parse();

                        // Extract alpha from rendered buffer
                        std::vector<uint8_t> rendered = cpuPainter.getBuffer();
                        Type3CachedGlyph cached;
                        cached.width = bmpW;
                        cached.height = bmpH;
                        cached.bboxW = bboxW;
                        cached.bboxH = bboxH;
                        cached.alpha.resize((size_t)bmpW * bmpH);

                        for (int py = 0; py < bmpH; ++py) {
                            for (int px = 0; px < bmpW; ++px) {
                                size_t idx = ((size_t)py * bmpW + px) * 4;
                                uint8_t b = rendered[idx + 0];
                                uint8_t g = rendered[idx + 1];
                                uint8_t r = rendered[idx + 2];
                                cached.alpha[py * bmpW + px] = (uint8_t)std::max({ (int)r, (int)g, (int)b });
                            }
                        }

                        cached.bearingX = (int)std::round(bboxMinX);
                        if (fmFlipY) {
                            cached.bearingY = (int)std::round(std::abs(bboxMinY));
                        } else {
                            cached.bearingY = (int)std::round(bboxMaxY);
                        }

                        if (t3RenderDebug < 15) {
                            int nonZero = 0;
                            for (size_t pi = 0; pi < cached.alpha.size(); pi++)
                                if (cached.alpha[pi] > 0) nonZero++;
                            LogDebug("[Type3] Result: bmp=%dx%d bearing=(%d,%d) nonZero=%d/%zu",
                                bmpW, bmpH, cached.bearingX, cached.bearingY, nonZero, cached.alpha.size());
                            t3RenderDebug++;
                        }

                        _type3Cache[cacheKey] = std::move(cached);
                        cacheIt = _type3Cache.find(cacheKey);
                    }

                    // Draw the cached glyph (separate X/Y scale for non-uniform text matrices)
                    if (cacheIt != _type3Cache.end())
                    {
                        const auto& glyph = cacheIt->second;
                        if (!glyph.alpha.empty() && glyph.width > 0 && glyph.height > 0)
                        {
                            double finalW = glyph.bboxW * ppuX;
                            double finalH = glyph.bboxH * ppuY;
                            double scaleGX = finalW / (double)glyph.width;
                            double scaleGY = finalH / (double)glyph.height;
                            if (scaleGX < 0.01) scaleGX = 0.01;
                            if (scaleGY < 0.01) scaleGY = 0.01;

                            // X bearing uses X ppu, Y bearing uses Y ppu
                            float destX = (float)(penX + glyph.bearingX * ppuX);
                            float destY = (float)(baselineY - glyph.bearingY * ppuY);

                            drawGlyphBitmapColored(
                                glyph.alpha.data(),
                                glyph.width, glyph.height, glyph.width,
                                destX, destY,
                                colorR, colorG, colorB,
                                scaleGX, scaleGY);
                        }
                    }
                }
            }

            penX += advPx;
            totalAdvance += advPx;
        }

        // Restore rotation transform
        if (hasTextRotation && _renderTarget) {
            if (_hasGlyphBatch) flushGlyphBatch();
            _renderTarget->SetTransform(origTransform);
        }

        if (!wasInDraw) endDraw();
        return totalAdvance / _scaleX;
    }

    // ============================================
    // IMAGE RENDERING
    // ============================================

    ID2D1Bitmap* PdfPainterGPU::createBitmapFromARGB(
        const std::vector<uint8_t>& rgba, int w, int h)
    {
        if (!_renderTarget) return nullptr;

        // Safety: Validate dimensions
        if (w <= 0 || h <= 0) return nullptr;
        if (w > 16384 || h > 16384) return nullptr;  // D2D max texture size limit

        size_t expectedSize = (size_t)w * (size_t)h * 4;
        if (rgba.size() < expectedSize) return nullptr;

        // Safety: Check for reasonable memory allocation
        if (expectedSize > 256 * 1024 * 1024) return nullptr;  // 256MB limit

        // Convert RGBA to BGRA (Direct2D expects BGRA format)
        std::vector<uint8_t> bgra;
        try {
            bgra.resize(expectedSize);
        }
        catch (const std::bad_alloc&) {
            return nullptr;  // Memory allocation failed
        }

        for (size_t i = 0; i < (size_t)w * h; ++i)
        {
            uint8_t r = rgba[i * 4 + 0];
            uint8_t g = rgba[i * 4 + 1];
            uint8_t b = rgba[i * 4 + 2];
            uint8_t a = rgba[i * 4 + 3];

            // ✅ FIX: Premultiplied alpha for D2D
            // When a < 255, RGB must be multiplied by alpha/255
            // When a == 0, RGB must be 0 (fully transparent pixel)
            if (a < 255)
            {
                if (a == 0)
                {
                    r = g = b = 0;  // Fully transparent - RGB must be 0
                }
                else
                {
                    r = (uint8_t)((r * a) / 255);
                    g = (uint8_t)((g * a) / 255);
                    b = (uint8_t)((b * a) / 255);
                }
            }

            bgra[i * 4 + 0] = b;
            bgra[i * 4 + 1] = g;
            bgra[i * 4 + 2] = r;
            bgra[i * 4 + 3] = a;
        }

        ID2D1Bitmap* bitmap = nullptr;
        HRESULT hr = _renderTarget->CreateBitmap(
            D2D1::SizeU(w, h),
            bgra.data(),
            w * 4,
            D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
            ),
            &bitmap
        );

        return SUCCEEDED(hr) ? bitmap : nullptr;
    }

    ID2D1Bitmap* PdfPainterGPU::createScaledBitmapFromARGB(
        const std::vector<uint8_t>& argb, int srcW, int srcH, int dstW, int dstH)
    {
        if (!_renderTarget || !s_wicFactory) return nullptr;
        if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return nullptr;
        if (dstW > 16384 || dstH > 16384) return nullptr;

        size_t expectedSize = (size_t)srcW * srcH * 4;
        if (argb.size() < expectedSize) return nullptr;

        // Convert RGBA to BGRA premultiplied for WIC source
        std::vector<uint8_t> bgra(expectedSize);
        for (size_t i = 0; i < (size_t)srcW * srcH; ++i)
        {
            uint8_t r = argb[i * 4 + 0];
            uint8_t g = argb[i * 4 + 1];
            uint8_t b = argb[i * 4 + 2];
            uint8_t a = argb[i * 4 + 3];
            if (a < 255) {
                if (a == 0) { r = g = b = 0; }
                else { r = (uint8_t)((r * a) / 255); g = (uint8_t)((g * a) / 255); b = (uint8_t)((b * a) / 255); }
            }
            bgra[i * 4 + 0] = b;
            bgra[i * 4 + 1] = g;
            bgra[i * 4 + 2] = r;
            bgra[i * 4 + 3] = a;
        }

        // Create WIC source bitmap from raw pixel data
        IWICBitmap* wicSrc = nullptr;
        HRESULT hr = s_wicFactory->CreateBitmapFromMemory(
            srcW, srcH,
            GUID_WICPixelFormat32bppPBGRA,
            srcW * 4,
            (UINT)bgra.size(),
            bgra.data(),
            &wicSrc
        );
        if (FAILED(hr) || !wicSrc) return nullptr;

        // Create WIC scaler with high-quality Fant interpolation
        IWICBitmapScaler* scaler = nullptr;
        hr = s_wicFactory->CreateBitmapScaler(&scaler);
        if (FAILED(hr) || !scaler) { wicSrc->Release(); return nullptr; }

        hr = scaler->Initialize(wicSrc, dstW, dstH, WICBitmapInterpolationModeFant);
        wicSrc->Release();
        if (FAILED(hr)) { scaler->Release(); return nullptr; }

        // Read scaled pixels
        std::vector<uint8_t> scaledBgra((size_t)dstW * dstH * 4);
        hr = scaler->CopyPixels(nullptr, dstW * 4, (UINT)scaledBgra.size(), scaledBgra.data());
        scaler->Release();
        if (FAILED(hr)) return nullptr;

        // Create D2D bitmap from scaled data
        ID2D1Bitmap* bitmap = nullptr;
        hr = _renderTarget->CreateBitmap(
            D2D1::SizeU(dstW, dstH),
            scaledBgra.data(),
            dstW * 4,
            D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
            ),
            &bitmap
        );

        return SUCCEEDED(hr) ? bitmap : nullptr;
    }

    void PdfPainterGPU::drawImage(
        const std::vector<uint8_t>& argb,
        int imgW, int imgH,
        const PdfMatrix& ctm)
    {
        if (!_renderTarget || argb.empty()) return;
        if (imgW <= 0 || imgH <= 0) return;
        if (imgW > 16384 || imgH > 16384) return;  // Safety limit

        // Safety: Check CTM values for NaN/Infinity
        if (!std::isfinite(ctm.a) || !std::isfinite(ctm.b) ||
            !std::isfinite(ctm.c) || !std::isfinite(ctm.d) ||
            !std::isfinite(ctm.e) || !std::isfinite(ctm.f))
        {
            return;  // Invalid transformation matrix
        }

        // Flush pending batches
        if (_inPageRender)
            flushFillBatch();
        if (_hasGlyphBatch)
            flushGlyphBatch();

        // PDF Image CTM explanation:
        // Standard PDF image CTM: [width 0 0 -height x y+height] (origin at top-left, y increases downward in image)
        // Less common: [width 0 0 height x y] (origin at bottom-left, y increases upward in image)
        //
        // ctm.d < 0: Normal case - image stored top-to-bottom, displayed top-to-bottom
        // ctm.d > 0: Rare case - image needs vertical flip
        //
        // Since we already flip Y coordinates in our rendering (_h - y), we need to:
        // - If ctm.d < 0: Don't flip bitmap (standard PDF behavior)
        // - If ctm.d > 0: Flip bitmap vertically

        bool needsYFlip = (ctm.d > 0);  // Flip only when d is POSITIVE (rare case)
        bool needsXFlip = (ctm.a < 0);

        // Create bitmap - possibly flipped
        std::vector<uint8_t> flippedArgb;
        const std::vector<uint8_t>* bitmapData = &argb;

        if (needsYFlip || needsXFlip)
        {
            flippedArgb.resize(argb.size());
            for (int y = 0; y < imgH; ++y)
            {
                int srcY = needsYFlip ? (imgH - 1 - y) : y;
                for (int x = 0; x < imgW; ++x)
                {
                    int srcX = needsXFlip ? (imgW - 1 - x) : x;
                    int srcIdx = (srcY * imgW + srcX) * 4;
                    int dstIdx = (y * imgW + x) * 4;
                    flippedArgb[dstIdx + 0] = argb[srcIdx + 0];
                    flippedArgb[dstIdx + 1] = argb[srcIdx + 1];
                    flippedArgb[dstIdx + 2] = argb[srcIdx + 2];
                    flippedArgb[dstIdx + 3] = argb[srcIdx + 3];
                }
            }
            bitmapData = &flippedArgb;
        }

        bool wasInDraw = _inDraw;
        if (!_inDraw) beginDraw();

        // Calculate image dimensions (always use absolute values)
        double imgWidthPts = std::abs(ctm.a);
        double imgHeightPts = std::abs(ctm.d);

        // Calculate destination rectangle
        // PDF coordinate: origin at bottom-left, y increases upward
        // Our coordinate: origin at top-left, y increases downward
        //
        // PDF Image CTM: [width 0 0 -height x y]
        // - ctm.e = x (left edge)
        // - ctm.f = y (TOP edge in PDF coords when d < 0)
        // - ctm.a = width
        // - ctm.d = -height (negative means image goes downward from ctm.f)

        double left, right, top, bottom;

        if (ctm.a >= 0)
        {
            left = ctm.e * _scaleX;
            right = left + imgWidthPts * _scaleX;
        }
        else
        {
            right = ctm.e * _scaleX;
            left = right - imgWidthPts * _scaleX;
        }

        if (ctm.d < 0)
        {
            double pdfTop = ctm.f;
            double pdfBottom = ctm.f + ctm.d;
            top = _h - pdfTop * _scaleY;
            bottom = _h - pdfBottom * _scaleY;
        }
        else
        {
            double pdfBottom = ctm.f;
            double pdfTop = ctm.f + ctm.d;
            top = _h - pdfTop * _scaleY;
            bottom = _h - pdfBottom * _scaleY;
        }

        // Ensure correct ordering
        if (left > right) std::swap(left, right);
        if (top > bottom) std::swap(top, bottom);

        // Calculate destination pixel dimensions
        int destPixW = (int)(right - left + 0.5);
        int destPixH = (int)(bottom - top + 0.5);

        // Use WIC high-quality downscaling when image is significantly larger than destination
        ID2D1Bitmap* bitmap = nullptr;
        if (destPixW > 0 && destPixH > 0 &&
            (imgW > destPixW * 2 || imgH > destPixH * 2))
        {
            // Image is more than 2x larger than destination - use WIC Fant downscale
            bitmap = createScaledBitmapFromARGB(*bitmapData, imgW, imgH, destPixW, destPixH);
        }

        if (!bitmap)
        {
            // Fallback: use original resolution
            bitmap = createBitmapFromARGB(*bitmapData, imgW, imgH);
        }
        if (!bitmap) { if (!wasInDraw) endDraw(); return; }

        D2D1_RECT_F destRect = D2D1::RectF((float)left, (float)top, (float)right, (float)bottom);
        drawBitmapHighQuality(bitmap, destRect);

        if (!wasInDraw) endDraw();
        bitmap->Release();
    }

    void PdfPainterGPU::drawImageWithClipRect(
        const std::vector<uint8_t>& argb,
        int imgW, int imgH,
        const PdfMatrix& ctm,
        int clipMinX, int clipMinY,
        int clipMaxX, int clipMaxY)
    {
        if (!_renderTarget || argb.empty()) return;
        if (imgW <= 0 || imgH <= 0) return;

        // Flush pending batches
        if (_inPageRender)
            flushFillBatch();
        if (_hasGlyphBatch)
            flushGlyphBatch();

        bool wasInDraw = _inDraw;
        if (!_inDraw) beginDraw();

        // Push axis-aligned clip
        D2D1_RECT_F clipRect = D2D1::RectF(
            (float)clipMinX, (float)clipMinY,
            (float)clipMaxX, (float)clipMaxY
        );
        _renderTarget->PushAxisAlignedClip(clipRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        // Draw image
        D2D1_POINT_2F p0 = transformPoint(0, 0, ctm);
        D2D1_POINT_2F p1 = transformPoint(1, 0, ctm);
        D2D1_POINT_2F p2 = transformPoint(1, 1, ctm);
        D2D1_POINT_2F p3 = transformPoint(0, 1, ctm);

        float minX = std::min({ p0.x, p1.x, p2.x, p3.x });
        float minY = std::min({ p0.y, p1.y, p2.y, p3.y });
        float maxX = std::max({ p0.x, p1.x, p2.x, p3.x });
        float maxY = std::max({ p0.y, p1.y, p2.y, p3.y });

        int destPixW = (int)(maxX - minX + 0.5f);
        int destPixH = (int)(maxY - minY + 0.5f);

        ID2D1Bitmap* bitmap = nullptr;
        if (destPixW > 0 && destPixH > 0 &&
            (imgW > destPixW * 2 || imgH > destPixH * 2))
        {
            bitmap = createScaledBitmapFromARGB(argb, imgW, imgH, destPixW, destPixH);
        }
        if (!bitmap) bitmap = createBitmapFromARGB(argb, imgW, imgH);

        if (bitmap)
        {
            D2D1_RECT_F destRect = D2D1::RectF(minX, minY, maxX, maxY);
            drawBitmapHighQuality(bitmap, destRect);
            bitmap->Release();
        }

        _renderTarget->PopAxisAlignedClip();

        if (!wasInDraw) endDraw();
    }

    void PdfPainterGPU::drawImageClipped(
        const std::vector<uint8_t>& argb,
        int imgW, int imgH,
        const PdfMatrix& ctm,
        const std::vector<PdfPathSegment>& clipPath,
        const PdfMatrix& clipCTM,
        bool hasRectClip,
        double rectMinX, double rectMinY,
        double rectMaxX, double rectMaxY)
    {
        if (!_renderTarget || argb.empty()) return;
        if (imgW <= 0 || imgH <= 0) return;
        if (imgW > 16384 || imgH > 16384) return;

        if (!std::isfinite(ctm.a) || !std::isfinite(ctm.b) ||
            !std::isfinite(ctm.c) || !std::isfinite(ctm.d) ||
            !std::isfinite(ctm.e) || !std::isfinite(ctm.f))
        {
            return;
        }

        if (_inPageRender) flushFillBatch();
        if (_hasGlyphBatch) flushGlyphBatch();

        bool wasInDraw = _inDraw;
        if (!_inDraw) beginDraw();

        // Create clip geometry from path
        ID2D1PathGeometry* clipGeometry = nullptr;
        if (!clipPath.empty())
        {
            clipGeometry = createPathGeometry(clipPath, clipCTM, false, true);  // implicitClose=true for clips
        }

        // Apply rect clip if provided
        if (hasRectClip)
        {
            D2D1_RECT_F clipRect = D2D1::RectF(
                (float)(rectMinX * _scaleX),
                (float)(_h - rectMaxY * _scaleY),
                (float)(rectMaxX * _scaleX),
                (float)(_h - rectMinY * _scaleY)
            );
            _renderTarget->PushAxisAlignedClip(clipRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        }

        // Compute per-axis device extent for proper pre-scaling
        // u-axis (image width direction) device length
        double uLen = std::sqrt((ctm.a * _scaleX) * (ctm.a * _scaleX) + (ctm.b * _scaleY) * (ctm.b * _scaleY));
        // v-axis (image height direction) device length
        double vLen = std::sqrt((ctm.c * _scaleX) * (ctm.c * _scaleX) + (ctm.d * _scaleY) * (ctm.d * _scaleY));
        int destPixW = std::max(1, (int)(uLen + 0.5));
        int destPixH = std::max(1, (int)(vLen + 0.5));

        // Use WIC pre-scaling when image is significantly larger than destination
        int bitmapW = imgW, bitmapH = imgH;
        ID2D1Bitmap* bitmap = nullptr;
        if (destPixW > 0 && destPixH > 0 &&
            (imgW > destPixW * 2 || imgH > destPixH * 2))
        {
            bitmap = createScaledBitmapFromARGB(argb, imgW, imgH, destPixW, destPixH);
            if (bitmap) { bitmapW = destPixW; bitmapH = destPixH; }
        }
        if (!bitmap) bitmap = createBitmapFromARGB(argb, imgW, imgH);

        if (bitmap && clipGeometry)
        {
            float sx = _scaleX;
            float sy = _scaleY;
            float h = (float)_h;

            float imgScaleX = 1.0f / bitmapW;
            float imgScaleY = 1.0f / bitmapH;

            D2D1_MATRIX_3X2_F brushTransform = D2D1::Matrix3x2F(
                (float)(ctm.a * sx * imgScaleX), (float)(-ctm.b * sy * imgScaleX),
                (float)(ctm.c * sx * imgScaleY), (float)(-ctm.d * sy * imgScaleY),
                (float)(ctm.e * sx), (float)(h - ctm.f * sy)
            );

            // Create bitmap brush
            ID2D1BitmapBrush* brush = nullptr;
            D2D1_BITMAP_BRUSH_PROPERTIES brushProps = D2D1::BitmapBrushProperties(
                D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR
            );
            D2D1_BRUSH_PROPERTIES bProps = D2D1::BrushProperties(1.0f, brushTransform);

            HRESULT hr = _renderTarget->CreateBitmapBrush(bitmap, brushProps, bProps, &brush);
            if (SUCCEEDED(hr) && brush)
            {
                // Fill the clipping geometry directly with the image brush
                // D2D handles transform + clipping automatically
                _renderTarget->FillGeometry(clipGeometry, brush);
                brush->Release();
            }

            bitmap->Release();
        }
        else if (bitmap)
        {
            // No clip geometry - fall back to simple DrawBitmap with transform
            float sx = _scaleX;
            float sy = _scaleY;
            float h = (float)_h;
            float imgScaleX = 1.0f / bitmapW;
            float imgScaleY = 1.0f / bitmapH;

            D2D1_MATRIX_3X2_F oldTransform;
            _renderTarget->GetTransform(&oldTransform);

            D2D1_MATRIX_3X2_F imageTransform = D2D1::Matrix3x2F(
                (float)(ctm.a * sx * imgScaleX), (float)(-ctm.b * sy * imgScaleX),
                (float)(ctm.c * sx * imgScaleY), (float)(-ctm.d * sy * imgScaleY),
                (float)(ctm.e * sx), (float)(h - ctm.f * sy)
            );

            _renderTarget->SetTransform(imageTransform);
            D2D1_RECT_F destRect = D2D1::RectF(0, 0, (float)bitmapW, (float)bitmapH);
            drawBitmapHighQuality(bitmap, destRect);
            _renderTarget->SetTransform(oldTransform);

            bitmap->Release();
        }

        // Pop rect clip
        if (hasRectClip)
        {
            _renderTarget->PopAxisAlignedClip();
        }

        // Release clip geometry
        if (clipGeometry)
            clipGeometry->Release();

        if (!wasInDraw) endDraw();
    }

    // ============================================
    // PAGE ROTATION
    // ============================================

    void PdfPainterGPU::setPageRotation(int degrees, double pageWPt, double pageHPt)
    {
        if (degrees == 0)
        {
            _hasRotate = false;
            _rotMatrix = D2D1::IdentityMatrix();
            return;
        }

        _hasRotate = true;

        double rad = degrees * 3.14159265358979 / 180.0;
        double cosA = cos(rad);
        double sinA = sin(rad);

        double tx = 0, ty = 0;
        switch (degrees)
        {
        case 90:
            tx = pageHPt * _scaleY;
            ty = 0;
            break;
        case 180:
            tx = pageWPt * _scaleX;
            ty = pageHPt * _scaleY;
            break;
        case 270:
            tx = 0;
            ty = pageWPt * _scaleX;
            break;
        }

        _rotMatrix = D2D1::Matrix3x2F(
            (float)cosA, (float)sinA,
            (float)-sinA, (float)cosA,
            (float)tx, (float)ty
        );
    }

    // ============================================
    // GET BUFFER
    // ============================================

    bool PdfPainterGPU::hasEndDrawError() const
    {
        return _endDrawFailed;
    }

    std::vector<uint8_t> PdfPainterGPU::getBuffer()
    {
        std::vector<uint8_t> result;
        if (!_wicBitmap) return result;

        // Flush any pending batches
        if (_hasGlyphBatch)
            flushGlyphBatch();

        // Make sure drawing is finished
        if (_inDraw) endDraw();

        IWICBitmapLock* lock = nullptr;
        WICRect rect = { 0, 0, _w, _h };

        HRESULT hr = _wicBitmap->Lock(&rect, WICBitmapLockRead, &lock);
        if (FAILED(hr)) return result;

        UINT stride = 0;
        UINT bufferSize = 0;
        BYTE* data = nullptr;

        lock->GetStride(&stride);
        lock->GetDataPointer(&bufferSize, &data);

        if (data && bufferSize > 0)
        {
            size_t rowBytes = (size_t)_w * 4;
            result.resize(rowBytes * _h);

            if ((size_t)stride == rowBytes)
            {
                // Contiguous layout (WICBitmapCacheOnLoad) — single fast memcpy
                std::memcpy(result.data(), data, rowBytes * _h);
            }
            else
            {
                // Non-contiguous layout — row-by-row copy (stride > rowBytes)
                for (int y = 0; y < _h; ++y)
                {
                    memcpy(result.data() + y * rowBytes, data + y * stride, rowBytes);
                }
            }
        }

        lock->Release();
        return result;
    }

    // ============================================
    // GET BUFFER DIRECT (zero-copy to caller buffer)
    // ============================================

    bool PdfPainterGPU::getBufferDirect(uint8_t* outBuffer, int outBufferSize)
    {
        if (!_wicBitmap || !outBuffer) return false;

        const int required = _w * _h * 4;
        if (outBufferSize < required) return false;

        // Flush any pending batches
        if (_hasGlyphBatch)
            flushGlyphBatch();

        // Make sure drawing is finished
        if (_inDraw) endDraw();

        IWICBitmapLock* lock = nullptr;
        WICRect rect = { 0, 0, _w, _h };

        HRESULT hr = _wicBitmap->Lock(&rect, WICBitmapLockRead, &lock);
        if (FAILED(hr)) return false;

        UINT stride = 0;
        UINT bufferSize = 0;
        BYTE* data = nullptr;

        lock->GetStride(&stride);
        lock->GetDataPointer(&bufferSize, &data);

        bool ok = false;
        if (data && bufferSize > 0)
        {
            size_t rowBytes = (size_t)_w * 4;
            if ((size_t)stride == rowBytes)
            {
                std::memcpy(outBuffer, data, rowBytes * _h);
            }
            else
            {
                for (int y = 0; y < _h; ++y)
                    memcpy(outBuffer + y * rowBytes, data + y * stride, rowBytes);
            }
            ok = true;
        }

        lock->Release();
        return ok;
    }

    // ============================================
    // TEXT BLOCK BATCHING (MuPDF-style)
    // ============================================

    void PdfPainterGPU::beginTextBlock()
    {
        // Enter text block mode - all glyphs until endTextBlock will be batched
        _inTextBlock = true;
    }

    void PdfPainterGPU::endTextBlock()
    {
        if (!_inTextBlock) return;

        // Flush all batched glyphs from this text block
        flushGlyphBatch();

        _inTextBlock = false;
    }

    // ============================================
    // CLIPPING LAYER STACK (for Form XObjects)
    // ============================================

    void PdfPainterGPU::pushClipPath(const std::vector<PdfPathSegment>& clipPath, const PdfMatrix& clipCTM, bool evenOdd)
    {
        if (!_renderTarget || clipPath.empty()) return;

        // Flush any pending operations before changing clip state
        flushFillBatch();
        flushGlyphBatch();

        bool wasInDraw = _inDraw;
        if (!_inDraw) beginDraw();

        // Create geometry from clip path (implicitClose=true: clips use filled region)
        ID2D1PathGeometry* geometry = createPathGeometry(clipPath, clipCTM, evenOdd, true);
        if (!geometry) return;

        // Create and push layer
        ID2D1Layer* layer = nullptr;
        HRESULT hr = _renderTarget->CreateLayer(&layer);
        if (FAILED(hr) || !layer) {
            geometry->Release();
            return;
        }

        _renderTarget->PushLayer(
            D2D1::LayerParameters(D2D1::InfiniteRect(), geometry),
            layer
        );

        // Save to stack for later pop
        ClipLayerInfo info;
        info.layer = layer;
        info.geometry = geometry;
        _clipLayerStack.push(info);
    }

    void PdfPainterGPU::popClipPath()
    {
        if (!_renderTarget || _clipLayerStack.empty()) return;

        // Flush any pending operations before changing clip state
        flushFillBatch();
        flushGlyphBatch();

        // Pop the layer
        _renderTarget->PopLayer();

        // Release resources
        ClipLayerInfo info = _clipLayerStack.top();
        _clipLayerStack.pop();

        if (info.layer) info.layer->Release();
        if (info.geometry) info.geometry->Release();
    }

    // ============================================
    // SOFT MASK (SMask) - D2D Layer with Opacity Bitmap
    // ============================================

    void PdfPainterGPU::pushSoftMask(const std::vector<uint8_t>& maskAlpha, int maskW, int maskH)
    {
        if (!_renderTarget || maskAlpha.empty() || maskW <= 0 || maskH <= 0) return;

        // Flush any pending operations before changing mask state
        flushFillBatch();
        flushGlyphBatch();

        bool wasInDraw = _inDraw;
        if (!_inDraw) beginDraw();

        // Convert grayscale alpha mask to BGRA premultiplied bitmap
        // Each pixel: (R,G,B) = (alpha, alpha, alpha), A = alpha
        // This creates a luminosity-based opacity mask
        size_t pixelCount = (size_t)maskW * (size_t)maskH;
        std::vector<uint8_t> bgraMask(pixelCount * 4);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            uint8_t a = maskAlpha[i];
            // Premultiplied alpha: RGB = alpha value (white * alpha)
            bgraMask[i * 4 + 0] = a;  // B
            bgraMask[i * 4 + 1] = a;  // G
            bgraMask[i * 4 + 2] = a;  // R
            bgraMask[i * 4 + 3] = a;  // A
        }

        // Create D2D bitmap for the opacity mask
        D2D1_BITMAP_PROPERTIES bmpProps = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
        );

        ID2D1Bitmap* maskBitmap = nullptr;
        HRESULT hr = _renderTarget->CreateBitmap(
            D2D1::SizeU(maskW, maskH),
            bgraMask.data(),
            maskW * 4,
            bmpProps,
            &maskBitmap
        );

        if (FAILED(hr) || !maskBitmap)
        {
            LogDebug("pushSoftMask: Failed to create mask bitmap (hr=0x%08x)", hr);
            return;
        }

        // Create a BitmapBrush from the mask bitmap (D2D PushLayer needs a brush, not a bitmap)
        ID2D1BitmapBrush* maskBrush = nullptr;
        hr = _renderTarget->CreateBitmapBrush(
            maskBitmap,
            D2D1::BitmapBrushProperties(
                D2D1_EXTEND_MODE_CLAMP,
                D2D1_EXTEND_MODE_CLAMP
            ),
            &maskBrush
        );
        if (FAILED(hr) || !maskBrush)
        {
            LogDebug("pushSoftMask: Failed to create bitmap brush (hr=0x%08x)", hr);
            maskBitmap->Release();
            return;
        }

        // Create D2D layer
        ID2D1Layer* layer = nullptr;
        hr = _renderTarget->CreateLayer(&layer);
        if (FAILED(hr) || !layer)
        {
            LogDebug("pushSoftMask: Failed to create layer (hr=0x%08x)", hr);
            maskBrush->Release();
            maskBitmap->Release();
            return;
        }

        // Push layer with opacity mask brush
        // The mask brush acts as per-pixel opacity for everything drawn within this layer
        _renderTarget->PushLayer(
            D2D1::LayerParameters(
                D2D1::InfiniteRect(),    // content bounds
                nullptr,                  // no geometric clip
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                D2D1::IdentityMatrix(),  // mask transform
                1.0f,                    // opacity
                maskBrush,               // opacity mask (BitmapBrush)
                D2D1_LAYER_OPTIONS_NONE
            ),
            layer
        );

        // Save to stack for later pop
        SoftMaskLayerInfo info;
        info.layer = layer;
        info.maskBitmap = maskBitmap;
        info.maskBrush = maskBrush;
        _softMaskLayerStack.push(info);

        LogDebug("pushSoftMask: Pushed mask %dx%d (stack depth: %zu)", maskW, maskH, _softMaskLayerStack.size());
    }

    void PdfPainterGPU::popSoftMask()
    {
        if (!_renderTarget || _softMaskLayerStack.empty()) return;

        // Flush any pending operations before changing mask state
        flushFillBatch();
        flushGlyphBatch();

        // Pop the layer
        _renderTarget->PopLayer();

        // Release resources
        SoftMaskLayerInfo info = _softMaskLayerStack.top();
        _softMaskLayerStack.pop();

        if (info.layer) info.layer->Release();
        if (info.maskBrush) info.maskBrush->Release();
        if (info.maskBitmap) info.maskBitmap->Release();

        LogDebug("popSoftMask: Popped (stack depth: %zu)", _softMaskLayerStack.size());
    }

    // ============================================
    // PAGE RENDERING LIFECYCLE - BATCHING
    // ============================================

    void PdfPainterGPU::beginPage()
    {
        if (!_renderTarget) return;

        _inPageRender = true;
        _inTextBlock = false;  // Reset text block state
        _fillBatch.clear();
        _hasBatchedFills = false;
        _batchColor = 0;

        // Clear glyph batch
        _glyphBatch.clear();
        _hasGlyphBatch = false;
        _glyphBatchMinX = FLT_MAX;
        _glyphBatchMinY = FLT_MAX;
        _glyphBatchMaxX = -FLT_MAX;
        _glyphBatchMaxY = -FLT_MAX;

        // Start D2D draw session for entire page
        if (!_inDraw)
            beginDraw();
    }

    void PdfPainterGPU::endPage()
    {
        if (!_renderTarget) return;

        // End any open text block
        if (_inTextBlock)
            endTextBlock();

        // Flush any remaining batched fills
        flushFillBatch();

        // Flush any remaining glyph batch
        flushGlyphBatch();

        // Clear brush cache (brushes are render target specific)
        clearBrushCache();

        // End D2D draw session
        if (_inDraw)
            endDraw();

        _inPageRender = false;

        // Free batch memory to reduce memory pressure
        _fillBatch.clear();
        _fillBatch.shrink_to_fit();
        _glyphBatch.clear();
        _glyphBatch.shrink_to_fit();
    }

    void PdfPainterGPU::addToBatch(
        const std::vector<PdfPathSegment>& path,
        uint32_t color,
        const PdfMatrix& ctm,
        bool evenOdd)
    {
        // Safety check
        if (path.empty()) return;

        // If color OR fill mode changed, flush current batch
        // ✅ FIX: Different fill modes must be in separate batches!
        if (_hasBatchedFills && (color != _batchColor || evenOdd != _batchEvenOdd))
        {
            flushFillBatch();
        }

        // Add to batch
        BatchedFill bf;
        bf.path = path;
        bf.ctm = ctm;
        bf.evenOdd = evenOdd;
        _fillBatch.push_back(std::move(bf));

        _batchColor = color;
        _batchEvenOdd = evenOdd;  // ✅ Track fill mode
        _hasBatchedFills = true;

        // Flush if batch gets too large (increased limit for combined geometry)
        if (_fillBatch.size() >= 5000)
        {
            flushFillBatch();
        }
    }

    void PdfPainterGPU::flushFillBatch()
    {
        if (!_hasBatchedFills || _fillBatch.empty() || !_renderTarget || !s_d2dFactory)
            return;

        // ===== OPTIMIZATION: Create SINGLE combined geometry =====
        // Instead of creating separate geometries and grouping them,
        // we write all paths into a single PathGeometry sink.
        // This is MUCH faster for Microsoft Print to PDF files.

        ID2D1PathGeometry* combinedGeometry = nullptr;
        HRESULT hr = s_d2dFactory->CreatePathGeometry(&combinedGeometry);

        if (FAILED(hr) || !combinedGeometry)
        {
            _fillBatch.clear();
            _hasBatchedFills = false;
            return;
        }

        ID2D1GeometrySink* sink = nullptr;
        hr = combinedGeometry->Open(&sink);

        if (FAILED(hr) || !sink)
        {
            combinedGeometry->Release();
            _fillBatch.clear();
            _hasBatchedFills = false;
            return;
        }

        // Set fill mode based on batch setting
        // ✅ FIX: Use the correct fill mode for this batch
        sink->SetFillMode(_batchEvenOdd ? D2D1_FILL_MODE_ALTERNATE : D2D1_FILL_MODE_WINDING);

        // Helper lambda for safe coordinate transformation
        const double MAX_COORD = 1e7;
        auto safeCoord = [this, MAX_COORD](double val) -> float {
            if (!std::isfinite(val)) return 0.0f;
            val = std::max(-MAX_COORD, std::min(MAX_COORD, val));
            return (float)val;
            };

        // Write ALL paths into the same sink
        for (const auto& bf : _fillBatch)
        {
            bool figureStarted = false;

            for (const auto& seg : bf.path)
            {
                // Transform point with safety checks
                double tx = bf.ctm.a * seg.x + bf.ctm.c * seg.y + bf.ctm.e;
                double ty = bf.ctm.b * seg.x + bf.ctm.d * seg.y + bf.ctm.f;
                tx *= _scaleX;
                ty = _h - ty * _scaleY;

                float fx = safeCoord(tx);
                float fy = safeCoord(ty);

                switch (seg.type)
                {
                case PdfPathSegment::MoveTo:
                    if (figureStarted)
                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);  // PDF spec: implicit close for fills
                    sink->BeginFigure(D2D1::Point2F(fx, fy), D2D1_FIGURE_BEGIN_FILLED);
                    figureStarted = true;
                    break;

                case PdfPathSegment::LineTo:
                    if (figureStarted)
                        sink->AddLine(D2D1::Point2F(fx, fy));
                    break;

                case PdfPathSegment::CurveTo:
                {
                    if (figureStarted)
                    {
                        double tx1 = bf.ctm.a * seg.x1 + bf.ctm.c * seg.y1 + bf.ctm.e;
                        double ty1 = bf.ctm.b * seg.x1 + bf.ctm.d * seg.y1 + bf.ctm.f;
                        tx1 *= _scaleX;
                        ty1 = _h - ty1 * _scaleY;

                        double tx2 = bf.ctm.a * seg.x2 + bf.ctm.c * seg.y2 + bf.ctm.e;
                        double ty2 = bf.ctm.b * seg.x2 + bf.ctm.d * seg.y2 + bf.ctm.f;
                        tx2 *= _scaleX;
                        ty2 = _h - ty2 * _scaleY;

                        double tx3 = bf.ctm.a * seg.x3 + bf.ctm.c * seg.y3 + bf.ctm.e;
                        double ty3 = bf.ctm.b * seg.x3 + bf.ctm.d * seg.y3 + bf.ctm.f;
                        tx3 *= _scaleX;
                        ty3 = _h - ty3 * _scaleY;

                        sink->AddBezier(D2D1::BezierSegment(
                            D2D1::Point2F(safeCoord(tx1), safeCoord(ty1)),
                            D2D1::Point2F(safeCoord(tx2), safeCoord(ty2)),
                            D2D1::Point2F(safeCoord(tx3), safeCoord(ty3))
                        ));
                    }
                    break;
                }

                case PdfPathSegment::Close:
                    if (figureStarted)
                    {
                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                        figureStarted = false;
                    }
                    break;
                }
            }

            // Close any open figure (PDF spec: implicit close for fills)
            if (figureStarted)
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        }

        hr = sink->Close();
        sink->Release();

        if (SUCCEEDED(hr) && _renderTarget)
        {
            // Use cached brush for better performance
            ID2D1SolidColorBrush* brush = getCachedBrush(_batchColor);

            if (brush)
            {
                // Render ENTIRE batch with SINGLE call!
                _renderTarget->FillGeometry(combinedGeometry, brush);
            }
        }

        combinedGeometry->Release();

        // Clear batch
        _fillBatch.clear();
        _hasBatchedFills = false;
    }

    // ============================================
    // HIGH QUALITY IMAGE DRAWING
    // ============================================

    void PdfPainterGPU::drawBitmapHighQuality(ID2D1Bitmap* bitmap, const D2D1_RECT_F& destRect, float opacity)
    {
        if (!bitmap) return;

        if (_deviceContext)
        {
            // Use ID2D1DeviceContext::DrawBitmap with high-quality cubic interpolation
            _deviceContext->DrawBitmap(
                bitmap,
                destRect,
                opacity,
                D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                nullptr  // source rect (null = entire bitmap)
            );
        }
        else
        {
            // Fallback: use ID2D1RenderTarget::DrawBitmap with bilinear
            _renderTarget->DrawBitmap(bitmap, destRect, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
    }

    // ============================================
    // BRUSH CACHING
    // ============================================

    ID2D1SolidColorBrush* PdfPainterGPU::getCachedBrush(uint32_t color)
    {
        if (!_renderTarget) return nullptr;

        auto it = _brushCache.find(color);
        if (it != _brushCache.end())
            return it->second;

        // Create new brush and cache it
        ID2D1SolidColorBrush* brush = nullptr;
        HRESULT hr = _renderTarget->CreateSolidColorBrush(toD2DColor(color), &brush);

        if (SUCCEEDED(hr) && brush)
        {
            // Limit cache size to prevent memory bloat
            if (_brushCache.size() > 100)
            {
                clearBrushCache();
            }
            _brushCache[color] = brush;
            return brush;
        }
        return nullptr;
    }

    void PdfPainterGPU::clearBrushCache()
    {
        for (auto& kv : _brushCache)
        {
            if (kv.second)
                kv.second->Release();
        }
        _brushCache.clear();
    }

} // namespace pdf