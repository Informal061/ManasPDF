# ManasPDF .NET API Reference

This document covers the .NET wrapper (`ManasPDF`) and WPF viewer control (`ManasPDF.Wpf`) packages.

## Installation

```
dotnet add package ManasPDF --prerelease
```

For WPF applications with a ready-to-use viewer control:

```
dotnet add package ManasPDF.Wpf --prerelease
```

> `ManasPDF.Wpf` automatically includes the `ManasPDF` core package as a dependency. Use `--prerelease` for alpha versions.

---

## ManasPDF (Core Library)

### Quick Start

```csharp
using ManasPDF;

// Open a PDF document
using var doc = PdfDocument.Open("sample.pdf");
Console.WriteLine($"Pages: {doc.PageCount}");

// Render a page (GPU-accelerated)
var page = doc.GetPage(0);
byte[]? pixels = page.Render(1.5, out int width, out int height);
// pixels is BGRA32 format, ready for display

// CPU-only rendering (no GPU required)
byte[]? cpuPixels = page.RenderCpu(1.0, out int w, out int h);

// Extract text
string text = page.ExtractText();

// Get text with position data
PdfTextGlyph[] glyphs = page.ExtractGlyphs();

// Get hyperlinks
PdfLink[] links = page.GetLinks();
```

### Password-Protected PDFs

```csharp
using var doc = PdfDocument.Open("protected.pdf");

if (doc.NeedsCredentials && doc.IsPasswordProtected)
{
    bool ok = doc.TryPassword("secret");
}
```

### Certificate-Encrypted PDFs

```csharp
using var doc = PdfDocument.Open("cert-encrypted.pdf");

if (doc.NeedsCredentials && doc.IsCertificateEncrypted)
{
    int recipientCount = doc.CertificateRecipientCount;
    byte[]? encryptedKey = doc.GetRecipientEncryptedKey(0);
    // Decrypt with your private key, then supply the seed
    doc.SupplyCertificateSeed(decryptedSeed);
}
```

### Multi-Tab / Multi-Document Optimization

```csharp
// When switching tabs, set the active document
// Inactive documents skip rendering for performance
doc.SetAsActive();

// Enable/disable the filter
PdfDocument.EnableActiveDocumentFilter(true);

// Clear caches when memory is tight
doc.ClearCache();             // This document only
PdfDocument.ClearAllCaches(); // All documents
```

---

## API Reference: ManasPDF

### PdfDocument

```csharp
public sealed class PdfDocument : IDisposable
```

**Static Methods:**

| Method | Returns | Description |
|--------|---------|-------------|
| `Open(string path)` | `PdfDocument` | Opens a PDF document from file path. Throws `PdfException` on failure. |
| `ClearAllCaches()` | `void` | Clears all render caches globally across all documents |
| `EnableActiveDocumentFilter(bool)` | `void` | Enables/disables active document filter for multi-tab optimization |

**Static Properties:**

| Property | Type | Description |
|----------|------|-------------|
| `NativeVersion` | `int` | Native library (ManasPDFCore.dll) version |

**Instance Properties:**

| Property | Type | Description |
|----------|------|-------------|
| `PageCount` | `int` | Number of pages in the document |
| `NeedsCredentials` | `bool` | True if document requires password or certificate to decrypt |
| `IsPasswordProtected` | `bool` | True if document is password-protected (/Standard encryption) |
| `IsCertificateEncrypted` | `bool` | True if document is certificate-encrypted (/Adobe.PubSec) |
| `EncryptionStatus` | `int` | `0` = not encrypted, `1` = encrypted and ready, `-1` = needs credentials |
| `EncryptionType` | `int` | `0` = none, `1` = password, `2` = certificate |
| `CertificateRecipientCount` | `int` | Number of certificate recipients |
| `Handle` | `IntPtr` | Native document handle for advanced interop |

**Instance Methods:**

