#pragma once

#include <cstdint>
#include <string>
#include <map>
#include <vector>
#include <memory>
#include <chrono>

#include "PdfDocument.h"
#include "PdfPainter.h"
#include "PdfPainterGPU.h"
#include "PdfContentParser.h"
#include "PdfObject.h"
#include "PdfTextExtractor.h"

#ifdef PDFCORE_EXPORTS
#define PDF_API extern "C" __declspec(dllexport)
#else
#define PDF_API extern "C" __declspec(dllimport)
#endif

#ifndef PDF_CALL
#define PDF_CALL __cdecl
#endif

typedef void* PDF_DOCUMENT;

namespace pdf
{
    // Zoom state için yardımcı struct
    struct RenderQuality
    {
        bool isZooming = false;
        int ssaa = 1;  // 🚀 CHANGED: Default SSAA=1 for speed (was 2)

        void startZoom()
        {
            isZooming = true;
            ssaa = 1;  // Hızlı render
        }

        void endZoom()
        {
            isZooming = false;
            ssaa = 1;  // 🚀 Keep SSAA=1 for speed
        }

        int getCurrentSSAA() const
        {
            return ssaa;  // Always return current ssaa
        }
    };

    extern RenderQuality g_renderQuality;

    // Link export structure for interop
    #pragma pack(push, 1)
    struct PdfLinkExport
    {
        double x1, y1, x2, y2;  // Bounding box in PDF points
        int destPage;           // Internal destination (-1 for external URI)
        int uriOffset;          // Offset into URI buffer
        int uriLength;          // Length of URI string
    };
    #pragma pack(pop)

    class PdfEngine
    {
    public:
        PdfEngine() = default;

        void LogDebug(const char* format, ...);

        bool load(const std::vector<uint8_t>& data)
        {
            return _doc.loadFromBytes(data);
        }

        int getPageCount() const
        {
            return _doc.getPageCountFromPageTree();
        }

        bool getPageSize(int pageIndex, double& wPt, double& hPt) const
        {
            return _doc.getPageSize(pageIndex, wPt, hPt);
        }

        int getPageRotate(int pageIndex) const
        {
            return _doc.getPageRotate(pageIndex);
        }

        bool getPageContent(int pageIndex, std::vector<uint8_t>& out) const
        {
            return _doc.getPageContentsBytes(pageIndex, out);
        }

        bool renderPage(int pageIndex, PdfPainter& painter, double /*zoom*/)
        {
            painter.clear(0xFFFFFFFF);

            std::vector<uint8_t> content;
            if (!_doc.getPageContentsBytes(pageIndex, content))
                return false;

            std::map<std::string, PdfFontInfo> fonts;
            _doc.getPageFonts(pageIndex, fonts);

            PdfGraphicsState initialGs;
            initialGs.ctm = PdfMatrix();

            std::vector<std::shared_ptr<PdfDictionary>> resourceStack;
            _doc.getPageResources(pageIndex, resourceStack);
            std::reverse(resourceStack.begin(), resourceStack.end());

            double wPt = 0, hPt = 0;
            if (_doc.getPageSize(pageIndex, wPt, hPt))
            {
                int rot = _doc.getPageRotate(pageIndex);
                painter.setPageRotation(rot, wPt, hPt);
            }

            PdfContentParser parser(
                content,
                &painter,
                &_doc,
                pageIndex,
                &fonts,
                initialGs,
                resourceStack
            );

            parser.parse();
            return true;
        }

        PdfDocument& document() { return _doc; }
        const PdfDocument& document() const { return _doc; }

        PdfTextExtractor& textExtractor() { return _textExtractor; }

    private:
        PdfDocument _doc;
        PdfTextExtractor _textExtractor;
    };
}

// =============================================
// API EXPORTS
// =============================================

// Zoom state management
PDF_API void Pdf_SetZoomState(PDF_DOCUMENT doc, int isZooming);

