using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Printing;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Shapes;
using System.Windows.Threading;
using System.Runtime.InteropServices;
using System.Windows.Interop;
using System.Windows.Xps;
using System.Windows.Xps.Packaging;
using Microsoft.Win32;

namespace ManasPDF.Wpf
{
    /// <summary>
    /// A drop-in WPF PDF viewer control with zoom, scroll, page navigation,
    /// text search, text selection, rotation, print, save-as, and hyperlink support.
    /// </summary>
    /// <example>
    /// XAML:
    /// <code>
    /// &lt;manaspdf:PdfViewer x:Name="Viewer" Source="C:\docs\sample.pdf" /&gt;
    /// </code>
    /// Code-behind:
    /// <code>
    /// Viewer.Source = "C:\\docs\\sample.pdf";
    /// Viewer.ZoomLevel = 1.5;
    /// Viewer.GoToPage(3);
    /// </code>
    /// </example>
    [ToolboxItem(true)]
    [System.Windows.Markup.ContentProperty("Source")]
    public partial class PdfViewer : UserControl
    {
        private PdfDocument? _document;
        private string? _documentPath;
        private double _zoomLevel = 1.0;
        private double _dpiScale = 1.0;
        private int _currentPage = 0;
        private int _totalPages = 0;

        private readonly ObservableCollection<PageViewModel> _pages = new();
        private readonly DispatcherTimer _zoomDebounceTimer;
        private const int ZoomDebounceMs = 150;
        private bool _isZooming;

        // Zoom levels
        private static readonly double[] ZoomLevels =
            { 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0 };

        // Search state
        private readonly List<SearchMatch> _searchMatches = new();
        private int _currentMatchIndex = -1;
        private string _lastSearchText = "";
        private DispatcherTimer? _searchDebounceTimer;

        // Text selection state
        private bool _isSelecting;
        private int _selectionStartPage = -1;
        private int _selectionStartGlyph = -1;
        private int _selectionEndPage = -1;
        private int _selectionEndGlyph = -1;
        private readonly Dictionary<int, PdfTextGlyph[]> _glyphCache = new();
        private readonly Dictionary<int, PdfLink[]> _linkCache = new();

        public PdfViewer()
        {
            InitializeComponent();
            PdfPagesContainer.ItemsSource = _pages;

            _zoomDebounceTimer = new DispatcherTimer
            {
                Interval = TimeSpan.FromMilliseconds(ZoomDebounceMs)
            };
            _zoomDebounceTimer.Tick += ZoomDebounceTimer_Tick;

            _searchDebounceTimer = new DispatcherTimer
            {
                Interval = TimeSpan.FromMilliseconds(300)
            };
            _searchDebounceTimer.Tick += SearchDebounceTimer_Tick;

            UpdateZoomText();
            UpdatePageInfo();

            Loaded += (_, _) =>
            {
                var source = PresentationSource.FromVisual(this);
                if (source?.CompositionTarget != null)
                    _dpiScale = source.CompositionTarget.TransformToDevice.M11;
            };
        }

        #region Dependency Properties

        /// <summary>
        /// PDF file path to display. Set this to open a PDF.
        /// </summary>
        public static readonly DependencyProperty SourceProperty =
            DependencyProperty.Register(nameof(Source), typeof(string), typeof(PdfViewer),
                new PropertyMetadata(null, OnSourceChanged));

        public string? Source
        {
            get => (string?)GetValue(SourceProperty);
            set => SetValue(SourceProperty, value);
        }

        private static void OnSourceChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
        {
            if (d is PdfViewer viewer && e.NewValue is string path)
                viewer.OpenDocument(path);
        }

        /// <summary>
        /// Whether to show the built-in toolbar. Default: true.
        /// </summary>
        public static readonly DependencyProperty ShowToolbarProperty =
            DependencyProperty.Register(nameof(ShowToolbar), typeof(bool), typeof(PdfViewer),
                new PropertyMetadata(true, OnShowToolbarChanged));

        public bool ShowToolbar
        {
            get => (bool)GetValue(ShowToolbarProperty);
            set => SetValue(ShowToolbarProperty, value);
        }

        private static void OnShowToolbarChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
        {
            if (d is PdfViewer viewer)
                viewer.ToolbarPanel.Visibility = (bool)e.NewValue ? Visibility.Visible : Visibility.Collapsed;
        }

        #endregion

        #region Public API

        /// <summary>Current zoom level (1.0 = 100%).</summary>
        public double ZoomLevel
        {
            get => _zoomLevel;
            set
            {
                _zoomLevel = Math.Clamp(value, 0.1, 10.0);
                UpdateZoomText();
                RenderAllPagesAsync();
            }
        }

        /// <summary>Current page index (0-based).</summary>
        public int CurrentPage => _currentPage;

        /// <summary>Total page count.</summary>
        public int PageCount => _totalPages;

        /// <summary>The underlying PdfDocument, or null if not loaded.</summary>
        public PdfDocument? Document => _document;

        /// <summary>
        /// Opens a PDF document from a file path.
        /// </summary>
        public void OpenDocument(string path)
        {
            CloseDocument();

            try
            {
                _document = PdfDocument.Open(path);
                _documentPath = path;
                _totalPages = _document.PageCount;
                _currentPage = 0;

                UpdatePageInfo();
                RenderAllPagesAsync();

                DocumentOpened?.Invoke(this, new PdfDocumentEventArgs(path, _totalPages));
            }
            catch (Exception ex)
            {
                DocumentError?.Invoke(this, new PdfErrorEventArgs(path, ex));
            }
        }

        /// <summary>
        /// Closes the current document and releases resources.
        /// </summary>
        public void CloseDocument()
        {
            ClearSearch();
            ClearSelection();
            _pages.Clear();
            _glyphCache.Clear();
            _linkCache.Clear();
            _document?.Dispose();
            _document = null;
            _documentPath = null;
            _totalPages = 0;
            _currentPage = 0;
            UpdatePageInfo();
        }

        /// <summary>
        /// Navigates to a specific page (0-based index).
        /// </summary>
        public void GoToPage(int pageIndex)
        {
            if (_document == null || pageIndex < 0 || pageIndex >= _totalPages)
                return;

            _currentPage = pageIndex;
            UpdatePageInfo();
            ScrollToPage(pageIndex);
        }

        /// <summary>Navigate to the next page.</summary>
        public void NextPage()
        {
            if (_currentPage < _totalPages - 1)
                GoToPage(_currentPage + 1);
        }

        /// <summary>Navigate to the previous page.</summary>
        public void PreviousPage()
        {
            if (_currentPage > 0)
                GoToPage(_currentPage - 1);
        }

        /// <summary>Zoom in (1.25x multiplier, viewport-anchored).</summary>
        public void ZoomIn()
        {
            ZoomWithViewportAnchor(_zoomLevel * 1.25);
        }

        /// <summary>Zoom out (1/1.25 multiplier, viewport-anchored).</summary>
        public void ZoomOut()
        {
            ZoomWithViewportAnchor(_zoomLevel / 1.25);
        }

