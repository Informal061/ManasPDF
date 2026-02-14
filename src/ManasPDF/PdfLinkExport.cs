using System.Runtime.InteropServices;

namespace ManasPDF
{
    /// <summary>
    /// Raw link data from native layer (interop struct).
    /// </summary>
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct PdfLinkExport
    {
        /// <summary>Bounding box in PDF points (X1, Y1 = bottom-left, X2, Y2 = top-right).</summary>
        public double X1, Y1, X2, Y2;

        /// <summary>Internal page destination (-1 for external URI).</summary>
        public int DestPage;

        /// <summary>Offset into URI buffer.</summary>
        public int UriOffset;

        /// <summary>Length of URI string.</summary>
        public int UriLength;
    }

    /// <summary>
    /// Represents a hyperlink found on a PDF page.
    /// </summary>
    public class PdfLink
    {
        /// <summary>Bounding box in PDF points.</summary>
        public double X1 { get; init; }
        public double Y1 { get; init; }
        public double X2 { get; init; }
        public double Y2 { get; init; }

        /// <summary>External URI (http://, mailto:, etc.), or null for internal links.</summary>
        public string? Uri { get; init; }

        /// <summary>Internal page destination (0-based), or -1 for external links.</summary>
        public int DestPage { get; init; } = -1;

        /// <summary>True if this is an external link (has URI).</summary>
        public bool IsExternal => DestPage < 0 && !string.IsNullOrEmpty(Uri);

        /// <summary>True if this is an internal page link.</summary>
        public bool IsInternal => DestPage >= 0;

        public override string ToString()
            => IsExternal ? $"URI: {Uri}" : $"Page: {DestPage + 1}";
    }
}