// 🚀 Cache management
PDF_API void Pdf_ClearCache(PDF_DOCUMENT doc);

// Info & Debug
PDF_API int Pdf_GetVersion();
PDF_API int Pdf_Debug_GetLastStage();
PDF_API int Pdf_Debug_GetObjectCountFromFile(const wchar_t* path);
PDF_API int Pdf_Debug_GetPageCountFromFile(const wchar_t* path);
PDF_API int Pdf_GetRealPageCountFromFile(const wchar_t* path);

// Document management
PDF_API PDF_DOCUMENT Pdf_OpenDocument(const wchar_t* path);
PDF_API void Pdf_CloseDocument(PDF_DOCUMENT doc);

// Page info
PDF_API int Pdf_GetPageCount(PDF_DOCUMENT doc);
PDF_API int Pdf_GetPageSize(PDF_DOCUMENT doc, int pageIndex, double* widthPt, double* heightPt);
PDF_API int Pdf_GetPageRotate(PDF_DOCUMENT doc, int pageIndex);
PDF_API int Pdf_GetPageContent(PDF_DOCUMENT doc, int pageIndex, uint8_t* outBuffer, int bufferSize);
PDF_API int Pdf_DecompressStream(const uint8_t* input, int inputSize, uint8_t* output, int outputCapacity);

// =============================================
// RENDER FUNCTIONS
// =============================================

// Standard render (GPU with CPU fallback, uses g_renderQuality)
PDF_API int PDF_CALL Pdf_RenderPageToRgba(
    PDF_DOCUMENT ptr,
    int pageIndex,
    double zoom,
    uint8_t* outBuffer,
    int outBufferSize,
    int* outW,
    int* outH);

// CPU only render (uses g_renderQuality for SSAA)
PDF_API int PDF_CALL Pdf_RenderPageToRgba_CPU(
    PDF_DOCUMENT ptr,
    int pageIndex,
    double zoom,
    uint8_t* outBuffer,
    int outBufferSize,
    int* outW,
    int* outH);

// 🚀 FAST RENDER - No SSAA, ~4x faster (for preview/initial load)
PDF_API int PDF_CALL Pdf_RenderPageToRgba_Fast(
    PDF_DOCUMENT ptr,
    int pageIndex,
    double zoom,
    uint8_t* outBuffer,
    int outBufferSize,
    int* outW,
    int* outH);

// 🎨 QUALITY RENDER - SSAA=2, best quality (for final display)
PDF_API int PDF_CALL Pdf_RenderPageToRgba_Quality(
    PDF_DOCUMENT ptr,
    int pageIndex,
    double zoom,
    uint8_t* outBuffer,
    int outBufferSize,
    int* outW,
    int* outH);

// =============================================
// ACTIVE DOCUMENT API - Multi-tab optimization
// =============================================

// Set active document - only this document will render, others skip
PDF_API void PDF_CALL Pdf_SetActiveDocument(PDF_DOCUMENT ptr);

// Get current active document
PDF_API PDF_DOCUMENT PDF_CALL Pdf_GetActiveDocument();

// Enable/disable active document filtering (default: enabled)
PDF_API void PDF_CALL Pdf_EnableActiveDocumentFilter(bool enable);

// Clear cache for specific document
PDF_API void PDF_CALL Pdf_ClearDocumentCache(PDF_DOCUMENT ptr);

// Clear all render cache (pages + glyphs)
PDF_API void PDF_CALL Pdf_ClearAllCache();

// Get cache statistics
PDF_API void PDF_CALL Pdf_GetCacheStats(
    size_t* outHits,
    size_t* outMisses,
    size_t* outCacheSize,
    size_t* outMemoryMB);

// =============================================
// ENCRYPTION API
// =============================================

// Get encryption status: 0=not encrypted, 1=encrypted+ready, -1=needs credentials
PDF_API int PDF_CALL Pdf_GetEncryptionStatus(PDF_DOCUMENT doc);

