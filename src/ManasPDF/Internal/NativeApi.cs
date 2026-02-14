using System;
using System.Runtime.InteropServices;

namespace ManasPDF.Internal
{
    /// <summary>
    /// Low-level P/Invoke bindings to ManasPDFCore.dll.
    /// These are internal - use <see cref="ManasPDF.PdfDocument"/> for the public API.
    /// </summary>
    internal static class NativeApi
    {
        private const string DllName = "ManasPDFCore.dll";

        // =============================================
        // VERSION & DEBUG
        // =============================================

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int Pdf_GetVersion();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int Pdf_Debug_GetLastStage();

        // =============================================
        // DOCUMENT MANAGEMENT
        // =============================================

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        internal static extern IntPtr Pdf_OpenDocument(string path);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void Pdf_CloseDocument(IntPtr doc);

        // =============================================
        // PAGE INFO
        // =============================================

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int Pdf_GetPageCount(IntPtr doc);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int Pdf_GetPageSize(
            IntPtr doc, int pageIndex,
            out double widthPt, out double heightPt);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int Pdf_GetPageRotate(IntPtr doc, int pageIndex);

        // =============================================
        // RENDERING
        // =============================================

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int Pdf_RenderPageToRgba(
            IntPtr doc, int pageIndex, double zoom,
            IntPtr buffer, int bufferSize,
            out int outWidth, out int outHeight);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int Pdf_RenderPageToRgba_CPU(
            IntPtr doc, int pageIndex, double zoom,
            IntPtr buffer, int bufferSize,
            out int outWidth, out int outHeight);

        // =============================================
        // ACTIVE DOCUMENT API
        // =============================================

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void Pdf_SetActiveDocument(IntPtr doc);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr Pdf_GetActiveDocument();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void Pdf_EnableActiveDocumentFilter([MarshalAs(UnmanagedType.I1)] bool enable);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void Pdf_ClearDocumentCache(IntPtr doc);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void Pdf_ClearAllCache();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void Pdf_GetCacheStats(
            out ulong outHits, out ulong outMisses,
            out ulong outCacheSize, out ulong outMemoryMB);

        // =============================================
        // ZOOM STATE
        // =============================================

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void Pdf_SetZoomState(IntPtr doc, int isZooming);

        // =============================================
        // ENCRYPTION API
        // =============================================

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int Pdf_GetEncryptionStatus(IntPtr doc);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int Pdf_GetEncryptionType(IntPtr doc);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        internal static extern int Pdf_TryPassword(IntPtr doc, string password);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int Pdf_SupplyCertSeed(IntPtr doc, byte[] seedData, int seedLen);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int Pdf_GetCertRecipientCount(IntPtr doc);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int Pdf_GetCertRecipientEncryptedKey(
            IntPtr doc, int recipientIndex,
            byte[]? outData, int outDataSize);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int Pdf_GetCertRecipientIssuerDer(
            IntPtr doc, int recipientIndex,
            byte[]? outData, int outDataSize);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int Pdf_GetCertRecipientSerial(
            IntPtr doc, int recipientIndex,
            byte[]? outData, int outDataSize);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        internal static extern int Pdf_GetCertRecipientKeyAlgorithm(
            IntPtr doc, int recipientIndex,
            [Out] byte[]? outBuffer, int outBufferSize);

        // =============================================
        // TEXT EXTRACTION API
        // =============================================

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int Pdf_ExtractPageText(IntPtr doc, int pageIndex);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int Pdf_GetTextGlyphCount(IntPtr doc, int pageIndex);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int Pdf_GetExtractedGlyphs(
            IntPtr doc, int pageIndex,
            [Out] PdfTextGlyph[]? outGlyphs, int maxCount);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        internal static extern int Pdf_GetExtractedTextUtf8(
            IntPtr doc, int pageIndex,
            [Out] byte[]? outBuffer, int maxLen);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void Pdf_ClearTextCache(IntPtr doc, int pageIndex);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void Pdf_ClearAllTextCache(IntPtr doc);

        // =============================================
        // LINK API
        // =============================================

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int Pdf_GetPageLinkCount(IntPtr doc, int pageIndex);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int Pdf_GetPageLinks(
            IntPtr doc, int pageIndex,
            [Out] PdfLinkExport[]? outLinks, int maxLinks,
            [Out] byte[]? outUriBuffer, int uriBufferSize);
    }
}
