#include "pch.h"

// Suppress MSVC secure function warnings
#ifdef _MSC_VER
#pragma warning(disable: 4996)
#endif

#include "PdfPainter.h"
#include "PdfPath.h"
#include "PdfDebug.h"
#include "PdfGraphicsState.h"
#include "PdfContentParser.h"
#include "GlyphCache.h"
#include "FontCache.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <ft2build.h>
#include FT_FREETYPE_H

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923
#endif

namespace pdf
{
    // ---------------------------------------------------------
    // Fallback Font (eksik glyph'ler için sistem fontu)
    // ---------------------------------------------------------
    static FT_Library g_fallbackFTLib = nullptr;
    static FT_Face g_fallbackFace = nullptr;
    static bool g_fallbackInitialized = false;

    static FT_Face getFallbackFace()
    {
        if (!g_fallbackInitialized)
        {
            g_fallbackInitialized = true;

            // DEBUG LOG
            FILE* dbgFile = nullptr; // fopen("C:\\temp\\fallback_debug.txt", "w");
            if (dbgFile) {
            }

            if (FT_Init_FreeType(&g_fallbackFTLib) == 0)
            {
                if (dbgFile) {
                }

                // Windows sistem fontlarını dene
                const char* fallbackFonts[] = {
                    "C:\\Windows\\Fonts\\arial.ttf",
                    "C:\\Windows\\Fonts\\segoeui.ttf",
                    "C:\\Windows\\Fonts\\tahoma.ttf",
                    "C:\\Windows\\Fonts\\calibri.ttf",
                    nullptr
                };

                for (int i = 0; fallbackFonts[i] != nullptr; ++i)
                {
                    if (dbgFile) {
                    }

                    FT_Error err = FT_New_Face(g_fallbackFTLib, fallbackFonts[i], 0, &g_fallbackFace);
                    if (err == 0)
                    {
                        if (dbgFile) {
                            fprintf(dbgFile, "Face family: %s, style: %s\n",
                                g_fallbackFace->family_name ? g_fallbackFace->family_name : "(null)",
                                g_fallbackFace->style_name ? g_fallbackFace->style_name : "(null)");
                        }

                        // Başarılı - Unicode charmap seç
                        for (int cm = 0; cm < g_fallbackFace->num_charmaps; ++cm)
                        {
                            if (g_fallbackFace->charmaps[cm]->encoding == FT_ENCODING_UNICODE)
                            {
                                FT_Set_Charmap(g_fallbackFace, g_fallbackFace->charmaps[cm]);
                                if (dbgFile) {
                                }
                                break;
                            }
                        }
                        break;
                    }
                    else
                    {
                        if (dbgFile) {
                        }
                    }
                }
            }
            else
            {
                if (dbgFile) {
                }
            }

            if (dbgFile) {
            }
        }
        return g_fallbackFace;
    }

    // ---------------------------------------------------------
    // helpers (cpp-local)
    // ---------------------------------------------------------

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

    // MacRoman → Unicode dönüşüm tablosu
    // https://en.wikipedia.org/wiki/Mac_OS_Roman
    static const uint16_t MacRoman[256] =
    {
        /* 0–31 (control characters) */
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,

        /* 32–127 (ASCII) */
        32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,
        48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,
        64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,
        80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,
        96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,
        112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,

        /* 128–159 (MacRoman special - 0x80-0x9F) */
        0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, // 80-87: Ä Å Ç É Ñ Ö Ü á
        0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8, // 88-8F: à â ä ã å ç é è
        0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, // 90-97: ê ë í ì î ï ñ ó
        0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC, // 98-9F: ò ô ö õ ú ù û ü

        /* 160–175 (0xA0-0xAF) */
        0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF, // A0-A7: † ° ¢ £ § • ¶ ß
        0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8, // A8-AF: ® © ™ ´ ¨ ≠ Æ Ø

        /* 176–191 (0xB0-0xBF) */
        0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211, // B0-B7: ∞ ± ≤ ≥ ¥ µ ∂ ∑
        0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8, // B8-BF: ∏ π ∫ ª º Ω æ ø

        /* 192–207 (0xC0-0xCF) */
        0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, // C0-C7: ¿ ¡ ¬ √ ƒ ≈ ∆ «
        0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153, // C8-CF: » … NBSP À Ã Õ Œ œ

        /* 208–223 (0xD0-0xDF) */
        0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, // D0-D7: – — " " ' ' ÷ ◊
        0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02, // D8-DF: ÿ Ÿ ⁄ € ‹ › fi fl

        /* 224–239 (0xE0-0xEF) */
        0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, // E0-E7: ‡ · ‚ „ ‰ Â Ê Á
        0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4, // E8-EF: Ë È Í Î Ï Ì Ó Ô

        /* 240–255 (0xF0-0xFF) - Apple extended */
        0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC, // F0-F7: Apple Ò Ú Û Ù ı ˆ ˜
        0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7  // F8-FF: ¯ ˘ ˙ ˚ ¸ ˝ ˛ ˇ
    };

    static inline uint32_t FixTurkish(uint32_t uni)
    {
        switch (uni)
        {
        case 0xDD: return L'İ'; // Ý -> İ
        case 0xDE: return L'Ş'; // Þ -> Ş
        case 0xF0: return L'ğ'; // ð -> ğ
        case 0xFD: return L'ı'; // ý -> ı
        case 0xFE: return L'ş'; // þ -> ş
        case 0xD0: return L'Ğ'; // Ð -> Ğ
        default:   return uni;
        }
    }


    static PdfMatrix PdfTranslate(double tx, double ty)
    {
        PdfMatrix m;
        m.a = 1; m.b = 0; m.c = 0; m.d = 1;
        m.e = tx; m.f = ty;
        return m;
    }

    // ---------------------------------------------------------
    // Bezier flatten helpers (device-space tolerance)
    // ---------------------------------------------------------
    static inline double distPointLineSq(double px, double py, double ax, double ay, double bx, double by)
    {
        // =====================================================
        // KRİTİK FİX: Point-to-LINE distance (segment değil!)
        // Bezier flatten için doğru formül bu.
        // 
        // Eski kod point-to-segment hesaplıyordu - bu yanlış!
        // Kontrol noktası baseline dışındaysa yanlış sonuç veriyordu.
        // =====================================================

        double vx = bx - ax;
        double vy = by - ay;

        double lenSq = vx * vx + vy * vy;

        // Dejenere durum: A ve B aynı nokta
        if (lenSq < 1e-12)
            return (px - ax) * (px - ax) + (py - ay) * (py - ay);

        // Point-to-line distance formula:
        // d = |cross(AP, AB)| / |AB|
        // d² = cross² / lenSq

        double wx = px - ax;
        double wy = py - ay;

        // 2D cross product: wx * vy - wy * vx
        double cross = wx * vy - wy * vx;

        return (cross * cross) / lenSq;
    }

    std::vector<uint8_t> PdfPainter::getDownsampledBuffer() const
    {
        if (_ssaa <= 1)
            return _buffer;

        std::vector<uint8_t> output(_finalW * _finalH * 4);

        // Bilinear downsampling (daha pürüzsüz)
        for (int y = 0; y < _finalH; ++y)
        {
            for (int x = 0; x < _finalW; ++x)
            {
                // SSAA grid'deki merkez pozisyon
                float centerX = (x + 0.5f) * _ssaa;
                float centerY = (y + 0.5f) * _ssaa;

                // Bilinear filter kernel (gaussian-like)
                float rSum = 0, gSum = 0, bSum = 0, aSum = 0;
                float weightSum = 0;

                // SSAA grid üzerinde weighted sampling
                for (int dy = 0; dy < _ssaa; ++dy)
                {
                    for (int dx = 0; dx < _ssaa; ++dx)
                    {
                        int sx = x * _ssaa + dx;
                        int sy = y * _ssaa + dy;

                        if (sx >= _w || sy >= _h) continue;

                        // Mesafeye göre weight (Gaussian-like)
                        float distX = (sx + 0.5f) - centerX;
                        float distY = (sy + 0.5f) - centerY;
                        float distSq = distX * distX + distY * distY;

                        // Gaussian kernel (sigma = _ssaa/2)
                        float sigma = _ssaa * 0.5f;
                        float weight = std::exp(-distSq / (2.0f * sigma * sigma));

                        size_t si = (size_t(sy) * _w + sx) * 4;

                        bSum += _buffer[si + 0] * weight;
                        gSum += _buffer[si + 1] * weight;
                        rSum += _buffer[si + 2] * weight;
                        aSum += _buffer[si + 3] * weight;
                        weightSum += weight;
                    }
                }

                if (weightSum > 0)
                {
                    size_t di = (size_t(y) * _finalW + x) * 4;
                    output[di + 0] = (uint8_t)std::clamp((int)(bSum / weightSum), 0, 255);
                    output[di + 1] = (uint8_t)std::clamp((int)(gSum / weightSum), 0, 255);
                    output[di + 2] = (uint8_t)std::clamp((int)(rSum / weightSum), 0, 255);
                    output[di + 3] = (uint8_t)std::clamp((int)(aSum / weightSum), 0, 255);
                }
            }
        }

        return output;
    }


    static void flattenCubicBezierDeviceD(
        double x0, double y0,
        double x1, double y1,
        double x2, double y2,
        double x3, double y3,
        std::vector<DPoint>& out,
        double tolPxSq,
        int depth = 0)
    {
        // =====================================================
        // KRİTİK FİX: Küçük bezier'ler için erken çıkış koşulları
        // çok agresifti - logolar kare çıkıyordu!
        // 
        // MuPDF tarzı yaklaşım: Sadece flatness kontrolü yap,
        // uzunluk kontrolü yapma.
        // =====================================================

        const int MAX_DEPTH = 24;  // Daha derin subdivision

        // Kontrol noktalarının baseline'a uzaklığı (flatness)
        double d1 = distPointLineSq(x1, y1, x0, y0, x3, y3);
        double d2 = distPointLineSq(x2, y2, x0, y0, x3, y3);
        double flatness = std::max(d1, d2);

        // Max depth'e ulaştıysak dur
        if (depth >= MAX_DEPTH)
        {
            out.push_back({ x3, y3 });
            return;
        }

        // Flatness kontrolü - bezier yeterince düzse dur
        // Tolerans: 0.0025 px² = 0.05 px (alt-pixel hassasiyet)
        if (flatness <= tolPxSq)
        {
            out.push_back({ x3, y3 });
            return;
        }

        // De Casteljau subdivision
        double x01 = (x0 + x1) * 0.5, y01 = (y0 + y1) * 0.5;
        double x12 = (x1 + x2) * 0.5, y12 = (y1 + y2) * 0.5;
        double x23 = (x2 + x3) * 0.5, y23 = (y2 + y3) * 0.5;

        double x012 = (x01 + x12) * 0.5, y012 = (y01 + y12) * 0.5;
        double x123 = (x12 + x23) * 0.5, y123 = (y12 + y23) * 0.5;

        double x0123 = (x012 + x123) * 0.5;
        double y0123 = (y012 + y123) * 0.5;

        // Recursive subdivision - her iki yarıyı da işle
        flattenCubicBezierDeviceD(
            x0, y0, x01, y01, x012, y012, x0123, y0123,
            out, tolPxSq, depth + 1);

        flattenCubicBezierDeviceD(
            x0123, y0123, x123, y123, x23, y23, x3, y3,
            out, tolPxSq, depth + 1);
    }


    static inline void addPointUnique(std::vector<pdf::IPoint>& pts, int x, int y)
    {
        if (!pts.empty() && pts.back().x == x && pts.back().y == y) return;
        pts.push_back({ x, y });
    }

    // ---------------------------------------------------------
// Helpers for stroke outline (single polygon fill)
// ---------------------------------------------------------
    static inline double dot2(double ax, double ay, double bx, double by) { return ax * bx + ay * by; }
    static inline double cross2(double ax, double ay, double bx, double by) { return ax * by - ay * bx; }
    static inline double len2d(double x, double y) { return std::sqrt(x * x + y * y); }

    static inline void normalize2(double& x, double& y)
    {
        double L = len2d(x, y);
        if (L < 1e-12) { x = 0; y = 0; return; }
        x /= L; y /= L;
    }

    // Y-down screen space: left normal for direction (dx,dy)
    static inline void leftNormal2(double dx, double dy, double& nx, double& ny)
    {
        nx = dy;
        ny = -dx;
        normalize2(nx, ny);
    }

    static inline double angleOf2(double x, double y) { return std::atan2(y, x); }

    // normalize to (-PI, PI]
    static inline double normSweep(double s)
    {
        while (s <= -M_PI) s += 2.0 * M_PI;
        while (s > M_PI)  s -= 2.0 * M_PI;
        return s;
    }

    // Append arc points from a0 to a1 (excluding start point, including end point)
    static inline void appendArcPoints(
        std::vector<DPoint>& out,
        const DPoint& center,
        double a0, double a1,
        double r)
    {
        double sweep = a1 - a0;
        while (sweep > M_PI) sweep -= 2.0 * M_PI;
        while (sweep < -M_PI) sweep += 2.0 * M_PI;

        // Daha fazla nokta (daha pürüzsüz)
        int steps = (int)std::ceil(std::abs(sweep) / (M_PI / 16.0)); // Her ~11 derece
        if (steps < 8) steps = 8;
        if (steps > 64) steps = 64;

        double step = sweep / steps;
        for (int i = 1; i <= steps; ++i)
        {
            double ang = a0 + step * i;
            out.push_back({
                center.x + std::cos(ang) * r,
                center.y + std::sin(ang) * r
                });
        }
    }

    // Line intersection: p0 + t*d0 intersects p1 + u*d1
    static inline bool intersectLines2(
        const DPoint& p0, const DPoint& d0,
        const DPoint& p1, const DPoint& d1,
        DPoint& out)
    {
        double a = d0.x, b = -d1.x;
        double c = d0.y, d = -d1.y;
        double e = p1.x - p0.x;
        double f = p1.y - p0.y;

        double det = a * d - b * c;
        if (std::abs(det) < 1e-10) return false;

        double t = (e * d - b * f) / det;
        out = { p0.x + t * d0.x, p0.y + t * d0.y };
        return true;
    }

    static inline void pushUniqueD(std::vector<DPoint>& v, const DPoint& p)
    {
        if (!v.empty())
        {
            double dx = v.back().x - p.x;
            double dy = v.back().y - p.y;
            if (dx * dx + dy * dy < 1e-8) return;
        }
        v.push_back(p);
    }

    // lineWidth (user space) -> device px (CTM dahil)
    static inline double lineWidthToDevicePx(
        double lineWidthUser,
        const PdfMatrix& ctm,
        double scaleX,
        double scaleY)
    {
        // CTM'in iki ekseninin device uzaydaki uzunluğu
        // ex = (a,b), ey = (c,d)
        double ex = std::hypot(ctm.a * scaleX, ctm.b * scaleY);
        double ey = std::hypot(ctm.c * scaleX, ctm.d * scaleY);

        // güvenli yaklaşım: max (barcode gibi kritik çizgilerde daha doğru)
        double s = std::max(ex, ey);

        double lwPx = lineWidthUser * s;

        // ❌ 1px’e kilitleme yapma. Çok küçükse 0.25px gibi taban ver.
        if (lwPx < 0.25) lwPx = 0.25;
        return lwPx;
    }


    static inline void addPointUniqueD(std::vector<DPoint>& pts, double x, double y)
    {
        // Çok yakın noktaları filtrele (Gürültüyü önler)
        if (!pts.empty()) {
            double dx = pts.back().x - x;
            double dy = pts.back().y - y;
            if (dx * dx + dy * dy < 0.001) return;
        }
        pts.push_back({ x, y });
    }


    static inline double len2(double x, double y) { return std::sqrt(x * x + y * y); }

    static inline void norm(double& x, double& y)
    {
        double l = len2(x, y);
        if (l < 1e-9) { x = 0; y = 0; return; }
        x /= l; y /= l;
    }

    static inline bool isCidFontActivePainter(const PdfFontInfo* f)
    {
        if (!f) return false;
        if (f->isCidFont) return true;
        if (f->encoding == "/Identity-H" || f->encoding == "/Identity-V") return true;
        return false;
    }

    static inline int getWidth1000ForCodePainter(const PdfFontInfo* f, int code)
    {
        if (!f) return 0;

        // CID font ise
        if (f->isCidFont || f->encoding == "/Identity-H" || f->encoding == "/Identity-V")
        {
            // Oncelikle /W array'de bu CID var mi?
            auto it = f->cidWidths.find((uint16_t)code);
            if (it != f->cidWidths.end())
                return it->second;
            // /W yoksa ve cidDefaultWidth 1000 (parse edilmemis default) ise
            // FreeType kullanilacak, 0 don
            if (f->cidWidths.empty() && f->cidDefaultWidth == 1000)
                return 0;
            return f->cidDefaultWidth;
        }

        // Simple font
        int w = f->missingWidth;
        if (w <= 0) w = 500;

        // Simple font /Widths
        if (f->hasWidths &&
            code >= f->firstChar &&
            code < f->firstChar + (int)f->widths.size())
        {
            int idx = code - f->firstChar;
            int ww = f->widths[idx];
            if (ww > 0) w = ww;
        }
        return w;
    }

    // NOT: static fillQuad ve fillTri fonksiyonlarını kaldırdık çünkü erişim hatası veriyordu.
    // Bu mantığı strokeSubpath içinde lambda olarak kullanacağız.