        /// <summary>
        /// Rotates all pages by 90 degrees clockwise.
        /// </summary>
        public void RotatePages()
        {
            if (_pages.Count == 0) return;
            foreach (var page in _pages)
                page.RotationAngle = (page.RotationAngle + 90) % 360;
        }

        /// <summary>
        /// Opens the search bar and focuses the search text box.
        /// </summary>
        public void ShowSearch()
        {
            SearchBarPanel.Visibility = Visibility.Visible;
            SearchTextBox.Focus();
            SearchTextBox.SelectAll();
        }

        /// <summary>
        /// Hides the search bar and clears highlights.
        /// </summary>
        public void HideSearch()
        {
            SearchBarPanel.Visibility = Visibility.Collapsed;
            ClearSearch();
        }

        /// <summary>
        /// Searches for text in the document and highlights all matches.
        /// </summary>
        /// <param name="query">The text to search for (case-insensitive).</param>
        public void Search(string query)
        {
            SearchTextBox.Text = query;
            ShowSearch();
            PerformSearch(query);
        }

        /// <summary>
        /// Gets the currently selected text, or empty string if no selection.
        /// </summary>
        public string GetSelectedText()
        {
            return BuildSelectedText();
        }

        /// <summary>
        /// Prints the current document using the system print dialog.
        /// </summary>
        public void PrintDocument()
        {
            DoPrint();
        }

        /// <summary>
        /// Shows a Save As dialog to save a copy of the current PDF.
        /// </summary>
        public void SaveDocumentAs()
        {
            DoSaveAs();
        }

        #endregion

        #region Events

        /// <summary>Raised when a document is successfully opened.</summary>
        public event EventHandler<PdfDocumentEventArgs>? DocumentOpened;

        /// <summary>Raised when a document fails to open.</summary>
        public event EventHandler<PdfErrorEventArgs>? DocumentError;

        /// <summary>Raised when the current page changes.</summary>
        public event EventHandler<int>? PageChanged;

        /// <summary>Raised when the zoom level changes.</summary>
        public event EventHandler<double>? ZoomChanged;

        /// <summary>Raised when a hyperlink is clicked. Set Handled=true to prevent default navigation.</summary>
        public event EventHandler<PdfLinkClickEventArgs>? LinkClicked;

        /// <summary>Raised when the text selection changes.</summary>
        public event EventHandler<string>? SelectionChanged;

        #endregion

        #region Rendering

        private async void RenderAllPagesAsync()
        {
            if (_document == null) return;

            LoadingOverlay.Visibility = Visibility.Visible;

            var doc = _document;
            int pageCount = _totalPages;
            double zoom = _zoomLevel;
            double dpi = _dpiScale;

            // Prepare page list
            while (_pages.Count < pageCount)
                _pages.Add(new PageViewModel());
            while (_pages.Count > pageCount)
                _pages.RemoveAt(_pages.Count - 1);

            // Render pages on background threads
            for (int i = 0; i < pageCount; i++)
            {
                int pageIndex = i;
                var bitmap = await Task.Run(() => RenderPageBitmap(doc, pageIndex, zoom, dpi));
                if (bitmap != null && _document == doc)
                {
                    var pvm = _pages[pageIndex];
                    pvm.PageImage = bitmap;
                    pvm.PageIndex = pageIndex;
                    pvm.OverlayWidth = bitmap.PixelWidth / dpi;
                    pvm.OverlayHeight = bitmap.PixelHeight / dpi;
                }
            }

            LoadingOverlay.Visibility = Visibility.Collapsed;
            ZoomChanged?.Invoke(this, _zoomLevel);

            // Rebuild overlays after render
            RefreshAllOverlays();
        }

        private static WriteableBitmap? RenderPageBitmap(PdfDocument doc, int pageIndex, double zoom, double dpiScale)
        {
            try
            {
                var page = doc.GetPage(pageIndex);
                double effectiveZoom = zoom * dpiScale;

                byte[]? pixels = page.Render(effectiveZoom, out int w, out int h);
                if (pixels == null || w <= 0 || h <= 0) return null;

                double bitmapDpi = 96.0 * dpiScale;
                var bmp = new WriteableBitmap(w, h, bitmapDpi, bitmapDpi,
                    PixelFormats.Bgra32, null);
                bmp.WritePixels(new Int32Rect(0, 0, w, h), pixels, w * 4, 0);
                bmp.Freeze(); // Make thread-safe
                return bmp;
            }
            catch
            {
                return null;
            }
        }

        #endregion

        #region Search

        private struct SearchMatch
        {
            public int PageIndex;
            public int GlyphStart;
            public int GlyphEnd; // exclusive
        }

        private void PerformSearch(string query)
        {
            ClearSearchHighlights();
            _searchMatches.Clear();
            _currentMatchIndex = -1;
            _lastSearchText = query;

            if (string.IsNullOrEmpty(query) || _document == null)
            {
                UpdateMatchCountText();
                return;
            }

            string lowerQuery = query.ToLowerInvariant();

            for (int p = 0; p < _totalPages; p++)
            {
                var glyphs = GetPageGlyphs(p);
                if (glyphs.Length == 0) continue;

                // Build text from glyphs
                var sb = new StringBuilder(glyphs.Length);
                foreach (var g in glyphs)
                    sb.Append(char.ToLowerInvariant(g.ToChar()));
                string pageText = sb.ToString();

                int searchFrom = 0;
                while (searchFrom < pageText.Length)
                {
                    int idx = pageText.IndexOf(lowerQuery, searchFrom, StringComparison.Ordinal);
                    if (idx < 0) break;

                    _searchMatches.Add(new SearchMatch
                    {
                        PageIndex = p,
                        GlyphStart = idx,
                        GlyphEnd = idx + lowerQuery.Length
                    });
                    searchFrom = idx + 1;
                }
            }

            // Navigate to first match on or after current page
            if (_searchMatches.Count > 0)
            {
                _currentMatchIndex = 0;
                for (int i = 0; i < _searchMatches.Count; i++)
                {
                    if (_searchMatches[i].PageIndex >= _currentPage)
                    {
                        _currentMatchIndex = i;
                        break;
                    }
                }
                GoToPage(_searchMatches[_currentMatchIndex].PageIndex);
            }

            UpdateMatchCountText();
            UpdateSearchHighlights();
        }

        private void NavigateMatch(int direction)
        {
            if (_searchMatches.Count == 0) return;

            _currentMatchIndex += direction;
            if (_currentMatchIndex >= _searchMatches.Count) _currentMatchIndex = 0;
            if (_currentMatchIndex < 0) _currentMatchIndex = _searchMatches.Count - 1;

            GoToPage(_searchMatches[_currentMatchIndex].PageIndex);
            UpdateSearchHighlights();
            UpdateMatchCountText();
        }

        private void UpdateSearchHighlights()
        {
            ClearSearchHighlights();

            for (int m = 0; m < _searchMatches.Count; m++)
            {
                var match = _searchMatches[m];
                bool isActive = (m == _currentMatchIndex);
                var glyphs = GetPageGlyphs(match.PageIndex);

                DrawGlyphHighlights(match.PageIndex, glyphs, match.GlyphStart, match.GlyphEnd,
                    isActive ? Brushes.Orange : Brushes.Yellow, 0.4, "SR");
            }
        }

