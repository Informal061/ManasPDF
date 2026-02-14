#include "pch.h"

#ifdef _MSC_VER
#pragma warning(disable: 4996)
#endif

#include "PdfDocument.h"
#include "IPdfPainter.h"
#include "PdfPainter.h"
#include "PdfPainterGPU.h"
#include "PdfContentParser.h"
#include "PdfEngine.h"
#include "PdfDebug.h"
#include "FontCache.h"
#include "zlib.h"
#include "PdfFilters.h"
#include <algorithm>
#include <map>
#include <cctype>
#include <cmath>
#include <functional>
#include <sstream> 
#include <fstream>
#include <iterator>
#include <mutex>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_ADVANCES_H


namespace pdf
{
    static FT_Library g_ftLib = nullptr;

    // ============================================
    // SYSTEM FONT RESOLVER
    // Maps PDF base font names to Windows system font paths
    // Handles: Standard 14 PDF fonts, Times, Arial, Courier, etc.
    // ============================================
    static std::string resolveSystemFontPath(const std::string& baseFont)
    {
        std::string bn = baseFont;
        if (!bn.empty() && bn[0] == '/') bn = bn.substr(1);

        // Remove subset prefix (e.g., "ABCDEF+TimesNewRomanPSMT" → "TimesNewRomanPSMT")
        auto plusPos = bn.find('+');
        std::string name = (plusPos != std::string::npos) ? bn.substr(plusPos + 1) : bn;

        // --- Times / Times New Roman family ---
        // PDF standard names: Times-Roman, Times-Bold, Times-Italic, Times-BoldItalic
        // Also: TimesNewRoman, TimesNewRomanPSMT, TimesNewRomanPS-BoldMT, etc.
        if (name.find("Times") != std::string::npos) {
            bool isBold = (name.find("Bold") != std::string::npos);
            bool isItalic = (name.find("Italic") != std::string::npos ||
                name.find("Oblique") != std::string::npos);
            if (isBold && isItalic)
                return "C:\\Windows\\Fonts\\timesbi.ttf";
            else if (isBold)
                return "C:\\Windows\\Fonts\\timesbd.ttf";
            else if (isItalic)
                return "C:\\Windows\\Fonts\\timesi.ttf";
            else
                return "C:\\Windows\\Fonts\\times.ttf";
        }

        // --- Arial / Helvetica family ---
        // PDF standard names: Helvetica, Helvetica-Bold, Helvetica-Oblique, Helvetica-BoldOblique
        if (name.find("Arial") != std::string::npos ||
            name.find("Helvetica") != std::string::npos) {
            bool isBold = (name.find("Bold") != std::string::npos);
            bool isItalic = (name.find("Italic") != std::string::npos ||
                name.find("Oblique") != std::string::npos);
            if (isBold && isItalic)
                return "C:\\Windows\\Fonts\\arialbi.ttf";
            else if (isBold)
                return "C:\\Windows\\Fonts\\arialbd.ttf";
            else if (isItalic)
                return "C:\\Windows\\Fonts\\ariali.ttf";
            else
                return "C:\\Windows\\Fonts\\arial.ttf";
        }

        // --- Courier family ---
        // PDF standard names: Courier, Courier-Bold, Courier-Oblique, Courier-BoldOblique
        if (name.find("Courier") != std::string::npos) {
            bool isBold = (name.find("Bold") != std::string::npos);
            bool isItalic = (name.find("Italic") != std::string::npos ||
                name.find("Oblique") != std::string::npos);
            if (isBold && isItalic)
                return "C:\\Windows\\Fonts\\courbi.ttf";
            else if (isBold)
                return "C:\\Windows\\Fonts\\courbd.ttf";
            else if (isItalic)
                return "C:\\Windows\\Fonts\\couri.ttf";
            else
                return "C:\\Windows\\Fonts\\cour.ttf";
        }

        // --- Symbol ---
        if (name.find("Symbol") != std::string::npos) {
            return "C:\\Windows\\Fonts\\symbol.ttf";
        }

        // --- ZapfDingbats ---
        if (name.find("ZapfDingbats") != std::string::npos ||
            name.find("Dingbats") != std::string::npos) {
            return "C:\\Windows\\Fonts\\wingding.ttf";
        }

        // --- Georgia ---
        if (name.find("Georgia") != std::string::npos) {
            if (name.find("Bold") != std::string::npos && name.find("Italic") != std::string::npos)
                return "C:\\Windows\\Fonts\\georgiaz.ttf";
            else if (name.find("Bold") != std::string::npos)
                return "C:\\Windows\\Fonts\\georgiab.ttf";
            else if (name.find("Italic") != std::string::npos)
                return "C:\\Windows\\Fonts\\georgiai.ttf";
            else
                return "C:\\Windows\\Fonts\\georgia.ttf";
        }

        // --- Verdana ---
        if (name.find("Verdana") != std::string::npos) {
            if (name.find("Bold") != std::string::npos && name.find("Italic") != std::string::npos)
                return "C:\\Windows\\Fonts\\verdanaz.ttf";
            else if (name.find("Bold") != std::string::npos)
                return "C:\\Windows\\Fonts\\verdanab.ttf";
            else if (name.find("Italic") != std::string::npos)
                return "C:\\Windows\\Fonts\\verdanai.ttf";
            else
                return "C:\\Windows\\Fonts\\verdana.ttf";
        }

        // --- Calibri ---
        if (name.find("Calibri") != std::string::npos) {
            if (name.find("Bold") != std::string::npos && name.find("Italic") != std::string::npos)
                return "C:\\Windows\\Fonts\\calibriz.ttf";
            else if (name.find("Bold") != std::string::npos)
                return "C:\\Windows\\Fonts\\calibrib.ttf";
            else if (name.find("Italic") != std::string::npos)
                return "C:\\Windows\\Fonts\\calibrii.ttf";
            else
                return "C:\\Windows\\Fonts\\calibri.ttf";
        }

        // --- Default fallback: Arial ---
        return "C:\\Windows\\Fonts\\arial.ttf";
    }

    // ============================================
    // GLOBAL CACHES FOR PERFORMANCE
    // ============================================
    static std::mutex g_pageFontsCacheMutex;
    static std::map<const PdfDocument*, std::map<int, std::map<std::string, PdfFontInfo>>> g_pageFontsCache;

    static int hexToInt(const std::string& s)
    {
        int v = 0;
        for (char c : s)
        {
            v <<= 4;
            if (c >= '0' && c <= '9') v += c - '0';
            else if (c >= 'A' && c <= 'F') v += 10 + (c - 'A');
            else if (c >= 'a' && c <= 'f') v += 10 + (c - 'a');
            else return -1;
        }
        return v;
    }

    static std::shared_ptr<PdfObject> dictGetAny(
        const std::shared_ptr<PdfDictionary>& d,
        const char* keyWithSlash,
        const char* keyNoSlash)
    {
        if (!d) return nullptr;
        auto o = d->get(keyWithSlash);
        if (!o) o = d->get(keyNoSlash);
        return o;
    }



    // Extract all <hex> values from a token that may contain multiple concatenated hex groups
    // e.g. "<0003><000A><0028>" -> {0x0003, 0x000A, 0x0028}
    // e.g. "<0003>" -> {0x0003}
    static std::vector<int> extractHexValues(const std::string& token)
    {
        std::vector<int> result;
        size_t pos = 0;
        while (pos < token.size())
        {
            size_t start = token.find('<', pos);
            if (start == std::string::npos) break;
            size_t end = token.find('>', start + 1);
            if (end == std::string::npos) break;
            std::string hex = token.substr(start + 1, end - start - 1);
            int val = hexToInt(hex);
            result.push_back(val);
            pos = end + 1;
        }
        return result;
    }

    // Read next N hex values from token stream, handling concatenated <hex><hex> tokens
    // Returns true if enough values were collected
    static bool readHexValues(std::istringstream& iss, const std::string& firstTok,
                              int count, std::vector<int>& out)
    {
        out.clear();
        // First, extract from firstTok
        auto vals = extractHexValues(firstTok);
        for (auto v : vals) out.push_back(v);

        // Read more tokens if needed
        while ((int)out.size() < count)
        {
            std::string tok;
            if (!(iss >> tok)) return false;
            auto more = extractHexValues(tok);
            for (auto v : more) out.push_back(v);
        }
        return (int)out.size() >= count;
    }

    // =========================================================
    // 👇 EKLEDİĞİMİZ ToUnicode CMap PARSER
    // =========================================================
    static void parseToUnicodeCMap(
        const std::vector<uint8_t>& data,
        PdfFontInfo& info)
    {
        std::string s(data.begin(), data.end());

        LogDebug("[ToUnicode] parseToUnicodeCMap called, data.size=%zu", data.size());

        std::istringstream iss(s);
        std::string tok;

        bool inBfChar = false;
        bool inBfRange = false;
        int parsedCount = 0;

        while (iss >> tok)
        {
            if (tok == "beginbfchar") {
                inBfChar = true;
                LogDebug("[ToUnicode] >>> beginbfchar");
                continue;
            }
            if (tok == "endbfchar") { inBfChar = false; continue; }
            if (tok == "beginbfrange") {
                inBfRange = true;
                LogDebug("[ToUnicode] >>> beginbfrange");
                continue;
            }
            if (tok == "endbfrange") { inBfRange = false; continue; }

            if (inBfChar && tok.find('<') != std::string::npos)
            {
                // bfchar: need 2 hex values (code, unicode)
                // May be in one token "<0003><0041>" or two tokens "<0003>" "<0041>"
                std::vector<int> vals;
                if (!readHexValues(iss, tok, 2, vals)) continue;

                int code = vals[0];
                int uni = vals[1];

                if (code < 0 || uni < 0) continue;
                parsedCount++;

                if (code <= 0xFF)
                {
                    info.codeToUnicode[code] = uni;
                    info.hasSimpleMap = true;
                }
                info.cidToUnicode[(uint16_t)code] = uni;
            }

            if (inBfRange && tok.find('<') != std::string::npos)
            {
                // bfrange: need 3 hex values or 2 hex + array
                // First extract start and end CIDs
                // Could be: "<0003><000A><0028>" (all concatenated)
                //       or: "<0003> <000A> <0028>" (space-separated)
                //       or: "<0003> <000A> [<0028> <0029>]" (array format)

                // Collect all hex values + check for array
                std::vector<int> hexVals = extractHexValues(tok);

                // Read more tokens until we have at least start+end (2 hex values)
                // and either a 3rd hex value or an array
                std::string nextTok;
                bool hasArray = false;
                std::string arrayContent;

                while ((int)hexVals.size() < 3 && !hasArray)
                {
                    if (!(iss >> nextTok)) break;

                    // Check for array start
                    if (nextTok.find('[') != std::string::npos)
                    {
                        hasArray = true;
                        arrayContent = nextTok;
                        break;
                    }

                    auto more = extractHexValues(nextTok);
                    for (auto v : more) hexVals.push_back(v);
                }

                if ((int)hexVals.size() < 2) continue;
                int start = hexVals[0];
                int end = hexVals[1];

                if (start < 0 || end < 0) continue;

                if (hasArray)
                {
                    // Array format: [<XXXX> <YYYY> ...]
                    std::vector<int> unicodes;
                    // Strip '[' and start extracting
                    std::string buf = arrayContent;

                    while (true)
                    {
                        bool isLast = (buf.find(']') != std::string::npos);
                        // Remove brackets
                        std::string clean;
                        for (char c : buf)
                        {
                            if (c != '[' && c != ']') clean += c;
                        }

                        auto arrVals = extractHexValues(clean);
                        for (auto v : arrVals) unicodes.push_back(v);

                        if (isLast) break;
                        if (!(iss >> buf)) break;
                    }

                    for (int i = 0; i <= (end - start) && i < (int)unicodes.size(); ++i)
                    {
                        int code = start + i;
                        int u = unicodes[i];
                        parsedCount++;
                        if (code <= 0xFF)
                        {
                            info.codeToUnicode[code] = u;
                            info.hasSimpleMap = true;
                        }
                        info.cidToUnicode[(uint16_t)code] = u;
                    }
                }
                else if ((int)hexVals.size() >= 3)
                {
                    // Normal format: start end unicodeStart
                    int uni = hexVals[2];
                    if (uni < 0) continue;

                    for (int i = 0; i <= (end - start); ++i)
                    {
                        int code = start + i;
                        int u = uni + i;
                        parsedCount++;
                        if (code <= 0xFF)
                        {
                            info.codeToUnicode[code] = u;
                            info.hasSimpleMap = true;
                        }
                        info.cidToUnicode[(uint16_t)code] = u;
                    }
                }
            }
        }

        LogDebug("[ToUnicode] FINISHED: parsedCount=%d, cidToUnicode.size=%zu, hasSimpleMap=%d",
            parsedCount, info.cidToUnicode.size(), info.hasSimpleMap ? 1 : 0);

        int cnt = 0;
        for (auto& kv : info.cidToUnicode) {
            if (cnt++ < 5)
                LogDebug("[ToUnicode]   CID 0x%04X -> Unicode 0x%04X ('%c')",
                    kv.first, kv.second, (kv.second >= 32 && kv.second < 127) ? (char)kv.second : '?');
        }
    }

    // =====================================================
    // Adobe Glyph Name to Unicode Mapping
    // =====================================================
    static uint32_t glyphNameToUnicode(const std::string& name)
    {
        // Standart Adobe glyph isimleri -> Unicode
        static const std::map<std::string, uint32_t> glyphMap = {
            // Latin Extended - Türkçe karakterler
            {"Odieresis", 0x00D6},      // Ö
            {"odieresis", 0x00F6},      // ö
            {"Udieresis", 0x00DC},      // Ü
            {"udieresis", 0x00FC},      // ü
            {"Ccedilla", 0x00C7},       // Ç
            {"ccedilla", 0x00E7},       // ç
            {"Scedilla", 0x015E},       // Ş
            {"scedilla", 0x015F},       // ş
            {"Gbreve", 0x011E},         // Ğ
            {"gbreve", 0x011F},         // ğ
            {"Idotaccent", 0x0130},     // İ
            {"dotlessi", 0x0131},       // ı

            // Almanca/Avrupa dilleri için ek karakterler
            {"Adieresis", 0x00C4},      // Ä
            {"adieresis", 0x00E4},      // ä
            {"Aring", 0x00C5},          // Å
            {"aring", 0x00E5},          // å
            {"AE", 0x00C6},             // Æ
            {"ae", 0x00E6},             // æ
            {"Ntilde", 0x00D1},         // Ñ
            {"ntilde", 0x00F1},         // ñ
            {"Oslash", 0x00D8},         // Ø
            {"oslash", 0x00F8},         // ø
            {"Thorn", 0x00DE},          // Þ
            {"thorn", 0x00FE},          // þ
            {"Eth", 0x00D0},            // Ð
            {"eth", 0x00F0},            // ð
            {"germandbls", 0x00DF},     // ß
            {"Yacute", 0x00DD},         // Ý
            {"yacute", 0x00FD},         // ý
            {"Ydieresis", 0x0178},      // Ÿ
            {"ydieresis", 0x00FF},      // ÿ

            // Aksanlı Latin karakterler
            {"Aacute", 0x00C1},         // Á
            {"aacute", 0x00E1},         // á
            {"Agrave", 0x00C0},         // À
            {"agrave", 0x00E0},         // à
            {"Acircumflex", 0x00C2},    // Â
            {"acircumflex", 0x00E2},    // â
            {"Atilde", 0x00C3},         // Ã
            {"atilde", 0x00E3},         // ã
            {"Eacute", 0x00C9},         // É
            {"eacute", 0x00E9},         // é
            {"Egrave", 0x00C8},         // È
            {"egrave", 0x00E8},         // è
            {"Ecircumflex", 0x00CA},    // Ê
            {"ecircumflex", 0x00EA},    // ê
            {"Edieresis", 0x00CB},      // Ë
            {"edieresis", 0x00EB},      // ë
            {"Iacute", 0x00CD},         // Í
            {"iacute", 0x00ED},         // í
            {"Igrave", 0x00CC},         // Ì
            {"igrave", 0x00EC},         // ì
            {"Icircumflex", 0x00CE},    // Î
            {"icircumflex", 0x00EE},    // î
            {"Idieresis", 0x00CF},      // Ï
            {"idieresis", 0x00EF},      // ï
            {"Oacute", 0x00D3},         // Ó
            {"oacute", 0x00F3},         // ó
            {"Ograve", 0x00D2},         // Ò
            {"ograve", 0x00F2},         // ò
            {"Ocircumflex", 0x00D4},    // Ô
            {"ocircumflex", 0x00F4},    // ô
            {"Otilde", 0x00D5},         // Õ
            {"otilde", 0x00F5},         // õ
            {"Uacute", 0x00DA},         // Ú
            {"uacute", 0x00FA},         // ú
            {"Ugrave", 0x00D9},         // Ù
            {"ugrave", 0x00F9},         // ù
            {"Ucircumflex", 0x00DB},    // Û
            {"ucircumflex", 0x00FB},    // û

            // Noktalama ve sembolleri
            {"space", 0x0020},
            {"exclam", 0x0021},
            {"quotedbl", 0x0022},
            {"numbersign", 0x0023},
            {"dollar", 0x0024},
            {"percent", 0x0025},
            {"ampersand", 0x0026},
            {"quotesingle", 0x0027},
            {"parenleft", 0x0028},
            {"parenright", 0x0029},
            {"asterisk", 0x002A},
            {"plus", 0x002B},
            {"comma", 0x002C},
            {"hyphen", 0x002D},
            {"period", 0x002E},
            {"slash", 0x002F},
            {"zero", 0x0030},
            {"one", 0x0031},
            {"two", 0x0032},
            {"three", 0x0033},
            {"four", 0x0034},
            {"five", 0x0035},
            {"six", 0x0036},
            {"seven", 0x0037},
            {"eight", 0x0038},
            {"nine", 0x0039},
            {"colon", 0x003A},
            {"semicolon", 0x003B},
            {"less", 0x003C},
            {"equal", 0x003D},
            {"greater", 0x003E},
            {"question", 0x003F},
            {"at", 0x0040},
            {"A", 0x0041}, {"B", 0x0042}, {"C", 0x0043}, {"D", 0x0044},
            {"E", 0x0045}, {"F", 0x0046}, {"G", 0x0047}, {"H", 0x0048},
            {"I", 0x0049}, {"J", 0x004A}, {"K", 0x004B}, {"L", 0x004C},
            {"M", 0x004D}, {"N", 0x004E}, {"O", 0x004F}, {"P", 0x0050},
            {"Q", 0x0051}, {"R", 0x0052}, {"S", 0x0053}, {"T", 0x0054},
            {"U", 0x0055}, {"V", 0x0056}, {"W", 0x0057}, {"X", 0x0058},
            {"Y", 0x0059}, {"Z", 0x005A},
            {"bracketleft", 0x005B},
            {"backslash", 0x005C},
            {"bracketright", 0x005D},
            {"asciicircum", 0x005E},
            {"underscore", 0x005F},
            {"grave", 0x0060},
            {"a", 0x0061}, {"b", 0x0062}, {"c", 0x0063}, {"d", 0x0064},
            {"e", 0x0065}, {"f", 0x0066}, {"g", 0x0067}, {"h", 0x0068},
            {"i", 0x0069}, {"j", 0x006A}, {"k", 0x006B}, {"l", 0x006C},
            {"m", 0x006D}, {"n", 0x006E}, {"o", 0x006F}, {"p", 0x0070},
            {"q", 0x0071}, {"r", 0x0072}, {"s", 0x0073}, {"t", 0x0074},
            {"u", 0x0075}, {"v", 0x0076}, {"w", 0x0077}, {"x", 0x0078},
            {"y", 0x0079}, {"z", 0x007A},
            {"braceleft", 0x007B},
            {"bar", 0x007C},
            {"braceright", 0x007D},
            {"asciitilde", 0x007E},

            // Tipografik işaretler
            {"quoteright", 0x2019},     // '
            {"quoteleft", 0x2018},      // '
            {"quotedblleft", 0x201C},   // "
            {"quotedblright", 0x201D},  // "
            {"bullet", 0x2022},
            {"endash", 0x2013},
            {"emdash", 0x2014},
            {"ellipsis", 0x2026},
            {"degree", 0x00B0},
            {"copyright", 0x00A9},
            {"registered", 0x00AE},
            {"trademark", 0x2122},
            {"section", 0x00A7},
            {"paragraph", 0x00B6},
            {"dagger", 0x2020},
            {"daggerdbl", 0x2021},
            {"fi", 0xFB01},
            {"fl", 0xFB02},
            {"f_l", 0xFB02},             // fl ligature variant name
            {"f_i", 0xFB01},             // fi ligature variant name
            {"Euro", 0x20AC},
            {"i.latn_TRK", 0x0069},      // Turkish lowercase i (same as regular i)
            {"I.latn_TRK", 0x0049},      // Turkish uppercase I
            {"minus", 0x2212},
            {"fraction", 0x2044},
            {"quotesingle", 0x0027},
            {"quotesinglbase", 0x201A},
            {"florin", 0x0192},
            {"quotedblbase", 0x201E},
            {"circumflex", 0x02C6},
            {"perthousand", 0x2030},
            {"Scaron", 0x0160},
            {"guilsinglleft", 0x2039},
            {"OE", 0x0152},
            {"Zcaron", 0x017D},
            {"tilde", 0x02DC},
            {"scaron", 0x0161},
            {"guilsinglright", 0x203A},
            {"oe", 0x0153},
            {"zcaron", 0x017E},
            {"exclamdown", 0x00A1},
            {"cent", 0x00A2},
            {"sterling", 0x00A3},
            {"currency", 0x00A4},
            {"yen", 0x00A5},
            {"brokenbar", 0x00A6},
            {"dieresis", 0x00A8},
            {"ordfeminine", 0x00AA},
            {"guillemotleft", 0x00AB},
            {"logicalnot", 0x00AC},
            {"macron", 0x00AF},
            {"plusminus", 0x00B1},
            {"twosuperior", 0x00B2},
            {"threesuperior", 0x00B3},
            {"acute", 0x00B4},
            {"mu", 0x00B5},
            {"periodcentered", 0x00B7},
            {"cedilla", 0x00B8},
            {"onesuperior", 0x00B9},
            {"ordmasculine", 0x00BA},
            {"guillemotright", 0x00BB},
            {"onequarter", 0x00BC},
            {"onehalf", 0x00BD},
            {"threequarters", 0x00BE},
            {"questiondown", 0x00BF},
            {"multiply", 0x00D7},
            {"divide", 0x00F7},
        };

        auto it = glyphMap.find(name);
        if (it != glyphMap.end())
            return it->second;

        // "uniXXXX" format kontrolü
        if (name.size() == 7 && name.substr(0, 3) == "uni")
        {
            int val = hexToInt(name.substr(3));
            if (val >= 0) return (uint32_t)val;
        }

        return 0; // bilinmeyen
    }

    // =====================================================
    // Ctor / Dtor
    // =====================================================

    PdfDocument::PdfDocument()
    {
        if (!g_ftLib)
            FT_Init_FreeType(&g_ftLib);
    }


    PdfDocument::~PdfDocument()
    {
        // Clear font cache for this document
        {
            std::lock_guard<std::mutex> lock(g_pageFontsCacheMutex);
            g_pageFontsCache.erase(this);
        }
    }

    // =====================================================
    // loadFromBytes  (SADE – SADECE PdfParser KULLANIYORUZ)
    // =====================================================