    double PdfPainter::drawTextFreeTypeRaw(
        double x,
        double y,
        const std::string& raw,
        double fontSizePt,
        double advanceSizePt,
        uint32_t color,
        const PdfFontInfo* font,
        double charSpacing,
        double wordSpacing,
        double horizScale,
        double textAngle
    )
    {
        {
            static FILE* dbgFile = nullptr;
            if (!dbgFile) {
                dbgFile = fopen("C:\\temp\\text_debug.txt", "w");
            }
            if (dbgFile) {
                fprintf(dbgFile, "Font: %s, encoding: %s\n",
                    font ? font->baseFont.c_str() : "(null)",
                    font ? font->encoding.c_str() : "(null)");
                fprintf(dbgFile, "isCidFont: %d, hasSimpleMap: %d\n",
                    (font && font->isCidFont) ? 1 : 0,
                    (font && font->hasSimpleMap) ? 1 : 0);
                fprintf(dbgFile, "ftReady: %d, ftFace: %p\n",
                    (font && font->ftReady) ? 1 : 0,
                    font ? (void*)font->ftFace : nullptr);
                for (size_t i = 0; i < raw.size() && i < 50; i++) {
                }
            }
        }

        if (!font || !font->ftReady || !font->ftFace) {
            LogDebug("*** EARLY RETURN drawTextFreeTypeRaw: font=%p, ftReady=%d, ftFace=%p, baseFont=%s ***",
                (void*)font,
                font ? (font->ftReady ? 1 : 0) : -1,
                font ? (void*)font->ftFace : nullptr,
                font ? font->baseFont.c_str() : "(null)");
            return 0.0;
        }
        if (raw.empty()) return 0.0;

        FT_Face face = font->ftFace;

        // Font size -> px
        double pxSize = fontSizePt * _scaleY;
        FT_Set_Char_Size(face, 0, (FT_F26Dot6)std::llround(pxSize * 64.0), 72, 72);

        // Starting position (device space)
        double penXf = x * _scaleX;
        double penYf = mapY(y * _scaleY) + 1.0;

        // 26.6 fixed point format
        FT_Pos penX26 = (FT_Pos)std::llround(penXf * 64.0);
        FT_Pos penY26 = (FT_Pos)std::llround(penYf * 64.0);
        FT_Pos startX26 = penX26;
        FT_Pos startY26 = penY26;  // For rotated text advance calculation

        // Horizontal compression: ratio of X-scale to Y-scale
        // For non-uniform text matrices like [7.2 0 0 8]:
        //   fontSizePt ∝ Y-scale (8), advanceSizePt ∝ X-scale (7.2)
        //   horzCompress = 7.2/8 = 0.9 -> glyph is 90% as wide
        double horzCompress = 1.0;
        if (fontSizePt > 0.001)
            horzCompress = advanceSizePt / fontSizePt;

        // Text rotation support
        double cosA = std::cos(textAngle);
        double sinA = std::sin(textAngle);
        bool hasTextRotation = (std::abs(textAngle) > 0.001);

        // FreeType transform matrix
        // Combines: PDF Th (horizScale), non-uniform matrix compression, and text rotation
        double hScale = horizScale / 100.0;
        FT_Matrix ftm;
        ftm.xx = (FT_Fixed)std::llround(cosA * hScale * horzCompress * 65536.0);
        ftm.xy = (FT_Fixed)std::llround(-sinA * 65536.0);
        ftm.yx = (FT_Fixed)std::llround(sinA * hScale * horzCompress * 65536.0);
        ftm.yy = (FT_Fixed)std::llround(cosA * 65536.0);

        auto getAdvancePx = [&](int code) -> double
            {
                int w1000 = getWidth1000ForCodePainter(font, code);
                // width 0 ise FreeType kullanilacak sinyali, gecici default
                if (w1000 <= 0) w1000 = 500;
                double advPt = (w1000 / 1000.0) * advanceSizePt;
                advPt += charSpacing;
                if (code == 32) advPt += wordSpacing;
                advPt *= (horizScale / 100.0);
                double advPx = advPt * _scaleX;
                return advPx;
            };

        auto setPenSubpixelTransform = [&](FT_Pos curX26, FT_Pos curY26) -> std::pair<int, int>
            {
                FT_Pos floorX26 = (curX26 & ~63);
                FT_Pos floorY26 = (curY26 & ~63);
                FT_Vector delta;
                delta.x = curX26 - floorX26;
                delta.y = curY26 - floorY26;
                FT_Set_Transform(face, &ftm, &delta);
                return { (int)(floorX26 >> 6), (int)(floorY26 >> 6) };
            };

        const bool cidMode = isCidFontActivePainter(font);

        {
            static FILE* dbgFile = nullptr;
            if (!dbgFile) dbgFile = nullptr; // fopen("C:\\temp\\text_debug.txt", "a");
            if (dbgFile) {
                fprintf(dbgFile, "hasCidToGidMap=%d, cidToGidIdentity=%d\n",
                    font->hasCidToGidMap ? 1 : 0, font->cidToGidIdentity ? 1 : 0);
                for (size_t ii = 0; ii < raw.size() && ii < 40; ii++) {
                }
                fprintf(dbgFile, "cidMode=%d, fontProgram.size=%zu, encoding='%s'\n",
                    cidMode ? 1 : 0, font->fontProgram.size(), font->encoding.c_str());
            }
        }

        if (cidMode)
        {
            {
                static FILE* dbgFile = nullptr;
                if (!dbgFile) dbgFile = nullptr; // fopen("C:\\temp\\text_debug.txt", "a");
                if (dbgFile) {
                }
            }

            {
                static FILE* cidDbg = nullptr;
                static bool firstTime = true;
                if (firstTime) {
                    cidDbg = fopen("C:\\temp\\cid_debug.txt", "w");
                    firstTime = false;
                }
                if (cidDbg) {
                    fprintf(cidDbg, "=== CID Font Debug ===\n");
                    fprintf(cidDbg, "fontName: %s\n", font->baseFont.c_str());
                    fprintf(cidDbg, "fontProgram.empty: %d\n", font->fontProgram.empty() ? 1 : 0);
                    fprintf(cidDbg, "cidToUnicode.size: %zu\n", font->cidToUnicode.size());
                    fprintf(cidDbg, "hasCidToGidMap: %d\n", font->hasCidToGidMap ? 1 : 0);
                    fprintf(cidDbg, "cidToGidIdentity: %d\n", font->cidToGidIdentity ? 1 : 0);
                    fprintf(cidDbg, "raw.size: %zu\n", raw.size());
                    fflush(cidDbg);
                }
            }

            for (size_t i = 0; i + 1 < raw.size(); i += 2)
            {
                int cid = ((unsigned char)raw[i] << 8) | (unsigned char)raw[i + 1];
                FT_UInt gid = 0;

                // DUZELTME: Font embedded degilse (sistem fontu), ToUnicode -> FT_Get_Char_Index kullan
                // Cunku sistem fontunda GID sirasi embedded fonttan farkli
                bool usedToUnicode = false;
                uint32_t unicodeVal = 0;
                if (font->fontProgram.empty() && !font->cidToUnicode.empty()) {
                    auto it = font->cidToUnicode.find((uint16_t)cid);
                    if (it != font->cidToUnicode.end() && it->second != 0) {
                        unicodeVal = it->second;
                        gid = FT_Get_Char_Index(face, (FT_ULong)it->second);
                        usedToUnicode = true;
                    }
                }

                // ToUnicode bulunamadiysa, eski CIDToGIDMap yontemini kullan
                if (!usedToUnicode) {
                    if (font->hasCidToGidMap) {
                        if (font->cidToGidIdentity) gid = (FT_UInt)cid;
                        else gid = (cid >= 0 && (size_t)cid < font->cidToGid.size()) ? (FT_UInt)font->cidToGid[cid] : (FT_UInt)cid;
                    }
                    else gid = (FT_UInt)cid;
                }

                {
                    static FILE* cidDbg = fopen("C:\\temp\\cid_debug.txt", "a");
                    if (cidDbg) {
                        char ch = (unicodeVal >= 32 && unicodeVal < 127) ? (char)unicodeVal : '?';
                        fprintf(cidDbg, "CID=0x%04X -> unicode=0x%04X ('%c') -> GID=%u, usedToUnicode=%d\n",
                            cid, unicodeVal, ch, gid, usedToUnicode ? 1 : 0);
                        fflush(cidDbg);
                    }
                }

                auto [penX, penY] = setPenSubpixelTransform(penX26, penY26);

                double advPx = getAdvancePx(cid);  // default: PDF width'ten

                // GLYPH CACHE - Massive performance boost!
                if (gid != 0)
                {
                    int pixelSize = std::max(4, (int)std::round(pxSize));
                    double scaleCorrection = pxSize / (double)pixelSize;
                    // Use fontHash for cache key (stable across font reloads)
                    size_t fontHash = font->fontHash > 0 ? font->fontHash : reinterpret_cast<size_t>(face);
                    const CachedGlyph* cached = GlyphCache::instance().getOrRender(face, fontHash, gid, pixelSize);

                    if (cached && !cached->bitmap.empty())
                    {
                        // Use cached advance if PDF doesn't have width info
                        bool useFreeTypeWidth = false;
                        if (font->isCidFont || font->encoding == "/Identity-H" || font->encoding == "/Identity-V") {
                            useFreeTypeWidth = font->cidWidths.empty();
                        }
                        else {
                            useFreeTypeWidth = !font->hasWidths;
                        }

                        if (useFreeTypeWidth)
                        {
                            double ftAdvPx = cached->advanceX * scaleCorrection;
                            // FreeType advance is based on fontSizePt (Y-scale), correct to X-scale
                            if (fontSizePt > 0.001)
                                ftAdvPx *= (advanceSizePt / fontSizePt);
                            ftAdvPx += charSpacing * _scaleX;
                            if (cid == 32) ftAdvPx += wordSpacing * _scaleX;
                            ftAdvPx *= (horizScale / 100.0);
                            advPx = ftAdvPx;
                        }

                        // Draw cached glyph with scale correction + horizontal compression
                        // bearingX is also compressed horizontally
                        double scaledBearingX = cached->bearingX * scaleCorrection * horzCompress;
                        double scaledBearingY = cached->bearingY * scaleCorrection;
                        int gx, gy;
                        if (hasTextRotation) {
                            // Rotate bearing offsets for rotated text
                            double rotBearX = scaledBearingX * cosA + scaledBearingY * sinA;
                            double rotBearY = -scaledBearingX * sinA + scaledBearingY * cosA;
                            gx = penX + (int)std::round(rotBearX);
                            gy = penY - (int)std::round(rotBearY);
                        } else {
                            gx = penX + (int)std::round(scaledBearingX);
                            gy = penY - (int)std::round(scaledBearingY);
                        }
                        double scaleCorrX = scaleCorrection * horzCompress;  // X: includes horz compression
                        double scaleCorrY = scaleCorrection;                 // Y: no compression
                        int drawW = std::max(1, (int)std::round(cached->width * scaleCorrX));
                        int drawH = std::max(1, (int)std::round(cached->height * scaleCorrY));
                        // When scales are ~1.0, draw directly; otherwise scale bitmap
                        if (std::abs(scaleCorrX - 1.0) < 0.01 && std::abs(scaleCorrY - 1.0) < 0.01) {
                            blendGray8ToBuffer(gx, gy, cached->width, cached->height,
                                cached->bitmap.data(), cached->pitch, color);
                        } else {
                            // Scale correction needed - create scaled bitmap (anisotropic)
                            std::vector<uint8_t> scaled(drawW * drawH);
                            for (int sy = 0; sy < drawH; ++sy) {
                                int srcY = std::min((int)(sy / scaleCorrY), cached->height - 1);
                                for (int sx = 0; sx < drawW; ++sx) {
                                    int srcX = std::min((int)(sx / scaleCorrX), cached->width - 1);
                                    scaled[sy * drawW + sx] = cached->bitmap[srcY * cached->pitch + srcX];
                                }
                            }
                            blendGray8ToBuffer(gx, gy, drawW, drawH, scaled.data(), drawW, color);
                        }
                    }
                }
                if (hasTextRotation) {
                    penX26 += (FT_Pos)std::llround(advPx * cosA * 64.0);
                    penY26 -= (FT_Pos)std::llround(advPx * sinA * 64.0);
                } else {
                    penX26 += (FT_Pos)std::llround(advPx * 64.0);
                }
            }
        }
        else
        {
            {
                static FILE* dbgFile = nullptr;
                if (!dbgFile) dbgFile = nullptr; // fopen("C:\\temp\\text_debug.txt", "a");
                if (dbgFile) {
                }
            }

            for (unsigned char c : raw)
            {
                int code = (int)c;
                FT_UInt gi = 0;

                // =====================================================
                // MuPDF tarzı: codeToGid tablosu varsa doğrudan kullan
                // =====================================================
                if (font->hasCodeToGid && font->codeToGid[code] > 0)
                {
                    gi = font->codeToGid[code];
                }

                // =====================================================
                // gi hala 0 ise fallback mekanizmaları
                // =====================================================
                if (gi == 0)
                {
                    // MacRomanEncoding + embedded font: karakter kodunu doğrudan kullan
                    if (font->encoding == "/MacRomanEncoding" && font->fontProgram.size() > 0) {
                        // Mac charmap'i bul ve karakter kodunu doğrudan ara
                        for (int cm = 0; cm < face->num_charmaps && gi == 0; cm++) {
                            if (face->charmaps[cm]->platform_id == 1 && face->charmaps[cm]->encoding_id == 0) {
                                FT_Set_Charmap(face, face->charmaps[cm]);
                                gi = FT_Get_Char_Index(face, (FT_ULong)code);
                                break;
                            }
                        }
                        // Mac charmap'te bulunamadıysa Unicode'a çevirip dene
                        if (gi == 0) {
                            uint32_t uni = MacRoman[c];
                            if (uni != 0) {
                                for (int cm = 0; cm < face->num_charmaps && gi == 0; cm++) {
                                    FT_Set_Charmap(face, face->charmaps[cm]);
                                    gi = FT_Get_Char_Index(face, (FT_ULong)uni);
                                }
                            }
                        }
                    }
                    else {
                        // WinAnsiEncoding veya diğer encoding'ler: Unicode'a çevir
                        uint32_t uni = 0;

                        // ToUnicode map varsa kullan
                        if (font->hasSimpleMap && font->codeToUnicode[c] != 0) {
                            uni = font->codeToUnicode[c];
                            uni = FixTurkish(uni);
                        }
                        // Encoding tablosuyla Unicode'a çevir
                        else {
                            uni = WinAnsi[c];
                            uni = FixTurkish(uni);
                        }

                        // Unicode değerini tüm charmap'lerde ara
                        if (uni != 0) {
                            for (int cm = 0; cm < face->num_charmaps && gi == 0; cm++) {
                                FT_Set_Charmap(face, face->charmaps[cm]);
                                gi = FT_Get_Char_Index(face, (FT_ULong)uni);
                            }
                        }

                        // Hala bulunamadıysa karakter kodunu doğrudan dene
                        if (gi == 0) {
                            for (int cm = 0; cm < face->num_charmaps && gi == 0; cm++) {
                                FT_Set_Charmap(face, face->charmaps[cm]);
                                gi = FT_Get_Char_Index(face, (FT_ULong)code);
                            }
                        }
                    }
                }

                {
                    static FILE* dbgFile = nullptr;
                    if (!dbgFile) dbgFile = nullptr; // fopen("C:\\temp\\text_debug.txt", "a");
                    if (dbgFile) {
                    }
                }

                auto [penX, penY] = setPenSubpixelTransform(penX26, penY26);

                // Glyph'i render et - bulunamazsa fallback font dene
                FT_Face renderFace = face;
                FT_UInt renderGi = gi;

                // Embedded fontta glyph bulunamadıysa fallback font dene
                if (gi == 0)
                {
                    // DEBUG LOG
                    {
                        static FILE* dbgFile = nullptr;
                        if (!dbgFile) dbgFile = nullptr; // fopen("C:\\temp\\fallback_debug.txt", "w");
                        if (dbgFile) {
                        }
                    }

                    FT_Face fallback = getFallbackFace();

                    // DEBUG LOG
                    {
                        static FILE* dbgFile = nullptr;
                        if (!dbgFile) dbgFile = nullptr; // fopen("C:\\temp\\fallback_debug.txt", "a");
                        if (dbgFile) {
                        }
                    }

                    if (fallback)
                    {
                        // Unicode değerini hesapla (fallback için)
                        uint32_t fallbackUni = 0;
                        if (font->hasSimpleMap && font->codeToUnicode[c] != 0) {
                            fallbackUni = font->codeToUnicode[c];
                        }
                        else {
                            fallbackUni = WinAnsi[c];
                        }
                        fallbackUni = FixTurkish(fallbackUni);

                        // DEBUG LOG
                        {
                            static FILE* dbgFile = nullptr;
                            if (!dbgFile) dbgFile = nullptr; // fopen("C:\\temp\\fallback_debug.txt", "a");
                            if (dbgFile) {
                            }
                        }

                        if (fallbackUni != 0)
                        {
                            // Fallback font'un boyutunu ayarla
                            FT_Set_Char_Size(fallback, 0, (FT_F26Dot6)std::llround(pxSize * 64.0), 72, 72);
                            FT_Set_Transform(fallback, &ftm, nullptr);

                            renderGi = FT_Get_Char_Index(fallback, (FT_ULong)fallbackUni);

                            // DEBUG LOG
                            {
                                static FILE* dbgFile = nullptr;
                                if (!dbgFile) dbgFile = nullptr; // fopen("C:\\temp\\fallback_debug.txt", "a");
                                if (dbgFile) {
                                }
                            }

                            if (renderGi != 0)
                            {
                                renderFace = fallback;
                            }
                        }
                    }
                }

                double advPx = getAdvancePx(code);  // default: PDF width'ten

                static FILE* renderDebug = nullptr;
                if (!renderDebug) {
                    renderDebug = nullptr; // fopen("C:\\temp\\render_debug.txt", "w");
                    if (renderDebug) {
                    }
                }

                // GLYPH CACHE - Massive performance boost!
                if (renderGi != 0)
                {
                    int pixelSize = std::max(4, (int)std::round(pxSize));
                    double scaleCorrection = pxSize / (double)pixelSize;
                    // Use fontHash if available, else use face pointer
                    // For fallback font, use face pointer as hash
                    size_t fontHash = (renderFace == face && font->fontHash > 0)
                        ? font->fontHash
                        : reinterpret_cast<size_t>(renderFace);
                    const CachedGlyph* cached = GlyphCache::instance().getOrRender(renderFace, fontHash, renderGi, pixelSize);

                    if (cached && !cached->bitmap.empty())
                    {
                        // Use FreeType advance if PDF doesn't have width info
                        if (!font->hasWidths)
                        {
                            double ftAdvPx = cached->advanceX * scaleCorrection;
                            // FreeType advance is Y-scale based, correct to X-scale
                            if (fontSizePt > 0.001)
                                ftAdvPx *= (advanceSizePt / fontSizePt);
                            ftAdvPx += charSpacing * _scaleX;
                            if (code == 32) ftAdvPx += wordSpacing * _scaleX;
                            ftAdvPx *= (horizScale / 100.0);
                            advPx = ftAdvPx;
                        }

                        // Apply horizontal compression to bearing and glyph width
                        double scaledBearingX = cached->bearingX * scaleCorrection * horzCompress;
                        double scaledBearingY = cached->bearingY * scaleCorrection;
                        int gx, gy;
                        if (hasTextRotation) {
                            // Rotate bearing offsets for rotated text
                            double rotBearX = scaledBearingX * cosA + scaledBearingY * sinA;
                            double rotBearY = -scaledBearingX * sinA + scaledBearingY * cosA;
                            gx = penX + (int)std::round(rotBearX);
                            gy = penY - (int)std::round(rotBearY);
                        } else {
                            gx = penX + (int)std::round(scaledBearingX);
                            gy = penY - (int)std::round(scaledBearingY);
                        }

                        if (renderDebug) {
                            uint32_t uniForLog = 0;
                            if (font->hasSimpleMap && font->codeToUnicode[c] != 0) {
                                uniForLog = font->codeToUnicode[c];
                            }
                            else {
                                uniForLog = WinAnsi[c];
                            }
                            char displayChar = '?';
                            if (uniForLog >= 32 && uniForLog < 127) {
                                displayChar = (char)uniForLog;
                            }
                            else if (code >= 32 && code < 127) {
                                displayChar = (char)code;
                            }
                            fprintf(renderDebug, "DRAW: code=%3d(0x%02X) gid=%3u pos=(%4d,%4d) adv=%.1f uni=0x%04X char='%c'\n",
                                code, code, renderGi, gx, gy, advPx, uniForLog, displayChar);
                        }

                        double scaleCorrX = scaleCorrection * horzCompress;  // X: with compression
                        double scaleCorrY = scaleCorrection;                 // Y: no compression
                        int drawW = std::max(1, (int)std::round(cached->width * scaleCorrX));
                        int drawH = std::max(1, (int)std::round(cached->height * scaleCorrY));
                        if (std::abs(scaleCorrX - 1.0) < 0.01 && std::abs(scaleCorrY - 1.0) < 0.01) {
                            blendGray8ToBuffer(gx, gy, cached->width, cached->height,
                                cached->bitmap.data(), cached->pitch, color);
                        } else {
                            std::vector<uint8_t> scaled(drawW * drawH);
                            for (int sy = 0; sy < drawH; ++sy) {
                                int srcY = std::min((int)(sy / scaleCorrY), cached->height - 1);
                                for (int sx = 0; sx < drawW; ++sx) {
                                    int srcX = std::min((int)(sx / scaleCorrX), cached->width - 1);
                                    scaled[sy * drawW + sx] = cached->bitmap[srcY * cached->pitch + srcX];
                                }
                            }
                            blendGray8ToBuffer(gx, gy, drawW, drawH, scaled.data(), drawW, color);
                        }
                    }
                }
                else if (renderDebug) {
                    // Glyph bulunamadı
                    uint32_t uniForLog = 0;
                    if (font->hasSimpleMap && font->codeToUnicode[c] != 0) {
                        uniForLog = font->codeToUnicode[c];
                    }
                    else {
                        uniForLog = WinAnsi[c];
                    }
                    fprintf(renderDebug, "SKIP: code=%3d(0x%02X) gid=%3u (glyph not found or load failed) uni=0x%04X\n",
                        code, code, renderGi, uniForLog);
                }

                if (hasTextRotation) {
                    penX26 += (FT_Pos)std::llround(advPx * cosA * 64.0);
                    penY26 -= (FT_Pos)std::llround(advPx * sinA * 64.0);
                } else {
                    penX26 += (FT_Pos)std::llround(advPx * 64.0);
                }
            }
        }

        FT_Set_Transform(face, nullptr, nullptr);

        if (hasTextRotation) {
            // For rotated text, return total advance as scalar distance
            double dx = (double)(penX26 - startX26) / 64.0;
            double dy = (double)(penY26 - startY26) / 64.0;
            return std::sqrt(dx * dx + dy * dy) / _scaleX;
        } else {
            FT_Pos delta26 = penX26 - startX26;
            double advPx = (double)delta26 / 64.0;
            return advPx / _scaleX;
        }
    }


