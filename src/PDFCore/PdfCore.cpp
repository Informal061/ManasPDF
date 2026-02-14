#include "pch.h"
#include "zlib.h"
#include "PdfEngine.h"
#include "PdfDocument.h"
#include "IPdfPainter.h"
#include "PdfPainter.h"
#include "PdfPainterGPU.h"
#include "PdfContentParser.h"
#include "PdfDebug.h"
#include "PdfTextExtractor.h"
#include "PageRenderCache.h"
#include "FontCache.h"
#include "GlyphCache.h"
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <chrono>

// ---------------------------------------------
// File Helper
// ---------------------------------------------

static bool ReadAllBytes(const wchar_t* path, std::vector<uint8_t>& out)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;

    out.assign(std::istreambuf_iterator<char>(ifs),
        std::istreambuf_iterator<char>());

    return !out.empty();
}

static int g_lastStage = 0;
static FT_Library g_ftLib = nullptr;

// =====================================================
// ACTIVE DOCUMENT TRACKING
// Sadece aktif document render edilir, diğerleri skip
// =====================================================
static std::mutex g_activeDocMutex;
static PDF_DOCUMENT g_activeDocument = nullptr;
static bool g_useActiveDocumentFilter = true;  // Enable/disable filtering

// =====================================================
// RENDER MUTEX - Prevent concurrent renders crashing D2D/WIC
// Multiple threads can request renders simultaneously during zoom;
// serialize them to prevent resource conflicts
// =====================================================
static std::mutex g_renderMutex;

namespace pdf
{
    RenderQuality g_renderQuality;
}

using pdf::g_renderQuality;

bool InitFreeType()
{
    if (FT_Init_FreeType(&g_ftLib) != 0)
        return false;
    return true;
}

void ShutdownFreeType()
{
    if (g_ftLib)
        FT_Done_FreeType(g_ftLib);
    g_ftLib = nullptr;
}


// ---------------------------------------------
static int CountSubstring(const std::vector<uint8_t>& data, const char* needle)
{
    size_t n = data.size(), m = std::strlen(needle);
    if (n < m) return 0;

    int count = 0;
    for (size_t i = 0; i + m <= n; i++)
    {
        bool ok = true;
        for (size_t j = 0; j < m; j++)
        {
            if (data[i + j] != (unsigned char)needle[j])
            {
                ok = false;
                break;
            }
        }
        if (ok)
        {
            count++;
            i += m - 1;
        }
    }
    return count;
}

PDF_API int Pdf_Debug_GetLastStage()
{
    return g_lastStage;
}

// ---------------------------------------------
PDF_API int Pdf_GetVersion()
{
    return 47;
}

// ---------------------------------------------
PDF_API int Pdf_Debug_GetObjectCountFromFile(const wchar_t* path)
{
    std::vector<uint8_t> data;
    if (!ReadAllBytes(path, data)) return -1;

    int count = 0;
    size_t i = 0, n = data.size();

    while (i < n)
    {
        while (i < n && (data[i] < '0' || data[i] > '9')) i++;
        if (i >= n) break;

        while (i < n && std::isdigit(data[i])) i++;
        while (i < n && data[i] <= 32) i++;

        if (i >= n || !std::isdigit(data[i])) continue;

        while (i < n && std::isdigit(data[i])) i++;
        while (i < n && data[i] <= 32) i++;

        if (i + 3 <= n && data[i] == 'o' && data[i + 1] == 'b' && data[i + 2] == 'j')
            count++;

        i++;
    }

    return count;
}

// ---------------------------------------------
PDF_API int Pdf_Debug_GetPageCountFromFile(const wchar_t* path)
{
    std::vector<uint8_t> data;
    if (!ReadAllBytes(path, data)) return -1;

    int c1 = CountSubstring(data, "/Type /Page");
    int c2 = CountSubstring(data, "/Type/Page");

    return c1 + c2;
}