        private void ClearSearchHighlights()
        {
            ClearOverlaysByTag("SR");
        }

        private void ClearSearch()
        {
            _searchMatches.Clear();
            _currentMatchIndex = -1;
            _lastSearchText = "";
            SearchTextBox.Text = "";
            ClearSearchHighlights();
            UpdateMatchCountText();
        }

        private void UpdateMatchCountText()
        {
            if (_searchMatches.Count == 0)
                MatchCountText.Text = "";
            else
                MatchCountText.Text = $"{_currentMatchIndex + 1}/{_searchMatches.Count}";
        }

        #endregion

        #region Text Selection

        private PdfTextGlyph[] GetPageGlyphs(int pageIndex)
        {
            if (_glyphCache.TryGetValue(pageIndex, out var cached))
                return cached;

            if (_document == null) return Array.Empty<PdfTextGlyph>();

            var page = _document.GetPage(pageIndex);
            var glyphs = page.ExtractGlyphs();
            _glyphCache[pageIndex] = glyphs;
            return glyphs;
        }

        private PdfLink[] GetPageLinks(int pageIndex)
        {
            if (_linkCache.TryGetValue(pageIndex, out var cached))
                return cached;

            if (_document == null) return Array.Empty<PdfLink>();

            var page = _document.GetPage(pageIndex);
            var links = page.GetLinks();
            _linkCache[pageIndex] = links;
            return links;
        }

        private int HitTestGlyph(int pageIndex, double x, double y, bool strict = false)
        {
            var glyphs = GetPageGlyphs(pageIndex);
            double zoom = _zoomLevel;

            // Convert canvas position to glyph coordinate space (zoom=1 pixel space)
            double gx = x / zoom;
            double gy = y / zoom;

            int bestIdx = -1;
            double bestDist = double.MaxValue;

            for (int i = 0; i < glyphs.Length; i++)
            {
                var g = glyphs[i];

                // Check if inside glyph bounds (with small padding)
                double pad = strict ? 0 : g.Height * 0.15;
                if (gx >= g.X - pad && gx <= g.X + g.Width + pad &&
                    gy >= g.Y - pad && gy <= g.Y + g.Height + pad)
                    return i;

                if (!strict)
                {
                    // For selection drag: find nearest glyph on same line
                    if (Math.Abs(gy - (g.Y + g.Height / 2.0)) < g.Height * 0.7)
                    {
                        double dist = Math.Abs(gx - (g.X + g.Width / 2.0));
                        if (dist < bestDist)
                        {
                            bestDist = dist;
                            bestIdx = i;
                        }
                    }
                }
            }

            // For drag selection: return nearest on same line within reasonable distance
            if (!strict && bestIdx >= 0 && bestDist < 200)
                return bestIdx;

            return -1;
        }

        private PdfLink? HitTestLink(int pageIndex, double x, double y)
        {
            var links = GetPageLinks(pageIndex);
            double zoom = _zoomLevel;
            double lx = x / zoom;
            double ly = y / zoom;

            foreach (var link in links)
            {
                if (lx >= link.X1 && lx <= link.X2 && ly >= link.Y1 && ly <= link.Y2)
                    return link;
            }
            return null;
        }

        private void UpdateSelectionHighlights()
        {
            ClearOverlaysByTag("TS");

            if (_selectionStartPage < 0 || _selectionStartGlyph < 0) return;

            int startPage = Math.Min(_selectionStartPage, _selectionEndPage);
            int endPage = Math.Max(_selectionStartPage, _selectionEndPage);

            for (int p = startPage; p <= endPage && p < _totalPages; p++)
            {
                var glyphs = GetPageGlyphs(p);
                if (glyphs.Length == 0) continue;

                int gs, ge;
                if (startPage == endPage)
                {
                    gs = Math.Min(_selectionStartGlyph, _selectionEndGlyph);
                    ge = Math.Max(_selectionStartGlyph, _selectionEndGlyph) + 1;
                }
                else if (p == startPage)
                {
                    gs = _selectionStartPage <= _selectionEndPage ? _selectionStartGlyph : 0;
                    ge = _selectionStartPage <= _selectionEndPage ? glyphs.Length : _selectionStartGlyph + 1;
                }
                else if (p == endPage)
                {
                    gs = _selectionStartPage <= _selectionEndPage ? 0 : _selectionEndGlyph;
                    ge = _selectionStartPage <= _selectionEndPage ? _selectionEndGlyph + 1 : glyphs.Length;
                }
                else
                {
                    gs = 0;
                    ge = glyphs.Length;
                }

                gs = Math.Clamp(gs, 0, glyphs.Length);
                ge = Math.Clamp(ge, 0, glyphs.Length);

                if (gs < ge)
                    DrawGlyphHighlights(p, glyphs, gs, ge, Brushes.CornflowerBlue, 0.35, "TS");
            }
        }

        private string BuildSelectedText()
        {
            if (_selectionStartPage < 0 || _selectionStartGlyph < 0) return "";

            int startPage = Math.Min(_selectionStartPage, _selectionEndPage);
            int endPage = Math.Max(_selectionStartPage, _selectionEndPage);
            var sb = new StringBuilder();

            for (int p = startPage; p <= endPage && p < _totalPages; p++)
            {
                var glyphs = GetPageGlyphs(p);
                if (glyphs.Length == 0) continue;

                int gs, ge;
                if (startPage == endPage)
                {
                    gs = Math.Min(_selectionStartGlyph, _selectionEndGlyph);
                    ge = Math.Max(_selectionStartGlyph, _selectionEndGlyph) + 1;
                }
                else if (p == startPage)
                {
                    gs = _selectionStartPage <= _selectionEndPage ? _selectionStartGlyph : 0;
                    ge = _selectionStartPage <= _selectionEndPage ? glyphs.Length : _selectionStartGlyph + 1;
                }
                else if (p == endPage)
                {
                    gs = _selectionStartPage <= _selectionEndPage ? 0 : _selectionEndGlyph;
                    ge = _selectionStartPage <= _selectionEndPage ? _selectionEndGlyph + 1 : glyphs.Length;
                }
                else
                {
                    gs = 0;
                    ge = glyphs.Length;
                }

                gs = Math.Clamp(gs, 0, glyphs.Length);
                ge = Math.Clamp(ge, 0, glyphs.Length);

                if (p > startPage && sb.Length > 0) sb.AppendLine();

                // Sort selected glyphs by visual position (Y then X)
                var selectedGlyphs = new List<PdfTextGlyph>();
                for (int i = gs; i < ge; i++)
                    selectedGlyphs.Add(glyphs[i]);

                if (selectedGlyphs.Count == 0) continue;

                selectedGlyphs.Sort((a, b) =>
                {
                    double yDiff = a.Y - b.Y;
                    if (Math.Abs(yDiff) > a.Height * 0.5) return yDiff < 0 ? -1 : 1;
                    return a.X.CompareTo(b.X);
                });

                double lastY = selectedGlyphs[0].Y;
                foreach (var g in selectedGlyphs)
                {
                    if (Math.Abs(g.Y - lastY) > g.Height * 0.5)
                        sb.AppendLine();
                    lastY = g.Y;
                    sb.Append(g.ToChar());
                }
            }

            return sb.ToString();
        }