    void PdfPainter::blendGray8ToBuffer(
        int dstX, int dstY,
        int w, int h,
        const uint8_t* src,
        int srcPitch,
        uint32_t color)
    {
        if (!src) return;

        uint8_t cr = (color >> 16) & 0xFF;
        uint8_t cg = (color >> 8) & 0xFF;
        uint8_t cb = (color) & 0xFF;

        for (int y = 0; y < h; ++y)
        {
            int py = dstY + y;
            if (py < 0 || py >= _h) continue;

            const uint8_t* row = src + y * srcPitch;

            for (int x = 0; x < w; ++x)
            {
                int px = dstX + x;
                if (px < 0 || px >= _w) continue;

                uint8_t a = row[x];
                if (a == 0) continue;

                size_t di = (size_t(py) * _w + px) * 4;

                uint8_t db = _buffer[di + 0];
                uint8_t dg = _buffer[di + 1];
                uint8_t dr = _buffer[di + 2];

                int ia = 255 - a;

                _buffer[di + 0] = (uint8_t)((cb * a + db * ia) / 255);
                _buffer[di + 1] = (uint8_t)((cg * a + dg * ia) / 255);
                _buffer[di + 2] = (uint8_t)((cr * a + dr * ia) / 255);
                _buffer[di + 3] = 255;
            }
        }
    }

    static inline int clampi(int v, int lo, int hi)
    {
        if (v < lo) return lo;
        if (v > hi) return hi;
        return v;
    }

    static inline uint8_t clampu8(int v)
    {
        if (v < 0) return 0;
        if (v > 255) return 255;
        return (uint8_t)v;
    }

    static inline double signedAngle(const DPoint& a, const DPoint& b)
    {
        // a -> b dönüş açısı (signed)
        double cr = cross2(a.x, a.y, b.x, b.y);
        double dp = dot2(a.x, a.y, b.x, b.y);
        return std::atan2(cr, dp); // (-pi, pi)
    }

    static inline void ApplyMatrix(const PdfMatrix& m, double x, double y, double& ox, double& oy)
    {
        ox = m.a * x + m.c * y + m.e;
        oy = m.b * x + m.d * y + m.f;
    }

    static bool InvertMatrix(const PdfMatrix& m, PdfMatrix& inv)
    {
        double det = m.a * m.d - m.b * m.c;
        if (std::abs(det) < 1e-12) return false;

        double id = 1.0 / det;
        inv.a = m.d * id;
        inv.b = -m.b * id;
        inv.c = -m.c * id;
        inv.d = m.a * id;
        inv.e = -(inv.a * m.e + inv.c * m.f);
        inv.f = -(inv.b * m.e + inv.d * m.f);
        return true;
    }

    // ---------------------------------------------------------
    // CONSTRUCTOR
    // ---------------------------------------------------------
    PdfPainter::PdfPainter(int width, int height, double scaleX, double scaleY, int ssaa)
        : _finalW(width), _finalH(height), _scaleX(scaleX), _scaleY(scaleY), _ssaa(ssaa)
    {
        // SSAA için internal buffer boyutunu büyüt
        _w = width * _ssaa;
        _h = height * _ssaa;

        // Scale'leri de SSAA ile çarp
        _scaleX *= _ssaa;
        _scaleY *= _ssaa;

        if (_w < 1) _w = 1;
        if (_h < 1) _h = 1;
        if (_scaleX <= 0.0) _scaleX = 1.0;
        if (_scaleY <= 0.0) _scaleY = 1.0;

        _buffer.resize((size_t)_w * (size_t)_h * 4, 255);

        _hasRotate = false;
        _rotA = _rotD = 1.0;
        _rotB = _rotC = _rotTx = _rotTy = 0.0;
    }


    // ---------------------------------------------------------
    // IMAGE DRAW
    // ---------------------------------------------------------
    void PdfPainter::drawImage(
        const std::vector<uint8_t>& rgba,
        int imgW,
        int imgH,
        const PdfMatrix& ctm)
    {
        if (imgW <= 0 || imgH <= 0) return;
        if ((int)rgba.size() < imgW * imgH * 4) return;

        LogDebug("drawImage: %dx%d CTM=[%.2f %.2f %.2f %.2f %.2f %.2f]",
            imgW, imgH, ctm.a, ctm.b, ctm.c, ctm.d, ctm.e, ctm.f);

        // =====================================================
        // MuPDF Yaklaşımı:
        // image_ctm zaten düzeltilmiş olarak geliyor (caller tarafından)
        // Bu yüzden burada ekstra flip yapmıyoruz.
        //
        // PDF Image Unit Square:
        // - (0,0) = image'ın alt-sol köşesi
        // - (1,1) = image'ın üst-sağ köşesi
        //
        // image_ctm bunu page space'e dönüştürür.
        // =====================================================

        // =====================================================
        // AUTO-SCALE: CTM'de scale yoksa image boyutlarini kullan
        // CTM'nin effective scale'ini hesapla
        // =====================================================
        double effScaleX = std::sqrt(ctm.a * ctm.a + ctm.b * ctm.b);
        double effScaleY = std::sqrt(ctm.c * ctm.c + ctm.d * ctm.d);

        // Kullanilacak CTM - scale yoksa augment et
        PdfMatrix useCTM = ctm;
        bool needsScale = (effScaleX < 2.0 && effScaleY < 2.0 && imgW > 1 && imgH > 1);

        if (needsScale) {
            // Scale a,b with imgW; c,d,f with imgH for flipped images
            useCTM.a = ctm.a * imgW;
            useCTM.b = ctm.b * imgW;
            useCTM.c = ctm.c * imgH;
            useCTM.d = ctm.d * imgH;
            useCTM.f = ctm.f * imgH;  // Scale f too for flipped images

            LogDebug("drawImage: AUTO-SCALE applied, effective CTM=[%.2f %.2f %.2f %.2f %.2f %.2f]",
                useCTM.a, useCTM.b, useCTM.c, useCTM.d, useCTM.e, useCTM.f);
        }

        // Inverse CTM'i yeniden hesapla (useCTM icin)
        PdfMatrix useInv;
        if (!InvertMatrix(useCTM, useInv)) {
            return;
        }

        // Compute bounding box in page space (4 koseyi transform et)
        double ux0, uy0, ux1, uy1, ux2, uy2, ux3, uy3;
        ApplyMatrix(useCTM, 0, 0, ux0, uy0);
        ApplyMatrix(useCTM, 1, 0, ux1, uy1);
        ApplyMatrix(useCTM, 0, 1, ux2, uy2);
        ApplyMatrix(useCTM, 1, 1, ux3, uy3);

        double minUx = std::min({ ux0, ux1, ux2, ux3 });
        double maxUx = std::max({ ux0, ux1, ux2, ux3 });
        double minUy = std::min({ uy0, uy1, uy2, uy3 });
        double maxUy = std::max({ uy0, uy1, uy2, uy3 });

        // Convert to device coordinates
        int minDx = clampi((int)std::floor(minUx * _scaleX), 0, _w - 1);
        int maxDx = clampi((int)std::ceil(maxUx * _scaleX), 0, _w - 1);
        int minDy = clampi((int)std::floor(_h - maxUy * _scaleY), 0, _h - 1);
        int maxDy = clampi((int)std::ceil(_h - minUy * _scaleY), 0, _h - 1);

        LogDebug("drawImage: page bounds (%.1f,%.1f)-(%.1f,%.1f) -> device (%d,%d)-(%d,%d)",
            minUx, minUy, maxUx, maxUy, minDx, minDy, maxDx, maxDy);

        // sRGB conversion functions
        auto srgbToLinear = [](double c) {
            c /= 255.0;
            return (c <= 0.04045) ? (c / 12.92) : std::pow((c + 0.055) / 1.055, 2.4);
            };

        auto linearToSrgb = [](double c) {
            c = std::clamp(c, 0.0, 1.0);
            return (c <= 0.0031308)
                ? (c * 12.92 * 255.0)
                : ((1.055 * std::pow(c, 1.0 / 2.4) - 0.055) * 255.0);
            };

        // Sample each device pixel
        for (int py = minDy; py <= maxDy; ++py)
        {
            for (int px = minDx; px <= maxDx; ++px)
            {
                // Device -> page coordinates
                double ux = (double)px / _scaleX;
                double uy = ((double)_h - py) / _scaleY;

                // Page -> unit square (inverse CTM)
                double s, t;
                ApplyMatrix(useInv, ux, uy, s, t);

                // Check bounds [0,1] x [0,1]
                if (s < 0 || s > 1 || t < 0 || t > 1) continue;

                // =====================================================
                // Unit square -> image pixel
                // =====================================================

                double fx = s * (imgW - 1);
                double fy = t * (imgH - 1);

                // =====================================================
                // BICUBIC INTERPOLATION (Catmull-Rom spline)
                // Daha keskin sonuç verir, özellikle text içeren imagelarda
                // =====================================================

                auto sample = [&](int x, int y, int c) -> double {
                    x = std::clamp(x, 0, imgW - 1);
                    y = std::clamp(y, 0, imgH - 1);
                    return rgba[(y * imgW + x) * 4 + c];
                    };

                // Catmull-Rom cubic weight function
                auto cubicWeight = [](double t) -> double {
                    t = std::abs(t);
                    if (t <= 1.0)
                        return (1.5 * t - 2.5) * t * t + 1.0;
                    else if (t < 2.0)
                        return ((-0.5 * t + 2.5) * t - 4.0) * t + 2.0;
                    else
                        return 0.0;
                    };

                int ix = (int)std::floor(fx);
                int iy = (int)std::floor(fy);
                double fracX = fx - ix;
                double fracY = fy - iy;

                double out[4] = { 0, 0, 0, 0 };

                // 4x4 kernel sampling
                for (int j = -1; j <= 2; j++)
                {
                    double wy = cubicWeight(fracY - j);
                    for (int i = -1; i <= 2; i++)
                    {
                        double wx = cubicWeight(fracX - i);
                        double w = wx * wy;

                        int sx = ix + i;
                        int sy = iy + j;

                        for (int c = 0; c < 3; c++)
                        {
                            out[c] += srgbToLinear(sample(sx, sy, c)) * w;
                        }
                    }
                }

                // Clamp results (bicubic can overshoot)
                for (int c = 0; c < 3; c++)
                {
                    out[c] = std::clamp(out[c], 0.0, 1.0);
                }

                // ALPHA: Nearest neighbor sampling (prevents bleeding at edges)
                int nearestX = std::clamp((int)std::round(fx), 0, imgW - 1);
                int nearestY = std::clamp((int)std::round(fy), 0, imgH - 1);
                out[3] = rgba[(nearestY * imgW + nearestX) * 4 + 3] / 255.0;

                // Output pixel
                size_t di = (size_t(py) * _w + px) * 4;

                uint8_t srcR = (uint8_t)linearToSrgb(out[0]);
                uint8_t srcG = (uint8_t)linearToSrgb(out[1]);
                uint8_t srcB = (uint8_t)linearToSrgb(out[2]);
                uint8_t srcA = (uint8_t)(out[3] * 255.0);

                // Tamamen şeffaf pikselleri atla
                if (srcA == 0)
                    continue;

                // Alpha blending uygula
                if (srcA < 255)
                {
                    // Arka plan rengini al
                    uint8_t dstB = _buffer[di + 0];
                    uint8_t dstG = _buffer[di + 1];
                    uint8_t dstR = _buffer[di + 2];

                    // Alpha blend: out = src * alpha + dst * (1 - alpha)
                    int alpha = srcA;
                    int invAlpha = 255 - alpha;

                    srcR = (uint8_t)((srcR * alpha + dstR * invAlpha) / 255);
                    srcG = (uint8_t)((srcG * alpha + dstG * invAlpha) / 255);
                    srcB = (uint8_t)((srcB * alpha + dstB * invAlpha) / 255);
                }

                // Write BGRA
                {
                    static FILE* imgOverwriteDebug = nullptr;
                    static int imgOverwriteCount = 0;
                    static bool imgOverwriteInit = false;
                    if (!imgOverwriteInit) {
                        char tempPath[MAX_PATH];
                        GetTempPathA(MAX_PATH, tempPath);
                        strcat_s(tempPath, "img_overwrite_debug.txt");
                        imgOverwriteDebug = fopen(tempPath, "w");
                        imgOverwriteInit = true;
                    }

                    bool isRightEdge = (px > 1100 && px < 1200);
                    bool isMiddleY = (py > 300 && py < 600);
                    uint8_t oldB = _buffer[di + 0];
                    uint8_t oldG = _buffer[di + 1];
                    uint8_t oldR = _buffer[di + 2];
                    bool oldIsColored = !(oldB == 255 && oldG == 255 && oldR == 255);

                    if (imgOverwriteDebug && isRightEdge && isMiddleY && oldIsColored && imgOverwriteCount < 100) {
                        fprintf(imgOverwriteDebug, "*** IMAGE OVERWRITE *** (%d, %d): RGB(%d,%d,%d) -> RGB(%d,%d,%d)\n",
                            px, py, oldR, oldG, oldB, srcR, srcG, srcB);
                        imgOverwriteCount++;
                        fflush(imgOverwriteDebug);
                    }
                }
                _buffer[di + 0] = srcB;
                _buffer[di + 1] = srcG;
                _buffer[di + 2] = srcR;
                _buffer[di + 3] = 255;
            }
        }
    }

    // =====================================================
    // Rect clipping ile image çizme
    // =====================================================
    void PdfPainter::drawImageWithClipRect(
        const std::vector<uint8_t>& rgba,
        int imgW,
        int imgH,
        const PdfMatrix& ctm,
        int clipMinX, int clipMinY, int clipMaxX, int clipMaxY)
    {
        if (imgW <= 0 || imgH <= 0) return;
        if ((int)rgba.size() < imgW * imgH * 4) return;

        LogDebug("drawImageWithClipRect: %dx%d clip=[%d,%d]-[%d,%d]",
            imgW, imgH, clipMinX, clipMinY, clipMaxX, clipMaxY);

        // Compute inverse CTM for sampling
        PdfMatrix inv;
        if (!InvertMatrix(ctm, inv)) {
            return;
        }

        // Compute bounding box in page space (4 köşeyi transform et)
        double ux0, uy0, ux1, uy1, ux2, uy2, ux3, uy3;
        ApplyMatrix(ctm, 0, 0, ux0, uy0);
        ApplyMatrix(ctm, 1, 0, ux1, uy1);
        ApplyMatrix(ctm, 0, 1, ux2, uy2);
        ApplyMatrix(ctm, 1, 1, ux3, uy3);

        double minUx = std::min({ ux0, ux1, ux2, ux3 });
        double maxUx = std::max({ ux0, ux1, ux2, ux3 });
        double minUy = std::min({ uy0, uy1, uy2, uy3 });
        double maxUy = std::max({ uy0, uy1, uy2, uy3 });

        // Convert to device coordinates
        int minDx = clampi((int)std::floor(minUx * _scaleX), 0, _w - 1);
        int maxDx = clampi((int)std::ceil(maxUx * _scaleX), 0, _w - 1);
        int minDy = clampi((int)std::floor(_h - maxUy * _scaleY), 0, _h - 1);
        int maxDy = clampi((int)std::ceil(_h - minUy * _scaleY), 0, _h - 1);

        // Clipping rect ile kesişim al
        minDx = std::max(minDx, clipMinX);
        maxDx = std::min(maxDx, clipMaxX);
        minDy = std::max(minDy, clipMinY);
        maxDy = std::min(maxDy, clipMaxY);

        if (minDx >= maxDx || minDy >= maxDy) {
            return;
        }


        // sRGB conversion functions
        auto srgbToLinear = [](double c) {
            c /= 255.0;
            return (c <= 0.04045) ? (c / 12.92) : std::pow((c + 0.055) / 1.055, 2.4);
            };

        auto linearToSrgb = [](double c) {
            c = std::clamp(c, 0.0, 1.0);
            return (c <= 0.0031308)
                ? (c * 12.92 * 255.0)
                : ((1.055 * std::pow(c, 1.0 / 2.4) - 0.055) * 255.0);
            };

        // Sample each device pixel (only within clipped area)
        for (int py = minDy; py <= maxDy; ++py)
        {
            for (int px = minDx; px <= maxDx; ++px)
            {
                // Device -> page coordinates
                double ux = (double)px / _scaleX;
                double uy = ((double)_h - py) / _scaleY;

                // Page -> unit square (inverse CTM)
                double s, t;
                ApplyMatrix(inv, ux, uy, s, t);

                // Check bounds [0,1] x [0,1]
                if (s < 0 || s > 1 || t < 0 || t > 1) continue;

                double fx = s * (imgW - 1);
                double fy = t * (imgH - 1);

                // BICUBIC INTERPOLATION (Catmull-Rom)
                auto sample = [&](int x, int y, int c) -> double {
                    x = std::clamp(x, 0, imgW - 1);
                    y = std::clamp(y, 0, imgH - 1);
                    return rgba[(y * imgW + x) * 4 + c];
                    };

                auto cubicWeight = [](double t) -> double {
                    t = std::abs(t);
                    if (t <= 1.0)
                        return (1.5 * t - 2.5) * t * t + 1.0;
                    else if (t < 2.0)
                        return ((-0.5 * t + 2.5) * t - 4.0) * t + 2.0;
                    else
                        return 0.0;
                    };

                int ix = (int)std::floor(fx);
                int iy = (int)std::floor(fy);
                double fracX = fx - ix;
                double fracY = fy - iy;

                double out[4] = { 0, 0, 0, 0 };

                for (int j = -1; j <= 2; j++)
                {
                    double wy = cubicWeight(fracY - j);
                    for (int i = -1; i <= 2; i++)
                    {
                        double wx = cubicWeight(fracX - i);
                        double w = wx * wy;

                        for (int c = 0; c < 3; c++)
                            out[c] += srgbToLinear(sample(ix + i, iy + j, c)) * w;
                    }
                }

                for (int c = 0; c < 3; c++)
                    out[c] = std::clamp(out[c], 0.0, 1.0);


                // ALPHA: Nearest neighbor sampling (prevents bleeding at edges)
                int nearestX = std::clamp((int)std::round(fx), 0, imgW - 1);
                int nearestY = std::clamp((int)std::round(fy), 0, imgH - 1);
                out[3] = rgba[(nearestY * imgW + nearestX) * 4 + 3] / 255.0;
                // Output pixel
                size_t di = (size_t(py) * _w + px) * 4;

                uint8_t srcR = (uint8_t)linearToSrgb(out[0]);
                uint8_t srcG = (uint8_t)linearToSrgb(out[1]);
                uint8_t srcB = (uint8_t)linearToSrgb(out[2]);
                uint8_t srcA = (uint8_t)(out[3] * 255.0);

                // Tamamen şeffaf pikselleri atla
                if (srcA == 0)
                    continue;

                // Alpha blending uygula
                if (srcA < 255)
                {
                    // Arka plan rengini al
                    uint8_t dstB = _buffer[di + 0];
                    uint8_t dstG = _buffer[di + 1];
                    uint8_t dstR = _buffer[di + 2];

                    // Alpha blend: out = src * alpha + dst * (1 - alpha)
                    int alpha = srcA;
                    int invAlpha = 255 - alpha;

                    srcR = (uint8_t)((srcR * alpha + dstR * invAlpha) / 255);
                    srcG = (uint8_t)((srcG * alpha + dstG * invAlpha) / 255);
                    srcB = (uint8_t)((srcB * alpha + dstB * invAlpha) / 255);
                }

                // Write BGRA
                _buffer[di + 0] = srcB;
                _buffer[di + 1] = srcG;
                _buffer[di + 2] = srcR;
                _buffer[di + 3] = 255;
            }
        }
    }

