#include "pch.h"
#include "PdfPainterD2D.h"
#include "PdfDocument.h"
#include <algorithm>
#include <cmath>
#include <cwchar>

namespace pdf
{
    // ============================================
    // CONSTRUCTOR / DESTRUCTOR
    // ============================================

    PdfPainterD2D::PdfPainterD2D(int width, int height, double scaleX, double scaleY)
        : _w(width), _h(height), _scaleX(scaleX), _scaleY(scaleY)
    {
    }

    PdfPainterD2D::~PdfPainterD2D()
    {
        // Release font faces
        for (auto& kv : _fontFaceCache)
        {
            if (kv.second) kv.second->Release();
        }
        _fontFaceCache.clear();

        // Release clip layers
        while (!_clipLayers.empty())
        {
            if (_clipLayers.top()) _clipLayers.top()->Release();
            _clipLayers.pop();
        }

        if (_renderTarget) _renderTarget->Release();
        if (_wicBitmap) _wicBitmap->Release();
        if (_d2dFactory) _d2dFactory->Release();
        if (_wicFactory) _wicFactory->Release();
        if (_dwFactory) _dwFactory->Release();
    }

    // ============================================
    // INITIALIZATION
    // ============================================

    bool PdfPainterD2D::initialize()
    {
        if (_initialized) return true;

        HRESULT hr;

        // Create D2D Factory (use ID2D1Factory1 for better features)
        D2D1_FACTORY_OPTIONS options = {};
#ifdef _DEBUG
        options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
        hr = D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory1),
            &options,
            reinterpret_cast<void**>(&_d2dFactory)
        );
        if (FAILED(hr))
        {
            // Fallback to ID2D1Factory
            hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                reinterpret_cast<ID2D1Factory**>(&_d2dFactory));
            if (FAILED(hr)) return false;
        }

        // Create WIC Factory
        hr = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&_wicFactory)
        );
        if (FAILED(hr)) return false;

        // Create DWrite Factory
        hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(&_dwFactory)
        );
        if (FAILED(hr)) return false;

        // Create WIC Bitmap
        hr = _wicFactory->CreateBitmap(
            _w, _h,
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapCacheOnDemand,
            &_wicBitmap
        );
        if (FAILED(hr)) return false;

        // Render target properties - use DEFAULT for GPU acceleration
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,  // Let D2D choose (prefers GPU)
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f
        );

        // Create WIC Render Target
        hr = _d2dFactory->CreateWicBitmapRenderTarget(
            _wicBitmap,
            &props,
            &_renderTarget
        );

        if (FAILED(hr))
        {
            // Fallback to software
            props.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
            hr = _d2dFactory->CreateWicBitmapRenderTarget(_wicBitmap, &props, &_renderTarget);
            if (FAILED(hr)) return false;
        }

        // Enable antialiasing
        _renderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        _renderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

        _initialized = true;
        return true;
    }

    // ============================================
    // DRAW CONTROL
    // ============================================

    void PdfPainterD2D::beginDraw()
    {
        if (!_renderTarget || _inDraw) return;
        _renderTarget->BeginDraw();
        _inDraw = true;
    }

    void PdfPainterD2D::endDraw()
    {
        if (!_renderTarget || !_inDraw) return;
        _renderTarget->EndDraw();
        _inDraw = false;
    }

    void PdfPainterD2D::clear(uint32_t bgraColor)
    {
        if (!_renderTarget) return;

        bool wasInDraw = _inDraw;
        if (!_inDraw) beginDraw();

        _renderTarget->Clear(toD2DColor(bgraColor));

        if (!wasInDraw) endDraw();
    }

    // ============================================
    // HELPER FUNCTIONS
    // ============================================

    D2D1_MATRIX_3X2_F PdfPainterD2D::toD2DMatrix(const PdfMatrix& m) const
    {
        return D2D1::Matrix3x2F(
            (float)(m.a * _scaleX),
            (float)(m.b * _scaleX),
            (float)(m.c * _scaleY),
            (float)(-m.d * _scaleY),
            (float)(m.e * _scaleX),
            (float)(_h - m.f * _scaleY)
        );
    }

    D2D1_COLOR_F PdfPainterD2D::toD2DColor(uint32_t argb) const
    {
        float a = ((argb >> 24) & 0xFF) / 255.0f;
        float r = ((argb >> 16) & 0xFF) / 255.0f;
        float g = ((argb >> 8) & 0xFF) / 255.0f;
        float b = (argb & 0xFF) / 255.0f;
        return D2D1::ColorF(r, g, b, a);
    }

    D2D1_POINT_2F PdfPainterD2D::transformPoint(double x, double y, const PdfMatrix& ctm) const
    {
        double tx = ctm.a * x + ctm.c * y + ctm.e;
        double ty = ctm.b * x + ctm.d * y + ctm.f;
        tx *= _scaleX;
        ty = _h - ty * _scaleY;
        return D2D1::Point2F((float)tx, (float)ty);
    }

    ID2D1PathGeometry* PdfPainterD2D::createPathGeometry(
        const std::vector<PdfPathSegment>& path,
        const PdfMatrix& ctm)
    {
        if (!_d2dFactory) return nullptr;

        ID2D1PathGeometry* geometry = nullptr;
        HRESULT hr = _d2dFactory->CreatePathGeometry(&geometry);
        if (FAILED(hr)) return nullptr;

        ID2D1GeometrySink* sink = nullptr;
        hr = geometry->Open(&sink);
        if (FAILED(hr))
        {
            geometry->Release();
            return nullptr;
        }

        bool figureStarted = false;
        D2D1_POINT_2F startPoint = {};

        for (const auto& seg : path)
        {
            switch (seg.type)
            {
            case PdfPathSegment::MoveTo:
                if (figureStarted)
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                startPoint = transformPoint(seg.x, seg.y, ctm);
                sink->BeginFigure(startPoint, D2D1_FIGURE_BEGIN_FILLED);
                figureStarted = true;
                break;

            case PdfPathSegment::LineTo:
                if (figureStarted)
                    sink->AddLine(transformPoint(seg.x, seg.y, ctm));
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
            sink->EndFigure(D2D1_FIGURE_END_OPEN);

        sink->Close();
        sink->Release();

        return geometry;
    }

    // ============================================
    // PATH OPERATIONS
    // ============================================

    void PdfPainterD2D::fillPath(
        const std::vector<PdfPathSegment>& path,
        uint32_t color,
        const PdfMatrix& ctm,
        bool evenOdd)
    {
        if (!_renderTarget || path.empty()) return;

        ID2D1PathGeometry* geometry = createPathGeometry(path, ctm);
        if (!geometry) return;

        ID2D1SolidColorBrush* brush = nullptr;
        HRESULT hr = _renderTarget->CreateSolidColorBrush(toD2DColor(color), &brush);

        if (SUCCEEDED(hr))
        {
            bool wasInDraw = _inDraw;
            if (!_inDraw) beginDraw();

            _renderTarget->FillGeometry(geometry, brush);

            if (!wasInDraw) endDraw();
            brush->Release();
        }

        geometry->Release();
    }

    void PdfPainterD2D::strokePath(
        const std::vector<PdfPathSegment>& path,
        uint32_t color,
        double lineWidth,
        const PdfMatrix& ctm,
        int lineCap,
        int lineJoin,
        double miterLimit,
        const std::vector<double>& dashArray,
        double dashPhase)
    {
        if (!_renderTarget || path.empty()) return;

        ID2D1PathGeometry* geometry = createPathGeometry(path, ctm);
        if (!geometry) return;

        ID2D1SolidColorBrush* brush = nullptr;
        HRESULT hr = _renderTarget->CreateSolidColorBrush(toD2DColor(color), &brush);

        if (SUCCEEDED(hr))
        {
            ID2D1StrokeStyle* strokeStyle = nullptr;

            D2D1_STROKE_STYLE_PROPERTIES strokeProps = {};
            strokeProps.startCap = (D2D1_CAP_STYLE)lineCap;
            strokeProps.endCap = (D2D1_CAP_STYLE)lineCap;
            strokeProps.dashCap = (D2D1_CAP_STYLE)lineCap;
            strokeProps.lineJoin = (D2D1_LINE_JOIN)lineJoin;
            strokeProps.miterLimit = (float)miterLimit;
            strokeProps.dashStyle = dashArray.empty() ? D2D1_DASH_STYLE_SOLID : D2D1_DASH_STYLE_CUSTOM;
            strokeProps.dashOffset = (float)dashPhase;

            if (!dashArray.empty())
            {
                std::vector<float> dashes(dashArray.size());
                for (size_t i = 0; i < dashArray.size(); ++i)
                    dashes[i] = (float)dashArray[i];

                _d2dFactory->CreateStrokeStyle(
                    &strokeProps,
                    dashes.data(),
                    (UINT32)dashes.size(),
                    &strokeStyle
                );
            }
            else
            {
                _d2dFactory->CreateStrokeStyle(&strokeProps, nullptr, 0, &strokeStyle);
            }

            bool wasInDraw = _inDraw;
            if (!_inDraw) beginDraw();

            float scaledWidth = (float)(lineWidth * _scaleX);
            if (scaledWidth < 0.5f) scaledWidth = 0.5f;

            _renderTarget->DrawGeometry(geometry, brush, scaledWidth, strokeStyle);

            if (!wasInDraw) endDraw();

            if (strokeStyle) strokeStyle->Release();
            brush->Release();
        }

        geometry->Release();
    }

    // ============================================
    // TEXT RENDERING (DirectWrite)
    // ============================================

    IDWriteFontFace* PdfPainterD2D::getOrCreateFontFace(const PdfFontInfo* font)
    {
        if (!font || !_dwFactory) return nullptr;

        // Check cache
        auto it = _fontFaceCache.find(font->baseFont);
        if (it != _fontFaceCache.end())
            return it->second;

        // Create font face from font program
        if (font->fontProgram.empty())
            return nullptr;

        // Create custom font collection loader (simplified - use system font fallback)
        IDWriteFontFile* fontFile = nullptr;

        // For embedded fonts, we need to create a custom font file loader
        // This is complex, so we'll use system font fallback for now
        IDWriteFontFace* fontFace = nullptr;

        // Try to find matching system font
        IDWriteFontCollection* fontCollection = nullptr;
        HRESULT hr = _dwFactory->GetSystemFontCollection(&fontCollection);

        if (SUCCEEDED(hr) && fontCollection)
        {
            // Convert font name to wide string
            std::wstring fontName(font->baseFont.begin(), font->baseFont.end());

            // Remove common prefixes
            size_t plusPos = fontName.find(L'+');
            if (plusPos != std::wstring::npos && plusPos < fontName.length() - 1)
                fontName = fontName.substr(plusPos + 1);

            // Try to find the font family
            UINT32 familyIndex = 0;
            BOOL exists = FALSE;
            fontCollection->FindFamilyName(fontName.c_str(), &familyIndex, &exists);

            if (!exists)
            {
                // Try common mappings
                if (fontName.find(L"Arial") != std::wstring::npos ||
                    fontName.find(L"Helvetica") != std::wstring::npos)
                    fontCollection->FindFamilyName(L"Arial", &familyIndex, &exists);
                else if (fontName.find(L"Times") != std::wstring::npos)
                    fontCollection->FindFamilyName(L"Times New Roman", &familyIndex, &exists);
                else if (fontName.find(L"Courier") != std::wstring::npos)
                    fontCollection->FindFamilyName(L"Courier New", &familyIndex, &exists);
                else
                    fontCollection->FindFamilyName(L"Segoe UI", &familyIndex, &exists);
            }

            if (exists)
            {
                IDWriteFontFamily* fontFamily = nullptr;
                hr = fontCollection->GetFontFamily(familyIndex, &fontFamily);

                if (SUCCEEDED(hr) && fontFamily)
                {
                    IDWriteFont* dwFont = nullptr;

                    // Determine weight and style
                    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
                    DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL;

                    if (fontName.find(L"Bold") != std::wstring::npos)
                        weight = DWRITE_FONT_WEIGHT_BOLD;
                    if (fontName.find(L"Italic") != std::wstring::npos ||
                        fontName.find(L"Oblique") != std::wstring::npos)
                        style = DWRITE_FONT_STYLE_ITALIC;

                    hr = fontFamily->GetFirstMatchingFont(weight, DWRITE_FONT_STRETCH_NORMAL, style, &dwFont);

                    if (SUCCEEDED(hr) && dwFont)
                    {
                        dwFont->CreateFontFace(&fontFace);
                        dwFont->Release();
                    }

                    fontFamily->Release();
                }
            }

            fontCollection->Release();
        }

        // Cache it (even if null)
        _fontFaceCache[font->baseFont] = fontFace;
        return fontFace;
    }

    double PdfPainterD2D::drawText(
        const std::string& text,
        double x, double y,
        const PdfFontInfo* font,
        double fontSize,
        uint32_t color,
        const PdfMatrix& ctm,
        double charSpacing,
        double wordSpacing,
        double horizScale)
    {
        std::vector<uint8_t> raw(text.begin(), text.end());
        return drawTextRaw(raw, x, y, font, fontSize, color, ctm, charSpacing, wordSpacing, horizScale);
    }

    double PdfPainterD2D::drawTextRaw(
        const std::vector<uint8_t>& raw,
        double x, double y,
        const PdfFontInfo* font,
        double fontSize,
        uint32_t color,
        const PdfMatrix& ctm,
        double charSpacing,
        double wordSpacing,
        double horizScale)
    {
        if (!_renderTarget || !_dwFactory || raw.empty()) return 0;
        if (!font) return 0;

        // Get or create DirectWrite font face
        IDWriteFontFace* fontFace = getOrCreateFontFace(font);

        // Create text format as fallback
        IDWriteTextFormat* textFormat = nullptr;
        std::wstring fontFamily = L"Segoe UI";

        // Map font name
        if (font->baseFont.find("Arial") != std::string::npos ||
            font->baseFont.find("Helvetica") != std::string::npos)
            fontFamily = L"Arial";
        else if (font->baseFont.find("Times") != std::string::npos)
            fontFamily = L"Times New Roman";
        else if (font->baseFont.find("Courier") != std::string::npos)
            fontFamily = L"Courier New";

        DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
        DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL;

        if (font->baseFont.find("Bold") != std::string::npos)
            weight = DWRITE_FONT_WEIGHT_BOLD;
        if (font->baseFont.find("Italic") != std::string::npos ||
            font->baseFont.find("Oblique") != std::string::npos)
            style = DWRITE_FONT_STYLE_ITALIC;

        float fontSizePx = (float)(fontSize * _scaleY);

        HRESULT hr = _dwFactory->CreateTextFormat(
            fontFamily.c_str(),
            nullptr,
            weight,
            style,
            DWRITE_FONT_STRETCH_NORMAL,
            fontSizePx,
            L"",
            &textFormat
        );

        if (FAILED(hr)) return 0;

        // Convert raw bytes to unicode string
        std::wstring wtext;
        wtext.reserve(raw.size());

        for (size_t i = 0; i < raw.size(); ++i)
        {
            uint32_t unicode = 0;

            if (font->isCidFont && i + 1 < raw.size())
            {
                // CID font: 2-byte codes
                int cid = ((unsigned char)raw[i] << 8) | (unsigned char)raw[i + 1];
                auto it = font->cidToUnicode.find((uint16_t)cid);
                if (it != font->cidToUnicode.end())
                    unicode = it->second;
                else
                    unicode = cid;
                ++i;  // Skip second byte
            }
            else
            {
                // Simple font
                unsigned char c = raw[i];
                if (font->hasSimpleMap && font->codeToUnicode[c] != 0)
                    unicode = font->codeToUnicode[c];
                else
                    unicode = c;
            }

            if (unicode > 0 && unicode < 0x10000)
                wtext.push_back((wchar_t)unicode);
            else if (unicode >= 0x10000)
            {
                // Surrogate pair
                unicode -= 0x10000;
                wtext.push_back((wchar_t)(0xD800 + (unicode >> 10)));
                wtext.push_back((wchar_t)(0xDC00 + (unicode & 0x3FF)));
            }
        }

        if (wtext.empty())
        {
            textFormat->Release();
            return 0;
        }

        // Create text layout
        IDWriteTextLayout* textLayout = nullptr;
        hr = _dwFactory->CreateTextLayout(
            wtext.c_str(),
            (UINT32)wtext.length(),
            textFormat,
            10000.0f,  // Max width
            10000.0f,  // Max height
            &textLayout
        );

        if (FAILED(hr))
        {
            textFormat->Release();
            return 0;
        }

        // Apply character spacing
        if (charSpacing != 0)
        {
            DWRITE_TEXT_RANGE range = { 0, (UINT32)wtext.length() };
            // Character spacing is tricky in DirectWrite, approximate with letter spacing
        }

        // Get text metrics for advance width
        DWRITE_TEXT_METRICS metrics = {};
        textLayout->GetMetrics(&metrics);

        // Transform position
        D2D1_POINT_2F pos = transformPoint(x, y, ctm);

        // Create brush
        ID2D1SolidColorBrush* brush = nullptr;
        hr = _renderTarget->CreateSolidColorBrush(toD2DColor(color), &brush);

        if (SUCCEEDED(hr))
        {
            bool wasInDraw = _inDraw;
            if (!_inDraw) beginDraw();

            // Draw text
            _renderTarget->DrawTextLayout(
                D2D1::Point2F(pos.x, pos.y - fontSizePx),
                textLayout,
                brush
            );

            if (!wasInDraw) endDraw();
            brush->Release();
        }

        textLayout->Release();
        textFormat->Release();

        return metrics.width / _scaleX;
    }

    // ============================================
    // IMAGE RENDERING
    // ============================================

    void PdfPainterD2D::drawImage(
        const std::vector<uint8_t>& argb,
        int imgW, int imgH,
        const PdfMatrix& ctm)
    {
        if (!_renderTarget || !_wicFactory || argb.empty()) return;
        if (argb.size() < (size_t)imgW * imgH * 4) return;

        IWICBitmap* wicBitmap = nullptr;
        HRESULT hr = _wicFactory->CreateBitmapFromMemory(
            imgW, imgH,
            GUID_WICPixelFormat32bppBGRA,
            imgW * 4,
            (UINT)argb.size(),
            (BYTE*)argb.data(),
            &wicBitmap
        );

        if (FAILED(hr)) return;

        ID2D1Bitmap* d2dBitmap = nullptr;
        hr = _renderTarget->CreateBitmapFromWicBitmap(wicBitmap, &d2dBitmap);
        wicBitmap->Release();

        if (FAILED(hr)) return;

        bool wasInDraw = _inDraw;
        if (!_inDraw) beginDraw();

        // Calculate destination rectangle based on CTM
        D2D1_POINT_2F p0 = transformPoint(0, 0, ctm);
        D2D1_POINT_2F p1 = transformPoint(1, 0, ctm);
        D2D1_POINT_2F p2 = transformPoint(0, 1, ctm);
        D2D1_POINT_2F p3 = transformPoint(1, 1, ctm);

        float minX = (std::min)({ p0.x, p1.x, p2.x, p3.x });
        float maxX = (std::max)({ p0.x, p1.x, p2.x, p3.x });
        float minY = (std::min)({ p0.y, p1.y, p2.y, p3.y });
        float maxY = (std::max)({ p0.y, p1.y, p2.y, p3.y });

        D2D1_RECT_F destRect = D2D1::RectF(minX, minY, maxX, maxY);

        _renderTarget->DrawBitmap(
            d2dBitmap,
            destRect,
            1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR
        );

        if (!wasInDraw) endDraw();

        d2dBitmap->Release();
    }

    void PdfPainterD2D::drawImageWithMask(
        const std::vector<uint8_t>& argb,
        int imgW, int imgH,
        const std::vector<uint8_t>& mask,
        int maskW, int maskH,
        const PdfMatrix& ctm)
    {
        // For now, just draw without mask
        // Full implementation would use opacity mask
        drawImage(argb, imgW, imgH, ctm);
    }

    // ============================================
    // GRADIENT RENDERING
    // ============================================

    ID2D1Brush* PdfPainterD2D::createGradientBrush(const PdfGradient& gradient, const PdfMatrix& ctm)
    {
        if (!_renderTarget) return nullptr;

        // Create gradient stops
        std::vector<D2D1_GRADIENT_STOP> stops;
        for (size_t i = 0; i < gradient.stops.size(); ++i)
        {
            D2D1_GRADIENT_STOP stop;
            stop.position = (float)gradient.stops[i].position;
            // Convert rgb[3] (0.0-1.0) to D2D1_COLOR_F
            stop.color = D2D1::ColorF(
                (float)gradient.stops[i].rgb[0],
                (float)gradient.stops[i].rgb[1],
                (float)gradient.stops[i].rgb[2],
                1.0f
            );
            stops.push_back(stop);
        }

        if (stops.empty())
        {
            // Default stops
            stops.push_back({ 0.0f, D2D1::ColorF(0, 0, 0, 1) });
            stops.push_back({ 1.0f, D2D1::ColorF(1, 1, 1, 1) });
        }

        ID2D1GradientStopCollection* stopCollection = nullptr;
        HRESULT hr = _renderTarget->CreateGradientStopCollection(
            stops.data(),
            (UINT32)stops.size(),
            &stopCollection
        );

        if (FAILED(hr)) return nullptr;

        ID2D1Brush* brush = nullptr;

        if (gradient.type == 2)  // Axial/Linear gradient
        {
            D2D1_POINT_2F start = transformPoint(gradient.x0, gradient.y0, ctm);
            D2D1_POINT_2F end = transformPoint(gradient.x1, gradient.y1, ctm);

            ID2D1LinearGradientBrush* linearBrush = nullptr;
            hr = _renderTarget->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(start, end),
                stopCollection,
                &linearBrush
            );

            if (SUCCEEDED(hr))
                brush = linearBrush;
        }
        else if (gradient.type == 3)  // Radial gradient
        {
            D2D1_POINT_2F center = transformPoint(gradient.x1, gradient.y1, ctm);
            float radiusX = (float)(gradient.r1 * _scaleX);
            float radiusY = (float)(gradient.r1 * _scaleY);

            ID2D1RadialGradientBrush* radialBrush = nullptr;
            hr = _renderTarget->CreateRadialGradientBrush(
                D2D1::RadialGradientBrushProperties(center, D2D1::Point2F(0, 0), radiusX, radiusY),
                stopCollection,
                &radialBrush
            );

            if (SUCCEEDED(hr))
                brush = radialBrush;
        }

        stopCollection->Release();
        return brush;
    }

    void PdfPainterD2D::fillPathWithGradient(
        const std::vector<PdfPathSegment>& path,
        const PdfGradient& gradient,
        const PdfMatrix& ctm,
        bool evenOdd)
    {
        if (!_renderTarget || path.empty()) return;

        ID2D1PathGeometry* geometry = createPathGeometry(path, ctm);
        if (!geometry) return;

        ID2D1Brush* brush = createGradientBrush(gradient, ctm);
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
    // CLIPPING
    // ============================================

    void PdfPainterD2D::pushClip(
        const std::vector<PdfPathSegment>& path,
        const PdfMatrix& ctm,
        bool evenOdd)
    {
        if (!_renderTarget || path.empty()) return;

        ID2D1PathGeometry* geometry = createPathGeometry(path, ctm);
        if (!geometry) return;

        ID2D1Layer* layer = nullptr;
        HRESULT hr = _renderTarget->CreateLayer(&layer);

        if (SUCCEEDED(hr) && layer)
        {
            bool wasInDraw = _inDraw;
            if (!_inDraw) beginDraw();

            _renderTarget->PushLayer(
                D2D1::LayerParameters(
                    D2D1::InfiniteRect(),
                    geometry
                ),
                layer
            );

            _clipLayers.push(layer);

            if (!wasInDraw && _inDraw)
            {
                // Keep drawing for subsequent operations
            }
        }

        geometry->Release();
    }

    void PdfPainterD2D::popClip()
    {
        if (!_renderTarget || _clipLayers.empty()) return;

        bool wasInDraw = _inDraw;
        if (!_inDraw) beginDraw();

        _renderTarget->PopLayer();

        ID2D1Layer* layer = _clipLayers.top();
        _clipLayers.pop();
        if (layer) layer->Release();

        if (!wasInDraw) endDraw();
    }

    // ============================================
    // STATE MANAGEMENT
    // ============================================

    void PdfPainterD2D::saveState()
    {
        // Direct2D doesn't have explicit state save/restore
        // We handle this through clip layers
    }

    void PdfPainterD2D::restoreState()
    {
        // Pop any active clip layers
    }

    void PdfPainterD2D::setPageRotation(int degrees, double pageWPt, double pageHPt)
    {
        if (degrees == 0)
        {
            _hasRotate = false;
            return;
        }

        float w = (float)(pageWPt * _scaleX);
        float h = (float)(pageHPt * _scaleY);

        switch (degrees)
        {
        case 90:
            _rotMatrix = D2D1::Matrix3x2F::Rotation(90.0f, D2D1::Point2F(0, 0)) *
                D2D1::Matrix3x2F::Translation(h, 0);
            break;
        case 180:
            _rotMatrix = D2D1::Matrix3x2F::Rotation(180.0f, D2D1::Point2F(0, 0)) *
                D2D1::Matrix3x2F::Translation(w, h);
            break;
        case 270:
            _rotMatrix = D2D1::Matrix3x2F::Rotation(270.0f, D2D1::Point2F(0, 0)) *
                D2D1::Matrix3x2F::Translation(0, w);
            break;
        }

        _hasRotate = true;
    }

    // ============================================
    // OUTPUT
    // ============================================

    std::vector<uint8_t> PdfPainterD2D::getBuffer()
    {
        if (!_wicBitmap) return {};

        // Make sure drawing is complete
        if (_inDraw) endDraw();
        if (_renderTarget) _renderTarget->Flush();

        std::vector<uint8_t> buffer(_w * _h * 4);

        WICRect rect = { 0, 0, _w, _h };
        IWICBitmapLock* lock = nullptr;
        HRESULT hr = _wicBitmap->Lock(&rect, WICBitmapLockRead, &lock);

        if (SUCCEEDED(hr))
        {
            UINT stride = 0;
            UINT bufferSize = 0;
            BYTE* data = nullptr;

            lock->GetStride(&stride);
            lock->GetDataPointer(&bufferSize, &data);

            for (int y = 0; y < _h; ++y)
            {
                memcpy(
                    buffer.data() + y * _w * 4,
                    data + y * stride,
                    _w * 4
                );
            }

            lock->Release();
        }

        return buffer;
    }

    bool PdfPainterD2D::uploadFromCPUBuffer(const std::vector<uint8_t>& cpuBuffer)
    {
        if (!_wicBitmap) return false;
        if (cpuBuffer.size() < (size_t)_w * _h * 4) return false;

        WICRect rect = { 0, 0, _w, _h };
        IWICBitmapLock* lock = nullptr;
        HRESULT hr = _wicBitmap->Lock(&rect, WICBitmapLockWrite, &lock);

        if (FAILED(hr)) return false;

        UINT stride = 0;
        UINT bufferSize = 0;
        BYTE* data = nullptr;

        lock->GetStride(&stride);
        lock->GetDataPointer(&bufferSize, &data);

        for (int y = 0; y < _h; ++y)
        {
            memcpy(
                data + y * stride,
                cpuBuffer.data() + y * _w * 4,
                _w * 4
            );
        }

        lock->Release();
        return true;
    }

} // namespace pdf
