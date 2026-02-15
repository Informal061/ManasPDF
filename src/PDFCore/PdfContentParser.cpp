#include "pch.h"

// MSVC güvenli fonksiyon uyarılarını kapat
#ifdef _MSC_VER
#pragma warning(disable: 4996)
#endif

#include "PdfContentParser.h"
#include "IPdfPainter.h"
#include "PdfPainter.h"  // For PdfPattern
#include "PdfDocument.h"
#include "PdfDebug.h"
#include "PdfGradient.h"
#include <windows.h>
#include <cctype>
#include <cmath>
#include <algorithm>
#include <cstring>

namespace pdf
{
    // ---------------------------------------------------------
    // Text advance helpers (Widths hesabı raw code üzerinden)
    // ---------------------------------------------------------
    static inline bool isCidFontActive(const PdfFontInfo* f)
    {
        if (!f) return false;
        if (f->isCidFont) return true;
        if (f->encoding == "/Identity-H" || f->encoding == "/Identity-V") return true;
        return false;
    }

    static inline void textAdvance(PdfGraphicsState& gs, double tx, double ty = 0.0)
    {
        // PDF: Tm = Tm * T(tx,ty)
        // Açılım:
        // e' = e + tx*a + ty*c
        // f' = f + tx*b + ty*d
        gs.textMatrix.e += tx * gs.textMatrix.a + ty * gs.textMatrix.c;
        gs.textMatrix.f += tx * gs.textMatrix.b + ty * gs.textMatrix.d;

        gs.textPosX = gs.textMatrix.e;
        gs.textPosY = gs.textMatrix.f;
    }