PDF_API void Pdf_SetZoomState(PDF_DOCUMENT ptr, int isZooming)
{
    if (isZooming)
        g_renderQuality.startZoom();
    else
        g_renderQuality.endZoom();
}

// ---------------------------------------------
PDF_API int Pdf_GetRealPageCountFromFile(const wchar_t* path)
{
    std::vector<uint8_t> data;
    if (!ReadAllBytes(path, data)) return -1;

    pdf::PdfDocument doc;
    if (!doc.loadFromBytes(data))
        return -2;

    return doc.getPageCountFromPageTree();
}

// ---------------------------------------------
struct PdfDocumentHandle
{
    std::vector<uint8_t> data;
    pdf::PdfDocument doc;
    pdf::PdfTextExtractor textExtractor;
};

// ---------------------------------------------
PDF_API PDF_DOCUMENT Pdf_OpenDocument(const wchar_t* path)
{
    pdf::PdfDebug::Init();
    LogDebug("=== Opening PDF: %ls ===", path);

    LogDebug("Step 1: Creating PdfDocumentHandle...");
    auto h = new PdfDocumentHandle();
    LogDebug("Step 1: DONE");

    LogDebug("Step 2: Reading file bytes...");
    if (!ReadAllBytes(path, h->data))
    {
        LogDebug("ERROR: Failed to read file");
        delete h;
        return nullptr;
    }
    LogDebug("Step 2: DONE - File size: %zu bytes", h->data.size());

    LogDebug("Step 3: Starting loadFromBytes...");
    if (!h->doc.loadFromBytes(h->data))
    {
        LogDebug("ERROR: loadFromBytes failed");
        delete h;
        return nullptr;
    }
    LogDebug("Step 3: loadFromBytes SUCCESS");

    LogDebug("Step 4: Returning handle...");
    return (PDF_DOCUMENT)h;
}

PDF_API void Pdf_CloseDocument(PDF_DOCUMENT ptr)
{
    if (!ptr) return;
    delete reinterpret_cast<PdfDocumentHandle*>(ptr);
}

// ---------------------------------------------
PDF_API int Pdf_GetPageCount(PDF_DOCUMENT ptr)
{
    if (!ptr) return -1;
    return reinterpret_cast<PdfDocumentHandle*>(ptr)->doc.getPageCountFromPageTree();
}

PDF_API int Pdf_GetPageSize(PDF_DOCUMENT ptr, int pageIndex, double* w, double* h)
{
    if (!ptr || !w || !h) return 0;

    auto& doc = reinterpret_cast<PdfDocumentHandle*>(ptr)->doc;
    return doc.getPageSize(pageIndex, *w, *h) ? 1 : 0;
}

PDF_API int Pdf_GetPageRotate(PDF_DOCUMENT ptr, int pageIndex)
{
    if (!ptr) return 0;
    return reinterpret_cast<PdfDocumentHandle*>(ptr)->doc.getPageRotate(pageIndex);
}

// ---------------------------------------------
PDF_API int Pdf_GetPageContent(
    PDF_DOCUMENT ptr,
    int pageIndex,
    uint8_t* out,
    int outCap)
{
    if (!ptr) return -1;

    auto& doc = reinterpret_cast<PdfDocumentHandle*>(ptr)->doc;
    std::vector<uint8_t> content;

    if (!doc.getPageContentsBytes(pageIndex, content))
        return 0;

    int len = (int)content.size();
    if (!out || outCap <= 0)
        return len;

    if (len > outCap) len = outCap;

    memcpy(out, content.data(), len);
    return len;
}

// ---------------------------------------------
PDF_API int Pdf_DecompressStream(
    const uint8_t* inData,
    int inSize,
    uint8_t* out,
    int cap)
{
    z_stream s{};
    s.next_in = (Bytef*)inData;
    s.avail_in = inSize;
    s.next_out = out;
    s.avail_out = cap;

    if (inflateInit(&s) != Z_OK)
        return -1;

    int ret = inflate(&s, Z_FINISH);
    inflateEnd(&s);

    if (ret != Z_STREAM_END)
        return -2;

    return s.total_out;
}

