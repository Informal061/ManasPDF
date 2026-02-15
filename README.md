# ManasPDF

[![Version](https://img.shields.io/badge/version-0.1.0--alpha-orange)]()
[![Status](https://img.shields.io/badge/status-in%20development-yellow)]()
[![License](https://img.shields.io/badge/license-Apache%202.0-blue)]()

A high-performance, GPU-accelerated PDF rendering engine written in C++ with a .NET wrapper and WPF viewer control.

Developed by **[The Big Studio](https://www.thebigstudio.net)**

> **Alpha Release** - This project is in active development. APIs may change between versions. Not recommended for production use yet. Feedback and bug reports are welcome!

## Overview

ManasPDF is a from-scratch PDF rendering engine built on **Direct2D** for GPU-accelerated rendering with an automatic **CPU software renderer** fallback. The core engine is a native C++ DLL (`ManasPDFCore.dll`) that handles all PDF parsing, content stream interpretation, font rasterization, and pixel output. A .NET wrapper and WPF control are provided for easy integration into C# applications.

## Engine Features

### Rendering
- **Direct2D GPU renderer** - Hardware-accelerated path rasterization, gradient shading, image compositing
- **CPU software renderer** - Pure C++ scanline rasterizer with configurable SSAA (1x/2x/4x)
- **Automatic fallback** - GPU failure (device lost, remote desktop) falls back to CPU seamlessly
- **Render quality modes** - Fast (no SSAA), Standard (SSAA=1), Quality (SSAA=2)
- **BGRA32 output** - Raw pixel buffer, ready for display or further processing

### PDF Parsing
- Cross-reference table and stream parsing
- Object streams, indirect references, stream filters
- Page tree traversal with inherited attributes
- **90+ PDF operators** implemented (paths, text, color, shading, clipping, XObjects)

### Text & Fonts
- **FreeType** font rasterization (TrueType, CFF/PostScript, Type1)
- Type0/CID fonts with CID-to-GID mapping
- Custom encodings and ToUnicode CMap support
- Glyph-level text extraction with pixel coordinates

### Image Codecs
| Codec | Filter | Library |
|-------|--------|---------|
| JPEG | DCTDecode | libjpeg-turbo |
| JPEG2000 | JPXDecode | OpenJPEG (x64) |
| Flate/Deflate | FlateDecode | zlib |
| LZW | LZWDecode | Built-in |
| CCITT Fax | CCITTFaxDecode | Built-in (Group 3/4) |
| ASCII85 | ASCII85Decode | Built-in |
| RunLength | RunLengthDecode | Built-in |
| PNG Predictor | - | Built-in (Up/Sub/Avg/Paeth) |

Filter chains (multiple filters in sequence) are fully supported.

### Color Spaces
DeviceRGB, DeviceCMYK, DeviceGray, CalRGB, CalGray, ICCBased, Indexed, Separation, DeviceN, Lab

### Vector Graphics
- Paths: moveto, lineto, cubic bezier, rectangle, close
- Fill rules: even-odd, non-zero winding
- Stroke: line width, cap styles, join styles, miter limit, dash patterns
- Gradients: axial (linear) and radial with LUT-based evaluation + dithering
- Tiling patterns with arbitrary transforms
- Nested clipping paths (Form XObject clip layer stacking)

### Encryption
- **Password encryption** (/Standard handler)
  - RC4 40-128 bit (Revision 2-3)
  - AES-128 / AES-256 (Revision 5)
- **Certificate encryption** (/Adobe.PubSec)
  - PKCS#7 EnvelopedData parsing
  - RSA key transport with recipient enumeration
  - Full ASN.1 DER parser for certificate matching

### Performance & Caching
- **Page render cache** - Per-document, per-zoom LRU cache (500MB limit)
- **Glyph cache** - Rasterized glyphs by font hash + glyph ID + pixel size (128MB / 20K glyphs)
- **Font cache** - FT_Face objects cached by font program hash (100 fonts max)
- **Text batching** - BT/ET glyph atlas composition reduces draw calls (GPU)
- **Active document filter** - Multi-tab optimization, skip rendering for inactive documents
- **Zero-copy interop** - Cache results copied directly to output buffer

## Architecture

```
                    +---------------------------+
                    |      Your Application     |
                    +---------------------------+
                               |
                    +----------+----------+
                    |                     |
           +-------v-------+    +--------v--------+
           |   ManasPDF    |    |  ManasPDF.Wpf   |
           | (.NET Wrapper)|    | (WPF Control)   |
           |   NuGet pkg   |    |   NuGet pkg     |
           +-------+-------+    +-----------------+
                   |
            P/Invoke (C interop)
                   |
         +---------v-----------+
         |   ManasPDFCore.dll  |
         |   (C++ Engine)      |
         +-----+-------+------+
               |       |
      +--------v--+ +--v---------+
      |  Direct2D | |  Software  |
      |    GPU    | |    CPU     |
      |  Renderer | |  Renderer  |
      +-----------+ +------------+
               |       |
    +----------+-------+-----------+
    |  FreeType  |  zlib    |      |
    |  libjpeg   | OpenJPEG |      |
    +------------------------------+
```

### Rendering Pipeline

```
Pdf_RenderPageToRgba(doc, pageIndex, zoom)
  |
  +-> Check PageRenderCache (LRU, zero-copy)
  |     |-> Cache hit: copy to output buffer, return
  |     |-> Cache miss: continue
  |
  +-> Acquire render mutex
  +-> PdfPainterGPU::initialize(width, height)
  +-> PdfDocument::renderPageToPainter()
  |     |-> PdfContentParser::parse(contentStream)
  |           |-> Graphics state operators (q, Q, cm, w, J, ...)
  |           |-> Path operators (m, l, c, re, h, ...)
  |           |-> Paint operators (f, S, B, n, ...)
  |           |-> Text operators (BT, ET, Tf, Tm, Tj, TJ, ...)
  |           |-> Color operators (CS, SC, RG, K, G, ...)
  |           |-> XObject operators (Do -> Form/Image)
  |           |-> Clipping operators (W, W*)
  |
  +-> PdfPainterGPU::getBuffer() -> BGRA32 pixels
  +-> Store in PageRenderCache
  +-> Return to caller
```

### GPU Renderer (Direct2D)

- `ID2D1RenderTarget` + `IWICBitmap` per render instance
- Static D2D/WIC/DirectWrite factories shared across all instances
- **Glyph atlas batching**: glyphs collected during BT/ET blocks, rendered as a single atlas texture (threshold: 1000 glyphs, 4MB atlas)
- **Brush caching**: D2D solid color brushes cached by BGRA value
- **Clip layer stack**: nested Form XObjects use D2D clip layers
- Max bitmap: 16384x16384 (1GB RGBA)

### CPU Renderer (Software)

- Scanline polygon rasterization
- Configurable supersampling (SSAA 1x/2x/4x)
- Direct pixel blending for grayscale glyphs
- Gradient evaluation with 4096-sample LUT + dithering
- No GPU/DirectX dependency

## C++ API (DLL Exports)

The native library exports a flat C API for cross-language interop:

```c
// Document
PDF_API void*  Pdf_OpenDocument(const wchar_t* path);
PDF_API void   Pdf_CloseDocument(void* doc);
PDF_API int    Pdf_GetPageCount(void* doc);
PDF_API int    Pdf_GetPageSize(void* doc, int pageIndex, double* w, double* h);
PDF_API int    Pdf_GetPageRotate(void* doc, int pageIndex);

// Rendering (BGRA32 output)
PDF_API int    Pdf_RenderPageToRgba(void* doc, int pageIndex, double zoom,
                                     void* buffer, int bufferSize,
                                     int* outWidth, int* outHeight);
PDF_API int    Pdf_RenderPageToRgba_CPU(void* doc, ...);    // CPU-only
PDF_API int    Pdf_RenderPageToRgba_Fast(void* doc, ...);   // No SSAA
PDF_API int    Pdf_RenderPageToRgba_Quality(void* doc, ...); // SSAA=2

// Text extraction
PDF_API int    Pdf_ExtractPageText(void* doc, int pageIndex);
PDF_API int    Pdf_GetTextGlyphCount(void* doc, int pageIndex);
PDF_API int    Pdf_GetExtractedGlyphs(void* doc, int pageIndex,
                                       PdfTextGlyphExport* out, int maxCount);
PDF_API int    Pdf_GetExtractedTextUtf8(void* doc, int pageIndex,
                                         char* outBuffer, int maxLen);

// Links
PDF_API int    Pdf_GetPageLinkCount(void* doc, int pageIndex);
PDF_API int    Pdf_GetPageLinks(void* doc, int pageIndex, ...);

// Encryption
PDF_API int    Pdf_GetEncryptionStatus(void* doc);   // 0=none, 1=ready, -1=locked
PDF_API int    Pdf_GetEncryptionType(void* doc);     // 0=none, 1=password, 2=cert
PDF_API int    Pdf_TryPassword(void* doc, const char* password);
PDF_API int    Pdf_SupplyCertSeed(void* doc, const uint8_t* seed, int seedLen);
PDF_API int    Pdf_GetCertRecipientCount(void* doc);
PDF_API int    Pdf_GetCertRecipientEncryptedKey(void* doc, int idx, ...);

// Cache management
PDF_API void   Pdf_ClearDocumentCache(void* doc);
PDF_API void   Pdf_ClearAllCache();
PDF_API void   Pdf_SetActiveDocument(void* doc);
```

### Glyph Data Structure (24 bytes, packed)

```c
struct PdfTextGlyphExport {
    uint32_t unicode;     // Unicode code point
    float    x, y;        // Position in bitmap pixels (at zoom=1, 96 DPI)
    float    width;       // Glyph advance width
    float    height;      // Glyph height
    float    fontSize;    // Effective font size in pixels
};
```

## .NET Integration

NuGet packages provide a managed wrapper and a ready-to-use WPF viewer control.

### Packages

| Package | Description | Target |
|---------|-------------|--------|
| **ManasPDF** | .NET wrapper over the native engine | `net8.0` |
| **ManasPDF.Wpf** | Drop-in WPF PdfViewer control with toolbar | `net8.0-windows` |

### Installation

```
dotnet add package ManasPDF.Wpf --prerelease
```

> `ManasPDF.Wpf` automatically includes the `ManasPDF` core package. Use `--prerelease` for alpha versions.

### Quick Start

```csharp
using ManasPDF;

using var doc = PdfDocument.Open("sample.pdf");
var page = doc.GetPage(0);

// GPU-accelerated render -> BGRA32 pixels
byte[]? pixels = page.Render(1.5, out int width, out int height);

// Text extraction
string text = page.ExtractText();
PdfTextGlyph[] glyphs = page.ExtractGlyphs();

// Hyperlinks
PdfLink[] links = page.GetLinks();
```

### WPF Viewer Control

```xml
<Window xmlns:manaspdf="clr-namespace:ManasPDF.Wpf;assembly=ManasPDF.Wpf">
    <manaspdf:PdfViewer x:Name="Viewer" Source="C:\docs\sample.pdf" />
</Window>
```

Built-in features: zoom, page navigation, text search & highlight, text selection & copy, rotation, print preview with printer selection, save as, hyperlink click, keyboard shortcuts (Ctrl+F/P/S/C).

**Full .NET API documentation: [DOTNET-API.md](DOTNET-API.md)**

## Building from Source

### Prerequisites

- Visual Studio 2022 with C++ Desktop workload
- .NET 8.0 SDK
- CMake 3.20+

### Build the native engine

```bash
cd src/PDFCore
cmake -B build -A x64
cmake --build build --config Release
# Output: ManasPDFCore.dll
```

### Build the .NET wrapper

```bash
dotnet build src/ManasPDF -c Release
dotnet build src/ManasPDF.Wpf -c Release
```

### Create NuGet packages

Place native DLLs into the runtimes folder, then pack:

```
src/ManasPDF/runtimes/win-x64/native/
  ManasPDFCore.dll
  freetype.dll
  jpeg62.dll
  openjp2.dll
```

```bash
dotnet pack src/ManasPDF -c Release
dotnet pack src/ManasPDF.Wpf -c Release
```

## Project Structure

```
ManasPDF/
  src/
    PDFCore/                    # C++ native engine (ManasPDFCore.dll)
      PdfEngine.h               #   DLL export declarations
      PdfDocument.cpp/h          #   PDF parsing, encryption, font loading
      PdfContentParser.cpp/h     #   Content stream operator interpreter
      PdfPainterGPU.cpp/h        #   Direct2D GPU renderer
      PdfPainter.cpp/h           #   CPU software renderer
      PdfTextExtractor.cpp/h     #   Text extraction
      GlyphCache.cpp/h           #   Glyph bitmap cache
      PdfGradient.cpp/h          #   Gradient rendering
      PdfFilters.cpp/h           #   Stream decompression (7 codecs)
      IPdfPainter.h              #   Abstract renderer interface
      PdfGraphicsState.h         #   Graphics state model
      PdfPath.h                  #   Path representation
      PdfObject.h                #   PDF object model
      PdfParser.cpp/h            #   Object parser
      PdfLexer.cpp/h             #   Tokenizer
      CMakeLists.txt             #   Build configuration
    ManasPDF/                   # .NET wrapper (NuGet: ManasPDF)
      PdfDocument.cs             #   High-level API
      PdfPage.cs                 #   Page operations
      PdfTextGlyph.cs            #   Glyph struct (24-byte interop)
      PdfLinkExport.cs           #   Link classes
      PdfException.cs            #   Exception type
      Internal/NativeApi.cs      #   P/Invoke bindings
    ManasPDF.Wpf/               # WPF control (NuGet: ManasPDF.Wpf)
      PdfViewer.xaml              #   UserControl layout
      PdfViewer.xaml.cs           #   Control logic
  DOTNET-API.md                  # .NET API reference
  LICENSE                        # Apache 2.0
  THIRD-PARTY-NOTICES.txt        # Third-party licenses
  README.md
```

## Native Dependencies

| Library | File | Purpose | License |
|---------|------|---------|---------|
| [FreeType](https://freetype.org/) | `freetype.dll` | Font rasterization | FTL/GPLv2 |
| [libjpeg-turbo](https://libjpeg-turbo.org/) | `jpeg62.dll` | JPEG decoding | IJG/BSD |
| [OpenJPEG](https://www.openjpeg.org/) | `openjp2.dll` | JPEG2000 decoding (x64) | BSD-2 |
| [zlib](https://zlib.net/) | statically linked | Deflate compression | zlib |

> **Planned:** [HarfBuzz](https://harfbuzz.github.io/) integration for complex text shaping (Arabic, Devanagari, etc.) is planned for a future release.

See [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt) for full license texts.

## License

Apache License 2.0 - see [LICENSE](LICENSE) for details.

---

**ManasPDF** is a product of [The Big Studio](https://www.thebigstudio.net) | [Report an Issue](https://github.com/Informal061/ManasPDF/issues)