    static inline int getWidth1000ForCode(const PdfFontInfo* f, int code)
    {
        if (!f) return 0;

        // CID font ise
        if (f->isCidFont || f->encoding == "/Identity-H" || f->encoding == "/Identity-V")
        {
            // Oncelikle cidWidths'te bu CID var mi?
            auto it = f->cidWidths.find((uint16_t)code);
            if (it != f->cidWidths.end())
                return it->second;

            // cidWidths'te yok - cidDefaultWidth kullan
            // cidDefaultWidth == 1000 ise FreeType'a sinyal gonder
            if (f->cidDefaultWidth == 1000)
                return 0;  // Signal: FreeType kullan
            return f->cidDefaultWidth;
        }

        // Simple font
        int w = f->missingWidth;
        if (w <= 0) w = 500; // guvenli default

        // Simple font Widths
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

    // ✅ DÜZELTME: Tc ve Tw parametreleri eklendi, mantık düzeltildi.
    static double computeAdvanceFromRaw(
        const PdfFontInfo* f,
        const std::string& raw,
        double fontSize,
        double charSpacing,
        double wordSpacing
    )
    {
        if (!f || raw.empty())
            return 0.0;

        double totalAdvance = 0.0;

        auto getGlyphWidth = [&](int code) -> double {
            int w1000 = getWidth1000ForCode(f, code);
            // width 0 ise (FreeType kullanilacak sinyali), varsayilan kullan
            // Bu deger PdfPainter'da FreeType ile override edilecek
            if (w1000 <= 0) w1000 = 500;  // Ortalama karakter genisligi
            return (w1000 / 1000.0) * fontSize;
            };

        if (isCidFontActive(f))
        {
            for (size_t i = 0; i + 1 < raw.size(); i += 2)
            {
                int code = ((unsigned char)raw[i] << 8) | (unsigned char)raw[i + 1];
                double w = getGlyphWidth(code);
                double step = w + charSpacing;
                if (code == 32)
                    step += wordSpacing;
                totalAdvance += step;
            }
        }
        else
        {
            for (unsigned char c : raw)
            {
                int code = (int)c;
                double w = getGlyphWidth(code);
                double step = w + charSpacing;
                if (code == 32)
                    step += wordSpacing;
                totalAdvance += step;
            }
        }

        return totalAdvance;
    }


    static uint32_t rgbToArgb(const double rgb[3])
    {
        int r = (int)(rgb[0] * 255.0);
        int g = (int)(rgb[1] * 255.0);
        int b = (int)(rgb[2] * 255.0);

        r = std::clamp(r, 0, 255);
        g = std::clamp(g, 0, 255);
        b = std::clamp(b, 0, 255);

        return 0xFF000000u | (r << 16) | (g << 8) | b;
    }

    // Alpha destekli versiyon
    static uint32_t rgbToArgbWithAlpha(const double rgb[3], double alpha)
    {
        int a = (int)(alpha * 255.0);
        int r = (int)(rgb[0] * 255.0);
        int g = (int)(rgb[1] * 255.0);
        int b = (int)(rgb[2] * 255.0);

        a = std::clamp(a, 0, 255);
        r = std::clamp(r, 0, 255);
        g = std::clamp(g, 0, 255);
        b = std::clamp(b, 0, 255);

        return ((uint32_t)a << 24) | (r << 16) | (g << 8) | b;
    }

    static void cmykToRgb(double c, double m, double y, double k, double out[3])
    {
        // CMYK to RGB - Calibrated to match Adobe Acrobat (US Web Coated SWOP v2)
        // Verified against pixel-picked Adobe output values

        // Clamp inputs
        c = std::min(1.0, std::max(0.0, c));
        m = std::min(1.0, std::max(0.0, m));
        y = std::min(1.0, std::max(0.0, y));
        k = std::min(1.0, std::max(0.0, k));

        // Step 1: Standard subtractive CMYK
        double r = (1.0 - c) * (1.0 - k);
        double g = (1.0 - m) * (1.0 - k);
        double b = (1.0 - y) * (1.0 - k);

        // Step 2: Ink impurity corrections (SWOP profile approximation)
        // Real printing inks are not spectrally pure - each ink absorbs/leaks
        // light in neighboring channels:

        // Cyan ink doesn't fully absorb red (leaks ~12% red light)
        r += 0.12 * c * (1.0 - k);

        // Yellow ink absorbs significant green light (~15% extra absorption)
        g -= 0.15 * y * (1.0 - m) * (1.0 - k);

        // Yellow ink doesn't fully absorb blue (leaks ~20% blue light)
        b += 0.20 * y * (1.0 - k);

        // Clamp output
        out[0] = std::min(1.0, std::max(0.0, r));
        out[1] = std::min(1.0, std::max(0.0, g));
        out[2] = std::min(1.0, std::max(0.0, b));
    }

    static inline void ApplyMatrixPoint(
        const PdfMatrix& m,
        double x,
        double y,
        double& ox,
        double& oy)
    {
        ox = m.a * x + m.c * y + m.e;
        oy = m.b * x + m.d * y + m.f;
    }


    // ---------------------------------------------------------
    // WinAnsi (CP1252) tablo – temel map
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

    PdfContentParser::PdfContentParser(
        const std::vector<uint8_t>& streamData,
        IPdfPainter* painter,
        PdfDocument* doc,
        int pageIndex,
        std::map<std::string, PdfFontInfo>* fonts,
        const PdfGraphicsState& initialGs,
        const std::vector<std::shared_ptr<PdfDictionary>>& resourceStack)
        : _data(streamData), _painter(painter), _doc(doc), _pageIndex(pageIndex), _fonts(fonts)
    {
        _pos = 0;
        _gs = initialGs;
        _gs.lineJoin = 1;
        _resStack = resourceStack;
    }


    // =========================================================
    // Basic Stream Helpers
    // =========================================================

    bool PdfContentParser::eof() const
    {
        return _pos >= _data.size();
    }

    uint8_t PdfContentParser::peek() const
    {
        return eof() ? 0 : _data[_pos];
    }

    uint8_t PdfContentParser::get()
    {
        return eof() ? 0 : _data[_pos++];
    }

    void PdfContentParser::skipSpaces()
    {
        while (!eof())
        {
            uint8_t c = peek();
            if (c == 0x0A || c == 0x0D || c == 0x09 || c == 0x20)
                _pos++;
            else
                break;
        }
    }

    void PdfContentParser::skipComment()
    {
        while (!eof())
        {
            uint8_t c = get();
            if (c == '\r' || c == '\n')
                break;
        }
    }

    // =========================================================
    // Token Readers
    // =========================================================

    double PdfContentParser::readNumber()
    {
        std::string s;

        uint8_t c = peek();
        if (c == '+' || c == '-')
            s.push_back((char)get());

        while (!eof())
        {
            c = peek();
            if (std::isdigit((unsigned char)c) || c == '.')
                s.push_back((char)get());
            else
                break;
        }

        if (s.empty())
            return 0.0;
        return std::stod(s);
    }


    std::string PdfContentParser::readName()
    {
        std::string out;

        while (!eof())
        {
            uint8_t c = peek();
            if (std::isspace(c) ||
                c == '/' || c == '(' || c == ')' ||
                c == '<' || c == '>' || c == '[' || c == ']')
                break;

            out.push_back((char)get());
        }

        return "/" + out;
    }
    std::string PdfContentParser::readString()
    {
        std::string out;
        int depth = 1;

        size_t limit = 0;
        const size_t MAX_STRING_LEN = 65535;
        size_t startPos = _pos;

        while (!eof() && depth > 0)  // ✅ EOF kontrolü ekle
        {
            if (++limit > MAX_STRING_LEN)
            {
                break;
            }

            uint8_t c = get();

            // ✅ EOF kontrolü
            if (c == 0 && eof())
            {
                break;
            }

            if (c == '\\')
            {
                if (!eof())
                {
                    uint8_t n = peek();  // ✅ Önce peek, sonra işle

                    // ✅ OCTAL ESCAPE: \ddd (1-3 oktal rakam: 0-7)
                    // PDF spec: backslash followed by 1-3 octal digits
                    if (n >= '0' && n <= '7')
                    {
                        int octalValue = 0;
                        int digitCount = 0;

                        // En fazla 3 oktal rakam oku
                        while (!eof() && digitCount < 3)
                        {
                            uint8_t d = peek();
                            if (d >= '0' && d <= '7')
                            {
                                octalValue = octalValue * 8 + (d - '0');
                                get();  // karakteri tüket
                                ++digitCount;
                            }
                            else
                            {
                                break;
                            }
                        }

                        // Byte olarak ekle (0-255 arası)
                        out.push_back(static_cast<char>(octalValue & 0xFF));
                    }
                    else
                    {
                        // Diğer escape karakterleri
                        get();  // n'yi tüket

                        switch (n)
                        {
                        case 'n': out.push_back('\n'); break;
                        case 'r': out.push_back('\r'); break;
                        case 't': out.push_back('\t'); break;
                        case 'b': out.push_back('\b'); break;
                        case 'f': out.push_back('\f'); break;
                        case '\\': out.push_back('\\'); break;
                        case '(': out.push_back('('); break;
                        case ')': out.push_back(')'); break;
                        case '\r':
                            // \<CR> veya \<CR><LF> → satır devamı, hiçbir şey ekleme
                            if (!eof() && peek() == '\n')
                                get();
                            break;
                        case '\n':
                            // \<LF> → satır devamı, hiçbir şey ekleme
                            break;
                        default:
                            // Bilinmeyen escape: backslash'ı yok say, karakteri ekle
                            out.push_back((char)n);
                            break;
                        }
                    }
                }
            }
            else if (c == '(')
            {
                depth++;
                out.push_back('(');
            }
            else if (c == ')')
            {
                depth--;
                if (depth > 0)
                    out.push_back(')');
            }
            else
            {
                out.push_back((char)c);
            }
        }

        return out;
    }

    std::string PdfContentParser::readWord()
    {
        std::string s;
        size_t limit = 0;
        const size_t MAX_WORD_LEN = 1000;

        while (!eof() && limit++ < MAX_WORD_LEN)
        {
            uint8_t c = peek();
            // PDF delimiter ve whitespace karakterlerinde dur
            if (std::isspace(c) ||
                c == '[' || c == ']' ||
                c == '(' || c == ')' ||
                c == '<' || c == '>' ||
                c == '/' || c == '%' ||
                c == '{' || c == '}')
                break;

            s.push_back((char)get());
        }

        return s;
    }

    // =========================================================
    // Stack Helpers
    // =========================================================

    double PdfContentParser::popNumber(double def)
    {
        if (_stack.empty()) return def;

        auto obj = _stack.back();
        _stack.pop_back();

        if (auto n = std::dynamic_pointer_cast<PdfNumber>(obj))
            return n->value;

        return def;
    }

    std::string PdfContentParser::popString()
    {
        if (_stack.empty())
            return "";

        auto obj = _stack.back();
        _stack.pop_back();

        if (auto s = std::dynamic_pointer_cast<PdfString>(obj))
            return s->value;

        if (auto n = std::dynamic_pointer_cast<PdfName>(obj))
            return n->value;

        return "";
    }

    std::string PdfContentParser::popName()
    {
        if (_stack.empty())
            return "";

        auto obj = _stack.back();
        _stack.pop_back();

        if (auto n = std::dynamic_pointer_cast<PdfName>(obj))
            return n->value;

        return "";
    }

    // =========================================================
    // Parse Loop
    // =========================================================

    void PdfContentParser::parseToken()
    {
        skipSpaces();
        if (eof()) return;

        uint8_t c = peek();

        if (c == '%')
        {
            skipComment();
            return;
        }
        if (c == '/')
        {
            get();
            _stack.push_back(std::make_shared<PdfName>(readName()));
            return;
        }

        // ✅ YENİ: Dictionary desteği (inline images için)
        if (c == '<')
        {
            // << dictionary başlangıcı mı kontrol et
            if (_pos + 1 < _data.size() && _data[_pos + 1] == '<')
            {
                get(); // ilk 
                get(); // ikinci 

                auto dict = std::make_shared<PdfDictionary>();

                // Dictionary'i oku (basitleştirilmiş)
                size_t dictLimit = 0;
                const size_t MAX_DICT_ENTRIES = 1000;

                while (!eof() && dictLimit++ < MAX_DICT_ENTRIES)
                {
                    skipSpaces();

                    // >> ile bitti mi?
                    if (peek() == '>')
                    {
                        if (_pos + 1 < _data.size() && _data[_pos + 1] == '>')
                        {
                            get(); // ilk >
                            get(); // ikinci >
                            break;
                        }
                    }

                    // Key oku (/ ile başlamalı)
                    if (peek() != '/')
                    {
                        // Hatalı format, dictionary'i kapat
                        while (!eof() && !(peek() == '>' && _pos + 1 < _data.size() && _data[_pos + 1] == '>'))
                            get();
                        if (!eof()) { get(); get(); } // >> atla
                        break;
                    }

                    get(); // / karakterini atla
                    std::string key = "/" + readName();

                    skipSpaces();

                    // Value oku
                    size_t posBefore = _pos;
                    parseToken();

                    if (_pos == posBefore)
                    {
                        break;
                    }

                    if (!_stack.empty())
                    {
                        dict->entries[key] = _stack.back();
                        _stack.pop_back();
                    }
                }

                _stack.push_back(dict);
                return;
            }

            // ============ HEX STRING PARSING ============
            // <XXXX> formatindaki hex string'leri parse et
            get(); // < karakterini atla
            std::string hexStr;
            while (!eof() && peek() != '>')
            {
                uint8_t ch = get();
                // Sadece hex karakterleri al, whitespace'leri atla
                if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f'))
                    hexStr += (char)ch;
            }
            if (!eof())
                get(); // > karakterini atla

            // Hex string'i binary'ye cevir
            std::string binary;
            for (size_t i = 0; i + 1 < hexStr.size(); i += 2)
            {
                char c1 = hexStr[i];
                char c2 = hexStr[i + 1];
                int hi = (c1 >= '0' && c1 <= '9') ? (c1 - '0') :
                    (c1 >= 'A' && c1 <= 'F') ? (10 + c1 - 'A') : (10 + c1 - 'a');
                int lo = (c2 >= '0' && c2 <= '9') ? (c2 - '0') :
                    (c2 >= 'A' && c2 <= 'F') ? (10 + c2 - 'A') : (10 + c2 - 'a');
                binary += (char)((hi << 4) | lo);
            }
            // Tek haneli kalan varsa (PDF spec: sondaki 0 ile tamamla)
            if (hexStr.size() % 2 == 1)
            {
                char c1 = hexStr.back();
                int hi = (c1 >= '0' && c1 <= '9') ? (c1 - '0') :
                    (c1 >= 'A' && c1 <= 'F') ? (10 + c1 - 'A') : (10 + c1 - 'a');
                binary += (char)(hi << 4);
            }

            _stack.push_back(std::make_shared<PdfString>(binary));
            return;
        }

        if (c == '(')
        {
            get();
            std::string str = readString();
            _stack.push_back(std::make_shared<PdfString>(str));

            if (_pos > _data.size())
            {
                _pos = _data.size();
            }
            return;
        }
        if (c == '[')
        {
            get();
            auto arr = std::make_shared<PdfArray>();

            size_t arrayLimit = 0;
            const size_t MAX_ARRAY_ITEMS = 10000;

            while (!eof())
            {
                if (++arrayLimit > MAX_ARRAY_ITEMS)
                {
                    break;
                }

                skipSpaces();
                if (peek() == ']')
                {
                    get();
                    break;
                }

                size_t posBefore = _pos;
                parseToken();

                if (_pos == posBefore && !_stack.empty())
                {
                    _stack.pop_back();
                    break;
                }

                if (!_stack.empty())
                {
                    arr->items.push_back(_stack.back());
                    _stack.pop_back();
                }
            }

            _stack.push_back(arr);
            return;
        }
        if (c == ']')
        {
            get();
            return;
        }
        if (c == '+' || c == '-' || c == '.' || std::isdigit(c))
        {
            _stack.push_back(std::make_shared<PdfNumber>(readNumber()));
            return;
        }

        size_t posBefore = _pos;
        std::string op = readWord();

        if (_pos == posBefore && !op.empty())
        {
            _pos++;
            return;
        }

        if (!op.empty())
            handleOperator(op);
    }

    void PdfContentParser::parse()
    {
        _pos = 0;
        _stack.clear();
        while (!_gsStack.empty()) _gsStack.pop();

        _currentFont = nullptr;

        // ✅ Dinamik limit: Her byte için ~1 iterasyon (güvenli üst sınır)
        // Büyük content stream'ler (örn. 500KB+) çok fazla operatör içerebilir
        size_t iterCount = 0;
        const size_t MAX_ITERS = std::max<size_t>(200000, _data.size() * 2);

        // ✅ PERFORMANCE: Log only for very large files
        // For Microsoft Print to PDF (2MB+), log every 5% instead of every 2%
        const bool isLargeFile = (_data.size() > 500000);  // > 500KB
        const size_t LOG_INTERVAL = isLargeFile
            ? std::max<size_t>(50000, MAX_ITERS / 20)  // Every 5%
            : std::max<size_t>(1000, MAX_ITERS / 50);   // Every 2%

        while (!eof())
        {
            if (++iterCount > MAX_ITERS)
            {
                LogDebug("ERROR: Exceeded max iterations (%zu) at pos %zu/%zu - possible infinite loop",
                    MAX_ITERS, _pos, _data.size());
                break;
            }

            // Only log progress for large files
            if (isLargeFile && iterCount % LOG_INTERVAL == 0)
            {
                LogDebug("Parse progress: %zu%% (%zu/%zu bytes, %zu iters)",
                    (_pos * 100) / _data.size(), _pos, _data.size(), iterCount);
            }

            parseToken();
        }

        LogDebug("PdfContentParser::parse() FINISHED - %zu iterations, %zu bytes",
            iterCount, _data.size());
    }


    // =========================================================
    // Path Operators
    // =========================================================

    void PdfContentParser::op_m()
    {
        double y = popNumber();
        double x = popNumber();

        _currentPath.push_back({ PdfPathSegment::MoveTo, x, y });

        _cpX = x;
        _cpY = y;

        _subpathStartX = x;
        _subpathStartY = y;
    }

    void PdfContentParser::op_h()
    {
        _currentPath.push_back({ PdfPathSegment::Close, 0, 0 });
        _cpX = _subpathStartX;
        _cpY = _subpathStartY;
    }


    void PdfContentParser::op_v()
    {
        double y3 = popNumber();
        double x3 = popNumber();
        double y2 = popNumber();
        double x2 = popNumber();

        double x1 = _cpX;
        double y1 = _cpY;

        _currentPath.emplace_back(
            x1, y1,   // control point 1 = current point
            x2, y2,   // control point 2
            x3, y3    // end point
        );

        _cpX = x3;
        _cpY = y3;
    }


    void PdfContentParser::op_y()
    {
        double y3 = popNumber();
        double x3 = popNumber();
        double y1 = popNumber();
        double x1 = popNumber();

        double x2 = x3;
        double y2 = y3;

        _currentPath.emplace_back(
            x1, y1,   // control point 1
            x2, y2,   // control point 2 = end point
            x3, y3
        );

        _cpX = x3;
        _cpY = y3;
    }


    void PdfContentParser::op_l()
    {
        double y = popNumber();
        double x = popNumber();

        _currentPath.push_back({ PdfPathSegment::LineTo, x, y });

        _cpX = x;
        _cpY = y;
    }


    void PdfContentParser::op_re()
    {
        double h = popNumber();
        double w = popNumber();
        double y = popNumber();
        double x = popNumber();

        _currentPath.push_back({ PdfPathSegment::MoveTo, x,     y });
        _currentPath.push_back({ PdfPathSegment::LineTo, x + w, y });
        _currentPath.push_back({ PdfPathSegment::LineTo, x + w, y + h });
        _currentPath.push_back({ PdfPathSegment::LineTo, x,     y + h });
        _currentPath.push_back({ PdfPathSegment::Close,  0, 0 });

    }

    void PdfContentParser::op_f()
    {
        // ========== DEBUG DISABLED FOR PERFORMANCE ==========
        // Uncomment for debugging fill operations
        /*
        static FILE* fillDebug = nullptr;
        static int fillCallCount = 0;
        if (!fillDebug) {
            char tempPath[MAX_PATH];
            GetTempPathA(MAX_PATH, tempPath);
            strcat(tempPath, "fill_debug.txt");
            fillDebug = fopen(tempPath, "w");
            if (fillDebug) {
                fprintf(fillDebug, "=== FILL DEBUG ===\n");
                fprintf(fillDebug, "Log file: %s\n", tempPath);
                fflush(fillDebug);
            }
        }
        fillCallCount++;

        int curveCount = 0, lineCount = 0, moveCount = 0;
        for (const auto& seg : _currentPath) {
            if (seg.type == PdfPathSegment::CurveTo) curveCount++;
            else if (seg.type == PdfPathSegment::LineTo) lineCount++;
            else if (seg.type == PdfPathSegment::MoveTo) moveCount++;
        }

        if (fillDebug) {
            fprintf(fillDebug, "\n[op_f #%d] _currentPath.size=%zu, moves=%d, lines=%d, CURVES=%d\n",
                fillCallCount, _currentPath.size(), moveCount, lineCount, curveCount);
            fprintf(fillDebug, "  fillPatternName='%s'\n", _gs.fillPatternName.c_str());
            fprintf(fillDebug, "  CTM=[%.4f %.4f %.4f %.4f %.4f %.4f]\n",
                _gs.ctm.a, _gs.ctm.b, _gs.ctm.c, _gs.ctm.d, _gs.ctm.e, _gs.ctm.f);
            fflush(fillDebug);
        }
        */
        // ========== END DEBUG ==========

        // ✅ FIX: Skip completely transparent fills (alpha = 0)
        if (_gs.fillAlpha <= 0.001)
        {
            _currentPath.clear();
            return;
        }

        if (_painter)
        {
            // =====================================================
            // ✅ PATTERN FILL KONTROLÜ
            // Eğer fillPatternName ayarlıysa, gradient ile fill et
            // =====================================================
            if (!_gs.fillPatternName.empty())
            {

                // 1. Try Resolving Tiling Pattern (Type 1)
                PdfPattern pattern;
                if (resolvePattern(_gs.fillPatternName, pattern))
                {

                    if (pattern.isUncolored) {
                        pattern.baseColor = rgbToArgbWithAlpha(_gs.fillColor, _gs.fillAlpha);
                    }

                    _painter->fillPathWithPattern(
                        _currentPath,
                        pattern,
                        _gs.ctm,
                        false // evenOdd
                    );

                    _currentPath.clear();
                    return;
                }

                PdfGradient gradient;
                PdfMatrix patternMatrix;

                if (resolvePatternToGradient(_gs.fillPatternName, gradient, patternMatrix))
                {

                    PdfMatrix gradientCTM = PdfMul(patternMatrix, _gs.ctm);

                    _painter->fillPathWithGradient(
                        _currentPath,
                        gradient,
                        _gs.ctm,        // clip/path CTM
                        gradientCTM,    // gradient CTM = PatternMatrix * CTM
                        false           // even-odd = false
                    );

                    _currentPath.clear();
                    return;
                }
                else
                {
                }
            }

            // Normal düz renk fill - alpha ile
            _painter->fillPath(
                _currentPath,
                rgbToArgbWithAlpha(_gs.fillColor, _gs.fillAlpha),
                _gs.ctm,
                false,  // evenOdd
                _hasClippingPath ? &_clippingPath : nullptr,
                _hasClippingPath ? &_clippingPathCTM : nullptr,
                _clippingEvenOdd
            );
        }

        _currentPath.clear();
    }


    void PdfContentParser::op_S()
    {
        if (!_painter)  // ✅ Null kontrolü ekle
        {
            _currentPath.clear();
            return;
        }

        // ✅ FIX: Skip completely transparent strokes (alpha = 0)
        if (_gs.strokeAlpha <= 0.001)
        {
            _currentPath.clear();
            return;
        }

        // Stroke işlemi - alpha ile
        uint32_t strokeArgb = rgbToArgbWithAlpha(_gs.strokeColor, _gs.strokeAlpha);

        _painter->strokePath(
            _currentPath,
            strokeArgb,
            _gs.lineWidth,
            _gs.ctm,
            _gs.lineCap,
            _gs.lineJoin,
            _gs.miterLimit
        );

        _currentPath.clear();
    }

    // =========================================================
    // Pattern → Gradient Resolution
    // =========================================================

    bool PdfContentParser::resolvePatternToGradient(
        const std::string& patternName,
        PdfGradient& gradient,
        PdfMatrix& patternMatrix)
    {
        if (!_doc) {
            return false;
        }

        // Pattern adını normalize et
        std::string name = patternName;
        if (!name.empty() && name[0] == '/') {
            name = name.substr(1);
        }

        LogDebug("resolvePatternToGradient: Looking for pattern '%s' (normalized from '%s')",
            name.c_str(), patternName.c_str());

        // Resources'tan Pattern dictionary'yi bul
        std::set<int> visited;

        int resIndex = 0;
        for (auto it = _resStack.rbegin(); it != _resStack.rend(); ++it, ++resIndex)
        {
            auto res = *it;
            if (!res) {
                continue;
            }


            auto patternsRaw = res->get("Pattern");
            if (!patternsRaw) patternsRaw = res->get("/Pattern");
            if (!patternsRaw) {
                continue;
            }


            auto patternsObj = _doc->resolve(patternsRaw, visited);
            auto patternsDict = std::dynamic_pointer_cast<PdfDictionary>(patternsObj);
            if (!patternsDict) {
                continue;
            }


            // Debug: tüm pattern key'lerini listele
            for (const auto& entry : patternsDict->entries) {
            }

            auto patternRaw = patternsDict->get(name);
            if (!patternRaw) patternRaw = patternsDict->get("/" + name);
            if (!patternRaw) {
                continue;
            }


            auto patternObj = _doc->resolve(patternRaw, visited);
            auto patternDict = std::dynamic_pointer_cast<PdfDictionary>(patternObj);
            if (!patternDict) {
                continue;
            }

            // PatternType kontrol et
            auto ptRaw = patternDict->get("PatternType");
            if (!ptRaw) ptRaw = patternDict->get("/PatternType");
            if (!ptRaw) {
                continue;
            }

            auto ptNum = std::dynamic_pointer_cast<PdfNumber>(ptRaw);
            int patternType = ptNum ? (int)ptNum->value : 0;

            if (patternType != 2) {
                continue;
            }

            // Pattern Matrix
            patternMatrix = PdfMatrix(); // identity default
            auto matrixRaw = patternDict->get("Matrix");
            if (!matrixRaw) matrixRaw = patternDict->get("/Matrix");
            if (matrixRaw) {
                patternMatrix = readMatrix6(matrixRaw);
                LogDebug("  Pattern matrix: [%.3f %.3f %.3f %.3f %.3f %.3f]",
                    patternMatrix.a, patternMatrix.b, patternMatrix.c,
                    patternMatrix.d, patternMatrix.e, patternMatrix.f);
            }
            else {
            }

            // Shading dictionary
            auto shadingRaw = patternDict->get("Shading");
            if (!shadingRaw) shadingRaw = patternDict->get("/Shading");
            if (!shadingRaw) {
                continue;
            }

            auto shadingObj = _doc->resolve(shadingRaw, visited);
            auto shadingDict = std::dynamic_pointer_cast<PdfDictionary>(shadingObj);
            if (!shadingDict) {
                continue;
            }

            // ShadingType
            auto stRaw = shadingDict->get("ShadingType");
            if (!stRaw) stRaw = shadingDict->get("/ShadingType");
            if (!stRaw) {
                continue;
            }

            auto stNum = std::dynamic_pointer_cast<PdfNumber>(stRaw);
            int shadingType = stNum ? (int)stNum->value : 0;

            if (shadingType != 2 && shadingType != 3) {
                continue;
            }

            gradient.type = shadingType;

            // Coords
            auto coordsRaw = shadingDict->get("Coords");
            if (!coordsRaw) coordsRaw = shadingDict->get("/Coords");
            if (!coordsRaw) {
                continue;
            }

            auto coordsObj = _doc->resolve(coordsRaw, visited);
            auto coordsArr = std::dynamic_pointer_cast<PdfArray>(coordsObj);
            if (!coordsArr || coordsArr->items.size() < 4) {
                continue;
            }

            std::vector<double> coords(6, 0.0);
            for (size_t i = 0; i < coordsArr->items.size() && i < 6; ++i) {
                auto n = std::dynamic_pointer_cast<PdfNumber>(_doc->resolve(coordsArr->items[i], visited));
                if (n) coords[i] = n->value;
            }

            if (shadingType == 2) {
                // Axial gradient
                gradient.x0 = coords[0];
                gradient.y0 = coords[1];
                gradient.x1 = coords[2];
                gradient.y1 = coords[3];
                LogDebug("  Axial gradient: (%.2f,%.2f) -> (%.2f,%.2f)",
                    gradient.x0, gradient.y0, gradient.x1, gradient.y1);
            }
            else if (shadingType == 3) {
                // Radial gradient
                gradient.x0 = coords[0];
                gradient.y0 = coords[1];
                gradient.r0 = coords[2];
                gradient.x1 = coords[3];
                gradient.y1 = coords[4];
                gradient.r1 = coords[5];
                LogDebug("  Radial gradient: (%.2f,%.2f,%.2f) -> (%.2f,%.2f,%.2f)",
                    gradient.x0, gradient.y0, gradient.r0,
                    gradient.x1, gradient.y1, gradient.r1);
            }

            // ColorSpace
            int numComponents = 3;
            auto csRaw = shadingDict->get("ColorSpace");
            if (!csRaw) csRaw = shadingDict->get("/ColorSpace");
            if (csRaw) {
                auto csObj = _doc->resolve(csRaw, visited);
                if (auto csName = std::dynamic_pointer_cast<PdfName>(csObj)) {
                    std::string cs = csName->value;
                    if (cs == "/DeviceGray" || cs == "DeviceGray") numComponents = 1;
                    else if (cs == "/DeviceCMYK" || cs == "DeviceCMYK") numComponents = 4;
                }
                else if (auto csArr = std::dynamic_pointer_cast<PdfArray>(csObj)) {
                    if (!csArr->items.empty()) {
                        auto first = std::dynamic_pointer_cast<PdfName>(
                            _doc->resolve(csArr->items[0], visited));
                        if (first) {
                            std::string csType = first->value;
                            if (csType == "/ICCBased" || csType == "ICCBased") {
                                if (csArr->items.size() >= 2) {
                                    auto iccStream = std::dynamic_pointer_cast<PdfStream>(
                                        _doc->resolve(csArr->items[1], visited));
                                    if (iccStream && iccStream->dict) {
                                        auto nObj = std::dynamic_pointer_cast<PdfNumber>(
                                            _doc->resolve(iccStream->dict->get("/N"), visited));
                                        if (nObj) numComponents = (int)nObj->value;
                                    }
                                }
                            }
                            else if (csType == "/Separation" || csType == "Separation") {
                                numComponents = 1;
                            }
                            else if (csType == "/DeviceN" || csType == "DeviceN") {
                                // Check alternate space
                                if (csArr->items.size() >= 3) {
                                    auto altCS = _doc->resolve(csArr->items[2], visited);
                                    if (auto altName = std::dynamic_pointer_cast<PdfName>(altCS)) {
                                        std::string alt = altName->value;
                                        if (alt == "/DeviceCMYK" || alt == "DeviceCMYK")
                                            numComponents = 4;
                                        else if (alt == "/DeviceGray" || alt == "DeviceGray")
                                            numComponents = 1;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Function
            auto funcRaw = shadingDict->get("Function");
            if (!funcRaw) funcRaw = shadingDict->get("/Function");
            if (funcRaw) {
                auto funcObj = _doc->resolve(funcRaw, visited);
                if (!PdfGradient::parseFunctionToGradient(funcObj, _doc, gradient, numComponents)) {
                    // Fallback: basit iki renk
                    GradientStop s0, s1;
                    s0.position = 0.0;
                    s0.rgb[0] = 1.0; s0.rgb[1] = 0.9; s0.rgb[2] = 0.0; // Sarı
                    s1.position = 1.0;
                    s1.rgb[0] = 1.0; s1.rgb[1] = 0.95; s1.rgb[2] = 0.5; // Açık sarı
                    gradient.stops.push_back(s0);
                    gradient.stops.push_back(s1);
                }
            }
            else {
                GradientStop s0, s1;
                s0.position = 0.0;
                s0.rgb[0] = 1.0; s0.rgb[1] = 0.9; s0.rgb[2] = 0.0;
                s1.position = 1.0;
                s1.rgb[0] = 1.0; s1.rgb[1] = 0.95; s1.rgb[2] = 0.5;
                gradient.stops.push_back(s0);
                gradient.stops.push_back(s1);
            }

            LogDebug("Pattern '%s' resolved successfully with %zu stops",
                name.c_str(), gradient.stops.size());
            return true;
        }

        return false;
    }

    bool PdfContentParser::resolvePattern(
        const std::string& patternName,
        PdfPattern& pattern)
    {
        if (!_doc) return false;

        std::string name = patternName;
        if (!name.empty() && name[0] == '/') name.erase(0, 1);

        // Find Resource
        std::shared_ptr<PdfObject> patternObj;
        int resIndex = 0;
        for (auto it = _resStack.rbegin(); it != _resStack.rend(); ++it, ++resIndex)
        {
            auto res = *it;
            if (!res) continue;
            auto patternsRaw = res->get("/Pattern");
            if (!patternsRaw) continue;

            std::set<int> visited;
            auto patternsObj = _doc->resolve(patternsRaw, visited);
            auto patternsDict = std::dynamic_pointer_cast<PdfDictionary>(patternsObj);
            if (!patternsDict) continue;

            auto patternRaw = patternsDict->get(name);
            if (!patternRaw) patternRaw = patternsDict->get("/" + name);
            if (patternRaw) {
                patternObj = _doc->resolve(patternRaw, visited);
                if (patternObj) break;
            }
        }

        if (!patternObj) {
            return false;
        }

        auto patternDict = std::dynamic_pointer_cast<PdfDictionary>(patternObj);
        if (!patternDict) return false;

        // PatternType
        auto ptNum = std::dynamic_pointer_cast<PdfNumber>(resolveObj(patternDict->get("/PatternType")));
        int type = ptNum ? (int)ptNum->value : 0;

        pattern.type = type;

        // Matrix
        pattern.matrix = PdfMatrix(); // identity
        auto matrixRaw = patternDict->get("/Matrix");
        if (matrixRaw) pattern.matrix = readMatrix6(matrixRaw);

        if (type == 1)
        {
            // Tiling Pattern

            auto ptRaw = patternDict->get("/PaintType");
            auto tmRaw = patternDict->get("/TilingType");
            auto xsRaw = patternDict->get("/XStep");
            auto ysRaw = patternDict->get("/YStep");

            auto ptN = std::dynamic_pointer_cast<PdfNumber>(resolveObj(ptRaw));
            auto tmN = std::dynamic_pointer_cast<PdfNumber>(resolveObj(tmRaw));
            auto xsN = std::dynamic_pointer_cast<PdfNumber>(resolveObj(xsRaw));
            auto ysN = std::dynamic_pointer_cast<PdfNumber>(resolveObj(ysRaw));

            pattern.isUncolored = (ptN && (int)ptN->value == 2);
            pattern.tilingType = tmN ? (int)tmN->value : 1;
            pattern.xStep = xsN ? xsN->value : 0;
            pattern.yStep = ysN ? ysN->value : 0;

            // Eğer uncolored ise mevcut color space'e göre renk ayarlanmalı. (Caller yapacak)

            // Render Tile
            // Tip dönüşümü: Type 1 Pattern bir Stream olmalıdır.
            auto stream = std::dynamic_pointer_cast<PdfStream>(patternObj);
            if (!stream) {
                return false;
            }
            // Stream dictionary ile pattern properties aynıdır.
            renderPatternTile(patternDict, pattern);

            // Stream decoding renderPatternTile içinde yapılacak ama stream objesine ihtiyacımız var.
            // renderPatternTile imzasını değiştirmek yerine stream'i burada decode edip verebiliriz 
            // VEYA renderPatternTile içinde patternObj üzerinden cast deneriz (ama patternObj local).
            // Düzeltme: renderPatternTile'ı PdfDictionary alacak şekilde tanımladık ama 
            // aslında PdfStream lazım. 
            // Burada stream decode yapıp raw data ile çağırmak daha temiz ama 
            // renderPatternTile, içeriği parse etmek için 'PdfContentParser' başlatacak.

            // Quick fix: decodeStream here
            std::vector<uint8_t> decoded;
            if (_doc->decodeStream(stream, decoded)) {
                // Şimdi bu decoded datayı ve dict'i kullanarak renderla
                // renderPatternTile(decoded, patternDict, pattern) şeklinde olmalı.
                // Mevcut imza (dict, pattern) yetersiz.
                // İmzayı overload edelim veya içeriği güncelleyelim.
                // Helper method'u private olarak düzenlemek en iyisi ama header değiştirmeyelim.
                // Sadece implementation'ı hackleyelim: 
                // renderPatternTile içine stream decoding logic'i koymak zordur çünkü sadece dict alıyor.
                // AMA, PdfStream yapısı genelde dict ile pointer paylaşır mı? Hayır.
                // PdfDictionary, stream datasına sahip değil.

                // ÇÖZÜM: renderPatternTile metodunu KULLANMADAN direkt buraya yazalım (Inline).

                // 1. BBox
                auto bboxArr = std::dynamic_pointer_cast<PdfArray>(resolveObj(patternDict->get("/BBox")));
                if (!bboxArr || bboxArr->items.size() < 4) return false;

                double bx = std::dynamic_pointer_cast<PdfNumber>(resolveObj(bboxArr->items[0]))->value;
                double by = std::dynamic_pointer_cast<PdfNumber>(resolveObj(bboxArr->items[1]))->value;
                double bw = std::dynamic_pointer_cast<PdfNumber>(resolveObj(bboxArr->items[2]))->value;
                double bh = std::dynamic_pointer_cast<PdfNumber>(resolveObj(bboxArr->items[3]))->value;
                double width = bw - bx;
                double height = bh - by;
                if (width <= 0 || height <= 0) return false;

                // 2. Setup resolution
                double scale = 1.0;
                // Eğer pattern 1x1 birimse ve ekranda 100 pikselse?
                // Scale factor kritik.
                // MuPDF, CTM'e göre scale hesaplar.
                // _gs.ctm ile patternMatrix çarpılır, oradaki scale'e bakılır.
                // Basitlik için: Global bir scale veya 1.0.
                // Şimdilik 1.0 ile devam.

                int bufW = (int)std::ceil(width * scale);
                int bufH = (int)std::ceil(height * scale);

                // Safe clamping using standard library
                bufW = std::max(1, std::min(bufW, 2048));
                bufH = std::max(1, std::min(bufH, 2048));

                pattern.width = bufW;
                pattern.height = bufH;

                // 3. Render
                PdfPainter tilePainter(bufW, bufH, 1.0, 1.0, 1);
                tilePainter.clear(0x00000000);

                PdfGraphicsState tileGS = _gs;
                // CTM: Translate(-bx, -by) -> M(1,0,0,1,-bx,-by)
                // Ama Painter Y-down.
                // BBox (0,0) -> Bottom Left.
                // Painter (0,0) -> Top Left.
                // İçerik parse edilirken "User Space" (Pattern Space) kullanılır.
                // PdfContentParser normalde CTM uygular ve sonra Painter'a gönderir.
                // Painter "User -> Device" için scale ve mapY uygular.
                // Tile render ederken:
                // Device Space = Buffer (0..W, 0..H).
                // User Space = Pattern Space (bx..bx+w, by..by+h).
                // Biz sadece Translation vermeliyiz.

                // PdfPainter constructor scale=1.0.
                // mapY(y) = H - y * scaleY.
                // ctm.f düzeltmesi?

                // Standart CTM: Identity.
                tileGS.ctm = PdfMatrix();
                tileGS.ctm.e = -bx;
                tileGS.ctm.f = -by;

                // Resources
                std::vector<std::shared_ptr<PdfDictionary>> childResStack = _resStack;
                auto rObj = patternDict->get("/Resources");
                auto patRes = resolveDict(rObj);
                if (patRes) {
                    childResStack.push_back(patRes);
                    if (_fonts && _doc)
                        _doc->loadFontsFromResourceDict(patRes, *_fonts);
                }

                PdfContentParser child(
                    decoded,
                    &tilePainter,
                    _doc,
                    _pageIndex,
                    _fonts,
                    tileGS,
                    childResStack
                );

                child.parse();

                // 4. Capture Buffer
                pattern.buffer.resize(bufW * bufH);
                const auto& rawBuf = tilePainter.getBuffer();
                // rawBuf is vector<uint8_t> BGRA (4 bytes per pixel)
                // pattern.buffer is vector<uint32_t>
                if (rawBuf.size() >= bufW * bufH * 4) {
                    memcpy(pattern.buffer.data(), rawBuf.data(), bufW * bufH * 4);
                }

                LogDebug("Rendered Pattern Tile: %dx%d (original bbox: %.2f %.2f %.2f %.2f)",
                    bufW, bufH, bx, by, bw, bh);

                return true;
            }
            else {
                return false;
            }
        }
        else if (type == 2)
        {
            return false;
        }

        return false;
    }

    void PdfContentParser::renderPatternTile(
        const std::shared_ptr<PdfDictionary>& patternDict,
        PdfPattern& pattern)
    {
        // Deprecated/Unused because implementation is inlined above 
        // to access PdfStream efficiently.
    }

    // =========================================================
    // Graphics State
    // =========================================================

    void PdfContentParser::op_cm()
    {
        double f = popNumber();
        double e = popNumber();
        double d = popNumber();
        double c = popNumber();
        double b = popNumber();
        double a = popNumber();

        PdfMatrix m;
        m.a = a; m.b = b;
        m.c = c; m.d = d;
        m.e = e; m.f = f;

        _gs.ctm = PdfMul(m, _gs.ctm);
    }

    void PdfContentParser::op_q()
    {
        _gsStack.push(_gs);
        _clippingPathStack.push(_clippingPath);
        _clippingPathCTMStack.push(_clippingPathCTM);
        _hasClippingPathStack.push(_hasClippingPath);
        _clippingEvenOddStack.push(_clippingEvenOdd);

        // Save clip layer count for this q/Q level, reset for new level
        _clipLayerCountStack.push(_clipLayerCount);
        _clipLayerCount = 0;

        // Rect clipping stack
        _rectClippingPathStack.push(_rectClippingPath);
        _rectClippingPathCTMStack.push(_rectClippingPathCTM);
        _hasRectClippingStack.push(_hasRectClipping);
    }

    void PdfContentParser::op_Q()
    {
        // Pop D2D clip layers pushed at this q/Q level
        if (_clipLayerCount > 0 && _painter) {
            for (int i = 0; i < _clipLayerCount; i++) {
                _painter->popClipPath();
            }
        }

        // Restore clip layer count from parent level
        if (!_clipLayerCountStack.empty()) {
            _clipLayerCount = _clipLayerCountStack.top();
            _clipLayerCountStack.pop();
        } else {
            _clipLayerCount = 0;
        }

        if (!_gsStack.empty())
        {
            _gs = _gsStack.top();
            _gsStack.pop();
        }

        if (!_clippingPathStack.empty())
        {
            _clippingPath = _clippingPathStack.top();
            _clippingPathStack.pop();
        }

        if (!_clippingPathCTMStack.empty())
        {
            _clippingPathCTM = _clippingPathCTMStack.top();
            _clippingPathCTMStack.pop();
        }

        if (!_hasClippingPathStack.empty())
        {
            _hasClippingPath = _hasClippingPathStack.top();
            _hasClippingPathStack.pop();
        }

        if (!_clippingEvenOddStack.empty())
        {
            _clippingEvenOdd = _clippingEvenOddStack.top();
            _clippingEvenOddStack.pop();
        }

        // Rect clipping restore
        if (!_rectClippingPathStack.empty())
        {
            _rectClippingPath = _rectClippingPathStack.top();
            _rectClippingPathStack.pop();
        }
        if (!_rectClippingPathCTMStack.empty())
        {
            _rectClippingPathCTM = _rectClippingPathCTMStack.top();
            _rectClippingPathCTMStack.pop();
        }
        if (!_hasRectClippingStack.empty())
        {
            _hasRectClipping = _hasRectClippingStack.top();
            _hasRectClippingStack.pop();
        }
    }

    // =========================================================
    // Text State
    // =========================================================

    void PdfContentParser::op_BT()
    {
        _gs.textMatrix = PdfMatrix();
        _gs.textLineMatrix = PdfMatrix();
        _gs.textPosX = 0;
        _gs.textPosY = 0;

        // Start text block batching (MuPDF-style optimization)
        if (_painter)
            _painter->beginTextBlock();

        // Push clip for text block if we have a clipping path
        if (_hasClippingPath && _painter && !_clippingPath.empty()) {
            _painter->pushClipPath(_clippingPath, _clippingPathCTM, _clippingEvenOdd);
            _textBlockClipPushed = true;
        } else {
            _textBlockClipPushed = false;
        }
    }

    void PdfContentParser::op_ET()
    {
        // Flush all batched text (MuPDF-style optimization)
        if (_painter)
            _painter->endTextBlock();

        // Metin bloğu clip'ini kaldır
        if (_textBlockClipPushed) {
            if (_painter) _painter->popClipPath();
            _textBlockClipPushed = false;
        }
    }

    void PdfContentParser::op_Tf()
    {
        double size = popNumber(12.0);
        std::string fontName = popName(); // /F1

        _gs.fontSize = size;
        _currentFont = nullptr;

        // DEBUG: Font secimi
        {
            static FILE* tfDbg = nullptr; // fopen("C:\\temp\\tf_debug.txt", "a");
            if (tfDbg) {
                fprintf(tfDbg, "Tf: fontName='%s', size=%.2f\n", fontName.c_str(), size);
                if (_fonts) {
                    fprintf(tfDbg, "  _fonts has %zu entries\n", _fonts->size());
                    for (auto& kv : *_fonts) {
                        fprintf(tfDbg, "    '%s' -> '%s'\n", kv.first.c_str(), kv.second.baseFont.c_str());
                    }
                }
                fflush(tfDbg);
            }
        }

        if (_fonts)
        {
            auto it = _fonts->find(fontName);
            if (it != _fonts->end())
            {
                _currentFont = &it->second;

                // FreeType hazır değilse yükle
                if (_doc && _currentFont && !_currentFont->ftReady)
                {
                    // embedded font varsa onu yükle
                    if (!_currentFont->fontProgram.empty())
                    {
                        _doc->prepareFreeTypeFont(*_currentFont);
                    }
                    else
                    {
                        // embedded yoksa fallback
                        _doc->loadFallbackFont(*_currentFont);
                    }
                }
            }
        }

        if (std::abs(_gs.leading) < 0.001)
            _gs.leading = size;
    }


    void PdfContentParser::op_TL()
    {
        _gs.leading = popNumber(0.0);
    }

    void PdfContentParser::op_Tm()
    {
        double f = popNumber();
        double e = popNumber();
        double d = popNumber();
        double c = popNumber();
        double b = popNumber();
        double a = popNumber();

        PdfMatrix m;
        m.a = a; m.b = b;
        m.c = c; m.d = d;
        m.e = e; m.f = f;

        _gs.textMatrix = m;
        _gs.textLineMatrix = m;

        _gs.textPosX = m.e;
        _gs.textPosY = m.f;
    }

    void PdfContentParser::op_Td()
    {
        double ty = popNumber();
        double tx = popNumber();

        // PDF spec: Td moves to the start of the next line,
        // offset from the start of the current line by (tx, ty).
        // The translation is applied in text line space:
        // T_lm = [ 1 0 0 1 tx ty ] × T_lm (previous)

        // yeni textLineMatrix = translation × eski textLineMatrix
        // Bu şu anlama gelir: (tx, ty) kadar kaydır, sonra eski dönüşümü uygula
        _gs.textLineMatrix.e += tx * _gs.textLineMatrix.a + ty * _gs.textLineMatrix.c;
        _gs.textLineMatrix.f += tx * _gs.textLineMatrix.b + ty * _gs.textLineMatrix.d;

        _gs.textMatrix = _gs.textLineMatrix;

        _gs.textPosX = _gs.textMatrix.e;
        _gs.textPosY = _gs.textMatrix.f;
    }

    void PdfContentParser::op_Tstar()
    {
        // T* is equivalent to: 0 -TL Td
        double tx = 0.0;
        double ty = -_gs.leading;

        // Same as Td: translate in text line space
        _gs.textLineMatrix.e += tx * _gs.textLineMatrix.a + ty * _gs.textLineMatrix.c;
        _gs.textLineMatrix.f += tx * _gs.textLineMatrix.b + ty * _gs.textLineMatrix.d;

        _gs.textMatrix = _gs.textLineMatrix;

        _gs.textPosX = _gs.textMatrix.e;
        _gs.textPosY = _gs.textMatrix.f;
    }

    // =========================================================
    // UTF-16 / Encoding Decode
    // =========================================================

    std::wstring PdfContentParser::decodeText(const std::string& raw)
    {
        std::wstring result;

        // 1) Font bilgisi yoksa → düz 1-byte ASCII/Latin-1
        if (!_currentFont)
        {
            for (unsigned char c : raw)
                result.push_back((wchar_t)c);

            // Türkçe düzeltme
            for (auto& ch : result)
            {
                switch (ch)
                {
                case 0xDD: ch = L'İ'; break; // Ý -> İ
                case 0xDE: ch = L'Ş'; break; // Þ -> Ş
                case 0xF0: ch = L'ğ'; break; // ð -> ğ
                case 0xFD: ch = L'ı'; break; // ý -> ı
                case 0xFE: ch = L'ş'; break; // þ -> ş
                case 0xD0: ch = L'Ğ'; break; // Ð -> Ğ
                }
            }
            return result;
        }

        const PdfFontInfo& fi = *_currentFont;

        // ------------------------------------------------------
        // 2) CID fontlar / Identity-H / Identity-V
        // ------------------------------------------------------
        if (fi.isCidFont ||
            fi.encoding == "/Identity-H" ||
            fi.encoding == "/Identity-V")
        {
            std::wstring out;

            if (raw.empty())
                return out;

            // 2-byte big-endian CID
            for (size_t i = 0; i + 1 < raw.size(); i += 2)
            {
                uint16_t code = ((uint8_t)raw[i] << 8) | (uint8_t)raw[i + 1];

                uint32_t uni = code;
                auto it = fi.cidToUnicode.find(code);
                if (it != fi.cidToUnicode.end())
                    uni = it->second;

                if (uni <= 0xFFFF)
                {
                    out.push_back((wchar_t)uni);
                }
                else
                {
                    // U+10000 üzeri için surrogate pair
                    uint32_t cp = uni - 0x10000;
                    wchar_t high = (wchar_t)(0xD800 + (cp >> 10));
                    wchar_t low = (wchar_t)(0xDC00 + (cp & 0x3FF));
                    out.push_back(high);
                    out.push_back(low);
                }
            }

            // Türkçe düzeltme
            for (auto& ch : out)
            {
                switch (ch)
                {
                case 0xDD: ch = L'İ'; break;
                case 0xDE: ch = L'Ş'; break;
                case 0xF0: ch = L'ğ'; break;
                case 0xFD: ch = L'ı'; break;
                case 0xFE: ch = L'ş'; break;
                case 0xD0: ch = L'Ğ'; break;
                }
            }
            return out;
        }

        // ------------------------------------------------------
        // 3) Simple 1-byte fontlar
        // ------------------------------------------------------
        for (unsigned char c : raw)
        {
            uint32_t uni = 0;

            if (fi.hasSimpleMap)
            {
                uni = fi.codeToUnicode[c];
            }
            else if (fi.encoding == "/WinAnsiEncoding" || fi.encoding.empty())
            {
                uni = WinAnsi[c];
            }
            else if (fi.encoding == "/MacRomanEncoding")
            {
                uni = (uint32_t)c;
            }
            else
            {
                uni = (uint32_t)c;
            }

            result.push_back((wchar_t)uni);
        }

        // ------------------------------------------------------
        // 4) Türkçe CP1254 düzeltme hack'i
        // ------------------------------------------------------
        for (auto& ch : result)
        {
            switch (ch)
            {
            case 0xDD: ch = L'İ'; break; // Ý -> İ
            case 0xDE: ch = L'Ş'; break; // Þ -> Ş
            case 0xF0: ch = L'ğ'; break; // ð -> ğ
            case 0xFD: ch = L'ı'; break; // ý -> ı
            case 0xFE: ch = L'ş'; break; // þ -> ş
            case 0xD0: ch = L'Ğ'; break; // Ð -> Ğ
            }
        }

        return result;
    }

    // =========================================================
    // Text Operators
    // =========================================================
    void PdfContentParser::op_Tj()
    {
        std::string raw = popString();
        if (!_painter || raw.empty() || !_currentFont)
            return;

        // Text space'te başlangıç noktası
        double tx = 0.0;
        double ty = _gs.textRise;

        // Text Matrix ile user space'e dönüştür
        double ux = _gs.textMatrix.a * tx +
            _gs.textMatrix.c * ty +
            _gs.textMatrix.e;

        double uy = _gs.textMatrix.b * tx +
            _gs.textMatrix.d * ty +
            _gs.textMatrix.f;

        // CTM uygula (user space -> device space için)
        double x, y;
        ApplyMatrixPoint(_gs.ctm, ux, uy, x, y);

        // =========================================
        // EFFECTIVE FONT SIZE HESAPLA
        // =========================================
        // PDF'de font size iki matris ile scale edilebilir:
        // 1. Text Matrix (Tm) - text space → user space
        // 2. CTM (cm) - user space → page space
        //
        // Örnek: cm=[0.8 0 0 -0.8 60 651], Tm=[11 0 0 -11 0 0], Tf=1
        // -> Effective = 1 * 11 * 0.8 = 8.8 pt
        //
        double tmScaleY = std::sqrt(_gs.textMatrix.c * _gs.textMatrix.c +
            _gs.textMatrix.d * _gs.textMatrix.d);
        double ctmScaleY = std::sqrt(_gs.ctm.c * _gs.ctm.c +
            _gs.ctm.d * _gs.ctm.d);

        // Effective font size = fontSize * textMatrix scale * CTM scale
        double effectiveFontSize = _gs.fontSize * tmScaleY * ctmScaleY;

        // =========================================
        // METİN ÇİZ
        // =========================================
        // drawTextFreeTypeRaw kendi içinde advance hesaplayıp
        // karakterleri doğru pozisyonlara çizer.
        // Dönen değer: toplam advance (user space, point cinsinden)

        // X yönü scale'leri (text matrix advance dönüşümü için)
        double tmScaleX = std::sqrt(_gs.textMatrix.a * _gs.textMatrix.a +
            _gs.textMatrix.b * _gs.textMatrix.b);
        double ctmScaleX = std::sqrt(_gs.ctm.a * _gs.ctm.a +
            _gs.ctm.b * _gs.ctm.b);

        // charSpacing ve wordSpacing X-scale bazlı (yatay bir yer değiştirme).
        // Painter advance hesabı advanceSizePt (X-scale) ile yapılır,
        // spacing de aynı uzayda olmalı.
        double effectiveCharSpacing = _gs.charSpacing * tmScaleX * ctmScaleX;
        double effectiveWordSpacing = _gs.wordSpacing * tmScaleX * ctmScaleX;

        // advanceSizePt = X-scale bazlı (yatay advance için)
        // fontSizePt = Y-scale bazlı (glyph yüksekliği/render boyutu için)
        // Non-uniform text matrix (örn [7.2 0 0 8]) durumunda:
        //   fontSizePt = fontSize * 8 * ctmScaleY  (glyph render boyutu)
        //   advanceSizePt = fontSize * 7.2 * ctmScaleX (yatay advance boyutu)
        //   -> glyph'ler X yönünde %90 sıkıştırılır (7.2/8 = 0.9)
        double effectiveAdvanceSize = _gs.fontSize * tmScaleX * ctmScaleX;

        // Text direction angle in page space (for rotated text matrices)
        // Text advances in direction (Tm.a, Tm.b) in user space;
        // after CTM: dx_page = ctm.a*tm.a + ctm.c*tm.b, dy_page = ctm.b*tm.a + ctm.d*tm.b
        double dx_page = _gs.ctm.a * _gs.textMatrix.a + _gs.ctm.c * _gs.textMatrix.b;
        double dy_page = _gs.ctm.b * _gs.textMatrix.a + _gs.ctm.d * _gs.textMatrix.b;
        double textAngle = std::atan2(dy_page, dx_page);

        double drawnAdvance = _painter->drawTextFreeTypeRaw(
            x,
            y,
            raw,
            effectiveFontSize,
            effectiveAdvanceSize,
            rgbToArgb(_gs.fillColor),
            _currentFont,
            effectiveCharSpacing,
            effectiveWordSpacing,
            _gs.horizontalScale,
            textAngle
        );

        // =========================================
        // TEXT MATRIX İLERLET
        // =========================================
        // Painter X-scale bazlı advance döndürür (advanceSizePt = X-scale).
        // textAdvance() tx'i Tm.a ile çarpar, dolayısıyla denom = tmScaleX * ctmScaleX:
        //   textAdv = painterAdv / (tmScaleX * ctmScaleX)
        //   textAdvance: Tm.e += textAdv * Tm.a ≈ painterAdv
        double denom = ctmScaleX * tmScaleX;
        double adv;
        if (denom > 0.0001) {
            adv = drawnAdvance / denom;
        } else {
            // Fallback: eski hesaplama
            adv = computeAdvanceFromRaw(
                _currentFont, raw,
                _gs.fontSize, _gs.charSpacing, _gs.wordSpacing
            );
            adv *= (_gs.horizontalScale / 100.0);
        }

        // Text matrix'i ilerlet
        textAdvance(_gs, adv, 0.0);
    }


    void PdfContentParser::op_TJ()
    {
        auto arr = std::dynamic_pointer_cast<PdfArray>(_stack.back());
        _stack.pop_back();

        if (!arr || !_painter || !_currentFont)
            return;

        // ========== TJ DEBUG ==========
        static FILE* tjDebug = nullptr;
        if (!tjDebug) {
            tjDebug = nullptr; // fopen("C:\\temp\\tj_debug.txt", "w");
            if (tjDebug) {
                fprintf(tjDebug, "=== TJ OPERATOR DEBUG ===\n");
                fflush(tjDebug);
            }
        }
        if (tjDebug) {
            fprintf(tjDebug, "\n--- TJ Array: %zu items ---\n", arr->items.size());
            fprintf(tjDebug, "Font: %s, encoding: %s\n",
                _currentFont->baseFont.c_str(), _currentFont->encoding.c_str());
            fprintf(tjDebug, "hasCodeToGid: %d, hasSimpleMap: %d\n",
                _currentFont->hasCodeToGid ? 1 : 0, _currentFont->hasSimpleMap ? 1 : 0);
            fprintf(tjDebug, "fontSize: %.2f, Tc: %.4f, Tw: %.4f\n",
                _gs.fontSize, _gs.charSpacing, _gs.wordSpacing);
            fprintf(tjDebug, "TextMatrix: [%.4f %.4f %.4f %.4f %.4f %.4f]\n",
                _gs.textMatrix.a, _gs.textMatrix.b, _gs.textMatrix.c,
                _gs.textMatrix.d, _gs.textMatrix.e, _gs.textMatrix.f);
            fflush(tjDebug);
        }
        // ========== END DEBUG ==========

        // Effective font size hesapla (text matrix + CTM scale dahil)
        double tmScaleY = std::sqrt(_gs.textMatrix.c * _gs.textMatrix.c +
            _gs.textMatrix.d * _gs.textMatrix.d);
        double ctmScaleY = std::sqrt(_gs.ctm.c * _gs.ctm.c +
            _gs.ctm.d * _gs.ctm.d);
        double effectiveFontSize = _gs.fontSize * tmScaleY * ctmScaleY;

        // X yönü scale'leri (advance dönüşümü için)
        double tmScaleX = std::sqrt(_gs.textMatrix.a * _gs.textMatrix.a +
            _gs.textMatrix.b * _gs.textMatrix.b);
        double ctmScaleX = std::sqrt(_gs.ctm.a * _gs.ctm.a +
            _gs.ctm.b * _gs.ctm.b);
        // Painter ile tutarlı text matrix advance için X-scale bazlı denom
        double denomTJ = ctmScaleX * tmScaleX;

        // charSpacing ve wordSpacing X-scale bazlı (yatay yer değiştirme)
        double effectiveCharSpacing = _gs.charSpacing * tmScaleX * ctmScaleX;
        double effectiveWordSpacing = _gs.wordSpacing * tmScaleX * ctmScaleX;

        // advanceSizePt = X-scale bazlı (yatay advance/glyph sıkıştırma)
        double effectiveAdvanceSize = _gs.fontSize * tmScaleX * ctmScaleX;

        // Text direction angle in page space (for rotated text matrices)
        double dx_page = _gs.ctm.a * _gs.textMatrix.a + _gs.ctm.c * _gs.textMatrix.b;
        double dy_page = _gs.ctm.b * _gs.textMatrix.a + _gs.ctm.d * _gs.textMatrix.b;
        double textAngle = std::atan2(dy_page, dx_page);

        for (auto& it : arr->items)
        {
            if (auto s = std::dynamic_pointer_cast<PdfString>(it))
            {
                std::string raw = s->value;
                if (raw.empty()) continue;

                // Mevcut pozisyon
                double tx = 0.0;
                double ty = _gs.textRise;

                double ux = _gs.textMatrix.a * tx +
                    _gs.textMatrix.c * ty +
                    _gs.textMatrix.e;

                double uy = _gs.textMatrix.b * tx +
                    _gs.textMatrix.d * ty +
                    _gs.textMatrix.f;

                double x, y;
                ApplyMatrixPoint(_gs.ctm, ux, uy, x, y);

                // Çiz ve advance al
                // fontSizePt = Y-scale (render boyutu), advanceSizePt = X-scale (advance)
                double drawnAdv = _painter->drawTextFreeTypeRaw(
                    x, y, raw,
                    effectiveFontSize,
                    effectiveAdvanceSize,
                    rgbToArgb(_gs.fillColor),
                    _currentFont,
                    effectiveCharSpacing,
                    effectiveWordSpacing,
                    _gs.horizontalScale,
                    textAngle
                );

                // Painter X-scale bazlı advance döndürür.
                // X-scale denom ile bölerek text-space advance üret.
                double adv;
                if (denomTJ > 0.0001) {
                    adv = drawnAdv / denomTJ;
                } else {
                    adv = computeAdvanceFromRaw(
                        _currentFont, raw,
                        _gs.fontSize, _gs.charSpacing, _gs.wordSpacing
                    );
                    adv *= (_gs.horizontalScale / 100.0);
                }

                textAdvance(_gs, adv, 0.0);
            }
            else if (auto n = std::dynamic_pointer_cast<PdfNumber>(it))
            {
                // TJ array'deki sayılar: kerning/positioning
                // Negatif sayı: sağa ilerle, pozitif: sola geri gel
                // Birim: text space'te 1/1000 em
                double adjust = (-n->value / 1000.0) *
                    _gs.fontSize *
                    (_gs.horizontalScale / 100.0);

                textAdvance(_gs, adjust, 0.0);
            }
        }
    }


    void PdfContentParser::op_c()
    {
        double y3 = popNumber();
        double x3 = popNumber();
        double y2 = popNumber();
        double x2 = popNumber();
        double y1 = popNumber();
        double x1 = popNumber();

        // ========== DEBUG: op_c çağrıldı mı? ==========
        static FILE* curveDebug = nullptr;
        static int curveCallCount = 0;
        if (!curveDebug) {
            char tempPath[MAX_PATH];
            GetTempPathA(MAX_PATH, tempPath);
            strcat(tempPath, "curve_parse_debug.txt");
            curveDebug = fopen(tempPath, "w");
            if (curveDebug) {
                fprintf(curveDebug, "=== CURVE PARSE DEBUG ===\n");
                fprintf(curveDebug, "Log file: %s\n", tempPath);
                fflush(curveDebug);
            }
        }
        curveCallCount++;
        if (curveDebug && curveCallCount <= 100) {
            fprintf(curveDebug, "[op_c #%d] (%.2f,%.2f)->(%.2f,%.2f)->(%.2f,%.2f)->(%.2f,%.2f)\n",
                curveCallCount, _cpX, _cpY, x1, y1, x2, y2, x3, y3);
            fprintf(curveDebug, "  _currentPath.size before=%zu\n", _currentPath.size());
            fflush(curveDebug);
        }
        // ========== END DEBUG ==========

        _currentPath.emplace_back(x1, y1, x2, y2, x3, y3);

        // ========== DEBUG: Eklendi mi? ==========
        if (curveDebug && curveCallCount <= 100) {
            fprintf(curveDebug, "  _currentPath.size after=%zu\n", _currentPath.size());
            if (!_currentPath.empty()) {
                const auto& last = _currentPath.back();
                fprintf(curveDebug, "  last segment type=%d\n", (int)last.type);
            }
            fflush(curveDebug);
        }
        // ========== END DEBUG ==========

        // ⚠️ KRİTİK
        _cpX = x3;
        _cpY = y3;
    }


    void PdfContentParser::op_f_evenodd()
    {
        // ✅ FIX: Skip completely transparent fills (alpha = 0)
        if (_gs.fillAlpha <= 0.001)
        {
            _currentPath.clear();
            return;
        }

        if (_painter)
        {
            // =====================================================
            // ✅ PATTERN FILL KONTROLÜ (even-odd)
            // =====================================================
            if (!_gs.fillPatternName.empty())
            {

                // 1. Try Resolving Tiling Pattern (Type 1)
                PdfPattern pattern;
                if (resolvePattern(_gs.fillPatternName, pattern))
                {
                    if (pattern.isUncolored) {
                        pattern.baseColor = rgbToArgbWithAlpha(_gs.fillColor, _gs.fillAlpha);
                    }

                    _painter->fillPathWithPattern(
                        _currentPath,
                        pattern,
                        _gs.ctm,
                        true // evenOdd
                    );

                    _currentPath.clear();
                    return;
                }

                PdfGradient gradient;
                PdfMatrix patternMatrix;

                if (resolvePatternToGradient(_gs.fillPatternName, gradient, patternMatrix))
                {
                    // =====================================================
                    // ✅ DOĞRU MATRİS ZİNCİRİ (MuPDF/Adobe gibi):
                    // gradientCTM = PatternMatrix * CTM
                    // =====================================================
                    PdfMatrix gradientCTM = PdfMul(patternMatrix, _gs.ctm);

                    _painter->fillPathWithGradient(
                        _currentPath,
                        gradient,
                        _gs.ctm,
                        gradientCTM,
                        true  // even-odd = true
                    );

                    _currentPath.clear();
                    return;
                }
            }

            // Normal düz renk fill - alpha ile
            _painter->fillPath(
                _currentPath,
                rgbToArgbWithAlpha(_gs.fillColor, _gs.fillAlpha),
                _gs.ctm,
                true, // even-odd
                _hasClippingPath ? &_clippingPath : nullptr,
                _hasClippingPath ? &_clippingPathCTM : nullptr,
                _clippingEvenOdd
            );
        }

        _currentPath.clear();
    }

    void PdfContentParser::op_fill_stroke()
    {
        // ========== DEBUG: B operator tracking ==========
        static FILE* bDebug = nullptr;
        static int bCallCount = 0;
        if (!bDebug) {
            char tempPath[MAX_PATH];
            GetTempPathA(MAX_PATH, tempPath);
            strcat(tempPath, "b_operator_debug.txt");
            bDebug = fopen(tempPath, "w");
            if (bDebug) {
                fprintf(bDebug, "=== B OPERATOR (FILL+STROKE) DEBUG ===\n");
                fflush(bDebug);
            }
        }
        bCallCount++;

        if (bDebug && (bCallCount <= 50 || bCallCount % 100 == 0)) {
            fprintf(bDebug, "[B #%d] path.size=%zu, CTM=[%.2f %.2f %.2f %.2f %.2f %.2f]\n",
                bCallCount, _currentPath.size(),
                _gs.ctm.a, _gs.ctm.b, _gs.ctm.c, _gs.ctm.d, _gs.ctm.e, _gs.ctm.f);

            // First point if path not empty
            if (!_currentPath.empty()) {
                fprintf(bDebug, "  first pt: (%.2f, %.2f), painter=%p\n",
                    _currentPath[0].x, _currentPath[0].y, (void*)_painter);
            }
            fflush(bDebug);
        }
        // ========== END DEBUG ==========

        if (_painter)
        {
            // ✅ FIX: Only fill if alpha > 0
            bool shouldFill = (_gs.fillAlpha > 0.001);

            // Enhanced Fill Logic for Pattern Support
            bool patternFilled = false;
            if (shouldFill && !_gs.fillPatternName.empty())
            {
                // 1. Try Tiling Pattern
                PdfPattern pattern;
                if (resolvePattern(_gs.fillPatternName, pattern))
                {
                    if (pattern.isUncolored) pattern.baseColor = rgbToArgbWithAlpha(_gs.fillColor, _gs.fillAlpha);
                    _painter->fillPathWithPattern(_currentPath, pattern, _gs.ctm, false);
                    patternFilled = true;
                }
                else
                {
                    // 2. Try Gradient (Type 2)
                    PdfGradient gradient;
                    PdfMatrix patternMatrix;
                    if (resolvePatternToGradient(_gs.fillPatternName, gradient, patternMatrix))
                    {
                        PdfMatrix gradientCTM = PdfMul(patternMatrix, _gs.ctm);
                        _painter->fillPathWithGradient(_currentPath, gradient, _gs.ctm, gradientCTM, false);
                        patternFilled = true;
                    }
                }
            }

            if (shouldFill && !patternFilled)
            {
                _painter->fillPath(
                    _currentPath,
                    rgbToArgbWithAlpha(_gs.fillColor, _gs.fillAlpha),
                    _gs.ctm,
                    false
                );
            }

            _painter->strokePath(
                _currentPath,
                rgbToArgbWithAlpha(_gs.strokeColor, _gs.strokeAlpha),
                _gs.lineWidth,
                _gs.ctm,
                _gs.lineCap,
                _gs.lineJoin,
                _gs.miterLimit
            );
        }

        _currentPath.clear();
    }


    void PdfContentParser::op_fill_stroke_evenodd()
    {
        if (_painter)
        {
            // ✅ FIX: Only fill if alpha > 0
            if (_gs.fillAlpha > 0.001)
            {
                _painter->fillPath(
                    _currentPath,
                    rgbToArgbWithAlpha(_gs.fillColor, _gs.fillAlpha),
                    _gs.ctm,
                    true
                );
            }

            _painter->strokePath(
                _currentPath,
                rgbToArgbWithAlpha(_gs.strokeColor, _gs.strokeAlpha),
                _gs.lineWidth,
                _gs.ctm,
                _gs.lineCap,
                _gs.lineJoin,
                _gs.miterLimit
            );
        }

        _currentPath.clear();
    }

    void PdfContentParser::op_d()
    {
        // PDF: dashArray dashPhase d
        // Stack'te genelde: [array] [number]
        double phase = popNumber(0.0);

        // Array’i popla
        std::shared_ptr<PdfArray> arr;
        if (!_stack.empty())
        {
            arr = std::dynamic_pointer_cast<PdfArray>(_stack.back());
            _stack.pop_back();
        }

        // Render etmiyorsan bile burada bitirmen yeterli.
        // İstersen _gs içine dash pattern saklayabilirsin.
    }


    void PdfContentParser::op_w()
    {
        _gs.lineWidth = popNumber(1.0);
    }

    void PdfContentParser::op_J()
    {
        _gs.lineCap = (int)popNumber(0);
    }

    void PdfContentParser::op_j()
    {
        _gs.lineJoin = (int)popNumber(0);
    }

    void PdfContentParser::op_M()
    {
        _gs.miterLimit = popNumber(10.0);
    }


    // =========================================================
    // Operator dispatch
    // =========================================================

    void PdfContentParser::handleOperator(const std::string& op)
    {
        // ============ PATH OPERATORS ============

        if (op == "BX")
        {
            return; // BX başlangıcını görmezden gel
        }

        if (op == "EX")
        {
            return; // EX bitişini görmezden gel
        }


        if (op == "m")
        {
            return op_m();
        }
        if (op == "l")
        {
            return op_l();
        }
        if (op == "c")
        {
            return op_c();
        }
        if (op == "v")
        {
            return op_v();
        }
        if (op == "y")
        {
            return op_y();
        }
        if (op == "h")
        {
            return op_h();
        }
        if (op == "re")
        {
            return op_re();
        }

        if (op == "f")
        {
            LogDebug("FILL: %zu segments, color=[%.2f,%.2f,%.2f]",
                _currentPath.size(), _gs.fillColor[0], _gs.fillColor[1], _gs.fillColor[2]);
            return op_f();
        }
        if (op == "f*")
        {
            return op_f_evenodd();
        }
        if (op == "S")
        {
            LogDebug("STROKE: %zu segments, lw=%.2f, color=[%.2f,%.2f,%.2f]",
                _currentPath.size(), _gs.lineWidth,
                _gs.strokeColor[0], _gs.strokeColor[1], _gs.strokeColor[2]);
            return op_S();
        }
        if (op == "s")
        {
            // s = close path + stroke (equivalent to h S)
            op_h();
            return op_S();
        }
        if (op == "B")
        {
            return op_fill_stroke();
        }
        if (op == "B*")
        {
            return op_fill_stroke_evenodd();
        }
        if (op == "b")
        {
            // b = close path + fill + stroke (equivalent to h B)
            op_h();
            return op_fill_stroke();
        }
        if (op == "b*")
        {
            // b* = close path + fill (even-odd) + stroke (equivalent to h B*)
            op_h();
            return op_fill_stroke_evenodd();
        }
        if (op == "F")
        {
            // F is equivalent to f (obsolete operator kept for compatibility)
            return op_f();
        }
        if (op == "W")
        {
            // PDF spec: W intersects current path with existing clip (cumulative)
            _clippingPath = _currentPath;
            _clippingPathCTM = _gs.ctm;
            _hasClippingPath = true;
            _clippingEvenOdd = false;
            // Push D2D clip layer for cumulative clipping (nested W operators)
            if (_painter && !_currentPath.empty()) {
                _painter->pushClipPath(_currentPath, _gs.ctm, false);
                _clipLayerCount++;
            }
            return;
        }

        // Color & Shading
        if (op == "CS") return op_CS();
        if (op == "cs") return op_cs();
        if (op == "SC") return op_SC();
        if (op == "sc") return op_sc();
        if (op == "SCN") return op_SCN();
        if (op == "scn") return op_scn();
        if (op == "G") return op_G();
        if (op == "g") return op_g();
        if (op == "RG") return op_RG();
        if (op == "rg") return op_rg();
        if (op == "K") return op_K();
        if (op == "k") return op_k();
        // sh operator handled below with full implementation

        if (op == "n")
        {
            // End path without painting (used for clipping)
            _currentPath.clear();
            return;
        }

        if (op == "W*")
        {
            // PDF spec: W* intersects current path with existing clip using even-odd rule
            _clippingPath = _currentPath;
            _clippingPathCTM = _gs.ctm;
            _hasClippingPath = true;
            _clippingEvenOdd = true;
            // Push D2D clip layer for cumulative clipping (nested W* operators)
            if (_painter && !_currentPath.empty()) {
                _painter->pushClipPath(_currentPath, _gs.ctm, true);
                _clipLayerCount++;
            }
            return;
        }

        // PdfContentParser.cpp - sh operatörü için düzeltme
 // handleOperator fonksiyonundaki if (op == "sh") bloğunu bu kodla değiştir

        if (op == "sh")
        {
            std::string shadingName = popName();

            // ========== DEBUG: sh operatörü path durumu ==========
            static FILE* shDebug = nullptr;
            static int shCallCount = 0;
            if (!shDebug) {
                char tempPath[MAX_PATH];
                GetTempPathA(MAX_PATH, tempPath);
                strcat(tempPath, "sh_debug.txt");
                shDebug = fopen(tempPath, "w");
                if (shDebug) {
                    fprintf(shDebug, "=== SH OPERATOR DEBUG ===\n");
                    fflush(shDebug);
                }
            }
            shCallCount++;

            // _clippingPath segment sayıları
            int clipCurves = 0, clipLines = 0, clipMoves = 0;
            for (const auto& seg : _clippingPath) {
                if (seg.type == PdfPathSegment::CurveTo) clipCurves++;
                else if (seg.type == PdfPathSegment::LineTo) clipLines++;
                else if (seg.type == PdfPathSegment::MoveTo) clipMoves++;
            }

            // _currentPath segment sayıları
            int curCurves = 0, curLines = 0, curMoves = 0;
            for (const auto& seg : _currentPath) {
                if (seg.type == PdfPathSegment::CurveTo) curCurves++;
                else if (seg.type == PdfPathSegment::LineTo) curLines++;
                else if (seg.type == PdfPathSegment::MoveTo) curMoves++;
            }

            if (shDebug) {
                fprintf(shDebug, "\n[sh #%d] shadingName='%s'\n", shCallCount, shadingName.c_str());
                fprintf(shDebug, "  _hasClippingPath=%d\n", _hasClippingPath ? 1 : 0);
                fprintf(shDebug, "  _clippingPath: size=%zu, moves=%d, lines=%d, CURVES=%d\n",
                    _clippingPath.size(), clipMoves, clipLines, clipCurves);
                fprintf(shDebug, "  _currentPath:  size=%zu, moves=%d, lines=%d, CURVES=%d\n",
                    _currentPath.size(), curMoves, curLines, curCurves);
                fprintf(shDebug, "  _clippingPathCTM=[%.4f %.4f %.4f %.4f %.4f %.4f]\n",
                    _clippingPathCTM.a, _clippingPathCTM.b, _clippingPathCTM.c,
                    _clippingPathCTM.d, _clippingPathCTM.e, _clippingPathCTM.f);
                fprintf(shDebug, "  _gs.ctm=[%.4f %.4f %.4f %.4f %.4f %.4f]\n",
                    _gs.ctm.a, _gs.ctm.b, _gs.ctm.c, _gs.ctm.d, _gs.ctm.e, _gs.ctm.f);
                fflush(shDebug);
            }
            // ========== END DEBUG ==========

            PdfMatrix shadingCTM = _gs.ctm;

            if (!_painter)
            {
                _currentPath.clear();
                return;
            }

            // Clipping path kontrolü
            if (!_hasClippingPath || _clippingPath.empty())
            {
                if (shDebug) {
                    fprintf(shDebug, "  -> Using _currentPath as clipping (hasClip=%d, clipEmpty=%d)\n",
                        _hasClippingPath ? 1 : 0, _clippingPath.empty() ? 1 : 0);
                    fflush(shDebug);
                }
                if (_currentPath.empty()) return;
                _clippingPath = _currentPath;
                _clippingPathCTM = _gs.ctm;
                _hasClippingPath = true;
            }

            // ===== SHADING DICTIONARY BUL =====
            std::shared_ptr<PdfDictionary> shadingDict;

            for (auto it = _resStack.rbegin(); it != _resStack.rend(); ++it)
            {
                auto res = *it;
                if (!res) continue;

                auto shDict = resolveDict(res->get("/Shading"));
                if (!shDict) continue;

                auto shObj = resolveObj(shDict->get(shadingName));
                shadingDict = std::dynamic_pointer_cast<PdfDictionary>(shObj);

                if (shadingDict) break;
            }

            if (!shadingDict)
            {
                _currentPath.clear();
                return;
            }

            // ===== SHADING TYPE =====
            auto typeObj = std::dynamic_pointer_cast<PdfNumber>(
                resolveObj(shadingDict->get("/ShadingType")));

            int shadingType = typeObj ? (int)typeObj->value : 0;

            if (shadingType != 2 && shadingType != 3)
            {
                _currentPath.clear();
                return;
            }

            // ===== COLOR SPACE =====
            auto csObj = resolveObj(shadingDict->get("/ColorSpace"));
            int numComponents = 3;
            bool isDeviceN = false;
            std::vector<std::string> deviceNNames;

            if (csObj)
            {
                if (auto csName = std::dynamic_pointer_cast<PdfName>(csObj))
                {
                    std::string cs = csName->value;
                    if (cs == "/DeviceGray" || cs == "DeviceGray")
                        numComponents = 1;
                    else if (cs == "/DeviceCMYK" || cs == "DeviceCMYK")
                        numComponents = 4;
                }
                else if (auto csArr = std::dynamic_pointer_cast<PdfArray>(csObj))
                {
                    if (!csArr->items.empty())
                    {
                        auto first = std::dynamic_pointer_cast<PdfName>(
                            resolveObj(csArr->items[0]));
                        if (first)
                        {
                            std::string csType = first->value;
                            if (csType == "/ICCBased" || csType == "ICCBased")
                            {
                                if (csArr->items.size() >= 2)
                                {
                                    auto iccStream = std::dynamic_pointer_cast<PdfStream>(
                                        resolveObj(csArr->items[1]));
                                    if (iccStream && iccStream->dict)
                                    {
                                        auto nObj = std::dynamic_pointer_cast<PdfNumber>(
                                            resolveObj(iccStream->dict->get("/N")));
                                        if (nObj)
                                            numComponents = (int)nObj->value;
                                    }
                                }
                            }
                            else if (csType == "/Separation" || csType == "Separation")
                            {
                                numComponents = 1;
                            }
                            // DeviceN: [/DeviceN names alternateSpace tintTransform]
                            else if (csType == "/DeviceN" || csType == "DeviceN")
                            {
                                isDeviceN = true;

                                // Get names array (2nd element, index 1)
                                if (csArr->items.size() >= 2)
                                {
                                    auto namesArr = std::dynamic_pointer_cast<PdfArray>(
                                        resolveObj(csArr->items[1]));
                                    if (namesArr)
                                    {
                                        for (auto& item : namesArr->items)
                                        {
                                            if (auto nameObj = std::dynamic_pointer_cast<PdfName>(
                                                resolveObj(item)))
                                            {
                                                deviceNNames.push_back(nameObj->value);
                                            }
                                        }
                                        numComponents = (int)deviceNNames.size();
                                        LogDebug("  DeviceN has %zu color names, numComponents=%d",
                                            deviceNNames.size(), numComponents);
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // ===== COORDS (6 elemanlı - radial için) =====
            auto coordsArr = std::dynamic_pointer_cast<PdfArray>(
                resolveObj(shadingDict->get("/Coords")));

            if (!coordsArr || coordsArr->items.size() < 4)
            {
                _currentPath.clear();
                return;
            }

            std::vector<double> coords(6, 0.0);
            for (size_t i = 0; i < coordsArr->items.size() && i < 6; ++i)
            {
                if (auto n = std::dynamic_pointer_cast<PdfNumber>(
                    resolveObj(coordsArr->items[i])))
                {
                    coords[i] = n->value;
                }
            }

            // ===== GRADIENT OLUŞTUR (YENİ API - LUT dahil) =====
            PdfGradient gradient;
            gradient.type = shadingType;

            if (shadingType == 2)
            {
                // Axial: x0, y0, x1, y1
                gradient.x0 = coords[0];
                gradient.y0 = coords[1];
                gradient.x1 = coords[2];
                gradient.y1 = coords[3];
            }
            else if (shadingType == 3)
            {
                // Radial: x0, y0, r0, x1, y1, r1
                gradient.x0 = coords[0];
                gradient.y0 = coords[1];
                gradient.r0 = coords[2];
                gradient.x1 = coords[3];
                gradient.y1 = coords[4];
                gradient.r1 = coords[5];
            }

            // ===== FUNCTION PARSE (LUT oluşturulacak) =====
            auto funcObj = resolveObj(shadingDict->get("/Function"));

            bool parseSuccess = false;
            if (isDeviceN && !deviceNNames.empty())
            {
                // Use DeviceN-specific parsing
                parseSuccess = PdfGradient::parseFunctionToGradientDeviceN(funcObj, _doc, gradient, deviceNNames);
            }
            else
            {
                // Standard parsing
                parseSuccess = PdfGradient::parseFunctionToGradient(funcObj, _doc, gradient, numComponents);
            }

            if (!parseSuccess)
            {
                // Fallback
                GradientStop s0, s1;
                s0.position = 0.0;
                s0.rgb[0] = s0.rgb[1] = s0.rgb[2] = 1.0;
                s1.position = 1.0;
                s1.rgb[0] = s1.rgb[1] = s1.rgb[2] = 0.0;
                gradient.stops.push_back(s0);
                gradient.stops.push_back(s1);
            }

            LogDebug("Gradient parsed: type=%d, stops=%zu, hasLUT=%d",
                gradient.type, gradient.stops.size(), gradient.hasLUT ? 1 : 0);

            // ===== RENDER =====
            // sh fills the entire clip region with the shading
            // D2D clip layers handle the clipping, so use _clippingPath as fill region
            // (it's the most recent clip path, defining the area to fill)
            _painter->fillPathWithGradient(
                _clippingPath,
                gradient,
                _clippingPathCTM,
                shadingCTM,
                false
            );

            _currentPath.clear();
            return;
        }

        // ============ GRAPHICS STATE ============
        if (op == "q")  return op_q();
        if (op == "Q")  return op_Q();
        if (op == "cm") return op_cm();
        if (op == "w")  return op_w();
        if (op == "J")  return op_J();
        if (op == "j")  return op_j();
        if (op == "M")  return op_M();
        if (op == "d")  return op_d();

        // ExtGState operator: /GS0 gs
        if (op == "gs")
        {
            std::string gsName = popName();

            // Find ExtGState in resources
            for (auto it = _resStack.rbegin(); it != _resStack.rend(); ++it)
            {
                auto res = *it;
                if (!res) continue;

                auto extGStateDict = resolveDict(res->get("/ExtGState"));
                if (!extGStateDict) continue;

                auto gsObj = resolveDict(extGStateDict->get(gsName));
                if (!gsObj) continue;

                // Parse ExtGState parameters
                // CA - stroke alpha
                if (auto caStroke = std::dynamic_pointer_cast<PdfNumber>(resolveObj(gsObj->get("/CA"))))
                {
                    _gs.strokeAlpha = std::clamp(caStroke->value, 0.0, 1.0);
                }

                // ca - fill alpha
                if (auto caFill = std::dynamic_pointer_cast<PdfNumber>(resolveObj(gsObj->get("/ca"))))
                {
                    _gs.fillAlpha = std::clamp(caFill->value, 0.0, 1.0);
                }

                // BM - blend mode (log only for now)
                if (auto bmName = std::dynamic_pointer_cast<PdfName>(resolveObj(gsObj->get("/BM"))))
                {
                    std::string bm = bmName->value;
                    // Store for future use
                    _gs.blendMode = bm;
                }

                // LW - line width
                if (auto lwNum = std::dynamic_pointer_cast<PdfNumber>(resolveObj(gsObj->get("/LW"))))
                {
                    _gs.lineWidth = lwNum->value;
                }

                // LC - line cap
                if (auto lcNum = std::dynamic_pointer_cast<PdfNumber>(resolveObj(gsObj->get("/LC"))))
                {
                    _gs.lineCap = (int)lcNum->value;
                }

                // LJ - line join
                if (auto ljNum = std::dynamic_pointer_cast<PdfNumber>(resolveObj(gsObj->get("/LJ"))))
                {
                    _gs.lineJoin = (int)ljNum->value;
                }

                // ML - miter limit
                if (auto mlNum = std::dynamic_pointer_cast<PdfNumber>(resolveObj(gsObj->get("/ML"))))
                {
                    _gs.miterLimit = mlNum->value;
                }

                break;
            }
            return;
        }

        // ============ COLOR OPERATORS ============
        if (op == "g")
        {
            double gray = popNumber(0.0);
            gray = std::clamp(gray, 0.0, 1.0);
            _gs.fillColor[0] = gray;
            _gs.fillColor[1] = gray;
            _gs.fillColor[2] = gray;
            return;
        }
        if (op == "G")
        {
            double gray = popNumber(0.0);
            gray = std::clamp(gray, 0.0, 1.0);
            _gs.strokeColor[0] = gray;
            _gs.strokeColor[1] = gray;
            _gs.strokeColor[2] = gray;
            return;
        }
        if (op == "rg")
        {
            double b = popNumber();
            double g = popNumber();
            double r = popNumber();
            _gs.fillColor[0] = r;
            _gs.fillColor[1] = g;
            _gs.fillColor[2] = b;
            return;
        }
        if (op == "RG")
        {
            double b = popNumber();
            double g = popNumber();
            double r = popNumber();
            _gs.strokeColor[0] = r;
            _gs.strokeColor[1] = g;
            _gs.strokeColor[2] = b;
            return;
        }
        if (op == "k")
        {
            double k = popNumber();
            double y = popNumber();
            double m = popNumber();
            double c = popNumber();
            cmykToRgb(c, m, y, k, _gs.fillColor);
            return;
        }
        if (op == "K")
        {
            double k = popNumber();
            double y = popNumber();
            double m = popNumber();
            double c = popNumber();
            cmykToRgb(c, m, y, k, _gs.strokeColor);
            return;
        }
        if (op == "cs" || op == "CS")
        {
            std::string csName = popName();
            bool isFill = (op == "cs");


            if (isFill) {
                _gs.fillColorSpace = csName;
            }
            else {
                _gs.strokeColorSpace = csName;
            }
            return;
        }

        // SC, SCN, sc, scn (color with colorspace)
        bool isStroke = (op == "SC" || op == "SCN");
        bool isFill = (op == "sc" || op == "scn");
        if (isStroke || isFill)
        {
            // ✅ Pattern kontrolü - stack'te Name varsa Pattern olabilir
            if (!_stack.empty()) {
                auto nameObj = std::dynamic_pointer_cast<PdfName>(_stack.back());
                if (nameObj) {
                    std::string patternName = nameObj->value;
                    _stack.pop_back();


                    if (isFill) {
                        _gs.fillPatternName = patternName;
                    }
                    else {
                        _gs.strokePatternName = patternName;
                    }
                    return;
                }
            }

            int numArgs = 0;
            for (int i = (int)_stack.size() - 1; i >= 0; --i)
            {
                if (std::dynamic_pointer_cast<PdfNumber>(_stack[i]))
                    numArgs++;
                else
                    break;
            }

            if (numArgs == 1) // Gray
            {
                double val = popNumber(0.0);
                val = std::clamp(val, 0.0, 1.0);
                if (isFill) {
                    _gs.fillColor[0] = val; _gs.fillColor[1] = val; _gs.fillColor[2] = val;
                    _gs.fillPatternName.clear();  // Pattern temizle
                }
                else {
                    _gs.strokeColor[0] = val; _gs.strokeColor[1] = val; _gs.strokeColor[2] = val;
                    _gs.strokePatternName.clear();
                }
            }
            else if (numArgs == 3) // RGB
            {
                double b = popNumber(0.0);
                double g = popNumber(0.0);
                double r = popNumber(0.0);

                // DEBUG: Log gold color detection
                if (r > 0.7 && g > 0.5 && b < 0.2) {
                }

                if (isFill) {
                    _gs.fillColor[0] = r; _gs.fillColor[1] = g; _gs.fillColor[2] = b;
                    _gs.fillPatternName.clear();
                }
                else {
                    _gs.strokeColor[0] = r; _gs.strokeColor[1] = g; _gs.strokeColor[2] = b;
                    _gs.strokePatternName.clear();
                }
            }
            else if (numArgs == 4) // CMYK
            {
                double k = popNumber(0.0);
                double y = popNumber(0.0);
                double m = popNumber(0.0);
                double c = popNumber(0.0);
                double rgb[3];
                cmykToRgb(c, m, y, k, rgb);
                if (isFill) {
                    _gs.fillColor[0] = rgb[0]; _gs.fillColor[1] = rgb[1]; _gs.fillColor[2] = rgb[2];
                    _gs.fillPatternName.clear();
                }
                else {
                    _gs.strokeColor[0] = rgb[0]; _gs.strokeColor[1] = rgb[1]; _gs.strokeColor[2] = rgb[2];
                    _gs.strokePatternName.clear();
                }
            }
            return;
        }

        // ============ SHADING (GRADIENT) ============


        // ============ TEXT OPERATORS ============
        if (op == "BT") return op_BT();
        if (op == "ET") return op_ET();
        if (op == "Tf") return op_Tf();
        if (op == "TL") return op_TL();
        if (op == "Tm") return op_Tm();
        if (op == "Td") return op_Td();
        if (op == "TD") {
            // TD is same as Td but also sets TL = -ty
            double ty = popNumber();
            double tx = popNumber();
            _gs.leading = -ty;
            _gs.textLineMatrix.e += tx * _gs.textLineMatrix.a + ty * _gs.textLineMatrix.c;
            _gs.textLineMatrix.f += tx * _gs.textLineMatrix.b + ty * _gs.textLineMatrix.d;
            _gs.textMatrix = _gs.textLineMatrix;
            _gs.textPosX = _gs.textMatrix.e;
            _gs.textPosY = _gs.textMatrix.f;
            return;
        }
        if (op == "T*") return op_Tstar();
        if (op == "Tj") return op_Tj();
        if (op == "TJ") return op_TJ();
        if (op == "'") {
            // ' (single quote): Move to next line and show text
            // Equivalent to: T*  string Tj
            double tx = 0.0;
            double ty = -_gs.leading;
            _gs.textLineMatrix.e += tx * _gs.textLineMatrix.a + ty * _gs.textLineMatrix.c;
            _gs.textLineMatrix.f += tx * _gs.textLineMatrix.b + ty * _gs.textLineMatrix.d;
            _gs.textMatrix = _gs.textLineMatrix;
            _gs.textPosX = _gs.textMatrix.e;
            _gs.textPosY = _gs.textMatrix.f;
            op_Tj();
            return;
        }
        if (op == "\"") {
            // " (double quote): Set word/char spacing, move to next line, show text
            // Equivalent to: aw Tw  ac Tc  string '
            std::string raw = popString();
            double ac = popNumber();
            double aw = popNumber();
            _gs.wordSpacing = aw;
            _gs.charSpacing = ac;
            double tx = 0.0;
            double ty = -_gs.leading;
            _gs.textLineMatrix.e += tx * _gs.textLineMatrix.a + ty * _gs.textLineMatrix.c;
            _gs.textLineMatrix.f += tx * _gs.textLineMatrix.b + ty * _gs.textLineMatrix.d;
            _gs.textMatrix = _gs.textLineMatrix;
            _gs.textPosX = _gs.textMatrix.e;
            _gs.textPosY = _gs.textMatrix.f;
            _stack.push_back(std::make_shared<PdfString>(raw));
            op_Tj();
            return;
        }
        if (op == "Tc") { _gs.charSpacing = popNumber(0.0); return; }
        if (op == "Tw") { _gs.wordSpacing = popNumber(0.0); return; }
        if (op == "Tz") { _gs.horizontalScale = popNumber(100.0); return; }
        if (op == "Ts") { _gs.textRise = popNumber(0.0); return; }

        // ============ XOBJECT ============
        if (op == "Do")
        {
            if (_stack.empty())
            {
                return;
            }

            auto nameObj = std::dynamic_pointer_cast<PdfName>(_stack.back());
            _stack.pop_back();

            if (!nameObj)
            {
                return;
            }

            renderXObjectDo(nameObj->value);
            return;
        }

        // ============ UNSUPPORTED ============
        static std::map<std::string, int> unsupported;
        if (unsupported[op]++ < 2)
        {
        }
    }

    void PdfContentParser::renderXObjectDo(const std::string& xNameRaw)
    {

        if (!_doc || !_painter)
        {
            return;
        }

        static thread_local int recursionDepth = 0;
        const int MAX_RECURSION = 20;

        if (recursionDepth >= MAX_RECURSION)
        {
            return;
        }

        recursionDepth++;

        std::string xName = xNameRaw;
        if (!xName.empty() && xName[0] == '/')
            xName.erase(0, 1);


        std::shared_ptr<PdfStream> xoStream;

        int resIdx = 0;
        for (auto it = _resStack.rbegin(); it != _resStack.rend(); ++it, ++resIdx)
        {
            auto res = *it;
            if (!res) {
                continue;
            }


            // Try both /XObject and XObject key formats
            auto xoObj = res->get("/XObject");
            if (!xoObj) xoObj = res->get("XObject");

            if (!xoObj) {
                // Debug: List all keys in this resource dict
                std::string keys;
                for (auto& kv : res->entries) {
                    keys += kv.first + " ";
                }
                continue;
            }

            auto xoDict = resolveDict(xoObj);
            if (!xoDict) {
                continue;
            }


            auto itX = xoDict->entries.find("/" + xName);
            if (itX == xoDict->entries.end()) {
                // Try without leading /
                itX = xoDict->entries.find(xName);
            }
            if (itX == xoDict->entries.end()) {
                // Debug: List XObject keys
                std::string xkeys;
                for (auto& kv : xoDict->entries) {
                    xkeys += kv.first + " ";
                }
                continue;
            }

            xoStream = std::dynamic_pointer_cast<PdfStream>(resolveObj(itX->second));
            if (xoStream)
            {
                break;
            }
            else {
            }
        }

        if (!xoStream || !xoStream->dict)
        {
            recursionDepth--;
            return;
        }

        auto subtype = std::dynamic_pointer_cast<PdfName>(
            resolveObj(xoStream->dict->get("/Subtype")));

        if (!subtype)
        {
            recursionDepth--;
            return;
        }


        // IMAGE XOBJECT
        if (subtype->value == "/Image" || subtype->value == "Image")
        {


            std::vector<uint8_t> argb;
            int iw = 0, ih = 0;
            if (_doc->decodeImageXObject(xoStream, argb, iw, ih))
            {

                // ========== DEBUG: İlk birkaç image'ı BMP olarak kaydet ==========
                static int savedImageCount = 0;
                if (savedImageCount < 5 && iw > 10 && ih > 10) {
                    char bmpPath[MAX_PATH];
                    GetTempPathA(MAX_PATH, bmpPath);
                    char filename[32];
                    sprintf(filename, "debug_image_%d.bmp", savedImageCount);
                    strcat(bmpPath, filename);

                    FILE* bmpFile = fopen(bmpPath, "wb");
                    if (bmpFile) {
                        // BMP Header
                        int rowSize = ((iw * 3 + 3) / 4) * 4;
                        int dataSize = rowSize * ih;
                        int fileSize = 54 + dataSize;

                        uint8_t header[54] = { 0 };
                        header[0] = 'B'; header[1] = 'M';
                        *(int*)&header[2] = fileSize;
                        *(int*)&header[10] = 54;
                        *(int*)&header[14] = 40;
                        *(int*)&header[18] = iw;
                        *(int*)&header[22] = ih;
                        *(short*)&header[26] = 1;
                        *(short*)&header[28] = 24;
                        *(int*)&header[34] = dataSize;

                        fwrite(header, 1, 54, bmpFile);

                        // BMP pixels (bottom-up, BGR)
                        std::vector<uint8_t> row(rowSize, 0);
                        for (int y = ih - 1; y >= 0; y--) {
                            for (int x = 0; x < iw; x++) {
                                int srcIdx = (y * iw + x) * 4;
                                row[x * 3 + 0] = argb[srcIdx + 2]; // B
                                row[x * 3 + 1] = argb[srcIdx + 1]; // G
                                row[x * 3 + 2] = argb[srcIdx + 0]; // R
                            }
                            fwrite(row.data(), 1, rowSize, bmpFile);
                        }
                        fclose(bmpFile);
                    }
                    savedImageCount++;
                }
                // ========== END DEBUG ==========

                if (iw == 1 && ih == 1)
                {
                    recursionDepth--;
                    return;
                }

                // =====================================================
                // ✅ MuPDF YAKLAŞIMI (pdf-op-run.c satır 823)
                // "PDF has images bottom-up, so flip them right side up here"
                // image_ctm = fz_pre_scale(fz_pre_translate(gstate->ctm, 0, 1), 1, -1)
                //
                // Matematiksel olarak: image_ctm = S(1,-1) * T(0,1) * ctm
                // S(1,-1) * T(0,1) = [1 0 0 -1 0 1]
                //
                // Bu matris CTM ile çarpıldığında:
                // - d işareti tersine döner (negatif -> pozitif)
                // - f değeri ayarlanır (origin düzeltmesi)
                // =====================================================

                // Flip matrisi: [1 0 0 -1 0 1]
                PdfMatrix flipMatrix;
                flipMatrix.a = 1;  flipMatrix.b = 0;
                flipMatrix.c = 0;  flipMatrix.d = -1;
                flipMatrix.e = 0;  flipMatrix.f = 1;

                // image_ctm = flipMatrix * ctm
                PdfMatrix image_ctm = PdfMul(flipMatrix, _gs.ctm);

                // =====================================================
                // AUTO-SCALE: CTM'de scale yoksa image boyutlarini kullan
                // PDF'de normalde "width 0 0 height tx ty cm" ile scale
                // set edilir. Ama bazi PDF'lerde bu eksik olabiliyor.
                // Adobe bu durumda image'in intrinsic boyutlarini kullanir.
                // =====================================================
                double effectiveScaleX = std::sqrt(image_ctm.a * image_ctm.a + image_ctm.b * image_ctm.b);
                double effectiveScaleY = std::sqrt(image_ctm.c * image_ctm.c + image_ctm.d * image_ctm.d);

                // Eger scale cok kucukse (< 2 point), image boyutlarina gore scale ekle
                if (effectiveScaleX < 2.0 && effectiveScaleY < 2.0 && iw > 1 && ih > 1) {
                    LogDebug("Image CTM has no scale (%.2f x %.2f), adding image dimensions %dx%d",
                        effectiveScaleX, effectiveScaleY, iw, ih);

                    // Scale matrix: image boyutlari
                    PdfMatrix scaleMatrix;
                    scaleMatrix.a = (double)iw;  scaleMatrix.b = 0;
                    scaleMatrix.c = 0;           scaleMatrix.d = (double)ih;
                    scaleMatrix.e = 0;           scaleMatrix.f = 0;

                    // Yeni CTM: scale * flip * original_ctm
                    image_ctm = PdfMul(scaleMatrix, PdfMul(flipMatrix, _gs.ctm));
                }

                LogDebug("Image CTM: [%.2f %.2f %.2f %.2f %.2f %.2f] -> [%.2f %.2f %.2f %.2f %.2f %.2f]",
                    _gs.ctm.a, _gs.ctm.b, _gs.ctm.c, _gs.ctm.d, _gs.ctm.e, _gs.ctm.f,
                    image_ctm.a, image_ctm.b, image_ctm.c, image_ctm.d, image_ctm.e, image_ctm.f);

                // ✅ Clipping path varsa drawImageClipped kullan
                // ========== DEBUG: Image clipping durumu ==========
                static FILE* imgClipDebug = nullptr;
                static int imgClipCount = 0;
                if (!imgClipDebug) {
                    char tempPath[MAX_PATH];
                    GetTempPathA(MAX_PATH, tempPath);
                    strcat(tempPath, "image_clip_debug.txt");
                    imgClipDebug = fopen(tempPath, "w");
                    if (imgClipDebug) {
                        fprintf(imgClipDebug, "=== IMAGE CLIPPING DEBUG ===\n");
                        fflush(imgClipDebug);
                    }
                }
                imgClipCount++;

                if (imgClipDebug) {
                    fprintf(imgClipDebug, "\n[Image #%d] size=%dx%d\n", imgClipCount, iw, ih);
                    fprintf(imgClipDebug, "  _hasClippingPath=%d\n", _hasClippingPath ? 1 : 0);
                    fprintf(imgClipDebug, "  _clippingPath.size=%zu\n", _clippingPath.size());

                    if (!_clippingPath.empty()) {
                        int curves = 0, lines = 0, moves = 0;
                        for (const auto& seg : _clippingPath) {
                            if (seg.type == PdfPathSegment::CurveTo) curves++;
                            else if (seg.type == PdfPathSegment::LineTo) lines++;
                            else if (seg.type == PdfPathSegment::MoveTo) moves++;
                        }
                        fprintf(imgClipDebug, "  clippingPath: moves=%d, lines=%d, CURVES=%d\n", moves, lines, curves);
                    }
                    fprintf(imgClipDebug, "  image_ctm=[%.2f %.2f %.2f %.2f %.2f %.2f]\n",
                        image_ctm.a, image_ctm.b, image_ctm.c, image_ctm.d, image_ctm.e, image_ctm.f);
                    fprintf(imgClipDebug, "  _clippingPathCTM=[%.2f %.2f %.2f %.2f %.2f %.2f]\n",
                        _clippingPathCTM.a, _clippingPathCTM.b, _clippingPathCTM.c,
                        _clippingPathCTM.d, _clippingPathCTM.e, _clippingPathCTM.f);
                    fflush(imgClipDebug);
                }
                // ========== END DEBUG ==========

                // Apply clipping if needed
                if (_hasClippingPath && !_clippingPath.empty()) {
                    // Check if clip path is a simple rect
                    bool isRect = true;
                    int lineCount = 0, moveCount = 0;
                    for (const auto& seg : _clippingPath) {
                        if (seg.type == PdfPathSegment::CurveTo) { isRect = false; break; }
                        if (seg.type == PdfPathSegment::LineTo) lineCount++;
                        if (seg.type == PdfPathSegment::MoveTo) moveCount++;
                    }
                    if (moveCount != 1 || lineCount < 3 || lineCount > 4) isRect = false;

                    if (isRect && _clippingPath.size() <= 6) {
                        // Simple rect clip - use drawImageWithClipRect
                        double minX = 1e30, minY = 1e30, maxX = -1e30, maxY = -1e30;
                        for (const auto& seg : _clippingPath) {
                            double tx = _clippingPathCTM.a * seg.x + _clippingPathCTM.c * seg.y + _clippingPathCTM.e;
                            double ty = _clippingPathCTM.b * seg.x + _clippingPathCTM.d * seg.y + _clippingPathCTM.f;
                            double sx = tx * _painter->scaleX();
                            double sy = (double)_painter->height() - ty * _painter->scaleY();
                            if (sx < minX) minX = sx;
                            if (sx > maxX) maxX = sx;
                            if (sy < minY) minY = sy;
                            if (sy > maxY) maxY = sy;
                        }
                        _painter->drawImageWithClipRect(argb, iw, ih, image_ctm,
                            (int)minX, (int)minY, (int)maxX, (int)maxY);
                    } else {
                        // Complex clip path - use drawImageClipped
                        _painter->drawImageClipped(argb, iw, ih, image_ctm,
                            _clippingPath, _clippingPathCTM);
                    }
                } else {
                    _painter->drawImage(argb, iw, ih, image_ctm);
                }
            }
            else
            {
            }
            recursionDepth--;
            return;
        }

        // FORM XOBJECT
        if (subtype->value == "/Form" || subtype->value == "Form")
        {

            std::vector<uint8_t> decoded;
            if (!_doc->decodeStream(xoStream, decoded))
            {
                recursionDepth--;
                return;
            }


            // Form Matrix
            PdfMatrix formM;
            auto mObj = xoStream->dict->get("/Matrix");
            if (mObj)
            {
                formM = readMatrix6(mObj);
                LogDebug("Form has Matrix: [%.2f %.2f %.2f %.2f %.2f %.2f]",
                    formM.a, formM.b, formM.c, formM.d, formM.e, formM.f);
            }
            else
            {
                formM = PdfMatrix(); // identity
            }

            // Resources
            std::vector<std::shared_ptr<PdfDictionary>> childResStack = _resStack;
            auto rObj = xoStream->dict->get("/Resources");
            auto formRes = resolveDict(rObj);
            if (formRes)
            {
                childResStack.push_back(formRes);
                childResStack.push_back(formRes);

                // Form XObject fontlarını yükle (encoding, codeToGid dahil)
                if (_fonts && _doc)
                    _doc->loadFontsFromResourceDict(formRes, *_fonts);
            }
            else
            {
            }
            PdfGraphicsState childGs = _gs;
            // ✅ FIX: MuPDF ile aynı sıra - formM × ctm (formM solda!)
            // MuPDF: gstate->ctm = fz_concat(transform, gstate->ctm);
            childGs.ctm = PdfMul(formM, _gs.ctm);


            PdfContentParser child(
                decoded,
                _painter,
                _doc,
                _pageIndex,
                _fonts,
                childGs,
                childResStack
            );

            // Pass inherited clipping state to child form
            if (_hasClippingPath && !_clippingPath.empty()) {
                child.setInheritedClipping(_clippingPath, _clippingPathCTM, _clippingEvenOdd);
            }
            child.parse();

            recursionDepth--;
            return;
        }

        recursionDepth--;
    }


    std::shared_ptr<PdfObject> PdfContentParser::resolveObj(const std::shared_ptr<PdfObject>& o) const
    {
        if (!_doc) return o;
        std::set<int> v;
        return _doc->resolve(o, v);
    }

    std::shared_ptr<PdfDictionary> PdfContentParser::resolveDict(const std::shared_ptr<PdfObject>& o) const
    {
        auto ro = resolveObj(o);
        return std::dynamic_pointer_cast<PdfDictionary>(ro);
    }

    PdfMatrix PdfContentParser::readMatrix6(const std::shared_ptr<PdfObject>& obj) const
    {
        PdfMatrix m;
        auto arr = std::dynamic_pointer_cast<PdfArray>(resolveObj(obj));
        if (!arr || arr->items.size() < 6) return m;

        auto n0 = std::dynamic_pointer_cast<PdfNumber>(resolveObj(arr->items[0]));
        auto n1 = std::dynamic_pointer_cast<PdfNumber>(resolveObj(arr->items[1]));
        auto n2 = std::dynamic_pointer_cast<PdfNumber>(resolveObj(arr->items[2]));
        auto n3 = std::dynamic_pointer_cast<PdfNumber>(resolveObj(arr->items[3]));
        auto n4 = std::dynamic_pointer_cast<PdfNumber>(resolveObj(arr->items[4]));
        auto n5 = std::dynamic_pointer_cast<PdfNumber>(resolveObj(arr->items[5]));

        if (!n0 || !n1 || !n2 || !n3 || !n4 || !n5) return m;

        m.a = n0->value; m.b = n1->value;
        m.c = n2->value; m.d = n3->value;
        m.e = n4->value; m.f = n5->value;
        return m;
    }


    void PdfContentParser::op_CS() { _currentStrokeCS = popName(); _stack.clear(); }
    void PdfContentParser::op_cs() { _currentFillCS = popName(); _stack.clear(); }

    // Resolve a named color space from resources and determine its type
    // Returns: 0=unknown, 1=DeviceGray-like, 2=DeviceRGB-like, 3=DeviceCMYK-like, 4=Separation/DeviceN-to-CMYK, 5=Separation/DeviceN-to-Gray
    int PdfContentParser::resolveColorSpaceType(const std::string& csName)
    {
        // Standard device color spaces
        if (csName == "/DeviceGray" || csName == "DeviceGray") return 1;
        if (csName == "/DeviceRGB" || csName == "DeviceRGB") return 2;
        if (csName == "/DeviceCMYK" || csName == "DeviceCMYK") return 3;

        // Look up in resources
        auto res = currentResources();
        if (!res) {
            return 0;
        }

        auto csDict = resolveDict(res);
        if (!csDict) return 0;

        // Dictionary keys include '/' prefix
        auto colorSpaces = csDict->get("/ColorSpace");
        if (!colorSpaces) colorSpaces = csDict->get("ColorSpace");
        if (!colorSpaces) {
            return 0;
        }
        auto csResolved = resolveObj(colorSpaces);
        if (!csResolved || csResolved->type() != PdfObjectType::Dictionary) return 0;

        auto csDictObj = std::dynamic_pointer_cast<PdfDictionary>(csResolved);
        if (!csDictObj) return 0;

        // Ensure lookup name has '/' prefix (dictionary keys have '/' prefix)
        std::string lookupName = csName;
        if (!lookupName.empty() && lookupName[0] != '/') lookupName = "/" + lookupName;

        auto csEntry = csDictObj->get(lookupName);
        if (!csEntry) {
            // Try without '/' prefix as fallback
            std::string noSlash = csName;
            if (!noSlash.empty() && noSlash[0] == '/') noSlash = noSlash.substr(1);
            csEntry = csDictObj->get(noSlash);
        }
        if (!csEntry) {
            return 0;
        }
        auto csArray = resolveObj(csEntry);
        if (!csArray || csArray->type() != PdfObjectType::Array) return 0;

        auto arr = std::dynamic_pointer_cast<PdfArray>(csArray);
        if (!arr || arr->items.empty()) return 0;

        auto typeObj = resolveObj(arr->items[0]);
        if (!typeObj) return 0;

        std::string csTypeName;
        if (typeObj->type() == PdfObjectType::Name) {
            csTypeName = static_cast<PdfName*>(typeObj.get())->value;
            // Strip '/' prefix for comparison
            if (!csTypeName.empty() && csTypeName[0] == '/') csTypeName = csTypeName.substr(1);
        }

        LogDebug("resolveColorSpaceType(%s): csTypeName='%s', items=%zu",
                 csName.c_str(), csTypeName.c_str(), arr->items.size());

        // Separation or DeviceN with alternate space
        if (csTypeName == "Separation" || csTypeName == "DeviceN") {
            // arr[0] = type, arr[1] = colorant names, arr[2] = alternate CS, arr[3] = tint transform
            if (arr->items.size() >= 3) {
                auto altCS = resolveObj(arr->items[2]);
                if (altCS && altCS->type() == PdfObjectType::Name) {
                    std::string altName = static_cast<PdfName*>(altCS.get())->value;
                    // Strip '/' prefix for comparison
                    if (!altName.empty() && altName[0] == '/') altName = altName.substr(1);
                    if (altName == "DeviceCMYK") return 4; // Separation/DeviceN -> CMYK
                    if (altName == "DeviceGray") return 5; // Separation/DeviceN -> Gray
                }
            }
            // Default: assume CMYK alternate (most common)
            return 4;
        }

        // ICCBased - check number of components
        if (csTypeName == "ICCBased") {
            if (arr->items.size() >= 2) {
                auto iccStream = resolveObj(arr->items[1]);
                // ICCBased profile is a stream - check both Stream and Dictionary types
                std::shared_ptr<PdfDictionary> iccDict;
                if (iccStream && iccStream->type() == PdfObjectType::Stream) {
                    auto stm = std::dynamic_pointer_cast<PdfStream>(iccStream);
                    if (stm) iccDict = stm->dict;
                } else if (iccStream && iccStream->type() == PdfObjectType::Dictionary) {
                    iccDict = std::dynamic_pointer_cast<PdfDictionary>(iccStream);
                }
                if (iccDict) {
                    auto nObj = iccDict->get("/N");
                    if (!nObj) nObj = iccDict->get("N");
                    if (nObj && nObj->type() == PdfObjectType::Number) {
                        int n = (int)static_cast<PdfNumber*>(nObj.get())->value;
                        if (n == 1) return 1; // Gray
                        if (n == 3) return 2; // RGB
                        if (n == 4) return 3; // CMYK
                    }
                }
            }
        }

        return 0; // Unknown
    }

    // Convert tint value(s) to RGB based on color space type
    void PdfContentParser::applyColorFromCS(const std::string& csName, double out[3])
    {
        int csType = resolveColorSpaceType(csName);

        if (csType == 4) {
            // Separation/DeviceN -> CMYK
            // For single-component Separation (like /Black), tint t -> CMYK(0,0,0,t)
            // This is a simplification - real impl should eval tint transform function
            if (_stack.size() >= 4) {
                double k = popNumber(); double y = popNumber(); double m = popNumber(); double c = popNumber();
                cmykToRgb(c, m, y, k, out);
            } else if (_stack.size() >= 1) {
                // Single tint value: assume it maps to K channel (most common for /Black Separation)
                double t = popNumber();
                // tint t -> CMYK(0,0,0,t) -> RGB(1-t, 1-t, 1-t)
                cmykToRgb(0, 0, 0, t, out);
            }
        }
        else if (csType == 5) {
            // Separation -> DeviceGray (tint transform inverts: output = 1 - t)
            if (_stack.size() >= 1) {
                double t = popNumber();
                double gray = 1.0 - t; // Separation/All to Gray: tint 1=black, 0=white
                out[0] = gray; out[1] = gray; out[2] = gray;
            }
        }
        else if (csType == 3) {
            // DeviceCMYK
            if (_stack.size() >= 4) {
                double k = popNumber(); double y = popNumber(); double m = popNumber(); double c = popNumber();
                cmykToRgb(c, m, y, k, out);
            }
        }
        else if (csType == 2) {
            // DeviceRGB
            if (_stack.size() >= 3) {
                double b = popNumber(); double g = popNumber(); double r = popNumber();
                out[0] = r; out[1] = g; out[2] = b;
            }
        }
        else {
            // DeviceGray or unknown - default to gray
            if (_stack.size() >= 3) {
                double b = popNumber(); double g = popNumber(); double r = popNumber();
                out[0] = r; out[1] = g; out[2] = b;
            }
            else if (_stack.size() >= 1) {
                double g = popNumber();
                out[0] = g; out[1] = g; out[2] = g;
            }
        }
    }

    void PdfContentParser::op_SC()
    {
        // Stroke Color
        if (_currentStrokeCS == "/Pattern" || _currentStrokeCS == "Pattern") {
            std::string name = popName();
            // Stroke Pattern Name (not supported in GS yet, maybe ignored)

            // Uncolored Pattern Color Extraction
            if (!_stack.empty()) {
                applyColorFromCS(_currentStrokeCS, _gs.strokeColor);
            }
        }
        else {
            applyColorFromCS(_currentStrokeCS, _gs.strokeColor);
        }
        _stack.clear();
    }

    void PdfContentParser::op_sc()
    {
        // Fill Color
        if (_currentFillCS == "/Pattern" || _currentFillCS == "Pattern") {
            // Pattern
            std::string name = popName(); // Last arg is pattern name
            _gs.fillPatternName = name;

            // Uncolored Pattern Color Extraction
            if (!_stack.empty()) {
                applyColorFromCS(_currentFillCS, _gs.fillColor);
            }
        }
        else {
            _gs.fillPatternName.clear(); // Switch to solid
            applyColorFromCS(_currentFillCS, _gs.fillColor);
        }
        _stack.clear();
    }

    void PdfContentParser::op_SCN() { op_SC(); } // Same for now
    void PdfContentParser::op_scn() { op_sc(); } // Same for now

    void PdfContentParser::op_G() { double g = popNumber(); _gs.strokeColor[0] = g; _gs.strokeColor[1] = g; _gs.strokeColor[2] = g; }
    void PdfContentParser::op_g() { double g = popNumber(); _gs.fillColor[0] = g; _gs.fillColor[1] = g; _gs.fillColor[2] = g; _gs.fillPatternName.clear(); }
    void PdfContentParser::op_RG() { double b = popNumber(); double g = popNumber(); double r = popNumber(); _gs.strokeColor[0] = r; _gs.strokeColor[1] = g; _gs.strokeColor[2] = b; }
    void PdfContentParser::op_rg() { double b = popNumber(); double g = popNumber(); double r = popNumber(); _gs.fillColor[0] = r; _gs.fillColor[1] = g; _gs.fillColor[2] = b; _gs.fillPatternName.clear(); }
    void PdfContentParser::op_K() {
        double k = popNumber(); double y = popNumber(); double m = popNumber(); double c = popNumber();
        cmykToRgb(c, m, y, k, _gs.strokeColor);
    }
    void PdfContentParser::op_k() {
        double k = popNumber(); double y = popNumber(); double m = popNumber(); double c = popNumber();
        cmykToRgb(c, m, y, k, _gs.fillColor);
        _gs.fillPatternName.clear();
    }

    void PdfContentParser::op_sh()
    {
        std::string name = popName();
        // TODO: Implement direct shading.
        // For now, at least we log it.
        // If the right border is drawn with 'sh', we need to implement this!
        // This requires parsing the Shading Resource and rendering it.

        // If it's a mesh shading, it's complex.
        // If it's axial/radial, we might reuse fillPathWithGradient but with a full-page rect?
    }

} // namespace pdf