    // =====================================================
    // Clipping path destekli image çizme
    // =====================================================
    void PdfPainter::drawImageClipped(
        const std::vector<uint8_t>& rgba,
        int imgW,
        int imgH,
        const PdfMatrix& imageCTM,
        const std::vector<PdfPathSegment>& clipPath,
        const PdfMatrix& clipCTM,
        bool hasRectClip,
        double rectMinX, double rectMinY,
        double rectMaxX, double rectMaxY)
    {
        if (imgW <= 0 || imgH <= 0) return;
        if ((int)rgba.size() < imgW * imgH * 4) return;

        // Clipping path yoksa normal drawImage kullan
        if (clipPath.empty()) {
            drawImage(rgba, imgW, imgH, imageCTM);
            return;
        }

        // =====================================================
        // CRITICAL FIX: Her iki CTM de AYNI koordinat sisteminde olmalı!
        // 
        // imageCTM ve clipCTM'in d değerleri aynı işaretli olmalı.
        // Eğer imageCTM.d < 0 ve clipCTM.d < 0 ise, ikisi de aynı
        // Y-flip'e sahip demektir.
        //
        // Flip'i CTM'de DEĞİL, sadece texture sampling'de uygulayacağız.
        // Bu sayede imageCTM ve clipCTM aynı koordinat sisteminde kalır.
        // =====================================================

        bool flipX = (imageCTM.a < 0);
        bool flipY = (imageCTM.d < 0);

        LogDebug("drawImageClipped: imageCTM=[%.2f %.2f %.2f %.2f %.2f %.2f] flipX=%d flipY=%d",
            imageCTM.a, imageCTM.b, imageCTM.c, imageCTM.d, imageCTM.e, imageCTM.f,
            flipX ? 1 : 0, flipY ? 1 : 0);
        LogDebug("drawImageClipped: clipCTM=[%.2f %.2f %.2f %.2f %.2f %.2f]",
            clipCTM.a, clipCTM.b, clipCTM.c, clipCTM.d, clipCTM.e, clipCTM.f);

        // =====================================================
        // Clipping polygon oluştur (clipCTM ile)
        // =====================================================
        std::vector<DPoint> clipPoly;
        double clipCpX = 0, clipCpY = 0;  // Current point for bezier

        const double tolPx = 0.05;
        const double tolPxSq = tolPx * tolPx;

        for (const auto& seg : clipPath) {
            double px, py;
            if (seg.type == PdfPathSegment::MoveTo) {
                ApplyMatrix(clipCTM, seg.x, seg.y, px, py);
                double dx = px * _scaleX;
                double dy = _h - py * _scaleY;
                clipPoly.push_back({ dx, dy });
                clipCpX = seg.x;
                clipCpY = seg.y;
            }
            else if (seg.type == PdfPathSegment::LineTo) {
                ApplyMatrix(clipCTM, seg.x, seg.y, px, py);
                double dx = px * _scaleX;
                double dy = _h - py * _scaleY;
                clipPoly.push_back({ dx, dy });
                clipCpX = seg.x;
                clipCpY = seg.y;
            }
            else if (seg.type == PdfPathSegment::CurveTo) {
                double x0d, y0d, x1d, y1d, x2d, y2d, x3d, y3d;

                ApplyMatrix(clipCTM, clipCpX, clipCpY, px, py);
                x0d = px * _scaleX;
                y0d = _h - py * _scaleY;

                ApplyMatrix(clipCTM, seg.x1, seg.y1, px, py);
                x1d = px * _scaleX;
                y1d = _h - py * _scaleY;

                ApplyMatrix(clipCTM, seg.x2, seg.y2, px, py);
                x2d = px * _scaleX;
                y2d = _h - py * _scaleY;

                ApplyMatrix(clipCTM, seg.x3, seg.y3, px, py);
                x3d = px * _scaleX;
                y3d = _h - py * _scaleY;

                flattenCubicBezierDeviceD(
                    x0d, y0d,
                    x1d, y1d,
                    x2d, y2d,
                    x3d, y3d,
                    clipPoly,
                    tolPxSq
                );

                clipCpX = seg.x3;
                clipCpY = seg.y3;
            }
        }

        if (clipPoly.size() < 3) {
            drawImage(rgba, imgW, imgH, imageCTM);
            return;
        }

        // =====================================================
        // NO TRANSFORM: Clipping path'i HİÇ değiştirme
        // 
        // clipCTM ile device space'e çevrilmiş clipping polygon'u
        // olduğu gibi kullan. Belki sorun başka yerde.
        // =====================================================


        // Point-in-polygon test (ray casting)
        auto pointInPolygon = [&clipPoly](double testX, double testY) -> bool {
            int n = (int)clipPoly.size();
            bool inside = false;
            for (int i = 0, j = n - 1; i < n; j = i++) {
                double xi = clipPoly[i].x, yi = clipPoly[i].y;
                double xj = clipPoly[j].x, yj = clipPoly[j].y;
                if (((yi > testY) != (yj > testY)) &&
                    (testX < (xj - xi) * (testY - yi) / (yj - yi) + xi)) {
                    inside = !inside;
                }
            }
            return inside;
            };

        // Clipping polygon bounding box (fit sonrası - device space)
        double finalClipMinX = clipPoly[0].x, finalClipMaxX = clipPoly[0].x;
        double finalClipMinY = clipPoly[0].y, finalClipMaxY = clipPoly[0].y;
        for (const auto& pt : clipPoly) {
            finalClipMinX = std::min(finalClipMinX, pt.x);
            finalClipMaxX = std::max(finalClipMaxX, pt.x);
            finalClipMinY = std::min(finalClipMinY, pt.y);
            finalClipMaxY = std::max(finalClipMaxY, pt.y);
        }

        LogDebug("drawImageClipped: clipBBox=[%.1f,%.1f -> %.1f,%.1f], %zu vertices",
            finalClipMinX, finalClipMinY, finalClipMaxX, finalClipMaxY, clipPoly.size());

        // =====================================================
        // CRITICAL: Orijinal imageCTM'i kullan (flip uygulamadan)
        // Böylece imageCTM ve clipCTM aynı koordinat sisteminde
        // =====================================================
        PdfMatrix inv;
        if (!InvertMatrix(imageCTM, inv)) return;

        // Image bounding box (device space) - orijinal imageCTM ile
        double ux0, uy0, ux1, uy1, ux2, uy2, ux3, uy3;
        ApplyMatrix(imageCTM, 0, 0, ux0, uy0);
        ApplyMatrix(imageCTM, 1, 0, ux1, uy1);
        ApplyMatrix(imageCTM, 0, 1, ux2, uy2);
        ApplyMatrix(imageCTM, 1, 1, ux3, uy3);

        double minUx = std::min({ ux0, ux1, ux2, ux3 });
        double maxUx = std::max({ ux0, ux1, ux2, ux3 });
        double minUy = std::min({ uy0, uy1, uy2, uy3 });
        double maxUy = std::max({ uy0, uy1, uy2, uy3 });

        int minDx = clampi((int)std::floor(minUx * _scaleX), 0, _w - 1);
        int maxDx = clampi((int)std::ceil(maxUx * _scaleX), 0, _w - 1);
        int minDy = clampi((int)std::floor(_h - maxUy * _scaleY), 0, _h - 1);
        int maxDy = clampi((int)std::ceil(_h - minUy * _scaleY), 0, _h - 1);

        // Clip bbox ile kesişim
        minDx = std::max(minDx, (int)std::floor(finalClipMinX));
        maxDx = std::min(maxDx, (int)std::ceil(finalClipMaxX));
        minDy = std::max(minDy, (int)std::floor(finalClipMinY));
        maxDy = std::min(maxDy, (int)std::ceil(finalClipMaxY));

        // =====================================================
        // RECT CLIPPING: Ek bbox kısıtlaması (oval'ın hangi parçası)
        // =====================================================
        if (hasRectClip) {
            LogDebug("drawImageClipped: Applying rect clip [%.1f,%.1f -> %.1f,%.1f]",
                rectMinX, rectMinY, rectMaxX, rectMaxY);
            minDx = std::max(minDx, (int)std::floor(rectMinX));
            maxDx = std::min(maxDx, (int)std::ceil(rectMaxX));
            minDy = std::max(minDy, (int)std::floor(rectMinY));
            maxDy = std::min(maxDy, (int)std::ceil(rectMaxY));
        }


        auto srgbToLinear = [](double c) {
            c /= 255.0;
            return (c <= 0.04045) ? (c / 12.92) : std::pow((c + 0.055) / 1.055, 2.4);
            };

        auto linearToSrgb = [](double c) {
            c = std::clamp(c, 0.0, 1.0);
            return (c <= 0.0031308)
                ? (c * 12.92 * 255.0)
                : ((1.055 * std::pow(c, 1.0 / 2.4) - 0.055) * 255.0);
            };

        for (int py = minDy; py <= maxDy; ++py) {
            for (int px = minDx; px <= maxDx; ++px) {
                // Clipping test
                if (!pointInPolygon((double)px, (double)py))
                    continue;

                double ux = (double)px / _scaleX;
                double uy = ((double)_h - py) / _scaleY;

                double s, t;
                ApplyMatrix(inv, ux, uy, s, t);
                if (s < 0 || s > 1 || t < 0 || t > 1) continue;

                // =====================================================
                // Flip'i SADECE texture sampling'de uygula
                // 
                // drawImage'dan FARKLI: Orada finalMtx (d pozitif) kullanılıyor,
                // burada orijinal imageCTM (d negatif olabilir) kullanılıyor.
                //
                // imageCTM.d < 0 ise:
                //   t=0 → device yukarı, t=1 → device aşağı
                //   Image row 0 = visual bottom (PDF bottom-up)
                //   Device aşağı → row 0 istiyoruz
                //   Yani imgT = 1 - t gerekli
                //
                // imageCTM.d > 0 ise:
                //   t=0 → device aşağı, t=1 → device yukarı
                //   Device aşağı → row 0 istiyoruz
                //   Yani imgT = t gerekli
                // =====================================================
                double imgS = flipX ? (1.0 - s) : s;
                double imgT = flipY ? (1.0 - t) : t;

                double fx = imgS * (imgW - 1);
                double fy = imgT * (imgH - 1);

                // BICUBIC INTERPOLATION (Catmull-Rom)
                auto sample = [&](int x, int y, int c) -> double {
                    x = std::clamp(x, 0, imgW - 1);
                    y = std::clamp(y, 0, imgH - 1);
                    return rgba[(y * imgW + x) * 4 + c];
                    };

                auto cubicWeight = [](double t) -> double {
                    t = std::abs(t);
                    if (t <= 1.0)
                        return (1.5 * t - 2.5) * t * t + 1.0;
                    else if (t < 2.0)
                        return ((-0.5 * t + 2.5) * t - 4.0) * t + 2.0;
                    else
                        return 0.0;
                    };

                int ix = (int)std::floor(fx);
                int iy = (int)std::floor(fy);
                double fracX = fx - ix;
                double fracY = fy - iy;

                double out[4] = { 0, 0, 0, 0 };

                for (int j = -1; j <= 2; j++)
                {
                    double wy = cubicWeight(fracY - j);
                    for (int i = -1; i <= 2; i++)
                    {
                        double wx = cubicWeight(fracX - i);
                        double w = wx * wy;

                        for (int c = 0; c < 3; c++)
                            out[c] += srgbToLinear(sample(ix + i, iy + j, c)) * w;
                    }
                }

                for (int c = 0; c < 3; c++)
                    out[c] = std::clamp(out[c], 0.0, 1.0);

                size_t di = (size_t(py) * _w + px) * 4;

                uint8_t srcR = (uint8_t)linearToSrgb(out[0]);
                uint8_t srcG = (uint8_t)linearToSrgb(out[1]);
                uint8_t srcB = (uint8_t)linearToSrgb(out[2]);

                // =====================================================
                // BEYAZ PİKSELLERİ SAYDAM YAP (Adobe uyumluluğu)
                // Threshold düşürüldü (250→220) JPEG artifacts için
                // =====================================================
                const uint8_t WHITE_THRESHOLD = 220;

                if (srcR >= WHITE_THRESHOLD && srcG >= WHITE_THRESHOLD && srcB >= WHITE_THRESHOLD) {
                    continue; // Bu pikseli çizme, arka plan görünsün
                }

                _buffer[di + 0] = srcB;
                _buffer[di + 1] = srcG;
                _buffer[di + 2] = srcR;
                _buffer[di + 3] = 255;
            }
        }
    }


    void PdfPainter::clear(uint32_t bgraColor)
    {
        const int N = _w * _h;
        for (int i = 0; i < N; i++) std::memcpy(&_buffer[i * 4], &bgraColor, 4);
    }

    inline void PdfPainter::putPixel(int x, int y, uint32_t argb)
    {
        if ((unsigned)x >= (unsigned)_w || (unsigned)y >= (unsigned)_h)
            return;

        static FILE* pixelDebug = nullptr;
        static int pixelLogCount = 0;
        static bool pixelDebugInit = false;
        if (!pixelDebugInit) {
            char tempPath[MAX_PATH];
            GetTempPathA(MAX_PATH, tempPath);
            strcat_s(tempPath, "pixel_debug.txt");
            pixelDebug = fopen(tempPath, "w");
            pixelDebugInit = true;
        }

        uint8_t* p = &_buffer[(y * _w + x) * 4];

        // Log only specific area
        bool isRightEdge = (x > 1100 && x < 1200);
        bool isMiddleY = (y > 400 && y < 500);

        // Check current pixel color before writing
        bool oldIsWhite = (p[0] == 255 && p[1] == 255 && p[2] == 255);
        bool newIsWhite = ((argb & 0x00FFFFFF) == 0x00FFFFFF);

        if (pixelDebug && isRightEdge && isMiddleY) {
            // Log if overwriting colored pixel with white
            if (!oldIsWhite && newIsWhite && pixelLogCount < 200) {
                uint32_t oldColor = (p[3] << 24) | (p[2] << 16) | (p[1] << 8) | p[0];
                fprintf(pixelDebug, "*** OVERWRITE *** (%d, %d): 0x%08X -> 0x%08X (colored -> white!)\n",
                    x, y, oldColor, argb);
                pixelLogCount++;
                fflush(pixelDebug);
            }
            // Log first few colored pixels
            else if (!newIsWhite && pixelLogCount < 50) {
                fprintf(pixelDebug, "putPixel(%d, %d, 0x%08X)\n", x, y, argb);
                pixelLogCount++;
                fflush(pixelDebug);
            }
        }

        // argb = 0xAARRGGBB
        uint8_t a = (argb >> 24) & 0xFF;
        uint8_t r = (argb >> 16) & 0xFF;
        uint8_t g = (argb >> 8) & 0xFF;
        uint8_t b = (argb) & 0xFF;

        // BGRA yaz
        p[0] = b;
        p[1] = g;
        p[2] = r;
        p[3] = a;
    }


    double PdfPainter::mapY(double y) const { return (double)_h - y; }

    void PdfPainter::applyRotate(double& x, double& y) const
    {
        if (!_hasRotate) return;
        double rx = _rotA * x + _rotC * y + _rotTx;
        double ry = _rotB * x + _rotD * y + _rotTy;
        x = rx; y = ry;
    }

    void PdfPainter::setPageRotation(int degrees, double pageWPt, double pageHPt)
    {
        _hasRotate = false;
        if (degrees == 0) return;

        double w = pageWPt * _scaleX;
        double h = pageHPt * _scaleY;
        double rad = degrees * M_PI / 180.0;
        double cosr = std::cos(rad);
        double sinr = std::sin(rad);

        _rotA = cosr; _rotB = sinr; _rotC = -sinr; _rotD = cosr;

        if (degrees == 90) { _rotTx = h; _rotTy = 0; }
        else if (degrees == 180) { _rotTx = w; _rotTy = h; }
        else if (degrees == 270) { _rotTx = 0; _rotTy = w; }

        _hasRotate = true;
    }

    void PdfPainter::fillRect(double x, double y, double w, double h, uint32_t color)
    {
        if (w <= 0 || h <= 0) return;
        double x1 = x * _scaleX;
        double y1 = y * _scaleY;
        double x2 = (x + w) * _scaleX;
        double y2 = (y + h) * _scaleY;
        double yy1 = mapY(y1);
        double yy2 = mapY(y2);

        int ix1 = (int)std::round(x1);
        int iy1 = (int)std::round(yy1);
        int ix2 = (int)std::round(x2);
        int iy2 = (int)std::round(yy2);

        if (ix1 > ix2) std::swap(ix1, ix2);
        if (iy1 > iy2) std::swap(iy1, iy2);

        for (int yy = iy1; yy < iy2; yy++)
        {
            for (int xx = ix1; xx < ix2; xx++)
            {
                double rx = (double)xx;
                double ry = (double)yy;
                applyRotate(rx, ry);
                putPixel((int)rx, (int)ry, color);
            }
        }
    }

    void PdfPainter::drawLine(double x1, double y1, double x2, double y2, uint32_t color)
    {
        double X1 = x1 * _scaleX;
        double Y1 = mapY(y1 * _scaleY);
        double X2 = x2 * _scaleX;
        double Y2 = mapY(y2 * _scaleY);

        int steps = (int)std::max(std::abs(X2 - X1), std::abs(Y2 - Y1));
        if (steps <= 0) steps = 1;

        double sx = (X2 - X1) / steps;
        double sy = (Y2 - Y1) / steps;
        double px = X1;
        double py = Y1;

        for (int i = 0; i <= steps; ++i)
        {
            double rx = px;
            double ry = py;
            applyRotate(rx, ry);
            putPixel((int)rx, (int)ry, color);
            px += sx; py += sy;
        }
    }