        private void ClearSelection()
        {
            _selectionStartPage = -1;
            _selectionStartGlyph = -1;
            _selectionEndPage = -1;
            _selectionEndGlyph = -1;
            _isSelecting = false;
            ClearOverlaysByTag("TS");
        }

        #endregion

        #region Print

        private void DoPrint()
        {
            if (_document == null || _documentPath == null) return;

            try
            {
                var doc = _document;
                int totalPg = _totalPages;
                string filePath = _documentPath;
                double dpiScale = _dpiScale;

                // Get monitor work area (multi-monitor aware)
                double screenW, screenH;
                var parentWindow = Window.GetWindow(this);
                try
                {
                    var hwnd = new WindowInteropHelper(parentWindow).Handle;
                    var monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                    var mi = new MONITORINFO();
                    mi.cbSize = Marshal.SizeOf(mi);
                    GetMonitorInfo(monitor, ref mi);

                    var source = PresentationSource.FromVisual(this);
                    double dpiX = source?.CompositionTarget?.TransformToDevice.M11 ?? 1.0;
                    double dpiY = source?.CompositionTarget?.TransformToDevice.M22 ?? 1.0;
                    screenW = (mi.rcWork.Right - mi.rcWork.Left) / dpiX;
                    screenH = (mi.rcWork.Bottom - mi.rcWork.Top) / dpiY;
                }
                catch
                {
                    screenW = SystemParameters.WorkArea.Width;
                    screenH = SystemParameters.WorkArea.Height;
                }

                // Render first page to calculate preview window size
                var firstPage = doc.GetPage(0);
                byte[]? firstPixels = firstPage.Render(1.0, out int firstW, out int firstH);
                double pdfW = firstPixels != null ? firstW / dpiScale : 600;
                double pdfH = firstPixels != null ? firstH / dpiScale : 800;

                double chromeH = 160;
                double paddingW = 80;
                double idealW = pdfW * 0.85 + paddingW;
                double idealH = pdfH * 0.85 + chromeH;

                if (idealW > screenW * 0.9 || idealH > screenH * 0.9)
                {
                    double s = Math.Min(screenW * 0.9 / idealW, screenH * 0.9 / idealH);
                    idealW *= s;
                    idealH *= s;
                }
                idealW = Math.Max(idealW, 500);
                idealH = Math.Max(idealH, 400);

                var previewWindow = new Window
                {
                    Title = $"Print Preview — {System.IO.Path.GetFileName(filePath)}",
                    Width = idealW,
                    Height = idealH,
                    MinWidth = 500,
                    MinHeight = 400,
                    WindowStartupLocation = WindowStartupLocation.CenterOwner,
                    Owner = parentWindow,
                    Background = new SolidColorBrush(Color.FromRgb(0xf0, 0xf0, 0xf0))
                };

                var mainGrid = new Grid();
                mainGrid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
                mainGrid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
                mainGrid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

                // === TOP TOOLBAR ===
                var toolbar = new Border
                {
                    Background = new SolidColorBrush(Color.FromRgb(0xe7, 0xed, 0xf3)),
                    BorderBrush = new SolidColorBrush(Color.FromRgb(0xcf, 0xdb, 0xe7)),
                    BorderThickness = new Thickness(0, 0, 0, 1),
                    Padding = new Thickness(12, 8, 12, 8)
                };

                var toolbarPanel = new DockPanel();

                // Page navigation (left)
                var navPanel = new StackPanel { Orientation = Orientation.Horizontal };
                var btnPrev = new Button { Content = "◀", Width = 30, Height = 30, FontSize = 12 };
                var pageText = new TextBlock
                {
                    Text = $"1 / {totalPg}",
                    VerticalAlignment = VerticalAlignment.Center,
                    Margin = new Thickness(10, 0, 10, 0),
                    FontWeight = FontWeights.SemiBold
                };
                var btnNext = new Button { Content = "▶", Width = 30, Height = 30, FontSize = 12 };
                navPanel.Children.Add(btnPrev);
                navPanel.Children.Add(pageText);
                navPanel.Children.Add(btnNext);
                DockPanel.SetDock(navPanel, Dock.Left);
                toolbarPanel.Children.Add(navPanel);

                // Printer selection (right)
                var printerPanel = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
                var printerCombo = new ComboBox { Width = 220, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 8, 0) };

                try
                {
                    var printServer = new LocalPrintServer();
                    var defaultName = "";
                    try { defaultName = LocalPrintServer.GetDefaultPrintQueue()?.FullName ?? ""; } catch { }

                    foreach (var q in printServer.GetPrintQueues())
                    {
                        int idx = printerCombo.Items.Add(q.FullName);
                        if (q.FullName == defaultName) printerCombo.SelectedIndex = idx;
                    }
                    if (printerCombo.SelectedIndex < 0 && printerCombo.Items.Count > 0)
                        printerCombo.SelectedIndex = 0;
                }
                catch { }

                printerPanel.Children.Add(printerCombo);
                DockPanel.SetDock(printerPanel, Dock.Right);
                toolbarPanel.Children.Add(printerPanel);

                toolbar.Child = toolbarPanel;
                Grid.SetRow(toolbar, 0);
                mainGrid.Children.Add(toolbar);

                // === CENTER: PREVIEW AREA ===
                var scrollViewer = new ScrollViewer
                {
                    HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
                    VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
                    Background = new SolidColorBrush(Color.FromRgb(0xd0, 0xd0, 0xd0)),
                    Padding = new Thickness(20)
                };

                var previewBorder = new Border
                {
                    Background = Brushes.White,
                    HorizontalAlignment = HorizontalAlignment.Center,
                    VerticalAlignment = VerticalAlignment.Center,
                    BorderBrush = new SolidColorBrush(Color.FromRgb(0x99, 0x99, 0x99)),
                    BorderThickness = new Thickness(1),
                    Effect = new System.Windows.Media.Effects.DropShadowEffect
                    {
                        Color = Colors.Black, Opacity = 0.2, BlurRadius = 12, ShadowDepth = 3
                    }
                };

                var previewImage = new Image { Stretch = Stretch.Uniform };
                RenderOptions.SetBitmapScalingMode(previewImage, BitmapScalingMode.HighQuality);
                previewBorder.Child = previewImage;
                scrollViewer.Content = previewBorder;
                Grid.SetRow(scrollViewer, 1);
                mainGrid.Children.Add(scrollViewer);

                // === BOTTOM: PRINT BUTTON ===
                var bottomBar = new Border
                {
                    Background = new SolidColorBrush(Color.FromRgb(0xe7, 0xed, 0xf3)),
                    BorderBrush = new SolidColorBrush(Color.FromRgb(0xcf, 0xdb, 0xe7)),
                    BorderThickness = new Thickness(0, 1, 0, 0),
                    Padding = new Thickness(12, 8, 12, 8)
                };

                var bottomPanel = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
                var btnCancel = new Button
                {
                    Content = "Cancel",
                    Width = 80,
                    Height = 32,
                    Margin = new Thickness(0, 0, 10, 0),
                    Cursor = Cursors.Hand
                };
                var btnPrint = new Button
                {
                    Content = "Print",
                    Width = 110,
                    Height = 32,
                    FontWeight = FontWeights.SemiBold,
                    Background = new SolidColorBrush(Color.FromRgb(0x13, 0x7f, 0xec)),
                    Foreground = Brushes.White,
                    BorderBrush = new SolidColorBrush(Color.FromRgb(0x13, 0x7f, 0xec)),
                    Cursor = Cursors.Hand
                };
                bottomPanel.Children.Add(btnCancel);
                bottomPanel.Children.Add(btnPrint);
                bottomBar.Child = bottomPanel;
                Grid.SetRow(bottomBar, 2);
                mainGrid.Children.Add(bottomBar);

                // === LOGIC ===
                int currentPreviewPage = 0;

                Action renderCurrentPage = null!;
                renderCurrentPage = () =>
                {
                    try
                    {
                        var page = doc.GetPage(currentPreviewPage);
                        byte[]? pixels = page.Render(1.0, out int w, out int h);
                        if (pixels != null && w > 0 && h > 0)
                        {
                            var bmp = new WriteableBitmap(w, h, 96 * dpiScale, 96 * dpiScale, PixelFormats.Bgra32, null);
                            bmp.WritePixels(new Int32Rect(0, 0, w, h), pixels, w * 4, 0);
                            bmp.Freeze();
                            previewImage.Source = bmp;
                            previewImage.Width = bmp.PixelWidth * 0.75 / dpiScale;
                            previewImage.Height = bmp.PixelHeight * 0.75 / dpiScale;
                        }
                    }
                    catch { }

                    pageText.Text = $"{currentPreviewPage + 1} / {totalPg}";
                    btnPrev.IsEnabled = currentPreviewPage > 0;
                    btnNext.IsEnabled = currentPreviewPage < totalPg - 1;
                };

                renderCurrentPage();

                btnPrev.Click += (s, a) => { if (currentPreviewPage > 0) { currentPreviewPage--; renderCurrentPage(); } };
                btnNext.Click += (s, a) => { if (currentPreviewPage < totalPg - 1) { currentPreviewPage++; renderCurrentPage(); } };
                btnCancel.Click += (s, a) => previewWindow.Close();

                btnPrint.Click += async (s, a) =>
                {
                    var printerName = printerCombo.SelectedItem as string;
                    if (string.IsNullOrEmpty(printerName))
                    {
                        MessageBox.Show("Please select a printer.", "Warning",
                            MessageBoxButton.OK, MessageBoxImage.Warning);
                        return;
                    }

                    btnPrint.IsEnabled = false;
                    btnCancel.IsEnabled = false;

                    try
                    {
                        var pd = new PrintDialog();
                        try
                        {
                            var server = new LocalPrintServer();
                            pd.PrintQueue = server.GetPrintQueue(printerName);
                        }
                        catch (Exception pex)
                        {
                            MessageBox.Show($"Cannot access printer: {pex.Message}", "Error",
                                MessageBoxButton.OK, MessageBoxImage.Error);
                            btnPrint.IsEnabled = true;
                            btnCancel.IsEnabled = true;
                            return;
                        }

                        double pageW = pd.PrintableAreaWidth;
                        double pageH = pd.PrintableAreaHeight;
                        var pageSize = new Size(pageW, pageH);

                        // Pre-render all pages at 2x zoom for high-quality printing (with progress)
                        var frozenBitmaps = new BitmapSource?[totalPg];
                        for (int i = 0; i < totalPg; i++)
                        {
                            btnPrint.Content = $"Page {i + 1}/{totalPg}...";
                            await Task.Delay(1); // Allow UI to update

                            try
                            {
                                var page = doc.GetPage(i);
                                byte[]? pixels = page.Render(2.0, out int w, out int h);
                                if (pixels != null && w > 0 && h > 0)
                                {
                                    var bmp = new WriteableBitmap(w, h, 96, 96, PixelFormats.Bgra32, null);
                                    bmp.WritePixels(new Int32Rect(0, 0, w, h), pixels, w * 4, 0);
                                    bmp.Freeze();
                                    frozenBitmaps[i] = bmp;
                                }
                            }
                            catch { }
                        }

                        btnPrint.Content = "Printing...";
                        await Task.Delay(1);

                        var paginator = new PreRenderedPaginator(frozenBitmaps, pageSize);
                        pd.PrintDocument(paginator, $"ManasPDF - {System.IO.Path.GetFileName(filePath)}");

                        previewWindow.Close();
                    }
                    catch (Exception ex2)
                    {
                        MessageBox.Show($"Print error: {ex2.Message}", "Error",
                            MessageBoxButton.OK, MessageBoxImage.Error);
                        btnPrint.IsEnabled = true;
                        btnCancel.IsEnabled = true;
                        btnPrint.Content = "Print";
                    }
                };

                previewWindow.Content = mainGrid;
                previewWindow.ShowDialog();
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Print error: {ex.Message}", "Print",
                    MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        #endregion

        #region Save As

        private void DoSaveAs()
        {
            if (_document == null || _documentPath == null) return;

            var dialog = new SaveFileDialog
            {
                Filter = "PDF Files (*.pdf)|*.pdf",
                FileName = System.IO.Path.GetFileName(_documentPath),
                DefaultExt = ".pdf"
            };

            if (dialog.ShowDialog() == true)
            {
                try
                {
                    File.Copy(_documentPath, dialog.FileName, true);
                }
                catch (Exception ex)
                {
                    MessageBox.Show($"Save error: {ex.Message}", "Save As", MessageBoxButton.OK, MessageBoxImage.Error);
                }
            }
        }

        #endregion

        #region Overlay Drawing

        private void DrawGlyphHighlights(int pageIndex, PdfTextGlyph[] glyphs, int start, int end,
            Brush fill, double opacity, string tag)
        {
            if (pageIndex < 0 || pageIndex >= _pages.Count) return;

            var canvas = GetOverlayCanvas(pageIndex);
            if (canvas == null) return;

            double zoom = _zoomLevel;

            for (int i = start; i < end && i < glyphs.Length; i++)
            {
                var g = glyphs[i];
                var rect = new Rectangle
                {
                    Width = Math.Max(g.Width * zoom, 1),
                    Height = Math.Max(g.Height * zoom, 1),
                    Fill = fill,
                    Opacity = opacity,
                    IsHitTestVisible = false,
                    Tag = tag
                };
                Canvas.SetLeft(rect, g.X * zoom);
                Canvas.SetTop(rect, g.Y * zoom);
                canvas.Children.Add(rect);
            }
        }

        private void DrawLinkOverlays(int pageIndex)
        {
            var links = GetPageLinks(pageIndex);
            if (links.Length == 0) return;

            var canvas = GetOverlayCanvas(pageIndex);
            if (canvas == null) return;

            double zoom = _zoomLevel;

            foreach (var link in links)
            {
                double w = (link.X2 - link.X1) * zoom;
                double h = (link.Y2 - link.Y1) * zoom;
                if (w <= 0 || h <= 0) continue;

                var rect = new Rectangle
                {
                    Width = w,
                    Height = h,
                    Fill = Brushes.Transparent,
                    Stroke = null,
                    Cursor = Cursors.Hand,
                    Tag = "LN",
                    ToolTip = link.Uri ?? $"Page {link.DestPage + 1}"
                };

                rect.MouseLeftButtonDown += (s, e) =>
                {
                    var args = new PdfLinkClickEventArgs(link);
                    LinkClicked?.Invoke(this, args);

                    if (!args.Handled)
                    {
                        if (link.DestPage >= 0)
                            GoToPage(link.DestPage);
                        else if (!string.IsNullOrEmpty(link.Uri))
                        {
                            try
                            {
                                System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo
                                {
                                    FileName = link.Uri,
                                    UseShellExecute = true
                                });
                            }
                            catch { }
                        }
                    }
                    e.Handled = true;
                };

                Canvas.SetLeft(rect, link.X1 * zoom);
                Canvas.SetTop(rect, link.Y1 * zoom);
                canvas.Children.Add(rect);
            }
        }

