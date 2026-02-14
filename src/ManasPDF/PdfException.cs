using System;

namespace ManasPDF
{
    /// <summary>
    /// Exception thrown by ManasPDF operations.
    /// </summary>
    public class PdfException : Exception
    {
        public PdfException(string message) : base(message) { }
        public PdfException(string message, Exception inner) : base(message, inner) { }
    }
}