| Method | Returns | Description |
|--------|---------|-------------|
| `GetPage(int index)` | `PdfPage` | Gets a page by 0-based index |
| `TryPassword(string password)` | `bool` | Attempts to decrypt with a password |
| `SupplyCertificateSeed(byte[] seed)` | `bool` | Supplies decrypted certificate seed |
| `GetRecipientEncryptedKey(int index)` | `byte[]?` | Gets encrypted key for a certificate recipient |
| `GetRecipientIssuerDer(int index)` | `byte[]?` | Gets issuer DER data for recipient matching |
| `GetRecipientSerial(int index)` | `byte[]?` | Gets serial number for a certificate recipient |
| `ClearCache()` | `void` | Clears render cache for this document |
| `ClearAllTextCache()` | `void` | Clears all text extraction caches |
| `SetAsActive()` | `void` | Sets as active document (multi-tab optimization) |
| `Dispose()` | `void` | Releases native resources |

---

### PdfPage

```csharp
public class PdfPage
```

**Properties:**

| Property | Type | Description |
|----------|------|-------------|
| `Index` | `int` | 0-based page index |
| `Width` | `double` | Page width in PDF points (1 point = 1/72 inch) |
| `Height` | `double` | Page height in PDF points |
| `Rotation` | `int` | Page rotation in degrees (0, 90, 180, 270) |

**Methods:**

| Method | Returns | Description |
|--------|---------|-------------|
| `Render(double zoom, out int width, out int height)` | `byte[]?` | GPU-accelerated render to BGRA32 pixel buffer |
| `RenderCpu(double zoom, out int width, out int height)` | `byte[]?` | CPU software render to BGRA32 pixel buffer |
| `ExtractGlyphs()` | `PdfTextGlyph[]` | Extracts text glyphs with position data |
| `ExtractText()` | `string` | Extracts text content as a string |
| `GetLinks()` | `PdfLink[]` | Gets all hyperlinks on this page |
| `ClearTextCache()` | `void` | Clears cached text extraction data |

**Render output:**
- Returns `null` on failure, otherwise a `byte[]` in **BGRA32** format (4 bytes per pixel)
- `width` and `height` are the output bitmap dimensions in pixels
- Pixel coordinates: `bitmap_pixel = pdf_point * zoom * (96.0 / 72.0)`

---

### PdfTextGlyph

```csharp
public struct PdfTextGlyph  // 24 bytes, StructLayout Sequential Pack=1
```

| Field | Type | Description |
|-------|------|-------------|
| `Unicode` | `uint` | Unicode code point of the character |
| `X` | `float` | X position in bitmap pixels (at zoom=1, 96 DPI) |
| `Y` | `float` | Y position in bitmap pixels (at zoom=1, top-left origin) |
| `Width` | `float` | Glyph advance width in pixels |
| `Height` | `float` | Glyph height in pixels |
| `FontSize` | `float` | Effective font size in pixels |

| Member | Type | Description |
|--------|------|-------------|
| `IsSpace` | `bool` (property) | True if the glyph is a space character (U+0020 or U+00A0) |
| `ToChar()` | `char` (method) | Converts Unicode code point to char (returns U+FFFD for surrogates) |

**Coordinate system:** Positions are in bitmap-space at zoom=1. To convert to screen coordinates at a given zoom level: `screen_x = glyph.X * zoom`.

---

### PdfLink

```csharp
public class PdfLink
```