        private Canvas? GetOverlayCanvas(int pageIndex)
        {
            if (pageIndex < 0 || pageIndex >= _pages.Count) return null;

            var container = PdfPagesContainer.ItemContainerGenerator.ContainerFromIndex(pageIndex);
            if (container == null) return null;

            return FindVisualChild<Canvas>(container);
        }

        private static T? FindVisualChild<T>(DependencyObject parent) where T : DependencyObject
        {
            int count = VisualTreeHelper.GetChildrenCount(parent);
            for (int i = 0; i < count; i++)
            {
                var child = VisualTreeHelper.GetChild(parent, i);
                if (child is T result) return result;
                var found = FindVisualChild<T>(child);
                if (found != null) return found;
            }
            return null;
        }

        private void ClearOverlaysByTag(string tag)
        {
            for (int i = 0; i < _pages.Count; i++)
            {
                var canvas = GetOverlayCanvas(i);
                if (canvas == null) continue;

                for (int c = canvas.Children.Count - 1; c >= 0; c--)
                {
                    if (canvas.Children[c] is FrameworkElement fe && fe.Tag is string t && t == tag)
                        canvas.Children.RemoveAt(c);
                }
            }
        }

        private void RefreshAllOverlays()
        {
            // Defer to allow ItemContainerGenerator to create containers
            Dispatcher.BeginInvoke(DispatcherPriority.Loaded, () =>
            {
                for (int i = 0; i < _pages.Count; i++)
                    DrawLinkOverlays(i);

                if (_searchMatches.Count > 0)
                    UpdateSearchHighlights();

                if (_selectionStartPage >= 0)
                    UpdateSelectionHighlights();
            });
        }

