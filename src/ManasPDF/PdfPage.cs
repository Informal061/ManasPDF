using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using ManasPDF.Internal;

namespace ManasPDF
{
    /// <summary>
    /// Represents a single page of a PDF document.
    /// </summary>
    public class PdfPage
    {
        private readonly IntPtr _docHandle;
        private readonly int _index;

        internal PdfPage(IntPtr docHandle, int index)
        {
            _docHandle = docHandle;
            _index = index;
        }

        /// <summary>0-based page index.</summary>
        public int Index => _index;

        /// <summary>Page width in PDF points (1 point = 1/72 inch).</summary>
        public double Width
        {
            get
            {
                NativeApi.Pdf_GetPageSize(_docHandle, _index, out double w, out _);
                return w;
            }
        }

        /// <summary>Page height in PDF points.</summary>
        public double Height
        {
            get
            {
                NativeApi.Pdf_GetPageSize(_docHandle, _index, out _, out double h);
                return h;
            }
        }

        /// <summary>Page rotation in degrees (0, 90, 180, 270).</summary>
        public int Rotation => NativeApi.Pdf_GetPageRotate(_docHandle, _index);

        /// <summary>
        /// Renders the page to a BGRA32 pixel buffer.
        /// </summary>
        /// <param name="zoom">Zoom factor (1.0 = 100%).</param>
        /// <param name="width">Output bitmap width in pixels.</param>
        /// <param name="height">Output bitmap height in pixels.</param>
        /// <returns>Pixel data in BGRA32 format, or null on failure.</returns>
        public byte[]? Render(double zoom, out int width, out int height)
        {
            // First call: get required buffer size
            int required = NativeApi.Pdf_RenderPageToRgba(
                _docHandle, _index, zoom,
                IntPtr.Zero, 0, out width, out height);

            if (required <= 0 || width <= 0 || height <= 0)
            {
                width = 0;
                height = 0;
                return null;
            }

            // Overflow check
            long expected = (long)width * height * 4L;
            if (expected > int.MaxValue || required != (int)expected)
            {
                width = 0;
                height = 0;
                return null;
            }

            var buffer = new byte[required];
            var gc = GCHandle.Alloc(buffer, GCHandleType.Pinned);
            try
            {
                int written = NativeApi.Pdf_RenderPageToRgba(
                    _docHandle, _index, zoom,
                    gc.AddrOfPinnedObject(), buffer.Length,
                    out width, out height);

                if (written <= 0)
                {
                    width = 0;
                    height = 0;
                    return null;
                }
            }
            finally
            {
                gc.Free();
            }

            return buffer;
        }

        /// <summary>
        /// Renders the page using CPU software renderer (fallback).
        /// </summary>
        public byte[]? RenderCpu(double zoom, out int width, out int height)
        {
            int required = NativeApi.Pdf_RenderPageToRgba_CPU(
                _docHandle, _index, zoom,
                IntPtr.Zero, 0, out width, out height);

            if (required <= 0 || width <= 0 || height <= 0)
            {
                width = 0;
                height = 0;
                return null;
            }

            long expected = (long)width * height * 4L;
            if (expected > int.MaxValue || required != (int)expected)
            {
                width = 0;
                height = 0;
                return null;
            }

            var buffer = new byte[required];
            var gc = GCHandle.Alloc(buffer, GCHandleType.Pinned);
            try
            {
                int written = NativeApi.Pdf_RenderPageToRgba_CPU(
                    _docHandle, _index, zoom,
                    gc.AddrOfPinnedObject(), buffer.Length,
                    out width, out height);

                if (written <= 0)
                {
                    width = 0;
                    height = 0;
                    return null;
                }
            }
            finally
            {
                gc.Free();
            }

            return buffer;
        }

        /// <summary>
        /// Extracts text glyphs from the page with position information.
        /// </summary>
        public PdfTextGlyph[] ExtractGlyphs()
        {
            int count = NativeApi.Pdf_ExtractPageText(_docHandle, _index);
            if (count <= 0)
                return Array.Empty<PdfTextGlyph>();

            var glyphs = new PdfTextGlyph[count];
            int written = NativeApi.Pdf_GetExtractedGlyphs(_docHandle, _index, glyphs, count);
            if (written <= 0)
                return Array.Empty<PdfTextGlyph>();

            if (written < count)
                Array.Resize(ref glyphs, written);

            return glyphs;
        }

        /// <summary>
        /// Extracts text content from the page as a string.
        /// </summary>
        public string ExtractText()
        {
            int count = NativeApi.Pdf_ExtractPageText(_docHandle, _index);
            if (count <= 0)
                return string.Empty;

            // Estimate buffer size (4 bytes per glyph average for UTF-8)
            int bufferSize = count * 4 + 1024;
            var buffer = new byte[bufferSize];
            int len = NativeApi.Pdf_GetExtractedTextUtf8(_docHandle, _index, buffer, bufferSize);
            if (len <= 0)
                return string.Empty;

            return Encoding.UTF8.GetString(buffer, 0, len);
        }

        /// <summary>
        /// Gets all hyperlinks on this page.
        /// </summary>
        public PdfLink[] GetLinks()
        {
            int count = NativeApi.Pdf_GetPageLinkCount(_docHandle, _index);
            if (count <= 0)
                return Array.Empty<PdfLink>();

            var exports = new PdfLinkExport[count];
            var uriBuffer = new byte[count * 256]; // generous URI buffer
            int written = NativeApi.Pdf_GetPageLinks(
                _docHandle, _index,
                exports, count,
                uriBuffer, uriBuffer.Length);

            if (written <= 0)
                return Array.Empty<PdfLink>();

            var links = new PdfLink[written];
            for (int i = 0; i < written; i++)
            {
                var e = exports[i];
                string? uri = null;
                if (e.UriLength > 0 && e.UriOffset >= 0 && e.UriOffset + e.UriLength <= uriBuffer.Length)
                    uri = Encoding.UTF8.GetString(uriBuffer, e.UriOffset, e.UriLength);

                links[i] = new PdfLink
                {
                    X1 = e.X1,
                    Y1 = e.Y1,
                    X2 = e.X2,
                    Y2 = e.Y2,
                    DestPage = e.DestPage,
                    Uri = uri
                };
            }

            return links;
        }

        /// <summary>
        /// Clears the cached text extraction data for this page.
        /// </summary>
        public void ClearTextCache()
        {
            NativeApi.Pdf_ClearTextCache(_docHandle, _index);
        }
    }
}