| Property | Type | Description |
|----------|------|-------------|
| `X1` | `double` | Bounding box left (PDF points) |
| `Y1` | `double` | Bounding box top (PDF points) |
| `X2` | `double` | Bounding box right (PDF points) |
| `Y2` | `double` | Bounding box bottom (PDF points) |
| `Uri` | `string?` | External URI (http://, mailto:, etc.), or null for internal links |
| `DestPage` | `int` | Internal page destination (0-based), or -1 for external links |
| `IsExternal` | `bool` | True if this is an external link (has URI) |
| `IsInternal` | `bool` | True if this is an internal page link (DestPage >= 0) |

---

### PdfException

```csharp
public class PdfException : Exception
```

Thrown when a PDF operation fails (file not found, corrupt PDF, etc.).

| Constructor | Description |
|-------------|-------------|
| `PdfException(string message)` | Creates exception with message |
| `PdfException(string message, Exception inner)` | Creates exception with message and inner exception |

---

## ManasPDF.Wpf (Viewer Control)

### Basic Usage

**XAML:**
```xml
<Window x:Class="MyApp.MainWindow"
        xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        xmlns:manaspdf="clr-namespace:ManasPDF.Wpf;assembly=ManasPDF.Wpf"
        Title="PDF Viewer" Height="700" Width="1000">
    <Grid>
        <manaspdf:PdfViewer x:Name="Viewer" />
    </Grid>
</Window>
```

**Code-behind:**
```csharp
// Open via property (bindable in XAML)
Viewer.Source = @"C:\docs\sample.pdf";

// Or open via method
Viewer.OpenDocument(@"C:\docs\sample.pdf");
```

### Navigation

```csharp
Viewer.GoToPage(3);       // Go to page 4 (0-based)
Viewer.NextPage();
Viewer.PreviousPage();
```

### Zoom & Rotate

```csharp
Viewer.ZoomLevel = 1.5;   // 150%
Viewer.ZoomIn();           // Next preset level
Viewer.ZoomOut();          // Previous preset level
Viewer.RotatePages();      // Rotate all pages 90° clockwise
```

Preset zoom levels: 25%, 50%, 75%, 100%, 125%, 150%, 200%, 300%, 400%.

### Text Search

```csharp
Viewer.ShowSearch();           // Open search bar (Ctrl+F)
Viewer.Search("keyword");     // Search and highlight matches
Viewer.HideSearch();           // Close search bar and clear highlights
```

Search is case-insensitive and highlights all matches across all pages. Navigate between matches with Enter / Shift+Enter in the search bar.

### Text Selection & Copy

```csharp
string selected = Viewer.GetSelectedText();
```

Drag with the mouse to select text. The cursor changes to IBeam over selectable text, and Hand over hyperlinks. Ctrl+C copies the selection to clipboard. Multi-page selection is supported.

### Print

```csharp
Viewer.PrintDocument();    // Opens print preview window (Ctrl+P)
```

The print feature opens a custom print preview window with:
- Page-by-page preview with navigation
- Printer selection dropdown (all system printers)
- High-quality 2x rendering for crisp output
- XPS-based printing pipeline (no PrintDialog conflicts)

### Save As

```csharp
Viewer.SaveDocumentAs();   // Opens Save As dialog (Ctrl+S)
```

### Events

```csharp
// Document loaded successfully
Viewer.DocumentOpened += (s, e) =>
{
    Console.WriteLine($"Opened: {e.FilePath} ({e.PageCount} pages)");
};

// Document failed to load
Viewer.DocumentError += (s, e) =>
{
    Console.WriteLine($"Error: {e.Exception.Message}");
};

// Page changed (via scroll or navigation)
Viewer.PageChanged += (s, pageIndex) =>
{
    Console.WriteLine($"Page: {pageIndex + 1}");
};

// Zoom changed
Viewer.ZoomChanged += (s, zoom) =>
{
    Console.WriteLine($"Zoom: {zoom:P0}");
};

// Text selected
Viewer.SelectionChanged += (s, text) =>
{
    Console.WriteLine($"Selected: {text}");
};

// Hyperlink clicked (set Handled=true to override default behavior)
Viewer.LinkClicked += (s, e) =>
{
    if (e.Link.IsExternal)
        Console.WriteLine($"External link: {e.Link.Uri}");
    else
        Console.WriteLine($"Internal link: page {e.Link.DestPage + 1}");
    // e.Handled = true; // Prevent default navigation
};
```

### Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| `Ctrl+F` | Open search bar |
| `Ctrl+P` | Print |
| `Ctrl+S` | Save As |
| `Ctrl+C` | Copy selected text |
| `Ctrl+Mouse Wheel` | Zoom in/out |
| `Enter` | Next search match (in search bar) |
| `Shift+Enter` | Previous search match (in search bar) |
| `Escape` | Close search bar |

---

## API Reference: ManasPDF.Wpf

### PdfViewer

```csharp
[ToolboxItem(true)]
public partial class PdfViewer : UserControl
```

**Dependency Properties (XAML-bindable):**

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `Source` | `string?` | `null` | PDF file path. Setting this opens the document. |
| `ShowToolbar` | `bool` | `true` | Show/hide the built-in toolbar |

**Read-only Properties:**

| Property | Type | Description |
|----------|------|-------------|
| `ZoomLevel` | `double` | Current zoom level (1.0 = 100%) |
| `CurrentPage` | `int` | Current page index (0-based) |
| `PageCount` | `int` | Total page count |
| `Document` | `PdfDocument?` | Underlying document instance |

**Methods:**

| Method | Returns | Description |
|--------|---------|-------------|
| `OpenDocument(string path)` | `void` | Opens a PDF from file path |
| `CloseDocument()` | `void` | Closes the current document |
| `GoToPage(int index)` | `void` | Navigates to page (0-based) |
| `NextPage()` | `void` | Go to next page |
| `PreviousPage()` | `void` | Go to previous page |
| `ZoomIn()` | `void` | Zoom in one preset level |
| `ZoomOut()` | `void` | Zoom out one preset level |
| `RotatePages()` | `void` | Rotate all pages 90° clockwise |
| `ShowSearch()` | `void` | Open the search bar |
| `HideSearch()` | `void` | Close search bar and clear highlights |
| `Search(string query)` | `void` | Search text and highlight matches |
| `GetSelectedText()` | `string` | Get mouse-selected text |
| `PrintDocument()` | `void` | Open print preview window |
| `SaveDocumentAs()` | `void` | Open Save As dialog |

**Events:**

| Event | Argument Type | Description |
|-------|---------------|-------------|
| `DocumentOpened` | `PdfDocumentEventArgs` | Document loaded successfully |
| `DocumentError` | `PdfErrorEventArgs` | Document failed to load |
| `PageChanged` | `int` | Current page changed (page index) |
| `ZoomChanged` | `double` | Zoom level changed |
| `SelectionChanged` | `string` | Text selection completed |
| `LinkClicked` | `PdfLinkClickEventArgs` | Hyperlink clicked |

---

### Event Args Classes

#### PdfDocumentEventArgs

| Property | Type | Description |
|----------|------|-------------|
| `FilePath` | `string` | Path of the opened PDF file |
| `PageCount` | `int` | Number of pages in the document |

#### PdfErrorEventArgs

| Property | Type | Description |
|----------|------|-------------|
| `FilePath` | `string` | Path that failed to open |
| `Exception` | `Exception` | The error that occurred |

#### PdfLinkClickEventArgs

| Property | Type | Description |
|----------|------|-------------|
| `Link` | `PdfLink` | The link that was clicked |
| `Handled` | `bool` | Set to `true` to prevent default navigation |

---

## Complete Example

A minimal WPF application using ManasPDF.Wpf:

**MainWindow.xaml:**
```xml
<Window x:Class="MyPdfApp.MainWindow"
        xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        xmlns:manaspdf="clr-namespace:ManasPDF.Wpf;assembly=ManasPDF.Wpf"
        Title="My PDF Viewer" Height="700" Width="1000">
    <Grid>
        <Grid.RowDefinitions>
            <RowDefinition Height="Auto" />
            <RowDefinition Height="*" />
        </Grid.RowDefinitions>

        <StackPanel Grid.Row="0" Orientation="Horizontal" Margin="8">
            <Button Content="Open PDF..." Click="OpenFile_Click" Padding="12,6" />
            <TextBlock x:Name="FileNameText" VerticalAlignment="Center" Margin="12,0" />
        </StackPanel>

        <manaspdf:PdfViewer Grid.Row="1" x:Name="Viewer" />
    </Grid>
</Window>
```

**MainWindow.xaml.cs:**
```csharp
using System.Windows;
using Microsoft.Win32;

namespace MyPdfApp;

public partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();

        Viewer.DocumentOpened += (s, e) =>
        {
            Title = $"My PDF Viewer - {System.IO.Path.GetFileName(e.FilePath)}";
            FileNameText.Text = $"{e.FilePath} ({e.PageCount} pages)";
        };

        Viewer.DocumentError += (s, e) =>
        {
            MessageBox.Show($"Error: {e.Exception.Message}", "Error",
                MessageBoxButton.OK, MessageBoxImage.Error);
        };
    }

    private void OpenFile_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog
        {
            Filter = "PDF Files (*.pdf)|*.pdf|All Files (*.*)|*.*",
            Title = "Open PDF File"
        };

        if (dialog.ShowDialog() == true)
        {
            Viewer.OpenDocument(dialog.FileName);
        }
    }
}
```

**Project file (.csproj):**
```xml
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>WinExe</OutputType>
    <TargetFramework>net8.0-windows</TargetFramework>
    <UseWPF>true</UseWPF>
  </PropertyGroup>
  <ItemGroup>
    <PackageReference Include="ManasPDF.Wpf" Version="0.1.0-alpha" />
  </ItemGroup>
</Project>
```