        #endregion

        #region UI Event Handlers

        private void PrevPage_Click(object sender, RoutedEventArgs e) => PreviousPage();
        private void NextPage_Click(object sender, RoutedEventArgs e) => NextPage();
        private void ZoomIn_Click(object sender, RoutedEventArgs e) => ZoomIn();
        private void ZoomOut_Click(object sender, RoutedEventArgs e) => ZoomOut();
        private void Rotate_Click(object sender, RoutedEventArgs e) => RotatePages();
        private void Print_Click(object sender, RoutedEventArgs e) => DoPrint();
        private void SaveAs_Click(object sender, RoutedEventArgs e) => DoSaveAs();

        private void SearchButton_Click(object sender, RoutedEventArgs e)
        {
            if (SearchBarPanel.Visibility == Visibility.Visible)
                HideSearch();
            else
                ShowSearch();
        }

        private void CloseSearch_Click(object sender, RoutedEventArgs e) => HideSearch();
        private void NextMatch_Click(object sender, RoutedEventArgs e) => NavigateMatch(1);
        private void PrevMatch_Click(object sender, RoutedEventArgs e) => NavigateMatch(-1);

        // Command bindings
        private void FindCommand_Executed(object sender, ExecutedRoutedEventArgs e) => ShowSearch();
        private void PrintCommand_Executed(object sender, ExecutedRoutedEventArgs e) => DoPrint();
        private void SaveAsCommand_Executed(object sender, ExecutedRoutedEventArgs e) => DoSaveAs();
        private void CopyCommand_Executed(object sender, ExecutedRoutedEventArgs e)
        {
            var text = BuildSelectedText();
            if (!string.IsNullOrEmpty(text))
            {
                try { Clipboard.SetText(text); } catch { }
            }
        }

        private void PageNumberBox_KeyDown(object sender, KeyEventArgs e)
        {
            if (e.Key == Key.Enter && int.TryParse(PageNumberBox.Text, out int page))
            {
                GoToPage(page - 1); // 1-based input -> 0-based index
                e.Handled = true;
            }
        }

        private void SearchTextBox_TextChanged(object sender, TextChangedEventArgs e)
        {
            _searchDebounceTimer?.Stop();
            _searchDebounceTimer?.Start();
        }

        private void SearchDebounceTimer_Tick(object? sender, EventArgs e)
        {
            _searchDebounceTimer?.Stop();
            PerformSearch(SearchTextBox.Text);
        }

        private void SearchTextBox_KeyDown(object sender, KeyEventArgs e)
        {
            if (e.Key == Key.Enter)
            {
                if (Keyboard.Modifiers == ModifierKeys.Shift)
                    NavigateMatch(-1);
                else
                    NavigateMatch(1);
                e.Handled = true;
            }
            else if (e.Key == Key.Escape)
            {
                HideSearch();
                e.Handled = true;
            }
        }

        private void PdfScrollViewer_PreviewMouseWheel(object sender, MouseWheelEventArgs e)
        {
            if (Keyboard.Modifiers == ModifierKeys.Control)
            {
                e.Handled = true;

                // Mouse-centered zoom: keep the content point under the cursor at the same screen position
                var mousePos = e.GetPosition(PdfScrollViewer);
                double oldVOffset = PdfScrollViewer.VerticalOffset;
                double oldHOffset = PdfScrollViewer.HorizontalOffset;
                double oldZoom = _zoomLevel;

                double factor = e.Delta > 0 ? 1.15 : 1.0 / 1.15;
                SetZoomFast(_zoomLevel * factor);

                double scale = _zoomLevel / oldZoom;
                PdfScrollViewer.ScrollToVerticalOffset(Math.Max(0, (oldVOffset + mousePos.Y) * scale - mousePos.Y));
                PdfScrollViewer.ScrollToHorizontalOffset(Math.Max(0, (oldHOffset + mousePos.X) * scale - mousePos.X));
            }
        }

        private void PdfScrollViewer_ScrollChanged(object sender, ScrollChangedEventArgs e)
        {
            UpdateCurrentPageFromScroll();
        }