static int RenderImpl(
    PDF_DOCUMENT ptr,
    int pageIndex,
    double zoom,
    uint8_t* outBuffer,
    int outBufferSize,
    int* outW,
    int* outH,
    bool useGPU)
{
    // =====================================================
    // ACTIVE DOCUMENT CHECK - Quick skip for inactive docs
    // =====================================================
    if (g_useActiveDocumentFilter)
    {
        std::lock_guard<std::mutex> lock(g_activeDocMutex);
        if (g_activeDocument != nullptr && g_activeDocument != ptr)
        {
            // This is not the active document - skip rendering
            // Return 0 to indicate "no render needed"
            LogDebug("Skipping render for inactive document (page %d)", pageIndex);
            return 0;
        }
    }

    LogDebug("=== Rendering page %d, zoom %.2f, GPU=%d ===", pageIndex, zoom, useGPU);

    g_lastStage = 10;

    if (!ptr || !outW || !outH)
    {
        LogDebug("ERROR: Invalid parameters");
        return -1;
    }

    auto h = reinterpret_cast<PdfDocumentHandle*>(ptr);
    auto& doc = h->doc;

    g_lastStage = 20;
    LogDebug("Stage 20: Getting page size");

    double wPt = 0, hPt = 0;
    if (!doc.getPageSize(pageIndex, wPt, hPt))
    {
        LogDebug("ERROR: Could not get page size");
        return -2;
    }

    LogDebug("Page size: %.2f x %.2f pt", wPt, hPt);

    g_lastStage = 30;

    if (!(zoom > 0))
        zoom = 1.0;

    const double DPI = 96.0;
    const double scale = DPI / 72.0 * zoom;

    const int wPx = (int)std::llround(wPt * scale);
    const int hPx = (int)std::llround(hPt * scale);

    LogDebug("Pixel size: %d x %d", wPx, hPx);

    if (wPx <= 0 || hPx <= 0)
    {
        LogDebug("ERROR: Invalid pixel dimensions");
        return -3;
    }

    // WIC/D2D bitmap dimension safety limit
    // Very large bitmaps can fail to allocate or cause GPU resource exhaustion
    // 16384 x 16384 = 1GB RGBA, which is a practical safe maximum
    const int MAX_BITMAP_DIM = 16384;
    if (wPx > MAX_BITMAP_DIM || hPx > MAX_BITMAP_DIM)
    {
        LogDebug("ERROR: Pixel dimensions too large (%d x %d), max=%d", wPx, hPx, MAX_BITMAP_DIM);
        return -4;
    }

    const long long required64 = (long long)wPx * (long long)hPx * 4LL;
    if (required64 <= 0 || required64 > 0x7FFFFFFFLL)
    {
        LogDebug("ERROR: Buffer size overflow");
        return -4;
    }

    const int required = (int)required64;
    *outW = wPx;
    *outH = hPx;

    g_lastStage = 40;

    if (!outBuffer || outBufferSize <= 0)
        return required;

    if (outBufferSize < required)
        return required;

    // =====================================================
    // CHECK PAGE CACHE FIRST (zero-copy: memcpy directly from cache)
    // =====================================================
    g_lastStage = 45;

    if (pdf::PageRenderCache::instance().getDirect(h, pageIndex, wPx, hPx, outBuffer, required))
    {
        LogDebug("Cache HIT for page %d", pageIndex);
        g_lastStage = 150;
        return required;
    }
    LogDebug("Cache MISS for page %d", pageIndex);

    // =====================================================
    // RENDER MUTEX - Serialize actual rendering to prevent
    // concurrent D2D/WIC resource conflicts that cause crashes
    // Size queries and cache hits above don't need this lock
    // =====================================================
    std::lock_guard<std::mutex> renderLock(g_renderMutex);

    // Double-check cache after acquiring lock (another thread may have rendered it)
    if (pdf::PageRenderCache::instance().getDirect(h, pageIndex, wPx, hPx, outBuffer, required))
    {
        LogDebug("Cache HIT (after lock) for page %d", pageIndex);
        g_lastStage = 150;
        return required;
    }

    g_lastStage = 50;
    LogDebug("Stage 50: Starting painter initialization");

    std::vector<uint8_t> resultBuffer;

    if (useGPU)
    {
        pdf::PdfPainterGPU painter(wPx, hPx, scale, scale);

        if (!painter.initialize())
        {
            LogDebug("GPU initialization failed, falling back to CPU");
            useGPU = false;
            goto CPU_RENDERING;
        }

        g_lastStage = 60;

        painter.setPageRotation(0, wPt, hPt);
        painter.clear(0xFFFFFFFF);

        g_lastStage = 70;
        LogDebug("Stage 70: Starting GPU renderPageToPainter");

        doc.renderPageToPainter(pageIndex, painter);

        g_lastStage = 120;
        LogDebug("Stage 120: GPU rendering complete");

        // Check if EndDraw had a D2D error (device lost, resource exhaustion)
        if (painter.hasEndDrawError())
        {
            LogDebug("WARNING: GPU EndDraw failed, falling back to CPU for page %d", pageIndex);
            useGPU = false;
            goto CPU_RENDERING;
        }

        resultBuffer = painter.getBuffer();
        if ((int)resultBuffer.size() < required)
        {
            LogDebug("ERROR: GPU buffer size mismatch (%d vs %d), falling back to CPU", (int)resultBuffer.size(), required);
            useGPU = false;
            goto CPU_RENDERING;
        }

        g_lastStage = 140;

        std::memcpy(outBuffer, resultBuffer.data(), required);

        // Store in cache
        pdf::PageRenderCache::instance().store(h, pageIndex, wPx, hPx, zoom, resultBuffer);

        g_lastStage = 150;
        LogDebug("Stage 150: GPU rendering finished successfully");
        return required;
    }

CPU_RENDERING:
    {
        const int ssaa = g_renderQuality.getCurrentSSAA();
        LogDebug("CPU rendering with SSAA=%d", ssaa);

        pdf::PdfPainter painter(wPx, hPx, scale, scale, ssaa);

        g_lastStage = 60;

        painter.setPageRotation(0, wPt, hPt);
        painter.clear(0xFFFFFFFF);

        g_lastStage = 70;
        LogDebug("Stage 70: Starting CPU renderPageToPainter");

        doc.renderPageToPainter(pageIndex, painter);

        g_lastStage = 120;
        LogDebug("Stage 120: CPU rendering complete");

        resultBuffer = painter.getDownsampledBuffer();
        if ((int)resultBuffer.size() < required)
        {
            LogDebug("ERROR: CPU buffer size mismatch");
            return -5;
        }

        g_lastStage = 140;

        std::memcpy(outBuffer, resultBuffer.data(), required);

        // Store in cache
        pdf::PageRenderCache::instance().store(h, pageIndex, wPx, hPx, zoom, resultBuffer);

        g_lastStage = 150;
        LogDebug("Stage 150: CPU rendering finished successfully");
        return required;
    }
}



