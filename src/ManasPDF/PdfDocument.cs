using System;
using System.Runtime.InteropServices;
using ManasPDF.Internal;

namespace ManasPDF
{
    /// <summary>
    /// Represents a PDF document. Provides access to pages, rendering, text extraction, and links.
    /// Implements <see cref="IDisposable"/> for automatic native resource cleanup.
    /// </summary>
    /// <example>
    /// <code>
    /// using var doc = PdfDocument.Open("file.pdf");
    /// Console.WriteLine($"Pages: {doc.PageCount}");
    ///
    /// var page = doc.GetPage(0);
    /// byte[]? pixels = page.Render(1.5, out int w, out int h);
    /// string text = page.ExtractText();
    /// </code>
    /// </example>
    public sealed class PdfDocument : IDisposable
    {
        private IntPtr _handle;
        private bool _disposed;

        private PdfDocument(IntPtr handle)
        {
            _handle = handle;
        }

        /// <summary>
        /// Opens a PDF document from a file path.
        /// </summary>
        /// <param name="path">Full path to the PDF file.</param>
        /// <returns>A new PdfDocument instance.</returns>
        /// <exception cref="PdfException">Thrown when the file cannot be opened.</exception>
        public static PdfDocument Open(string path)
        {
            if (string.IsNullOrEmpty(path))
                throw new ArgumentException("Path cannot be null or empty.", nameof(path));

            IntPtr handle = NativeApi.Pdf_OpenDocument(path);
            if (handle == IntPtr.Zero)
                throw new PdfException($"Failed to open PDF: {path}");

            return new PdfDocument(handle);
        }

        /// <summary>
        /// Gets the native library version.
        /// </summary>
        public static int NativeVersion => NativeApi.Pdf_GetVersion();

        /// <summary>
        /// Number of pages in the document.
        /// </summary>
        public int PageCount
        {
            get
            {
                ThrowIfDisposed();
                return NativeApi.Pdf_GetPageCount(_handle);
            }
        }

        /// <summary>
        /// Gets a page by index (0-based).
        /// </summary>
        public PdfPage GetPage(int index)
        {
            ThrowIfDisposed();
            if (index < 0 || index >= PageCount)
                throw new ArgumentOutOfRangeException(nameof(index));
            return new PdfPage(_handle, index);
        }

        // =============================================
        // ENCRYPTION
        // =============================================

        /// <summary>
        /// Encryption status: 0 = not encrypted, 1 = encrypted and ready, -1 = needs credentials.
        /// </summary>
        public int EncryptionStatus
        {
            get
            {
                ThrowIfDisposed();
                return NativeApi.Pdf_GetEncryptionStatus(_handle);
            }
        }

        /// <summary>
        /// Encryption type: 0 = none, 1 = password (/Standard), 2 = certificate (/Adobe.PubSec).
        /// </summary>
        public int EncryptionType
        {
            get
            {
                ThrowIfDisposed();
                return NativeApi.Pdf_GetEncryptionType(_handle);
            }
        }

        /// <summary>
        /// True if the document requires a password or certificate to decrypt.
        /// </summary>
        public bool NeedsCredentials => EncryptionStatus == -1;

        /// <summary>
        /// True if the document is password-protected (/Standard encryption).
        /// </summary>
        public bool IsPasswordProtected => EncryptionType == 1;

        /// <summary>
        /// True if the document is certificate-encrypted (/Adobe.PubSec).
        /// </summary>
        public bool IsCertificateEncrypted => EncryptionType == 2;

        /// <summary>
        /// Attempts to decrypt the document with a password.
        /// </summary>
        /// <returns>True if the password was accepted.</returns>
        public bool TryPassword(string password)
        {
            ThrowIfDisposed();
            return NativeApi.Pdf_TryPassword(_handle, password) == 1;
        }

        /// <summary>
        /// Supplies a decrypted certificate seed for certificate-based encryption.
        /// </summary>
        /// <returns>True if the seed was accepted.</returns>
        public bool SupplyCertificateSeed(byte[] seedData)
        {
            ThrowIfDisposed();
            if (seedData == null || seedData.Length == 0)
                throw new ArgumentException("Seed data cannot be null or empty.", nameof(seedData));
            return NativeApi.Pdf_SupplyCertSeed(_handle, seedData, seedData.Length) == 1;
        }

        /// <summary>
        /// Gets the number of certificate recipients (for certificate-encrypted PDFs).
        /// </summary>
        public int CertificateRecipientCount
        {
            get
            {
                ThrowIfDisposed();
                return NativeApi.Pdf_GetCertRecipientCount(_handle);
            }
        }