        private void ZoomDebounceTimer_Tick(object? sender, EventArgs e)
        {
            _zoomDebounceTimer.Stop();
            _isZooming = false;
            RenderAllPagesAsync();
        }

        /// <summary>
        /// Viewport-centered zoom for +/- toolbar buttons.
        /// Keeps the center of the viewport at the same content position.
        /// </summary>
        private void ZoomWithViewportAnchor(double newZoom)
        {
            double oldVOffset = PdfScrollViewer.VerticalOffset;
            double oldHOffset = PdfScrollViewer.HorizontalOffset;
            double oldZoom = _zoomLevel;

            SetZoomFast(newZoom);

            double scale = _zoomLevel / oldZoom;
            double centerY = PdfScrollViewer.ViewportHeight / 2;
            double centerX = PdfScrollViewer.ViewportWidth / 2;
            PdfScrollViewer.ScrollToVerticalOffset(Math.Max(0, (oldVOffset + centerY) * scale - centerY));
            PdfScrollViewer.ScrollToHorizontalOffset(Math.Max(0, (oldHOffset + centerX) * scale - centerX));
        }

        /// <summary>
        /// Fast zoom: instantly scales all existing page bitmaps using TransformedBitmap (O(1)),
        /// then schedules a debounced full-quality re-render.
        /// </summary>
        private void SetZoomFast(double newZoom)
        {
            newZoom = Math.Clamp(newZoom, 0.1, 10.0);
            if (Math.Abs(newZoom - _zoomLevel) < 0.001) return;

            double oldZoom = _zoomLevel;
            _zoomLevel = newZoom;
            _isZooming = true;

            UpdateZoomText();
            ZoomChanged?.Invoke(this, _zoomLevel);

            // Scale all existing page bitmaps instantly (TransformedBitmap is O(1), no pixel copy)
            UpdateAllPagesWithScaling(oldZoom, newZoom);

            // Restart debounce timer for full quality re-render
            _zoomDebounceTimer.Stop();
            _zoomDebounceTimer.Start();
        }

        /// <summary>
        /// Scales all page bitmaps using TransformedBitmap for instant visual feedback.
        /// All pages resize immediately on zoom — not just visible ones.
        /// </summary>
        private void UpdateAllPagesWithScaling(double oldZoom, double newZoom)
        {
            double scaleFactor = newZoom / oldZoom;

            for (int i = 0; i < _pages.Count; i++)
            {
                var pageItem = _pages[i];
                if (pageItem.PageImage is BitmapSource bmpSource)
                {
                    var scaled = ScaleBitmapDirect(bmpSource, scaleFactor);
                    if (scaled != null)
                    {
                        pageItem.PageImage = scaled;
                        pageItem.OverlayWidth = scaled.PixelWidth / _dpiScale;
                        pageItem.OverlayHeight = scaled.PixelHeight / _dpiScale;
                    }
                }
            }

            RefreshAllOverlays();
        }

        private static BitmapSource? ScaleBitmapDirect(BitmapSource? source, double scale)
        {
            if (source == null || Math.Abs(scale - 1.0) < 0.001) return source;

            var transformed = new TransformedBitmap(source, new ScaleTransform(scale, scale));
            transformed.Freeze();
            return transformed;
        }

        #endregion

        #region Mouse Handling for Text Selection

        /// <summary>
        /// Handles mouse button down for text selection and link clicks.
        /// </summary>
        protected override void OnPreviewMouseLeftButtonDown(MouseButtonEventArgs e)
        {
            base.OnPreviewMouseLeftButtonDown(e);

            if (_document == null) return;

            // Focus so keyboard shortcuts work
            Focus();

            // Find which page canvas was clicked
            var (pageIndex, pos) = HitTestPagePosition(e);
            if (pageIndex < 0) return;

            // Check for link click first
            var link = HitTestLink(pageIndex, pos.X, pos.Y);
            if (link != null)
                return; // Let the link rectangle handle it

            // Start text selection
            ClearSelection();
            int glyphIdx = HitTestGlyph(pageIndex, pos.X, pos.Y);
            if (glyphIdx >= 0)
            {
                _isSelecting = true;
                _selectionStartPage = pageIndex;
                _selectionStartGlyph = glyphIdx;
                _selectionEndPage = pageIndex;
                _selectionEndGlyph = glyphIdx;
                CaptureMouse();
            }
        }

        /// <summary>
        /// Handles mouse move for cursor changes and drag selection.
        /// </summary>
        protected override void OnPreviewMouseMove(MouseEventArgs e)
        {
            base.OnPreviewMouseMove(e);

            if (_document == null) return;

            // During selection drag
            if (_isSelecting)
            {
                var (pageIndex, pos) = HitTestPagePosition(e);
                if (pageIndex < 0) return;

                int glyphIdx = HitTestGlyph(pageIndex, pos.X, pos.Y);
                if (glyphIdx >= 0)
                {
                    _selectionEndPage = pageIndex;
                    _selectionEndGlyph = glyphIdx;
                    UpdateSelectionHighlights();
                }
                return;
            }

            // Not selecting: update cursor based on what's under mouse
            {
                var (pageIndex, pos) = HitTestPagePosition(e);
                if (pageIndex < 0)
                {
                    Cursor = Cursors.Arrow;
                    return;
                }

                // Check link first
                var link = HitTestLink(pageIndex, pos.X, pos.Y);
                if (link != null)
                {
                    Cursor = Cursors.Hand;
                    return;
                }

                // Check text (strict mode - only over actual glyphs)
                int glyphIdx = HitTestGlyph(pageIndex, pos.X, pos.Y, strict: true);
                if (glyphIdx >= 0)
                {
                    Cursor = Cursors.IBeam;
                    return;
                }

                Cursor = Cursors.Arrow;
            }
        }

        /// <summary>
        /// Handles mouse button up to finish text selection.
        /// </summary>
        protected override void OnPreviewMouseLeftButtonUp(MouseButtonEventArgs e)
        {
            base.OnPreviewMouseLeftButtonUp(e);

            if (_isSelecting)
            {
                _isSelecting = false;
                ReleaseMouseCapture();

                var text = BuildSelectedText();
                if (!string.IsNullOrEmpty(text))
                    SelectionChanged?.Invoke(this, text);
            }
        }

        /// <summary>
        /// Reset cursor when mouse leaves the control.
        /// </summary>
        protected override void OnMouseLeave(MouseEventArgs e)
        {
            base.OnMouseLeave(e);
            Cursor = Cursors.Arrow;
        }

        /// <summary>
        /// Hit-test to find which page overlay canvas the mouse is over and the position within it.
        /// Uses actual bounds checking instead of IsMouseOver for reliability in DataTemplate.
        /// </summary>
        private (int pageIndex, Point position) HitTestPagePosition(MouseEventArgs e)
        {
            for (int i = 0; i < _pages.Count; i++)
            {
                var canvas = GetOverlayCanvas(i);
                if (canvas == null) continue;

                // Use TransformToAncestor for reliable position calculation in DataTemplate
                try
                {
                    var pos = e.GetPosition(canvas);
                    if (pos.X >= 0 && pos.Y >= 0 &&
                        pos.X <= canvas.ActualWidth && pos.Y <= canvas.ActualHeight)
                    {
                        return (i, pos);
                    }
                }
                catch
                {
                    // canvas might not be connected to visual tree yet
                    continue;
                }
            }
            return (-1, default);
        }