// ---------------------------------------------
//         FINAL RENDER (FONT DESTEKLİ)
// ---------------------------------------------
PDF_API int PDF_CALL Pdf_RenderPageToRgba(
    PDF_DOCUMENT ptr,
    int pageIndex,
    double zoom,
    uint8_t* outBuffer,
    int outBufferSize,
    int* outW,
    int* outH)
{
    __try
    {
        return RenderImpl(ptr, pageIndex, zoom, outBuffer, outBufferSize, outW, outH, true);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -999;
    }
}

PDF_API int PDF_CALL Pdf_RenderPageToRgba_CPU(
    PDF_DOCUMENT ptr,
    int pageIndex,
    double zoom,
    uint8_t* outBuffer,
    int outBufferSize,
    int* outW,
    int* outH)
{
    __try
    {
        return RenderImpl(ptr, pageIndex, zoom, outBuffer, outBufferSize, outW, outH, false);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -999;
    }
}

// =====================================================
// ACTIVE DOCUMENT API
// =====================================================

// Set the active document - only this document will be rendered
// Other documents' render calls will return early (quick skip)
PDF_API void PDF_CALL Pdf_SetActiveDocument(PDF_DOCUMENT ptr)
{
    std::lock_guard<std::mutex> lock(g_activeDocMutex);

    // If switching to a different document, optionally clear old cache
    if (g_activeDocument != nullptr && g_activeDocument != ptr)
    {
        // Clear previous document's cache to free memory
        auto* oldHandle = reinterpret_cast<PdfDocumentHandle*>(g_activeDocument);
        pdf::PageRenderCache::instance().clearDocument(oldHandle);
    }

    g_activeDocument = ptr;
}

