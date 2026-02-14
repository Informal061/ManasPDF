using System.Runtime.InteropServices;

namespace ManasPDF
{
    /// <summary>
    /// Represents a single text glyph extracted from a PDF page.
    /// Coordinates are in bitmap-space at zoom=1 (points × 96/72).
    /// </summary>
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct PdfTextGlyph
    {
        /// <summary>Unicode code point.</summary>
        public uint Unicode;

        /// <summary>X position in bitmap-space (zoom=1).</summary>
        public float X;

        /// <summary>Y position in bitmap-space (zoom=1, top-left origin).</summary>
        public float Y;

        /// <summary>Glyph advance width (points).</summary>
        public float Width;

        /// <summary>Glyph height (points).</summary>
        public float Height;

        /// <summary>Effective font size (points).</summary>
        public float FontSize;

        /// <summary>Returns true if this glyph is a space character.</summary>
        public bool IsSpace => Unicode == 32 || Unicode == 0x00A0;

        /// <summary>Converts the Unicode value to a char. Returns U+FFFD for surrogates.</summary>
        public char ToChar() => Unicode <= 0xFFFF ? (char)Unicode : '\uFFFD';

        public override string ToString()
            => $"U+{Unicode:X4} '{ToChar()}' ({X:F1},{Y:F1}) {Width:F1}x{Height:F1}";
    }
}