        #endregion

        #region P/Invoke (Multi-Monitor)

        [DllImport("user32.dll")]
        private static extern IntPtr MonitorFromWindow(IntPtr hwnd, uint dwFlags);

        [DllImport("user32.dll")]
        private static extern bool GetMonitorInfo(IntPtr hMonitor, ref MONITORINFO lpmi);

        [StructLayout(LayoutKind.Sequential)]
        private struct RECT { public int Left, Top, Right, Bottom; }

        [StructLayout(LayoutKind.Sequential)]
        private struct MONITORINFO
        {
            public int cbSize;
            public RECT rcMonitor;
            public RECT rcWork;
            public uint dwFlags;
        }

        private const uint MONITOR_DEFAULTTONEAREST = 0x00000002;

        #endregion

        #region Helpers

        private void UpdatePageInfo()
        {
            PageNumberBox.Text = _totalPages > 0 ? (_currentPage + 1).ToString() : "";
            PageCountText.Text = _totalPages > 0 ? $"/ {_totalPages}" : "";
        }

        private void UpdateZoomText()
        {
            ZoomText.Text = $"{(int)(_zoomLevel * 100)}%";
        }

        private void ScrollToPage(int pageIndex)
        {
            if (PdfPagesContainer.ItemContainerGenerator
                    .ContainerFromIndex(pageIndex) is FrameworkElement container)
            {
                container.BringIntoView();
            }
        }

        private void UpdateCurrentPageFromScroll()
        {
            if (_totalPages == 0) return;

            double viewportTop = PdfScrollViewer.VerticalOffset;
            double viewportMid = viewportTop + PdfScrollViewer.ViewportHeight / 2;

            double cumHeight = 0;
            for (int i = 0; i < _pages.Count; i++)
            {
                if (PdfPagesContainer.ItemContainerGenerator
                        .ContainerFromIndex(i) is FrameworkElement el)
                {
                    cumHeight += el.ActualHeight;
                    if (cumHeight > viewportMid)
                    {
                        if (_currentPage != i)
                        {
                            _currentPage = i;
                            UpdatePageInfo();
                            PageChanged?.Invoke(this, _currentPage);
                        }
                        return;
                    }
                }
            }
        }

        #endregion
    }

    #region View Models & Event Args

    /// <summary>
    /// View model for a single rendered page.
    /// </summary>
    public class PageViewModel : INotifyPropertyChanged
    {
        private BitmapSource? _pageImage;
        private double _rotationAngle;
        private double _overlayWidth;
        private double _overlayHeight;
        private int _pageIndex;

        /// <summary>0-based page index.</summary>
        public int PageIndex
        {
            get => _pageIndex;
            set { _pageIndex = value; OnPropertyChanged(nameof(PageIndex)); }
        }

        /// <summary>Rendered page bitmap.</summary>
        public BitmapSource? PageImage
        {
            get => _pageImage;
            set { _pageImage = value; OnPropertyChanged(nameof(PageImage)); }
        }

        /// <summary>Rotation angle in degrees (0, 90, 180, 270).</summary>
        public double RotationAngle
        {
            get => _rotationAngle;
            set { _rotationAngle = value; OnPropertyChanged(nameof(RotationAngle)); }
        }

        /// <summary>Width of the overlay canvas (matches bitmap display size).</summary>
        public double OverlayWidth
        {
            get => _overlayWidth;
            set { _overlayWidth = value; OnPropertyChanged(nameof(OverlayWidth)); }
        }

        /// <summary>Height of the overlay canvas (matches bitmap display size).</summary>
        public double OverlayHeight
        {
            get => _overlayHeight;
            set { _overlayHeight = value; OnPropertyChanged(nameof(OverlayHeight)); }
        }

        public event PropertyChangedEventHandler? PropertyChanged;
        private void OnPropertyChanged(string name) =>
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
    }

    /// <summary>Event args for DocumentOpened event.</summary>
    public class PdfDocumentEventArgs : EventArgs
    {
        public string FilePath { get; }
        public int PageCount { get; }
        public PdfDocumentEventArgs(string filePath, int pageCount)
        {
            FilePath = filePath;
            PageCount = pageCount;
        }
    }

    /// <summary>Event args for DocumentError event.</summary>
    public class PdfErrorEventArgs : EventArgs
    {
        public string FilePath { get; }
        public Exception Exception { get; }
        public PdfErrorEventArgs(string filePath, Exception exception)
        {
            FilePath = filePath;
            Exception = exception;
        }
    }

    /// <summary>Event args for LinkClicked event.</summary>
    public class PdfLinkClickEventArgs : EventArgs
    {
        /// <summary>The link that was clicked.</summary>
        public PdfLink Link { get; }
        /// <summary>Set to true to prevent default navigation behavior.</summary>
        public bool Handled { get; set; }
        public PdfLinkClickEventArgs(PdfLink link)
        {
            Link = link;
        }
    }

    #endregion

    #region Print Support

    /// <summary>
    /// DocumentPaginator that uses pre-rendered bitmaps for high-quality, async-friendly printing.
    /// Pages are rendered beforehand at 2x zoom and stored as frozen BitmapSource objects.
    /// </summary>
    internal class PreRenderedPaginator : DocumentPaginator
    {
        private readonly BitmapSource?[] _bitmaps;
        private Size _pageSize;

        internal PreRenderedPaginator(BitmapSource?[] bitmaps, Size pageSize)
        {
            _bitmaps = bitmaps;
            _pageSize = pageSize;
        }

        public override bool IsPageCountValid => true;
        public override int PageCount => _bitmaps.Length;

        public override Size PageSize
        {
            get => _pageSize;
            set => _pageSize = value;
        }

        public override IDocumentPaginatorSource? Source => null;

        public override DocumentPage GetPage(int pageNumber)
        {
            if (pageNumber < 0 || pageNumber >= _bitmaps.Length || _bitmaps[pageNumber] == null)
                return DocumentPage.Missing;

            var bitmap = _bitmaps[pageNumber]!;

            double areaWidth = _pageSize.Width;
            double areaHeight = _pageSize.Height;

            double scaleX = areaWidth / bitmap.PixelWidth;
            double scaleY = areaHeight / bitmap.PixelHeight;
            double scale = Math.Min(scaleX, scaleY);

            double w = bitmap.PixelWidth * scale;
            double h = bitmap.PixelHeight * scale;
            double x = (areaWidth - w) / 2;
            double y = (areaHeight - h) / 2;

            var visual = new DrawingVisual();
            using (var dc = visual.RenderOpen())
            {
                dc.DrawImage(bitmap, new Rect(x, y, w, h));
            }

            return new DocumentPage(visual, _pageSize,
                new Rect(_pageSize), new Rect(_pageSize));
        }
    }

    #endregion
}