// Get the current active document
PDF_API PDF_DOCUMENT PDF_CALL Pdf_GetActiveDocument()
{
    std::lock_guard<std::mutex> lock(g_activeDocMutex);
    return g_activeDocument;
}

// Enable/disable active document filtering
// When disabled, all documents render normally (legacy behavior)
PDF_API void PDF_CALL Pdf_EnableActiveDocumentFilter(bool enable)
{
    g_useActiveDocumentFilter = enable;
}

// Clear cache for a specific document
PDF_API void PDF_CALL Pdf_ClearDocumentCache(PDF_DOCUMENT ptr)
{
    if (!ptr) return;
    auto* handle = reinterpret_cast<PdfDocumentHandle*>(ptr);
    pdf::PageRenderCache::instance().clearDocument(handle);
}

// Clear all render cache
PDF_API void PDF_CALL Pdf_ClearAllCache()
{
    pdf::PageRenderCache::instance().clear();
    pdf::GlyphCache::instance().clear();
}

// Get cache statistics
PDF_API void PDF_CALL Pdf_GetCacheStats(
    size_t* outHits,
    size_t* outMisses,
    size_t* outCacheSize,
    size_t* outMemoryMB)
{
    if (outHits) *outHits = pdf::PageRenderCache::instance().hitCount();
    if (outMisses) *outMisses = pdf::PageRenderCache::instance().missCount();
    if (outCacheSize) *outCacheSize = pdf::PageRenderCache::instance().cacheSize();
    if (outMemoryMB) *outMemoryMB = pdf::PageRenderCache::instance().memoryUsage() / (1024 * 1024);
}

// =============================================
// ENCRYPTION API
// =============================================

PDF_API int PDF_CALL Pdf_GetEncryptionStatus(PDF_DOCUMENT ptr)
{
    if (!ptr) return 0;
    return reinterpret_cast<PdfDocumentHandle*>(ptr)->doc.getEncryptionStatus();
}

PDF_API int PDF_CALL Pdf_GetEncryptionType(PDF_DOCUMENT ptr)
{
    if (!ptr) return 0;
    return reinterpret_cast<PdfDocumentHandle*>(ptr)->doc.getEncryptionType();
}

PDF_API int PDF_CALL Pdf_TryPassword(PDF_DOCUMENT ptr, const char* password)
{
    if (!ptr || !password) return 0;
    return reinterpret_cast<PdfDocumentHandle*>(ptr)->doc.tryPassword(password) ? 1 : 0;
}

PDF_API int PDF_CALL Pdf_SupplyCertSeed(PDF_DOCUMENT ptr, const uint8_t* seedData, int seedLen)
{
    if (!ptr || !seedData || seedLen <= 0) return 0;
    return reinterpret_cast<PdfDocumentHandle*>(ptr)->doc.supplySeed(seedData, seedLen) ? 1 : 0;
}

PDF_API int PDF_CALL Pdf_GetCertRecipientCount(PDF_DOCUMENT ptr)
{
    if (!ptr) return 0;
    return (int)reinterpret_cast<PdfDocumentHandle*>(ptr)->doc.getCertRecipients().size();
}

