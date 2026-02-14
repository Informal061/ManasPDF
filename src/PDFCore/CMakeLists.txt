cmake_minimum_required(VERSION 3.20)

# ==============================================================================
# PDFCore - Native PDF rendering library (Direct2D + FreeType)
# ==============================================================================

set(PDFCORE_SOURCES
    dllmain.cpp
    pch.cpp
    PdfCore.cpp
    PdfDocument.cpp
    PdfParser.cpp
    PdfLexer.cpp
    PdfContentParser.cpp
    PdfPainter.cpp
    PdfPainterGPU.cpp
    PdfPainterD2D.cpp
    PdfTextExtractor.cpp
    PdfFilters.cpp
    PdfGradient.cpp
    PdfDebug.cpp
    GlyphCache.cpp
    PdfBitmapCanvas.cpp
)

set(ZLIB_SOURCES
    zlib/adler32.c
    zlib/compress.c
    zlib/crc32.c
    zlib/deflate.c
    zlib/infback.c
    zlib/inffast.c
    zlib/inflate.c
    zlib/inftrees.c
    zlib/trees.c
    zlib/uncompr.c
    zlib/zutil.c
)

add_library(PDFCore SHARED ${PDFCORE_SOURCES} ${ZLIB_SOURCES})

# Output name: ManasPDFCore.dll
set_target_properties(PDFCore PROPERTIES
    OUTPUT_NAME "ManasPDFCore"
    PREFIX ""
)

# Preprocessor definitions
target_compile_definitions(PDFCore PRIVATE
    PDFCORE_EXPORTS
    _WINDOWS
    _USRDLL
    UNICODE
    _UNICODE
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CONFIG:Release>:NDEBUG>
)

# OpenJPEG only for x64
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_compile_definitions(PDFCore PRIVATE USE_OPENJPEG)
endif()

# Include directories
target_include_directories(PDFCore PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/zlib
    ${CMAKE_CURRENT_SOURCE_DIR}/jpeglib/include
    ${CMAKE_CURRENT_SOURCE_DIR}/Projects/include/freetype2
    ${CMAKE_CURRENT_SOURCE_DIR}/ProjectHarfBuzz/include/harfbuzz
    ${CMAKE_CURRENT_SOURCE_DIR}/openjpeg/include
)

# Third-party library directories
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(LIB_ARCH_SUFFIX "")  # x64 libs are in lib/ directly
else()
    set(LIB_ARCH_SUFFIX "")  # x86 libs - adjust if separate dirs exist
endif()

target_link_directories(PDFCore PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Projects/lib${LIB_ARCH_SUFFIX}
    ${CMAKE_CURRENT_SOURCE_DIR}/ProjectHarfBuzz/lib${LIB_ARCH_SUFFIX}
    ${CMAKE_CURRENT_SOURCE_DIR}/jpeglib/lib${LIB_ARCH_SUFFIX}
    ${CMAKE_CURRENT_SOURCE_DIR}/openjpeg/lib${LIB_ARCH_SUFFIX}
)

# Link third-party libraries
target_link_libraries(PDFCore PRIVATE
    freetype
    harfbuzz
    jpeg
    turbojpeg
)

# Link OpenJPEG only for x64
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_link_libraries(PDFCore PRIVATE openjp2)
endif()

# Windows system libraries (Direct2D, DirectWrite, WIC, etc.)
target_link_libraries(PDFCore PRIVATE
    d2d1
    dwrite
    d3d11
    dxgi
    windowscodecs
    crypt32
    ole32
)

# Precompiled header
target_precompile_headers(PDFCore PRIVATE pch.h)

# Install rules
install(TARGETS PDFCore
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(FILES
    PdfEngine.h
    IPdfPainter.h
    DESTINATION include/ManasPDF
)