// Get encryption type: 0=none, 1=password (/Standard), 2=certificate (/Adobe.PubSec)
PDF_API int PDF_CALL Pdf_GetEncryptionType(PDF_DOCUMENT doc);

// Try user/owner password (for /Standard encryption)
// Returns 1 on success, 0 on failure
PDF_API int PDF_CALL Pdf_TryPassword(PDF_DOCUMENT doc, const char* password);

// Supply decrypted seed for certificate encryption (/Adobe.PubSec)
// seedData: pointer to decrypted seed bytes (typically 20 bytes)
// seedLen: length of seed
// Returns 1 on success, 0 on failure
PDF_API int PDF_CALL Pdf_SupplyCertSeed(PDF_DOCUMENT doc, const uint8_t* seedData, int seedLen);

// Get number of certificate recipients
PDF_API int PDF_CALL Pdf_GetCertRecipientCount(PDF_DOCUMENT doc);

// Get encrypted key data for a specific recipient (for RSA decryption on C# side)
// recipientIndex: 0-based index
// outData: buffer to receive encrypted key bytes
// outDataSize: size of output buffer
// Returns: actual size of encrypted key, or -1 on error
// Call with outData=NULL to get required size
PDF_API int PDF_CALL Pdf_GetCertRecipientEncryptedKey(
    PDF_DOCUMENT doc, int recipientIndex,
    uint8_t* outData, int outDataSize);

// Get issuer DER for a specific recipient (for certificate matching)
// Returns: actual size of issuer DER, or -1 on error
PDF_API int PDF_CALL Pdf_GetCertRecipientIssuerDer(
    PDF_DOCUMENT doc, int recipientIndex,
    uint8_t* outData, int outDataSize);

// Get serial number for a specific recipient
// Returns: actual size of serial number bytes, or -1 on error
PDF_API int PDF_CALL Pdf_GetCertRecipientSerial(
    PDF_DOCUMENT doc, int recipientIndex,
    uint8_t* outData, int outDataSize);

// Get key encryption algorithm OID string for a specific recipient
// outBuffer: buffer to receive null-terminated OID string
// outBufferSize: size of output buffer
// Returns: actual string length (excluding null), or -1 on error
PDF_API int PDF_CALL Pdf_GetCertRecipientKeyAlgorithm(
    PDF_DOCUMENT doc, int recipientIndex,
    char* outBuffer, int outBufferSize);

// =============================================
// TEXT EXTRACTION API
// =============================================

// Extract text from page - returns glyph count, or -1 on error
// Stores results internally (cached per page)
PDF_API int PDF_CALL Pdf_ExtractPageText(PDF_DOCUMENT doc, int pageIndex);

// Get glyph count for previously extracted page
PDF_API int PDF_CALL Pdf_GetTextGlyphCount(PDF_DOCUMENT doc, int pageIndex);

// Get extracted glyph data (call after Pdf_ExtractPageText)
// Each glyph: PdfTextGlyphExport struct (24 bytes packed)
// outBuffer: pointer to PdfTextGlyphExport array
// maxCount: max number of glyphs to write
// Returns: actual number of glyphs written
PDF_API int PDF_CALL Pdf_GetExtractedGlyphs(
    PDF_DOCUMENT doc, int pageIndex,
    void* outBuffer, int maxCount);

// Get extracted text as UTF-8 string (call after Pdf_ExtractPageText)
// outBuffer: buffer to receive UTF-8 string (null-terminated)
// maxLen: size of output buffer
// Returns: actual string length (excluding null), or -1 on error
PDF_API int PDF_CALL Pdf_GetExtractedTextUtf8(
    PDF_DOCUMENT doc, int pageIndex,
    char* outBuffer, int maxLen);

// Clear text extraction cache for a page
PDF_API void PDF_CALL Pdf_ClearTextCache(PDF_DOCUMENT doc, int pageIndex);

// Clear all text extraction cache
PDF_API void PDF_CALL Pdf_ClearAllTextCache(PDF_DOCUMENT doc);