PDF_API int PDF_CALL Pdf_GetCertRecipientEncryptedKey(
    PDF_DOCUMENT ptr, int recipientIndex,
    uint8_t* outData, int outDataSize)
{
    if (!ptr) return -1;
    const auto& recipients = reinterpret_cast<PdfDocumentHandle*>(ptr)->doc.getCertRecipients();
    if (recipientIndex < 0 || recipientIndex >= (int)recipients.size()) return -1;

    const auto& encKey = recipients[recipientIndex].encryptedKey;
    if (!outData) return (int)encKey.size(); // query size mode

    int copyLen = std::min((int)encKey.size(), outDataSize);
    if (copyLen > 0) memcpy(outData, encKey.data(), copyLen);
    return (int)encKey.size();
}

PDF_API int PDF_CALL Pdf_GetCertRecipientIssuerDer(
    PDF_DOCUMENT ptr, int recipientIndex,
    uint8_t* outData, int outDataSize)
{
    if (!ptr) return -1;
    const auto& recipients = reinterpret_cast<PdfDocumentHandle*>(ptr)->doc.getCertRecipients();
    if (recipientIndex < 0 || recipientIndex >= (int)recipients.size()) return -1;

    const auto& issuer = recipients[recipientIndex].issuerDer;
    if (!outData) return (int)issuer.size();

    int copyLen = std::min((int)issuer.size(), outDataSize);
    if (copyLen > 0) memcpy(outData, issuer.data(), copyLen);
    return (int)issuer.size();
}

PDF_API int PDF_CALL Pdf_GetCertRecipientSerial(
    PDF_DOCUMENT ptr, int recipientIndex,
    uint8_t* outData, int outDataSize)
{
    if (!ptr) return -1;
    const auto& recipients = reinterpret_cast<PdfDocumentHandle*>(ptr)->doc.getCertRecipients();
    if (recipientIndex < 0 || recipientIndex >= (int)recipients.size()) return -1;

    const auto& serial = recipients[recipientIndex].serialNumber;
    if (!outData) return (int)serial.size();

    int copyLen = std::min((int)serial.size(), outDataSize);
    if (copyLen > 0) memcpy(outData, serial.data(), copyLen);
    return (int)serial.size();
}

PDF_API int PDF_CALL Pdf_GetCertRecipientKeyAlgorithm(
    PDF_DOCUMENT ptr, int recipientIndex,
    char* outBuffer, int outBufferSize)
{
    if (!ptr) return -1;
    const auto& recipients = reinterpret_cast<PdfDocumentHandle*>(ptr)->doc.getCertRecipients();
    if (recipientIndex < 0 || recipientIndex >= (int)recipients.size()) return -1;

    const auto& oid = recipients[recipientIndex].keyEncAlgorithmOid;
    if (!outBuffer) return (int)oid.size();

    int copyLen = std::min((int)oid.size(), outBufferSize - 1);
    if (copyLen > 0) memcpy(outBuffer, oid.c_str(), copyLen);
    outBuffer[copyLen] = '\0';
    return (int)oid.size();
}


// =============================================
// TEXT EXTRACTION API IMPLEMENTATION
// =============================================

PDF_API int PDF_CALL Pdf_ExtractPageText(PDF_DOCUMENT ptr, int pageIndex)
{
    if (!ptr) return -1;
    auto* h = reinterpret_cast<PdfDocumentHandle*>(ptr);

    // Zaten extract edilmişse cache'den döndür
    if (h->textExtractor.hasPage(pageIndex))
        return h->textExtractor.getGlyphCount(pageIndex);

    return h->textExtractor.extractPage(h->doc, pageIndex);
}

PDF_API int PDF_CALL Pdf_GetTextGlyphCount(PDF_DOCUMENT ptr, int pageIndex)
{
    if (!ptr) return 0;
    auto* h = reinterpret_cast<PdfDocumentHandle*>(ptr);
    return h->textExtractor.getGlyphCount(pageIndex);
}