        /// <summary>
        /// Gets the encrypted key data for a certificate recipient.
        /// </summary>
        public byte[]? GetRecipientEncryptedKey(int recipientIndex)
        {
            ThrowIfDisposed();
            int size = NativeApi.Pdf_GetCertRecipientEncryptedKey(_handle, recipientIndex, null, 0);
            if (size <= 0) return null;

            var data = new byte[size];
            int written = NativeApi.Pdf_GetCertRecipientEncryptedKey(_handle, recipientIndex, data, size);
            return written > 0 ? data : null;
        }

        /// <summary>
        /// Gets the issuer DER data for a certificate recipient (for matching).
        /// </summary>
        public byte[]? GetRecipientIssuerDer(int recipientIndex)
        {
            ThrowIfDisposed();
            int size = NativeApi.Pdf_GetCertRecipientIssuerDer(_handle, recipientIndex, null, 0);
            if (size <= 0) return null;

            var data = new byte[size];
            int written = NativeApi.Pdf_GetCertRecipientIssuerDer(_handle, recipientIndex, data, size);
            return written > 0 ? data : null;
        }

        /// <summary>
        /// Gets the serial number for a certificate recipient.
        /// </summary>
        public byte[]? GetRecipientSerial(int recipientIndex)
        {
            ThrowIfDisposed();
            int size = NativeApi.Pdf_GetCertRecipientSerial(_handle, recipientIndex, null, 0);
            if (size <= 0) return null;

            var data = new byte[size];
            int written = NativeApi.Pdf_GetCertRecipientSerial(_handle, recipientIndex, data, size);
            return written > 0 ? data : null;
        }

        // =============================================
        // CACHE MANAGEMENT
        // =============================================

        /// <summary>
        /// Clears the render cache for this document.
        /// </summary>
        public void ClearCache()
        {
            ThrowIfDisposed();
            NativeApi.Pdf_ClearDocumentCache(_handle);
        }

        /// <summary>
        /// Clears all text extraction caches for this document.
        /// </summary>
        public void ClearAllTextCache()
        {
            ThrowIfDisposed();
            NativeApi.Pdf_ClearAllTextCache(_handle);
        }

        /// <summary>
        /// Clears all render caches globally (all documents).
        /// </summary>
        public static void ClearAllCaches()
        {
            NativeApi.Pdf_ClearAllCache();
        }

        /// <summary>
        /// Gets global cache statistics.
        /// </summary>
        public static CacheStats GetCacheStats()
        {
            NativeApi.Pdf_GetCacheStats(out ulong hits, out ulong misses, out ulong size, out ulong memMB);
            return new CacheStats(hits, misses, size, memMB);
        }

        // =============================================
        // ACTIVE DOCUMENT (multi-tab optimization)
        // =============================================

        /// <summary>
        /// Sets this document as the active document. Only the active document
        /// will be rendered; other documents' render calls are skipped.
        /// Useful for multi-tab viewers.
        /// </summary>
        public void SetAsActive()
        {
            ThrowIfDisposed();
            NativeApi.Pdf_SetActiveDocument(_handle);
        }

        /// <summary>
        /// Enables or disables the active document filter.
        /// When disabled, all documents render normally.
        /// </summary>
        public static void EnableActiveDocumentFilter(bool enable)
        {
            NativeApi.Pdf_EnableActiveDocumentFilter(enable);
        }

        // =============================================
        // INTERNAL
        // =============================================

        /// <summary>
        /// Gets the native document handle. For advanced interop scenarios.
        /// </summary>
        public IntPtr Handle
        {
            get
            {
                ThrowIfDisposed();
                return _handle;
            }
        }

        private void ThrowIfDisposed()
        {
            if (_disposed)
                throw new ObjectDisposedException(nameof(PdfDocument));
        }

        public void Dispose()
        {
            if (!_disposed)
            {
                _disposed = true;
                if (_handle != IntPtr.Zero)
                {
                    NativeApi.Pdf_ClearDocumentCache(_handle);
                    NativeApi.Pdf_CloseDocument(_handle);
                    _handle = IntPtr.Zero;
                }
            }
        }
    }

    /// <summary>
    /// Render cache statistics.
    /// </summary>
    public readonly struct CacheStats
    {
        public ulong Hits { get; }
        public ulong Misses { get; }
        public ulong CacheSize { get; }
        public ulong MemoryMB { get; }
        public double HitRate => (Hits + Misses) > 0 ? (double)Hits / (Hits + Misses) * 100 : 0;

        internal CacheStats(ulong hits, ulong misses, ulong cacheSize, ulong memoryMB)
        {
            Hits = hits;
            Misses = misses;
            CacheSize = cacheSize;
            MemoryMB = memoryMB;
        }
    }
}