    bool PdfDocument::getPageFonts(
        int pageIndex,
        std::map<std::string, PdfFontInfo>& out) const
    {
        // ============================================
        // PAGE-LEVEL FONT CACHE - Avoid re-parsing fonts for same page
        // ============================================
        {
            std::lock_guard<std::mutex> lock(g_pageFontsCacheMutex);
            auto docIt = g_pageFontsCache.find(this);
            if (docIt != g_pageFontsCache.end())
            {
                auto pageIt = docIt->second.find(pageIndex);
                if (pageIt != docIt->second.end())
                {
                    out = pageIt->second;
                    return true;
                }
            }
        }

        out.clear();

        auto page = getPageDictionary(pageIndex);
        if (!page) return false;

        std::set<int> v;

        // -------- Resources --------
        auto resObj = resolveIndirect(dictGetAny(page, "/Resources", "Resources"), v);
        auto res = std::dynamic_pointer_cast<PdfDictionary>(resObj);
        if (!res) return true; // fonts yoksa "true" dönebilir

        // -------- Font dict --------
        v.clear();
        auto fontObj = resolveIndirect(dictGetAny(res, "/Font", "Font"), v);
        auto fontDict = std::dynamic_pointer_cast<PdfDictionary>(fontObj);
        if (!fontDict) return true;

        // -------- Iterate fonts (/F1, /F2...) --------
        for (auto& kv : fontDict->entries)
        {
            PdfFontInfo info;

            // resourceName normalize: "/F1"
            {
                std::string rn = kv.first;          // bazen "F1" bazen "/F1"
                if (!rn.empty() && rn[0] != '/')
                    rn = "/" + rn;
                info.resourceName = rn;
            }

            // Resolve font dictionary
            v.clear();
            auto fdictObj = resolveIndirect(kv.second, v);
            auto fdict = std::dynamic_pointer_cast<PdfDictionary>(fdictObj);
            if (!fdict) continue;

            // Subtype / BaseFont / Encoding
            if (auto s = std::dynamic_pointer_cast<PdfName>(dictGetAny(fdict, "/Subtype", "Subtype")))
                info.subtype = s->value;

            if (auto b = std::dynamic_pointer_cast<PdfName>(dictGetAny(fdict, "/BaseFont", "BaseFont")))
                info.baseFont = b->value;

            // ---- Encoding ----
            {
                auto encObj = dictGetAny(fdict, "/Encoding", "Encoding");
                if (encObj)
                {
                    // Encoding bir /Name olabilir (örn. /WinAnsiEncoding)
                    if (auto e = std::dynamic_pointer_cast<PdfName>(encObj))
                    {
                        info.encoding = e->value;
                    }
                    // Veya bir Dictionary olabilir (içinde /Differences var)
                    else
                    {
                        std::set<int> venc;
                        auto encDictObj = resolveIndirect(encObj, venc);
                        auto encDict = std::dynamic_pointer_cast<PdfDictionary>(encDictObj);
                        if (encDict)
                        {
                            // BaseEncoding varsa oku
                            if (auto be = std::dynamic_pointer_cast<PdfName>(dictGetAny(encDict, "/BaseEncoding", "BaseEncoding")))
                                info.encoding = be->value;

                            // /Differences array'ini parse et
                            auto diffObj = dictGetAny(encDict, "/Differences", "Differences");
                            auto diffArr = std::dynamic_pointer_cast<PdfArray>(diffObj);
                            if (diffArr && !diffArr->items.empty())
                            {
                                int currentCode = 0;
                                for (auto& item : diffArr->items)
                                {
                                    if (auto num = std::dynamic_pointer_cast<PdfNumber>(item))
                                    {
                                        // Yeni başlangıç kodu
                                        currentCode = (int)num->value;
                                    }
                                    else if (auto name = std::dynamic_pointer_cast<PdfName>(item))
                                    {
                                        // Glyph ismi -> Unicode dönüşümü
                                        std::string glyphName = name->value;
                                        if (!glyphName.empty() && glyphName[0] == '/')
                                            glyphName = glyphName.substr(1);

                                        // ✅ Glyph name'i sakla (MuPDF tarzı cid_to_gid için)
                                        if (currentCode >= 0 && currentCode < 256)
                                            info.codeToGlyphName[currentCode] = glyphName;

                                        uint32_t uni = glyphNameToUnicode(glyphName);
                                        if (uni != 0 && currentCode >= 0 && currentCode < 256)
                                        {
                                            info.codeToUnicode[currentCode] = uni;
                                            info.hasSimpleMap = true;
                                        }
                                        currentCode++;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // ---- ToUnicode (decode ederek) ----
            {
                std::set<int> vt;
                auto tuObj = resolveIndirect(dictGetAny(fdict, "/ToUnicode", "ToUnicode"), vt);
                auto tu = std::dynamic_pointer_cast<PdfStream>(tuObj);

                LogDebug("[Font] %s (baseFont=%s): ToUnicode stream %s",
                    info.resourceName.c_str(), info.baseFont.c_str(),
                    tu ? "FOUND" : "NOT FOUND");

                if (tu)
                {
                    std::vector<uint8_t> tuDecoded;
                    if (decodeStream(tu, tuDecoded)) {
                        LogDebug("[Font] ToUnicode decoded, size=%zu bytes", tuDecoded.size());
                        parseToUnicodeCMap(tuDecoded, info);
                    }
                    else {
                        LogDebug("[Font] ToUnicode decode FAILED, using raw data");
                        parseToUnicodeCMap(tu->data, info); // fallback
                    }
                }
                else {
                    LogDebug("[Font] NO ToUnicode for %s", info.baseFont.c_str());
                }
            }

            // ---- Embedded font program (FontDescriptor -> FontFile2/FontFile3) ----
            {
                std::set<int> vfdesc;
                auto fdObj = resolveIndirect(dictGetAny(fdict, "/FontDescriptor", "FontDescriptor"), vfdesc);
                auto fd = std::dynamic_pointer_cast<PdfDictionary>(fdObj);

                // Type0 (CID) ise descriptor descendant font içinde olabilir
                if (!fd && info.subtype == "/Type0")
                {
                    std::set<int> vd2;
                    auto descObj2 = resolveIndirect(dictGetAny(fdict, "/DescendantFonts", "DescendantFonts"), vd2);
                    auto descArr2 = std::dynamic_pointer_cast<PdfArray>(descObj2);
                    if (descArr2 && !descArr2->items.empty())
                    {
                        vd2.clear();
                        auto cidObj2 = resolveIndirect(descArr2->items[0], vd2);
                        auto cidDict2 = std::dynamic_pointer_cast<PdfDictionary>(cidObj2);
                        if (cidDict2)
                        {
                            vfdesc.clear();
                            auto fdObj2 = resolveIndirect(dictGetAny(cidDict2, "/FontDescriptor", "FontDescriptor"), vfdesc);
                            fd = std::dynamic_pointer_cast<PdfDictionary>(fdObj2);
                        }
                    }
                }

                if (fd)
                {
                    std::shared_ptr<PdfStream> ff;

                    // FontFile (Type 1 PFA/PFB) - en önce bu denen
                    {
                        std::set<int> vff;
                        auto ffObj = resolveIndirect(dictGetAny(fd, "/FontFile", "FontFile"), vff);
                        ff = std::dynamic_pointer_cast<PdfStream>(ffObj);
                        if (ff)
                            info.fontProgramSubtype = "Type1";
                    }

                    // FontFile2 (TrueType)
                    if (!ff)
                    {
                        std::set<int> vff;
                        auto ffObj = resolveIndirect(dictGetAny(fd, "/FontFile2", "FontFile2"), vff);
                        ff = std::dynamic_pointer_cast<PdfStream>(ffObj);
                        if (ff)
                            info.fontProgramSubtype = "TrueType";
                    }

                    // FontFile3 (Type1C/CFF/OpenType)
                    if (!ff)
                    {
                        std::set<int> vff;
                        auto ffObj = resolveIndirect(dictGetAny(fd, "/FontFile3", "FontFile3"), vff);
                        ff = std::dynamic_pointer_cast<PdfStream>(ffObj);

                        if (ff && ff->dict)
                        {
                            // /Subtype in FontFile3 stream
                            if (auto st = std::dynamic_pointer_cast<PdfName>(dictGetAny(ff->dict, "/Subtype", "Subtype")))
                                info.fontProgramSubtype = st->value;
                            else
                                info.fontProgramSubtype = "FontFile3";
                        }
                    }

                    if (ff)
                    {
                        std::vector<uint8_t> decoded;
                        if (decodeStream(ff, decoded))
                            info.fontProgram = std::move(decoded);
                        else
                            info.fontProgram = ff->data; // fallback

                        LogDebug("  Font '%s': Loaded %s font program (%zu bytes)",
                            info.resourceName.c_str(), info.fontProgramSubtype.c_str(), info.fontProgram.size());
                    }
                }
            }

            // ---- Simple fonts (Type0 değilse) ----
            if (info.subtype != "/Type0")
            {
                // /FirstChar
                if (auto fc = std::dynamic_pointer_cast<PdfNumber>(dictGetAny(fdict, "/FirstChar", "FirstChar")))
                    info.firstChar = (int)fc->value;

                // /MissingWidth (opsiyonel)
                if (auto mw = std::dynamic_pointer_cast<PdfNumber>(dictGetAny(fdict, "/MissingWidth", "MissingWidth")))
                    info.missingWidth = (int)mw->value;

                // /Widths
                std::set<int> vw;
                auto wObj = resolveIndirect(dictGetAny(fdict, "/Widths", "Widths"), vw);
                auto wArr = std::dynamic_pointer_cast<PdfArray>(wObj);

                info.widths.clear();
                if (wArr && !wArr->items.empty())
                {
                    info.widths.reserve(wArr->items.size());
                    for (auto& itW : wArr->items)
                    {
                        auto n = std::dynamic_pointer_cast<PdfNumber>(itW);
                        info.widths.push_back(n ? (int)n->value : info.missingWidth);
                    }
                    info.hasWidths = true;
                }

                // Eger fontProgram yoksa, baseFont'a gore sistem fontunu yukle
                if (info.fontProgram.empty())
                {
                    std::string fontPath = resolveSystemFontPath(info.baseFont);

                    std::ifstream fontFile(fontPath, std::ios::binary);
                    if (fontFile) {
                        info.fontProgram.assign(
                            std::istreambuf_iterator<char>(fontFile),
                            std::istreambuf_iterator<char>()
                        );
                        LogDebug("  Font '%s': baseFont='%s' -> system '%s' (%zu bytes)",
                            info.resourceName.c_str(), info.baseFont.c_str(), fontPath.c_str(), info.fontProgram.size());
                    }
                }

                // ═══════════════════════════════════════════════════════════
                // WinAnsiEncoding ise standart glyph isimlerini codeToGlyphName'e doldur
                // CFF/Type1C fontlarda charmap olmayabilir, glyph name ile eşleştirmek şart
                // ═══════════════════════════════════════════════════════════
                {
                    static const char* winAnsiGlyphNames[256] = {
                        nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
                        nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
                        nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
                        nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
                        "space","exclam","quotedbl","numbersign","dollar","percent","ampersand","quotesingle",
                        "parenleft","parenright","asterisk","plus","comma","hyphen","period","slash",
                        "zero","one","two","three","four","five","six","seven",
                        "eight","nine","colon","semicolon","less","equal","greater","question",
                        "at","A","B","C","D","E","F","G",
                        "H","I","J","K","L","M","N","O",
                        "P","Q","R","S","T","U","V","W",
                        "X","Y","Z","bracketleft","backslash","bracketright","asciicircum","underscore",
                        "grave","a","b","c","d","e","f","g",
                        "h","i","j","k","l","m","n","o",
                        "p","q","r","s","t","u","v","w",
                        "x","y","z","braceleft","bar","braceright","asciitilde",nullptr,
                        "Euro",nullptr,"quotesinglbase","florin","quotedblbase","ellipsis","dagger","daggerdbl",
                        "circumflex","perthousand","Scaron","guilsinglleft","OE",nullptr,"Zcaron",nullptr,
                        nullptr,"quoteleft","quoteright","quotedblleft","quotedblright","bullet","endash","emdash",
                        "tilde","trademark","scaron","guilsinglright","oe",nullptr,"zcaron","Ydieresis",
                        "space","exclamdown","cent","sterling","currency","yen","brokenbar","section",
                        "dieresis","copyright","ordfeminine","guillemotleft","logicalnot","hyphen","registered","macron",
                        "degree","plusminus","twosuperior","threesuperior","acute","mu","paragraph","periodcentered",
                        "cedilla","onesuperior","ordmasculine","guillemotright","onequarter","onehalf","threequarters","questiondown",
                        "Agrave","Aacute","Acircumflex","Atilde","Adieresis","Aring","AE","Ccedilla",
                        "Egrave","Eacute","Ecircumflex","Edieresis","Igrave","Iacute","Icircumflex","Idieresis",
                        "Eth","Ntilde","Ograve","Oacute","Ocircumflex","Otilde","Odieresis","multiply",
                        "Oslash","Ugrave","Uacute","Ucircumflex","Udieresis","Yacute","Thorn","germandbls",
                        "agrave","aacute","acircumflex","atilde","adieresis","aring","ae","ccedilla",
                        "egrave","eacute","ecircumflex","edieresis","igrave","iacute","icircumflex","idieresis",
                        "eth","ntilde","ograve","oacute","ocircumflex","otilde","odieresis","divide",
                        "oslash","ugrave","uacute","ucircumflex","udieresis","yacute","thorn","ydieresis"
                    };
                    bool isWinAnsi = (info.encoding == "/WinAnsiEncoding" || info.encoding == "WinAnsiEncoding");
                    // Encoding boş bile olsa (base encoding yoksa), standart glyph isimleri doldur
                    // CFF fontlarda bu zorunlu — charmap olmadan sadece glyph name ile erişilebilir
                    if (isWinAnsi || info.encoding.empty()) {
                        for (int code = 0; code < 256; code++) {
                            if (info.codeToGlyphName[code].empty() && winAnsiGlyphNames[code] != nullptr) {
                                info.codeToGlyphName[code] = winAnsiGlyphNames[code];
                            }
                        }
                    }
                }

                // MuPDF tarzi: Font programi varsa, codeToGid ve width tablosunu olustur
                if (!info.fontProgram.empty())
                {
                    FT_Face tempFace = nullptr;
                    FT_Error err = FT_New_Memory_Face(
                        g_ftLib,
                        info.fontProgram.data(),
                        (FT_Long)info.fontProgram.size(),
                        0,
                        &tempFace
                    );

                    if (err == 0 && tempFace)
                    {
                        FT_UShort units_per_EM = tempFace->units_per_EM;
                        if (units_per_EM == 0)
                            units_per_EM = 2048;

                        // ═══════════════════════════════════════════════════════════
                        // MuPDF tarzı: codeToGid tablosunu oluştur
                        // ═══════════════════════════════════════════════════════════

                        // 1. Doğru charmap'i seç (MuPDF select_truetype_cmap gibi)
                        FT_CharMap bestCmap = nullptr;

                        // Önce Microsoft Unicode cmap (platform=3, encoding=1)
                        for (int cm = 0; cm < tempFace->num_charmaps; cm++) {
                            if (tempFace->charmaps[cm]->platform_id == 3 &&
                                tempFace->charmaps[cm]->encoding_id == 1) {
                                bestCmap = tempFace->charmaps[cm];
                                break;
                            }
                        }
                        // Yoksa Apple MacRoman (platform=1, encoding=0)
                        if (!bestCmap) {
                            for (int cm = 0; cm < tempFace->num_charmaps; cm++) {
                                if (tempFace->charmaps[cm]->platform_id == 1 &&
                                    tempFace->charmaps[cm]->encoding_id == 0) {
                                    bestCmap = tempFace->charmaps[cm];
                                    break;
                                }
                            }
                        }
                        // En son ilk charmap
                        if (!bestCmap && tempFace->num_charmaps > 0) {
                            bestCmap = tempFace->charmaps[0];
                        }

                        if (bestCmap) {
                            FT_Set_Charmap(tempFace, bestCmap);
                        }

                        // ═══════════════════════════════════════════════════════════
                        // CFF/Type1C fontlar için: FT_Get_Glyph_Name ile glyph isimlerini oku
                        // ═══════════════════════════════════════════════════════════

                        std::map<std::string, FT_UInt> nameToGid;
                        for (FT_UInt gidx = 0; gidx < (FT_UInt)tempFace->num_glyphs; gidx++) {
                            char glyphName[256] = { 0 };
                            if (FT_HAS_GLYPH_NAMES(tempFace)) {
                                if (FT_Get_Glyph_Name(tempFace, gidx, glyphName, 256) == 0 && glyphName[0] != 0) {
                                    nameToGid[glyphName] = gidx;
                                }
                            }
                        }

                        // 2. Önce codeToGlyphName'den eşleştir (en güvenilir)
                        for (int code = 0; code < 256; code++) {
                            if (!info.codeToGlyphName[code].empty()) {
                                auto it = nameToGid.find(info.codeToGlyphName[code]);
                                if (it != nameToGid.end()) {
                                    info.codeToGid[code] = (uint16_t)it->second;
                                }
                            }
                        }

                        // 3. Fallback: charmap'ten dene
                        for (int code = 0; code < 256; code++) {
                            if (info.codeToGid[code] == 0) {
                                if (info.codeToUnicode[code] != 0) {
                                    FT_UInt uniGid = FT_Get_Char_Index(tempFace, (FT_ULong)info.codeToUnicode[code]);
                                    if (uniGid > 0) {
                                        info.codeToGid[code] = (uint16_t)uniGid;
                                    }
                                }
                                if (info.codeToGid[code] == 0) {
                                    FT_UInt gi = FT_Get_Char_Index(tempFace, (FT_ULong)code);
                                    if (gi > 0) {
                                        info.codeToGid[code] = (uint16_t)gi;
                                    }
                                }
                            }
                        }

                        info.hasCodeToGid = true;

                        // ═══════════════════════════════════════════════════════════
                        // Width tablosunu oluştur (eğer /Widths yoksa)
                        // ═══════════════════════════════════════════════════════════
                        if (!info.hasWidths)
                        {
                            info.firstChar = 0;
                            info.widths.resize(256, info.missingWidth);

                            for (int code = 0; code < 256; code++)
                            {
                                FT_UInt gi = info.codeToGid[code];
                                if (gi != 0)
                                {
                                    FT_Fixed adv = 0;
                                    int mask = FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP | FT_LOAD_IGNORE_TRANSFORM;
                                    FT_Error advErr = FT_Get_Advance(tempFace, gi, mask, &adv);

                                    if (advErr == 0 && adv > 0)
                                    {
                                        int w = (int)(adv * 1000 / units_per_EM);
                                        if (w > 0)
                                            info.widths[code] = w;
                                    }
                                }
                            }

                            info.hasWidths = true;
                        }

                        FT_Done_Face(tempFace);
                    }
                }
            }

            // ---- CID fonts (Type0) ----
            if (info.subtype == "/Type0")
            {
                info.isCidFont = true;

                // DescendantFonts[0] = CIDFontType0/2
                std::set<int> vd;
                auto descObj = resolveIndirect(dictGetAny(fdict, "/DescendantFonts", "DescendantFonts"), vd);
                auto descArr = std::dynamic_pointer_cast<PdfArray>(descObj);

                std::shared_ptr<PdfDictionary> cidFontDict;
                if (descArr && !descArr->items.empty())
                {
                    vd.clear();
                    auto cidObj = resolveIndirect(descArr->items[0], vd);
                    cidFontDict = std::dynamic_pointer_cast<PdfDictionary>(cidObj);
                }

                // Default width (DW) ve Width array (W)
                if (cidFontDict)
                {
                    // /DW - CID default width
                    if (auto dw = std::dynamic_pointer_cast<PdfNumber>(dictGetAny(cidFontDict, "/DW", "DW"))) {
                        info.cidDefaultWidth = (int)dw->value;
                        info.missingWidth = (int)dw->value;
                    }

                    // /W - CID width array
                    // Format: [ cid [w1 w2 ...] ] veya [ cid1 cid2 w ]
                    if (auto wArr = std::dynamic_pointer_cast<PdfArray>(dictGetAny(cidFontDict, "/W", "W")))
                    {
                        size_t idx = 0;
                        while (idx < wArr->items.size())
                        {
                            auto cidStart = std::dynamic_pointer_cast<PdfNumber>(wArr->items[idx]);
                            if (!cidStart) { idx++; continue; }

                            int startCid = (int)cidStart->value;
                            idx++;

                            if (idx >= wArr->items.size()) break;

                            // Sonraki eleman array mi yoksa number mi?
                            if (auto widthArr = std::dynamic_pointer_cast<PdfArray>(wArr->items[idx]))
                            {
                                // Format: cid [w1 w2 w3 ...]
                                int cid = startCid;
                                for (auto& wItem : widthArr->items)
                                {
                                    if (auto wNum = std::dynamic_pointer_cast<PdfNumber>(wItem))
                                        info.cidWidths[(uint16_t)cid] = (int)wNum->value;
                                    cid++;
                                }
                                idx++;
                            }
                            else if (auto cidEnd = std::dynamic_pointer_cast<PdfNumber>(wArr->items[idx]))
                            {
                                // Format: cid1 cid2 w (aralik)
                                int endCid = (int)cidEnd->value;
                                idx++;

                                if (idx < wArr->items.size())
                                {
                                    if (auto wNum = std::dynamic_pointer_cast<PdfNumber>(wArr->items[idx]))
                                    {
                                        int w = (int)wNum->value;
                                        for (int c = startCid; c <= endCid; c++)
                                            info.cidWidths[(uint16_t)c] = w;
                                        idx++;
                                    }
                                }
                            }
                        }
                    }

                    // -------------------------------------------------
                    // ✅ CIDToGIDMap (Type0 -> DescendantFonts[0])
                    // -------------------------------------------------
                    {
                        std::set<int> vis;
                        auto mapObj = resolveIndirect(dictGetAny(cidFontDict, "/CIDToGIDMap", "CIDToGIDMap"), vis);

                        // Yoksa default Identity kabul et
                        info.hasCidToGidMap = false;
                        info.cidToGidIdentity = true;
                        info.cidToGid.clear();

                        if (auto nm = std::dynamic_pointer_cast<PdfName>(mapObj))
                        {
                            if (nm->value == "/Identity" || nm->value == "Identity")
                            {
                                info.hasCidToGidMap = true;
                                info.cidToGidIdentity = true;
                            }
                        }
                        else if (auto st = std::dynamic_pointer_cast<PdfStream>(mapObj))
                        {
                            std::vector<uint8_t> bytes;
                            if (decodeStream(st, bytes))
                            {
                                info.cidToGid.resize(bytes.size() / 2);
                                for (size_t i = 0; i + 1 < bytes.size(); i += 2)
                                    info.cidToGid[i / 2] =
                                    (uint16_t(bytes[i]) << 8) | uint16_t(bytes[i + 1]);

                                info.hasCidToGidMap = true;
                                info.cidToGidIdentity = false;
                            }
                            else
                            {
                                // decode edemediysek yine identity varsay
                                info.hasCidToGidMap = true;
                                info.cidToGidIdentity = true;
                                info.cidToGid.clear();
                            }
                        }
                    }
                }
                else
                {
                    // bazı PDF'lerde DW Type0'ın kendisinde de olabiliyor
                    if (auto dw = std::dynamic_pointer_cast<PdfNumber>(dictGetAny(fdict, "/DW", "DW")))
                        info.missingWidth = (int)dw->value;

                    // CIDToGIDMap yok → identity varsay
                    info.hasCidToGidMap = false;
                    info.cidToGidIdentity = true;
                    info.cidToGid.clear();
                }
            }

            // ================================================================
            // CID font icin: Eksik CID widthlerini FreeType'tan hesapla
            // /W array'de olan degerler korunur, olmayanlar FreeType ile doldurulur
            // ================================================================
            if (info.isCidFont && !info.fontProgram.empty())
            {
                FT_Face widthFace = nullptr;
                FT_Error err = FT_New_Memory_Face(
                    g_ftLib,
                    info.fontProgram.data(),
                    (FT_Long)info.fontProgram.size(),
                    0,
                    &widthFace
                );

                if (err == 0 && widthFace)
                {
                    FT_UShort unitsPerEM = widthFace->units_per_EM;
                    if (unitsPerEM == 0) unitsPerEM = 1000;

                    // Unicode charmap sec
                    for (int cm = 0; cm < widthFace->num_charmaps; cm++) {
                        if (widthFace->charmaps[cm]->platform_id == 3 &&
                            widthFace->charmaps[cm]->encoding_id == 1) {
                            FT_Set_Charmap(widthFace, widthFace->charmaps[cm]);
                            break;
                        }
                    }

                    // CIDToGIDMap kullanarak eksik widthleri FreeType'tan doldur
                    // Identity mapping: CID = GID
                    // Explicit mapping: cidToGid tablosundan
                    auto getGidForCid = [&](uint16_t cid) -> FT_UInt {
                        if (info.cidToGidIdentity) {
                            return (FT_UInt)cid;
                        }
                        else if (!info.cidToGid.empty() && cid < info.cidToGid.size()) {
                            return (FT_UInt)info.cidToGid[cid];
                        }
                        // Fallback: cidToUnicode tablosundan unicode'a, sonra charmap ile GID'e
                        auto uniIt = info.cidToUnicode.find(cid);
                        if (uniIt != info.cidToUnicode.end()) {
                            return FT_Get_Char_Index(widthFace, (FT_ULong)uniIt->second);
                        }
                        return 0;
                        };

                    // Yontem 1: cidToUnicode tablosundaki tum CID'ler icin
                    for (auto& kv : info.cidToUnicode)
                    {
                        uint16_t cid = kv.first;
                        // /W array'deki deger zaten var, FreeType'tan ezme
                        if (info.cidWidths.count(cid)) continue;

                        FT_UInt gid = getGidForCid(cid);
                        if (gid == 0) {
                            // Try unicode lookup
                            gid = FT_Get_Char_Index(widthFace, (FT_ULong)kv.second);
                        }
                        if (gid == 0) continue;

                        FT_Fixed adv = 0;
                        FT_Error advErr = FT_Get_Advance(widthFace, gid,
                            FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING, &adv);

                        if (advErr == 0 && adv > 0)
                        {
                            int w = (int)((adv * 1000) / unitsPerEM);
                            if (w > 0)
                                info.cidWidths[cid] = w;
                        }
                    }

                    // Yontem 2: CIDToGIDMap Identity ise, font'taki tum glyphleri tara
                    // (cidToUnicode eksik olabilir)
                    if (info.cidToGidIdentity)
                    {
                        for (FT_UInt gid = 1; gid < (FT_UInt)widthFace->num_glyphs && gid < 65535; gid++)
                        {
                            uint16_t cid = (uint16_t)gid;
                            if (info.cidWidths.count(cid)) continue;

                            FT_Fixed adv = 0;
                            FT_Error advErr = FT_Get_Advance(widthFace, gid,
                                FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING, &adv);

                            if (advErr == 0 && adv > 0)
                            {
                                int w = (int)((adv * 1000) / unitsPerEM);
                                if (w > 0)
                                    info.cidWidths[cid] = w;
                            }
                        }
                    }

                    LogDebug("  CID Font '%s': cidWidths populated: %zu entries (from /W + FreeType)",
                        info.resourceName.c_str(), info.cidWidths.size());

                    FT_Done_Face(widthFace);
                }
            }

            // MUTLAKA MAP'E KOY
            out[info.resourceName] = info;

            // DEBUG: Her font icin log
            {
                static FILE* gpfDbg = nullptr; // fopen("C:\\temp\\getpagefonts_debug.txt", "a");
                if (gpfDbg) {
                    fprintf(gpfDbg, "  FONT: '%s' -> baseFont='%s', subtype='%s', encoding='%s'\n",
                        info.resourceName.c_str(), info.baseFont.c_str(),
                        info.subtype.c_str(), info.encoding.c_str());
                    fprintf(gpfDbg, "    isCidFont=%d, cidToUnicode.size=%zu, fontProgram.size=%zu\n",
                        info.isCidFont ? 1 : 0, info.cidToUnicode.size(), info.fontProgram.size());
                    fprintf(gpfDbg, "    cidDefaultWidth=%d, cidWidths.size=%zu\n",
                        info.cidDefaultWidth, info.cidWidths.size());
                    // Ilk 10 cidWidth'i goster
                    int cnt = 0;
                    for (auto& kv : info.cidWidths) {
                        if (cnt++ < 10)
                            fprintf(gpfDbg, "      CID 0x%04X -> width=%d\n", kv.first, kv.second);
                    }
                    fflush(gpfDbg);
                }
            }
        }

        // ============================================
        // CACHE RESULT
        // ============================================
        {
            std::lock_guard<std::mutex> lock(g_pageFontsCacheMutex);
            g_pageFontsCache[this][pageIndex] = out;
        }

        return true;
    }


    // =========================================================
    // Form XObject fontlarını yüklemek için
    // =========================================================
    bool PdfDocument::loadFontsFromResourceDict(
        const std::shared_ptr<PdfDictionary>& resDict,
        std::map<std::string, PdfFontInfo>& fonts) const
    {
        if (!resDict) return false;

        std::set<int> v;

        // Font dictionary'yi bul
        auto fontObj = resolveIndirect(dictGetAny(resDict, "/Font", "Font"), v);
        auto fontDict = std::dynamic_pointer_cast<PdfDictionary>(fontObj);
        if (!fontDict) return false;

        LogDebug("loadFontsFromResourceDict: Found %zu fonts", fontDict->entries.size());

        // DEBUG: Dosyaya yaz
        {
            static FILE* fontDbg = nullptr; // fopen("C:\\temp\\font_load_debug.txt", "w");
            if (fontDbg) {
                fprintf(fontDbg, "=== loadFontsFromResourceDict ===\n");
                fprintf(fontDbg, "Found %zu fonts in resource dict\n", fontDict->entries.size());
                for (auto& kv : fontDict->entries) {
                    fprintf(fontDbg, "  Font entry: '%s'\n", kv.first.c_str());
                }
                fflush(fontDbg);
            }
        }

        // Her font için
        for (auto& kv : fontDict->entries)
        {
            // Bu font zaten yüklü mü?
            std::string rn = kv.first;
            if (!rn.empty() && rn[0] != '/')
                rn = "/" + rn;

            if (fonts.find(rn) != fonts.end())
            {
                LogDebug("  Font '%s' already loaded, skipping", rn.c_str());
                continue;
            }

            PdfFontInfo info;
            info.resourceName = rn;

            // Font dictionary'yi resolve et
            v.clear();
            auto fdictObj = resolveIndirect(kv.second, v);
            auto fdict = std::dynamic_pointer_cast<PdfDictionary>(fdictObj);
            if (!fdict) continue;

            // Subtype / BaseFont / Encoding
            if (auto s = std::dynamic_pointer_cast<PdfName>(dictGetAny(fdict, "/Subtype", "Subtype")))
                info.subtype = s->value;

            if (auto b = std::dynamic_pointer_cast<PdfName>(dictGetAny(fdict, "/BaseFont", "BaseFont")))
                info.baseFont = b->value;

            LogDebug("  Loading font '%s' (BaseFont: %s, Subtype: %s)",
                rn.c_str(), info.baseFont.c_str(), info.subtype.c_str());

            // Encoding
            {
                auto encObj = dictGetAny(fdict, "/Encoding", "Encoding");
                LogDebug("  Font '%s': encObj=%p", rn.c_str(), (void*)encObj.get());

                if (encObj)
                {
                    if (auto e = std::dynamic_pointer_cast<PdfName>(encObj))
                    {
                        info.encoding = e->value;
                        LogDebug("    Encoding (Name): '%s'", info.encoding.c_str());
                    }
                    else
                    {
                        // Encoding bir IndirectRef veya Dictionary olabilir
                        std::set<int> venc;
                        auto encDictObj = resolveIndirect(encObj, venc);
                        LogDebug("    Encoding encDictObj=%p (type after resolve)", (void*)encDictObj.get());

                        auto encDict = std::dynamic_pointer_cast<PdfDictionary>(encDictObj);
                        if (encDict)
                        {
                            LogDebug("    Encoding is Dictionary with %zu entries", encDict->entries.size());

                            if (auto be = std::dynamic_pointer_cast<PdfName>(dictGetAny(encDict, "/BaseEncoding", "BaseEncoding")))
                            {
                                info.encoding = be->value;
                                LogDebug("    BaseEncoding: '%s'", info.encoding.c_str());
                            }

                            // ✅ /Differences array'ini parse et
                            auto diffObj = dictGetAny(encDict, "/Differences", "Differences");
                            auto diffArr = std::dynamic_pointer_cast<PdfArray>(diffObj);

                            if (diffArr && !diffArr->items.empty())
                            {
                                LogDebug("    Differences array: %zu items", diffArr->items.size());
                                int currentCode = 0;
                                int glyphCount = 0;

                                for (auto& item : diffArr->items)
                                {
                                    if (auto num = std::dynamic_pointer_cast<PdfNumber>(item))
                                    {
                                        currentCode = (int)num->value;
                                    }
                                    else if (auto name = std::dynamic_pointer_cast<PdfName>(item))
                                    {
                                        std::string glyphName = name->value;
                                        if (!glyphName.empty() && glyphName[0] == '/')
                                            glyphName = glyphName.substr(1);

                                        if (currentCode >= 0 && currentCode < 256) {
                                            info.codeToGlyphName[currentCode] = glyphName;
                                            uint32_t uni = glyphNameToUnicode(glyphName);
                                            if (uni != 0) {
                                                info.codeToUnicode[currentCode] = uni;
                                                info.hasSimpleMap = true;
                                            }
                                            glyphCount++;
                                        }
                                        currentCode++;
                                    }
                                }
                                LogDebug("    Parsed %d glyph names from Differences", glyphCount);
                            }
                            else
                            {
                                LogDebug("    No Differences array found");
                            }
                        }
                        else
                        {
                            LogDebug("    Encoding is not a Dictionary after resolve");
                        }
                    }
                }
                else
                {
                    LogDebug("    No Encoding found");
                }
            }

            // ToUnicode - parseToUnicodeCMap fonksiyonunu kullan (tam destek)
            {
                std::set<int> vt;
                auto tuObj = resolveIndirect(dictGetAny(fdict, "/ToUnicode", "ToUnicode"), vt);
                auto tu = std::dynamic_pointer_cast<PdfStream>(tuObj);
                if (tu)
                {
                    std::vector<uint8_t> tuDecoded;
                    if (decodeStream(tu, tuDecoded))
                    {
                        // Tam ToUnicode CMap parser kullan (beginbfchar + beginbfrange)
                        parseToUnicodeCMap(tuDecoded, info);
                    }
                    else
                    {
                        // Fallback - raw data ile dene
                        parseToUnicodeCMap(tu->data, info);
                    }
                }
            }

            // Embedded font program (FontDescriptor)
            {
                std::set<int> vfdesc;
                auto fdObj = resolveIndirect(dictGetAny(fdict, "/FontDescriptor", "FontDescriptor"), vfdesc);
                auto fd = std::dynamic_pointer_cast<PdfDictionary>(fdObj);

                // Type0 için DescendantFonts'ta olabilir
                if (!fd && info.subtype == "/Type0")
                {
                    std::set<int> vd2;
                    auto descObj2 = resolveIndirect(dictGetAny(fdict, "/DescendantFonts", "DescendantFonts"), vd2);
                    auto descArr2 = std::dynamic_pointer_cast<PdfArray>(descObj2);
                    if (descArr2 && !descArr2->items.empty())
                    {
                        vd2.clear();
                        auto cidObj2 = resolveIndirect(descArr2->items[0], vd2);
                        auto cidDict2 = std::dynamic_pointer_cast<PdfDictionary>(cidObj2);
                        if (cidDict2)
                        {
                            vfdesc.clear();
                            auto fdObj2 = resolveIndirect(dictGetAny(cidDict2, "/FontDescriptor", "FontDescriptor"), vfdesc);
                            fd = std::dynamic_pointer_cast<PdfDictionary>(fdObj2);
                        }
                    }
                }

                if (fd)
                {
                    std::shared_ptr<PdfStream> ff;

                    // FontFile (Type 1 PFA/PFB) - en önce bu denen
                    {
                        std::set<int> vff;
                        auto ffObj = resolveIndirect(dictGetAny(fd, "/FontFile", "FontFile"), vff);
                        ff = std::dynamic_pointer_cast<PdfStream>(ffObj);
                        if (ff)
                            info.fontProgramSubtype = "Type1";
                    }

                    // FontFile2 (TrueType)
                    if (!ff)
                    {
                        std::set<int> vff;
                        auto ffObj = resolveIndirect(dictGetAny(fd, "/FontFile2", "FontFile2"), vff);
                        ff = std::dynamic_pointer_cast<PdfStream>(ffObj);
                        if (ff)
                            info.fontProgramSubtype = "TrueType";
                    }

                    // FontFile3 (Type1C/CFF/OpenType)
                    if (!ff)
                    {
                        std::set<int> vff;
                        auto ffObj = resolveIndirect(dictGetAny(fd, "/FontFile3", "FontFile3"), vff);
                        ff = std::dynamic_pointer_cast<PdfStream>(ffObj);

                        if (ff && ff->dict)
                        {
                            if (auto st = std::dynamic_pointer_cast<PdfName>(dictGetAny(ff->dict, "/Subtype", "Subtype")))
                                info.fontProgramSubtype = st->value;
                            else
                                info.fontProgramSubtype = "FontFile3";
                        }
                    }

                    if (ff)
                    {
                        std::vector<uint8_t> decoded;
                        if (decodeStream(ff, decoded))
                            info.fontProgram = std::move(decoded);
                        else
                            info.fontProgram = ff->data;

                        LogDebug("    Font program loaded: %s, %zu bytes",
                            info.fontProgramSubtype.c_str(), info.fontProgram.size());
                    }
                }
            }

            // Widths (simple fonts)
            if (info.subtype != "/Type0")
            {
                if (auto fc = std::dynamic_pointer_cast<PdfNumber>(dictGetAny(fdict, "/FirstChar", "FirstChar")))
                    info.firstChar = (int)fc->value;

                if (auto mw = std::dynamic_pointer_cast<PdfNumber>(dictGetAny(fdict, "/MissingWidth", "MissingWidth")))
                    info.missingWidth = (int)mw->value;

                std::set<int> vw;
                auto wObj = resolveIndirect(dictGetAny(fdict, "/Widths", "Widths"), vw);
                auto wArr = std::dynamic_pointer_cast<PdfArray>(wObj);

                if (wArr && !wArr->items.empty())
                {
                    info.widths.reserve(wArr->items.size());
                    for (auto& itW : wArr->items)
                    {
                        auto n = std::dynamic_pointer_cast<PdfNumber>(itW);
                        info.widths.push_back(n ? (int)n->value : info.missingWidth);
                    }
                    info.hasWidths = true;
                }

                // Eger fontProgram yoksa, baseFont'a gore sistem fontunu yukle
                if (info.fontProgram.empty())
                {
                    std::string fontPath = resolveSystemFontPath(info.baseFont);

                    std::ifstream fontFile(fontPath, std::ios::binary);
                    if (fontFile) {
                        info.fontProgram.assign(
                            std::istreambuf_iterator<char>(fontFile),
                            std::istreambuf_iterator<char>()
                        );
                        LogDebug("  Font '%s': baseFont='%s' -> system '%s' (%zu bytes)",
                            info.resourceName.c_str(), info.baseFont.c_str(), fontPath.c_str(), info.fontProgram.size());
                    }
                }

                // ═══════════════════════════════════════════════════════════
                // WinAnsiEncoding ise standart glyph isimlerini codeToGlyphName'e doldur
                // CFF/Type1C fontlarda charmap olmayabilir, glyph name ile eşleştirmek şart
                // ═══════════════════════════════════════════════════════════
                {
                    static const char* winAnsiGlyphNames[256] = {
                        nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
                        nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
                        nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
                        nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
                        "space","exclam","quotedbl","numbersign","dollar","percent","ampersand","quotesingle",
                        "parenleft","parenright","asterisk","plus","comma","hyphen","period","slash",
                        "zero","one","two","three","four","five","six","seven",
                        "eight","nine","colon","semicolon","less","equal","greater","question",
                        "at","A","B","C","D","E","F","G",
                        "H","I","J","K","L","M","N","O",
                        "P","Q","R","S","T","U","V","W",
                        "X","Y","Z","bracketleft","backslash","bracketright","asciicircum","underscore",
                        "grave","a","b","c","d","e","f","g",
                        "h","i","j","k","l","m","n","o",
                        "p","q","r","s","t","u","v","w",
                        "x","y","z","braceleft","bar","braceright","asciitilde",nullptr,
                        "Euro",nullptr,"quotesinglbase","florin","quotedblbase","ellipsis","dagger","daggerdbl",
                        "circumflex","perthousand","Scaron","guilsinglleft","OE",nullptr,"Zcaron",nullptr,
                        nullptr,"quoteleft","quoteright","quotedblleft","quotedblright","bullet","endash","emdash",
                        "tilde","trademark","scaron","guilsinglright","oe",nullptr,"zcaron","Ydieresis",
                        "space","exclamdown","cent","sterling","currency","yen","brokenbar","section",
                        "dieresis","copyright","ordfeminine","guillemotleft","logicalnot","hyphen","registered","macron",
                        "degree","plusminus","twosuperior","threesuperior","acute","mu","paragraph","periodcentered",
                        "cedilla","onesuperior","ordmasculine","guillemotright","onequarter","onehalf","threequarters","questiondown",
                        "Agrave","Aacute","Acircumflex","Atilde","Adieresis","Aring","AE","Ccedilla",
                        "Egrave","Eacute","Ecircumflex","Edieresis","Igrave","Iacute","Icircumflex","Idieresis",
                        "Eth","Ntilde","Ograve","Oacute","Ocircumflex","Otilde","Odieresis","multiply",
                        "Oslash","Ugrave","Uacute","Ucircumflex","Udieresis","Yacute","Thorn","germandbls",
                        "agrave","aacute","acircumflex","atilde","adieresis","aring","ae","ccedilla",
                        "egrave","eacute","ecircumflex","edieresis","igrave","iacute","icircumflex","idieresis",
                        "eth","ntilde","ograve","oacute","ocircumflex","otilde","odieresis","divide",
                        "oslash","ugrave","uacute","ucircumflex","udieresis","yacute","thorn","ydieresis"
                    };
                    bool isWinAnsi = (info.encoding == "/WinAnsiEncoding" || info.encoding == "WinAnsiEncoding");
                    if (isWinAnsi || info.encoding.empty()) {
                        for (int code = 0; code < 256; code++) {
                            if (info.codeToGlyphName[code].empty() && winAnsiGlyphNames[code] != nullptr) {
                                info.codeToGlyphName[code] = winAnsiGlyphNames[code];
                            }
                        }
                    }
                }

                // MuPDF tarzi: Font programi varsa, codeToGid ve width tablosunu olustur
                if (!info.fontProgram.empty())
                {
                    FT_Face tempFace = nullptr;
                    FT_Error err = FT_New_Memory_Face(
                        g_ftLib,
                        info.fontProgram.data(),
                        (FT_Long)info.fontProgram.size(),
                        0,
                        &tempFace
                    );

                    if (err == 0 && tempFace)
                    {
                        FT_UShort units_per_EM = tempFace->units_per_EM;
                        if (units_per_EM == 0)
                            units_per_EM = 2048;

                        // ═══════════════════════════════════════════════════════════
                        // MuPDF tarzı: codeToGid tablosunu oluştur
                        // ═══════════════════════════════════════════════════════════

                        // 1. Doğru charmap'i seç
                        FT_CharMap bestCmap = nullptr;
                        for (int cm = 0; cm < tempFace->num_charmaps; cm++) {
                            if (tempFace->charmaps[cm]->platform_id == 3 &&
                                tempFace->charmaps[cm]->encoding_id == 1) {
                                bestCmap = tempFace->charmaps[cm];
                                break;
                            }
                        }
                        if (!bestCmap) {
                            for (int cm = 0; cm < tempFace->num_charmaps; cm++) {
                                if (tempFace->charmaps[cm]->platform_id == 1 &&
                                    tempFace->charmaps[cm]->encoding_id == 0) {
                                    bestCmap = tempFace->charmaps[cm];
                                    break;
                                }
                            }
                        }
                        if (!bestCmap && tempFace->num_charmaps > 0) {
                            bestCmap = tempFace->charmaps[0];
                        }
                        if (bestCmap) {
                            FT_Set_Charmap(tempFace, bestCmap);
                        }

                        // ═══════════════════════════════════════════════════════════
                        // CFF/Type1C fontlar için: FT_Get_Glyph_Name ile glyph isimlerini oku
                        // ve manuel olarak eşleştir (FT_Get_Name_Index CFF'te güvenilir değil)
                        // ═══════════════════════════════════════════════════════════

                        // Tüm glyph isimlerini oku ve nameToGid map oluştur
                        std::map<std::string, FT_UInt> nameToGid;
                        for (FT_UInt gidx = 0; gidx < (FT_UInt)tempFace->num_glyphs; gidx++) {
                            char glyphName[256] = { 0 };
                            if (FT_HAS_GLYPH_NAMES(tempFace)) {
                                if (FT_Get_Glyph_Name(tempFace, gidx, glyphName, 256) == 0 && glyphName[0] != 0) {
                                    nameToGid[glyphName] = gidx;
                                }
                            }
                        }

                        LogDebug("    Font has %d glyphs, nameToGid map has %zu entries",
                            tempFace->num_glyphs, nameToGid.size());

                        // 2. Önce codeToGlyphName'den eşleştir (en güvenilir)
                        for (int code = 0; code < 256; code++) {
                            if (!info.codeToGlyphName[code].empty()) {
                                auto it = nameToGid.find(info.codeToGlyphName[code]);
                                if (it != nameToGid.end()) {
                                    info.codeToGid[code] = (uint16_t)it->second;
                                }
                            }
                        }

                        // 3. Glyph name yoksa charmap'ten dene (fallback)
                        for (int code = 0; code < 256; code++) {
                            if (info.codeToGid[code] == 0) {
                                // Unicode'dan dene
                                if (info.codeToUnicode[code] != 0) {
                                    FT_UInt uniGid = FT_Get_Char_Index(tempFace, (FT_ULong)info.codeToUnicode[code]);
                                    if (uniGid > 0) {
                                        info.codeToGid[code] = (uint16_t)uniGid;
                                    }
                                }
                                // Hala 0 ise charcode'dan dene
                                if (info.codeToGid[code] == 0) {
                                    FT_UInt gi = FT_Get_Char_Index(tempFace, (FT_ULong)code);
                                    if (gi > 0) {
                                        info.codeToGid[code] = (uint16_t)gi;
                                    }
                                }
                            }
                        }

                        info.hasCodeToGid = true;
                        LogDebug("    Built codeToGid table for font '%s'", rn.c_str());

                        // ═══════════════════════════════════════════════════════════
                        // Width tablosunu oluştur (eğer /Widths yoksa)
                        // ═══════════════════════════════════════════════════════════
                        if (!info.hasWidths)
                        {
                            info.firstChar = 0;
                            info.widths.resize(256, info.missingWidth);

                            for (int code = 0; code < 256; code++)
                            {
                                FT_UInt gi = info.codeToGid[code];
                                if (gi != 0)
                                {
                                    FT_Fixed adv = 0;
                                    int mask = FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP | FT_LOAD_IGNORE_TRANSFORM;
                                    FT_Error advErr = FT_Get_Advance(tempFace, gi, mask, &adv);

                                    if (advErr == 0 && adv > 0)
                                    {
                                        int w = (int)(adv * 1000 / units_per_EM);
                                        if (w > 0)
                                            info.widths[code] = w;
                                    }
                                }
                            }

                            info.hasWidths = true;
                            LogDebug("    Extracted widths from FreeType for font '%s'", rn.c_str());
                        }

                        FT_Done_Face(tempFace);
                    }
                }
            }

            // CID fonts
            if (info.subtype == "/Type0")
            {
                info.isCidFont = true;

                std::set<int> vd;
                auto descObj = resolveIndirect(dictGetAny(fdict, "/DescendantFonts", "DescendantFonts"), vd);
                auto descArr = std::dynamic_pointer_cast<PdfArray>(descObj);

                std::shared_ptr<PdfDictionary> cidFontDict;
                if (descArr && !descArr->items.empty())
                {
                    vd.clear();
                    auto cidObj = resolveIndirect(descArr->items[0], vd);
                    cidFontDict = std::dynamic_pointer_cast<PdfDictionary>(cidObj);
                }

                if (cidFontDict)
                {
                    // /DW - CID default width
                    if (auto dw = std::dynamic_pointer_cast<PdfNumber>(dictGetAny(cidFontDict, "/DW", "DW"))) {
                        info.cidDefaultWidth = (int)dw->value;
                        info.missingWidth = (int)dw->value;
                    }

                    // /W - CID width array (same parsing as main font path)
                    if (auto wArr = std::dynamic_pointer_cast<PdfArray>(dictGetAny(cidFontDict, "/W", "W")))
                    {
                        size_t idx = 0;
                        while (idx < wArr->items.size())
                        {
                            auto cidStart = std::dynamic_pointer_cast<PdfNumber>(wArr->items[idx]);
                            if (!cidStart) { idx++; continue; }

                            int startCid = (int)cidStart->value;
                            idx++;

                            if (idx >= wArr->items.size()) break;

                            if (auto widthArr = std::dynamic_pointer_cast<PdfArray>(wArr->items[idx]))
                            {
                                // Format: cid [w1 w2 w3 ...]
                                int cid = startCid;
                                for (auto& wItem : widthArr->items)
                                {
                                    if (auto wNum = std::dynamic_pointer_cast<PdfNumber>(wItem))
                                        info.cidWidths[(uint16_t)cid] = (int)wNum->value;
                                    cid++;
                                }
                                idx++;
                            }
                            else if (auto cidEnd = std::dynamic_pointer_cast<PdfNumber>(wArr->items[idx]))
                            {
                                // Format: cid1 cid2 w (range)
                                int endCid = (int)cidEnd->value;
                                idx++;

                                if (idx < wArr->items.size())
                                {
                                    if (auto wNum = std::dynamic_pointer_cast<PdfNumber>(wArr->items[idx]))
                                    {
                                        int w = (int)wNum->value;
                                        for (int c = startCid; c <= endCid; c++)
                                            info.cidWidths[(uint16_t)c] = w;
                                        idx++;
                                    }
                                }
                            }
                        }
                        LogDebug("    XObj Font '%s': /W array parsed, %zu entries",
                            info.resourceName.c_str(), info.cidWidths.size());
                    }

                    // CIDToGIDMap
                    std::set<int> vis;
                    auto mapObj = resolveIndirect(dictGetAny(cidFontDict, "/CIDToGIDMap", "CIDToGIDMap"), vis);

                    info.hasCidToGidMap = false;
                    info.cidToGidIdentity = true;
                    info.cidToGid.clear();

                    if (auto nm = std::dynamic_pointer_cast<PdfName>(mapObj))
                    {
                        if (nm->value == "/Identity" || nm->value == "Identity")
                        {
                            info.hasCidToGidMap = true;
                            info.cidToGidIdentity = true;
                        }
                    }
                    else if (auto st = std::dynamic_pointer_cast<PdfStream>(mapObj))
                    {
                        std::vector<uint8_t> bytes;
                        if (decodeStream(st, bytes))
                        {
                            info.cidToGid.resize(bytes.size() / 2);
                            for (size_t i = 0; i + 1 < bytes.size(); i += 2)
                                info.cidToGid[i / 2] =
                                (uint16_t(bytes[i]) << 8) | uint16_t(bytes[i + 1]);

                            info.hasCidToGidMap = true;
                            info.cidToGidIdentity = false;
                        }
                    }
                }
            }

            // ================================================================
            // CID font icin: cidWidths bossa, FreeType'tan width hesapla
            // Embedded font veya sistem fontu olabilir
            // ================================================================
            {
                static FILE* cwDbg = nullptr; // fopen("C:\\temp\\cidwidth_debug.txt", "a");
                if (cwDbg) {
                    fprintf(cwDbg, "CID width check: font='%s' isCidFont=%d, cidWidths.empty=%d, cidToUnicode.size=%zu, fontProgram.size=%zu\n",
                        info.resourceName.c_str(),
                        info.isCidFont ? 1 : 0, info.cidWidths.empty() ? 1 : 0,
                        info.cidToUnicode.size(), info.fontProgram.size());
                    fflush(cwDbg);
                }
            }

            if (info.isCidFont && !info.fontProgram.empty())
            {

                FT_Face widthFace = nullptr;
                bool needCleanup = false;

                if (!info.fontProgram.empty())
                {
                    static FILE* cwDbg = nullptr; // fopen("C:\\temp\\cidwidth_debug.txt", "a");
                    if (cwDbg) {
                        fprintf(cwDbg, "  -> Using embedded font (size=%zu)\n", info.fontProgram.size());
                        fflush(cwDbg);
                    }
                    // Embedded font
                    FT_Error err = FT_New_Memory_Face(
                        g_ftLib,
                        info.fontProgram.data(),
                        (FT_Long)info.fontProgram.size(),
                        0,
                        &widthFace
                    );
                    {
                        static FILE* cwDbg = nullptr; // fopen("C:\\temp\\cidwidth_debug.txt", "a");
                        if (cwDbg) {
                            fprintf(cwDbg, "  -> FT_New_Memory_Face result: err=%d, widthFace=%p\n", (int)err, (void*)widthFace);
                            fflush(cwDbg);
                        }
                    }
                    if (err != 0) widthFace = nullptr;
                    needCleanup = true;
                }
                else
                {
                    // Sistem fontu - baseFont'tan font dosyasini bul
                    LogDebug("    -> Using system font, baseFont='%s'", info.baseFont.c_str());

                    // Font adini Windows font dosyasina esle
                    std::string pathA = resolveSystemFontPath(info.baseFont);

                    LogDebug("    -> Loading system font from: %s", pathA.c_str());
                    FT_Error err = FT_New_Face(g_ftLib, pathA.c_str(), 0, &widthFace);
                    LogDebug("    -> FT_New_Face result: err=%d, widthFace=%p", (int)err, (void*)widthFace);
                    if (err != 0) widthFace = nullptr;
                    needCleanup = true;
                }

                {
                    static FILE* cwDbg = nullptr; // fopen("C:\\temp\\cidwidth_debug.txt", "a");
                    if (cwDbg) {
                        fprintf(cwDbg, "  -> widthFace after all loads: %p\n", (void*)widthFace);
                        fflush(cwDbg);
                    }
                }
                if (widthFace)
                {
                    FT_UShort unitsPerEM = widthFace->units_per_EM;
                    {
                        static FILE* cwDbg = nullptr; // fopen("C:\\temp\\cidwidth_debug.txt", "a");
                        if (cwDbg) {
                            fprintf(cwDbg, "  -> unitsPerEM=%d, num_charmaps=%d, cidToUnicode.size=%zu\n",
                                (int)unitsPerEM, widthFace->num_charmaps, info.cidToUnicode.size());
                            fflush(cwDbg);
                        }
                    }
                    if (unitsPerEM == 0) unitsPerEM = 1000;

                    // Unicode charmap sec
                    for (int cm = 0; cm < widthFace->num_charmaps; cm++) {
                        if (widthFace->charmaps[cm]->platform_id == 3 &&
                            widthFace->charmaps[cm]->encoding_id == 1) {
                            FT_Set_Charmap(widthFace, widthFace->charmaps[cm]);
                            break;
                        }
                    }

                    // CIDToGIDMap-based glyph ID lookup (same as main font path)
                    auto getGidForCid = [&](uint16_t cid) -> FT_UInt {
                        if (info.cidToGidIdentity) {
                            return (FT_UInt)cid;
                        }
                        else if (!info.cidToGid.empty() && cid < info.cidToGid.size()) {
                            return (FT_UInt)info.cidToGid[cid];
                        }
                        // Fallback: cidToUnicode -> unicode -> charmap GID
                        auto uniIt = info.cidToUnicode.find(cid);
                        if (uniIt != info.cidToUnicode.end()) {
                            return FT_Get_Char_Index(widthFace, (FT_ULong)uniIt->second);
                        }
                        return 0;
                    };

                    // Method 1: Fill widths for CIDs in cidToUnicode table
                    for (auto& kv : info.cidToUnicode)
                    {
                        uint16_t cid = kv.first;
                        // /W array values preserved, don't overwrite
                        if (info.cidWidths.count(cid)) continue;

                        FT_UInt gid = getGidForCid(cid);
                        if (gid == 0) {
                            // Try unicode lookup as fallback
                            gid = FT_Get_Char_Index(widthFace, (FT_ULong)kv.second);
                        }
                        if (gid == 0) continue;

                        FT_Fixed adv = 0;
                        FT_Error advErr = FT_Get_Advance(widthFace, gid,
                            FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING, &adv);

                        if (advErr == 0 && adv > 0)
                        {
                            int w = (int)((adv * 1000) / unitsPerEM);
                            if (w > 0)
                                info.cidWidths[cid] = w;
                        }
                    }

                    // Method 2: If CIDToGIDMap is Identity, scan all font glyphs
                    if (info.cidToGidIdentity)
                    {
                        for (FT_UInt gid = 1; gid < (FT_UInt)widthFace->num_glyphs && gid < 65535; gid++)
                        {
                            uint16_t cid = (uint16_t)gid;
                            if (info.cidWidths.count(cid)) continue;

                            FT_Fixed adv = 0;
                            FT_Error advErr = FT_Get_Advance(widthFace, gid,
                                FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING, &adv);

                            if (advErr == 0 && adv > 0)
                            {
                                int w = (int)((adv * 1000) / unitsPerEM);
                                if (w > 0)
                                    info.cidWidths[cid] = w;
                            }
                        }
                    }

                    if (needCleanup)
                        FT_Done_Face(widthFace);

                    LogDebug("  CID Font '%s' (XObj): cidWidths populated: %zu entries (from /W + FreeType)",
                        info.resourceName.c_str(), info.cidWidths.size());
                }
                else
                {
                    static FILE* cwDbg = nullptr; // fopen("C:\\temp\\cidwidth_debug.txt", "a");
                    if (cwDbg) {
                        fprintf(cwDbg, "  -> FAILED: widthFace is NULL\n");
                        fflush(cwDbg);
                    }
                }
            }

            // Map'e ekle
            fonts[info.resourceName] = info;

            // DEBUG: Dosyaya font detaylarini yaz
            {
                static FILE* fontDbg = nullptr; // fopen("C:\\temp\\font_load_debug.txt", "a");
                if (fontDbg) {
                    fprintf(fontDbg, "  ADDED: '%s' -> BaseFont='%s', Subtype='%s', Encoding='%s', isCidFont=%d\n",
                        info.resourceName.c_str(), info.baseFont.c_str(),
                        info.subtype.c_str(), info.encoding.c_str(), info.isCidFont ? 1 : 0);
                    fprintf(fontDbg, "    fontProgram.size=%zu, cidToUnicode.size=%zu\n",
                        info.fontProgram.size(), info.cidToUnicode.size());
                    fflush(fontDbg);
                }
            }
            LogDebug("    Font '%s' added to map", rn.c_str());
        }

        return true;
    }


    bool PdfDocument::decodeStream(
        const std::shared_ptr<PdfStream>& stream,
        std::vector<uint8_t>& outDecoded) const
    {
        outDecoded.clear();


        if (!stream || !stream->dict) return false;

        std::set<int> visited;

        // Filter ve DecodeParms al
        auto fObj = dictGetAny(stream->dict, "/Filter", "Filter");
        auto pObj = dictGetAny(stream->dict, "/DecodeParms", "DecodeParms");

        // Log dict entries for debugging
        LogDebug("decodeStream: dict has %zu entries, data=%zu bytes",
            stream->dict->entries.size(), stream->data.size());
        for (const auto& kv : stream->dict->entries) {
            LogDebug("  dict key='%s' type=%d", kv.first.c_str(), kv.second ? (int)kv.second->type() : -1);
        }
        LogDebug("decodeStream: fObj=%s", fObj ? "FOUND" : "NULL");

        // Indirect reference'ları resolve et
        fObj = resolveIndirect(fObj, visited);
        visited.clear();
        pObj = resolveIndirect(pObj, visited);

        LogDebug("decodeStream: after resolve fObj=%s type=%d",
            fObj ? "FOUND" : "NULL", fObj ? (int)fObj->type() : -1);

        // Filtre yok → raw
        if (!fObj)
        {
            LogDebug("decodeStream: NO FILTER - returning raw data");
            outDecoded = stream->data;
            return true;
        }

        std::vector<std::string> filters;
        std::vector<std::map<std::string, int>> params;

        // ================================================================
        // DecodeParms'ı parse etmek için yardımcı lambda
        // ================================================================
        auto parseDecodeParms = [&](const std::shared_ptr<PdfObject>& parmsObj) -> std::map<std::string, int>
            {
                std::map<std::string, int> mp;

                if (!parmsObj) return mp;

                std::set<int> v;
                auto resolved = resolveIndirect(parmsObj, v);

                auto d = std::dynamic_pointer_cast<PdfDictionary>(resolved);
                if (!d) return mp;

                // Tüm numeric değerleri oku
                for (auto& kv : d->entries)
                {
                    if (!kv.second) continue;

                    v.clear();
                    auto numObj = std::dynamic_pointer_cast<PdfNumber>(resolveIndirect(kv.second, v));
                    if (numObj)
                    {
                        // Key'i normalize et (hem "/Predictor" hem "Predictor" çalışsın)
                        std::string key = kv.first;
                        if (!key.empty() && key[0] == '/')
                            key = key.substr(1);

                        mp[key] = (int)numObj->value;

                        // Slash'lı versiyonu da ekle (bazı kodlar bunu bekliyor olabilir)
                        mp["/" + key] = (int)numObj->value;
                    }
                }

                return mp;
            };

        // ================================================================
        // Tek filtre durumu (Name)
        // ================================================================
        if (fObj->type() == PdfObjectType::Name)
        {
            auto nm = std::dynamic_pointer_cast<PdfName>(fObj);
            std::string filterName = nm ? nm->value : "";
            filters.push_back(filterName);
            LogDebug("decodeStream: Single filter = '%s'", filterName.c_str());

            // ÖNEMLİ DÜZELTME: DecodeParms'ı oku!
            params.push_back(parseDecodeParms(pObj));
        }
        // ================================================================
        // Çoklu filtre durumu (Array)
        // ================================================================
        else if (fObj->type() == PdfObjectType::Array)
        {
            auto arr = std::dynamic_pointer_cast<PdfArray>(fObj);
            auto parr = std::dynamic_pointer_cast<PdfArray>(pObj);

            if (arr)
            {
                LogDebug("decodeStream: Array of %zu filters", arr->items.size());
                for (size_t i = 0; i < arr->items.size(); i++)
                {
                    visited.clear();
                    auto n = std::dynamic_pointer_cast<PdfName>(resolveIndirect(arr->items[i], visited));
                    if (n) {
                        filters.push_back(n->value);
                        LogDebug("decodeStream:   filter[%zu] = '%s'", i, n->value.c_str());
                    }
                    else
                        filters.push_back("");

                    // DecodeParms array ise her filtre için ayrı params
                    if (parr && i < parr->items.size())
                    {
                        params.push_back(parseDecodeParms(parr->items[i]));
                    }
                    else if (!parr && pObj && i == 0)
                    {
                        // DecodeParms tek dictionary ise ilk filtre için kullan
                        params.push_back(parseDecodeParms(pObj));
                    }
                    else
                    {
                        params.push_back({});
                    }
                }
            }
        }
        else
        {
            // beklenmeyen tip → raw
            LogDebug("decodeStream: Unexpected filter type %d - returning raw", (int)fObj->type());
            outDecoded = stream->data;
            return true;
        }

        bool decodeResult = PdfFilters::Decode(stream->data, filters, params, outDecoded);
        LogDebug("decodeStream: PdfFilters::Decode returned %s, input=%zu bytes, output=%zu bytes",
            decodeResult ? "TRUE" : "FALSE", stream->data.size(), outDecoded.size());

        // If PdfFilters failed and data looks like zlib, try direct decompression
        if (!decodeResult && !stream->data.empty() &&
            filters.size() == 1 && filters[0] == "/FlateDecode")
        {
            LogDebug("decodeStream: PdfFilters failed, trying direct decompressFlate...");
            // Log first 16 bytes
            {
                std::string hex;
                for (size_t i = 0; i < 16 && i < stream->data.size(); i++) {
                    char buf[4]; snprintf(buf, sizeof(buf), "%02x ", stream->data[i]);
                    hex += buf;
                }
                LogDebug("decodeStream: first 16 bytes: %s", hex.c_str());
            }

            // Try direct zlib decompression with inflateInit2
            std::vector<uint8_t> directOutput;
            if (decompressFlate(stream->data, directOutput) && !directOutput.empty())
            {
                LogDebug("decodeStream: Direct decompressFlate SUCCEEDED! %zu bytes", directOutput.size());
                outDecoded = std::move(directOutput);
                return true;
            }
            else
            {
                LogDebug("decodeStream: Direct decompressFlate also FAILED");

                // Try inflateInit2 with MAX_WBITS+32 (auto detect zlib/gzip/raw)
                z_stream strm{};
                strm.next_in = (Bytef*)stream->data.data();
                strm.avail_in = (uInt)stream->data.size();

                int initRet = inflateInit2(&strm, 15 + 32);
                LogDebug("decodeStream: inflateInit2(15+32) returned %d", initRet);

                if (initRet == Z_OK)
                {
                    outDecoded.clear();
                    outDecoded.reserve(stream->data.size() * 4);
                    const size_t CHUNK = 4096;
                    uint8_t buffer[CHUNK];
                    int ret = Z_OK;
                    while (ret != Z_STREAM_END)
                    {
                        strm.next_out = buffer;
                        strm.avail_out = CHUNK;
                        ret = inflate(&strm, Z_NO_FLUSH);
                        LogDebug("decodeStream: inflate returned %d, produced %zu bytes",
                            ret, (size_t)(CHUNK - strm.avail_out));
                        if (ret != Z_OK && ret != Z_STREAM_END)
                        {
                            LogDebug("decodeStream: inflate error at total_in=%lu total_out=%lu",
                                strm.total_in, strm.total_out);
                            break;
                        }
                        size_t produced = CHUNK - strm.avail_out;
                        outDecoded.insert(outDecoded.end(), buffer, buffer + produced);
                    }
                    inflateEnd(&strm);
                    if (ret == Z_STREAM_END && !outDecoded.empty())
                    {
                        LogDebug("decodeStream: Manual inflate SUCCEEDED! %zu bytes", outDecoded.size());
                        return true;
                    }
                }
            }
        }

        return decodeResult;
    }

    bool PdfDocument::decodeImageXObject(
        const std::shared_ptr<PdfStream>& st,
        std::vector<uint8_t>& argb,
        int& w, int& h)
    {
        argb.clear();
        w = h = 0;
        if (!st || !st->dict) return false;

        auto dict = st->dict;
        std::set<int> v;

        // ================================================================
        // Width ve Height al - indirect reference'ları resolve et!
        // ================================================================
        auto wObj = dict->get("/Width");
        if (!wObj) wObj = dict->get("Width");
        v.clear();
        wObj = resolveIndirect(wObj, v);
        auto wNum = std::dynamic_pointer_cast<PdfNumber>(wObj);

        auto hObj = dict->get("/Height");
        if (!hObj) hObj = dict->get("Height");
        v.clear();
        hObj = resolveIndirect(hObj, v);
        auto hNum = std::dynamic_pointer_cast<PdfNumber>(hObj);

        if (!wNum || !hNum) return false;

        w = (int)wNum->value;
        h = (int)hNum->value;

        if (w <= 0 || h <= 0) return false;

        // ================================================================
        // Filter'ı analiz et
        // ================================================================
        auto fObj = dict->get("/Filter");
        if (!fObj) fObj = dict->get("Filter");
        v.clear();
        fObj = resolveIndirect(fObj, v);

        std::vector<std::string> filters;

        if (fObj)
        {
            if (auto fName = std::dynamic_pointer_cast<PdfName>(fObj))
            {
                filters.push_back(fName->value);
            }
            else if (auto fArr = std::dynamic_pointer_cast<PdfArray>(fObj))
            {
                for (auto& item : fArr->items)
                {
                    v.clear();
                    auto nm = std::dynamic_pointer_cast<PdfName>(resolveIndirect(item, v));
                    if (nm) filters.push_back(nm->value);
                }
            }
        }

        // ================================================================
        // JPEG (DCTDecode) kontrolü
        // ================================================================
        bool isDCT = false;
        bool isJPX = false;
        bool isCCITT = false;

        for (const auto& f : filters)
        {
            if (f == "/DCTDecode" || f == "DCTDecode") isDCT = true;
            if (f == "/JPXDecode" || f == "JPXDecode") isJPX = true;
            if (f == "/CCITTFaxDecode" || f == "CCITTFaxDecode") isCCITT = true;
        }

        // ================================================================
        // JPEG2000 (JPXDecode) işlemi - WIC ile decode
        // ================================================================
        if (isJPX)
        {
            // JPEG2000 uses WIC decoder
            int jpxWidth = 0, jpxHeight = 0;
            std::vector<uint8_t> jpxArgb;

            if (PdfFilters::JPEG2000Decode(st->data, jpxArgb, jpxWidth, jpxHeight))
            {
                w = jpxWidth;
                h = jpxHeight;
                argb = std::move(jpxArgb);
                return true;
            }
            else
            {
                // WIC decode failed
                return false;
            }
        }

        // ================================================================
        // CCITTFaxDecode icin ozel islem
        // ================================================================
        if (isCCITT)
        {
            // DecodeParms'tan parametreleri al
            auto dpObj = dict->get("/DecodeParms");
            if (!dpObj) dpObj = dict->get("DecodeParms");

            int k = 0;  // Default: Group 3 1D
            bool blackIs1 = false;
            bool endOfLine = false;
            bool encodedByteAlign = false;

            if (dpObj) {
                v.clear();
                auto dp = std::dynamic_pointer_cast<PdfDictionary>(resolveIndirect(dpObj, v));
                if (dp) {
                    // K parameter
                    auto kObj = dp->get("/K");
                    if (!kObj) kObj = dp->get("K");
                    if (auto kNum = std::dynamic_pointer_cast<PdfNumber>(kObj)) {
                        k = (int)kNum->value;
                    }

                    // BlackIs1 parameter
                    auto bi1Obj = dp->get("/BlackIs1");
                    if (!bi1Obj) bi1Obj = dp->get("BlackIs1");
                    if (auto bi1Boolean = std::dynamic_pointer_cast<PdfBoolean>(bi1Obj)) {
                        blackIs1 = bi1Boolean->value;
                    }

                    // EndOfLine parameter
                    auto eolObj = dp->get("/EndOfLine");
                    if (!eolObj) eolObj = dp->get("EndOfLine");
                    if (auto eolBoolean = std::dynamic_pointer_cast<PdfBoolean>(eolObj)) {
                        endOfLine = eolBoolean->value;
                    }

                    // EncodedByteAlign parameter
                    auto ebaObj = dp->get("/EncodedByteAlign");
                    if (!ebaObj) ebaObj = dp->get("EncodedByteAlign");
                    if (auto ebaBoolean = std::dynamic_pointer_cast<PdfBoolean>(ebaObj)) {
                        encodedByteAlign = ebaBoolean->value;
                    }
                }
            }

            // CCITT decode
            std::vector<uint8_t> ccittDecoded;
            if (PdfFilters::CCITTFaxDecode(st->data, ccittDecoded, w, h, k, blackIs1, endOfLine, encodedByteAlign))
            {
                // 1-bit decoded data to ARGB
                argb.resize((size_t)w * (size_t)h * 4);
                int rowBytes = (w + 7) / 8;

                for (int row = 0; row < h; row++) {
                    for (int col = 0; col < w; col++) {
                        int byteIdx = row * rowBytes + col / 8;
                        int bitIdx = 7 - (col % 8);

                        uint8_t val = 255;  // Default white
                        if (byteIdx < (int)ccittDecoded.size()) {
                            int bit = (ccittDecoded[byteIdx] >> bitIdx) & 1;
                            // bit==1 means black, bit==0 means white
                            val = bit ? 0 : 255;
                        }

                        int i = row * w + col;
                        argb[i * 4 + 0] = val;
                        argb[i * 4 + 1] = val;
                        argb[i * 4 + 2] = val;
                        argb[i * 4 + 3] = 255;
                    }
                }
                return true;
            }
            return false;
        }

        // ================================================================
        // JPEG için özel işlem
        // ================================================================
        if (isDCT)
        {
            bool jpegSuccess = false;

            if (filters.size() == 1)
            {
                // Sadece DCT - raw stream'i JPEG decoder'a gönder
                jpegSuccess = PdfFilters::JPEGDecode(st->data, argb, w, h);
            }
            else
            {
                // Birden fazla filtre - DCT'den önceki filtreleri uygula
                std::vector<uint8_t> preDecoded = st->data;

                for (size_t i = 0; i < filters.size(); i++)
                {
                    const std::string& f = filters[i];

                    if (f == "/DCTDecode" || f == "DCTDecode")
                    {
                        jpegSuccess = PdfFilters::JPEGDecode(preDecoded, argb, w, h);
                        break;
                    }

                    std::vector<uint8_t> temp;

                    if (f == "/FlateDecode" || f == "FlateDecode")
                    {
                        if (!PdfFilters::FlateDecode(preDecoded, temp))
                            return false;
                    }
                    else if (f == "/ASCII85Decode" || f == "ASCII85Decode")
                    {
                        PdfFilters::ASCII85Decode(preDecoded, temp);
                    }
                    else if (f == "/LZWDecode" || f == "LZWDecode")
                    {
                        PdfFilters::LZWDecode(preDecoded, temp);
                    }
                    else if (f == "/RunLengthDecode" || f == "RunLengthDecode")
                    {
                        PdfFilters::RunLengthDecode(preDecoded, temp);
                    }
                    else
                    {
                        temp = preDecoded;
                    }

                    preDecoded.swap(temp);
                }
            }

            if (!jpegSuccess)
                return false;

            // ✅ FIX: JPEG decode sonrası SMask işle!
            {
                auto smaskObj = dict->get("/SMask");
                if (!smaskObj) smaskObj = dict->get("SMask");
                std::set<int> smaskVisited;
                auto smaskStream = std::dynamic_pointer_cast<PdfStream>(resolveIndirect(smaskObj, smaskVisited));

                if (smaskStream && smaskStream->dict)
                {
                    int smW = 0, smH = 0;

                    auto smWObj = smaskStream->dict->get("/Width");
                    if (!smWObj) smWObj = smaskStream->dict->get("Width");
                    smaskVisited.clear();
                    if (auto smWNum = std::dynamic_pointer_cast<PdfNumber>(resolveIndirect(smWObj, smaskVisited)))
                        smW = (int)smWNum->value;

                    auto smHObj = smaskStream->dict->get("/Height");
                    if (!smHObj) smHObj = smaskStream->dict->get("Height");
                    smaskVisited.clear();
                    if (auto smHNum = std::dynamic_pointer_cast<PdfNumber>(resolveIndirect(smHObj, smaskVisited)))
                        smH = (int)smHNum->value;

                    // Check for /Decode array in SMask
                    bool invertAlpha = false;
                    auto decodeObj = smaskStream->dict->get("/Decode");
                    if (!decodeObj) decodeObj = smaskStream->dict->get("Decode");
                    if (decodeObj)
                    {
                        smaskVisited.clear();
                        auto decodeArr = std::dynamic_pointer_cast<PdfArray>(resolveIndirect(decodeObj, smaskVisited));
                        if (decodeArr && decodeArr->items.size() >= 2)
                        {
                            double d0 = 0.0, d1 = 1.0;
                            if (auto n0 = std::dynamic_pointer_cast<PdfNumber>(decodeArr->items[0]))
                                d0 = n0->value;
                            if (auto n1 = std::dynamic_pointer_cast<PdfNumber>(decodeArr->items[1]))
                                d1 = n1->value;

                            if (d0 > d1)
                                invertAlpha = true;
                        }
                    }

                    if (smW == w && smH == h)
                    {
                        std::vector<uint8_t> smaskDecoded;
                        if (decodeStream(smaskStream, smaskDecoded))
                        {
                            size_t pixels = (size_t)w * h;
                            if (smaskDecoded.size() >= pixels)
                            {
                                for (size_t i = 0; i < pixels; i++)
                                {
                                    uint8_t alphaVal = smaskDecoded[i];
                                    if (invertAlpha)
                                        alphaVal = 255 - alphaVal;
                                    argb[i * 4 + 3] = alphaVal;
                                }
                            }
                        }
                    }
                }
            }

            return true;
        }

        // ================================================================
        // Normal decode (non-JPEG)
        // ================================================================
        std::vector<uint8_t> decoded;
        if (!decodeStream(st, decoded))
            return false;

        // ================================================================
        // BitsPerComponent
        // ================================================================
        int bpc = 8;
        auto bpcObj = dict->get("/BitsPerComponent");
        if (!bpcObj) bpcObj = dict->get("BitsPerComponent");
        v.clear();
        if (auto bpcNum = std::dynamic_pointer_cast<PdfNumber>(resolveIndirect(bpcObj, v)))
            bpc = (int)bpcNum->value;

        // ImageMask kontrolü (1-bit mask)
        bool isImageMask = false;
        auto imObj = dict->get("/ImageMask");
        if (!imObj) imObj = dict->get("ImageMask");
        if (auto imBool = std::dynamic_pointer_cast<PdfBoolean>(imObj))
            isImageMask = imBool->value;

        if (isImageMask)
            bpc = 1;

        // ================================================================
        // ColorSpace analizi - ÖNEMLİ DÜZELTME!
        // ================================================================
        auto csObj = dict->get("/ColorSpace");
        if (!csObj) csObj = dict->get("ColorSpace");
        v.clear();
        csObj = resolveIndirect(csObj, v);

        std::string colorSpace;
        int comps = 1;  // default grayscale
        std::vector<uint8_t> palette;
        int paletteColors = 0;
        std::string baseColorSpace;

        if (isImageMask)
        {
            // Predictor varsa mutlaka width kullanılmalı
            auto dpObj = dict->get("/DecodeParms");
            if (!dpObj) dpObj = dict->get("DecodeParms");

            std::set<int> vdp;
            auto dp = std::dynamic_pointer_cast<PdfDictionary>(
                resolveIndirect(dpObj, vdp));

            if (dp)
            {
                int predictor = 1;
                if (auto p = std::dynamic_pointer_cast<PdfNumber>(dp->get("/Predictor")))
                    predictor = (int)p->value;

                if (predictor > 1)
                {
                    PdfFilters::ApplyPredictor(
                        predictor,
                        1,      // colors
                        1,      // bits per component
                        w,      // columns = image width
                        decoded
                    );
                }
            }
        }
        else if (auto csName = std::dynamic_pointer_cast<PdfName>(csObj))
        {
            // Basit ColorSpace: /DeviceRGB, /DeviceGray, /DeviceCMYK
            colorSpace = csName->value;
        }
        else if (auto csArr = std::dynamic_pointer_cast<PdfArray>(csObj))
        {
            // Array formatında ColorSpace: [/ICCBased stream] veya [/Indexed base max palette]
            if (!csArr->items.empty())
            {
                v.clear();
                auto first = std::dynamic_pointer_cast<PdfName>(resolveIndirect(csArr->items[0], v));
                if (first)
                {
                    colorSpace = first->value;

                    // ====================================================
                    // ICCBased ColorSpace - ÇOK ÖNEMLİ!
                    // ====================================================
                    if (colorSpace == "/ICCBased" || colorSpace == "ICCBased")
                    {
                        // ICCBased stream'den /N (component sayısı) al
                        if (csArr->items.size() >= 2)
                        {
                            v.clear();
                            auto iccStream = std::dynamic_pointer_cast<PdfStream>(
                                resolveIndirect(csArr->items[1], v));

                            if (iccStream && iccStream->dict)
                            {
                                auto nObj = iccStream->dict->get("/N");
                                if (!nObj) nObj = iccStream->dict->get("N");
                                v.clear();
                                auto nNum = std::dynamic_pointer_cast<PdfNumber>(
                                    resolveIndirect(nObj, v));

                                if (nNum)
                                {
                                    comps = (int)nNum->value;
                                }
                                else
                                {
                                    // /N bulunamadı, /Alternate'e bak
                                    auto altObj = iccStream->dict->get("/Alternate");
                                    if (!altObj) altObj = iccStream->dict->get("Alternate");
                                    v.clear();
                                    auto altName = std::dynamic_pointer_cast<PdfName>(
                                        resolveIndirect(altObj, v));

                                    if (altName)
                                    {
                                        if (altName->value == "/DeviceRGB" || altName->value == "DeviceRGB")
                                            comps = 3;
                                        else if (altName->value == "/DeviceCMYK" || altName->value == "DeviceCMYK")
                                            comps = 4;
                                        else
                                            comps = 1;
                                    }
                                    else
                                    {
                                        comps = 3; // varsayılan RGB
                                    }
                                }
                            }
                            else
                            {
                                comps = 3; // varsayılan RGB
                            }
                        }
                        else
                        {
                            comps = 3; // varsayılan RGB
                        }
                    }
                    // ====================================================
                    // Indexed ColorSpace
                    // ====================================================
                    else if ((colorSpace == "/Indexed" || colorSpace == "Indexed") && csArr->items.size() >= 4)
                    {
                        v.clear();
                        auto baseName = std::dynamic_pointer_cast<PdfName>(
                            resolveIndirect(csArr->items[1], v));
                        if (baseName)
                            baseColorSpace = baseName->value;

                        v.clear();
                        auto maxIdx = std::dynamic_pointer_cast<PdfNumber>(
                            resolveIndirect(csArr->items[2], v));
                        if (maxIdx)
                            paletteColors = (int)maxIdx->value + 1;

                        // Palette verisi
                        v.clear();
                        auto palObj = resolveIndirect(csArr->items[3], v);
                        if (auto palStr = std::dynamic_pointer_cast<PdfString>(palObj))
                        {
                            palette.assign(palStr->value.begin(), palStr->value.end());
                        }
                        else if (auto palStream = std::dynamic_pointer_cast<PdfStream>(palObj))
                        {
                            decodeStream(palStream, palette);
                        }
                    }
                }
            }
        }

        // Component sayısını belirle (ICCBased dışındaki durumlar için)
        if (colorSpace == "/DeviceRGB" || colorSpace == "DeviceRGB")
            comps = 3;
        else if (colorSpace == "/DeviceCMYK" || colorSpace == "DeviceCMYK")
            comps = 4;
        else if (colorSpace == "/DeviceGray" || colorSpace == "DeviceGray")
            comps = 1;
        else if (colorSpace == "/Indexed" || colorSpace == "Indexed")
            comps = 1; // index değerleri 1 byte
        // ICCBased için comps yukarıda ayarlandı, değiştirme!

        // ================================================================
        // Piksel dönüşümü
        // ================================================================
        argb.resize((size_t)w * (size_t)h * 4);

        // Indexed ColorSpace
        if (colorSpace == "/Indexed" || colorSpace == "Indexed")
        {
            int baseComps = 3;
            if (baseColorSpace == "/DeviceGray" || baseColorSpace == "DeviceGray")
                baseComps = 1;
            else if (baseColorSpace == "/DeviceCMYK" || baseColorSpace == "DeviceCMYK")
                baseComps = 4;

            for (int i = 0; i < w * h; i++)
            {
                int idx = 0;
                if (bpc == 8)
                {
                    idx = (i < (int)decoded.size()) ? decoded[i] : 0;
                }
                else if (bpc == 4)
                {
                    int byteIdx = i / 2;
                    if (byteIdx < (int)decoded.size())
                    {
                        if (i % 2 == 0)
                            idx = (decoded[byteIdx] >> 4) & 0x0F;
                        else
                            idx = decoded[byteIdx] & 0x0F;
                    }
                }
                else if (bpc == 1)
                {
                    int byteIdx = i / 8;
                    int bitIdx = 7 - (i % 8);
                    if (byteIdx < (int)decoded.size())
                        idx = (decoded[byteIdx] >> bitIdx) & 1;
                }

                uint8_t r = 0, g = 0, b = 0;

                if (idx < paletteColors && (size_t)(idx * baseComps + baseComps - 1) < palette.size())
                {
                    if (baseComps == 1)
                    {
                        r = g = b = palette[idx];
                    }
                    else if (baseComps == 3)
                    {
                        r = palette[idx * 3 + 0];
                        g = palette[idx * 3 + 1];
                        b = palette[idx * 3 + 2];
                    }
                    else if (baseComps == 4)
                    {
                        double c = palette[idx * 4 + 0] / 255.0;
                        double m = palette[idx * 4 + 1] / 255.0;
                        double y = palette[idx * 4 + 2] / 255.0;
                        double k = palette[idx * 4 + 3] / 255.0;
                        r = (uint8_t)std::clamp((int)((1.0 - c) * (1.0 - k) * 255), 0, 255);
                        g = (uint8_t)std::clamp((int)((1.0 - m) * (1.0 - k) * 255), 0, 255);
                        b = (uint8_t)std::clamp((int)((1.0 - y) * (1.0 - k) * 255), 0, 255);
                    }
                }

                argb[i * 4 + 0] = r;
                argb[i * 4 + 1] = g;
                argb[i * 4 + 2] = b;
                argb[i * 4 + 3] = 255;
            }
        }
        // 1-bit görüntü (ImageMask veya 1bpc grayscale)
        else if (bpc == 1)
        {
            int rowBytes = (w + 7) / 8;

            for (int row = 0; row < h; row++)
            {
                for (int col = 0; col < w; col++)
                {
                    int byteIdx = row * rowBytes + col / 8;
                    int bitIdx = 7 - (col % 8);

                    uint8_t val = 255;
                    if (byteIdx < (int)decoded.size())
                    {
                        int bit = (decoded[byteIdx] >> bitIdx) & 1;
                        val = isImageMask ? (bit ? 0 : 255) : (bit ? 255 : 0);
                    }

                    int i = row * w + col;
                    argb[i * 4 + 0] = val;
                    argb[i * 4 + 1] = val;
                    argb[i * 4 + 2] = val;
                    argb[i * 4 + 3] = 255;
                }
            }
        }
        // Normal 8-bit görüntü
        else
        {
            size_t src = 0;
            for (int i = 0; i < w * h; i++)
            {
                uint8_t r = 0, g = 0, b = 0;

                if (comps == 1)
                {
                    uint8_t val = (src < decoded.size()) ? decoded[src++] : 255;
                    r = g = b = val;
                }
                else if (comps == 3)
                {
                    r = (src < decoded.size()) ? decoded[src++] : 0;
                    g = (src < decoded.size()) ? decoded[src++] : 0;
                    b = (src < decoded.size()) ? decoded[src++] : 0;
                }
                else if (comps == 4)
                {
                    uint8_t c = (src < decoded.size()) ? decoded[src++] : 0;
                    uint8_t m = (src < decoded.size()) ? decoded[src++] : 0;
                    uint8_t y = (src < decoded.size()) ? decoded[src++] : 0;
                    uint8_t k = (src < decoded.size()) ? decoded[src++] : 0;

                    double cd = c / 255.0, md = m / 255.0, yd = y / 255.0, kd = k / 255.0;
                    double rr = (1.0 - cd) * (1.0 - kd);
                    double gg = (1.0 - md) * (1.0 - kd);
                    double bb = (1.0 - yd) * (1.0 - kd);

                    r = (uint8_t)std::clamp((int)std::lround(rr * 255.0), 0, 255);
                    g = (uint8_t)std::clamp((int)std::lround(gg * 255.0), 0, 255);
                    b = (uint8_t)std::clamp((int)std::lround(bb * 255.0), 0, 255);
                }

                argb[i * 4 + 0] = r;
                argb[i * 4 + 1] = g;
                argb[i * 4 + 2] = b;
                argb[i * 4 + 3] = 255;
            }
        }

        // ================================================================
        // SMask (Soft Mask / Alpha Channel) İŞLEME
        // ================================================================
        auto smaskObj = dict->get("/SMask");
        if (!smaskObj) smaskObj = dict->get("SMask");
        v.clear();
        auto smaskStream = std::dynamic_pointer_cast<PdfStream>(resolveIndirect(smaskObj, v));

        if (smaskStream && smaskStream->dict)
        {
            // SMask boyutlarını al
            int smW = 0, smH = 0;

            auto smWObj = smaskStream->dict->get("/Width");
            if (!smWObj) smWObj = smaskStream->dict->get("Width");
            v.clear();
            if (auto smWNum = std::dynamic_pointer_cast<PdfNumber>(resolveIndirect(smWObj, v)))
                smW = (int)smWNum->value;

            auto smHObj = smaskStream->dict->get("/Height");
            if (!smHObj) smHObj = smaskStream->dict->get("Height");
            v.clear();
            if (auto smHNum = std::dynamic_pointer_cast<PdfNumber>(resolveIndirect(smHObj, v)))
                smH = (int)smHNum->value;

            // ✅ FIX: Check for /Decode array in SMask
            // /Decode [1 0] means values should be inverted (1=transparent, 0=opaque)
            // /Decode [0 1] or no Decode means normal (0=transparent, 1=opaque)
            bool invertAlpha = false;
            auto decodeObj = smaskStream->dict->get("/Decode");
            if (!decodeObj) decodeObj = smaskStream->dict->get("Decode");
            if (decodeObj)
            {
                v.clear();
                auto decodeArr = std::dynamic_pointer_cast<PdfArray>(resolveIndirect(decodeObj, v));
                if (decodeArr && decodeArr->items.size() >= 2)
                {
                    double d0 = 0.0, d1 = 1.0;
                    if (auto n0 = std::dynamic_pointer_cast<PdfNumber>(decodeArr->items[0]))
                        d0 = n0->value;
                    if (auto n1 = std::dynamic_pointer_cast<PdfNumber>(decodeArr->items[1]))
                        d1 = n1->value;

                    // If Decode is [1 0], we need to invert alpha values
                    if (d0 > d1)
                    {
                        invertAlpha = true;
                        LogDebug("SMask has inverted Decode [%.1f %.1f] - will invert alpha", d0, d1);
                    }
                }
            }

            if (smW == w && smH == h)
            {
                // SMask stream'ini decode et
                std::vector<uint8_t> smaskDecoded;
                if (decodeStream(smaskStream, smaskDecoded))
                {
                    // SMask genelde grayscale (1 component per pixel)
                    // Her piksel için alpha değerini SMask'tan al
                    size_t pixels = (size_t)w * h;

                    if (smaskDecoded.size() >= pixels)
                    {
                        for (size_t i = 0; i < pixels; i++)
                        {
                            // SMask değeri alpha olarak kullanılır
                            uint8_t alphaVal = smaskDecoded[i];

                            // ✅ FIX: Invert if Decode [1 0]
                            if (invertAlpha)
                                alphaVal = 255 - alphaVal;

                            argb[i * 4 + 3] = alphaVal;
                        }
                    }
                }
            }
        }

        return true;
    }

    bool PdfDocument::prepareFreeTypeFont(PdfFontInfo& fi)
    {
        if (fi.fontProgram.empty())
            return false;

        if (fi.ftReady)
            return true;

        // 🚀 USE FONT CACHE - Same font = same FT_Face
        fi.fontHash = FontCache::instance().getFontHash(fi.fontProgram);
        fi.ftFace = FontCache::instance().getOrCreate(g_ftLib, fi.fontProgram);

        if (!fi.ftFace)
            return false;

        fi.ftReady = true;
        return true;
    }


    bool PdfDocument::loadFallbackFont(PdfFontInfo& fi)
    {
        std::wstring path = L"C:\\Windows\\Fonts\\arial.ttf";

        std::ifstream f(path, std::ios::binary);
        if (!f) return false;

        fi.fontProgram.assign(
            std::istreambuf_iterator<char>(f),
            std::istreambuf_iterator<char>()
        );

        return prepareFreeTypeFont(fi);
    }



    bool PdfDocument::loadFromBytes(const std::vector<uint8_t>& data)
    {
        _data = data;
        _objects.clear();
        _xrefTable.clear();
        _trailer.reset();
        _root.reset();
        _pages.reset();

        if (_data.size() < 4)
            return false;

        // 1. Load XRef table first (for correct object positions with incremental updates)
        if (loadXRefTable())
        {
            LogDebug("PDF: XRef table loaded with %zu entries", _xrefTable.size());
        }
        else
        {
            LogDebug("PDF: XRef table not found or invalid, using linear scan only");
        }

        // 2. Linear scan parser (fallback for objects not in XRef)
        PdfParser parser(_data);
        if (!parser.parse())
            return false;

        _objects = parser.objects();

        // 2.5. Check for encryption and initialize decryption if needed
        LogDebug("PDF: Checking encryption, _trailer=%s", _trailer ? "YES" : "NULL");
        if (_trailer)
        {
            auto encryptRef = _trailer->get("/Encrypt");
            if (!encryptRef) encryptRef = _trailer->get("Encrypt");
            LogDebug("PDF: /Encrypt ref = %s", encryptRef ? "FOUND" : "NOT FOUND");
            if (encryptRef)
            {
                _isEncrypted = true;
                LogDebug("PDF: Document is ENCRYPTED - initializing decryption");
                if (initEncryption())
                {
                    // Certificate encryption: initCertEncryption sets _encryptionReady=false
                    // because we need to wait for supplySeed() call from C# side.
                    // Do NOT override _encryptionReady here for cert encryption.
                    if (_isCertEncrypted)
                    {
                        LogDebug("PDF: Certificate encryption detected - waiting for certificate/seed");
                        // Don't decrypt streams yet - need seed from C# RSA decrypt
                    }
                    else
                    {
                        _encryptionReady = true;

                        // Log encryption key hex
                        std::string keyHex;
                        for (auto b : _encryptKey) {
                            char buf[4]; snprintf(buf, sizeof(buf), "%02x", b);
                            keyHex += buf;
                        }
                        LogDebug("PDF: Encryption key computed: %s (%d bytes)", keyHex.c_str(), (int)_encryptKey.size());

                        // Decrypt all stream objects
                        int decryptCount = 0;
                        for (auto& kv : _objects)
                        {
                            auto stream = std::dynamic_pointer_cast<PdfStream>(kv.second);
                            if (stream)
                            {
                                size_t beforeSize = stream->data.size();
                                decryptStream(stream);
                                decryptCount++;
                                // Log first 4 bytes after decrypt for key streams
                                if (!stream->data.empty() && stream->data.size() >= 2)
                                {
                                    LogDebug("PDF: Decrypted obj %d: %zu bytes, first2=0x%02x%02x",
                                        kv.first, stream->data.size(),
                                        stream->data[0], stream->data[1]);
                                }
                            }
                        }
                        LogDebug("PDF: Decrypted %d streams total", decryptCount);
                    } // end else (password encryption)
                }
                else
                {
                    LogDebug("PDF: Encryption init incomplete - password may be required (V=%d, R=%d)", _encryptV, _encryptR);
                }
            }
        }

        // 3. XRef tablosundan eksik objeleri yükle (Linearized PDF desteği için kritik)
        if (!_xrefTable.empty())
        {
            LogDebug("PDF: Loading objects from XRef table...");
            int loadedFromXref = 0;
            for (const auto& kv : _xrefTable)
            {
                int objNum = kv.first;
                size_t offset = kv.second;

                // Eğer bu obje zaten linear scan ile yüklenmişse atla
                if (_objects.find(objNum) != _objects.end())
                    continue;

                // XRef'ten objeyi yükle
                auto obj = loadObjectAtOffset(offset);
                if (obj)
                {
                    // Decrypt stream if encryption is active
                    if (_encryptionReady)
                    {
                        auto stream = std::dynamic_pointer_cast<PdfStream>(obj);
                        if (stream) decryptStream(stream);
                    }
                    _objects[objNum] = obj;
                    loadedFromXref++;
                }
            }
            LogDebug("PDF: Loaded %d additional objects from XRef", loadedFromXref);
        }

        // 4. Object Stream (ObjStm) içindeki objeleri yükle (type 2 xref girdileri)
        if (!_objStmEntries.empty())
        {
            LogDebug("PDF: Loading %zu objects from Object Streams...", _objStmEntries.size());
            int loadedFromObjStm = 0;
            for (const auto& kv : _objStmEntries)
            {
                int objNum = kv.first;
                if (_objects.find(objNum) != _objects.end())
                    continue;

                auto obj = loadFromObjStm(objNum, kv.second.objStmNum, kv.second.indexInStream);
                if (obj)
                {
                    _objects[objNum] = obj;
                    loadedFromObjStm++;
                }
            }
            LogDebug("PDF: Loaded %d objects from Object Streams", loadedFromObjStm);
        }

        if (_objects.empty() && _xrefTable.empty())
            return false;

        // /Root (Catalog) bul
        for (const auto& kv : _objects)
        {
            auto dict = std::dynamic_pointer_cast<PdfDictionary>(kv.second);
            if (!dict) continue;

            std::set<int> v;
            auto typeObj = resolveIndirect(dict->get("/Type"), v);
            auto typeName = std::dynamic_pointer_cast<PdfName>(typeObj);
            if (!typeName) continue;

            if (typeName->value == "/Catalog" || typeName->value == "Catalog")
            {
                _root = dict;
                break;
            }
        }

        // /Pages bul
        if (_root)
        {
            std::set<int> v;
            auto pagesObj = resolveIndirect(_root->get("/Pages"), v);
            _pages = std::dynamic_pointer_cast<PdfDictionary>(pagesObj);
        }

        if (!_pages)
        {
            // Yine de fail yapmıyoruz; fallback olarak tüm objeleri tarayıp sayfa bulacağız
            for (const auto& kv : _objects)
            {
                auto dict = std::dynamic_pointer_cast<PdfDictionary>(kv.second);
                if (!dict) continue;

                std::set<int> v;
                auto typeObj = resolveIndirect(dict->get("/Type"), v);
                auto typeName = std::dynamic_pointer_cast<PdfName>(typeObj);
                if (!typeName) continue;

                if (typeName->value == "/Pages" || typeName->value == "Pages")
                {
                    _pages = dict;
                    break;
                }
            }
        }

        // Burada _pages bulamasak bile SUCCESS kabul ediyoruz
        // Sayfa sayımı ve boyut hesapları fallback ile çalışacak.
        return true;
    }

    // =====================================================
    // XRef Table Parsing - Incremental Updates Destekli
    // =====================================================

    // Helper: Find last occurrence of a string in data
    static size_t rfind_string(const std::vector<uint8_t>& data, const char* str)
    {
        size_t len = strlen(str);
        if (data.size() < len) return std::string::npos;

        for (size_t i = data.size() - len; i > 0; --i)
        {
            bool match = true;
            for (size_t j = 0; j < len && match; ++j)
            {
                if (data[i + j] != (uint8_t)str[j])
                    match = false;
            }
            if (match) return i;
        }
        return std::string::npos;
    }

    // Helper: Skip whitespace
    static size_t skipWhitespaceXRef(const std::vector<uint8_t>& data, size_t pos)
    {
        while (pos < data.size() &&
            (data[pos] == ' ' || data[pos] == '\t' ||
                data[pos] == '\r' || data[pos] == '\n'))
        {
            ++pos;
        }
        return pos;
    }

    // Helper: Read integer from data
    static size_t readIntegerXRef(const std::vector<uint8_t>& data, size_t pos, int64_t& value)
    {
        pos = skipWhitespaceXRef(data, pos);
        value = 0;
        bool negative = false;

        if (pos < data.size() && data[pos] == '-')
        {
            negative = true;
            ++pos;
        }
        else if (pos < data.size() && data[pos] == '+')
        {
            ++pos;
        }

        size_t start = pos;
        while (pos < data.size() && data[pos] >= '0' && data[pos] <= '9')
        {
            value = value * 10 + (data[pos] - '0');
            ++pos;
        }

        if (pos == start) return std::string::npos; // No digits found

        if (negative) value = -value;
        return pos;
    }

    // Parse traditional XRef table at given offset
    bool PdfDocument::parseXRefTableAt(size_t offset, std::map<int, size_t>& xrefEntries)
    {
        if (offset >= _data.size()) return false;

        size_t pos = offset;

        // Check for "xref" keyword
        if (pos + 4 > _data.size()) return false;
        if (_data[pos] != 'x' || _data[pos + 1] != 'r' ||
            _data[pos + 2] != 'e' || _data[pos + 3] != 'f')
        {
            return false; // Not a traditional xref table
        }
        pos += 4;

        // Parse subsections
        while (pos < _data.size())
        {
            pos = skipWhitespaceXRef(_data, pos);

            // Check for "trailer" keyword
            if (pos + 7 <= _data.size() &&
                _data[pos] == 't' && _data[pos + 1] == 'r' && _data[pos + 2] == 'a' &&
                _data[pos + 3] == 'i' && _data[pos + 4] == 'l' && _data[pos + 5] == 'e' &&
                _data[pos + 6] == 'r')
            {
                break; // End of xref table
            }

            // Read first object number and count
            int64_t firstObj, count;
            pos = readIntegerXRef(_data, pos, firstObj);
            if (pos == std::string::npos) break;

            pos = readIntegerXRef(_data, pos, count);
            if (pos == std::string::npos) break;

            // Read entries
            for (int64_t i = 0; i < count && pos < _data.size(); ++i)
            {
                pos = skipWhitespaceXRef(_data, pos);

                // Each entry: offset (10 digits) generation (5 digits) flag (n/f)
                int64_t entryOffset, generation;
                pos = readIntegerXRef(_data, pos, entryOffset);
                if (pos == std::string::npos) break;

                pos = readIntegerXRef(_data, pos, generation);
                if (pos == std::string::npos) break;

                pos = skipWhitespaceXRef(_data, pos);
                if (pos >= _data.size()) break;

                char flag = (char)_data[pos];
                ++pos;

                int objNum = (int)(firstObj + i);

                // 'n' = in use, 'f' = free
                if (flag == 'n' && entryOffset > 0)
                {
                    // Only add if not already present (newer entries take precedence)
                    if (xrefEntries.find(objNum) == xrefEntries.end())
                    {
                        xrefEntries[objNum] = (size_t)entryOffset;
                    }
                }
            }
        }

        return true;
    }

    // Parse XRef stream at given offset (PDF 1.5+)
    bool PdfDocument::parseXRefStreamAt(size_t offset, std::map<int, size_t>& xrefEntries)
    {
        // XRef stream is an object: "N 0 obj << ... >> stream ... endstream endobj"
        if (offset >= _data.size()) return false;

        // Parse the object
        PdfParser parser(_data);
        auto obj = parser.parseObjectAt(offset);
        if (!obj) return false;

        auto stream = std::dynamic_pointer_cast<PdfStream>(obj);
        if (!stream || !stream->dict) return false;

        // Check /Type /XRef
        auto typeObj = stream->dict->get("/Type");
        if (!typeObj) typeObj = stream->dict->get("Type");
        auto typeName = std::dynamic_pointer_cast<PdfName>(typeObj);
        if (!typeName || (typeName->value != "/XRef" && typeName->value != "XRef"))
            return false;

        // Get /Size
        auto sizeObj = stream->dict->get("/Size");
        if (!sizeObj) sizeObj = stream->dict->get("Size");
        auto sizeNum = std::dynamic_pointer_cast<PdfNumber>(sizeObj);
        if (!sizeNum) return false;
        int xrefSize = (int)sizeNum->value;

        // Get /W array (field widths)
        auto wObj = stream->dict->get("/W");
        if (!wObj) wObj = stream->dict->get("W");
        auto wArr = std::dynamic_pointer_cast<PdfArray>(wObj);
        if (!wArr || wArr->items.size() < 3) return false;

        int w1 = 0, w2 = 0, w3 = 0;
        if (auto n = std::dynamic_pointer_cast<PdfNumber>(wArr->items[0])) w1 = (int)n->value;
        if (auto n = std::dynamic_pointer_cast<PdfNumber>(wArr->items[1])) w2 = (int)n->value;
        if (auto n = std::dynamic_pointer_cast<PdfNumber>(wArr->items[2])) w3 = (int)n->value;

        int entrySize = w1 + w2 + w3;
        if (entrySize == 0) return false;

        // Get /Index array (optional, defaults to [0 Size])
        std::vector<std::pair<int, int>> subsections;
        auto indexObj = stream->dict->get("/Index");
        if (!indexObj) indexObj = stream->dict->get("Index");
        auto indexArr = std::dynamic_pointer_cast<PdfArray>(indexObj);

        if (indexArr && indexArr->items.size() >= 2)
        {
            for (size_t i = 0; i + 1 < indexArr->items.size(); i += 2)
            {
                auto startNum = std::dynamic_pointer_cast<PdfNumber>(indexArr->items[i]);
                auto countNum = std::dynamic_pointer_cast<PdfNumber>(indexArr->items[i + 1]);
                if (startNum && countNum)
                {
                    subsections.push_back({ (int)startNum->value, (int)countNum->value });
                }
            }
        }
        else
        {
            subsections.push_back({ 0, xrefSize });
        }

        // Decode stream data
        std::vector<uint8_t> streamData;
        if (!decodeStream(stream, streamData) || streamData.empty())
            return false;
        // Continue with parsed streamData

        // Parse entries
        size_t dataPos = 0;
        for (const auto& subsec : subsections)
        {
            int firstObj = subsec.first;
            int cnt = subsec.second;

            for (int i = 0; i < cnt && dataPos + entrySize <= streamData.size(); ++i)
            {
                // Read field 1 (type, default 1)
                uint64_t type = (w1 == 0) ? 1 : 0;
                for (int j = 0; j < w1; ++j)
                {
                    type = (type << 8) | streamData[dataPos++];
                }

                // Read field 2 (offset or object number)
                uint64_t field2 = 0;
                for (int j = 0; j < w2; ++j)
                {
                    field2 = (field2 << 8) | streamData[dataPos++];
                }

                // Read field 3 (generation or index)
                uint64_t field3 = 0;
                for (int j = 0; j < w3; ++j)
                {
                    field3 = (field3 << 8) | streamData[dataPos++];
                }

                int objNum = firstObj + i;

                if (type == 1) // In-use object
                {
                    // field2 = offset, field3 = generation
                    if (xrefEntries.find(objNum) == xrefEntries.end())
                    {
                        xrefEntries[objNum] = (size_t)field2;
                    }
                }
                else if (type == 2) // Compressed object (in object stream)
                {
                    // field2 = object stream number, field3 = index within stream
                    if (_objStmEntries.find(objNum) == _objStmEntries.end())
                    {
                        _objStmEntries[objNum] = { (int)field2, (int)field3 };
                    }
                }
                // type 0 = free
            }
        }

        // Store trailer info from stream dict
        _trailer = stream->dict;

        return true;
    }

    // Parse trailer dictionary
    std::shared_ptr<PdfDictionary> PdfDocument::parseTrailerAt(size_t xrefOffset)
    {
        if (xrefOffset >= _data.size()) return nullptr;

        // Find "trailer" after xref table
        size_t pos = xrefOffset;

        // Skip past xref table to find trailer
        while (pos < _data.size() - 7)
        {
            if (_data[pos] == 't' && _data[pos + 1] == 'r' && _data[pos + 2] == 'a' &&
                _data[pos + 3] == 'i' && _data[pos + 4] == 'l' && _data[pos + 5] == 'e' &&
                _data[pos + 6] == 'r')
            {
                pos += 7;
                pos = skipWhitespaceXRef(_data, pos);

                // Parse trailer dictionary
                if (pos < _data.size() && _data[pos] == '<' && pos + 1 < _data.size() && _data[pos + 1] == '<')
                {
                    PdfParser parser(_data);
                    auto obj = parser.parseObjectAt(pos);
                    return std::dynamic_pointer_cast<PdfDictionary>(obj);
                }
                break;
            }
            ++pos;
        }

        return nullptr;
    }

    bool PdfDocument::loadXRefTable()
    {
        _xrefTable.clear();
        _trailer.reset();

        // 1. Find "startxref" from end of file
        size_t startxrefPos = rfind_string(_data, "startxref");
        if (startxrefPos == std::string::npos)
        {
            LogDebug("XRef: startxref not found");
            return false;
        }

        // 2. Read the offset after "startxref"
        size_t pos = startxrefPos + 9; // strlen("startxref")
        int64_t xrefOffset;
        pos = readIntegerXRef(_data, pos, xrefOffset);
        if (pos == std::string::npos || xrefOffset < 0)
        {
            LogDebug("XRef: Invalid startxref offset");
            return false;
        }

        LogDebug("XRef: startxref points to offset %lld", (long long)xrefOffset);

        // 3. Process XRef chain (handle incremental updates via /Prev)
        std::set<size_t> visitedOffsets; // Prevent infinite loops
        std::map<int, size_t> allEntries;

        while (xrefOffset > 0 && xrefOffset < (int64_t)_data.size())
        {
            if (visitedOffsets.count((size_t)xrefOffset))
            {
                LogDebug("XRef: Circular reference detected at offset %lld", (long long)xrefOffset);
                break;
            }
            visitedOffsets.insert((size_t)xrefOffset);

            // Check if it's traditional xref or xref stream
            size_t checkPos = (size_t)xrefOffset;
            checkPos = skipWhitespaceXRef(_data, checkPos);

            std::shared_ptr<PdfDictionary> currentTrailer;

            if (checkPos + 4 <= _data.size() &&
                _data[checkPos] == 'x' && _data[checkPos + 1] == 'r' &&
                _data[checkPos + 2] == 'e' && _data[checkPos + 3] == 'f')
            {
                // Traditional xref table
                LogDebug("XRef: Parsing traditional xref at %lld", (long long)xrefOffset);

                std::map<int, size_t> entries;
                if (parseXRefTableAt(checkPos, entries))
                {
                    // Merge entries (newer entries take precedence)
                    for (const auto& kv : entries)
                    {
                        if (allEntries.find(kv.first) == allEntries.end())
                        {
                            allEntries[kv.first] = kv.second;
                        }
                    }
                }

                // Parse trailer
                currentTrailer = parseTrailerAt(checkPos);
            }
            else
            {
                // XRef stream (PDF 1.5+)
                LogDebug("XRef: Parsing xref stream at %lld", (long long)xrefOffset);

                std::map<int, size_t> entries;
                if (parseXRefStreamAt(checkPos, entries))
                {
                    for (const auto& kv : entries)
                    {
                        if (allEntries.find(kv.first) == allEntries.end())
                        {
                            allEntries[kv.first] = kv.second;
                        }
                    }
                    currentTrailer = _trailer; // parseXRefStreamAt sets _trailer
                }
            }

            // Store first (most recent) trailer
            if (!_trailer && currentTrailer)
            {
                _trailer = currentTrailer;
            }

            // Follow /Prev chain for incremental updates
            xrefOffset = -1;
            if (currentTrailer)
            {
                auto prevObj = currentTrailer->get("/Prev");
                if (!prevObj) prevObj = currentTrailer->get("Prev");
                auto prevNum = std::dynamic_pointer_cast<PdfNumber>(prevObj);
                if (prevNum)
                {
                    xrefOffset = (int64_t)prevNum->value;
                    LogDebug("XRef: Following /Prev to offset %lld", (long long)xrefOffset);
                }
            }
        }

        // 4. Store results
        _xrefTable = std::move(allEntries);

        LogDebug("XRef: Loaded %zu entries", _xrefTable.size());

        return !_xrefTable.empty();
    }

    bool PdfDocument::loadTrailer()
    {
        // Trailer is loaded as part of loadXRefTable
        return _trailer != nullptr;
    }

    bool PdfDocument::loadRootAndPages() { return false; }
    int  PdfDocument::getPageCountByScan() const { return 0; }
    int PdfDocument::countPagesRecursive(
        const std::shared_ptr<PdfDictionary>& node,
        std::set<const PdfDictionary*>& visited) const
    {
        if (!node) return 0;
        if (visited.count(node.get())) return 0;
        visited.insert(node.get());

        std::set<int> v;
        auto typeObj = resolveIndirect(node->get("/Type"), v);
        if (!typeObj) { v.clear(); typeObj = resolveIndirect(node->get("Type"), v); }
        auto typeName = std::dynamic_pointer_cast<PdfName>(typeObj);
        std::string t = typeName ? typeName->value : "";

        // Yaprak düğüm: /Page
        if (t == "/Page" || t == "Page")
            return isPageObject(node) ? 1 : 0;

        // Ara düğüm: /Pages
        if (t == "/Pages" || t == "Pages")
        {
            v.clear();
            auto kidsObj = resolveIndirect(node->get("/Kids"), v);
            auto kidsArr = std::dynamic_pointer_cast<PdfArray>(kidsObj);
            if (!kidsArr) return 0;

            int total = 0;
            for (auto& item : kidsArr->items)
            {
                v.clear();
                auto childDict = std::dynamic_pointer_cast<PdfDictionary>(
                    resolveIndirect(item, v));
                if (childDict)
                    total += countPagesRecursive(childDict, visited);
            }
            return total;
        }

        return 0;
    }

    // =====================================================
    // Indirect Referans Çözücü
    // =====================================================

    std::shared_ptr<PdfObject> PdfDocument::resolveIndirect(
        const std::shared_ptr<PdfObject>& obj,
        std::set<int>& visitedIds
    ) const
    {
        if (!obj) return nullptr;

        if (obj->type() != PdfObjectType::IndirectRef)
            return obj;

        auto ref = std::dynamic_pointer_cast<PdfIndirectRef>(obj);
        if (!ref) return nullptr;

        // ✅ Circular reference kontrolü
        if (visitedIds.count(ref->objNum))
        {
            // Döngüsel referans - null dön
            return nullptr;
        }

        // ✅ Maksimum derinlik kontrolü
        if (visitedIds.size() > 100)
        {
            // Çok derin referans zinciri
            return nullptr;
        }

        visitedIds.insert(ref->objNum);

        // FIRST: Check _objects (already parsed and possibly decrypted)
        auto it = _objects.find(ref->objNum);
        if (it != _objects.end())
            return resolveIndirect(it->second, visitedIds);

        // FALLBACK: Load from XRef table if not in _objects
        auto itX = _xrefTable.find(ref->objNum);
        if (itX != _xrefTable.end())
        {
            auto loaded = const_cast<PdfDocument*>(this)->loadObjectAtOffset(itX->second);
            if (loaded)
            {
                // Decrypt if needed
                if (_encryptionReady)
                {
                    auto stream = std::dynamic_pointer_cast<PdfStream>(loaded);
                    if (stream && !stream->data.empty())
                    {
                        auto objKey = computeObjectKey(ref->objNum, ref->genNum);
                        if (_useAES)
                        {
                            std::vector<uint8_t> decrypted;
                            if (aesDecryptCBC(objKey, stream->data.data(), stream->data.size(), decrypted))
                                stream->data = std::move(decrypted);
                        }
                        else
                        {
                            std::vector<uint8_t> decrypted;
                            rc4Crypt(objKey, stream->data.data(), stream->data.size(), decrypted);
                            stream->data = std::move(decrypted);
                        }
                    }
                }
                // Cache in _objects so we don't reload again
                const_cast<PdfDocument*>(this)->_objects[ref->objNum] = loaded;
                return resolveIndirect(loaded, visitedIds);
            }
        }

        // FALLBACK 2: Object Stream (ObjStm) — type 2 xref entries
        auto itObjStm = _objStmEntries.find(ref->objNum);
        if (itObjStm != _objStmEntries.end())
        {
            auto loaded = const_cast<PdfDocument*>(this)->loadFromObjStm(
                ref->objNum, itObjStm->second.objStmNum, itObjStm->second.indexInStream);
            if (loaded)
            {
                const_cast<PdfDocument*>(this)->_objects[ref->objNum] = loaded;
                return resolveIndirect(loaded, visitedIds);
            }
        }

        return nullptr;
    }

    // =====================================================
    // Object Stream (ObjStm) Parse & Load
    // =====================================================

    std::shared_ptr<PdfObject> PdfDocument::loadFromObjStm(int objNum, int objStmNum, int indexInStream)
    {
        // 1. ObjStm objesini yükle (kendisi type 1 veya zaten _objects'te olmalı)
        auto itObj = _objects.find(objStmNum);
        std::shared_ptr<PdfStream> objStmStream;

        if (itObj != _objects.end())
        {
            objStmStream = std::dynamic_pointer_cast<PdfStream>(itObj->second);
        }

        // _objects'te yoksa xrefTable'dan yükle
        if (!objStmStream)
        {
            auto itX = _xrefTable.find(objStmNum);
            if (itX != _xrefTable.end())
            {
                auto loaded = loadObjectAtOffset(itX->second);
                if (loaded)
                {
                    _objects[objStmNum] = loaded;
                    objStmStream = std::dynamic_pointer_cast<PdfStream>(loaded);
                }
            }
        }

        if (!objStmStream || !objStmStream->dict) return nullptr;

        // 2. /N (obje sayısı) ve /First (ilk obje verisi offset) oku
        int n = 0, first = 0;
        if (auto nObj = std::dynamic_pointer_cast<PdfNumber>(objStmStream->dict->get("/N")))
            n = (int)nObj->value;
        else if (auto nObj2 = std::dynamic_pointer_cast<PdfNumber>(objStmStream->dict->get("N")))
            n = (int)nObj2->value;

        if (auto fObj = std::dynamic_pointer_cast<PdfNumber>(objStmStream->dict->get("/First")))
            first = (int)fObj->value;
        else if (auto fObj2 = std::dynamic_pointer_cast<PdfNumber>(objStmStream->dict->get("First")))
            first = (int)fObj2->value;

        if (n <= 0 || first <= 0 || indexInStream >= n) return nullptr;

        // 3. Stream verisini decode et
        std::vector<uint8_t> decoded;
        if (!decodeStream(objStmStream, decoded) || decoded.empty())
        {
            decoded = objStmStream->data; // fallback raw
        }

        if (decoded.empty()) return nullptr;

        // 4. İlk /N çift integer'ı parse et: objNum1 offset1 objNum2 offset2 ...
        //    Bu header kısmı 0'dan first'e kadar
        std::vector<std::pair<int, int>> entries; // (objNum, relativeOffset)
        {
            size_t pos = 0;
            for (int i = 0; i < n && pos < (size_t)first; i++)
            {
                // whitespace atla
                while (pos < decoded.size() && (decoded[pos] == ' ' || decoded[pos] == '\n' || decoded[pos] == '\r' || decoded[pos] == '\t'))
                    pos++;

                // objNum oku
                int oNum = 0;
                while (pos < decoded.size() && decoded[pos] >= '0' && decoded[pos] <= '9')
                {
                    oNum = oNum * 10 + (decoded[pos] - '0');
                    pos++;
                }

                // whitespace atla
                while (pos < decoded.size() && (decoded[pos] == ' ' || decoded[pos] == '\n' || decoded[pos] == '\r' || decoded[pos] == '\t'))
                    pos++;

                // offset oku
                int off = 0;
                while (pos < decoded.size() && decoded[pos] >= '0' && decoded[pos] <= '9')
                {
                    off = off * 10 + (decoded[pos] - '0');
                    pos++;
                }

                entries.push_back({ oNum, off });
            }
        }

        if (indexInStream >= (int)entries.size()) return nullptr;

        // 5. İstenen objeyi parse et
        int targetOffset = first + entries[indexInStream].second;
        if (targetOffset >= (int)decoded.size()) return nullptr;

        // PdfParser ile objeyi parse et
        PdfParser parser(decoded);
        auto result = parser.parseObjectAt((size_t)targetOffset);

        if (result)
        {
            LogDebug("[ObjStm] Loaded obj %d from ObjStm %d (index %d, offset %d)",
                objNum, objStmNum, indexInStream, targetOffset);
        }

        return result;
    }

    // =====================================================
    // Obje GERÇEK sayfa mı?
    // =====================================================

    bool PdfDocument::isPageObject(const std::shared_ptr<PdfDictionary>& dict) const
    {
        if (!dict) return false;

        std::set<int> v;
        auto typeObj = resolveIndirect(dict->get("/Type"), v);
        if (!typeObj)
        {
            v.clear();
            typeObj = resolveIndirect(dict->get("Type"), v);
        }

        auto typeName = std::dynamic_pointer_cast<PdfName>(typeObj);
        if (typeName)
        {
            if (typeName->value != "/Page" && typeName->value != "Page")
                return false;
        }
        else
        {
            // Tam Type yoksa ama MediaBox/Parent varsa yine sayfa kabul edelim
            if (!dict->get("/MediaBox") && !dict->get("MediaBox") && !dict->get("/Parent"))
                return false;
        }

        // Ghost page filtresi (çok dar sayfaları ele)
        v.clear();
        auto mbObj = resolveIndirect(dict->get("/MediaBox"), v);
        if (!mbObj)
        {
            v.clear();
            mbObj = resolveIndirect(dict->get("MediaBox"), v);
        }

        auto mbArr = std::dynamic_pointer_cast<PdfArray>(mbObj);
        if (mbArr && mbArr->items.size() >= 4)
        {
            v.clear();
            auto x1 = std::dynamic_pointer_cast<PdfNumber>(resolveIndirect(mbArr->items[0], v));
            v.clear();
            auto x2 = std::dynamic_pointer_cast<PdfNumber>(resolveIndirect(mbArr->items[2], v));

            if (x1 && x2)
            {
                double w = std::abs(x2->value - x1->value);
                if (w > 0.0 && w < 5.0) // 5 pt altı saçma
                    return false;
            }
        }

        return true;
    }

    // =====================================================
    // Sayfa Sayısı
    // =====================================================

    int PdfDocument::getPageCountFromPageTree() const
    {
        // BİRİNCİL: Sayfa ağacını yürü (getPageDictionary ile tutarlı)
        if (_pages)
        {
            std::set<const PdfDictionary*> visited;
            int treeCount = countPagesRecursive(_pages, visited);
            if (treeCount > 0)
                return treeCount;
        }

        // YEDEK: Bozuk/eksik sayfa ağacı için tüm objeleri tara
        int manualCount = 0;
        for (const auto& kv : _objects)
        {
            auto dict = std::dynamic_pointer_cast<PdfDictionary>(kv.second);
            if (dict && isPageObject(dict))
                manualCount++;
        }

        if (manualCount > 0)
            return manualCount;

        return _objects.empty() ? -1 : 0;
    }

    // =====================================================
    // Page Dictionary – 0 based
    // =====================================================

    std::shared_ptr<PdfDictionary> PdfDocument::getPageDictionary(int pageIndex) const
    {
        if (pageIndex < 0)
            return nullptr;

        std::vector<std::shared_ptr<PdfDictionary>> pages;

        // 1) Mümkünse Page tree kullan
        if (_pages)
        {
            std::set<const PdfDictionary*> visited;

            std::function<void(std::shared_ptr<PdfDictionary>)> walk =
                [&](std::shared_ptr<PdfDictionary> node)
                {
                    if (!node || visited.count(node.get())) return;
                    visited.insert(node.get());

                    std::set<int> v;
                    auto typeObj = resolveIndirect(node->get("/Type"), v);
                    if (!typeObj)
                    {
                        v.clear();
                        typeObj = resolveIndirect(node->get("Type"), v);
                    }
                    auto typeName = std::dynamic_pointer_cast<PdfName>(typeObj);
                    std::string t = typeName ? typeName->value : "";

                    if (t == "/Page" || t == "Page")
                    {
                        if (isPageObject(node))
                            pages.push_back(node);
                        return;
                    }

                    if (t == "/Pages" || t == "Pages")
                    {
                        v.clear();
                        auto kidsObj = resolveIndirect(node->get("/Kids"), v);
                        auto kidsArr = std::dynamic_pointer_cast<PdfArray>(kidsObj);
                        if (!kidsArr) return;

                        for (auto& k : kidsArr->items)
                        {
                            v.clear();
                            auto d = std::dynamic_pointer_cast<PdfDictionary>(resolveIndirect(k, v));
                            if (d) walk(d);
                        }
                    }
                };

            walk(_pages);
        }

        // 2) Tree işe yaramazsa → tüm objeleri tara
        if (pages.empty())
        {
            for (const auto& kv : _objects)
            {
                auto dict = std::dynamic_pointer_cast<PdfDictionary>(kv.second);
                if (dict && isPageObject(dict))
                    pages.push_back(dict);
            }
        }

        if (pageIndex < 0 || pageIndex >= (int)pages.size())
            return nullptr;

        return pages[pageIndex];
    }

    // =====================================================
    // /Rotate Çözümü (dictionary üzerinden)
    // =====================================================

    int PdfDocument::getPageRotate(std::shared_ptr<PdfDictionary> pageDict) const
    {
        if (!pageDict) return 0;

        std::shared_ptr<PdfDictionary> current = pageDict;
        int depth = 0;
        const int MAX_DEPTH = 32;

        while (current && depth++ < MAX_DEPTH)
        {
            std::set<int> v;
            auto rotObj = resolveIndirect(current->get("/Rotate"), v);
            auto rotNum = std::dynamic_pointer_cast<PdfNumber>(rotObj);

            if (rotNum)
            {
                int r = (int)std::round(rotNum->value);
                r = ((r % 360) + 360) % 360;

                if (r == 90 || r == 180 || r == 270)
                    return r;

                return 0;
            }

            v.clear();
            auto parentObj = resolveIndirect(current->get("/Parent"), v);
            current = std::dynamic_pointer_cast<PdfDictionary>(parentObj);
        }

        return 0;
    }

    // =====================================================
    // /Rotate Çözümü (index ile)
    // =====================================================

    int PdfDocument::getPageRotate(int pageIndex) const
    {
        auto page = getPageDictionary(pageIndex);
        if (!page)
            return 0;

        return getPageRotate(page);
    }

    // =====================================================
    // Box Çözümü (MediaBox / CropBox)
    // =====================================================

    bool PdfDocument::extractBox(
        const std::shared_ptr<PdfDictionary>& page,
        const std::string& key,
        double& x1, double& y1,
        double& x2, double& y2
    ) const
    {
        std::shared_ptr<PdfDictionary> cur = page;
        int depth = 0;

        while (cur && depth++ < 32)
        {
            std::set<int> v;
            auto obj = resolveIndirect(cur->get(key), v);
            if (!obj)
            {
                v.clear();
                if (!key.empty() && key[0] == '/')
                    obj = resolveIndirect(cur->get(key.substr(1)), v);
            }

            auto arr = std::dynamic_pointer_cast<PdfArray>(obj);
            if (arr && arr->items.size() >= 4)
            {
                v.clear();
                auto n1 = std::dynamic_pointer_cast<PdfNumber>(resolveIndirect(arr->items[0], v));
                v.clear();
                auto n2 = std::dynamic_pointer_cast<PdfNumber>(resolveIndirect(arr->items[1], v));
                v.clear();
                auto n3 = std::dynamic_pointer_cast<PdfNumber>(resolveIndirect(arr->items[2], v));
                v.clear();
                auto n4 = std::dynamic_pointer_cast<PdfNumber>(resolveIndirect(arr->items[3], v));

                if (n1 && n2 && n3 && n4)
                {
                    x1 = n1->value;
                    y1 = n2->value;
                    x2 = n3->value;
                    y2 = n4->value;
                    return true;
                }
            }

            // Parent'a tırman
            std::set<int> vv;
            auto parent = resolveIndirect(cur->get("/Parent"), vv);
            cur = std::dynamic_pointer_cast<PdfDictionary>(parent);
        }

        return false;
    }

    // =====================================================
    // Sayfa Boyutu
    // =====================================================

    // Raw page size (MediaBox without rotation)
    bool PdfDocument::getRawPageSize(int pageIndex, double& wPt, double& hPt) const
    {
        auto page = getPageDictionary(pageIndex);
        if (!page)
        {
            // Page dictionary bulunamadı - default A4 döndür
            // Bu durum parsing hatası olabilir ama render'ı kırmayalım
            LogDebug("WARNING: Page %d dictionary not found, using default A4 size", pageIndex);
            wPt = 595.0;
            hPt = 842.0;
            return true;  // true döndür ki render devam etsin
        }

        double x1 = 0, y1 = 0, x2 = 0, y2 = 0;

        // ✅ FIX: Önce CropBox kontrol edilmeli (PDF Spec)
        // CropBox, görüntülenecek/basılacak alanı belirler.
        if (extractBox(page, "/CropBox", x1, y1, x2, y2))
        {
            wPt = std::abs(x2 - x1);
            hPt = std::abs(y2 - y1);
            return true;
        }

        if (extractBox(page, "/MediaBox", x1, y1, x2, y2))
        {
            wPt = std::abs(x2 - x1);
            hPt = std::abs(y2 - y1);
            return true;
        }

        wPt = 595.0;
        hPt = 842.0;
        return true;
    }

    // Display page size (rotation-aware) - for UI and painter
    bool PdfDocument::getPageSize(int pageIndex, double& wPt, double& hPt) const
    {
        if (!getRawPageSize(pageIndex, wPt, hPt))
            return false;

        int rot = getPageRotate(pageIndex);
        if (rot == 90 || rot == 270) {
            std::swap(wPt, hPt);
        }
        return true;
    }

    // Alias for getPageSize
    bool PdfDocument::getDisplayPageSize(int pageIndex, double& wPt, double& hPt) const
    {
        return getPageSize(pageIndex, wPt, hPt);
    }

    // =====================================================
    // İçerik Stream'leri
    // =====================================================
    bool PdfDocument::getPageContentsBytesInternal(
        int index,
        std::vector<uint8_t>& out) const
    {
        LogDebug("Getting page %d contents", index);

        out.clear();

        auto page = getPageDictionary(index);
        if (!page)
        {
            LogDebug("ERROR: Page %d not found", index);
            return false;
        }

        auto contObj = dictGetAny(page, "/Contents", "Contents");
        if (!contObj)
        {
            LogDebug("Page %d has no contents", index);
            return false;
        }

        std::set<int> visited;
        auto resolved = resolveIndirect(contObj, visited);
        if (!resolved)
        {
            LogDebug("ERROR: Could not resolve contents for page %d", index);
            return false;
        }

        // ✅ EKSİK OLAN KOD - BURASI ÖNEMLİ!

        // Contents tek stream ise
        if (auto st = std::dynamic_pointer_cast<PdfStream>(resolved))
        {
            appendStreamData(st, out);
            LogDebug("Page %d contents size: %zu bytes (single stream)", index, out.size());
            return !out.empty();
        }

        // Contents array ise
        if (auto arr = std::dynamic_pointer_cast<PdfArray>(resolved))
        {
            for (auto& item : arr->items)
            {
                visited.clear();
                auto itemResolved = resolveIndirect(item, visited);
                auto st = std::dynamic_pointer_cast<PdfStream>(itemResolved);
                if (st)
                {
                    appendStreamData(st, out);
                }
            }
            LogDebug("Page %d contents size: %zu bytes (array of %zu streams)",
                index, out.size(), arr->items.size());
            return !out.empty();
        }

        LogDebug("ERROR: Page %d contents is neither stream nor array", index);
        return false;
    }

    bool PdfDocument::getPageContentsBytes(int pageIndex, std::vector<uint8_t>& out) const
    {
        return getPageContentsBytesInternal(pageIndex, out);
    }

    void PdfDocument::appendStreamData(
        const std::shared_ptr<PdfStream>& st,
        std::vector<uint8_t>& out
    ) const
    {
        if (!st) return;

        // ✅ FIX: Use decodeStream for ALL filter combinations
        // This handles ASCII85+Flate, LZW, RunLength, etc.
        std::vector<uint8_t> decoded;
        if (decodeStream(st, decoded) && !decoded.empty())
        {
            out.insert(out.end(), decoded.begin(), decoded.end());
            out.push_back('\n');
        }
        else
        {
            // Fallback: append raw data if decode fails
            out.insert(out.end(), st->data.begin(), st->data.end());
            out.push_back('\n');
        }
    }

    // =====================================================
    // Flate Decompress
    // =====================================================

    bool PdfDocument::decompressFlate(
        const std::vector<uint8_t>& input,
        std::vector<uint8_t>& output
    ) const
    {
        output.clear();
        if (input.empty())
            return false;

        auto doInflate = [&](int windowBits, std::vector<uint8_t>& out) -> bool
            {
                z_stream strm{};
                strm.zalloc = Z_NULL;
                strm.zfree = Z_NULL;
                strm.opaque = Z_NULL;
                strm.next_in = (Bytef*)input.data();
                strm.avail_in = (uInt)input.size();

                if (inflateInit2(&strm, windowBits) != Z_OK)
                    return false;

                const size_t CHUNK = 4096;
                uint8_t buffer[CHUNK];

                int ret = Z_OK;
                bool ok = false;

                while (true)
                {
                    strm.next_out = buffer;
                    strm.avail_out = CHUNK;

                    ret = inflate(&strm, Z_NO_FLUSH);

                    if (ret == Z_STREAM_END)
                    {
                        size_t produced = CHUNK - strm.avail_out;
                        out.insert(out.end(), buffer, buffer + produced);
                        ok = true;
                        break;
                    }
                    else if (ret == Z_OK)
                    {
                        size_t produced = CHUNK - strm.avail_out;
                        out.insert(out.end(), buffer, buffer + produced);
                        continue;
                    }
                    else
                    {
                        ok = false;
                        break;
                    }
                }

                inflateEnd(&strm);
                return ok;
            };

        // 1) Normal zlib header
        if (doInflate(MAX_WBITS, output) && !output.empty())
            return true;

        // 2) Raw deflate
        output.clear();
        if (doInflate(-MAX_WBITS, output) && !output.empty())
            return true;

        output.clear();
        return false;
    }

    bool PdfDocument::getPageResources(
        int pageIndex,
        std::vector<std::shared_ptr<PdfDictionary>>& outStack) const
    {
        outStack.clear();

        auto page = getPageDictionary(pageIndex);
        if (!page) return false;

        // DEBUG: Page dictionary keys
        {
            std::string keys;
            for (auto& kv : page->entries) {
                keys += kv.first + " ";
            }
            LogDebug("getPageResources: page %d dict has %zu entries. Keys: %s",
                pageIndex, page->entries.size(), keys.c_str());
        }

        std::shared_ptr<PdfDictionary> cur = page;
        int depth = 0;

        while (cur && depth++ < 32)
        {
            std::set<int> v;
            auto resObj = resolveIndirect(dictGetAny(cur, "/Resources", "Resources"), v);
            auto res = std::dynamic_pointer_cast<PdfDictionary>(resObj);

            LogDebug("getPageResources: depth=%d, resObj=%s, res=%s",
                depth - 1,
                resObj ? "FOUND" : "NULL",
                res ? "DICT" : "NULL");

            if (res)
                outStack.push_back(res);

            v.clear();
            auto parentObj = resolveIndirect(dictGetAny(cur, "/Parent", "Parent"), v);
            cur = std::dynamic_pointer_cast<PdfDictionary>(parentObj);
        }

        LogDebug("getPageResources: outStack.size=%zu", outStack.size());
        return !outStack.empty();
    }

    // =====================================================
    // XObject’lar
    // =====================================================
    bool PdfDocument::renderPageToPainter(
        int pageIndex,
        PdfPainter& painter)
    {
        // 1) Page dictionary
        auto page = getPageDictionary(pageIndex);
        if (!page)
            return false;

        // 2) Page size - display dimensions (rotation-aware)
        double pageW = 595.0;
        double pageH = 842.0;
        getPageSize(pageIndex, pageW, pageH);

        // 3) RAW MediaBox (rotation yok) - rotation CTM için gerekli
        double rawW = 595.0, rawH = 842.0;
        getRawPageSize(pageIndex, rawW, rawH);

        // 4) Rotation değerini al
        const int rot = getPageRotate(pageIndex);

        // 5) Initial CTM with rotation
        // Content stream portrait MediaBox'a göre yazılmış.
        // Rotation CTM ile landscape buffer'a doğru çizilmesini sağlıyoruz.
        PdfMatrix pageCTM;
        pageCTM.a = 1;
        pageCTM.b = 0;
        pageCTM.c = 0;
        pageCTM.d = 1;
        pageCTM.e = 0;
        pageCTM.f = 0;

        if (rot == 90) {
            // 90° rotation: portrait content -> landscape buffer
            // Content stream'deki CTM [0,1,-1,0,rawW,0] ile birleşince identity olur
            // Transform: (x,y) -> (y, rawW - x)
            pageCTM.a = 0;
            pageCTM.b = -1;
            pageCTM.c = 1;
            pageCTM.d = 0;
            pageCTM.e = 0;
            pageCTM.f = rawW;
        }
        else if (rot == 180) {
            pageCTM.a = -1;
            pageCTM.b = 0;
            pageCTM.c = 0;
            pageCTM.d = -1;
            pageCTM.e = rawW;
            pageCTM.f = rawH;
        }
        else if (rot == 270) {
            // 270° rotation: portrait content -> landscape buffer  
            // Transform: (x,y) -> (rawH - y, x)
            pageCTM.a = 0;
            pageCTM.b = 1;
            pageCTM.c = -1;
            pageCTM.d = 0;
            pageCTM.e = rawH;
            pageCTM.f = 0;
        }

        // 6) Initial graphics state
        PdfGraphicsState gs;
        gs.ctm = pageCTM;
        gs.lineWidth = 1.0;
        gs.lineCap = 1;
        gs.lineJoin = 1;
        gs.miterLimit = 10.0;

        gs.fillColor[0] = 0;
        gs.fillColor[1] = 0;
        gs.fillColor[2] = 0;

        gs.strokeColor[0] = 0;
        gs.strokeColor[1] = 0;
        gs.strokeColor[2] = 0;

        // 7) Content
        std::vector<uint8_t> content;
        if (!getPageContentsBytes(pageIndex, content))
            return true; // empty page

        // 8) Fonts
        std::map<std::string, PdfFontInfo> fonts;
        getPageFonts(pageIndex, fonts);

        // 9) Resources (stack)
        std::vector<std::shared_ptr<PdfDictionary>> resStack;
        getPageResources(pageIndex, resStack);

        // PdfContentParser expects resStack.back()
        std::reverse(resStack.begin(), resStack.end());

        // 10) Parse content
        PdfContentParser parser(
            content,
            &painter,
            this,
            pageIndex,
            &fonts,
            gs,
            resStack
        );

        parser.parse();
        return true;
    }




    bool PdfDocument::getPageXObjects(
        int pageIndex,
        std::map<std::string, std::shared_ptr<PdfStream>>& out) const
    {
        out.clear();

        auto page = getPageDictionary(pageIndex);
        if (!page) return false;

        std::set<int> v;

        auto resObj = resolveIndirect(dictGetAny(page, "/Resources", "Resources"), v);
        auto res = std::dynamic_pointer_cast<PdfDictionary>(resObj);
        if (!res) return false;

        v.clear();
        auto xoObj = resolveIndirect(dictGetAny(res, "/XObject", "XObject"), v);
        auto xo = std::dynamic_pointer_cast<PdfDictionary>(xoObj);
        if (!xo) return false;

        for (auto& kv : xo->entries)
        {
            std::set<int> v2;
            auto stObj = resolveIndirect(kv.second, v2);
            auto st = std::dynamic_pointer_cast<PdfStream>(stObj);
            if (!st) continue;

            std::string key = kv.first;
            if (!key.empty() && key[0] == '/') key = key.substr(1);

            out[key] = st;
        }

        return !out.empty();
    }

    // =====================================================
// GPU RENDERING (PdfPainterGPU) - Direct2D Hardware Acceleration
// =====================================================
    bool PdfDocument::renderPageToPainter(
        int pageIndex,
        PdfPainterGPU& painter)
    {
        auto page = getPageDictionary(pageIndex);
        if (!page)
            return false;

        double pageW = 595.0;
        double pageH = 842.0;
        getPageSize(pageIndex, pageW, pageH);

        // RAW MediaBox (rotation yok) - rotation CTM için gerekli
        double rawW = 595.0, rawH = 842.0;
        getRawPageSize(pageIndex, rawW, rawH);

        // Rotation değerini al
        const int rot = getPageRotate(pageIndex);

        // Initial CTM with rotation
        PdfGraphicsState gs;
        gs.ctm = PdfMatrix();

        if (rot == 90) {
            gs.ctm.a = 0;
            gs.ctm.b = -1;
            gs.ctm.c = 1;
            gs.ctm.d = 0;
            gs.ctm.e = 0;
            gs.ctm.f = rawW;
        }
        else if (rot == 180) {
            gs.ctm.a = -1;
            gs.ctm.b = 0;
            gs.ctm.c = 0;
            gs.ctm.d = -1;
            gs.ctm.e = rawW;
            gs.ctm.f = rawH;
        }
        else if (rot == 270) {
            gs.ctm.a = 0;
            gs.ctm.b = 1;
            gs.ctm.c = -1;
            gs.ctm.d = 0;
            gs.ctm.e = rawH;
            gs.ctm.f = 0;
        }

        gs.lineWidth = 1.0;
        gs.lineCap = 1;
        gs.lineJoin = 1;
        gs.miterLimit = 10.0;
        gs.fillColor[0] = gs.fillColor[1] = gs.fillColor[2] = 0;
        gs.strokeColor[0] = gs.strokeColor[1] = gs.strokeColor[2] = 0;

        std::vector<uint8_t> content;
        if (!getPageContentsBytes(pageIndex, content))
            return true;

        std::map<std::string, PdfFontInfo> fonts;
        getPageFonts(pageIndex, fonts);

        std::vector<std::shared_ptr<PdfDictionary>> resStack;
        getPageResources(pageIndex, resStack);
        std::reverse(resStack.begin(), resStack.end());

        // GPU rendering - pass directly to PdfContentParser
        // PdfPainterGPU implements IPdfPainter interface
        painter.beginDraw();

        PdfContentParser parser(content, &painter, this, pageIndex, &fonts, gs, resStack);
        parser.parse();

        painter.endDraw();

        return true;
    }

    // IPdfPainter interface version (generic)
    bool PdfDocument::renderPageToPainter(
        int pageIndex,
        IPdfPainter& painter)
    {
        auto page = getPageDictionary(pageIndex);
        if (!page)
            return false;

        double pageW = 595.0;
        double pageH = 842.0;
        getPageSize(pageIndex, pageW, pageH);

        double rawW = 595.0, rawH = 842.0;
        getRawPageSize(pageIndex, rawW, rawH);

        const int rot = getPageRotate(pageIndex);

        PdfMatrix pageCTM;
        pageCTM.a = 1; pageCTM.b = 0;
        pageCTM.c = 0; pageCTM.d = 1;
        pageCTM.e = 0; pageCTM.f = 0;

        if (rot == 90) {
            pageCTM.a = 0; pageCTM.b = -1;
            pageCTM.c = 1; pageCTM.d = 0;
            pageCTM.e = 0; pageCTM.f = rawW;
        }
        else if (rot == 180) {
            pageCTM.a = -1; pageCTM.b = 0;
            pageCTM.c = 0; pageCTM.d = -1;
            pageCTM.e = rawW; pageCTM.f = rawH;
        }
        else if (rot == 270) {
            pageCTM.a = 0; pageCTM.b = 1;
            pageCTM.c = -1; pageCTM.d = 0;
            pageCTM.e = rawH; pageCTM.f = 0;
        }

        PdfGraphicsState gs;
        gs.ctm = pageCTM;
        gs.lineWidth = 1.0;
        gs.lineCap = 1;
        gs.lineJoin = 1;
        gs.miterLimit = 10.0;
        gs.fillColor[0] = gs.fillColor[1] = gs.fillColor[2] = 0;
        gs.strokeColor[0] = gs.strokeColor[1] = gs.strokeColor[2] = 0;

        std::vector<uint8_t> content;
        if (!getPageContentsBytes(pageIndex, content))
            return true;

        std::map<std::string, PdfFontInfo> fonts;
        getPageFonts(pageIndex, fonts);

        std::vector<std::shared_ptr<PdfDictionary>> resStack;
        getPageResources(pageIndex, resStack);
        std::reverse(resStack.begin(), resStack.end());

        // ===== PERFORMANCE: Start page rendering batch =====
        painter.beginPage();

        PdfContentParser parser(content, &painter, this, pageIndex, &fonts, gs, resStack);
        parser.parse();

        // ===== PERFORMANCE: End page rendering batch =====
        painter.endPage();

        return true;
    }

    // =====================================================
    // Offset’ten obje parse (gerekirse)
    // =====================================================

    std::shared_ptr<PdfObject> PdfDocument::loadObjectAtOffset(size_t offset)
    {
        PdfParser parser(_data);
        return parser.parseObjectAt(offset);
    }

    // =====================================================
    // PDF ENCRYPTION / DECRYPTION
    // Supports:
    //   /Standard handler (password-based):
    //     V=1,2 R=2,3 (RC4 40-128 bit)
    //     V=4   R=4   (AES-128-CBC)
    //     V=5   R=5,6 (AES-256)
    //   /Adobe.PubSec handler (certificate-based):
    //     SubFilter adbe.pkcs7.s3 / adbe.pkcs7.s5
    //     PKCS#7/CMS EnvelopedData with RSA recipients
    //     AES-128/256 stream encryption
    // =====================================================

    // RC4 cipher implementation
    void PdfDocument::rc4Crypt(const std::vector<uint8_t>& key,
        const uint8_t* input, size_t inputLen,
        std::vector<uint8_t>& output)
    {
        output.resize(inputLen);
        if (inputLen == 0) return;

        // Key-Scheduling Algorithm (KSA)
        uint8_t S[256];
        for (int i = 0; i < 256; ++i) S[i] = (uint8_t)i;

        int j = 0;
        for (int i = 0; i < 256; ++i)
        {
            j = (j + S[i] + key[i % key.size()]) & 0xFF;
            std::swap(S[i], S[j]);
        }

        // Pseudo-Random Generation Algorithm (PRGA)
        int si = 0, sj = 0;
        for (size_t n = 0; n < inputLen; ++n)
        {
            si = (si + 1) & 0xFF;
            sj = (sj + S[si]) & 0xFF;
            std::swap(S[si], S[sj]);
            output[n] = input[n] ^ S[(S[si] + S[sj]) & 0xFF];
        }
    }

    // =====================================================
    // AES-128-CBC Decryption for PDF (V=4, R=4)
    // PDF streams: first 16 bytes = IV, rest = ciphertext
    // PKCS#7 padding removed from decrypted output
    // =====================================================

    // Compact AES-128 implementation (decrypt only)
    namespace {
        // AES S-Box
        static const uint8_t AES_SBOX[256] = {
            0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
            0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
            0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
            0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
            0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
            0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
            0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
            0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
            0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
            0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
            0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
            0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
            0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
            0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
            0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
            0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
        };

        // AES Inverse S-Box
        static const uint8_t AES_INV_SBOX[256] = {
            0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
            0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
            0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
            0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
            0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
            0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
            0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
            0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
            0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
            0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
            0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
            0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
            0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
            0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
            0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
            0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
        };

        // AES round constants
        static const uint8_t AES_RCON[11] = { 0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36 };

        // Multiply in GF(2^8)
        static uint8_t gmul(uint8_t a, uint8_t b) {
            uint8_t p = 0;
            for (int i = 0; i < 8; i++) {
                if (b & 1) p ^= a;
                bool hi = (a & 0x80) != 0;
                a <<= 1;
                if (hi) a ^= 0x1b;
                b >>= 1;
            }
            return p;
        }

        // Key expansion for AES-128 (produces 11 round keys = 176 bytes)
        static void aes128KeyExpansion(const uint8_t key[16], uint8_t roundKeys[176]) {
            memcpy(roundKeys, key, 16);
            for (int i = 4; i < 44; i++) {
                uint8_t temp[4];
                memcpy(temp, roundKeys + (i - 1) * 4, 4);
                if (i % 4 == 0) {
                    // RotWord + SubBytes + Rcon
                    uint8_t t = temp[0];
                    temp[0] = AES_SBOX[temp[1]] ^ AES_RCON[i / 4];
                    temp[1] = AES_SBOX[temp[2]];
                    temp[2] = AES_SBOX[temp[3]];
                    temp[3] = AES_SBOX[t];
                }
                for (int j = 0; j < 4; j++)
                    roundKeys[i * 4 + j] = roundKeys[(i - 4) * 4 + j] ^ temp[j];
            }
        }

        // Key expansion for AES-256 (produces 15 round keys = 240 bytes)
        static void aes256KeyExpansion(const uint8_t key[32], uint8_t roundKeys[240]) {
            memcpy(roundKeys, key, 32);
            for (int i = 8; i < 60; i++) {
                uint8_t temp[4];
                memcpy(temp, roundKeys + (i - 1) * 4, 4);
                if (i % 8 == 0) {
                    uint8_t t = temp[0];
                    temp[0] = AES_SBOX[temp[1]] ^ AES_RCON[i / 8];
                    temp[1] = AES_SBOX[temp[2]];
                    temp[2] = AES_SBOX[temp[3]];
                    temp[3] = AES_SBOX[t];
                }
                else if (i % 8 == 4) {
                    // SubWord only (AES-256 specific)
                    for (int j = 0; j < 4; j++) temp[j] = AES_SBOX[temp[j]];
                }
                for (int j = 0; j < 4; j++)
                    roundKeys[i * 4 + j] = roundKeys[(i - 8) * 4 + j] ^ temp[j];
            }
        }

        // Generic AES decrypt block (works for both 128 and 256)
        static void aesDecryptBlock(const uint8_t input[16], uint8_t output[16],
            const uint8_t* roundKeys, int numRounds) {
            uint8_t state[16];
            memcpy(state, input, 16);

            // AddRoundKey (last round)
            for (int i = 0; i < 16; i++) state[i] ^= roundKeys[numRounds * 16 + i];

            for (int round = numRounds - 1; round >= 1; round--) {
                // InvShiftRows
                uint8_t t;
                t = state[13]; state[13] = state[9]; state[9] = state[5]; state[5] = state[1]; state[1] = t;
                t = state[2]; state[2] = state[10]; state[10] = t;
                t = state[6]; state[6] = state[14]; state[14] = t;
                t = state[3]; state[3] = state[7]; state[7] = state[11]; state[11] = state[15]; state[15] = t;

                // InvSubBytes
                for (int i = 0; i < 16; i++) state[i] = AES_INV_SBOX[state[i]];

                // AddRoundKey
                for (int i = 0; i < 16; i++) state[i] ^= roundKeys[round * 16 + i];

                // InvMixColumns
                for (int c = 0; c < 4; c++) {
                    int ci = c * 4;
                    uint8_t s0 = state[ci], s1 = state[ci + 1], s2 = state[ci + 2], s3 = state[ci + 3];
                    state[ci] = gmul(s0, 0x0e) ^ gmul(s1, 0x0b) ^ gmul(s2, 0x0d) ^ gmul(s3, 0x09);
                    state[ci + 1] = gmul(s0, 0x09) ^ gmul(s1, 0x0e) ^ gmul(s2, 0x0b) ^ gmul(s3, 0x0d);
                    state[ci + 2] = gmul(s0, 0x0d) ^ gmul(s1, 0x09) ^ gmul(s2, 0x0e) ^ gmul(s3, 0x0b);
                    state[ci + 3] = gmul(s0, 0x0b) ^ gmul(s1, 0x0d) ^ gmul(s2, 0x09) ^ gmul(s3, 0x0e);
                }
            }

            // Round 0: InvShiftRows + InvSubBytes + AddRoundKey (no InvMixColumns)
            {
                uint8_t t;
                t = state[13]; state[13] = state[9]; state[9] = state[5]; state[5] = state[1]; state[1] = t;
                t = state[2]; state[2] = state[10]; state[10] = t;
                t = state[6]; state[6] = state[14]; state[14] = t;
                t = state[3]; state[3] = state[7]; state[7] = state[11]; state[11] = state[15]; state[15] = t;
            }
            for (int i = 0; i < 16; i++) state[i] = AES_INV_SBOX[state[i]];
            for (int i = 0; i < 16; i++) state[i] ^= roundKeys[i];

            memcpy(output, state, 16);
        }

        // AES-128 encrypt single block (needed for Algorithm 2.B / R=6)
        static void aes128EncryptBlock(const uint8_t input[16], uint8_t output[16], const uint8_t roundKeys[176]) {
            uint8_t state[16];
            memcpy(state, input, 16);

            // AddRoundKey (round 0)
            for (int i = 0; i < 16; i++) state[i] ^= roundKeys[i];

            for (int round = 1; round <= 9; round++) {
                // SubBytes
                for (int i = 0; i < 16; i++) state[i] = AES_SBOX[state[i]];
                // ShiftRows
                uint8_t t;
                t = state[1]; state[1] = state[5]; state[5] = state[9]; state[9] = state[13]; state[13] = t;
                t = state[2]; state[2] = state[10]; state[10] = t;
                t = state[6]; state[6] = state[14]; state[14] = t;
                t = state[11]; state[11] = state[7]; state[7] = state[3]; state[3] = state[15]; state[15] = t;
                // MixColumns
                for (int c = 0; c < 4; c++) {
                    int ci = c * 4;
                    uint8_t s0 = state[ci], s1 = state[ci + 1], s2 = state[ci + 2], s3 = state[ci + 3];
                    state[ci] = gmul(s0, 2) ^ gmul(s1, 3) ^ s2 ^ s3;
                    state[ci + 1] = s0 ^ gmul(s1, 2) ^ gmul(s2, 3) ^ s3;
                    state[ci + 2] = s0 ^ s1 ^ gmul(s2, 2) ^ gmul(s3, 3);
                    state[ci + 3] = gmul(s0, 3) ^ s1 ^ s2 ^ gmul(s3, 2);
                }
                // AddRoundKey
                for (int i = 0; i < 16; i++) state[i] ^= roundKeys[round * 16 + i];
            }
            // Final round (no MixColumns)
            for (int i = 0; i < 16; i++) state[i] = AES_SBOX[state[i]];
            {
                uint8_t t;
                t = state[1]; state[1] = state[5]; state[5] = state[9]; state[9] = state[13]; state[13] = t;
                t = state[2]; state[2] = state[10]; state[10] = t;
                t = state[6]; state[6] = state[14]; state[14] = t;
                t = state[11]; state[11] = state[7]; state[7] = state[3]; state[3] = state[15]; state[15] = t;
            }
            for (int i = 0; i < 16; i++) state[i] ^= roundKeys[160 + i];
            memcpy(output, state, 16);
        }

        // AES-128-CBC encrypt (for Algorithm 2.B)
        static void aes128EncryptCBC(const uint8_t key[16], const uint8_t iv[16],
            const uint8_t* input, size_t inputLen,
            std::vector<uint8_t>& output) {
            uint8_t roundKeys[176];
            aes128KeyExpansion(key, roundKeys);
            size_t numBlocks = inputLen / 16;
            output.resize(numBlocks * 16);
            uint8_t prev[16];
            memcpy(prev, iv, 16);
            for (size_t b = 0; b < numBlocks; b++) {
                uint8_t block[16];
                for (int i = 0; i < 16; i++) block[i] = input[b * 16 + i] ^ prev[i];
                aes128EncryptBlock(block, output.data() + b * 16, roundKeys);
                memcpy(prev, output.data() + b * 16, 16);
            }
        }

        // Keep old function as wrapper
        static void aes128DecryptBlock(const uint8_t input[16], uint8_t output[16], const uint8_t roundKeys[176]) {
            aesDecryptBlock(input, output, roundKeys, 10);
        }

        // =====================================================
        // SHA-256 Implementation
        // =====================================================
        struct SHA256 {
            uint32_t state[8];
            uint64_t count;
            uint8_t buffer[64];

            static const uint32_t K[64];

            static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
            static uint32_t Ch(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
            static uint32_t Maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
            static uint32_t Sig0(uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
            static uint32_t Sig1(uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
            static uint32_t sig0(uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
            static uint32_t sig1(uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

            static void transform(uint32_t st[8], const uint8_t block[64]) {
                uint32_t W[64];
                for (int i = 0; i < 16; i++)
                    W[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
                    ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
                for (int i = 16; i < 64; i++)
                    W[i] = sig1(W[i - 2]) + W[i - 7] + sig0(W[i - 15]) + W[i - 16];

                uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
                for (int i = 0; i < 64; i++) {
                    uint32_t T1 = h + Sig1(e) + Ch(e, f, g) + K[i] + W[i];
                    uint32_t T2 = Sig0(a) + Maj(a, b, c);
                    h = g; g = f; f = e; e = d + T1; d = c; c = b; b = a; a = T1 + T2;
                }
                st[0] += a; st[1] += b; st[2] += c; st[3] += d;
                st[4] += e; st[5] += f; st[6] += g; st[7] += h;
            }

            void init() {
                state[0] = 0x6a09e667; state[1] = 0xbb67ae85; state[2] = 0x3c6ef372; state[3] = 0xa54ff53a;
                state[4] = 0x510e527f; state[5] = 0x9b05688c; state[6] = 0x1f83d9ab; state[7] = 0x5be0cd19;
                count = 0; memset(buffer, 0, 64);
            }

            void update(const uint8_t* data, size_t len) {
                size_t index = (size_t)(count & 0x3F); count += len; size_t i = 0;
                if (index) {
                    size_t part = 64 - index;
                    if (len >= part) { memcpy(buffer + index, data, part); transform(state, buffer); i = part; index = 0; }
                    else { memcpy(buffer + index, data, len); return; }
                }
                for (; i + 64 <= len; i += 64) transform(state, data + i);
                if (i < len) memcpy(buffer, data + i, len - i);
            }

            void digest(uint8_t result[32]) {
                uint64_t bitCount = count * 8;
                uint8_t pad = 0x80; update(&pad, 1);
                uint8_t z = 0; while ((count & 0x3F) != 56) update(&z, 1);
                uint8_t bits[8];
                for (int i = 0; i < 8; i++) bits[i] = (uint8_t)(bitCount >> (56 - i * 8));
                update(bits, 8);
                for (int i = 0; i < 8; i++) {
                    result[i * 4] = (uint8_t)(state[i] >> 24);
                    result[i * 4 + 1] = (uint8_t)(state[i] >> 16);
                    result[i * 4 + 2] = (uint8_t)(state[i] >> 8);
                    result[i * 4 + 3] = (uint8_t)(state[i]);
                }
            }

            static void hash(const uint8_t* data, size_t len, uint8_t result[32]) {
                SHA256 ctx; ctx.init(); ctx.update(data, len); ctx.digest(result);
            }
        };

        const uint32_t SHA256::K[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
        };

        // =====================================================
        // SHA-384 / SHA-512 Implementation (both use 64-bit words)
        // SHA-384 = SHA-512 with different IV, output truncated to 48 bytes
        // =====================================================
        struct SHA512 {
            uint64_t state[8];
            uint64_t count;
            uint8_t buffer[128];

            static const uint64_t K[80];

            static uint64_t rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }
            static uint64_t Ch(uint64_t x, uint64_t y, uint64_t z) { return (x & y) ^ (~x & z); }
            static uint64_t Maj(uint64_t x, uint64_t y, uint64_t z) { return (x & y) ^ (x & z) ^ (y & z); }
            static uint64_t Sig0(uint64_t x) { return rotr64(x, 28) ^ rotr64(x, 34) ^ rotr64(x, 39); }
            static uint64_t Sig1(uint64_t x) { return rotr64(x, 14) ^ rotr64(x, 18) ^ rotr64(x, 41); }
            static uint64_t sig0(uint64_t x) { return rotr64(x, 1) ^ rotr64(x, 8) ^ (x >> 7); }
            static uint64_t sig1(uint64_t x) { return rotr64(x, 19) ^ rotr64(x, 61) ^ (x >> 6); }

            static void transform(uint64_t st[8], const uint8_t block[128]) {
                uint64_t W[80];
                for (int i = 0; i < 16; i++) {
                    W[i] = 0;
                    for (int j = 0; j < 8; j++) W[i] = (W[i] << 8) | block[i * 8 + j];
                }
                for (int i = 16; i < 80; i++)
                    W[i] = sig1(W[i - 2]) + W[i - 7] + sig0(W[i - 15]) + W[i - 16];

                uint64_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
                for (int i = 0; i < 80; i++) {
                    uint64_t T1 = h + Sig1(e) + Ch(e, f, g) + K[i] + W[i];
                    uint64_t T2 = Sig0(a) + Maj(a, b, c);
                    h = g; g = f; f = e; e = d + T1; d = c; c = b; b = a; a = T1 + T2;
                }
                st[0] += a; st[1] += b; st[2] += c; st[3] += d;
                st[4] += e; st[5] += f; st[6] += g; st[7] += h;
            }

            void initSHA512() {
                state[0] = 0x6a09e667f3bcc908ULL; state[1] = 0xbb67ae8584caa73bULL;
                state[2] = 0x3c6ef372fe94f82bULL; state[3] = 0xa54ff53a5f1d36f1ULL;
                state[4] = 0x510e527fade682d1ULL; state[5] = 0x9b05688c2b3e6c1fULL;
                state[6] = 0x1f83d9abfb41bd6bULL; state[7] = 0x5be0cd19137e2179ULL;
                count = 0; memset(buffer, 0, 128);
            }
            void initSHA384() {
                state[0] = 0xcbbb9d5dc1059ed8ULL; state[1] = 0x629a292a367cd507ULL;
                state[2] = 0x9159015a3070dd17ULL; state[3] = 0x152fecd8f70e5939ULL;
                state[4] = 0x67332667ffc00b31ULL; state[5] = 0x8eb44a8768581511ULL;
                state[6] = 0xdb0c2e0d64f98fa7ULL; state[7] = 0x47b5481dbefa4fa4ULL;
                count = 0; memset(buffer, 0, 128);
            }

            void update(const uint8_t* data, size_t len) {
                size_t index = (size_t)(count & 0x7F); count += len; size_t i = 0;
                if (index) {
                    size_t part = 128 - index;
                    if (len >= part) { memcpy(buffer + index, data, part); transform(state, buffer); i = part; }
                    else { memcpy(buffer + index, data, len); return; }
                }
                for (; i + 128 <= len; i += 128) transform(state, data + i);
                if (i < len) memcpy(buffer, data + i, len - i);
            }

            void digest(uint8_t* result, int outLen) {
                uint64_t bitCount = count * 8;
                uint8_t pad = 0x80; update(&pad, 1);
                uint8_t z = 0; while ((count & 0x7F) != 112) update(&z, 1);
                uint8_t bits[16]; memset(bits, 0, 8);
                for (int i = 0; i < 8; i++) bits[8 + i] = (uint8_t)(bitCount >> (56 - i * 8));
                update(bits, 16);
                int words = outLen / 8;
                if (words > 8) words = 8;
                for (int i = 0; i < words; i++) {
                    result[i * 8] = (uint8_t)(state[i] >> 56); result[i * 8 + 1] = (uint8_t)(state[i] >> 48);
                    result[i * 8 + 2] = (uint8_t)(state[i] >> 40); result[i * 8 + 3] = (uint8_t)(state[i] >> 32);
                    result[i * 8 + 4] = (uint8_t)(state[i] >> 24); result[i * 8 + 5] = (uint8_t)(state[i] >> 16);
                    result[i * 8 + 6] = (uint8_t)(state[i] >> 8);  result[i * 8 + 7] = (uint8_t)(state[i]);
                }
            }

            // SHA-256 of data
            static void hashSHA256(const uint8_t* data, size_t len, uint8_t result[32]) {
                SHA256::hash(data, len, result);
            }
            // SHA-384 of data (output 48 bytes)
            static void hashSHA384(const uint8_t* data, size_t len, uint8_t result[48]) {
                SHA512 ctx; ctx.initSHA384(); ctx.update(data, len); ctx.digest(result, 48);
            }
            // SHA-512 of data (output 64 bytes)
            static void hashSHA512(const uint8_t* data, size_t len, uint8_t result[64]) {
                SHA512 ctx; ctx.initSHA512(); ctx.update(data, len); ctx.digest(result, 64);
            }
        };

        const uint64_t SHA512::K[80] = {
            0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
            0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
            0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
            0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
            0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
            0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
            0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
            0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
            0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
            0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
            0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
            0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
            0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
            0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
            0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
            0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
            0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
            0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
            0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
            0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL
        };

        // =====================================================
        // Algorithm 2.B (ISO 32000-2) - used for R=6 password verification
        // Iterative hash: SHA-256 → SHA-384/SHA-512 based on AES output
        // =====================================================
        static void algorithm2B(const uint8_t* password, size_t passLen,
            const uint8_t* input, size_t inputLen,
            const uint8_t* userKey, size_t userKeyLen,
            uint8_t result[32])
        {
            // Initial hash: SHA-256(input)
            uint8_t K[64]; // up to 64 bytes (SHA-512 output)
            SHA256::hash(input, inputLen, K);
            int Klen = 32;

            int round = 0;
            while (true) {
                // K1 = (password + K[0:Klen] + userKey) repeated 64 times
                size_t seqLen = passLen + (size_t)Klen + userKeyLen;
                std::vector<uint8_t> K1(seqLen * 64);
                for (int r = 0; r < 64; r++) {
                    size_t off = r * seqLen;
                    if (passLen > 0) memcpy(K1.data() + off, password, passLen);
                    memcpy(K1.data() + off + passLen, K, Klen);
                    if (userKeyLen > 0) memcpy(K1.data() + off + passLen + Klen, userKey, userKeyLen);
                }

                // E = AES-128-CBC(key=K[0:16], iv=K[16:32], data=K1)
                std::vector<uint8_t> E;
                // Pad K1 to multiple of 16 if needed
                size_t totalLen = K1.size();
                size_t padded = ((totalLen + 15) / 16) * 16;
                if (padded > totalLen) K1.resize(padded, 0);
                aes128EncryptCBC(K, K + 16, K1.data(), padded, E);

                // mod3 = (sum of FIRST 16 bytes of E) % 3
                // Per ISO 32000-2: "Take the first 16 bytes of E as an unsigned 
                // big-endian integer and compute the remainder, modulo 3."
                // Since 256 ≡ 1 (mod 3), sum of bytes mod 3 = big-endian int mod 3
                uint32_t sum = 0;
                if (E.size() >= 16) {
                    for (size_t i = 0; i < 16; i++) sum += E[i];
                }
                int mod3 = sum % 3;

                if (mod3 == 0) {
                    SHA256::hash(E.data(), E.size(), K);
                    Klen = 32;
                }
                else if (mod3 == 1) {
                    SHA512::hashSHA384(E.data(), E.size(), K);
                    Klen = 48;
                }
                else {
                    SHA512::hashSHA512(E.data(), E.size(), K);
                    Klen = 64;
                }

                round++;
                // Exit condition: round >= 64 AND last byte of E <= round - 32
                if (round >= 64 && E.size() > 0) {
                    uint8_t lastByte = E[E.size() - 1];
                    if (lastByte <= (uint8_t)(round - 32)) break;
                }
                // Safety limit
                if (round > 1000) break;
            }

            memcpy(result, K, 32);
        }
        // =================================================================
        // SHA-1 (RFC 3174) - Required for certificate encryption key derivation
        // =================================================================
        struct SHA1 {
            uint32_t state[5];
            uint64_t count;
            uint8_t buffer[64];

            static uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

            static void transform(uint32_t st[5], const uint8_t block[64])
            {
                uint32_t W[80];
                for (int i = 0; i < 16; i++)
                    W[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
                    ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
                for (int i = 16; i < 80; i++)
                    W[i] = rotl(W[i - 3] ^ W[i - 8] ^ W[i - 14] ^ W[i - 16], 1);

                uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4];
                for (int i = 0; i < 80; i++)
                {
                    uint32_t f, k;
                    if (i < 20) { f = (b & c) | (~b & d);           k = 0x5A827999; }
                    else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1; }
                    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
                    else { f = b ^ c ^ d;                    k = 0xCA62C1D6; }

                    uint32_t temp = rotl(a, 5) + f + e + k + W[i];
                    e = d; d = c; c = rotl(b, 30); b = a; a = temp;
                }
                st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e;
            }

            void init()
            {
                state[0] = 0x67452301; state[1] = 0xEFCDAB89; state[2] = 0x98BADCFE;
                state[3] = 0x10325476; state[4] = 0xC3D2E1F0;
                count = 0; memset(buffer, 0, 64);
            }

            void update(const uint8_t* data, size_t len)
            {
                size_t index = (size_t)(count & 0x3F);
                count += len;
                size_t i = 0;
                if (index) {
                    size_t part = 64 - index;
                    if (len >= part) {
                        memcpy(buffer + index, data, part);
                        transform(state, buffer);
                        i = part; index = 0;
                    }
                    else {
                        memcpy(buffer + index, data, len);
                        return;
                    }
                }
                for (; i + 64 <= len; i += 64)
                    transform(state, data + i);
                if (i < len)
                    memcpy(buffer, data + i, len - i);
            }

            void digest(uint8_t result[20])
            {
                uint64_t bitCount = count * 8;
                uint8_t pad = 0x80;
                update(&pad, 1);
                uint8_t z = 0;
                while ((count & 0x3F) != 56) update(&z, 1);
                uint8_t bits[8];
                for (int i = 0; i < 8; i++)
                    bits[i] = (uint8_t)(bitCount >> (56 - i * 8));
                update(bits, 8);
                for (int i = 0; i < 5; i++) {
                    result[i * 4] = (uint8_t)(state[i] >> 24);
                    result[i * 4 + 1] = (uint8_t)(state[i] >> 16);
                    result[i * 4 + 2] = (uint8_t)(state[i] >> 8);
                    result[i * 4 + 3] = (uint8_t)(state[i]);
                }
            }

            static void hash(const uint8_t* data, size_t len, uint8_t result[20])
            {
                SHA1 ctx; ctx.init(); ctx.update(data, len); ctx.digest(result);
            }
        };

        // =================================================================
        // ASN.1 DER Parser - Full production-grade implementation
        // Handles: all tag classes, multi-byte tags, indefinite length,
        // constructed OCTET STRING, nested structures
        // =================================================================

        // Parse tag byte(s), return true on success
        static bool parseAsn1Tag(const uint8_t* data, size_t dataLen, size_t& offset,
            uint8_t& tagClass, bool& constructed, uint32_t& tagNumber)
        {
            if (offset >= dataLen) return false;
            uint8_t b = data[offset++];
            tagClass = (b >> 6) & 0x03;
            constructed = (b & 0x20) != 0;
            tagNumber = b & 0x1F;

            if (tagNumber == 0x1F) {
                // Long form tag
                tagNumber = 0;
                for (int i = 0; i < 5; i++) { // max 5 continuation bytes (enough for 35-bit tag)
                    if (offset >= dataLen) return false;
                    uint8_t next = data[offset++];
                    tagNumber = (tagNumber << 7) | (next & 0x7F);
                    if ((next & 0x80) == 0) break;
                    if (i == 4) return false; // tag too large
                }
            }
            return true;
        }

        // Parse length field, return true on success
        // Sets isIndefinite=true for indefinite length encoding (0x80)
        static bool parseAsn1Length(const uint8_t* data, size_t dataLen, size_t& offset,
            size_t& length, bool& isIndefinite)
        {
            if (offset >= dataLen) return false;
            uint8_t b = data[offset++];
            isIndefinite = false;

            if (b < 0x80) {
                length = b;
            }
            else if (b == 0x80) {
                isIndefinite = true;
                length = 0; // will be determined by searching for end-of-contents
            }
            else {
                int numBytes = b & 0x7F;
                if (numBytes > 4 || offset + numBytes > dataLen) return false;
                length = 0;
                for (int i = 0; i < numBytes; i++)
                    length = (length << 8) | data[offset++];
            }
            return true;
        }

        // Find end-of-contents octets (0x00, 0x00) for indefinite length
        static size_t findEndOfContents(const uint8_t* data, size_t dataLen, size_t offset)
        {
            while (offset + 1 < dataLen) {
                if (data[offset] == 0x00 && data[offset + 1] == 0x00)
                    return offset;
                // Skip nested TLVs properly
                size_t savedOffset = offset;
                uint8_t tc; bool con; uint32_t tn;
                if (!parseAsn1Tag(data, dataLen, offset, tc, con, tn)) return dataLen;
                size_t len; bool indef;
                if (!parseAsn1Length(data, dataLen, offset, len, indef)) return dataLen;
                if (indef) {
                    offset = findEndOfContents(data, dataLen, offset);
                    if (offset + 2 <= dataLen) offset += 2; // skip EOC
                }
                else {
                    offset += len;
                }
            }
            return dataLen; // not found
        }

        // OID byte sequence to dotted string
        static std::string oidBytesToString(const uint8_t* data, size_t len)
        {
            if (len == 0) return "";
            std::string result;
            // First byte encodes first two components
            uint32_t first = data[0] / 40;
            uint32_t second = data[0] % 40;
            result = std::to_string(first) + "." + std::to_string(second);

            uint32_t val = 0;
            for (size_t i = 1; i < len; i++) {
                val = (val << 7) | (data[i] & 0x7F);
                if ((data[i] & 0x80) == 0) {
                    result += "." + std::to_string(val);
                    val = 0;
                }
            }
            return result;
        }

    } // anonymous namespace

    // =================================================================
    // Asn1Element method implementations (in pdf namespace)
    // =================================================================
    std::string Asn1Element::oidToString() const
    {
        if (!isOid() || value.empty()) return "";
        return oidBytesToString(value.data(), value.size());
    }

    std::vector<uint8_t> Asn1Element::integerBytes() const
    {
        if (!isInteger()) return {};
        // Strip leading zero padding (sign byte for unsigned)
        std::vector<uint8_t> result = value;
        while (result.size() > 1 && result[0] == 0x00)
            result.erase(result.begin());
        return result;
    }

    int Asn1Element::integerToInt() const
    {
        if (!isInteger()) return 0;
        int result = 0;
        bool negative = (!value.empty() && (value[0] & 0x80) != 0);
        for (size_t i = 0; i < value.size() && i < 4; i++)
            result = (result << 8) | value[i];
        if (negative && value.size() < 4) {
            // Sign extend
            for (size_t i = value.size(); i < 4; i++)
                result |= (0xFF << (i * 8));
        }
        return result;
    }

    std::string Asn1Element::stringValue() const
    {
        return std::string(value.begin(), value.end());
    }

    bool Asn1Element::booleanValue() const
    {
        return !value.empty() && value[0] != 0;
    }

    const Asn1Element* Asn1Element::findContextChild(uint32_t n) const
    {
        for (const auto& c : children)
            if (c.tagClass == 2 && c.tagNumber == n) return &c;
        return nullptr;
    }

    // =================================================================
    // ASN.1 DER parsing - PdfDocument static methods
    // =================================================================
    bool PdfDocument::parseAsn1Element(const uint8_t* data, size_t dataLen,
        size_t& offset, Asn1Element& elem)
    {
        size_t startOffset = offset;

        // Parse tag
        if (!parseAsn1Tag(data, dataLen, offset, elem.tagClass, elem.constructed, elem.tagNumber))
            return false;

        // Parse length
        size_t contentLen = 0;
        bool isIndefinite = false;
        if (!parseAsn1Length(data, dataLen, offset, contentLen, isIndefinite))
            return false;

        elem.headerLength = offset - startOffset;

        if (isIndefinite) {
            // Find end-of-contents
            size_t eocPos = findEndOfContents(data, dataLen, offset);
            contentLen = eocPos - offset;
            elem.contentLength = contentLen;
            elem.totalEncodedLength = (eocPos + 2) - startOffset; // +2 for EOC bytes
        }
        else {
            if (offset + contentLen > dataLen) return false;
            elem.contentLength = contentLen;
            elem.totalEncodedLength = elem.headerLength + contentLen;
        }

        // Store raw DER
        elem.rawDer.assign(data + startOffset, data + startOffset + elem.totalEncodedLength);

        if (elem.constructed) {
            // Parse children recursively
            size_t childEnd = offset + contentLen;
            while (offset < childEnd) {
                Asn1Element child;
                if (!parseAsn1Element(data, dataLen, offset, child))
                    break;
                elem.children.push_back(std::move(child));
            }
            offset = childEnd;
            if (isIndefinite) offset += 2; // skip EOC
        }
        else {
            // Primitive: store value bytes
            elem.value.assign(data + offset, data + offset + contentLen);
            offset += contentLen;
            if (isIndefinite) offset += 2;
        }

        return true;
    }

    bool PdfDocument::parseAsn1All(const uint8_t* data, size_t dataLen,
        std::vector<Asn1Element>& elements)
    {
        size_t offset = 0;
        while (offset < dataLen) {
            Asn1Element elem;
            if (!parseAsn1Element(data, dataLen, offset, elem))
                return !elements.empty(); // partial parse OK
            elements.push_back(std::move(elem));
        }
        return true;
    }

    // =================================================================
    // PKCS#7 / CMS EnvelopedData Parser
    // Parses ContentInfo → EnvelopedData → RecipientInfo(s)
    // =================================================================
    bool PdfDocument::parsePkcs7EnvelopedData(const uint8_t* data, size_t dataLen,
        Pkcs7EnvelopedData& result)
    {
        // Parse outer ContentInfo: SEQUENCE { OID, [0] content }
        Asn1Element contentInfo;
        size_t offset = 0;
        if (!parseAsn1Element(data, dataLen, offset, contentInfo))
        {
            LogDebug("PKCS7: Failed to parse ContentInfo SEQUENCE");
            return false;
        }

        if (!contentInfo.isSequence() || contentInfo.childCount() < 2)
        {
            LogDebug("PKCS7: ContentInfo is not a valid SEQUENCE (children=%zu)", contentInfo.childCount());
            return false;
        }

        // Verify contentType OID = 1.2.840.113549.1.7.3 (envelopedData)
        const auto* oidElem = contentInfo.childAt(0);
        if (!oidElem || !oidElem->isOid())
        {
            LogDebug("PKCS7: Missing contentType OID");
            return false;
        }
        std::string contentTypeOid = oidElem->oidToString();
        if (contentTypeOid != "1.2.840.113549.1.7.3")
        {
            LogDebug("PKCS7: ContentType is '%s', expected envelopedData (1.2.840.113549.1.7.3)",
                contentTypeOid.c_str());
            return false;
        }

        // [0] EXPLICIT wrapper → contains EnvelopedData SEQUENCE
        const auto* explicitWrap = contentInfo.childAt(1);
        if (!explicitWrap || !explicitWrap->isContextTag(0) || explicitWrap->children.empty())
        {
            LogDebug("PKCS7: Missing [0] EXPLICIT wrapper for EnvelopedData");
            return false;
        }

        const auto* envelopedSeq = explicitWrap->childAt(0);
        if (!envelopedSeq || !envelopedSeq->isSequence())
        {
            LogDebug("PKCS7: EnvelopedData is not a SEQUENCE");
            return false;
        }

        // EnvelopedData: SEQUENCE { version, [0]? originatorInfo, recipientInfos SET, encContentInfo, ... }
        size_t idx = 0;

        // version INTEGER
        if (idx >= envelopedSeq->childCount()) return false;
        const auto* versionElem = envelopedSeq->childAt(idx);
        if (versionElem && versionElem->isInteger()) {
            result.version = versionElem->integerToInt();
            idx++;
        }
        LogDebug("PKCS7: EnvelopedData version = %d", result.version);

        // [0] OriginatorInfo OPTIONAL (skip if present)
        if (idx < envelopedSeq->childCount()) {
            const auto* maybeOriginator = envelopedSeq->childAt(idx);
            if (maybeOriginator && maybeOriginator->isContextTag(0)) {
                idx++; // skip OriginatorInfo
            }
        }

        // RecipientInfos SET
        if (idx >= envelopedSeq->childCount()) {
            LogDebug("PKCS7: Missing RecipientInfos SET");
            return false;
        }
        const auto* recipientSet = envelopedSeq->childAt(idx);
        if (!recipientSet || !recipientSet->isSet()) {
            LogDebug("PKCS7: RecipientInfos is not a SET");
            return false;
        }
        idx++;

        // Parse each RecipientInfo
        for (size_t ri = 0; ri < recipientSet->childCount(); ri++)
        {
            const auto* riSeq = recipientSet->childAt(ri);
            if (!riSeq || !riSeq->isSequence()) continue;

            Pkcs7RecipientInfo info;

            size_t riIdx = 0;
            // version INTEGER
            if (riIdx < riSeq->childCount() && riSeq->childAt(riIdx)->isInteger()) {
                info.version = riSeq->childAt(riIdx)->integerToInt();
                riIdx++;
            }

            if (info.version == 0 || info.version == 1) {
                // KeyTransRecipientInfo: { version, issuerAndSerial, keyEncAlg, encryptedKey }
                // issuerAndSerialNumber SEQUENCE { issuer, serial }
                if (riIdx >= riSeq->childCount()) continue;
                const auto* issuerSerial = riSeq->childAt(riIdx);
                if (issuerSerial && issuerSerial->isSequence() && issuerSerial->childCount() >= 2) {
                    // issuer Name (raw DER)
                    const auto* issuerElem = issuerSerial->childAt(0);
                    if (issuerElem) info.issuerDer = issuerElem->rawDer;

                    // serialNumber INTEGER
                    const auto* serialElem = issuerSerial->childAt(1);
                    if (serialElem && serialElem->isInteger())
                        info.serialNumber = serialElem->integerBytes();
                }
                riIdx++;

                // keyEncryptionAlgorithm AlgorithmIdentifier
                if (riIdx < riSeq->childCount()) {
                    const auto* algId = riSeq->childAt(riIdx);
                    if (algId && algId->isSequence() && algId->childCount() >= 1) {
                        const auto* algOid = algId->childAt(0);
                        if (algOid && algOid->isOid())
                            info.keyEncAlgorithmOid = algOid->oidToString();
                        if (algId->childCount() >= 2)
                            info.keyEncAlgorithmParams = algId->childAt(1)->value;
                    }
                    riIdx++;
                }

                // encryptedKey OCTET STRING
                if (riIdx < riSeq->childCount()) {
                    const auto* encKeyElem = riSeq->childAt(riIdx);
                    if (encKeyElem && encKeyElem->isOctetString())
                        info.encryptedKey = encKeyElem->value;
                    riIdx++;
                }

            }
            else if (info.version == 2) {
                // KeyTransRecipientInfo v2 with SubjectKeyIdentifier
                // or KeyAgreeRecipientInfo
                // For v2 KeyTrans: { version, [0] subjectKeyId, keyEncAlg, encryptedKey }
                if (riIdx < riSeq->childCount()) {
                    const auto* ridElem = riSeq->childAt(riIdx);
                    if (ridElem) {
                        if (ridElem->isContextTag(0)) {
                            // SubjectKeyIdentifier
                            if (ridElem->constructed && ridElem->childCount() > 0)
                                info.subjectKeyId = ridElem->childAt(0)->value;
                            else
                                info.subjectKeyId = ridElem->value;
                        }
                        else if (ridElem->isSequence()) {
                            // IssuerAndSerialNumber
                            if (ridElem->childCount() >= 2) {
                                info.issuerDer = ridElem->childAt(0)->rawDer;
                                if (ridElem->childAt(1)->isInteger())
                                    info.serialNumber = ridElem->childAt(1)->integerBytes();
                            }
                        }
                    }
                    riIdx++;
                }

                // keyEncryptionAlgorithm
                if (riIdx < riSeq->childCount()) {
                    const auto* algId = riSeq->childAt(riIdx);
                    if (algId && algId->isSequence() && algId->childCount() >= 1) {
                        info.keyEncAlgorithmOid = algId->childAt(0)->oidToString();
                    }
                    riIdx++;
                }

                // encryptedKey
                if (riIdx < riSeq->childCount()) {
                    const auto* encKeyElem = riSeq->childAt(riIdx);
                    if (encKeyElem && encKeyElem->isOctetString())
                        info.encryptedKey = encKeyElem->value;
                    riIdx++;
                }
            }

            LogDebug("PKCS7: RecipientInfo[%zu]: version=%d, issuerDer=%zu bytes, serial=%zu bytes, "
                "keyEnc=%s, encKey=%zu bytes",
                ri, info.version, info.issuerDer.size(), info.serialNumber.size(),
                info.keyEncAlgorithmOid.c_str(), info.encryptedKey.size());

            result.recipients.push_back(std::move(info));
        }

        // EncryptedContentInfo SEQUENCE
        if (idx < envelopedSeq->childCount()) {
            const auto* encContentSeq = envelopedSeq->childAt(idx);
            if (encContentSeq && encContentSeq->isSequence()) {
                size_t eci = 0;
                // contentType OID
                if (eci < encContentSeq->childCount() && encContentSeq->childAt(eci)->isOid()) {
                    result.encryptedContentInfo.contentTypeOid = encContentSeq->childAt(eci)->oidToString();
                    eci++;
                }
                // contentEncryptionAlgorithm AlgorithmIdentifier
                if (eci < encContentSeq->childCount() && encContentSeq->childAt(eci)->isSequence()) {
                    const auto* algId = encContentSeq->childAt(eci);
                    if (algId->childCount() >= 1 && algId->childAt(0)->isOid())
                        result.encryptedContentInfo.encAlgorithmOid = algId->childAt(0)->oidToString();
                    if (algId->childCount() >= 2 && algId->childAt(1)->isOctetString())
                        result.encryptedContentInfo.encAlgorithmIv = algId->childAt(1)->value;
                    eci++;
                }
                // [0] IMPLICIT encryptedContent OPTIONAL
                if (eci < encContentSeq->childCount()) {
                    const auto* encContent = encContentSeq->childAt(eci);
                    if (encContent && encContent->isContextTag(0))
                        result.encryptedContentInfo.encryptedContent = encContent->value;
                }
            }
            idx++;
        }

        LogDebug("PKCS7: Parsed %zu recipients, contentEnc=%s",
            result.recipients.size(),
            result.encryptedContentInfo.encAlgorithmOid.c_str());

        return !result.recipients.empty();
    }

    // AES-CBC decrypt for PDF streams (supports both AES-128 and AES-256)
    // PDF format: [16 bytes IV] [ciphertext blocks...] with PKCS#7 padding
    bool PdfDocument::aesDecryptCBC(const std::vector<uint8_t>& key,
        const uint8_t* input, size_t inputLen,
        std::vector<uint8_t>& output)
    {
        output.clear();

        // Need at least IV (16 bytes) + one block (16 bytes)
        if (inputLen < 32 || (inputLen - 16) % 16 != 0)
        {
            if (inputLen < 16)
                return false;
        }

        // Determine AES mode by key size
        bool is256 = (key.size() >= 32);

        // IV = first 16 bytes
        const uint8_t* iv = input;
        const uint8_t* ciphertext = input + 16;
        size_t cipherLen = inputLen - 16;

        if (cipherLen == 0 || cipherLen % 16 != 0)
            return false;

        size_t numBlocks = cipherLen / 16;
        output.resize(cipherLen);

        if (is256)
        {
            // AES-256: 14 rounds, 240 bytes round keys
            uint8_t aesKey[32];
            memcpy(aesKey, key.data(), 32);
            uint8_t roundKeys[240];
            aes256KeyExpansion(aesKey, roundKeys);

            const uint8_t* prevBlock = iv;
            for (size_t b = 0; b < numBlocks; b++)
            {
                const uint8_t* encBlock = ciphertext + b * 16;
                uint8_t decBlock[16];
                aesDecryptBlock(encBlock, decBlock, roundKeys, 14);
                for (int i = 0; i < 16; i++)
                    output[b * 16 + i] = decBlock[i] ^ prevBlock[i];
                prevBlock = encBlock;
            }
        }
        else
        {
            // AES-128: 10 rounds, 176 bytes round keys
            uint8_t aesKey[16];
            size_t keyLen = std::min(key.size(), (size_t)16);
            memcpy(aesKey, key.data(), keyLen);
            if (keyLen < 16) memset(aesKey + keyLen, 0, 16 - keyLen);

            uint8_t roundKeys[176];
            aes128KeyExpansion(aesKey, roundKeys);

            const uint8_t* prevBlock = iv;
            for (size_t b = 0; b < numBlocks; b++)
            {
                const uint8_t* encBlock = ciphertext + b * 16;
                uint8_t decBlock[16];
                aes128DecryptBlock(encBlock, decBlock, roundKeys);
                for (int i = 0; i < 16; i++)
                    output[b * 16 + i] = decBlock[i] ^ prevBlock[i];
                prevBlock = encBlock;
            }
        }

        // Remove PKCS#7 padding
        if (!output.empty())
        {
            uint8_t padByte = output.back();
            if (padByte > 0 && padByte <= 16)
            {
                bool validPad = true;
                for (size_t i = output.size() - padByte; i < output.size(); i++)
                {
                    if (output[i] != padByte) { validPad = false; break; }
                }
                if (validPad)
                    output.resize(output.size() - padByte);
            }
        }

        return true;
    }

    // Parse a PDF literal string from raw bytes (handles escape sequences)
    std::vector<uint8_t> PdfDocument::parsePdfLiteralString(
        const uint8_t* data, size_t len, size_t startAfterParen, size_t& endPos) const
    {
        std::vector<uint8_t> result;
        size_t i = startAfterParen;
        int depth = 1;

        while (i < len)
        {
            uint8_t b = data[i];
            if (b == '(')
            {
                depth++;
                result.push_back(b);
            }
            else if (b == ')')
            {
                depth--;
                if (depth == 0) { endPos = i + 1; return result; }
                result.push_back(b);
            }
            else if (b == '\\')
            {
                i++;
                if (i >= len) break;
                uint8_t nb = data[i];
                switch (nb)
                {
                case 'n': result.push_back(0x0A); break;
                case 'r': result.push_back(0x0D); break;
                case 't': result.push_back(0x09); break;
                case 'b': result.push_back(0x08); break;
                case 'f': result.push_back(0x0C); break;
                case '(': result.push_back('('); break;
                case ')': result.push_back(')'); break;
                case '\\': result.push_back('\\'); break;
                default:
                    if (nb >= '0' && nb <= '7')
                    {
                        // Octal escape
                        int val = nb - '0';
                        for (int k = 0; k < 2; ++k)
                        {
                            if (i + 1 < len && data[i + 1] >= '0' && data[i + 1] <= '7')
                            {
                                i++;
                                val = val * 8 + (data[i] - '0');
                            }
                            else break;
                        }
                        result.push_back((uint8_t)(val & 0xFF));
                    }
                    else if (nb == '\r')
                    {
                        // Line continuation
                        if (i + 1 < len && data[i + 1] == '\n') i++;
                    }
                    else if (nb == '\n')
                    {
                        // Line continuation
                    }
                    else
                    {
                        result.push_back(nb);
                    }
                    break;
                }
            }
            else
            {
                result.push_back(b);
            }
            i++;
        }
        endPos = i;
        return result;
    }

    // Initialize encryption by parsing /Encrypt dict and /ID from trailer
    bool PdfDocument::initEncryption()
    {
        if (!_trailer) return false;

        // We need to parse /Encrypt and /ID directly from the raw PDF data
        // because the parser may not handle binary strings in /O and /U correctly.

        // Find the Encrypt dictionary object number
        auto encryptRef = _trailer->get("/Encrypt");
        if (!encryptRef) encryptRef = _trailer->get("Encrypt");
        if (!encryptRef) return false;

        auto ref = std::dynamic_pointer_cast<PdfIndirectRef>(encryptRef);
        int encryptObjNum = -1;
        if (ref) encryptObjNum = ref->objNum;

        // Find the Encrypt dictionary in the raw data
        // We need to parse O, U values from raw bytes because they contain binary data
        // that may have escape sequences

        // Search for the encrypt object in raw data
        std::string searchPattern = std::to_string(encryptObjNum >= 0 ? encryptObjNum : 30) + " 0 obj";
        size_t encObjPos = std::string::npos;

        for (size_t pos = 0; pos < _data.size() - searchPattern.size(); ++pos)
        {
            if (memcmp(&_data[pos], searchPattern.c_str(), searchPattern.size()) == 0)
            {
                encObjPos = pos;
                break;
            }
        }

        if (encObjPos == std::string::npos)
        {
            LogDebug("PDF Encrypt: Cannot find encrypt object %d in raw data", encryptObjNum);
            return false;
        }

        // Find endobj
        size_t encObjEnd = std::string::npos;
        for (size_t p = encObjPos; p < _data.size() - 6; ++p)
        {
            if (memcmp(&_data[p], "endobj", 6) == 0) { encObjEnd = p; break; }
        }
        if (encObjEnd == std::string::npos) return false;

        const uint8_t* encData = &_data[encObjPos];
        size_t encLen = encObjEnd - encObjPos;

        // Parse /V, /R, /Length from the encrypt dict (use parsed objects for these)
        std::set<int> visited;
        auto encryptObj = resolveIndirect(encryptRef, visited);
        auto encryptDict = std::dynamic_pointer_cast<PdfDictionary>(encryptObj);

        if (encryptDict)
        {
            // ===== Check /Filter to determine encryption handler =====
            visited.clear();
            auto filterObj = resolveIndirect(encryptDict->get("/Filter"), visited);
            auto filterName = std::dynamic_pointer_cast<PdfName>(filterObj);
            std::string filter = filterName ? filterName->value : "/Standard";

            if (filter == "/Adobe.PubSec" || filter == "Adobe.PubSec")
            {
                LogDebug("PDF Encrypt: Certificate-based encryption (/Adobe.PubSec)");
                _isEncrypted = true;
                _isCertEncrypted = true;
                return initCertEncryption(encryptDict, encData, encLen);
            }

            visited.clear();
            auto vObj = std::dynamic_pointer_cast<PdfNumber>(resolveIndirect(encryptDict->get("/V"), visited));
            visited.clear();
            auto rObj = std::dynamic_pointer_cast<PdfNumber>(resolveIndirect(encryptDict->get("/R"), visited));
            visited.clear();
            auto lenObj = std::dynamic_pointer_cast<PdfNumber>(resolveIndirect(encryptDict->get("/Length"), visited));
            visited.clear();
            auto pObj = std::dynamic_pointer_cast<PdfNumber>(resolveIndirect(encryptDict->get("/P"), visited));

            if (vObj) _encryptV = (int)vObj->value;
            if (rObj) _encryptR = (int)rObj->value;
            if (lenObj) _encryptKeyLength = (int)lenObj->value / 8; // bits to bytes
            if (pObj) {
                // P can be stored as unsigned in PDF but is actually signed 32-bit
                double pVal = pObj->value;
                if (pVal > 2147483647.0)
                    _encryptP = (int32_t)((uint32_t)pVal);
                else
                    _encryptP = (int32_t)pVal;
            }
        }

        // Default key length
        if (_encryptKeyLength <= 0) _encryptKeyLength = (_encryptV == 1) ? 5 : (_encryptV == 5 ? 32 : 16);
        if (_encryptV != 5 && _encryptKeyLength > 16) _encryptKeyLength = 16;

        LogDebug("PDF Encrypt: V=%d, R=%d, KeyLen=%d bytes, P=%d",
            _encryptV, _encryptR, _encryptKeyLength, _encryptP);

        // Support V=1 (RC4-40), V=2 (RC4-128), V=4 (AES-128), V=5 (AES-256)
        if (_encryptV > 5 || _encryptR > 6)
        {
            LogDebug("PDF Encrypt: Unsupported encryption V=%d R=%d", _encryptV, _encryptR);
            return false;
        }

        // V=5: AES-256 - always AES, key length 32
        if (_encryptV == 5)
        {
            _useAES = true;
            _encryptKeyLength = 32;
            LogDebug("PDF Encrypt: V=5 R=%d - AES-256 mode", _encryptR);
        }

        // For V=4, parse Crypt Filters to determine AES vs RC4
        if (_encryptV == 4 && encryptDict)
        {
            _useAES = false; // default to RC4 unless CF says AESV2

            // Get /CF dictionary
            std::set<int> vcf;
            auto cfObj = resolveIndirect(encryptDict->get("/CF"), vcf);
            auto cfDict = std::dynamic_pointer_cast<PdfDictionary>(cfObj);

            // Get /StmF (stream filter name, usually /StdCF)
            vcf.clear();
            auto stmfObj = resolveIndirect(encryptDict->get("/StmF"), vcf);
            auto stmfName = std::dynamic_pointer_cast<PdfName>(stmfObj);
            std::string stmfFilter = stmfName ? stmfName->value : "/StdCF";

            LogDebug("PDF Encrypt: V=4, StmF=%s", stmfFilter.c_str());

            if (cfDict)
            {
                // Look up the crypt filter by name (e.g. /StdCF)
                vcf.clear();
                auto filterObj = resolveIndirect(cfDict->get(stmfFilter), vcf);
                if (!filterObj) {
                    // Try without leading /
                    vcf.clear();
                    std::string noSlash = stmfFilter.size() > 1 && stmfFilter[0] == '/' ? stmfFilter.substr(1) : stmfFilter;
                    filterObj = resolveIndirect(cfDict->get(noSlash), vcf);
                }
                auto filterDict = std::dynamic_pointer_cast<PdfDictionary>(filterObj);

                if (filterDict)
                {
                    vcf.clear();
                    auto cfmObj = resolveIndirect(filterDict->get("/CFM"), vcf);
                    auto cfmName = std::dynamic_pointer_cast<PdfName>(cfmObj);
                    std::string cfm = cfmName ? cfmName->value : "";

                    LogDebug("PDF Encrypt: CF filter CFM=%s", cfm.c_str());

                    if (cfm == "/AESV2" || cfm == "AESV2")
                    {
                        _useAES = true;
                        LogDebug("PDF Encrypt: Using AES-128-CBC encryption");
                    }
                    else
                    {
                        LogDebug("PDF Encrypt: Using RC4 encryption (CFM=%s)", cfm.c_str());
                    }
                }
                else
                {
                    // No CF details found, assume AES for V=4 R=4
                    _useAES = true;
                    LogDebug("PDF Encrypt: No CF filter dict found, assuming AES for V=4");
                }
            }
            else
            {
                // No CF dictionary, assume AES for V=4 R=4
                _useAES = true;
                LogDebug("PDF Encrypt: No /CF dictionary, assuming AES for V=4");
            }
        }

        // Parse /O and /U from raw bytes (critical - contains binary escape sequences)
        // Helper: parse hex string <...> from raw bytes
        auto parseHexString = [](const uint8_t* data, size_t len, size_t startAfterAngle) -> std::vector<uint8_t>
            {
                std::vector<uint8_t> result;
                std::string hexChars;
                for (size_t i = startAfterAngle; i < len; ++i)
                {
                    uint8_t c = data[i];
                    if (c == '>') break;
                    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
                        hexChars.push_back((char)c);
                }
                // Pad with 0 if odd length
                if (hexChars.size() % 2 != 0) hexChars.push_back('0');
                for (size_t i = 0; i + 1 < hexChars.size(); i += 2)
                {
                    char hex[3] = { hexChars[i], hexChars[i + 1], 0 };
                    result.push_back((uint8_t)strtol(hex, nullptr, 16));
                }
                return result;
            };

        // Helper: find key and parse either literal string (...) or hex string <...>
        auto findAndParseString = [&](const uint8_t* searchData, size_t searchLen, const char* key) -> std::vector<uint8_t>
            {
                std::string keyStr = key;
                for (size_t p = 0; p + keyStr.size() + 2 < searchLen; ++p)
                {
                    if (memcmp(searchData + p, keyStr.c_str(), keyStr.size()) == 0)
                    {
                        // Skip whitespace after key
                        size_t valPos = p + keyStr.size();
                        while (valPos < searchLen && (searchData[valPos] == ' ' || searchData[valPos] == '\r' || searchData[valPos] == '\n' || searchData[valPos] == '\t'))
                            valPos++;
                        if (valPos >= searchLen) return {};

                        if (searchData[valPos] == '(')
                        {
                            // Literal string
                            size_t endPos;
                            return parsePdfLiteralString(searchData, searchLen, valPos + 1, endPos);
                        }
                        else if (searchData[valPos] == '<' && valPos + 1 < searchLen && searchData[valPos + 1] != '<')
                        {
                            // Hex string (not dictionary <<)
                            return parseHexString(searchData, searchLen, valPos + 1);
                        }
                    }
                }
                return {};
            };

        _encryptO = findAndParseString(encData, encLen, "/O");
        _encryptU = findAndParseString(encData, encLen, "/U");

        // V=5: O and U are 48 bytes, also need OE and UE (32 bytes each)
        if (_encryptV == 5)
        {
            _encryptOE = findAndParseString(encData, encLen, "/OE");
            _encryptUE = findAndParseString(encData, encLen, "/UE");
            _encryptPerms = findAndParseString(encData, encLen, "/Perms");

            // Fallback to parsed dict for OE/UE/Perms
            if (encryptDict)
            {
                auto getStringBytesV5 = [&](const char* key) -> std::vector<uint8_t>
                    {
                        std::set<int> v;
                        auto obj = resolveIndirect(encryptDict->get(key), v);
                        if (!obj) return {};
                        auto str = std::dynamic_pointer_cast<PdfString>(obj);
                        if (str) return std::vector<uint8_t>(str->value.begin(), str->value.end());
                        return {};
                    };
                if (_encryptOE.size() != 32) _encryptOE = getStringBytesV5("/OE");
                if (_encryptUE.size() != 32) _encryptUE = getStringBytesV5("/UE");
                if (_encryptPerms.size() != 16) _encryptPerms = getStringBytesV5("/Perms");
                if (_encryptO.size() != 48) _encryptO = getStringBytesV5("/O");
                if (_encryptU.size() != 48) _encryptU = getStringBytesV5("/U");
            }

            int expectedOU = 48;
            if (_encryptO.size() < (size_t)expectedOU || _encryptU.size() < (size_t)expectedOU)
            {
                LogDebug("PDF Encrypt: V=5 Invalid O(%zu) or U(%zu) size (expected %d)",
                    _encryptO.size(), _encryptU.size(), expectedOU);
                return false;
            }
            if (_encryptUE.size() != 32)
            {
                LogDebug("PDF Encrypt: V=5 Invalid UE size (%zu, expected 32)", _encryptUE.size());
                return false;
            }

            LogDebug("PDF Encrypt: V=5 O=%zu, U=%zu, OE=%zu, UE=%zu, Perms=%zu",
                _encryptO.size(), _encryptU.size(), _encryptOE.size(),
                _encryptUE.size(), _encryptPerms.size());

            // V=5: Use SHA-256 based key derivation (no fileID needed)
            if (!computeEncryptionKeyV5())
            {
                LogDebug("PDF Encrypt: V=5 key derivation failed - password required");
                return false;
            }

            // Log the computed key
            {
                std::string kh;
                for (auto b : _encryptKey) { char buf[4]; snprintf(buf, sizeof(buf), "%02x", b); kh += buf; }
                LogDebug("PDF Encrypt: V=5 Computed key = %s", kh.c_str());
            }

            LogDebug("PDF Encrypt: V=5 encryption key derived successfully");
            return true;
        }

        // V <= 4: O and U are 32 bytes
        // Fallback: try parsed encrypt dictionary if raw parsing failed
        if ((_encryptO.size() != 32 || _encryptU.size() != 32) && encryptDict)
        {
            LogDebug("PDF Encrypt: Raw O/U parse failed (O=%zu, U=%zu), trying parsed dict", _encryptO.size(), _encryptU.size());
            auto getStringBytes = [&](const char* key) -> std::vector<uint8_t>
                {
                    std::set<int> v;
                    auto obj = resolveIndirect(encryptDict->get(key), v);
                    if (!obj) return {};
                    auto str = std::dynamic_pointer_cast<PdfString>(obj);
                    if (str) return std::vector<uint8_t>(str->value.begin(), str->value.end());
                    return {};
                };
            if (_encryptO.size() != 32) _encryptO = getStringBytes("/O");
            if (_encryptU.size() != 32) _encryptU = getStringBytes("/U");
        }

        if (_encryptO.size() != 32 || _encryptU.size() != 32)
        {
            LogDebug("PDF Encrypt: Invalid O(%zu) or U(%zu) size (expected 32)",
                _encryptO.size(), _encryptU.size());
            return false;
        }

        // Parse /ID from trailer
        // First try raw data search (handles both literal and hex strings)
        size_t trailerPos = std::string::npos;
        for (size_t p = _data.size() > 1000 ? _data.size() - 1000 : 0; p < _data.size() - 7; ++p)
        {
            if (memcmp(&_data[p], "trailer", 7) == 0)
            {
                trailerPos = p;
                // Don't break - we want the LAST trailer
            }
        }

        if (trailerPos != std::string::npos)
        {
            // Find /ID in trailer
            for (size_t p = trailerPos; p < _data.size() - 4; ++p)
            {
                if (memcmp(&_data[p], "/ID", 3) == 0)
                {
                    // Skip whitespace and [
                    size_t valPos = p + 3;
                    while (valPos < _data.size() && (_data[valPos] == ' ' || _data[valPos] == '[' || _data[valPos] == '\r' || _data[valPos] == '\n'))
                        valPos++;

                    if (valPos < _data.size())
                    {
                        if (_data[valPos] == '(')
                        {
                            // Literal string
                            size_t endPos;
                            _fileId = parsePdfLiteralString(_data.data(), _data.size(), valPos + 1, endPos);
                        }
                        else if (_data[valPos] == '<' && valPos + 1 < _data.size() && _data[valPos + 1] != '<')
                        {
                            // Hex string
                            _fileId = parseHexString(_data.data(), _data.size(), valPos + 1);
                        }
                    }
                    break;
                }
            }
        }

        // Also check if /ID is in XRef stream trailer (not raw trailer)
        if (_fileId.empty() && _trailer)
        {
            auto idObj = _trailer->get("/ID");
            if (!idObj) idObj = _trailer->get("ID");
            auto idArr = std::dynamic_pointer_cast<PdfArray>(idObj);
            if (idArr && !idArr->items.empty())
            {
                auto firstId = std::dynamic_pointer_cast<PdfString>(idArr->items[0]);
                if (firstId)
                {
                    _fileId.assign(firstId->value.begin(), firstId->value.end());
                }
            }
        }

        if (_fileId.empty())
        {
            LogDebug("PDF Encrypt: Cannot find /ID in trailer");
            return false;
        }

        LogDebug("PDF Encrypt: O=%zu bytes, U=%zu bytes, ID=%zu bytes",
            _encryptO.size(), _encryptU.size(), _fileId.size());

        // Compute encryption key and verify
        if (!computeEncryptionKey())
        {
            LogDebug("PDF Encrypt: Failed to compute encryption key");
            return false;
        }

        // Log the computed key
        {
            std::string kh;
            for (auto b : _encryptKey) { char buf[4]; snprintf(buf, sizeof(buf), "%02x", b); kh += buf; }
            LogDebug("PDF Encrypt: Computed key = %s", kh.c_str());
        }

        if (!verifyUserPassword())
        {
            LogDebug("PDF Encrypt: User password verification FAILED - password required");
            return false;
        }

        LogDebug("PDF Encrypt: User password verification PASSED");
        return true;
    }

    // PDF password padding constant (PDF Reference Table 3.18)
    static const uint8_t PDF_PASSWORD_PADDING[32] = {
        0x28, 0xBF, 0x4E, 0x5E, 0x4E, 0x75, 0x8A, 0x41,
        0x64, 0x00, 0x4E, 0x56, 0xFF, 0xFA, 0x01, 0x08,
        0x2E, 0x2E, 0x00, 0xB6, 0xD0, 0x68, 0x3E, 0x80,
        0x2F, 0x0C, 0xA9, 0xFE, 0x64, 0x53, 0x69, 0x7A
    };

    // Algorithm 2: Computing an encryption key (with optional user password)
    bool PdfDocument::computeEncryptionKey()
    {
        // Step 1: Pad password to 32 bytes
        uint8_t paddedPassword[32];
        if (_userPassword.empty())
        {
            // Empty password = full padding
            memcpy(paddedPassword, PDF_PASSWORD_PADDING, 32);
        }
        else
        {
            // Use user-supplied password, pad or truncate to 32 bytes
            size_t passLen = std::min(_userPassword.size(), (size_t)32);
            memcpy(paddedPassword, _userPassword.data(), passLen);
            if (passLen < 32)
                memcpy(paddedPassword + passLen, PDF_PASSWORD_PADDING, 32 - passLen);
        }

        // Step 2-5: MD5(paddedPassword + O + P + fileID)
        // Using Windows CryptoAPI for MD5

        // Simple MD5 implementation (RFC 1321)
        // Instead of pulling in a library, we use the same approach as many PDF libs

        // We'll use a minimal MD5 implementation inline
        struct MD5 {
            uint32_t state[4];
            uint64_t count;
            uint8_t buffer[64];

            static void transform(uint32_t state[4], const uint8_t block[64]) {
                uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
                uint32_t M[16];
                for (int i = 0; i < 16; ++i)
                    M[i] = (uint32_t)block[i * 4] | ((uint32_t)block[i * 4 + 1] << 8) |
                    ((uint32_t)block[i * 4 + 2] << 16) | ((uint32_t)block[i * 4 + 3] << 24);

#define F(x,y,z) (((x)&(y))|((~(x))&(z)))
#define G(x,y,z) (((x)&(z))|((y)&(~(z))))
#define H(x,y,z) ((x)^(y)^(z))
#define I(x,y,z) ((y)^((x)|(~(z))))
#define ROT(x,n) (((x)<<(n))|((x)>>(32-(n))))
#define FF(a,b,c,d,x,s,t) { a += F(b,c,d)+x+t; a = ROT(a,s)+b; }
#define GG(a,b,c,d,x,s,t) { a += G(b,c,d)+x+t; a = ROT(a,s)+b; }
#define HH(a,b,c,d,x,s,t) { a += H(b,c,d)+x+t; a = ROT(a,s)+b; }
#define II(a,b,c,d,x,s,t) { a += I(b,c,d)+x+t; a = ROT(a,s)+b; }

                FF(a, b, c, d, M[0], 7, 0xd76aa478); FF(d, a, b, c, M[1], 12, 0xe8c7b756);
                FF(c, d, a, b, M[2], 17, 0x242070db); FF(b, c, d, a, M[3], 22, 0xc1bdceee);
                FF(a, b, c, d, M[4], 7, 0xf57c0faf); FF(d, a, b, c, M[5], 12, 0x4787c62a);
                FF(c, d, a, b, M[6], 17, 0xa8304613); FF(b, c, d, a, M[7], 22, 0xfd469501);
                FF(a, b, c, d, M[8], 7, 0x698098d8); FF(d, a, b, c, M[9], 12, 0x8b44f7af);
                FF(c, d, a, b, M[10], 17, 0xffff5bb1); FF(b, c, d, a, M[11], 22, 0x895cd7be);
                FF(a, b, c, d, M[12], 7, 0x6b901122); FF(d, a, b, c, M[13], 12, 0xfd987193);
                FF(c, d, a, b, M[14], 17, 0xa679438e); FF(b, c, d, a, M[15], 22, 0x49b40821);

                GG(a, b, c, d, M[1], 5, 0xf61e2562); GG(d, a, b, c, M[6], 9, 0xc040b340);
                GG(c, d, a, b, M[11], 14, 0x265e5a51); GG(b, c, d, a, M[0], 20, 0xe9b6c7aa);
                GG(a, b, c, d, M[5], 5, 0xd62f105d); GG(d, a, b, c, M[10], 9, 0x02441453);
                GG(c, d, a, b, M[15], 14, 0xd8a1e681); GG(b, c, d, a, M[4], 20, 0xe7d3fbc8);
                GG(a, b, c, d, M[9], 5, 0x21e1cde6); GG(d, a, b, c, M[14], 9, 0xc33707d6);
                GG(c, d, a, b, M[3], 14, 0xf4d50d87); GG(b, c, d, a, M[8], 20, 0x455a14ed);
                GG(a, b, c, d, M[13], 5, 0xa9e3e905); GG(d, a, b, c, M[2], 9, 0xfcefa3f8);
                GG(c, d, a, b, M[7], 14, 0x676f02d9); GG(b, c, d, a, M[12], 20, 0x8d2a4c8a);

                HH(a, b, c, d, M[5], 4, 0xfffa3942); HH(d, a, b, c, M[8], 11, 0x8771f681);
                HH(c, d, a, b, M[11], 16, 0x6d9d6122); HH(b, c, d, a, M[14], 23, 0xfde5380c);
                HH(a, b, c, d, M[1], 4, 0xa4beea44); HH(d, a, b, c, M[4], 11, 0x4bdecfa9);
                HH(c, d, a, b, M[7], 16, 0xf6bb4b60); HH(b, c, d, a, M[10], 23, 0xbebfbc70);
                HH(a, b, c, d, M[13], 4, 0x289b7ec6); HH(d, a, b, c, M[0], 11, 0xeaa127fa);
                HH(c, d, a, b, M[3], 16, 0xd4ef3085); HH(b, c, d, a, M[6], 23, 0x04881d05);
                HH(a, b, c, d, M[9], 4, 0xd9d4d039); HH(d, a, b, c, M[12], 11, 0xe6db99e5);
                HH(c, d, a, b, M[15], 16, 0x1fa27cf8); HH(b, c, d, a, M[2], 23, 0xc4ac5665);

                II(a, b, c, d, M[0], 6, 0xf4292244); II(d, a, b, c, M[7], 10, 0x432aff97);
                II(c, d, a, b, M[14], 15, 0xab9423a7); II(b, c, d, a, M[5], 21, 0xfc93a039);
                II(a, b, c, d, M[12], 6, 0x655b59c3); II(d, a, b, c, M[3], 10, 0x8f0ccc92);
                II(c, d, a, b, M[10], 15, 0xffeff47d); II(b, c, d, a, M[1], 21, 0x85845dd1);
                II(a, b, c, d, M[8], 6, 0x6fa87e4f); II(d, a, b, c, M[15], 10, 0xfe2ce6e0);
                II(c, d, a, b, M[6], 15, 0xa3014314); II(b, c, d, a, M[13], 21, 0x4e0811a1);
                II(a, b, c, d, M[4], 6, 0xf7537e82); II(d, a, b, c, M[11], 10, 0xbd3af235);
                II(c, d, a, b, M[2], 15, 0x2ad7d2bb); II(b, c, d, a, M[9], 21, 0xeb86d391);

                state[0] += a; state[1] += b; state[2] += c; state[3] += d;

#undef F
#undef G
#undef H
#undef I
#undef ROT
#undef FF
#undef GG
#undef HH
#undef II
            }

            void init() {
                state[0] = 0x67452301; state[1] = 0xefcdab89;
                state[2] = 0x98badcfe; state[3] = 0x10325476;
                count = 0;
                memset(buffer, 0, 64);
            }

            void update(const uint8_t* data, size_t len) {
                size_t index = (size_t)(count & 0x3F);
                count += len;
                size_t i = 0;
                if (index) {
                    size_t part = 64 - index;
                    if (len >= part) {
                        memcpy(buffer + index, data, part);
                        transform(state, buffer);
                        i = part;
                        index = 0;
                    }
                    else {
                        memcpy(buffer + index, data, len);
                        return;
                    }
                }
                for (; i + 64 <= len; i += 64)
                    transform(state, data + i);
                if (i < len)
                    memcpy(buffer, data + i, len - i);
            }

            void digest(uint8_t result[16]) {
                uint8_t bits[8];
                uint64_t bitCount = count * 8;
                for (int i = 0; i < 8; ++i)
                    bits[i] = (uint8_t)(bitCount >> (i * 8));

                size_t index = (size_t)(count & 0x3F);
                uint8_t padding_byte = 0x80;
                update(&padding_byte, 1);

                uint8_t zero = 0;
                while ((count & 0x3F) != 56)
                    update(&zero, 1);

                update(bits, 8);

                for (int i = 0; i < 4; ++i) {
                    result[i * 4 + 0] = (uint8_t)(state[i]);
                    result[i * 4 + 1] = (uint8_t)(state[i] >> 8);
                    result[i * 4 + 2] = (uint8_t)(state[i] >> 16);
                    result[i * 4 + 3] = (uint8_t)(state[i] >> 24);
                }
            }

            // Convenience: hash entire buffer
            static void hash(const uint8_t* data, size_t len, uint8_t result[16]) {
                MD5 ctx;
                ctx.init();
                ctx.update(data, len);
                ctx.digest(result);
            }
        };

        // Algorithm 2: Computing encryption key
        MD5 md5;
        md5.init();
        md5.update(paddedPassword, 32);
        md5.update(_encryptO.data(), _encryptO.size());

        uint8_t pBytes[4];
        pBytes[0] = (uint8_t)(_encryptP & 0xFF);
        pBytes[1] = (uint8_t)((_encryptP >> 8) & 0xFF);
        pBytes[2] = (uint8_t)((_encryptP >> 16) & 0xFF);
        pBytes[3] = (uint8_t)((_encryptP >> 24) & 0xFF);
        md5.update(pBytes, 4);
        md5.update(_fileId.data(), _fileId.size());

        uint8_t hash[16];
        md5.digest(hash);

        // R >= 3: 50 additional MD5 iterations
        if (_encryptR >= 3)
        {
            for (int i = 0; i < 50; ++i)
            {
                MD5::hash(hash, _encryptKeyLength, hash);
            }
        }

        _encryptKey.assign(hash, hash + _encryptKeyLength);
        return true;
    }

    // Verify user password (Algorithm 4 for R=2, Algorithm 5 for R=3)
    bool PdfDocument::verifyUserPassword()
    {
        struct MD5 {
            uint32_t state[4];
            uint64_t count;
            uint8_t buffer[64];
            static void transform(uint32_t state[4], const uint8_t block[64]);
            void init();
            void update(const uint8_t* data, size_t len);
            void digest(uint8_t result[16]);
            static void hash(const uint8_t* data, size_t len, uint8_t result[16]);
        };
        // Reuse the MD5 from above - same implementation
        // For simplicity, we duplicate the static methods here

        // Actually, let's just use a lambda-based approach
        auto md5Hash = [](const std::vector<uint8_t>& input) -> std::vector<uint8_t> {
            // Minimal MD5 - same as in computeEncryptionKey
            struct MD5Local {
                uint32_t state[4];
                uint64_t count;
                uint8_t buffer[64];

                static void transform(uint32_t st[4], const uint8_t block[64]) {
                    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], M[16];
                    for (int i = 0; i < 16; ++i)M[i] = (uint32_t)block[i * 4] | ((uint32_t)block[i * 4 + 1] << 8) | ((uint32_t)block[i * 4 + 2] << 16) | ((uint32_t)block[i * 4 + 3] << 24);
#define F(x,y,z)(((x)&(y))|((~(x))&(z)))
#define G(x,y,z)(((x)&(z))|((y)&(~(z))))
#define H(x,y,z)((x)^(y)^(z))
#define I(x,y,z)((y)^((x)|(~(z))))
#define R(x,n)(((x)<<(n))|((x)>>(32-(n))))
#define FF(a,b,c,d,x,s,t){a+=F(b,c,d)+x+t;a=R(a,s)+b;}
#define GG(a,b,c,d,x,s,t){a+=G(b,c,d)+x+t;a=R(a,s)+b;}
#define HH(a,b,c,d,x,s,t){a+=H(b,c,d)+x+t;a=R(a,s)+b;}
#define II(a,b,c,d,x,s,t){a+=I(b,c,d)+x+t;a=R(a,s)+b;}
                    FF(a, b, c, d, M[0], 7, 0xd76aa478); FF(d, a, b, c, M[1], 12, 0xe8c7b756); FF(c, d, a, b, M[2], 17, 0x242070db); FF(b, c, d, a, M[3], 22, 0xc1bdceee);
                    FF(a, b, c, d, M[4], 7, 0xf57c0faf); FF(d, a, b, c, M[5], 12, 0x4787c62a); FF(c, d, a, b, M[6], 17, 0xa8304613); FF(b, c, d, a, M[7], 22, 0xfd469501);
                    FF(a, b, c, d, M[8], 7, 0x698098d8); FF(d, a, b, c, M[9], 12, 0x8b44f7af); FF(c, d, a, b, M[10], 17, 0xffff5bb1); FF(b, c, d, a, M[11], 22, 0x895cd7be);
                    FF(a, b, c, d, M[12], 7, 0x6b901122); FF(d, a, b, c, M[13], 12, 0xfd987193); FF(c, d, a, b, M[14], 17, 0xa679438e); FF(b, c, d, a, M[15], 22, 0x49b40821);
                    GG(a, b, c, d, M[1], 5, 0xf61e2562); GG(d, a, b, c, M[6], 9, 0xc040b340); GG(c, d, a, b, M[11], 14, 0x265e5a51); GG(b, c, d, a, M[0], 20, 0xe9b6c7aa);
                    GG(a, b, c, d, M[5], 5, 0xd62f105d); GG(d, a, b, c, M[10], 9, 0x02441453); GG(c, d, a, b, M[15], 14, 0xd8a1e681); GG(b, c, d, a, M[4], 20, 0xe7d3fbc8);
                    GG(a, b, c, d, M[9], 5, 0x21e1cde6); GG(d, a, b, c, M[14], 9, 0xc33707d6); GG(c, d, a, b, M[3], 14, 0xf4d50d87); GG(b, c, d, a, M[8], 20, 0x455a14ed);
                    GG(a, b, c, d, M[13], 5, 0xa9e3e905); GG(d, a, b, c, M[2], 9, 0xfcefa3f8); GG(c, d, a, b, M[7], 14, 0x676f02d9); GG(b, c, d, a, M[12], 20, 0x8d2a4c8a);
                    HH(a, b, c, d, M[5], 4, 0xfffa3942); HH(d, a, b, c, M[8], 11, 0x8771f681); HH(c, d, a, b, M[11], 16, 0x6d9d6122); HH(b, c, d, a, M[14], 23, 0xfde5380c);
                    HH(a, b, c, d, M[1], 4, 0xa4beea44); HH(d, a, b, c, M[4], 11, 0x4bdecfa9); HH(c, d, a, b, M[7], 16, 0xf6bb4b60); HH(b, c, d, a, M[10], 23, 0xbebfbc70);
                    HH(a, b, c, d, M[13], 4, 0x289b7ec6); HH(d, a, b, c, M[0], 11, 0xeaa127fa); HH(c, d, a, b, M[3], 16, 0xd4ef3085); HH(b, c, d, a, M[6], 23, 0x04881d05);
                    HH(a, b, c, d, M[9], 4, 0xd9d4d039); HH(d, a, b, c, M[12], 11, 0xe6db99e5); HH(c, d, a, b, M[15], 16, 0x1fa27cf8); HH(b, c, d, a, M[2], 23, 0xc4ac5665);
                    II(a, b, c, d, M[0], 6, 0xf4292244); II(d, a, b, c, M[7], 10, 0x432aff97); II(c, d, a, b, M[14], 15, 0xab9423a7); II(b, c, d, a, M[5], 21, 0xfc93a039);
                    II(a, b, c, d, M[12], 6, 0x655b59c3); II(d, a, b, c, M[3], 10, 0x8f0ccc92); II(c, d, a, b, M[10], 15, 0xffeff47d); II(b, c, d, a, M[1], 21, 0x85845dd1);
                    II(a, b, c, d, M[8], 6, 0x6fa87e4f); II(d, a, b, c, M[15], 10, 0xfe2ce6e0); II(c, d, a, b, M[6], 15, 0xa3014314); II(b, c, d, a, M[13], 21, 0x4e0811a1);
                    II(a, b, c, d, M[4], 6, 0xf7537e82); II(d, a, b, c, M[11], 10, 0xbd3af235); II(c, d, a, b, M[2], 15, 0x2ad7d2bb); II(b, c, d, a, M[9], 21, 0xeb86d391);
                    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
#undef F
#undef G
#undef H
#undef I
#undef R
#undef FF
#undef GG
#undef HH
#undef II
                }
                void init() { state[0] = 0x67452301; state[1] = 0xefcdab89; state[2] = 0x98badcfe; state[3] = 0x10325476; count = 0; memset(buffer, 0, 64); }
                void update(const uint8_t* data, size_t len) {
                    size_t index = (size_t)(count & 0x3F); count += len; size_t i = 0;
                    if (index) { size_t part = 64 - index; if (len >= part) { memcpy(buffer + index, data, part); transform(state, buffer); i = part; index = 0; } else { memcpy(buffer + index, data, len); return; } }
                    for (; i + 64 <= len; i += 64)transform(state, data + i);
                    if (i < len)memcpy(buffer, data + i, len - i);
                }
                void digest(uint8_t r[16]) {
                    uint8_t bits[8]; uint64_t bc = count * 8; for (int i = 0; i < 8; ++i)bits[i] = (uint8_t)(bc >> (i * 8));
                    uint8_t p = 0x80; update(&p, 1); uint8_t z = 0; while ((count & 0x3F) != 56)update(&z, 1); update(bits, 8);
                    for (int i = 0; i < 4; ++i) { r[i * 4] = (uint8_t)state[i]; r[i * 4 + 1] = (uint8_t)(state[i] >> 8); r[i * 4 + 2] = (uint8_t)(state[i] >> 16); r[i * 4 + 3] = (uint8_t)(state[i] >> 24); }
                }
            };

            MD5Local ctx;
            ctx.init();
            ctx.update(input.data(), input.size());
            uint8_t h[16];
            ctx.digest(h);
            return std::vector<uint8_t>(h, h + 16);
            };

        if (_encryptR == 2)
        {
            // Algorithm 4: RC4 encrypt padding with key
            std::vector<uint8_t> paddingVec(PDF_PASSWORD_PADDING, PDF_PASSWORD_PADDING + 32);
            std::vector<uint8_t> computed;
            rc4Crypt(_encryptKey, paddingVec.data(), paddingVec.size(), computed);
            return computed == _encryptU;
        }
        else // R >= 3 (covers R=3 and R=4)
        {
            // Algorithm 5: MD5(padding + fileID), then RC4 with key, then 19 more RC4 passes
            std::vector<uint8_t> input;
            input.insert(input.end(), PDF_PASSWORD_PADDING, PDF_PASSWORD_PADDING + 32);
            input.insert(input.end(), _fileId.begin(), _fileId.end());

            auto uHash = md5Hash(input);

            // RC4 encrypt with key
            std::vector<uint8_t> encrypted;
            rc4Crypt(_encryptKey, uHash.data(), 16, encrypted);

            // 19 more RC4 passes with XOR'd key
            for (int i = 1; i <= 19; ++i)
            {
                std::vector<uint8_t> xorKey(_encryptKey.size());
                for (size_t j = 0; j < _encryptKey.size(); ++j)
                    xorKey[j] = _encryptKey[j] ^ (uint8_t)i;

                std::vector<uint8_t> temp;
                rc4Crypt(xorKey, encrypted.data(), encrypted.size(), temp);
                encrypted = temp;
            }

            // Compare first 16 bytes of U
            return encrypted.size() >= 16 &&
                memcmp(encrypted.data(), _encryptU.data(), 16) == 0;
        }
    }

    // =====================================================
    // V=5 (AES-256) Key Derivation - ISO 32000-2
    // R=5: Simple SHA-256
    // R=6: Algorithm 2.B (iterative hash with SHA-256/384/512)
    // =====================================================

    bool PdfDocument::computeEncryptionKeyV5()
    {
        // Get password bytes (truncated to 127 bytes per spec)
        std::vector<uint8_t> passBytes;
        if (!_userPassword.empty())
        {
            size_t pLen = std::min(_userPassword.size(), (size_t)127);
            passBytes.assign(_userPassword.begin(), _userPassword.begin() + pLen);
        }

        // Try user password first, then owner password
        // User password: validation salt = U[32:40], key salt = U[40:48]
        bool userOk = false;
        if (_encryptU.size() >= 48)
        {
            if (_encryptR == 5)
            {
                // R=5: Simple SHA-256(password + validation_salt)
                std::vector<uint8_t> hashInput;
                hashInput.insert(hashInput.end(), passBytes.begin(), passBytes.end());
                hashInput.insert(hashInput.end(), _encryptU.begin() + 32, _encryptU.begin() + 40);

                uint8_t hashResult[32];
                SHA256::hash(hashInput.data(), hashInput.size(), hashResult);

                userOk = (memcmp(hashResult, _encryptU.data(), 32) == 0);
                LogDebug("PDF Encrypt: V5 R=5 user password check: %s", userOk ? "PASS" : "FAIL");
            }
            else if (_encryptR == 6)
            {
                // R=6: Algorithm 2.B
                std::vector<uint8_t> hashInput;
                hashInput.insert(hashInput.end(), passBytes.begin(), passBytes.end());
                hashInput.insert(hashInput.end(), _encryptU.begin() + 32, _encryptU.begin() + 40);

                uint8_t hashResult[32];
                algorithm2B(passBytes.data(), passBytes.size(),
                    hashInput.data(), hashInput.size(),
                    nullptr, 0, hashResult);

                userOk = (memcmp(hashResult, _encryptU.data(), 32) == 0);
                LogDebug("PDF Encrypt: V5 R=6 user password check: %s", userOk ? "PASS" : "FAIL");
            }
        }

        if (userOk)
        {
            // Derive file encryption key from UE
            // key_hash = SHA-256(password + key_salt) or Algorithm 2.B
            std::vector<uint8_t> keyHashInput;
            keyHashInput.insert(keyHashInput.end(), passBytes.begin(), passBytes.end());
            keyHashInput.insert(keyHashInput.end(), _encryptU.begin() + 40, _encryptU.begin() + 48);

            uint8_t keyHash[32];
            if (_encryptR == 5)
            {
                SHA256::hash(keyHashInput.data(), keyHashInput.size(), keyHash);
            }
            else
            {
                algorithm2B(passBytes.data(), passBytes.size(),
                    keyHashInput.data(), keyHashInput.size(),
                    nullptr, 0, keyHash);
            }

            // Decrypt UE with AES-256-CBC, IV = 16 zeros
            // file_key = AES-256-CBC-decrypt(key=keyHash, iv=zeros, data=UE)
            if (_encryptUE.size() >= 32)
            {
                uint8_t roundKeys[240];
                aes256KeyExpansion(keyHash, roundKeys);

                uint8_t iv[16] = { 0 };
                _encryptKey.resize(32);
                // Decrypt 2 blocks (32 bytes of UE) manually without padding removal
                const uint8_t* prevBlock = iv;
                for (int b = 0; b < 2; b++)
                {
                    uint8_t decBlock[16];
                    aesDecryptBlock(_encryptUE.data() + b * 16, decBlock, roundKeys, 14);
                    for (int i = 0; i < 16; i++)
                        _encryptKey[b * 16 + i] = decBlock[i] ^ prevBlock[i];
                    prevBlock = _encryptUE.data() + b * 16;
                }
                _encryptionReady = true;
                return true;
            }
        }

        // Try owner password
        bool ownerOk = false;
        if (_encryptO.size() >= 48 && _encryptOE.size() >= 32)
        {
            if (_encryptR == 5)
            {
                std::vector<uint8_t> hashInput;
                hashInput.insert(hashInput.end(), passBytes.begin(), passBytes.end());
                hashInput.insert(hashInput.end(), _encryptO.begin() + 32, _encryptO.begin() + 40);
                hashInput.insert(hashInput.end(), _encryptU.begin(), _encryptU.begin() + 48);

                uint8_t hashResult[32];
                SHA256::hash(hashInput.data(), hashInput.size(), hashResult);
                ownerOk = (memcmp(hashResult, _encryptO.data(), 32) == 0);
            }
            else if (_encryptR == 6)
            {
                std::vector<uint8_t> hashInput;
                hashInput.insert(hashInput.end(), passBytes.begin(), passBytes.end());
                hashInput.insert(hashInput.end(), _encryptO.begin() + 32, _encryptO.begin() + 40);

                uint8_t hashResult[32];
                algorithm2B(passBytes.data(), passBytes.size(),
                    hashInput.data(), hashInput.size(),
                    _encryptU.data(), std::min(_encryptU.size(), (size_t)48),
                    hashResult);
                ownerOk = (memcmp(hashResult, _encryptO.data(), 32) == 0);
            }
            LogDebug("PDF Encrypt: V5 R=%d owner password check: %s", _encryptR, ownerOk ? "PASS" : "FAIL");

            if (ownerOk)
            {
                // Derive from OE
                std::vector<uint8_t> keyHashInput;
                keyHashInput.insert(keyHashInput.end(), passBytes.begin(), passBytes.end());
                keyHashInput.insert(keyHashInput.end(), _encryptO.begin() + 40, _encryptO.begin() + 48);
                if (_encryptR == 5)
                    keyHashInput.insert(keyHashInput.end(), _encryptU.begin(), _encryptU.begin() + 48);

                uint8_t keyHash[32];
                if (_encryptR == 5)
                {
                    SHA256::hash(keyHashInput.data(), keyHashInput.size(), keyHash);
                }
                else
                {
                    // R=6: need U as userKey
                    std::vector<uint8_t> alg2bInput;
                    alg2bInput.insert(alg2bInput.end(), passBytes.begin(), passBytes.end());
                    alg2bInput.insert(alg2bInput.end(), _encryptO.begin() + 40, _encryptO.begin() + 48);

                    algorithm2B(passBytes.data(), passBytes.size(),
                        alg2bInput.data(), alg2bInput.size(),
                        _encryptU.data(), std::min(_encryptU.size(), (size_t)48),
                        keyHash);
                }

                uint8_t roundKeys[240];
                aes256KeyExpansion(keyHash, roundKeys);

                uint8_t iv[16] = { 0 };
                _encryptKey.resize(32);
                const uint8_t* prevBlock = iv;
                for (int b = 0; b < 2; b++)
                {
                    uint8_t decBlock[16];
                    aesDecryptBlock(_encryptOE.data() + b * 16, decBlock, roundKeys, 14);
                    for (int i = 0; i < 16; i++)
                        _encryptKey[b * 16 + i] = decBlock[i] ^ prevBlock[i];
                    prevBlock = _encryptOE.data() + b * 16;
                }
                _encryptionReady = true;
                return true;
            }
        }

        LogDebug("PDF Encrypt: V5 - neither user nor owner password matched");
        return false;
    }

    bool PdfDocument::verifyUserPasswordV5()
    {
        // For V=5, password verification is done in computeEncryptionKeyV5
        // If we got here, key is already verified
        return _encryptionReady;
    }

    // Compute per-object encryption key
    std::vector<uint8_t> PdfDocument::computeObjectKey(int objNum, int genNum) const
    {
        // V=5: No per-object key derivation - use file key directly
        if (_encryptV == 5)
        {
            return _encryptKey; // 32-byte AES-256 key
        }

        // V <= 4: Algorithm 1 step: key + objNum(3 bytes LE) + genNum(2 bytes LE)
        std::vector<uint8_t> input = _encryptKey;
        input.push_back((uint8_t)(objNum & 0xFF));
        input.push_back((uint8_t)((objNum >> 8) & 0xFF));
        input.push_back((uint8_t)((objNum >> 16) & 0xFF));
        input.push_back((uint8_t)(genNum & 0xFF));
        input.push_back((uint8_t)((genNum >> 8) & 0xFF));

        // For AES (V=4), append "sAlT" (0x73 0x41 0x6C 0x54)
        if (_useAES)
        {
            input.push_back(0x73); // s
            input.push_back(0x41); // A
            input.push_back(0x6C); // l
            input.push_back(0x54); // T
        }

        // MD5 hash
        // Reuse inline MD5
        struct MD5L {
            uint32_t state[4]; uint64_t count; uint8_t buffer[64];
            static void transform(uint32_t st[4], const uint8_t bl[64]) {
                uint32_t a = st[0], b = st[1], c = st[2], d = st[3], M[16];
                for (int i = 0; i < 16; ++i)M[i] = (uint32_t)bl[i * 4] | ((uint32_t)bl[i * 4 + 1] << 8) | ((uint32_t)bl[i * 4 + 2] << 16) | ((uint32_t)bl[i * 4 + 3] << 24);
#define F(x,y,z)(((x)&(y))|((~(x))&(z)))
#define G(x,y,z)(((x)&(z))|((y)&(~(z))))
#define H(x,y,z)((x)^(y)^(z))
#define I(x,y,z)((y)^((x)|(~(z))))
#define R(x,n)(((x)<<(n))|((x)>>(32-(n))))
#define FF(a,b,c,d,x,s,t){a+=F(b,c,d)+x+t;a=R(a,s)+b;}
#define GG(a,b,c,d,x,s,t){a+=G(b,c,d)+x+t;a=R(a,s)+b;}
#define HH(a,b,c,d,x,s,t){a+=H(b,c,d)+x+t;a=R(a,s)+b;}
#define II(a,b,c,d,x,s,t){a+=I(b,c,d)+x+t;a=R(a,s)+b;}
                FF(a, b, c, d, M[0], 7, 0xd76aa478); FF(d, a, b, c, M[1], 12, 0xe8c7b756); FF(c, d, a, b, M[2], 17, 0x242070db); FF(b, c, d, a, M[3], 22, 0xc1bdceee);
                FF(a, b, c, d, M[4], 7, 0xf57c0faf); FF(d, a, b, c, M[5], 12, 0x4787c62a); FF(c, d, a, b, M[6], 17, 0xa8304613); FF(b, c, d, a, M[7], 22, 0xfd469501);
                FF(a, b, c, d, M[8], 7, 0x698098d8); FF(d, a, b, c, M[9], 12, 0x8b44f7af); FF(c, d, a, b, M[10], 17, 0xffff5bb1); FF(b, c, d, a, M[11], 22, 0x895cd7be);
                FF(a, b, c, d, M[12], 7, 0x6b901122); FF(d, a, b, c, M[13], 12, 0xfd987193); FF(c, d, a, b, M[14], 17, 0xa679438e); FF(b, c, d, a, M[15], 22, 0x49b40821);
                GG(a, b, c, d, M[1], 5, 0xf61e2562); GG(d, a, b, c, M[6], 9, 0xc040b340); GG(c, d, a, b, M[11], 14, 0x265e5a51); GG(b, c, d, a, M[0], 20, 0xe9b6c7aa);
                GG(a, b, c, d, M[5], 5, 0xd62f105d); GG(d, a, b, c, M[10], 9, 0x02441453); GG(c, d, a, b, M[15], 14, 0xd8a1e681); GG(b, c, d, a, M[4], 20, 0xe7d3fbc8);
                GG(a, b, c, d, M[9], 5, 0x21e1cde6); GG(d, a, b, c, M[14], 9, 0xc33707d6); GG(c, d, a, b, M[3], 14, 0xf4d50d87); GG(b, c, d, a, M[8], 20, 0x455a14ed);
                GG(a, b, c, d, M[13], 5, 0xa9e3e905); GG(d, a, b, c, M[2], 9, 0xfcefa3f8); GG(c, d, a, b, M[7], 14, 0x676f02d9); GG(b, c, d, a, M[12], 20, 0x8d2a4c8a);
                HH(a, b, c, d, M[5], 4, 0xfffa3942); HH(d, a, b, c, M[8], 11, 0x8771f681); HH(c, d, a, b, M[11], 16, 0x6d9d6122); HH(b, c, d, a, M[14], 23, 0xfde5380c);
                HH(a, b, c, d, M[1], 4, 0xa4beea44); HH(d, a, b, c, M[4], 11, 0x4bdecfa9); HH(c, d, a, b, M[7], 16, 0xf6bb4b60); HH(b, c, d, a, M[10], 23, 0xbebfbc70);
                HH(a, b, c, d, M[13], 4, 0x289b7ec6); HH(d, a, b, c, M[0], 11, 0xeaa127fa); HH(c, d, a, b, M[3], 16, 0xd4ef3085); HH(b, c, d, a, M[6], 23, 0x04881d05);
                HH(a, b, c, d, M[9], 4, 0xd9d4d039); HH(d, a, b, c, M[12], 11, 0xe6db99e5); HH(c, d, a, b, M[15], 16, 0x1fa27cf8); HH(b, c, d, a, M[2], 23, 0xc4ac5665);
                II(a, b, c, d, M[0], 6, 0xf4292244); II(d, a, b, c, M[7], 10, 0x432aff97); II(c, d, a, b, M[14], 15, 0xab9423a7); II(b, c, d, a, M[5], 21, 0xfc93a039);
                II(a, b, c, d, M[12], 6, 0x655b59c3); II(d, a, b, c, M[3], 10, 0x8f0ccc92); II(c, d, a, b, M[10], 15, 0xffeff47d); II(b, c, d, a, M[1], 21, 0x85845dd1);
                II(a, b, c, d, M[8], 6, 0x6fa87e4f); II(d, a, b, c, M[15], 10, 0xfe2ce6e0); II(c, d, a, b, M[6], 15, 0xa3014314); II(b, c, d, a, M[13], 21, 0x4e0811a1);
                II(a, b, c, d, M[4], 6, 0xf7537e82); II(d, a, b, c, M[11], 10, 0xbd3af235); II(c, d, a, b, M[2], 15, 0x2ad7d2bb); II(b, c, d, a, M[9], 21, 0xeb86d391);
                st[0] += a; st[1] += b; st[2] += c; st[3] += d;
#undef F
#undef G
#undef H
#undef I
#undef R
#undef FF
#undef GG
#undef HH
#undef II
            }
            void init() { state[0] = 0x67452301; state[1] = 0xefcdab89; state[2] = 0x98badcfe; state[3] = 0x10325476; count = 0; memset(buffer, 0, 64); }
            void update(const uint8_t* d, size_t l) {
                size_t idx = (size_t)(count & 0x3F); count += l; size_t i = 0;
                if (idx) { size_t p = 64 - idx; if (l >= p) { memcpy(buffer + idx, d, p); transform(state, buffer); i = p; idx = 0; } else { memcpy(buffer + idx, d, l); return; } }
                for (; i + 64 <= l; i += 64)transform(state, d + i); if (i < l)memcpy(buffer, d + i, l - i);
            }
            void digest(uint8_t r[16]) {
                uint8_t bits[8]; uint64_t bc = count * 8; for (int i = 0; i < 8; ++i)bits[i] = (uint8_t)(bc >> (i * 8));
                uint8_t p = 0x80; update(&p, 1); uint8_t z = 0; while ((count & 0x3F) != 56)update(&z, 1); update(bits, 8);
                for (int i = 0; i < 4; ++i) { r[i * 4] = (uint8_t)state[i]; r[i * 4 + 1] = (uint8_t)(state[i] >> 8); r[i * 4 + 2] = (uint8_t)(state[i] >> 16); r[i * 4 + 3] = (uint8_t)(state[i] >> 24); }
            }
        };

        MD5L ctx;
        ctx.init();
        ctx.update(input.data(), input.size());
        uint8_t hash[16];
        ctx.digest(hash);

        // Key length = min(encryptKeyLength + 5, 16)
        int objKeyLen = std::min(_encryptKeyLength + 5, 16);
        return std::vector<uint8_t>(hash, hash + objKeyLen);
    }

    // Decrypt a stream in-place
    void PdfDocument::decryptStream(const std::shared_ptr<PdfStream>& stream) const
    {
        if (!_encryptionReady || !stream || stream->data.empty()) return;

        // Find object number for this stream
        int objNum = -1;
        int genNum = 0;
        for (const auto& kv : _objects)
        {
            if (kv.second.get() == stream.get())
            {
                objNum = kv.first;
                break;
            }
        }

        if (objNum < 0)
        {
            LogDebug("PDF Encrypt: Cannot find object number for stream, skipping");
            return;
        }

        auto objKey = computeObjectKey(objNum, genNum);

        if (_useAES)
        {
            std::vector<uint8_t> decrypted;
            if (aesDecryptCBC(objKey, stream->data.data(), stream->data.size(), decrypted))
            {
                stream->data = std::move(decrypted);
            }
            else
            {
                LogDebug("PDF Encrypt: AES decrypt failed for obj %d (%zu bytes)", objNum, stream->data.size());
            }
        }
        else
        {
            std::vector<uint8_t> decrypted;
            rc4Crypt(objKey, stream->data.data(), stream->data.size(), decrypted);
            stream->data = std::move(decrypted);
        }
    }

    // Decrypt a string object
    void PdfDocument::decryptString(std::shared_ptr<PdfObject>& obj, int objNum, int genNum) const
    {
        if (!_encryptionReady) return;
        auto str = std::dynamic_pointer_cast<PdfString>(obj);
        if (!str || str->value.empty()) return;

        auto objKey = computeObjectKey(objNum, genNum);
        std::vector<uint8_t> input(str->value.begin(), str->value.end());

        if (_useAES)
        {
            std::vector<uint8_t> decrypted;
            if (aesDecryptCBC(objKey, input.data(), input.size(), decrypted))
                str->value = std::string(decrypted.begin(), decrypted.end());
        }
        else
        {
            std::vector<uint8_t> decrypted;
            rc4Crypt(objKey, input.data(), input.size(), decrypted);
            str->value = std::string(decrypted.begin(), decrypted.end());
        }
    }

    // Encryption status API
    // =================================================================
    // Certificate Encryption - initCertEncryption
    // Handles /Adobe.PubSec filter with SubFilter variants
    // =================================================================
    bool PdfDocument::initCertEncryption(const std::shared_ptr<PdfDictionary>& encryptDict,
        const uint8_t* encData, size_t encLen)
    {
        std::set<int> visited;

        // Parse /SubFilter
        visited.clear();
        auto subFilterObj = resolveIndirect(encryptDict->get("/SubFilter"), visited);
        auto subFilterName = std::dynamic_pointer_cast<PdfName>(subFilterObj);
        _certSubFilter = subFilterName ? subFilterName->value : "";
        if (!_certSubFilter.empty() && _certSubFilter[0] == '/')
            _certSubFilter = _certSubFilter.substr(1);

        LogDebug("PDF CertEncrypt: SubFilter = %s", _certSubFilter.c_str());

        // Parse /V, /R, /Length, /P
        visited.clear();
        auto vObj = std::dynamic_pointer_cast<PdfNumber>(resolveIndirect(encryptDict->get("/V"), visited));
        visited.clear();
        auto rObj = std::dynamic_pointer_cast<PdfNumber>(resolveIndirect(encryptDict->get("/R"), visited));
        visited.clear();
        auto lenObj = std::dynamic_pointer_cast<PdfNumber>(resolveIndirect(encryptDict->get("/Length"), visited));
        visited.clear();
        auto pObj = std::dynamic_pointer_cast<PdfNumber>(resolveIndirect(encryptDict->get("/P"), visited));

        if (vObj) _encryptV = (int)vObj->value;
        if (rObj) _encryptR = (int)rObj->value;
        if (lenObj) _encryptKeyLength = (int)lenObj->value / 8;
        if (pObj) {
            double pVal = pObj->value;
            _encryptP = (pVal > 2147483647.0) ? (int32_t)((uint32_t)pVal) : (int32_t)pVal;
        }
        else {
            // /P is often absent in cert-encrypted PDFs (SubFilter s5).
            // Default to -4 (0xFFFFFFFC) = all permissions granted.
            // ISO 32000-2: "If the P entry is not present, all permissions are granted."
            _encryptP = -4;
            LogDebug("PDF CertEncrypt: /P not found, defaulting to -4 (all permissions)");
        }

        // Default key length
        if (_encryptKeyLength <= 0)
            _encryptKeyLength = (_encryptV == 1) ? 5 : (_encryptV == 5 ? 32 : 16);

        // Parse /EncryptMetadata
        visited.clear();
        auto emObj = resolveIndirect(encryptDict->get("/EncryptMetadata"), visited);
        if (emObj) {
            auto emBool = std::dynamic_pointer_cast<PdfBoolean>(emObj);
            if (emBool) _encryptMetadata = emBool->value;
            if (!emBool) {
                auto emName = std::dynamic_pointer_cast<PdfName>(emObj);
                if (emName && (emName->value == "false" || emName->value == "/false"))
                    _encryptMetadata = false;
            }
        }

        // Determine AES mode
        _useAES = false;
        if (_encryptV >= 4) _useAES = true;

        LogDebug("PDF CertEncrypt: V=%d, R=%d, KeyLen=%d, P=%d, AES=%d, EncryptMetadata=%d",
            _encryptV, _encryptR, _encryptKeyLength, _encryptP,
            _useAES ? 1 : 0, _encryptMetadata ? 1 : 0);

        // ===== Parse /Recipients =====
        // SubFilter s3: Recipients in Encrypt dict directly
        // SubFilter s5: Recipients in CF/DefaultCryptFilter dict

        // Helper: parse hex string from raw bytes
        auto parseHexString = [](const uint8_t* data, size_t len, size_t startAfterAngle) -> std::vector<uint8_t>
            {
                std::vector<uint8_t> result;
                std::string hexChars;
                for (size_t i = startAfterAngle; i < len; ++i) {
                    uint8_t c = data[i];
                    if (c == '>') break;
                    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
                        hexChars.push_back((char)c);
                }
                if (hexChars.size() % 2 != 0) hexChars.push_back('0');
                for (size_t i = 0; i + 1 < hexChars.size(); i += 2) {
                    char hex[3] = { hexChars[i], hexChars[i + 1], 0 };
                    result.push_back((uint8_t)strtol(hex, nullptr, 16));
                }
                return result;
            };

        // Find /Recipients in raw encrypt data
        // Search first in CF/DefaultCryptFilter (for s5), then in encrypt dict itself (for s3)
        bool foundRecipients = false;

        // For SubFilter s5: look for /Recipients inside /CF
        if (_certSubFilter == "adbe.pkcs7.s5" || _encryptV >= 4)
        {
            // Parse CF → crypt filter → Recipients
            visited.clear();
            auto cfObj = resolveIndirect(encryptDict->get("/CF"), visited);
            auto cfDict = std::dynamic_pointer_cast<PdfDictionary>(cfObj);
            if (cfDict) {
                // Try DefaultCryptFilter, StdCF, etc.
                for (const auto& cfName : { "/DefaultCryptFilter", "/StdCF" }) {
                    visited.clear();
                    auto filterObj = resolveIndirect(cfDict->get(cfName), visited);
                    auto filterDict = std::dynamic_pointer_cast<PdfDictionary>(filterObj);
                    if (!filterDict) continue;

                    // Check /CFM for AES mode
                    visited.clear();
                    auto cfmObj = resolveIndirect(filterDict->get("/CFM"), visited);
                    auto cfmName = std::dynamic_pointer_cast<PdfName>(cfmObj);
                    if (cfmName) {
                        if (cfmName->value == "/AESV2" || cfmName->value == "AESV2") { _useAES = true; _encryptKeyLength = 16; }
                        else if (cfmName->value == "/AESV3" || cfmName->value == "AESV3") { _useAES = true; _encryptKeyLength = 32; }
                    }

                    // Parse /Recipients from raw bytes (binary data, need raw parse)
                    // Find "/Recipients" in the encrypt object raw data
                    std::string recipKey = "/Recipients";
                    for (size_t p = 0; p + recipKey.size() + 1 < encLen; ++p) {
                        if (memcmp(encData + p, recipKey.c_str(), recipKey.size()) == 0) {
                            size_t afterKey = p + recipKey.size();
                            // Skip whitespace
                            while (afterKey < encLen && (encData[afterKey] == ' ' || encData[afterKey] == '\n' ||
                                encData[afterKey] == '\r' || encData[afterKey] == '\t'))
                                afterKey++;

                            if (afterKey < encLen && encData[afterKey] == '[') {
                                // Array of recipient strings
                                afterKey++; // skip [
                                while (afterKey < encLen && encData[afterKey] != ']') {
                                    // Skip whitespace
                                    while (afterKey < encLen && (encData[afterKey] == ' ' || encData[afterKey] == '\n' ||
                                        encData[afterKey] == '\r' || encData[afterKey] == '\t'))
                                        afterKey++;

                                    if (afterKey >= encLen || encData[afterKey] == ']') break;

                                    if (encData[afterKey] == '<') {
                                        // Hex string
                                        auto blob = parseHexString(encData, encLen, afterKey + 1);
                                        if (!blob.empty()) {
                                            _recipientBlobs.push_back(std::move(blob));
                                        }
                                        // Skip past closing >
                                        afterKey++;
                                        while (afterKey < encLen && encData[afterKey] != '>') afterKey++;
                                        if (afterKey < encLen) afterKey++;
                                    }
                                    else if (encData[afterKey] == '(') {
                                        // Literal string
                                        size_t endPos = 0;
                                        auto blob = parsePdfLiteralString(encData, encLen, afterKey + 1, endPos);
                                        if (!blob.empty()) {
                                            _recipientBlobs.push_back(std::move(blob));
                                        }
                                        afterKey = endPos;
                                    }
                                    else {
                                        afterKey++; // skip unknown
                                    }
                                }
                                foundRecipients = !_recipientBlobs.empty();
                            }
                            break;
                        }
                    }
                    if (foundRecipients) break;
                }
            }
        }

        // Fallback: look for /Recipients directly in encrypt dict (for s3 or if not found in CF)
        if (!foundRecipients) {
            std::string recipKey = "/Recipients";
            for (size_t p = 0; p + recipKey.size() + 1 < encLen; ++p) {
                if (memcmp(encData + p, recipKey.c_str(), recipKey.size()) == 0) {
                    size_t afterKey = p + recipKey.size();
                    while (afterKey < encLen && (encData[afterKey] == ' ' || encData[afterKey] == '\n' ||
                        encData[afterKey] == '\r' || encData[afterKey] == '\t'))
                        afterKey++;

                    if (afterKey < encLen && encData[afterKey] == '[') {
                        afterKey++;
                        while (afterKey < encLen && encData[afterKey] != ']') {
                            while (afterKey < encLen && (encData[afterKey] == ' ' || encData[afterKey] == '\n' ||
                                encData[afterKey] == '\r' || encData[afterKey] == '\t'))
                                afterKey++;
                            if (afterKey >= encLen || encData[afterKey] == ']') break;

                            if (encData[afterKey] == '<') {
                                auto blob = parseHexString(encData, encLen, afterKey + 1);
                                if (!blob.empty()) _recipientBlobs.push_back(std::move(blob));
                                afterKey++;
                                while (afterKey < encLen && encData[afterKey] != '>') afterKey++;
                                if (afterKey < encLen) afterKey++;
                            }
                            else if (encData[afterKey] == '(') {
                                size_t endPos = 0;
                                auto blob = parsePdfLiteralString(encData, encLen, afterKey + 1, endPos);
                                if (!blob.empty()) _recipientBlobs.push_back(std::move(blob));
                                afterKey = endPos;
                            }
                            else {
                                afterKey++;
                            }
                        }
                        foundRecipients = !_recipientBlobs.empty();
                    }
                    break;
                }
            }
        }

        if (!foundRecipients || _recipientBlobs.empty()) {
            LogDebug("PDF CertEncrypt: No /Recipients found");
            return false;
        }

        LogDebug("PDF CertEncrypt: Found %zu recipient blob(s)", _recipientBlobs.size());

        // Parse PKCS#7 EnvelopedData from first (or only) recipient blob
        if (!parsePkcs7EnvelopedData(_recipientBlobs[0].data(), _recipientBlobs[0].size(), _envelopedData))
        {
            LogDebug("PDF CertEncrypt: Failed to parse PKCS#7 EnvelopedData");
            return false;
        }

        LogDebug("PDF CertEncrypt: Parsed %zu recipient(s) from PKCS#7",
            _envelopedData.recipients.size());

        // Certificate encrypted - ready to receive seed via supplySeed()
        _encryptionReady = false;
        return true;
    }

    // =================================================================
    // supplySeed - derive file key from decrypted seed (certificate enc)
    // PDF Reference 7.6.4.3.3
    // =================================================================
    bool PdfDocument::supplySeed(const uint8_t* seed, size_t seedLen)
    {
        if (!_isCertEncrypted || _recipientBlobs.empty()) return false;

        LogDebug("PDF CertEncrypt: supplySeed called with %zu byte seed", seedLen);

        // Build hash input: seed + each recipient blob + 4-byte permissions (LE)
        std::vector<uint8_t> hashInput;
        hashInput.insert(hashInput.end(), seed, seed + seedLen);

        for (const auto& blob : _recipientBlobs)
            hashInput.insert(hashInput.end(), blob.begin(), blob.end());

        // 4-byte permissions (little-endian)
        uint32_t permsU = (uint32_t)_encryptP;
        hashInput.push_back((uint8_t)(permsU & 0xFF));
        hashInput.push_back((uint8_t)((permsU >> 8) & 0xFF));
        hashInput.push_back((uint8_t)((permsU >> 16) & 0xFF));
        hashInput.push_back((uint8_t)((permsU >> 24) & 0xFF));

        // If EncryptMetadata is false, append 4 bytes of 0xFF
        if (!_encryptMetadata) {
            for (int i = 0; i < 4; i++) hashInput.push_back(0xFF);
        }

        // Hash and truncate to key length
        if (_encryptV == 5 || _encryptKeyLength > 20) {
            // AES-256: use SHA-256, key = 32 bytes
            uint8_t hash[32];
            SHA256::hash(hashInput.data(), hashInput.size(), hash);
            _encryptKey.assign(hash, hash + std::min(_encryptKeyLength, 32));
        }
        else {
            // V1/V2/V4: use SHA-1, truncate to key length
            uint8_t hash[20];
            SHA1::hash(hashInput.data(), hashInput.size(), hash);
            _encryptKey.assign(hash, hash + std::min(_encryptKeyLength, 20));
        }

        // Debug: log derived key
        {
            std::string keyHex;
            for (auto b : _encryptKey) {
                char buf[4]; snprintf(buf, sizeof(buf), "%02x", b);
                keyHex += buf;
            }
            LogDebug("PDF CertEncrypt: Derived %zu byte file key: %s (P=%d, hashInputLen=%zu)",
                _encryptKey.size(), keyHex.c_str(), _encryptP, hashInput.size());
        }
        _encryptionReady = true;

        // Decrypt all streams with the new key
        for (auto& kv : _objects) {
            auto stream = std::dynamic_pointer_cast<PdfStream>(kv.second);
            if (stream && !stream->data.empty()) {
                auto itX = _xrefTable.find(kv.first);
                if (itX != _xrefTable.end()) {
                    auto reloaded = loadObjectAtOffset(itX->second);
                    auto reloadedStream = std::dynamic_pointer_cast<PdfStream>(reloaded);
                    if (reloadedStream && !reloadedStream->data.empty()) {
                        int objNum = kv.first;
                        int genNum = 0;
                        auto objKey = computeObjectKey(objNum, genNum);
                        if (_useAES) {
                            std::vector<uint8_t> decrypted;
                            if (aesDecryptCBC(objKey, reloadedStream->data.data(),
                                reloadedStream->data.size(), decrypted))
                                stream->data = std::move(decrypted);
                        }
                        else {
                            std::vector<uint8_t> decrypted;
                            rc4Crypt(objKey, reloadedStream->data.data(),
                                reloadedStream->data.size(), decrypted);
                            stream->data = std::move(decrypted);
                        }
                        if (reloadedStream->dict && !stream->dict)
                            stream->dict = reloadedStream->dict;
                    }
                }
            }
        }

        return true;
    }

    // =================================================================
    // getEncryptionType - 0=none, 1=password, 2=certificate
    // =================================================================
    int PdfDocument::getEncryptionType() const
    {
        if (!_isEncrypted) return 0;
        if (_isCertEncrypted) return 2;
        return 1;
    }

    // =================================================================
    // getCertRecipients - return parsed PKCS#7 recipient list
    // =================================================================
    const std::vector<Pkcs7RecipientInfo>& PdfDocument::getCertRecipients() const
    {
        return _envelopedData.recipients;
    }

    int PdfDocument::getEncryptionStatus() const
    {
        if (!_isEncrypted) return 0;          // Not encrypted
        if (_encryptionReady) return 1;       // Encrypted + decrypted OK
        return -1;                            // Encrypted + needs password
    }

    // Try a user-supplied password and re-derive encryption key
    bool PdfDocument::tryPassword(const std::string& password)
    {
        if (!_isEncrypted) return true;
        if (_encryptionReady) return true; // Already decrypted

        _userPassword = password;
        _encryptKey.clear();
        _encryptionReady = false;

        bool keyOk = false;

        if (_encryptV == 5)
        {
            // V=5: SHA-256 based key derivation
            keyOk = computeEncryptionKeyV5();
        }
        else
        {
            // V <= 4: MD5 based
            if (!computeEncryptionKey())
            {
                LogDebug("PDF Encrypt: computeEncryptionKey failed with supplied password");
                _userPassword.clear();
                return false;
            }
            if (!verifyUserPassword())
            {
                LogDebug("PDF Encrypt: Password verification FAILED");
                _userPassword.clear();
                return false;
            }
            keyOk = true;
        }

        if (!keyOk)
        {
            LogDebug("PDF Encrypt: Key derivation failed for supplied password");
            _userPassword.clear();
            return false;
        }

        LogDebug("PDF Encrypt: Password accepted, decrypting streams...");
        _encryptionReady = true;

        // Re-decrypt all streams with the new key
        for (auto& kv : _objects)
        {
            auto stream = std::dynamic_pointer_cast<PdfStream>(kv.second);
            if (stream && !stream->data.empty())
            {
                // We need to re-parse from raw data since existing stream data
                // may have been corrupted by previous failed decrypt attempt.
                // Reload from file offset
                auto itX = _xrefTable.find(kv.first);
                if (itX != _xrefTable.end())
                {
                    auto reloaded = loadObjectAtOffset(itX->second);
                    auto reloadedStream = std::dynamic_pointer_cast<PdfStream>(reloaded);
                    if (reloadedStream && !reloadedStream->data.empty())
                    {
                        // Now decrypt the fresh copy
                        int objNum = kv.first;
                        int genNum = 0;
                        auto objKey = computeObjectKey(objNum, genNum);

                        if (_useAES)
                        {
                            std::vector<uint8_t> decrypted;
                            if (aesDecryptCBC(objKey, reloadedStream->data.data(), reloadedStream->data.size(), decrypted))
                                stream->data = std::move(decrypted);
                        }
                        else
                        {
                            std::vector<uint8_t> decrypted;
                            rc4Crypt(objKey, reloadedStream->data.data(), reloadedStream->data.size(), decrypted);
                            stream->data = std::move(decrypted);
                        }

                        // Keep the dictionary from original parsed object
                        if (reloadedStream->dict && !stream->dict)
                            stream->dict = reloadedStream->dict;
                    }
                }
            }
        }

        return true;
    }

    // ============================================
    // LINK HELPERS
    // ============================================

    // Destination array'den ([pageRef /XYZ ...]) sayfa indeksini çöz.
    // Başarısız olursa -1 döner.
    int PdfDocument::resolvePageFromDestArray(const std::shared_ptr<PdfArray>& destArr) const
    {
        if (!destArr || destArr->items.empty()) return -1;

        std::set<int> visited;

        // İlk eleman sayfa referansı
        visited.clear();
        auto pageRef = resolveIndirect(destArr->items[0], visited);
        auto pageRefDict = std::dynamic_pointer_cast<PdfDictionary>(pageRef);
        if (pageRefDict)
        {
            for (int pi = 0; pi < getPageCountFromPageTree(); pi++)
            {
                if (getPageDictionary(pi).get() == pageRefDict.get())
                    return pi;
            }
        }

        // Indirect reference olarak dene
        auto indRef = std::dynamic_pointer_cast<PdfIndirectRef>(destArr->items[0]);
        if (indRef)
        {
            for (int pi = 0; pi < getPageCountFromPageTree(); pi++)
            {
                auto pd = getPageDictionary(pi);
                auto objIt = _objects.find(indRef->objNum);
                if (objIt != _objects.end())
                {
                    visited.clear();
                    auto resolvedPage = resolveIndirect(objIt->second, visited);
                    if (resolvedPage.get() == pd.get())
                        return pi;
                }
            }
        }

        return -1;
    }

    // Named destination'ı çöz: Catalog -> /Names -> /Dests -> /Names array'den ara
    // Büyük PDF'lerde /Kids tree yapısını da destekler.
    std::shared_ptr<PdfArray> PdfDocument::resolveNamedDestination(const std::string& name) const
    {
        if (!_root || name.empty()) return nullptr;

        std::set<int> visited;

        // Catalog -> /Names
        visited.clear();
        auto namesObj = resolveIndirect(dictGetAny(_root, "/Names", "Names"), visited);
        auto namesDict = std::dynamic_pointer_cast<PdfDictionary>(namesObj);
        if (!namesDict) return nullptr;

        // /Names -> /Dests
        visited.clear();
        auto destsObj = resolveIndirect(dictGetAny(namesDict, "/Dests", "Dests"), visited);
        auto destsDict = std::dynamic_pointer_cast<PdfDictionary>(destsObj);
        if (!destsDict) return nullptr;

        // Recursive name tree search (flat /Names array veya /Kids hiyerarşisi)
        // Lambda ile recursive arama
        std::function<std::shared_ptr<PdfArray>(const std::shared_ptr<PdfDictionary>&)> searchNameTree;
        searchNameTree = [&](const std::shared_ptr<PdfDictionary>& node) -> std::shared_ptr<PdfArray>
        {
            if (!node) return nullptr;

            // Leaf node: /Names array [(key1) value1 (key2) value2 ...]
            std::set<int> v;
            auto namesArrObj = resolveIndirect(dictGetAny(node, "/Names", "Names"), v);
            auto namesArr = std::dynamic_pointer_cast<PdfArray>(namesArrObj);
            if (namesArr)
            {
                for (size_t i = 0; i + 1 < namesArr->items.size(); i += 2)
                {
                    auto keyStr = std::dynamic_pointer_cast<PdfString>(namesArr->items[i]);
                    if (keyStr && keyStr->value == name)
                    {
                        v.clear();
                        auto val = resolveIndirect(namesArr->items[i + 1], v);
                        return std::dynamic_pointer_cast<PdfArray>(val);
                    }
                }
            }

            // Intermediate node: /Kids array [childRef1, childRef2, ...]
            v.clear();
            auto kidsObj = resolveIndirect(dictGetAny(node, "/Kids", "Kids"), v);
            auto kidsArr = std::dynamic_pointer_cast<PdfArray>(kidsObj);
            if (kidsArr)
            {
                for (const auto& kidRef : kidsArr->items)
                {
                    v.clear();
                    auto kidObj = resolveIndirect(kidRef, v);
                    auto kidDict = std::dynamic_pointer_cast<PdfDictionary>(kidObj);
                    if (kidDict)
                    {
                        // /Limits kontrolü: [minKey, maxKey] - aralık dışındaysa atla
                        v.clear();
                        auto limitsObj = resolveIndirect(dictGetAny(kidDict, "/Limits", "Limits"), v);
                        auto limitsArr = std::dynamic_pointer_cast<PdfArray>(limitsObj);
                        if (limitsArr && limitsArr->items.size() >= 2)
                        {
                            auto minStr = std::dynamic_pointer_cast<PdfString>(limitsArr->items[0]);
                            auto maxStr = std::dynamic_pointer_cast<PdfString>(limitsArr->items[1]);
                            if (minStr && maxStr)
                            {
                                if (name < minStr->value || name > maxStr->value)
                                    continue; // Bu alt ağaçta olamaz
                            }
                        }

                        auto result = searchNameTree(kidDict);
                        if (result) return result;
                    }
                }
            }

            return nullptr;
        };

        return searchNameTree(destsDict);
    }

    // ============================================
    // LINK EXTRACTION
    // Extracts URI and GoTo link annotations from a page
    // ============================================
    bool PdfDocument::getPageLinks(int pageIndex, std::vector<PdfLinkInfo>& outLinks) const
    {
        outLinks.clear();

        auto pageDict = getPageDictionary(pageIndex);
        if (!pageDict) return false;

        // Get /Annots array from page
        std::set<int> visited;
        auto annotsObj = resolveIndirect(dictGetAny(pageDict, "/Annots", "Annots"), visited);
        auto annotsArr = std::dynamic_pointer_cast<PdfArray>(annotsObj);
        if (!annotsArr) return true; // No annotations - not an error

        for (size_t ai = 0; ai < annotsArr->items.size(); ai++)
        {
            const auto& annotRef = annotsArr->items[ai];
            visited.clear();
            auto annotObj = resolveIndirect(annotRef, visited);
            auto annotDict = std::dynamic_pointer_cast<PdfDictionary>(annotObj);
            if (!annotDict) continue;

            // Check if this is a Link annotation
            visited.clear();
            auto subtypeObj = resolveIndirect(dictGetAny(annotDict, "/Subtype", "Subtype"), visited);
            auto subtypeName = std::dynamic_pointer_cast<PdfName>(subtypeObj);
            if (!subtypeName) continue;

            std::string subtype = subtypeName->value;
            if (!subtype.empty() && subtype[0] == '/') subtype = subtype.substr(1);
            if (subtype != "Link") continue;

            // Get the Rect (bounding box)
            visited.clear();
            auto rectObj = resolveIndirect(dictGetAny(annotDict, "/Rect", "Rect"), visited);
            auto rectArr = std::dynamic_pointer_cast<PdfArray>(rectObj);
            if (!rectArr || rectArr->items.size() < 4) continue;

            PdfLinkInfo link;
            auto getNumber = [](const PdfObjectPtr& obj) -> double {
                if (auto num = std::dynamic_pointer_cast<PdfNumber>(obj))
                    return num->value;
                return 0.0;
            };

            link.x1 = getNumber(rectArr->items[0]);
            link.y1 = getNumber(rectArr->items[1]);
            link.x2 = getNumber(rectArr->items[2]);
            link.y2 = getNumber(rectArr->items[3]);

            // Normalize rect (ensure x1 < x2, y1 < y2)
            if (link.x1 > link.x2) std::swap(link.x1, link.x2);
            if (link.y1 > link.y2) std::swap(link.y1, link.y2);

            // Check for /A (action) dictionary
            visited.clear();
            auto actionObj = resolveIndirect(dictGetAny(annotDict, "/A", "A"), visited);
            auto actionDict = std::dynamic_pointer_cast<PdfDictionary>(actionObj);

            if (actionDict)
            {
                // Get action type /S
                visited.clear();
                auto sObj = resolveIndirect(dictGetAny(actionDict, "/S", "S"), visited);
                auto sName = std::dynamic_pointer_cast<PdfName>(sObj);
                std::string actionType;
                if (sName) {
                    actionType = sName->value;
                    if (!actionType.empty() && actionType[0] == '/') actionType = actionType.substr(1);
                }

                if (actionType == "URI")
                {
                    // External URI link
                    visited.clear();
                    auto uriObj = resolveIndirect(dictGetAny(actionDict, "/URI", "URI"), visited);
                    if (auto uriStr = std::dynamic_pointer_cast<PdfString>(uriObj))
                    {
                        link.uri = uriStr->value;
                        link.destPage = -1;
                        outLinks.push_back(link);
                    }
                }
                else if (actionType == "GoTo")
                {
                    // Internal page link
                    visited.clear();
                    auto destObj = resolveIndirect(dictGetAny(actionDict, "/D", "D"), visited);

                    // Case 1: Direct array destination [pageRef, /XYZ, ...]
                    auto destArr = std::dynamic_pointer_cast<PdfArray>(destObj);
                    if (destArr && !destArr->items.empty())
                    {
                        link.destPage = resolvePageFromDestArray(destArr);
                        if (link.destPage >= 0)
                            outLinks.push_back(link);
                    }

                    // Case 2: Named destination (string) - Names tree'den çöz
                    if (link.destPage < 0)
                    {
                        auto destStr = std::dynamic_pointer_cast<PdfString>(destObj);
                        if (destStr && !destStr->value.empty())
                        {
                            auto resolvedArr = resolveNamedDestination(destStr->value);
                            if (resolvedArr)
                            {
                                link.destPage = resolvePageFromDestArray(resolvedArr);
                                if (link.destPage >= 0)
                                    outLinks.push_back(link);
                            }
                        }
                    }
                }
            }

            // Check for /Dest (direct destination without action)
            if (link.uri.empty() && link.destPage < 0)
            {
                visited.clear();
                auto destObj = resolveIndirect(dictGetAny(annotDict, "/Dest", "Dest"), visited);

                // Array destination [pageRef, /XYZ, ...]
                auto destArr = std::dynamic_pointer_cast<PdfArray>(destObj);
                if (destArr && !destArr->items.empty())
                {
                    link.destPage = resolvePageFromDestArray(destArr);
                    if (link.destPage >= 0)
                        outLinks.push_back(link);
                }

                // Named destination (string)
                if (link.destPage < 0)
                {
                    auto destStr = std::dynamic_pointer_cast<PdfString>(destObj);
                    if (destStr && !destStr->value.empty())
                    {
                        auto resolvedArr = resolveNamedDestination(destStr->value);
                        if (resolvedArr)
                        {
                            link.destPage = resolvePageFromDestArray(resolvedArr);
                            if (link.destPage >= 0)
                                outLinks.push_back(link);
                        }
                    }
                }
            }
        }

        return true;
    }

} // namespace pdf