PDF_API int PDF_CALL Pdf_GetExtractedGlyphs(
    PDF_DOCUMENT ptr, int pageIndex,
    void* outBuffer, int maxCount)
{
    if (!ptr) return 0;
    auto* h = reinterpret_cast<PdfDocumentHandle*>(ptr);

    const auto& glyphs = h->textExtractor.getPageGlyphs(pageIndex);
    if (glyphs.empty()) return 0;

    int copyCount = std::min(maxCount, (int)glyphs.size());
    if (outBuffer && copyCount > 0)
    {
        memcpy(outBuffer, glyphs.data(),
            copyCount * sizeof(pdf::PdfTextGlyphExport));
    }
    return copyCount;
}

PDF_API int PDF_CALL Pdf_GetExtractedTextUtf8(
    PDF_DOCUMENT ptr, int pageIndex,
    char* outBuffer, int maxLen)
{
    // Glyph bazlı text seçimi kullanılıyor, UTF-8 export şimdilik devre dışı
    if (outBuffer && maxLen > 0) outBuffer[0] = '\0';
    return 0;
}

PDF_API void PDF_CALL Pdf_ClearTextCache(PDF_DOCUMENT ptr, int pageIndex)
{
    if (!ptr) return;
    auto* h = reinterpret_cast<PdfDocumentHandle*>(ptr);
    h->textExtractor.clearPage(pageIndex);
}

PDF_API void PDF_CALL Pdf_ClearAllTextCache(PDF_DOCUMENT ptr)
{
    if (!ptr) return;
    auto* h = reinterpret_cast<PdfDocumentHandle*>(ptr);
    h->textExtractor.clearAll();
}

// =====================================================
// LINK API
// =====================================================

PDF_API int PDF_CALL Pdf_GetPageLinkCount(PDF_DOCUMENT ptr, int pageIndex)
{
    if (!ptr) return 0;
    auto* h = reinterpret_cast<PdfDocumentHandle*>(ptr);

    std::vector<pdf::PdfLinkInfo> links;
    if (!h->doc.getPageLinks(pageIndex, links))
        return 0;
    return (int)links.size();
}

PDF_API int PDF_CALL Pdf_GetPageLinks(
    PDF_DOCUMENT ptr, int pageIndex,
    void* outLinksBuffer, int maxLinks,
    char* outUriBuffer, int uriBufferSize)
{
    if (!ptr) return 0;
    auto* h = reinterpret_cast<PdfDocumentHandle*>(ptr);

    std::vector<pdf::PdfLinkInfo> links;
    if (!h->doc.getPageLinks(pageIndex, links))
        return 0;

    if (links.empty()) return 0;

    int copyCount = std::min(maxLinks, (int)links.size());
    pdf::PdfLinkExport* outLinks = reinterpret_cast<pdf::PdfLinkExport*>(outLinksBuffer);

    int uriOffset = 0;
    for (int i = 0; i < copyCount; i++)
    {
        const auto& link = links[i];
        outLinks[i].x1 = link.x1;
        outLinks[i].y1 = link.y1;
        outLinks[i].x2 = link.x2;
        outLinks[i].y2 = link.y2;
        outLinks[i].destPage = link.destPage;

        if (!link.uri.empty() && outUriBuffer && uriOffset + (int)link.uri.size() + 1 <= uriBufferSize)
        {
            outLinks[i].uriOffset = uriOffset;
            outLinks[i].uriLength = (int)link.uri.size();
            memcpy(outUriBuffer + uriOffset, link.uri.c_str(), link.uri.size());
            outUriBuffer[uriOffset + link.uri.size()] = '\0';
            uriOffset += (int)link.uri.size() + 1;
        }
        else
        {
            outLinks[i].uriOffset = -1;
            outLinks[i].uriLength = 0;
        }
    }

    return copyCount;
}