    void PdfPainter::drawText(
        double x,
        double y,
        const std::wstring& text,
        double fontSizePt,
        uint32_t color)
    {
        if (text.empty()) return;
        int fontSize = (int)std::round(fontSizePt * _scaleY);
        if (fontSize < 4) fontSize = 4;

        double px = x * _scaleX;
        double py = mapY(y * _scaleY) - fontSize;

        int width = (int)(text.length() * fontSize * 0.65) + 10;
        int height = fontSize + 8;
        if (width < 1 || height < 1) return;

        HDC hdc = CreateCompatibleDC(NULL);
        if (!hdc) return;

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (!bmp) { DeleteDC(hdc); return; }

        HBITMAP oldBmp = (HBITMAP)SelectObject(hdc, bmp);
        HFONT font = CreateFontW(-fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, FF_DONTCARE, L"Arial");
        HFONT oldFont = (HFONT)SelectObject(hdc, font);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));
        TextOutW(hdc, 0, 0, text.c_str(), (int)text.length());

        uint32_t* src = (uint32_t*)bits;
        for (int yy = 0; yy < height; yy++)
        {
            for (int xx = 0; xx < width; xx++)
            {
                uint32_t p = src[yy * width + xx];
                if ((p & 0x00FFFFFFu) == 0) continue;
                double dx = px + xx;
                double dy = py + yy;
                applyRotate(dx, dy);
                putPixel((int)dx, (int)dy, color);
            }
        }

        SelectObject(hdc, oldFont);
        SelectObject(hdc, oldBmp);
        DeleteObject(font);
        DeleteObject(bmp);
        DeleteDC(hdc);
    }

    void PdfPainter::drawGlyph(double x, double y, double w, double h, uint32_t c)
    {
        fillRect(x, y, w, h, c);
    }

    // === YARDIMCI: Path'i polygon'a dönüştür ===
    static void pathToPolygons(
        const PdfPath& path,
        const PdfMatrix& ctm,
        double scaleX, double scaleY, int h,
        std::vector<std::vector<IPoint>>& outPolys,
        double tolPxSq = 0.0025)
    {
        std::vector<DPoint> cur;
        double curUx = 0.0, curUy = 0.0;
        double startUx = 0.0, startUy = 0.0;
        bool hasSubpath = false;

        auto userToDevice = [&](double ux, double uy, double& dx, double& dy) {
            dx = ctm.a * ux + ctm.c * uy + ctm.e;
            dy = ctm.b * ux + ctm.d * uy + ctm.f;
            dx *= scaleX;
            dy = (double)h - (dy * scaleY);
            };

        auto flush = [&]() {
            if (cur.size() >= 3) {
                const DPoint& a = cur.front();
                const DPoint& b = cur.back();
                if (std::abs(a.x - b.x) > 1e-6 || std::abs(a.y - b.y) > 1e-6)
                    cur.push_back(a);

                std::vector<IPoint> ip;
                ip.reserve(cur.size());
                for (auto& p : cur) {
                    ip.push_back({ (int)std::lround(p.x), (int)std::lround(p.y) });
                }
                if (ip.size() >= 3)
                    outPolys.push_back(std::move(ip));
            }
            cur.clear();
            hasSubpath = false;
            };

        for (const auto& seg : path) {
            if (seg.type == PdfPathSegment::MoveTo) {
                flush();
                curUx = startUx = seg.x;
                curUy = startUy = seg.y;
                hasSubpath = true;
                double dx, dy;
                userToDevice(curUx, curUy, dx, dy);
                cur.push_back({ dx, dy });
            }
            else if (seg.type == PdfPathSegment::LineTo) {
                if (!hasSubpath) {
                    curUx = startUx = seg.x;
                    curUy = startUy = seg.y;
                    hasSubpath = true;
                    double dx, dy;
                    userToDevice(curUx, curUy, dx, dy);
                    cur.push_back({ dx, dy });
                    continue;
                }
                curUx = seg.x;
                curUy = seg.y;
                double dx, dy;
                userToDevice(curUx, curUy, dx, dy);
                cur.push_back({ dx, dy });
            }
            else if (seg.type == PdfPathSegment::CurveTo) {
                if (!hasSubpath) continue;
                double x0d, y0d, x1d, y1d, x2d, y2d, x3d, y3d;
                userToDevice(curUx, curUy, x0d, y0d);
                userToDevice(seg.x1, seg.y1, x1d, y1d);
                userToDevice(seg.x2, seg.y2, x2d, y2d);
                userToDevice(seg.x3, seg.y3, x3d, y3d);
                // Flatten bezier
                std::function<void(double, double, double, double, double, double, double, double)> flatten;
                flatten = [&](double ax, double ay, double bx, double by, double cx, double cy, double ex, double ey) {
                    double mx = (ax + 3 * bx + 3 * cx + ex) / 8.0;
                    double my = (ay + 3 * by + 3 * cy + ey) / 8.0;
                    double dx1 = mx - (ax + ex) / 2.0;
                    double dy1 = my - (ay + ey) / 2.0;
                    if (dx1 * dx1 + dy1 * dy1 < tolPxSq) {
                        cur.push_back({ ex, ey });
                    }
                    else {
                        double ab_x = (ax + bx) / 2.0, ab_y = (ay + by) / 2.0;
                        double bc_x = (bx + cx) / 2.0, bc_y = (by + cy) / 2.0;
                        double cd_x = (cx + ex) / 2.0, cd_y = (cy + ey) / 2.0;
                        double abc_x = (ab_x + bc_x) / 2.0, abc_y = (ab_y + bc_y) / 2.0;
                        double bcd_x = (bc_x + cd_x) / 2.0, bcd_y = (bc_y + cd_y) / 2.0;
                        double m_x = (abc_x + bcd_x) / 2.0, m_y = (abc_y + bcd_y) / 2.0;
                        flatten(ax, ay, ab_x, ab_y, abc_x, abc_y, m_x, m_y);
                        flatten(m_x, m_y, bcd_x, bcd_y, cd_x, cd_y, ex, ey);
                    }
                    };
                flatten(x0d, y0d, x1d, y1d, x2d, y2d, x3d, y3d);
                curUx = seg.x3;
                curUy = seg.y3;
            }
            else if (seg.type == PdfPathSegment::Close) {
                if (hasSubpath) {
                    double dx, dy;
                    userToDevice(startUx, startUy, dx, dy);
                    cur.push_back({ dx, dy });
                }
                flush();
            }
        }
        flush();
    }

    // === YARDIMCI: Scanline için clipping X span'lerini hesapla ===
    static void getClipSpansForScanline(
        int y,
        const std::vector<std::vector<IPoint>>& clipPolys,
        bool clipEvenOdd,
        std::vector<std::pair<int, int>>& outSpans)
    {
        outSpans.clear();

        if (clipEvenOdd) {
            // Even-odd: kesişim noktalarını topla
            std::vector<int> xs;
            xs.reserve(64);

            for (const auto& poly : clipPolys) {
                for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
                    const IPoint& p1 = poly[j];
                    const IPoint& p2 = poly[i];
                    if (p1.y == p2.y) continue;

                    int yMin = std::min(p1.y, p2.y);
                    int yMax = std::max(p1.y, p2.y);
                    if (y < yMin || y >= yMax) continue;

                    double t = (double)(y - p1.y) / (double)(p2.y - p1.y);
                    int x = (int)std::lround(p1.x + t * (p2.x - p1.x));
                    xs.push_back(x);
                }
            }

            std::sort(xs.begin(), xs.end());
            for (size_t i = 0; i + 1 < xs.size(); i += 2) {
                outSpans.emplace_back(xs[i], xs[i + 1]);
            }
        }
        else {
            // Non-zero winding
            std::vector<std::pair<int, int>> edges;
            edges.reserve(64);

            for (const auto& poly : clipPolys) {
                for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
                    const IPoint& p1 = poly[j];
                    const IPoint& p2 = poly[i];
                    if (p1.y == p2.y) continue;

                    int yMin = std::min(p1.y, p2.y);
                    int yMax = std::max(p1.y, p2.y);
                    if (y < yMin || y >= yMax) continue;

                    double t = (double)(y - p1.y) / (double)(p2.y - p1.y);
                    int x = (int)std::lround(p1.x + t * (p2.x - p1.x));
                    int w = (p2.y > p1.y) ? +1 : -1;
                    edges.emplace_back(x, w);
                }
            }

            std::sort(edges.begin(), edges.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });

            int wsum = 0;
            for (size_t i = 0; i + 1 < edges.size(); ++i) {
                wsum += edges[i].second;
                if (wsum != 0) {
                    outSpans.emplace_back(edges[i].first, edges[i + 1].first);
                }
            }
        }
    }

    // === YARDIMCI: İki span listesinin kesişimini al ===
    static void intersectSpans(
        const std::vector<std::pair<int, int>>& fillSpans,
        const std::vector<std::pair<int, int>>& clipSpans,
        std::vector<std::pair<int, int>>& outSpans)
    {
        outSpans.clear();

        for (const auto& fs : fillSpans) {
            for (const auto& cs : clipSpans) {
                int x1 = std::max(fs.first, cs.first);
                int x2 = std::min(fs.second, cs.second);
                if (x1 < x2) {
                    outSpans.emplace_back(x1, x2);
                }
            }
        }
    }

    void PdfPainter::fillPath(
        const PdfPath& path,
        uint32_t color,
        const PdfMatrix& ctm,
        bool evenOdd,
        const PdfPath* clipPath,
        const PdfMatrix* clipCTM,
        bool clipEvenOdd)
    {
        static FILE* debugFile = nullptr;
        static int callCount = 0;
        if (!debugFile) {
            // TEMP klasörüne yaz (C:\ yerine)
            char tempPath[MAX_PATH];
            GetTempPathA(MAX_PATH, tempPath);
            strcat_s(tempPath, "bezier_debug.txt");
            debugFile = fopen(tempPath, "w");
            if (debugFile) {
            }
        }
        callCount++;

        int curveCount = 0;
        int lineCount = 0;
        int moveCount = 0;
        for (const auto& seg : path) {
            if (seg.type == PdfPathSegment::CurveTo) curveCount++;
            else if (seg.type == PdfPathSegment::LineTo) lineCount++;
            else if (seg.type == PdfPathSegment::MoveTo) moveCount++;
        }

        // === Build polygon list from path ===
        std::vector<std::vector<DPoint>> polys;
        std::vector<DPoint> cur;

        // Daha ince tolerans (daha düzgün eğri)
        const double tolPx = 0.05;
        const double tolPxSq = tolPx * tolPx;

        double curUx = 0.0, curUy = 0.0;
        double startUx = 0.0, startUy = 0.0;
        bool hasSubpath = false;

        auto userToDevice = [&](double ux, double uy, double& dx, double& dy)
            {
                ApplyMatrix(ctm, ux, uy, dx, dy);
                dx *= _scaleX;
                dy = mapY(dy * _scaleY);
                applyRotate(dx, dy);
            };

        auto userToDeviceDebug = [&](double ux, double uy, double& dx, double& dy, bool logIt)
            {
                double afterCTM_x, afterCTM_y;
                ApplyMatrix(ctm, ux, uy, afterCTM_x, afterCTM_y);
                dx = afterCTM_x * _scaleX;
                double beforeMapY = afterCTM_y * _scaleY;
                dy = mapY(beforeMapY);
                applyRotate(dx, dy);

                if (logIt && debugFile && callCount <= 20) {
                    fprintf(debugFile, "    TRANSFORM: user(%.2f,%.2f) -> ctm(%.2f,%.2f) -> scale(%.2f,%.2f) -> mapY(%.2f) -> device(%.2f,%.2f)\n",
                        ux, uy, afterCTM_x, afterCTM_y, dx, beforeMapY, dy, dx, dy);
                }
            };

        auto flush = [&]()
            {
                if (cur.size() >= 3)
                {
                    // Zorunlu kapatma
                    const DPoint& a = cur.front();
                    const DPoint& b = cur.back();
                    if (std::abs(a.x - b.x) > 1e-6 || std::abs(a.y - b.y) > 1e-6)
                        cur.push_back(a);

                    polys.push_back(cur);
                }
                cur.clear();
                hasSubpath = false;
            };

        for (const auto& seg : path)
        {
            if (seg.type == PdfPathSegment::MoveTo)
            {
                flush();
                curUx = startUx = seg.x;
                curUy = startUy = seg.y;
                hasSubpath = true;

                double dx, dy;
                userToDevice(curUx, curUy, dx, dy);
                cur.push_back({ dx, dy });
            }
            else if (seg.type == PdfPathSegment::LineTo)
            {
                if (!hasSubpath)
                {
                    curUx = startUx = seg.x;
                    curUy = startUy = seg.y;
                    hasSubpath = true;

                    double dx, dy;
                    userToDevice(curUx, curUy, dx, dy);
                    cur.push_back({ dx, dy });
                    continue;
                }

                curUx = seg.x;
                curUy = seg.y;

                double dx, dy;
                userToDevice(curUx, curUy, dx, dy);
                cur.push_back({ dx, dy });
            }
            else if (seg.type == PdfPathSegment::CurveTo)
            {
                if (!hasSubpath) continue;

                double x0d, y0d, x1d, y1d, x2d, y2d, x3d, y3d;
                userToDevice(curUx, curUy, x0d, y0d);
                userToDevice(seg.x1, seg.y1, x1d, y1d);
                userToDevice(seg.x2, seg.y2, x2d, y2d);
                userToDevice(seg.x3, seg.y3, x3d, y3d);

                size_t beforeSize = cur.size();

                // Başlangıç noktası (duplicate olmasın)
                addPointUniqueD(cur, x0d, y0d);

                // Cubic flatten (device space)
                flattenCubicBezierDeviceD(
                    x0d, y0d,
                    x1d, y1d,
                    x2d, y2d,
                    x3d, y3d,
                    cur,
                    tolPxSq
                );

                if (debugFile && curveCount > 0 && curveCount <= 20) {
                    fprintf(debugFile, "  CURVE: user(%.2f,%.2f)->(%.2f,%.2f)->(%.2f,%.2f)->(%.2f,%.2f)\n",
                        curUx, curUy, seg.x1, seg.y1, seg.x2, seg.y2, seg.x3, seg.y3);
                    fprintf(debugFile, "         dev (%.2f,%.2f)->(%.2f,%.2f)->(%.2f,%.2f)->(%.2f,%.2f)\n",
                        x0d, y0d, x1d, y1d, x2d, y2d, x3d, y3d);
                    fprintf(debugFile, "         flatten: %zu -> %zu points (+%zu)\n",
                        beforeSize, cur.size(), cur.size() - beforeSize);
                }

                curUx = seg.x3;
                curUy = seg.y3;
            }
            else if (seg.type == PdfPathSegment::Close)
            {
                if (hasSubpath)
                {
                    double dx, dy;
                    userToDevice(startUx, startUy, dx, dy);
                    cur.push_back({ dx, dy });
                }
                flush();
            }
        }

        flush();
        if (polys.empty()) return;

        if (debugFile && callCount >= 2160 && callCount <= 2170) {
            for (size_t pi = 0; pi < polys.size(); pi++) {
                for (size_t vi = 0; vi < polys[pi].size() && vi < 6; vi++) {
                }
                if (polys[pi].size() > 6) fprintf(debugFile, "...");
            }
        }

        // === DOUBLE → INT POLYGON DÖNÜŞÜMÜ ===
        std::vector<std::vector<IPoint>> ipolys;
        ipolys.reserve(polys.size());

        int minY = INT_MAX, maxY = INT_MIN;

        for (auto& poly : polys)
        {
            std::vector<IPoint> ip;
            ip.reserve(poly.size());

            for (auto& p : poly)
            {
                int x = (int)std::lround(p.x);
                int y = (int)std::lround(p.y);
                ip.push_back({ x, y });

                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }

            if (ip.size() >= 3)
                ipolys.push_back(std::move(ip));
        }

        if (ipolys.empty()) return;

        // === CLIPPING POLYGON OLUŞTUR ===
        std::vector<std::vector<IPoint>> clipPolys;
        bool hasClip = (clipPath != nullptr && clipCTM != nullptr && !clipPath->empty());

        if (hasClip) {
            pathToPolygons(*clipPath, *clipCTM, _scaleX, _scaleY, _h, clipPolys);

            if (debugFile && callCount >= 2160 && callCount <= 2170) {
                for (size_t i = 0; i < clipPolys.size() && i < 3; i++) {
                }
            }
        }

        minY = clampi(minY, 0, _h - 1);
        maxY = clampi(maxY, 0, _h - 1);

        // === SCANLINE DOLDURMA ===
        std::vector<std::pair<int, int>> clipSpans;
        std::vector<std::pair<int, int>> fillSpans;
        std::vector<std::pair<int, int>> finalSpans;

        for (int y = minY; y <= maxY; ++y)
        {
            // Clipping span'lerini bu satır için hesapla (eğer clip varsa)
            if (hasClip) {
                getClipSpansForScanline(y, clipPolys, clipEvenOdd, clipSpans);
                if (clipSpans.empty()) continue; // Bu satırda clip yok, atla
            }

            fillSpans.clear();

            if (evenOdd)
            {
                std::vector<int> xs;
                xs.reserve(128);

                for (const auto& poly : ipolys)
                {
                    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
                    {
                        const IPoint& p1 = poly[j];
                        const IPoint& p2 = poly[i];
                        if (p1.y == p2.y) continue;

                        int yMin = std::min(p1.y, p2.y);
                        int yMax = std::max(p1.y, p2.y);
                        if (y < yMin || y >= yMax) continue;

                        double t = (double)(y - p1.y) / (double)(p2.y - p1.y);
                        int x = (int)std::lround(p1.x + t * (p2.x - p1.x));
                        xs.push_back(x);
                    }
                }

                std::sort(xs.begin(), xs.end());
                for (size_t i = 0; i + 1 < xs.size(); i += 2)
                {
                    int x1 = clampi(xs[i], 0, _w - 1);
                    int x2 = clampi(xs[i + 1], 0, _w - 1);
                    if (x2 > x1) fillSpans.emplace_back(x1, x2);
                }
            }
            else
            {
                std::vector<std::pair<int, int>> edges;
                edges.reserve(128);

                for (const auto& poly : ipolys)
                {
                    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
                    {
                        const IPoint& p1 = poly[j];
                        const IPoint& p2 = poly[i];
                        if (p1.y == p2.y) continue;

                        int yMin = std::min(p1.y, p2.y);
                        int yMax = std::max(p1.y, p2.y);
                        if (y < yMin || y >= yMax) continue;

                        double t = (double)(y - p1.y) / (double)(p2.y - p1.y);
                        int x = (int)std::lround(p1.x + t * (p2.x - p1.x));
                        int w = (p2.y > p1.y) ? +1 : -1;
                        edges.emplace_back(x, w);
                    }
                }

                std::sort(edges.begin(), edges.end(),
                    [](const auto& a, const auto& b) { return a.first < b.first; });

                int wsum = 0;
                for (size_t i = 0; i + 1 < edges.size(); ++i)
                {
                    wsum += edges[i].second;
                    if (wsum != 0)
                    {
                        int x1 = clampi(edges[i].first, 0, _w - 1);
                        int x2 = clampi(edges[i + 1].first, 0, _w - 1);
                        if (x2 > x1) fillSpans.emplace_back(x1, x2);
                    }
                }
            }

            // === CLIPPING İLE KESİŞİM AL VE BOYA ===
            if (hasClip) {
                intersectSpans(fillSpans, clipSpans, finalSpans);
                for (const auto& span : finalSpans) {
                    for (int x = span.first; x < span.second; ++x) {
                        putPixel(x, y, color);
                    }
                }
            }
            else {
                for (const auto& span : fillSpans) {
                    for (int x = span.first; x < span.second; ++x) {
                        putPixel(x, y, color);
                    }
                }
            }
        }
    }

    // =========================================================================
    //                 PATTERN FILLING IMPLEMENTATION (TILING TYPE 1)
    // =========================================================================

    // Yardımcı: 3x3 Matrix Inversion
    static bool invertMatrix(const PdfMatrix& m, PdfMatrix& inv)
    {
        double det = m.a * m.d - m.b * m.c;
        if (std::abs(det) < 1e-9) return false;
        double invDet = 1.0 / det;
        inv.a = m.d * invDet;
        inv.b = -m.b * invDet;
        inv.c = -m.c * invDet;
        inv.d = m.a * invDet;
        // e,f (translation) tersi:
        // x' = ax + cy + e => x = invA(x' - e) + invC(y' - f)
        // x = invA*x' + invC*y' + (-invA*e - invC*f)
        inv.e = -(inv.a * m.e + inv.c * m.f);
        inv.f = -(inv.b * m.e + inv.d * m.f);
        return true;
    }

    void PdfPainter::fillPathWithPattern(
        const std::vector<PdfPathSegment>& path,
        const PdfPattern& pattern,
        const PdfMatrix& ctm,
        bool evenOdd)
    {
        if (pattern.width <= 0 || pattern.height <= 0 || pattern.buffer.empty()) return;

        // 1. Path -> Polygons dönüşümü
        std::vector<std::vector<IPoint>> polys;

        // Helper: Path'i polygon'a çevir (mevcut static pathToPolygons fonksiyonunu kullanmak için)
        // pathToPolygons static olduğu için çağırabiliriz.
        pathToPolygons(path, ctm, _scaleX, _scaleY, _h, polys);

        if (polys.empty()) return;

        // 2. Rasterize Patterns (Tüm polygonlar için)
        // Çoklu polygonları flatten etmek yerine tek tek çizelim (Scanline birleştirmesi daha doğru olurdu ama bu da çalışır)
        // Ancak delikler için tüm polygonları birlikte işlemek gerekir.
        // Burada basitçe rasterFillPolygonPattern'e "polys" listesini verecek şekilde overload yapmadık.
        // Bu yüzden tüm IPoint polygonlarını tek bir listede toplayıp rasterizer'a gönderemeyiz (bu yanlıştır).
        // Doğrusu: rasterFillPolygonPattern fonksiyonunu vector<vector<IPoint>> alacak şekilde güncellemek.
        // Ama Header değiştirmek istemiyoruz.
        // O yüzden burada rasterFillPolygonPattern'in içeriğini (scanline loop) buraya alıyoruz (Inline Implementation).

        // A. Tüm polygonları hazırla (IPoint formatında zaten polys var)
        std::vector<std::vector<IPoint>> ipolys = polys;

        // B. Min/Max Y bul
        int minY = INT_MAX, maxY = INT_MIN;
        for (const auto& poly : ipolys) {
            for (const auto& p : poly) {
                minY = std::min(minY, p.y);
                maxY = std::max(maxY, p.y);
            }
        }

        minY = std::max(0, minY);
        maxY = std::min(_h - 1, maxY);

        // C. Matris Terslerini Hazırla
        // CTM Inverse: Device -> User
        // Pattern matris: Pattern -> User
        // User -> Pattern: PatternInv
        // Total: Device -> Pattern = MapYInv * CTMInv * PatternInv

        // Inverse CTM
        double det = ctm.a * ctm.d - ctm.b * ctm.c;
        if (std::abs(det) < 1e-9) return;
        double invDet = 1.0 / det;

        // Inverse Pattern Matrix
        double pDet = pattern.matrix.a * pattern.matrix.d - pattern.matrix.b * pattern.matrix.c;
        double pInvDet = (std::abs(pDet) > 1e-9) ? 1.0 / pDet : 0.0; // Fallback?

        std::vector<std::pair<int, int>> spans;

        for (int y = minY; y <= maxY; y++)
        {
            // Scanline span'lerini al
            getClipSpansForScanline(y, ipolys, evenOdd, spans);

            for (const auto& span : spans)
            {
                int xStart = std::max(0, span.first);
                int xEnd = std::min(_w, span.second);

                for (int x = xStart; x < xEnd; x++) {
                    // 1. Device (x,y) to User (ux, uy)
                    double dx = (double)x / _scaleX;
                    double dy = (double)(_h - y) / _scaleY; // MapY tersi

                    // CTM^-1 * (dx - e, dy - f) -> ux, uy
                    double tx = dx - ctm.e;
                    double ty = dy - ctm.f;

                    double ux = (tx * ctm.d - ty * ctm.c) * invDet;
                    double uy = (ty * ctm.a - tx * ctm.b) * invDet;

                    // 2. User (ux, uy) to Pattern (pu, pv) using Inverse Pattern Matrix
                    // P_user = P_pat * Mat => P_pat = (P_user - e) * Mat^-1
                    double ptx = ux - pattern.matrix.e;
                    double pty = uy - pattern.matrix.f;

                    double pu = 0, pv = 0;
                    if (std::abs(pDet) > 1e-9) {
                        pu = (ptx * pattern.matrix.d - pty * pattern.matrix.c) * pInvDet;
                        pv = (pty * pattern.matrix.a - ptx * pattern.matrix.b) * pInvDet;
                    }

                    // 3. Tiling (Wrapping)
                    // Pattern buffer boyutu: pattern.width x pattern.height (pixel)
                    // pu, pv: Pattern Space coordinates. 
                    // Genelde Pattern Space 1 birim = 1 pixel kabul edilir Mİ?
                    // HAYIR. Pattern Space keyfi bir koordinattır.
                    // XStep, YStep pattern space birimidir.
                    // Desen çizilirken BBox sınırları içinde çizilir.
                    // Bizim pattern.buffer = o BBox'ın render edilmiş hali.
                    // XStep/YStep genelde BBox genişliğine eşittir.
                    // Ve bizim buffer'ımız pattern.width pixel genişliğinde.
                    // Bu durumda: PatternSpaceCoordinate -> PixelCoordinate dönüşümü lazım.
                    // Varsayım: Pattern cell render edilirken, scale faktörü öyle ayarlandı ki
                    // Pattern Space BBox Width = Buffer Width Pixel
                    // Bu durumda: u_pixel = pu * (pattern.width / XStep)

                    double uScale = (pattern.xStep != 0) ? (double)pattern.width / pattern.xStep : 1.0;
                    double vScale = (pattern.yStep != 0) ? (double)pattern.height / pattern.yStep : 1.0;

                    // Tiling: Modulo XStep
                    // Pattern Space'de tiling yapalım, sonra pixele çevirelim.
                    if (pattern.xStep != 0) {
                        pu = std::fmod(pu, pattern.xStep);
                        if (pu < 0) pu += pattern.xStep;
                    }
                    if (pattern.yStep != 0) {
                        pv = std::fmod(pv, pattern.yStep);
                        if (pv < 0) pv += pattern.yStep;
                    }

                    int u = (int)(pu * uScale);
                    int v = (int)(pv * vScale);

                    // Safety Clamp (Modulo sonrası küçük hatalar için)
                    u = std::max(0, std::min(u, pattern.width - 1));

                    // V ekseni PDF'te yukarı doğrudur, Buffer'da aşağı doğrudur.
                    // Eğer Pattern Space de yukarı doğruysa, ters çevirmeliyiz.
                    // Genelde Pattern Content stream'i render ederken Painter Y-Flip yapar.
                    // Yani Buffer Top-Down (0 üstte).
                    // Pattern Space Y: 0 altta.
                    // Bu durumda v'yi ters çevir.
                    // v = pattern.height - 1 - v;
                    // ANCAK, rasterizer Y-Flip yapıyor zaten (mapY).
                    // Bizim buffer'ımız da Y-Flip'li render edildiyse?
                    // Deneyerek göreceğiz. Şimdilik düz alalım.
                    v = std::max(0, std::min(v, pattern.height - 1));
                    // v = pattern.height - 1 - v; // Olası fix

                    int bufIdx = v * pattern.width + u;
                    if (bufIdx < 0 || bufIdx >= pattern.buffer.size()) continue;

                    uint32_t srcColor = pattern.buffer[bufIdx];

                    // Uncolored Pattern Masking
                    if (pattern.isUncolored) {
                        // Alpha of mask * Base Color
                        uint8_t alpha = (srcColor >> 24) & 0xFF;
                        // Veya grayscale intensity? Pattern genelde grayscale render edilir.
                        // Render edilen buffer ARGB ise A kullanılır.

                        uint8_t baseA = (pattern.baseColor >> 24) & 0xFF;
                        uint8_t baseR = (pattern.baseColor >> 16) & 0xFF;
                        uint8_t baseG = (pattern.baseColor >> 8) & 0xFF;
                        uint8_t baseB = (pattern.baseColor) & 0xFF;

                        uint8_t finalA = (uint8_t)((alpha * baseA) / 255);
                        srcColor = (finalA << 24) | (baseR << 16) | (baseG << 8) | baseB;
                    }

                    // Blend process
                    if ((srcColor & 0xFF000000) == 0) continue;

                    // Simple Alpha Blend inline (since putPixel is overwrite)
                    // putPixel(x, y, srcColor); // Overwrite (basit)
                    // Doğrusu blend, ama şimdilik overwrite yeterli olabilir.

                    // Blend Implementation:
                    int dstIdx = y * _w * 4 + x * 4;
                    if (dstIdx >= 0 && dstIdx + 3 < _buffer.size()) {
                        uint8_t db = _buffer[dstIdx + 0];
                        uint8_t dg = _buffer[dstIdx + 1];
                        uint8_t dr = _buffer[dstIdx + 2];

                        uint8_t sa = (srcColor >> 24) & 0xFF;
                        uint8_t sr = (srcColor >> 16) & 0xFF;
                        uint8_t sg = (srcColor >> 8) & 0xFF;
                        uint8_t sb = (srcColor >> 0) & 0xFF;

                        if (sa == 255) {
                            _buffer[dstIdx + 0] = sb;
                            _buffer[dstIdx + 1] = sg;
                            _buffer[dstIdx + 2] = sr;
                            _buffer[dstIdx + 3] = 255;
                        }
                        else {
                            int invA = 255 - sa;
                            _buffer[dstIdx + 0] = (sb * sa + db * invA) / 255;
                            _buffer[dstIdx + 1] = (sg * sa + dg * invA) / 255;
                            _buffer[dstIdx + 2] = (sr * sa + dr * invA) / 255;
                            _buffer[dstIdx + 3] = 255;
                        }
                    }
                }
            }
        }
    }

    void PdfPainter::rasterFillPolygonPattern(
        const std::vector<IPoint>& poly,
        const PdfPattern& pattern,
        bool evenOdd)
    {
        // Redundant - implementation moved to fillPathWithPattern
    }


    void PdfPainter::drawLineDevice(int x1, int y1, int x2, int y2, uint32_t color)
    {
        int dx = std::abs(x2 - x1);
        int dy = std::abs(y2 - y1);
        int sx = (x1 < x2) ? 1 : -1;
        int sy = (y1 < y2) ? 1 : -1;
        int err = dx - dy;
        while (true) {
            putPixel(x1, y1, color);
            if (x1 == x2 && y1 == y2) break;
            int e2 = err * 2;
            if (e2 > -dy) { err -= dy; x1 += sx; }
            if (e2 < dx) { err += dx; y1 += sy; }
        }
    }

    void PdfPainter::rasterFillPolygon(
        const std::vector<IPoint>& poly,
        uint32_t color,
        bool evenOdd)
    {
        if (poly.size() < 3) return;

        int ymin = poly[0].y, ymax = poly[0].y;
        int xmin = poly[0].x, xmax = poly[0].x;
        for (auto& p : poly)
        {
            ymin = std::min(ymin, p.y);
            ymax = std::max(ymax, p.y);
            xmin = std::min(xmin, p.x);
            xmax = std::max(xmax, p.x);
        }

        // DEBUG FILE
        static FILE* fillDebug = nullptr;
        static bool fillDebugInit = false;
        static int fillCallCount = 0;
        if (!fillDebugInit) {
            char tempPath[MAX_PATH];
            GetTempPathA(MAX_PATH, tempPath);
            strcat_s(tempPath, "fill_debug.txt");
            fillDebug = fopen(tempPath, "w");
            fillDebugInit = true;
        }
        fillCallCount++;

        struct Edge
        {
            int y0, y1;
            double x;
            double dx;
            int winding;
        };

        std::vector<Edge> edges;

        for (size_t i = 0; i + 1 < poly.size(); ++i)
        {
            IPoint p0 = poly[i];
            IPoint p1 = poly[i + 1];
            if (p0.y == p1.y) continue;

            bool up = p0.y < p1.y;
            Edge e;
            e.y0 = up ? p0.y : p1.y;
            e.y1 = up ? p1.y : p0.y;
            e.x = up ? p0.x : p1.x;
            e.dx = (double)(p1.x - p0.x) / (double)(p1.y - p0.y);
            e.winding = up ? +1 : -1;
            edges.push_back(e);
        }

        if (fillDebug) {
            fprintf(fillDebug, "\n=== rasterFillPolygon #%d ===\n", fillCallCount);
            fprintf(fillDebug, "Bounds: (%d,%d)-(%d,%d), color=0x%08X\n", xmin, ymin, xmax, ymax, color);
            fprintf(fillDebug, "Total edges: %zu\n", edges.size());

            // Count and log right-side edges
            int rightEdgeCount = 0;
            for (size_t i = 0; i < edges.size(); i++) {
                double edgeMaxX = std::max(edges[i].x, edges[i].x + edges[i].dx * (edges[i].y1 - edges[i].y0));
                if (edgeMaxX > 1200) {
                    fprintf(fillDebug, "  Edge[%zu]: y=%d-%d, x=%.1f, dx=%.4f, w=%d (RIGHT SIDE)\n",
                        i, edges[i].y0, edges[i].y1, edges[i].x, edges[i].dx, edges[i].winding);
                    rightEdgeCount++;
                }
            }
            fprintf(fillDebug, "Right-side edges (X>1200): %d\n", rightEdgeCount);
            fflush(fillDebug);
        }

        int rightPixelCount = 0;

        for (int y = ymin; y < ymax; ++y)
        {
            struct Hit { double x; int w; };
            std::vector<Hit> hits;

            for (auto& e : edges)
            {
                if (y >= e.y0 && y < e.y1)
                {
                    hits.push_back({ e.x + e.dx * (y - e.y0), e.winding });
                }
            }

            std::sort(hits.begin(), hits.end(),
                [](const Hit& a, const Hit& b) { return a.x < b.x; });

            if (evenOdd)
            {
                for (size_t i = 0; i + 1 < hits.size(); i += 2)
                {
                    int x0 = (int)std::ceil(hits[i].x);
                    int x1 = (int)std::floor(hits[i + 1].x);
                    for (int x = x0; x <= x1; ++x) {
                        putPixel(x, y, color);
                        if (x > 1200) rightPixelCount++;
                    }
                }
            }
            else
            {
                int wsum = 0;
                double xstart = 0;
                for (auto& h : hits)
                {
                    int prev = wsum;
                    wsum += h.w;
                    if (prev == 0 && wsum != 0)
                        xstart = h.x;
                    else if (prev != 0 && wsum == 0)
                    {
                        int x0 = (int)std::ceil(xstart);
                        int x1 = (int)std::floor(h.x);
                        for (int x = x0; x <= x1; ++x) {
                            putPixel(x, y, color);
                            if (x > 1200) rightPixelCount++;
                        }
                    }
                }
            }
        }

        if (fillDebug) {
            fprintf(fillDebug, "Pixels drawn with X > 1200: %d\n", rightPixelCount);
            fflush(fillDebug);
        }
    }
    // ---------------------------------------------------------
    // STROKE SUBPATH (PROFESSIONAL - ROUND JOINS & CAPS)
    // ---------------------------------------------------------
    void PdfPainter::strokeSubpath(
        const std::vector<DPoint>& pts,
        bool closed,
        uint32_t color,
        double lineWidthPx,
        int lineJoin,
        int lineCap,
        double miterLimit)
    {
        static FILE* subpathDebug = nullptr;
        static int subpathCount = 0;
        if (!subpathDebug) {
            char tempPath[MAX_PATH];
            GetTempPathA(MAX_PATH, tempPath);
            strcat_s(tempPath, "subpath_debug.txt");
            subpathDebug = fopen(tempPath, "w");
        }
        subpathCount++;

        // Log input
        if (subpathDebug) {
            fprintf(subpathDebug, "\n=== strokeSubpath #%d ===\n", subpathCount);
            fprintf(subpathDebug, "Input: pts.size=%zu, closed=%d, color=0x%08X, lw=%.2f\n",
                pts.size(), closed ? 1 : 0, color, lineWidthPx);

            // Find right edge points in input (X > 1200)
            fprintf(subpathDebug, "Input pts with X > 1200:\n");
            for (size_t i = 0; i < pts.size(); i++) {
                if (pts[i].x > 1200) {
                    fprintf(subpathDebug, "  pts[%zu] = (%.2f, %.2f)\n", i, pts[i].x, pts[i].y);
                }
            }
            fflush(subpathDebug);
        }

        // Check bounds for right edge middle
        bool isRightEdge = false;
        bool isMiddleY = false;
        if (pts.size() >= 1) {
            double minX = pts[0].x, maxX = pts[0].x;
            double minY = pts[0].y, maxY = pts[0].y;
            for (const auto& p : pts) {
                if (p.x < minX) minX = p.x;
                if (p.x > maxX) maxX = p.x;
                if (p.y < minY) minY = p.y;
                if (p.y > maxY) maxY = p.y;
            }
            isRightEdge = (maxX > 1100);
            isMiddleY = (minY > 300 && maxY < 600);
        }

        if (pts.size() < 2) {
            if (subpathDebug && isRightEdge && isMiddleY) {
                fprintf(subpathDebug, "[Subpath #%d] SKIPPED: pts.size()=%zu < 2\n", subpathCount, pts.size());
                fflush(subpathDebug);
            }
            return;
        }
        if (miterLimit <= 0) miterLimit = 10.0;

        const double hw = lineWidthPx * 0.5;
        if (hw <= 0.0) {
            if (subpathDebug && isRightEdge && isMiddleY) {
                fprintf(subpathDebug, "[Subpath #%d] SKIPPED: hw=%.4f <= 0\n", subpathCount, hw);
                fflush(subpathDebug);
            }
            return;
        }

        // DÜZELTME 1: Kapalı path kontrolü düzeltildi
        std::vector<DPoint> P = pts;
        if (closed)
        {
            // Son nokta ilk noktaya çok yakınsa çıkar
            if (P.size() > 1)
            {
                double dx = P.front().x - P.back().x;
                double dy = P.front().y - P.back().y;
                if (dx * dx + dy * dy < 1e-6)
                    P.pop_back();
            }
            // Açık şekilde tekrar ekle
            if (P.size() > 1)
                P.push_back(P.front());
        }

        if (P.size() < 2) return;

        const size_t segN = P.size() - 1;

        struct Seg
        {
            DPoint d;
            DPoint n;
            double len;
        };

        std::vector<Seg> segs(segN);

        // Segment hesaplama
        for (size_t i = 0; i < segN; ++i)
        {
            double vx = P[i + 1].x - P[i].x;
            double vy = P[i + 1].y - P[i].y;
            double L = std::hypot(vx, vy);
            if (L < 1e-10)
            {
                segs[i] = { {0,0}, {0,0}, 0.0 };
                continue;
            }
            double dx = vx / L;
            double dy = vy / L;
            double nx, ny;
            leftNormal2(dx, dy, nx, ny);
            segs[i] = { {dx, dy}, {nx, ny}, L };
        }

        if (subpathDebug) {
            fprintf(subpathDebug, "P.size=%zu, segN=%zu\n", P.size(), segN);
            fprintf(subpathDebug, "Segments with right edge endpoints (X > 1200):\n");
            for (size_t i = 0; i < segN; ++i) {
                bool p0Right = (P[i].x > 1200);
                bool p1Right = (P[i + 1].x > 1200);
                if (p0Right || p1Right) {
                    fprintf(subpathDebug, "  seg[%zu]: (%.1f,%.1f)->(%.1f,%.1f), len=%.1f, d=(%.3f,%.3f), n=(%.3f,%.3f)\n",
                        i, P[i].x, P[i].y, P[i + 1].x, P[i + 1].y, segs[i].len,
                        segs[i].d.x, segs[i].d.y, segs[i].n.x, segs[i].n.y);
                }
            }
            fflush(subpathDebug);
        }

        std::vector<DPoint> leftC;
        std::vector<DPoint> rightC;
        leftC.reserve(P.size() * 2);
        rightC.reserve(P.size() * 2);

        // DÜZELTME 2: Join point fonksiyonu iyileştirildi
        auto addJoinPoint = [&](std::vector<DPoint>& contour,
            const DPoint& V,
            const Seg& s0,
            const Seg& s1,
            bool isLeftSide,
            bool isOuterSide)
            {
                DPoint n0 = isLeftSide ? s0.n : DPoint{ -s0.n.x, -s0.n.y };
                DPoint n1 = isLeftSide ? s1.n : DPoint{ -s1.n.x, -s1.n.y };

                DPoint p0 = { V.x + n0.x * hw, V.y + n0.y * hw };
                DPoint p1 = { V.x + n1.x * hw, V.y + n1.y * hw };

                // İç taraf: direkt bağla
                if (!isOuterSide)
                {
                    pushUniqueD(contour, p1);
                    return;
                }

                // Normaller neredeyse aynıysa join gereksiz
                double dp = dot2(n0.x, n0.y, n1.x, n1.y);
                if (dp > 0.99995)
                {
                    pushUniqueD(contour, p1);
                    return;
                }

                // Round join
                if (lineJoin == 1) // round
                {
                    double a0 = angleOf2(n0.x, n0.y);
                    double a1 = angleOf2(n1.x, n1.y);

                    double sweep = signedAngle(n0, n1); // (-pi, pi)

                    // Dış tarafta kalmak için sweep yönünü düzelt:
                    // outer join: büyük yayı gerekiyorsa 2pi ile zorla.
                    if (isOuterSide)
                    {
                        // Y-down’da yönler ters; pratik kural:
                        // sweep "yanlış tarafa" gidiyorsa 2pi ile çevir.
                        // (Burada işareti outerLeft testiyle uyumlu seçiyoruz.)
                        if (sweep > 0) sweep -= 2.0 * M_PI;  // dış yay genelde negatif tarafa düşer
                    }
                    else
                    {
                        // iç taraf: kısa yay yeterli
                        // sweep zaten (-pi,pi)
                    }

                    // a1'i sweep ile yeniden tanımla:
                    a1 = a0 + sweep;

                    appendArcPoints(contour, V, a0, a1, hw);
                    return;
                }


                // Miter join
                if (lineJoin == 0)
                {
                    DPoint miterPt;
                    bool ok = intersectLines2(p0, s0.d, p1, s1.d, miterPt);
                    if (!ok)
                    {
                        pushUniqueD(contour, p1);
                        return;
                    }

                    double mx = miterPt.x - V.x;
                    double my = miterPt.y - V.y;
                    double mLen = std::hypot(mx, my);

                    if (mLen < 1e-10 || (mLen / hw) > miterLimit)
                    {
                        pushUniqueD(contour, p1);
                        return;
                    }

                    pushUniqueD(contour, miterPt);
                    return;
                }

                // Bevel (varsayılan)
                pushUniqueD(contour, p1);
            };

        // DÜZELTME 3: Dejenere segment kontrolü
        size_t firstSeg = 0;
        while (firstSeg < segN && segs[firstSeg].len < 1e-10) firstSeg++;
        if (firstSeg >= segN) return;

        size_t lastSeg = segN - 1;
        while (lastSeg > 0 && segs[lastSeg].len < 1e-10) lastSeg--;

        const Seg& sFirst = segs[firstSeg];
        const Seg& sLast = segs[lastSeg];

        DPoint P0 = P[firstSeg];
        DPoint Pn = P[lastSeg + 1];

        // DÜZELTME 4: Square cap düzeltmesi
        DPoint startShift = { 0,0 };
        DPoint endShift = { 0,0 };
        if (!closed && lineCap == 2)
        {
            startShift = { -sFirst.d.x * hw, -sFirst.d.y * hw };
            endShift = { sLast.d.x * hw, sLast.d.y * hw };
        }

        // İlk noktaları ekle
        pushUniqueD(leftC, { P0.x + sFirst.n.x * hw + startShift.x, P0.y + sFirst.n.y * hw + startShift.y });
        pushUniqueD(rightC, { P0.x - sFirst.n.x * hw + startShift.x, P0.y - sFirst.n.y * hw + startShift.y });

        // DÜZELTME 5: Outer side tespiti iyileştirildi
        auto isOuterLeftAt = [&](const Seg& prev, const Seg& next) -> bool
            {
                double t = cross2(prev.d.x, prev.d.y, next.d.x, next.d.y);
                return (t < 1e-9); // Tolerans eklendi
            };

        if (closed)
        {
            // Kapali path: her segment icin offset noktalari + koselerde join
            for (size_t i = 0; i < segN; ++i)
            {
                const Seg& s = segs[i];
                if (s.len < 1e-10) continue;

                // Segment baslangic ve bitis noktalari
                DPoint p0 = P[i];
                DPoint p1 = P[(i + 1) % P.size()];

                // Segment offset noktalari (sol ve sag contour)
                pushUniqueD(leftC, { p0.x + s.n.x * hw, p0.y + s.n.y * hw });
                pushUniqueD(leftC, { p1.x + s.n.x * hw, p1.y + s.n.y * hw });

                pushUniqueD(rightC, { p0.x - s.n.x * hw, p0.y - s.n.y * hw });
                pushUniqueD(rightC, { p1.x - s.n.x * hw, p1.y - s.n.y * hw });

                // Kosede join (sonraki segment ile)
                size_t nextI = (i + 1) % segN;
                const Seg& sNext = segs[nextI];
                if (sNext.len >= 1e-10) {
                    bool outerLeft = isOuterLeftAt(s, sNext);
                    addJoinPoint(leftC, p1, s, sNext, true, outerLeft);
                    addJoinPoint(rightC, p1, s, sNext, false, !outerLeft);
                }
            }
        }
        else
        {
            // Açık path: sadece iç vertex'lerde join
            for (size_t i = firstSeg + 1; i <= lastSeg; ++i)
            {
                const Seg& s0 = segs[i - 1];
                const Seg& s1 = segs[i];

                if (s0.len < 1e-10 || s1.len < 1e-10) continue;

                DPoint V = P[i];
                bool outerLeft = isOuterLeftAt(s0, s1);

                addJoinPoint(leftC, V, s0, s1, true, outerLeft);
                addJoinPoint(rightC, V, s0, s1, false, !outerLeft);
            }

            // Son noktaları ekle
            pushUniqueD(leftC, { Pn.x + sLast.n.x * hw + endShift.x, Pn.y + sLast.n.y * hw + endShift.y });
            pushUniqueD(rightC, { Pn.x - sLast.n.x * hw + endShift.x, Pn.y - sLast.n.y * hw + endShift.y });
        }

        // DÜZELTME 6: Cap rendering düzeltildi
        std::vector<DPoint> capEnd;
        std::vector<DPoint> capStart;

        if (!closed && lineCap == 1) // Round caps
        {
            // Son cap
            {
                DPoint C = { Pn.x + endShift.x, Pn.y + endShift.y };
                double a0 = angleOf2(sLast.n.x, sLast.n.y);
                double a1 = angleOf2(-sLast.n.x, -sLast.n.y);
                appendArcPoints(capEnd, C, a0, a1, hw);
            }

            // Başlangıç cap
            {
                DPoint C = { P0.x + startShift.x, P0.y + startShift.y };
                double a0 = angleOf2(-sFirst.n.x, -sFirst.n.y);
                double a1 = angleOf2(sFirst.n.x, sFirst.n.y);
                appendArcPoints(capStart, C, a0, a1, hw);
            }
        }

        if (subpathDebug) {
            fprintf(subpathDebug, "leftC.size=%zu, rightC.size=%zu\n", leftC.size(), rightC.size());

            // Find consecutive right edge points in leftC
            fprintf(subpathDebug, "leftC points with X > 1200:\n");
            for (size_t i = 0; i < leftC.size(); i++) {
                if (leftC[i].x > 1200) {
                    fprintf(subpathDebug, "  leftC[%zu] = (%.1f, %.1f)\n", i, leftC[i].x, leftC[i].y);
                }
            }

            // Check for missing right edge segment
            fprintf(subpathDebug, "Checking for right edge vertical segment in leftC:\n");
            bool foundRightVertical = false;
            for (size_t i = 0; i + 1 < leftC.size(); i++) {
                if (leftC[i].x > 1200 && leftC[i + 1].x > 1200) {
                    double dy = std::abs(leftC[i + 1].y - leftC[i].y);
                    if (dy > 100) {  // Long vertical segment
                        fprintf(subpathDebug, "  FOUND: leftC[%zu]->[%zu]: (%.1f,%.1f)->(%.1f,%.1f), dy=%.1f\n",
                            i, i + 1, leftC[i].x, leftC[i].y, leftC[i + 1].x, leftC[i + 1].y, dy);
                        foundRightVertical = true;
                    }
                }
            }
            if (!foundRightVertical) {
                fprintf(subpathDebug, "  *** NO LONG VERTICAL SEGMENT ON RIGHT EDGE! ***\n");
            }
            fflush(subpathDebug);
        }

        // Final outline
        std::vector<DPoint> outline;
        outline.reserve(leftC.size() + rightC.size() + capEnd.size() + capStart.size() + 8);

        for (auto& p : leftC) pushUniqueD(outline, p);
        for (auto& p : capEnd) pushUniqueD(outline, p);
        for (size_t i = rightC.size(); i-- > 0; ) pushUniqueD(outline, rightC[i]);
        for (auto& p : capStart) pushUniqueD(outline, p);

        // Polygon'u kapat
        if (outline.size() >= 3)
        {
            double dx = outline.front().x - outline.back().x;
            double dy = outline.front().y - outline.back().y;
            if (dx * dx + dy * dy > 1e-6)
                outline.push_back(outline.front());
        }

        if (outline.size() < 3) {
            if (subpathDebug && isRightEdge && isMiddleY) {
                fprintf(subpathDebug, "[Subpath #%d] SKIPPED: outline.size()=%zu < 3, pts.size()=%zu\n",
                    subpathCount, outline.size(), pts.size());
                fflush(subpathDebug);
            }
            return;
        }

        // Integer'a çevir ve render et
        std::vector<IPoint> poly;
        poly.reserve(outline.size());
        for (auto& p : outline)
            poly.push_back({ (int)std::lround(p.x), (int)std::lround(p.y) });

        static FILE* rasterDebug = nullptr;
        static int rasterCount = 0;
        if (!rasterDebug) {
            char tempPath[MAX_PATH];
            GetTempPathA(MAX_PATH, tempPath);
            strcat_s(tempPath, "raster_debug.txt");
            rasterDebug = fopen(tempPath, "w");
        }
        rasterCount++;

        // Find bounds
        int minX = poly[0].x, maxX = poly[0].x;
        int minY = poly[0].y, maxY = poly[0].y;
        for (const auto& p : poly) {
            if (p.x < minX) minX = p.x;
            if (p.x > maxX) maxX = p.x;
            if (p.y < minY) minY = p.y;
            if (p.y > maxY) maxY = p.y;
        }

        // Log ALL polygons for debugging
        if (rasterDebug) {
            fprintf(rasterDebug, "[Polygon #%d] size=%zu, bounds=(%d,%d)-(%d,%d), color=0x%08X\n",
                rasterCount, poly.size(), minX, minY, maxX, maxY, color);
            fprintf(rasterDebug, "  Buffer: _w=%d, _h=%d\n", _w, _h);
            fprintf(rasterDebug, "  leftC.size=%zu, rightC.size=%zu\n", leftC.size(), rightC.size());

            // Log RIGHT EDGE points (X > 1200)
            fprintf(rasterDebug, "  RIGHT EDGE POINTS (X > 1200):\n");
            int rightCount = 0;
            for (size_t i = 0; i < poly.size(); i++) {
                if (poly[i].x > 1200) {
                    fprintf(rasterDebug, "    poly[%zu] = (%d, %d)\n", i, poly[i].x, poly[i].y);
                    rightCount++;
                }
            }
            fprintf(rasterDebug, "  Total right edge points: %d\n", rightCount);

            // Check if right edge has full Y range
            int rightMinY = 99999, rightMaxY = -99999;
            for (size_t i = 0; i < poly.size(); i++) {
                if (poly[i].x > 1200) {
                    if (poly[i].y < rightMinY) rightMinY = poly[i].y;
                    if (poly[i].y > rightMaxY) rightMaxY = poly[i].y;
                }
            }
            fprintf(rasterDebug, "  Right edge Y range: %d - %d (expected ~88 - ~832)\n", rightMinY, rightMaxY);

            fprintf(rasterDebug, "\n");
            fflush(rasterDebug);
        }

        this->rasterFillPolygon(poly, color, false);


    }


    void PdfPainter::strokePath(
        const PdfPath& path,
        uint32_t color,
        double lineWidth,
        const PdfMatrix& ctm,
        int lineCap,
        int lineJoin,
        double miterLimit)
    {
        // PDF lineWidth user space; burada device px'e çeviriyoruz.
        // (Non-uniform scale varsa istersen max(_scaleX,_scaleY) kullanabilirsin.)
        LogDebug("PdfPainter::strokePath called - %zu segments, lw=%.2f, color=0x%08X",
            path.size(), lineWidth, color);

        // ========== STROKE DEBUG ==========
        static FILE* strokeDebug = nullptr;
        static int strokeCount = 0;
        if (!strokeDebug) {
            char tempPath[MAX_PATH];
            GetTempPathA(MAX_PATH, tempPath);
            strcat_s(tempPath, "stroke_debug.txt");
            strokeDebug = fopen(tempPath, "w");
            if (strokeDebug) {
                fprintf(strokeDebug, "=== STROKE PATH DEBUG ===\n");
                fprintf(strokeDebug, "_w=%d, _h=%d, _scaleX=%.4f, _scaleY=%.4f, _hasRotate=%d\n\n",
                    _w, _h, _scaleX, _scaleY, _hasRotate ? 1 : 0);
                fflush(strokeDebug);
            }
        }
        strokeCount++;

        // Log CTM for first strokes
        if (strokeDebug && strokeCount <= 20) {
            fprintf(strokeDebug, "\n=== Stroke #%d ===\n", strokeCount);
            fprintf(strokeDebug, "CTM=[%.4f %.4f %.4f %.4f %.4f %.4f]\n",
                ctm.a, ctm.b, ctm.c, ctm.d, ctm.e, ctm.f);
            fprintf(strokeDebug, "lineWidth=%.2f, color=0x%08X, segments=%zu\n",
                lineWidth, color, path.size());

            // Log first point
            if (!path.empty()) {
                fprintf(strokeDebug, "First point: (%.2f, %.2f)\n", path[0].x, path[0].y);
            }
            fflush(strokeDebug);
        }


        // Eğer path boşsa
        if (path.empty())
        {
            return;
        }


        double lwPx = lineWidthToDevicePx(lineWidth, ctm, _scaleX, _scaleY);

        const double tolPx = 0.05;
        const double tolPxSq = tolPx * tolPx;

        std::vector<DPoint> pts;
        pts.reserve(256);

        bool hasSubpath = false;
        bool closed = false;

        double curUx = 0.0, curUy = 0.0;
        double startUx = 0.0, startUy = 0.0;

        auto userToDevicePoint = [&](double ux, double uy, double& dx, double& dy)
            {
                ApplyMatrix(ctm, ux, uy, dx, dy);
                dx *= _scaleX;
                dy = mapY(dy * _scaleY);
                applyRotate(dx, dy);
            };

        auto flush = [&]()
            {
                if (pts.size() >= 2) {
                    if (strokeDebug) {
                        double minX = pts[0].x, maxX = pts[0].x;
                        double minY = pts[0].y, maxY = pts[0].y;
                        for (const auto& p : pts) {
                            if (p.x < minX) minX = p.x;
                            if (p.x > maxX) maxX = p.x;
                            if (p.y < minY) minY = p.y;
                            if (p.y > maxY) maxY = p.y;
                        }

                        // HER STROKE ICIN DETAYLI LOG
                        fprintf(strokeDebug, "[Stroke #%d] pts=%zu, color=0x%08X, lw=%.2f\n",
                            strokeCount, pts.size(), color, lwPx);
                        fprintf(strokeDebug, "  Device bounds: (%.1f,%.1f)-(%.1f,%.1f)\n",
                            minX, minY, maxX, maxY);
                        fprintf(strokeDebug, "  Buffer size: _w=%d, _h=%d\n", _w, _h);

                        // Check if any part is out of bounds
                        bool partiallyOut = (minX < 0 || maxX >= _w || minY < 0 || maxY >= _h);
                        bool completelyOut = (maxX < 0 || minX >= _w || maxY < 0 || minY >= _h);

                        if (completelyOut) {
                            fprintf(strokeDebug, "  *** COMPLETELY OUT OF BOUNDS! ***\n");
                        }
                        else if (partiallyOut) {
                            fprintf(strokeDebug, "  ! Partially out of bounds (will be clipped)\n");
                        }

                        // Log first few points
                        size_t logCount = (pts.size() < 5) ? pts.size() : 5;
                        for (size_t i = 0; i < logCount; i++) {
                            fprintf(strokeDebug, "    pt[%zu] = (%.2f, %.2f)\n", i, pts[i].x, pts[i].y);
                        }
                        if (pts.size() > 5) {
                            fprintf(strokeDebug, "    ... (%zu more points)\n", pts.size() - 5);
                            fprintf(strokeDebug, "    pt[%zu] = (%.2f, %.2f) (last)\n",
                                pts.size() - 1, pts.back().x, pts.back().y);
                        }
                        fprintf(strokeDebug, "\n");
                        fflush(strokeDebug);
                    }
                    strokeSubpath(pts, closed, color, lwPx, lineJoin, lineCap, miterLimit);
                }

                pts.clear();
                closed = false;
                hasSubpath = false;
            };

        for (const auto& seg : path)
        {
            if (seg.type == PdfPathSegment::MoveTo)
            {
                flush();
                curUx = startUx = seg.x;
                curUy = startUy = seg.y;
                hasSubpath = true;

                double dx, dy;
                userToDevicePoint(curUx, curUy, dx, dy);
                addPointUniqueD(pts, dx, dy);
            }
            else if (seg.type == PdfPathSegment::LineTo)
            {
                if (!hasSubpath)
                {
                    curUx = startUx = seg.x;
                    curUy = startUy = seg.y;
                    hasSubpath = true;

                    double dx, dy;
                    userToDevicePoint(curUx, curUy, dx, dy);
                    addPointUniqueD(pts, dx, dy);
                    continue;
                }

                curUx = seg.x;
                curUy = seg.y;

                double dx, dy;
                userToDevicePoint(curUx, curUy, dx, dy);
                addPointUniqueD(pts, dx, dy);
            }
            else if (seg.type == PdfPathSegment::CurveTo)
            {
                if (!hasSubpath) continue;

                double x0d, y0d, x1d, y1d, x2d, y2d, x3d, y3d;
                userToDevicePoint(curUx, curUy, x0d, y0d);
                userToDevicePoint(seg.x1, seg.y1, x1d, y1d);
                userToDevicePoint(seg.x2, seg.y2, x2d, y2d);
                userToDevicePoint(seg.x3, seg.y3, x3d, y3d);

                addPointUniqueD(pts, x0d, y0d);

                // Cubic flatten
                flattenCubicBezierDeviceD(
                    x0d, y0d,
                    x1d, y1d,
                    x2d, y2d,
                    x3d, y3d,
                    pts,
                    tolPxSq
                );

                curUx = seg.x3;
                curUy = seg.y3;
            }
            else if (seg.type == PdfPathSegment::Close)
            {
                if (hasSubpath)
                {
                    double sdx, sdy;
                    userToDevicePoint(startUx, startUy, sdx, sdy);
                    addPointUniqueD(pts, sdx, sdy);
                    closed = true;
                    flush();
                }
            }
        }

        flush();
    }

    // Dosyanın sonuna ekle:

  // PdfPainter.cpp içindeki fillPathWithGradient fonksiyonunu bu kodla değiştir
