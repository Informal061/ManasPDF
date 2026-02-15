#pragma once
// =====================================================
// PdfGradient.h - Kapsamli yeniden yazim
// Adobe kalitesinde gradient rendering icin
// =====================================================

#include <vector>
#include <memory>
#include <cstdint>
#include "PdfObject.h"

namespace pdf
{
    class PdfDocument;

    // =====================================================
    // GradientStop - Temel renk duragi
    // =====================================================
    struct GradientStop
    {
        double position;   // 0.0 - 1.0
        double rgb[3];     // RGB renk (0.0-1.0)
    };

    // =====================================================
    // PdfGradient - Ana gradient sinifi
    // =====================================================
    class PdfGradient
    {
    public:
        // =====================================================
        // GRADIENT GEOMETRY
        // =====================================================

        int type = 2;  // 2=Axial (linear), 3=Radial

        // Axial gradient koordinatlari
        double x0 = 0, y0 = 0, x1 = 1, y1 = 0;

        // Radial gradient icin
        double r0 = 0, r1 = 1;

        // =====================================================
        // COLOR DATA
        // =====================================================

        // Stop listesi (uyumluluk icin)
        std::vector<GradientStop> stops;

        // HIGH-RESOLUTION LUT (Type 0 icin)
        // 4096 sample ile 8-bit quantization hatasi minimize edilir
        static const int LUT_SIZE = 4096;
        bool hasLUT = false;
        std::vector<float> lutR;  // [LUT_SIZE] Red values
        std::vector<float> lutG;  // [LUT_SIZE] Green values
        std::vector<float> lutB;  // [LUT_SIZE] Blue values

        // =====================================================
        // COLOR EVALUATION
        // =====================================================

        // Ana renk hesaplama - LUT varsa LUT'tan, if absent stop'lardan
        void evaluateColor(double t, double outRgb[3]) const;

        // Dithering dahil (x,y koordinatina gore)
        void evaluateColorDithered(double t, int x, int y, uint8_t outRgb[3]) const;

        // =====================================================
        // PARSING
        // =====================================================

        static bool parseFunction(
            const std::shared_ptr<PdfObject>& funcObj,
            PdfDocument* doc,
            std::vector<GradientStop>& outStops);

        static bool parseFunctionWithColorSpace(
            const std::shared_ptr<PdfObject>& funcObj,
            PdfDocument* doc,
            std::vector<GradientStop>& outStops,
            int numComponents);

        // Dogrudan gradient'a parse et (LUT dahil)
        static bool parseFunctionToGradient(
            const std::shared_ptr<PdfObject>& funcObj,
            PdfDocument* doc,
            PdfGradient& outGradient,
            int numComponents);

        // YENI: DeviceN color space icin ozel parsing
        // deviceNNames: ["/Cyan", "/Magenta", "/Yellow", "/Black"] kombinasyonlari
        static bool parseFunctionToGradientDeviceN(
            const std::shared_ptr<PdfObject>& funcObj,
            PdfDocument* doc,
            PdfGradient& outGradient,
            const std::vector<std::string>& deviceNNames);

        // LUT olusturma (public - static fonksiyonlardan erisim icin)
        void buildLUTFromSamples(
            const std::vector<double>& samplesR,
            const std::vector<double>& samplesG,
            const std::vector<double>& samplesB);
    };

} // namespace pdf