// Dosyanın başına şu include'ları ekle (eğer yoksa):
// #include <random>

    void PdfPainter::fillPathWithGradient(
        const std::vector<PdfPathSegment>& clipPath,
        const PdfGradient& gradient,
        const PdfMatrix& clipCTM,
        const PdfMatrix& gradientCTM,
        bool evenOdd)
    {
        static FILE* gradDebugFile = nullptr;
        static int gradCallCount = 0;
        if (!gradDebugFile) {
            char tempPath[MAX_PATH];
            GetTempPathA(MAX_PATH, tempPath);
            strcat_s(tempPath, "gradient_debug.txt");
            gradDebugFile = fopen(tempPath, "w");
            if (gradDebugFile) {
                fprintf(gradDebugFile, "=== GRADIENT DEBUG LOG ===\n");
                fprintf(gradDebugFile, "Log file: %s\n", tempPath);
                fflush(gradDebugFile);
            }
        }
        gradCallCount++;

        int curveCount = 0;
        int lineCount = 0;
        int moveCount = 0;
        for (const auto& seg : clipPath) {
            if (seg.type == PdfPathSegment::CurveTo) curveCount++;
            else if (seg.type == PdfPathSegment::LineTo) lineCount++;
            else if (seg.type == PdfPathSegment::MoveTo) moveCount++;
        }

        if (gradDebugFile) {
            fprintf(gradDebugFile, "\n[fillPathWithGradient #%d] path.size=%zu, moves=%d, lines=%d, CURVES=%d\n",
                gradCallCount, clipPath.size(), moveCount, lineCount, curveCount);
            fprintf(gradDebugFile, "  clipCTM=[%.4f %.4f %.4f %.4f %.4f %.4f]\n",
                clipCTM.a, clipCTM.b, clipCTM.c, clipCTM.d, clipCTM.e, clipCTM.f);
            fprintf(gradDebugFile, "  gradientCTM=[%.4f %.4f %.4f %.4f %.4f %.4f]\n",
                gradientCTM.a, gradientCTM.b, gradientCTM.c, gradientCTM.d, gradientCTM.e, gradientCTM.f);
            fprintf(gradDebugFile, "  gradient: (%.2f,%.2f) -> (%.2f,%.2f), %zu stops\n",
                gradient.x0, gradient.y0, gradient.x1, gradient.y1, gradient.stops.size());
            fflush(gradDebugFile);
        }

        if (clipPath.empty() || gradient.stops.empty())
        {
            return;
        }


        // =====================================================
        // 1. GRADIENT VECTOR'Ü HESAPLA
        // =====================================================
        double gx0_page = gradientCTM.a * gradient.x0 + gradientCTM.c * gradient.y0 + gradientCTM.e;
        double gy0_page = gradientCTM.b * gradient.x0 + gradientCTM.d * gradient.y0 + gradientCTM.f;
        double gx1_page = gradientCTM.a * gradient.x1 + gradientCTM.c * gradient.y1 + gradientCTM.e;
        double gy1_page = gradientCTM.b * gradient.x1 + gradientCTM.d * gradient.y1 + gradientCTM.f;

        double gx0_dev = gx0_page * _scaleX;
        double gy0_dev = mapY(gy0_page * _scaleY);
        double gx1_dev = gx1_page * _scaleX;
        double gy1_dev = mapY(gy1_page * _scaleY);

        applyRotate(gx0_dev, gy0_dev);
        applyRotate(gx1_dev, gy1_dev);

        double gdx = gx1_dev - gx0_dev;
        double gdy = gy1_dev - gy0_dev;
        double gradLen = std::sqrt(gdx * gdx + gdy * gdy);

        if (gradLen < 0.001)
        {
            double rgb[3];
            gradient.evaluateColor(0.5, rgb);
            uint32_t color = 0xFF000000u |
                ((int)(rgb[0] * 255) << 16) |
                ((int)(rgb[1] * 255) << 8) |
                (int)(rgb[2] * 255);
            fillPath(clipPath, color, clipCTM, evenOdd);
            return;
        }

        double gndx = gdx / gradLen;
        double gndy = gdy / gradLen;

        // =====================================================
        // 2. PATH TRANSFORM HELPER
        // =====================================================
        auto pathToDevice = [&](double px, double py, double& dx, double& dy) {
            double tx = clipCTM.a * px + clipCTM.c * py + clipCTM.e;
            double ty = clipCTM.b * px + clipCTM.d * py + clipCTM.f;
            dx = tx * _scaleX;
            dy = mapY(ty * _scaleY);
            applyRotate(dx, dy);
            };

        // =====================================================
        // 3. BOUNDING BOX
        // =====================================================
        double devMinX = 1e9, devMinY = 1e9, devMaxX = -1e9, devMaxY = -1e9;

        for (const auto& seg : clipPath)
        {
            double dx, dy;
            if (seg.type == PdfPathSegment::MoveTo || seg.type == PdfPathSegment::LineTo)
            {
                pathToDevice(seg.x, seg.y, dx, dy);
                devMinX = std::min(devMinX, dx); devMaxX = std::max(devMaxX, dx);
                devMinY = std::min(devMinY, dy); devMaxY = std::max(devMaxY, dy);
            }
            else if (seg.type == PdfPathSegment::CurveTo)
            {
                pathToDevice(seg.x1, seg.y1, dx, dy);
                devMinX = std::min(devMinX, dx); devMaxX = std::max(devMaxX, dx);
                devMinY = std::min(devMinY, dy); devMaxY = std::max(devMaxY, dy);

                pathToDevice(seg.x2, seg.y2, dx, dy);
                devMinX = std::min(devMinX, dx); devMaxX = std::max(devMaxX, dx);
                devMinY = std::min(devMinY, dy); devMaxY = std::max(devMaxY, dy);

                pathToDevice(seg.x3, seg.y3, dx, dy);
                devMinX = std::min(devMinX, dx); devMaxX = std::max(devMaxX, dx);
                devMinY = std::min(devMinY, dy); devMaxY = std::max(devMaxY, dy);
            }
        }

        int startX = std::max(0, (int)std::floor(devMinX));
        int endX = std::min(_w, (int)std::ceil(devMaxX));
        int startY = std::max(0, (int)std::floor(devMinY));
        int endY = std::min(_h, (int)std::ceil(devMaxY));

        if (startX >= endX || startY >= endY) return;

        // =====================================================
        // 4. PATH'İ POLYGON'A DÖNÜŞTÜR
        // =====================================================
        std::vector<std::vector<DPoint>> polygons;
        std::vector<DPoint> currentPoly;

        double cpx = 0, cpy = 0;
        double subStartX = 0, subStartY = 0;
        bool inSubpath = false;

        static FILE* bezierDebugFile = nullptr;
        static int bezierDebugCallCount = 0;
        if (!bezierDebugFile) {
            char tempPath[MAX_PATH];
            GetTempPathA(MAX_PATH, tempPath);
            strcat_s(tempPath, "bezier_flatten_debug.txt");
            bezierDebugFile = fopen(tempPath, "w");
            if (bezierDebugFile) {
                fprintf(bezierDebugFile, "=== BEZIER FLATTEN DEBUG ===\n");
                fflush(bezierDebugFile);
            }
        }
        bezierDebugCallCount++;

        int totalCurves = 0;
        int totalFlattenedPoints = 0;

        auto flushPoly = [&]() {
            if (currentPoly.size() >= 3)
            {
                const DPoint& first = currentPoly.front();
                const DPoint& last = currentPoly.back();
                if (std::abs(first.x - last.x) > 0.5 || std::abs(first.y - last.y) > 0.5)
                    currentPoly.push_back(first);
                polygons.push_back(currentPoly);
            }
            currentPoly.clear();
            inSubpath = false;
            };

        for (const auto& seg : clipPath)
        {
            if (seg.type == PdfPathSegment::MoveTo)
            {
                flushPoly();
                cpx = seg.x; cpy = seg.y;
                subStartX = cpx; subStartY = cpy;
                inSubpath = true;

                double dx, dy;
                pathToDevice(cpx, cpy, dx, dy);
                currentPoly.push_back({ dx, dy });
            }
            else if (seg.type == PdfPathSegment::LineTo)
            {
                if (!inSubpath)
                {
                    cpx = subStartX = seg.x;
                    cpy = subStartY = seg.y;
                    inSubpath = true;
                    double dx, dy;
                    pathToDevice(cpx, cpy, dx, dy);
                    currentPoly.push_back({ dx, dy });
                    continue;
                }

                cpx = seg.x; cpy = seg.y;
                double dx, dy;
                pathToDevice(cpx, cpy, dx, dy);

                if (currentPoly.empty() ||
                    std::abs(currentPoly.back().x - dx) > 0.1 ||
                    std::abs(currentPoly.back().y - dy) > 0.1)
                {
                    currentPoly.push_back({ dx, dy });
                }
            }
            else if (seg.type == PdfPathSegment::CurveTo)
            {
                if (!inSubpath) continue;

                double x0d, y0d, x1d, y1d, x2d, y2d, x3d, y3d;
                pathToDevice(cpx, cpy, x0d, y0d);
                pathToDevice(seg.x1, seg.y1, x1d, y1d);
                pathToDevice(seg.x2, seg.y2, x2d, y2d);
                pathToDevice(seg.x3, seg.y3, x3d, y3d);

                // FIX: fillPath ile aynı tolerans ve fonksiyonu kullan
                // Eski kod 0.5 pixel tolerans kullanıyordu - çok gevşek!
                // Şimdi 0.05 pixel (0.0025 sq) kullanıyoruz - fillPath ile aynı
                const double tolPx = 0.05;
                const double tolPxSq = tolPx * tolPx;

                // Başlangıç noktasını ekle (duplicate olmasın)
                size_t pointsBefore = currentPoly.size();
                if (currentPoly.empty() ||
                    std::abs(currentPoly.back().x - x0d) > 0.1 ||
                    std::abs(currentPoly.back().y - y0d) > 0.1)
                {
                    currentPoly.push_back({ x0d, y0d });
                }

                // Global flattenCubicBezierDeviceD fonksiyonunu kullan
                flattenCubicBezierDeviceD(
                    x0d, y0d,
                    x1d, y1d,
                    x2d, y2d,
                    x3d, y3d,
                    currentPoly,
                    tolPxSq
                );

                totalCurves++;
                size_t pointsAfter = currentPoly.size();
                int pointsAdded = (int)(pointsAfter - pointsBefore);
                totalFlattenedPoints += pointsAdded;

                if (bezierDebugFile && bezierDebugCallCount <= 20 && totalCurves <= 10) {
                    fprintf(bezierDebugFile, "  Curve #%d: user(%.2f,%.2f)->(%.2f,%.2f)->(%.2f,%.2f)->(%.2f,%.2f)\n",
                        totalCurves, cpx, cpy, seg.x1, seg.y1, seg.x2, seg.y2, seg.x3, seg.y3);
                    fprintf(bezierDebugFile, "            device(%.2f,%.2f)->(%.2f,%.2f)->(%.2f,%.2f)->(%.2f,%.2f)\n",
                        x0d, y0d, x1d, y1d, x2d, y2d, x3d, y3d);
                    fprintf(bezierDebugFile, "            points: %zu -> %zu (+%d)\n",
                        pointsBefore, pointsAfter, pointsAdded);
                    fflush(bezierDebugFile);
                }

                cpx = seg.x3; cpy = seg.y3;
            }
            else if (seg.type == PdfPathSegment::Close)
            {
                if (inSubpath)
                {
                    double dx, dy;
                    pathToDevice(subStartX, subStartY, dx, dy);
                    if (currentPoly.empty() ||
                        std::abs(currentPoly.back().x - dx) > 0.1 ||
                        std::abs(currentPoly.back().y - dy) > 0.1)
                    {
                        currentPoly.push_back({ dx, dy });
                    }
                }
                flushPoly();
            }
        }
        flushPoly();

        if (bezierDebugFile && bezierDebugCallCount <= 50) {
            int totalPolyPoints = 0;
            for (const auto& poly : polygons) {
                totalPolyPoints += (int)poly.size();
            }
            fprintf(bezierDebugFile, "\n[fillPathWithGradient #%d] SUMMARY:\n", bezierDebugCallCount);
            fprintf(bezierDebugFile, "  Input: %zu path segments\n", clipPath.size());
            fprintf(bezierDebugFile, "  Curves processed: %d\n", totalCurves);
            fprintf(bezierDebugFile, "  Flattened points from curves: %d\n", totalFlattenedPoints);
            fprintf(bezierDebugFile, "  Output polygons: %zu\n", polygons.size());
            fprintf(bezierDebugFile, "  Total polygon points: %d\n", totalPolyPoints);
            fprintf(bezierDebugFile, "  clipCTM: [%.4f %.4f %.4f %.4f %.4f %.4f]\n",
                clipCTM.a, clipCTM.b, clipCTM.c, clipCTM.d, clipCTM.e, clipCTM.f);
            fflush(bezierDebugFile);
        }

        static int svgSaveCount = 0;
        if (svgSaveCount < 4 && !polygons.empty() && totalCurves > 0) {
            char svgPath[MAX_PATH];
            GetTempPathA(MAX_PATH, svgPath);
            char svgFilename[64];
            sprintf(svgFilename, "polygon_debug_%d.svg", svgSaveCount);
            strcat(svgPath, svgFilename);

            FILE* svgFile = fopen(svgPath, "w");
            if (svgFile) {
                // SVG bounds hesapla
                double minX = 1e30, maxX = -1e30, minY = 1e30, maxY = -1e30;
                for (const auto& poly : polygons) {
                    for (const auto& pt : poly) {
                        minX = std::min(minX, pt.x);
                        maxX = std::max(maxX, pt.x);
                        minY = std::min(minY, pt.y);
                        maxY = std::max(maxY, pt.y);
                    }
                }

                double width = maxX - minX + 20;
                double height = maxY - minY + 20;

                fprintf(svgFile, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
                fprintf(svgFile, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%.0f\" height=\"%.0f\" viewBox=\"%.0f %.0f %.0f %.0f\">\n",
                    width, height, minX - 10, minY - 10, width, height);
                fprintf(svgFile, "  <rect x=\"%.0f\" y=\"%.0f\" width=\"%.0f\" height=\"%.0f\" fill=\"white\"/>\n",
                    minX - 10, minY - 10, width, height);

                // Her polygon'u çiz
                int polyIdx = 0;
                const char* colors[] = { "red", "green", "blue", "purple", "orange" };
                for (const auto& poly : polygons) {
                    if (poly.size() < 3) continue;

                    fprintf(svgFile, "  <polygon points=\"");
                    for (size_t i = 0; i < poly.size(); i++) {
                        fprintf(svgFile, "%.2f,%.2f ", poly[i].x, poly[i].y);
                    }
                    fprintf(svgFile, "\" fill=\"none\" stroke=\"%s\" stroke-width=\"1\"/>\n",
                        colors[polyIdx % 5]);

                    // İlk ve son noktayı işaretle
                    fprintf(svgFile, "  <circle cx=\"%.2f\" cy=\"%.2f\" r=\"3\" fill=\"%s\"/>\n",
                        poly[0].x, poly[0].y, colors[polyIdx % 5]);

                    polyIdx++;
                }

                fprintf(svgFile, "</svg>\n");
                fclose(svgFile);

                if (bezierDebugFile) {
                    fprintf(bezierDebugFile, "  SVG saved to: %s\n", svgPath);
                    fflush(bezierDebugFile);
                }
            }
            svgSaveCount++;
        }

        if (polygons.empty()) return;

        // =====================================================
        // 5. ORDERED DITHERING MATRIX (8x8 Bayer - daha pürüzsüz)
        // =====================================================
        static const float bayerMatrix8[8][8] = {
            {  0 / 64.0f, 32 / 64.0f,  8 / 64.0f, 40 / 64.0f,  2 / 64.0f, 34 / 64.0f, 10 / 64.0f, 42 / 64.0f },
            { 48 / 64.0f, 16 / 64.0f, 56 / 64.0f, 24 / 64.0f, 50 / 64.0f, 18 / 64.0f, 58 / 64.0f, 26 / 64.0f },
            { 12 / 64.0f, 44 / 64.0f,  4 / 64.0f, 36 / 64.0f, 14 / 64.0f, 46 / 64.0f,  6 / 64.0f, 38 / 64.0f },
            { 60 / 64.0f, 28 / 64.0f, 52 / 64.0f, 20 / 64.0f, 62 / 64.0f, 30 / 64.0f, 54 / 64.0f, 22 / 64.0f },
            {  3 / 64.0f, 35 / 64.0f, 11 / 64.0f, 43 / 64.0f,  1 / 64.0f, 33 / 64.0f,  9 / 64.0f, 41 / 64.0f },
            { 51 / 64.0f, 19 / 64.0f, 59 / 64.0f, 27 / 64.0f, 49 / 64.0f, 17 / 64.0f, 57 / 64.0f, 25 / 64.0f },
            { 15 / 64.0f, 47 / 64.0f,  7 / 64.0f, 39 / 64.0f, 13 / 64.0f, 45 / 64.0f,  5 / 64.0f, 37 / 64.0f },
            { 63 / 64.0f, 31 / 64.0f, 55 / 64.0f, 23 / 64.0f, 61 / 64.0f, 29 / 64.0f, 53 / 64.0f, 21 / 64.0f }
        };

        // =====================================================
        // 6. SCANLINE FILL WITH GRADIENT + CORRECT DITHERING
        // =====================================================
        for (int y = startY; y < endY; ++y)
        {
            std::vector<std::pair<double, int>> intersections;

            for (const auto& poly : polygons)
            {
                for (size_t i = 0; i < poly.size(); ++i)
                {
                    size_t j = (i + 1) % poly.size();

                    double y0 = poly[i].y;
                    double y1 = poly[j].y;
                    double x0 = poly[i].x;
                    double x1 = poly[j].x;

                    if (std::abs(y1 - y0) < 0.001) continue;

                    double yMin = std::min(y0, y1);
                    double yMax = std::max(y0, y1);

                    if (y < yMin || y >= yMax) continue;

                    double t = (y - y0) / (y1 - y0);
                    double ix = x0 + t * (x1 - x0);
                    int winding = (y1 > y0) ? 1 : -1;

                    intersections.push_back({ ix, winding });
                }
            }

            if (intersections.empty()) continue;

            std::sort(intersections.begin(), intersections.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });

            if (evenOdd)
            {
                for (size_t i = 0; i + 1 < intersections.size(); i += 2)
                {
                    int x1 = std::max(startX, (int)std::ceil(intersections[i].first));
                    int x2 = std::min(endX - 1, (int)std::floor(intersections[i + 1].first));

                    for (int x = x1; x <= x2; ++x)
                    {
                        // Gradient t parametresi
                        double px = x - gx0_dev;
                        double py = y - gy0_dev;
                        double t = (px * gndx + py * gndy) / gradLen;
                        t = std::clamp(t, 0.0, 1.0);

                        // Renk hesapla
                        double rgb[3];
                        gradient.evaluateColor(t, rgb);

                        // Temiz render - dithering yok
                        uint8_t rb = (uint8_t)std::clamp((int)(rgb[0] * 255.0 + 0.5), 0, 255);
                        uint8_t gb = (uint8_t)std::clamp((int)(rgb[1] * 255.0 + 0.5), 0, 255);
                        uint8_t bb = (uint8_t)std::clamp((int)(rgb[2] * 255.0 + 0.5), 0, 255);

                        uint32_t color = 0xFF000000u | (rb << 16) | (gb << 8) | bb;
                        putPixel(x, y, color);
                    }
                }
            }
            else
            {
                int winding = 0;

                for (size_t i = 0; i + 1 < intersections.size(); ++i)
                {
                    winding += intersections[i].second;

                    if (winding != 0)
                    {
                        int x1 = std::max(startX, (int)std::ceil(intersections[i].first));
                        int x2 = std::min(endX - 1, (int)std::floor(intersections[i + 1].first));

                        for (int x = x1; x <= x2; ++x)
                        {
                            // Gradient t parametresi
                            double px = x - gx0_dev;
                            double py = y - gy0_dev;
                            double t = (px * gndx + py * gndy) / gradLen;
                            t = std::clamp(t, 0.0, 1.0);

                            // Renk hesapla - temiz, dithering yok
                            double rgb[3];
                            gradient.evaluateColor(t, rgb);

                            uint8_t rb = (uint8_t)std::clamp((int)(rgb[0] * 255.0 + 0.5), 0, 255);
                            uint8_t gb = (uint8_t)std::clamp((int)(rgb[1] * 255.0 + 0.5), 0, 255);
                            uint8_t bb = (uint8_t)std::clamp((int)(rgb[2] * 255.0 + 0.5), 0, 255);

                            uint32_t color = 0xFF000000u | (rb << 16) | (gb << 8) | bb;
                            putPixel(x, y, color);
                        }
                    }
                }
            }
        }

    }

    void PdfPainter::fillPathWithGradient(
        const std::vector<PdfPathSegment>& path,
        const PdfGradient& gradient,
        const PdfMatrix& ctm,
        bool evenOdd)
    {
        // Aynı CTM'i hem clip hem gradient için kullan
        fillPathWithGradient(path, gradient, ctm, ctm, evenOdd);
    }


} // namespace pdf
