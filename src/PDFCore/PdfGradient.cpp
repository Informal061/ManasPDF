// =====================================================
// PdfGradient.cpp - KAPSAMLI YENİDEN YAZIM
// Adobe kalitesinde gradient rendering için:
// 1. High-resolution LUT (4096 samples)
// 2. Cubic Hermite interpolation
// 3. Gamma-correct color blending
// 4. Blue noise dithering
// =====================================================

#include "pch.h"
#include "PdfGradient.h"
#include "PdfDocument.h"
#include "PdfDebug.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace pdf
{
    // =====================================================
    // GAMMA CORRECTION FUNCTIONS
    // sRGB <-> Linear RGB dönüşümü
    // Adobe tarzı render için kritik öneme sahip
    // =====================================================

    static inline double srgbToLinear(double srgb)
    {
        srgb = std::clamp(srgb, 0.0, 1.0);
        if (srgb <= 0.04045)
            return srgb / 12.92;
        else
            return std::pow((srgb + 0.055) / 1.055, 2.4);
    }

    static inline double linearToSrgb(double linear)
    {
        linear = std::clamp(linear, 0.0, 1.0);
        if (linear <= 0.0031308)
            return linear * 12.92;
        else
            return 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
    }

    // =====================================================
    // BLUE NOISE DITHERING
    // Bayer pattern'den çok daha doğal görünüm sağlar
    // 64x64 blue noise texture (16x16 tiled)
    // =====================================================

    static const float blueNoise16[16][16] = {
        {0.498f,0.827f,0.200f,0.953f,0.329f,0.702f,0.075f,0.580f,0.890f,0.267f,0.643f,0.439f,0.784f,0.114f,0.549f,0.361f},
        {0.141f,0.612f,0.376f,0.063f,0.549f,0.188f,0.878f,0.298f,0.471f,0.110f,0.831f,0.204f,0.918f,0.345f,0.729f,0.173f},
        {0.753f,0.016f,0.847f,0.439f,0.729f,0.400f,0.643f,0.024f,0.769f,0.565f,0.345f,0.596f,0.055f,0.502f,0.863f,0.439f},
        {0.286f,0.439f,0.569f,0.243f,0.918f,0.098f,0.502f,0.173f,0.925f,0.220f,0.710f,0.467f,0.275f,0.667f,0.220f,0.612f},
        {0.925f,0.173f,0.710f,0.129f,0.612f,0.314f,0.784f,0.408f,0.612f,0.055f,0.878f,0.129f,0.769f,0.400f,0.024f,0.761f},
        {0.063f,0.667f,0.329f,0.800f,0.031f,0.863f,0.235f,0.082f,0.337f,0.486f,0.259f,0.565f,0.008f,0.898f,0.337f,0.494f},
        {0.518f,0.878f,0.047f,0.494f,0.471f,0.588f,0.698f,0.565f,0.816f,0.165f,0.745f,0.384f,0.635f,0.188f,0.580f,0.110f},
        {0.235f,0.392f,0.612f,0.267f,0.729f,0.141f,0.408f,0.933f,0.024f,0.627f,0.039f,0.910f,0.259f,0.455f,0.831f,0.275f},
        {0.784f,0.157f,0.816f,0.953f,0.196f,0.933f,0.047f,0.275f,0.455f,0.384f,0.533f,0.173f,0.698f,0.071f,0.706f,0.961f},
        {0.008f,0.549f,0.447f,0.098f,0.376f,0.549f,0.322f,0.745f,0.196f,0.847f,0.290f,0.800f,0.471f,0.322f,0.439f,0.149f},
        {0.345f,0.698f,0.259f,0.643f,0.784f,0.259f,0.643f,0.118f,0.580f,0.071f,0.612f,0.031f,0.541f,0.863f,0.204f,0.612f},
        {0.910f,0.071f,0.863f,0.361f,0.016f,0.478f,0.863f,0.502f,0.933f,0.353f,0.439f,0.706f,0.196f,0.627f,0.024f,0.486f},
        {0.471f,0.533f,0.188f,0.525f,0.890f,0.141f,0.329f,0.024f,0.251f,0.753f,0.165f,0.878f,0.369f,0.098f,0.784f,0.322f},
        {0.165f,0.745f,0.039f,0.745f,0.298f,0.667f,0.729f,0.204f,0.659f,0.494f,0.055f,0.557f,0.243f,0.502f,0.369f,0.898f},
        {0.635f,0.298f,0.627f,0.110f,0.455f,0.008f,0.541f,0.400f,0.878f,0.110f,0.816f,0.337f,0.753f,0.910f,0.235f,0.078f},
        {0.847f,0.408f,0.890f,0.337f,0.816f,0.392f,0.165f,0.808f,0.016f,0.392f,0.267f,0.635f,0.078f,0.455f,0.588f,0.718f}
    };

    static inline float getBlueNoise(int x, int y)
    {
        return blueNoise16[y & 15][x & 15];
    }

    // =====================================================
    // CUBIC HERMITE INTERPOLATION
    // 4 nokta arasında C1-continuous smooth interpolasyon
    // Catmull-Rom spline variant
    // =====================================================

    static double cubicHermite(double y0, double y1, double y2, double y3, double t)
    {
        // Catmull-Rom spline coefficients
        double a = -0.5 * y0 + 1.5 * y1 - 1.5 * y2 + 0.5 * y3;
        double b = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
        double c = -0.5 * y0 + 0.5 * y2;
        double d = y1;

        double t2 = t * t;
        double t3 = t2 * t;

        return a * t3 + b * t2 + c * t + d;
    }

    // =====================================================
    // COLOR SPACE CONVERSIONS
    // =====================================================

    static void cmykToRgb(double c, double m, double y, double k, double outRgb[3])
    {
        outRgb[0] = (1.0 - c) * (1.0 - k);
        outRgb[1] = (1.0 - m) * (1.0 - k);
        outRgb[2] = (1.0 - y) * (1.0 - k);

        for (int i = 0; i < 3; ++i)
            outRgb[i] = std::clamp(outRgb[i], 0.0, 1.0);
    }

    static void colorToRGB(const double* color, int numComponents, double outRgb[3])
    {
        if (numComponents == 1)
        {
            outRgb[0] = outRgb[1] = outRgb[2] = std::clamp(color[0], 0.0, 1.0);
        }
        else if (numComponents == 3)
        {
            for (int i = 0; i < 3; ++i)
                outRgb[i] = std::clamp(color[i], 0.0, 1.0);
        }
        else if (numComponents == 4)
        {
            cmykToRgb(color[0], color[1], color[2], color[3], outRgb);
        }
        else
        {
            double avg = 0;
            for (int i = 0; i < numComponents; ++i)
                avg += color[i];
            avg /= numComponents;
            outRgb[0] = outRgb[1] = outRgb[2] = std::clamp(avg, 0.0, 1.0);
        }
    }

    // =====================================================
    // PdfGradient::buildLUTFromSamples
    // Düşük çözünürlüklü sample'lardan yüksek çözünürlüklü LUT oluştur
    // =====================================================

    void PdfGradient::buildLUTFromSamples(
        const std::vector<double>& samplesR,
        const std::vector<double>& samplesG,
        const std::vector<double>& samplesB)
    {
        int numSamples = (int)samplesR.size();
        if (numSamples < 2) return;

        lutR.resize(LUT_SIZE);
        lutG.resize(LUT_SIZE);
        lutB.resize(LUT_SIZE);
        hasLUT = true;

        // =====================================================
        // DEBUG: Sample değerlerini logla - sorunun kaynağını bul
        // =====================================================
        LogDebug("=== SAMPLE DEBUG: %d samples ===", numSamples);
        for (int i = 0; i < numSamples; ++i)
        {
            double brightness = 0.299 * samplesR[i] + 0.587 * samplesG[i] + 0.114 * samplesB[i];
            if (i < 10 || i > numSamples - 10 || brightness > 0.8)
            {
                LogDebug("  [%2d] R=%.3f G=%.3f B=%.3f (bright=%.3f)",
                    i, samplesR[i], samplesG[i], samplesB[i], brightness);
            }
        }

        // =====================================================
        // BASIT LINEAR INTERPOLASYON - MuPDF gibi
        // Smooth/blur yok - sample'ları olduğu gibi kullan
        // =====================================================
        for (int i = 0; i < LUT_SIZE; ++i)
        {
            double t = (double)i / (LUT_SIZE - 1);
            double floatIdx = t * (numSamples - 1);

            int idx = (int)floatIdx;
            double frac = floatIdx - idx;

            int i1 = std::min(idx, numSamples - 1);
            int i2 = std::min(idx + 1, numSamples - 1);

            // MuPDF tarzı basit linear interpolation
            lutR[i] = (float)(samplesR[i1] + frac * (samplesR[i2] - samplesR[i1]));
            lutG[i] = (float)(samplesG[i1] + frac * (samplesG[i2] - samplesG[i1]));
            lutB[i] = (float)(samplesB[i1] + frac * (samplesB[i2] - samplesB[i1]));
        }

        LogDebug("Built LUT: %d samples -> %d (MuPDF-style linear, NO SMOOTH)", numSamples, LUT_SIZE);
    }

    // =====================================================
    // PdfGradient::evaluateColor
    // Ana renk hesaplama fonksiyonu
    // =====================================================

    void PdfGradient::evaluateColor(double t, double outRgb[3]) const
    {
        t = std::clamp(t, 0.0, 1.0);

        // =====================================================
        // LUT VARSA LUT'TAN OKU (highlight'ları korur, banding yok)
        // LUT 4096 entry, smooth edilmiş - ara tonlar korunur
        // =====================================================
        if (hasLUT && !lutR.empty())
        {
            double floatIdx = t * (LUT_SIZE - 1);
            int idx = (int)floatIdx;
            double frac = floatIdx - idx;

            int i0 = std::min(idx, LUT_SIZE - 1);
            int i1 = std::min(idx + 1, LUT_SIZE - 1);

            // Linear interpolasyon (LUT zaten yüksek çözünürlüklü)
            outRgb[0] = lutR[i0] + frac * (lutR[i1] - lutR[i0]);
            outRgb[1] = lutG[i0] + frac * (lutG[i1] - lutG[i0]);
            outRgb[2] = lutB[i0] + frac * (lutB[i1] - lutB[i0]);
            return;
        }

        // =====================================================
        // LUT YOKSA: İlk ve son stop arasında pure linear
        // =====================================================
        if (stops.empty())
        {
            outRgb[0] = outRgb[1] = outRgb[2] = 0.0;
            return;
        }

        if (stops.size() == 1)
        {
            outRgb[0] = stops[0].rgb[0];
            outRgb[1] = stops[0].rgb[1];
            outRgb[2] = stops[0].rgb[2];
            return;
        }

        // İlk ve son stop arasında pure linear
        const GradientStop& first = stops.front();
        const GradientStop& last = stops.back();

        for (int c = 0; c < 3; ++c)
        {
            outRgb[c] = first.rgb[c] + t * (last.rgb[c] - first.rgb[c]);
        }
    }

    // =====================================================
    // PdfGradient::evaluateColorDithered
    // Blue noise dithering dahil renk hesaplama
    // =====================================================

    void PdfGradient::evaluateColorDithered(double t, int x, int y, uint8_t outRgb[3]) const
    {
        double rgb[3];
        evaluateColor(t, rgb);

        // Blue noise dithering
        float noise = getBlueNoise(x, y);

        for (int c = 0; c < 3; ++c)
        {
            // ±0.5 LSB dithering
            float val = (float)(rgb[c] * 255.0) + (noise - 0.5f);
            outRgb[c] = (uint8_t)std::clamp((int)(val + 0.5f), 0, 255);
        }
    }

    // =====================================================
    // FORWARD DECLARATIONS
    // =====================================================

    static bool parseFunctionType0(
        const std::shared_ptr<PdfDictionary>& funcDict,
        const std::shared_ptr<PdfStream>& funcStream,
        PdfDocument* doc,
        PdfGradient& gradient,
        int numComponents);

    static bool parseFunctionType2(
        const std::shared_ptr<PdfDictionary>& funcDict,
        PdfDocument* doc,
        std::vector<GradientStop>& outStops,
        int numComponents);

    static bool parseFunctionType3(
        const std::shared_ptr<PdfDictionary>& funcDict,
        PdfDocument* doc,
        PdfGradient& gradient,
        int numComponents);

    // =====================================================
    // PdfGradient::parseFunction (eski API)
    // =====================================================

    bool PdfGradient::parseFunction(
        const std::shared_ptr<PdfObject>& funcObj,
        PdfDocument* doc,
        std::vector<GradientStop>& outStops)
    {
        PdfGradient temp;
        bool result = parseFunctionToGradient(funcObj, doc, temp, 3);
        if (result)
            outStops = temp.stops;
        return result;
    }

    // =====================================================
    // PdfGradient::parseFunctionWithColorSpace (eski API)
    // =====================================================

    bool PdfGradient::parseFunctionWithColorSpace(
        const std::shared_ptr<PdfObject>& funcObj,
        PdfDocument* doc,
        std::vector<GradientStop>& outStops,
        int numComponents)
    {
        PdfGradient temp;
        bool result = parseFunctionToGradient(funcObj, doc, temp, numComponents);
        if (result)
            outStops = temp.stops;
        return result;
    }

    // =====================================================
    // PdfGradient::parseFunctionToGradient (YENİ API)
    // =====================================================

    bool PdfGradient::parseFunctionToGradient(
        const std::shared_ptr<PdfObject>& funcObj,
        PdfDocument* doc,
        PdfGradient& outGradient,
        int numComponents)
    {
        if (!funcObj || !doc) return false;

        std::set<int> visited;
        auto resolved = doc->resolve(funcObj, visited);

        if (!resolved) return false;

        std::shared_ptr<PdfDictionary> funcDict;
        std::shared_ptr<PdfStream> funcStream;

        if (auto stream = std::dynamic_pointer_cast<PdfStream>(resolved))
        {
            funcStream = stream;
            funcDict = stream->dict;
        }
        else if (auto dict = std::dynamic_pointer_cast<PdfDictionary>(resolved))
        {
            funcDict = dict;
        }

        if (!funcDict) return false;

        visited.clear();
        auto typeObj = std::dynamic_pointer_cast<PdfNumber>(
            doc->resolve(funcDict->get("/FunctionType"), visited));

        if (!typeObj) return false;

        int funcType = (int)typeObj->value;
        LogDebug("parseFunctionToGradient: type=%d, components=%d", funcType, numComponents);

        bool result = false;
        if (funcType == 0)
        {
            result = parseFunctionType0(funcDict, funcStream, doc, outGradient, numComponents);
            LogDebug("After Type0: hasLUT=%d, lutR.size=%zu, stops=%zu",
                outGradient.hasLUT ? 1 : 0, outGradient.lutR.size(), outGradient.stops.size());
        }
        else if (funcType == 2)
        {
            result = parseFunctionType2(funcDict, doc, outGradient.stops, numComponents);
            LogDebug("After Type2: stops=%zu", outGradient.stops.size());
        }
        else if (funcType == 3)
        {
            result = parseFunctionType3(funcDict, doc, outGradient, numComponents);
            LogDebug("After Type3: hasLUT=%d, lutR.size=%zu, stops=%zu",
                outGradient.hasLUT ? 1 : 0, outGradient.lutR.size(), outGradient.stops.size());
        }
        else
        {
            LogDebug("WARNING: Unsupported function type: %d", funcType);
        }

        return result;
    }

    // =====================================================
    // parseFunctionType0 - HIGH-RES LUT GENERATION
    // 8-bit quantized sample'ları 4096-entry LUT'a çevir
    // =====================================================

    static bool parseFunctionType0(
        const std::shared_ptr<PdfDictionary>& funcDict,
        const std::shared_ptr<PdfStream>& funcStream,
        PdfDocument* doc,
        PdfGradient& gradient,
        int numComponents)
    {
        if (!funcStream) return false;

        std::set<int> visited;

        LogDebug("--- parseFunctionType0 (LUT MODE) ---");

        // Size
        visited.clear();
        auto sizeArr = std::dynamic_pointer_cast<PdfArray>(
            doc->resolve(funcDict->get("/Size"), visited));
        if (!sizeArr || sizeArr->items.empty()) return false;

        visited.clear();
        auto sizeNum = std::dynamic_pointer_cast<PdfNumber>(
            doc->resolve(sizeArr->items[0], visited));
        int numSamples = sizeNum ? (int)sizeNum->value : 2;

        // BitsPerSample
        visited.clear();
        auto bpsObj = std::dynamic_pointer_cast<PdfNumber>(
            doc->resolve(funcDict->get("/BitsPerSample"), visited));
        int bitsPerSample = bpsObj ? (int)bpsObj->value : 8;

        LogDebug("Samples: %d, BitsPerSample: %d", numSamples, bitsPerSample);

        // Range
        visited.clear();
        auto rangeArr = std::dynamic_pointer_cast<PdfArray>(
            doc->resolve(funcDict->get("/Range"), visited));

        std::vector<double> rangeMin, rangeMax;
        int outputComponents = numComponents;

        if (rangeArr && rangeArr->items.size() >= 2)
        {
            outputComponents = (int)rangeArr->items.size() / 2;
            for (int i = 0; i < outputComponents; ++i)
            {
                visited.clear();
                auto rmin = std::dynamic_pointer_cast<PdfNumber>(
                    doc->resolve(rangeArr->items[i * 2], visited));
                visited.clear();
                auto rmax = std::dynamic_pointer_cast<PdfNumber>(
                    doc->resolve(rangeArr->items[i * 2 + 1], visited));
                rangeMin.push_back(rmin ? rmin->value : 0.0);
                rangeMax.push_back(rmax ? rmax->value : 1.0);
            }
        }
        else
        {
            for (int i = 0; i < outputComponents; ++i)
            {
                rangeMin.push_back(0.0);
                rangeMax.push_back(1.0);
            }
        }

        // Decode
        visited.clear();
        auto decodeArr = std::dynamic_pointer_cast<PdfArray>(
            doc->resolve(funcDict->get("/Decode"), visited));

        std::vector<double> decodeMin, decodeMax;
        if (decodeArr && decodeArr->items.size() >= (size_t)(outputComponents * 2))
        {
            for (int i = 0; i < outputComponents; ++i)
            {
                visited.clear();
                auto dmin = std::dynamic_pointer_cast<PdfNumber>(
                    doc->resolve(decodeArr->items[i * 2], visited));
                visited.clear();
                auto dmax = std::dynamic_pointer_cast<PdfNumber>(
                    doc->resolve(decodeArr->items[i * 2 + 1], visited));
                decodeMin.push_back(dmin ? dmin->value : rangeMin[i]);
                decodeMax.push_back(dmax ? dmax->value : rangeMax[i]);
            }
        }
        else
        {
            decodeMin = rangeMin;
            decodeMax = rangeMax;
        }

        // Stream decode
        std::vector<uint8_t> data;
        if (!doc->decodeStream(funcStream, data) || data.empty())
            data = funcStream->data;

        if (data.empty()) return false;

        // Sample okuma
        double maxSampleValue = (1 << bitsPerSample) - 1;
        int bytesPerSample = (bitsPerSample + 7) / 8;
        int bytesPerEntry = bytesPerSample * outputComponents;

        // Ham sample'ları oku
        std::vector<double> samplesR(numSamples);
        std::vector<double> samplesG(numSamples);
        std::vector<double> samplesB(numSamples);

        for (int sampleIdx = 0; sampleIdx < numSamples; ++sampleIdx)
        {
            int offset = sampleIdx * bytesPerEntry;
            if (offset + bytesPerEntry > (int)data.size()) break;

            std::vector<double> outputValues(outputComponents);
            for (int c = 0; c < outputComponents; ++c)
            {
                int byteOffset = offset + c * bytesPerSample;
                uint32_t rawValue = 0;
                for (int b = 0; b < bytesPerSample; ++b)
                    rawValue = (rawValue << 8) | data[byteOffset + b];

                double normalized = rawValue / maxSampleValue;
                outputValues[c] = decodeMin[c] + normalized * (decodeMax[c] - decodeMin[c]);
            }

            double rgb[3];
            colorToRGB(outputValues.data(), outputComponents, rgb);

            samplesR[sampleIdx] = rgb[0];
            samplesG[sampleIdx] = rgb[1];
            samplesB[sampleIdx] = rgb[2];
        }

        // =====================================================
        // SMOOTHING YOK - Orijinal sample değerlerini koru
        // LUT'un 4096 çözünürlüğü banding'i önlemeye yeterli
        // Smoothing highlight peak'lerini azaltıyordu!
        // =====================================================

        LogDebug("Using %d original samples (no smoothing, preserving highlights)", numSamples);

        // =====================================================
        // LUT OLUŞTUR - orijinal sample'lardan
        // =====================================================
        gradient.buildLUTFromSamples(samplesR, samplesG, samplesB);

        // =====================================================
        // STOP'LARI DA ORİJİNAL DEĞERLERLE DOLDUR
        // =====================================================
        for (int i = 0; i < numSamples; ++i)
        {
            GradientStop stop;
            stop.position = (numSamples > 1) ? (double)i / (numSamples - 1) : 0.0;
            stop.rgb[0] = samplesR[i];
            stop.rgb[1] = samplesG[i];
            stop.rgb[2] = samplesB[i];
            gradient.stops.push_back(stop);
        }

        LogDebug("Type 0: %d samples -> LUT[%d], stops=%zu",
            numSamples, PdfGradient::LUT_SIZE, gradient.stops.size());
        return true;
    }

    // =====================================================
    // parseFunctionType2 - Exponential Interpolation
    // =====================================================

    static bool parseFunctionType2(
        const std::shared_ptr<PdfDictionary>& funcDict,
        PdfDocument* doc,
        std::vector<GradientStop>& outStops,
        int numComponents)
    {
        std::set<int> visited;

        // Exponent N
        double N = 1.0;
        auto nObj = std::dynamic_pointer_cast<PdfNumber>(
            doc->resolve(funcDict->get("/N"), visited));
        if (nObj) N = nObj->value;

        // C0
        std::vector<double> c0(numComponents, 0.0);
        visited.clear();
        auto c0Arr = std::dynamic_pointer_cast<PdfArray>(
            doc->resolve(funcDict->get("/C0"), visited));
        if (c0Arr)
        {
            c0.resize(c0Arr->items.size());
            for (size_t i = 0; i < c0Arr->items.size(); ++i)
            {
                visited.clear();
                if (auto n = std::dynamic_pointer_cast<PdfNumber>(
                    doc->resolve(c0Arr->items[i], visited)))
                    c0[i] = n->value;
            }
        }

        // C1
        std::vector<double> c1(numComponents, 1.0);
        visited.clear();
        auto c1Arr = std::dynamic_pointer_cast<PdfArray>(
            doc->resolve(funcDict->get("/C1"), visited));
        if (c1Arr)
        {
            c1.resize(c1Arr->items.size());
            for (size_t i = 0; i < c1Arr->items.size(); ++i)
            {
                visited.clear();
                if (auto n = std::dynamic_pointer_cast<PdfNumber>(
                    doc->resolve(c1Arr->items[i], visited)))
                    c1[i] = n->value;
            }
        }

        double rgb0[3], rgb1[3];
        colorToRGB(c0.data(), (int)c0.size(), rgb0);
        colorToRGB(c1.data(), (int)c1.size(), rgb1);

        // Yeterli sayıda stop oluştur (256)
        const int numSteps = 256;
        for (int i = 0; i <= numSteps; ++i)
        {
            double t = (double)i / numSteps;
            double factor = std::pow(t, N);

            GradientStop s;
            s.position = t;

            // Gamma-correct interpolasyon
            for (int c = 0; c < 3; ++c)
            {
                double lin0 = srgbToLinear(rgb0[c]);
                double lin1 = srgbToLinear(rgb1[c]);
                double val = lin0 + factor * (lin1 - lin0);
                s.rgb[c] = linearToSrgb(std::clamp(val, 0.0, 1.0));
            }

            outStops.push_back(s);
        }

        LogDebug("Type 2: N=%.2f, %zu stops", N, outStops.size());
        return true;
    }

    // =====================================================
    // parseFunctionType3 - Stitching Function
    // MuPDF TARZI: 256 sample, function'ı runtime'da çağır
    // =====================================================

    static bool parseFunctionType3(
        const std::shared_ptr<PdfDictionary>& funcDict,
        PdfDocument* doc,
        PdfGradient& gradient,
        int numComponents)
    {
        std::set<int> visited;

        auto funcsArr = std::dynamic_pointer_cast<PdfArray>(
            doc->resolve(funcDict->get("/Functions"), visited));
        if (!funcsArr || funcsArr->items.empty()) return false;

        visited.clear();
        auto boundsArr = std::dynamic_pointer_cast<PdfArray>(
            doc->resolve(funcDict->get("/Bounds"), visited));

        std::vector<double> bounds;
        if (boundsArr)
        {
            for (auto& item : boundsArr->items)
            {
                visited.clear();
                if (auto n = std::dynamic_pointer_cast<PdfNumber>(
                    doc->resolve(item, visited)))
                    bounds.push_back(n->value);
            }
        }

        // Encode array - sub-function domain mapping
        visited.clear();
        auto encodeArr = std::dynamic_pointer_cast<PdfArray>(
            doc->resolve(funcDict->get("/Encode"), visited));

        std::vector<double> encode;
        if (encodeArr)
        {
            for (auto& item : encodeArr->items)
            {
                visited.clear();
                if (auto n = std::dynamic_pointer_cast<PdfNumber>(
                    doc->resolve(item, visited)))
                    encode.push_back(n->value);
            }
        }

        visited.clear();
        auto domainArr = std::dynamic_pointer_cast<PdfArray>(
            doc->resolve(funcDict->get("/Domain"), visited));

        double domainMin = 0.0, domainMax = 1.0;
        if (domainArr && domainArr->items.size() >= 2)
        {
            visited.clear();
            if (auto d0 = std::dynamic_pointer_cast<PdfNumber>(
                doc->resolve(domainArr->items[0], visited)))
                domainMin = d0->value;
            visited.clear();
            if (auto d1 = std::dynamic_pointer_cast<PdfNumber>(
                doc->resolve(domainArr->items[1], visited)))
                domainMax = d1->value;
        }

        LogDebug("Type 3: %zu sub-functions, bounds=%zu, encode=%zu",
            funcsArr->items.size(), bounds.size(), encode.size());

        // Sub-gradient'ları parse et
        std::vector<PdfGradient> subGradients;
        for (size_t i = 0; i < funcsArr->items.size(); ++i)
        {
            visited.clear();
            auto subFuncObj = doc->resolve(funcsArr->items[i], visited);
            if (!subFuncObj) continue;

            PdfGradient subGrad;
            if (PdfGradient::parseFunctionToGradient(subFuncObj, doc, subGrad, numComponents))
            {
                subGradients.push_back(std::move(subGrad));
            }
        }

        if (subGradients.empty()) return false;

        // =====================================================
        // MuPDF TARZI: 256 sample, HER T İÇİN DOĞRU SUB-FUNCTION'I ÇAĞIR
        // =====================================================
        const int NUM_SAMPLES = 256;
        std::vector<double> samplesR(NUM_SAMPLES), samplesG(NUM_SAMPLES), samplesB(NUM_SAMPLES);

        for (int i = 0; i < NUM_SAMPLES; ++i)
        {
            double t = domainMin + (double)i / (NUM_SAMPLES - 1) * (domainMax - domainMin);

            // Hangi sub-function?
            size_t funcIdx = 0;
            double low = domainMin, high = domainMax;

            for (size_t j = 0; j < bounds.size(); ++j)
            {
                if (t < bounds[j])
                {
                    funcIdx = j;
                    high = bounds[j];
                    if (j > 0) low = bounds[j - 1];
                    break;
                }
                else
                {
                    funcIdx = j + 1;
                    low = bounds[j];
                }
            }

            if (funcIdx >= subGradients.size())
                funcIdx = subGradients.size() - 1;

            // Encode uygula - sub-function domain'ine map et
            double subT = 0.0;
            if (high > low)
            {
                double encLo = 0.0, encHi = 1.0;
                if (encode.size() >= (funcIdx + 1) * 2)
                {
                    encLo = encode[funcIdx * 2];
                    encHi = encode[funcIdx * 2 + 1];
                }
                subT = encLo + (t - low) / (high - low) * (encHi - encLo);
            }
            subT = std::clamp(subT, 0.0, 1.0);

            // Sub-gradient'tan renk al
            double rgb[3];
            subGradients[funcIdx].evaluateColor(subT, rgb);

            samplesR[i] = rgb[0];
            samplesG[i] = rgb[1];
            samplesB[i] = rgb[2];
        }

        // LUT oluştur
        gradient.buildLUTFromSamples(samplesR, samplesG, samplesB);

        // Stop'ları da doldur (fallback için)
        for (int i = 0; i < NUM_SAMPLES; i += 4)
        {
            GradientStop s;
            s.position = (double)i / (NUM_SAMPLES - 1);
            s.rgb[0] = samplesR[i];
            s.rgb[1] = samplesG[i];
            s.rgb[2] = samplesB[i];
            gradient.stops.push_back(s);
        }

        LogDebug("Type 3: Built %d-sample LUT (MuPDF-style runtime eval)", NUM_SAMPLES);
        return true;
    }

    // =====================================================
    // DeviceN -> CMYK Mapping Helper
    // =====================================================
    static void deviceNToCMYK(
        const double* deviceNValues,
        const std::vector<std::string>& deviceNNames,
        double cmyk[4])
    {
        // Initialize CMYK to 0
        cmyk[0] = cmyk[1] = cmyk[2] = cmyk[3] = 0.0;

        // Map each DeviceN component to appropriate CMYK channel
        for (size_t i = 0; i < deviceNNames.size(); ++i)
        {
            std::string name = deviceNNames[i];
            // Normalize name (remove leading /)
            if (!name.empty() && name[0] == '/')
                name = name.substr(1);

            double val = deviceNValues[i];

            if (name == "Cyan" || name == "C")
                cmyk[0] = val;
            else if (name == "Magenta" || name == "M")
                cmyk[1] = val;
            else if (name == "Yellow" || name == "Y")
                cmyk[2] = val;
            else if (name == "Black" || name == "K")
                cmyk[3] = val;
            else
            {
                // Unknown component - try to guess based on common patterns
                // Many DeviceN use spot colors that map to specific CMYK
                LogDebug("DeviceN: Unknown component '%s', treating as gray", name.c_str());
                // Add to black channel as fallback
                cmyk[3] = std::max(cmyk[3], val);
            }
        }

        // Clamp values
        for (int i = 0; i < 4; ++i)
            cmyk[i] = std::clamp(cmyk[i], 0.0, 1.0);
    }

    // =====================================================
    // parseFunctionToGradientDeviceN
    // DeviceN color space icin ozel parsing
    // =====================================================
    bool PdfGradient::parseFunctionToGradientDeviceN(
        const std::shared_ptr<PdfObject>& funcObj,
        PdfDocument* doc,
        PdfGradient& outGradient,
        const std::vector<std::string>& deviceNNames)
    {
        if (!funcObj || !doc || deviceNNames.empty())
            return false;

        int numComponents = (int)deviceNNames.size();
        LogDebug("parseFunctionToGradientDeviceN: %d components", numComponents);

        std::set<int> visited;
        auto resolved = doc->resolve(funcObj, visited);
        if (!resolved) return false;

        std::shared_ptr<PdfDictionary> funcDict;
        std::shared_ptr<PdfStream> funcStream;

        if (auto stream = std::dynamic_pointer_cast<PdfStream>(resolved))
        {
            funcStream = stream;
            funcDict = stream->dict;
        }
        else if (auto dict = std::dynamic_pointer_cast<PdfDictionary>(resolved))
        {
            funcDict = dict;
        }

        if (!funcDict) return false;

        visited.clear();
        auto typeObj = std::dynamic_pointer_cast<PdfNumber>(
            doc->resolve(funcDict->get("/FunctionType"), visited));
        if (!typeObj) return false;

        int funcType = (int)typeObj->value;
        LogDebug("DeviceN Function type: %d", funcType);

        // =====================================================
        // Type 2: Exponential Interpolation
        // =====================================================
        if (funcType == 2)
        {
            // Get N (exponent)
            double N = 1.0;
            visited.clear();
            auto nObj = std::dynamic_pointer_cast<PdfNumber>(
                doc->resolve(funcDict->get("/N"), visited));
            if (nObj) N = nObj->value;

            // Get C0 (start color in DeviceN space)
            std::vector<double> c0(numComponents, 0.0);
            visited.clear();
            auto c0Arr = std::dynamic_pointer_cast<PdfArray>(
                doc->resolve(funcDict->get("/C0"), visited));
            if (c0Arr)
            {
                for (size_t i = 0; i < c0Arr->items.size() && i < (size_t)numComponents; ++i)
                {
                    visited.clear();
                    if (auto n = std::dynamic_pointer_cast<PdfNumber>(
                        doc->resolve(c0Arr->items[i], visited)))
                        c0[i] = n->value;
                }
            }

            // Get C1 (end color in DeviceN space)
            std::vector<double> c1(numComponents, 1.0);
            visited.clear();
            auto c1Arr = std::dynamic_pointer_cast<PdfArray>(
                doc->resolve(funcDict->get("/C1"), visited));
            if (c1Arr)
            {
                for (size_t i = 0; i < c1Arr->items.size() && i < (size_t)numComponents; ++i)
                {
                    visited.clear();
                    if (auto n = std::dynamic_pointer_cast<PdfNumber>(
                        doc->resolve(c1Arr->items[i], visited)))
                        c1[i] = n->value;
                }
            }

            LogDebug("DeviceN Type2: N=%.4f, C0 size=%zu, C1 size=%zu", N, c0.size(), c1.size());

            // Build LUT with 256 samples
            const int NUM_SAMPLES = 256;
            std::vector<double> samplesR(NUM_SAMPLES), samplesG(NUM_SAMPLES), samplesB(NUM_SAMPLES);

            for (int i = 0; i < NUM_SAMPLES; ++i)
            {
                double t = (double)i / (NUM_SAMPLES - 1);
                double factor = std::pow(t, N);

                // Interpolate in DeviceN space
                std::vector<double> deviceNColor(numComponents);
                for (int c = 0; c < numComponents; ++c)
                {
                    deviceNColor[c] = c0[c] + factor * (c1[c] - c0[c]);
                }

                // Convert DeviceN -> CMYK
                double cmyk[4];
                deviceNToCMYK(deviceNColor.data(), deviceNNames, cmyk);

                // Convert CMYK -> RGB
                double rgb[3];
                cmykToRgb(cmyk[0], cmyk[1], cmyk[2], cmyk[3], rgb);

                samplesR[i] = rgb[0];
                samplesG[i] = rgb[1];
                samplesB[i] = rgb[2];
            }

            // Build LUT
            outGradient.buildLUTFromSamples(samplesR, samplesG, samplesB);

            // Also fill stops for fallback
            for (int i = 0; i < NUM_SAMPLES; i += 4)
            {
                GradientStop s;
                s.position = (double)i / (NUM_SAMPLES - 1);
                s.rgb[0] = samplesR[i];
                s.rgb[1] = samplesG[i];
                s.rgb[2] = samplesB[i];
                outGradient.stops.push_back(s);
            }

            LogDebug("DeviceN Type2: Built %d-sample LUT", NUM_SAMPLES);
            return true;
        }

        // =====================================================
        // Type 3: Stitching Function
        // =====================================================
        if (funcType == 3)
        {
            visited.clear();
            auto funcsArr = std::dynamic_pointer_cast<PdfArray>(
                doc->resolve(funcDict->get("/Functions"), visited));
            if (!funcsArr || funcsArr->items.empty()) return false;

            // Get Bounds
            visited.clear();
            auto boundsArr = std::dynamic_pointer_cast<PdfArray>(
                doc->resolve(funcDict->get("/Bounds"), visited));
            std::vector<double> bounds;
            if (boundsArr)
            {
                for (auto& item : boundsArr->items)
                {
                    visited.clear();
                    if (auto n = std::dynamic_pointer_cast<PdfNumber>(
                        doc->resolve(item, visited)))
                        bounds.push_back(n->value);
                }
            }

            // Get Encode
            visited.clear();
            auto encodeArr = std::dynamic_pointer_cast<PdfArray>(
                doc->resolve(funcDict->get("/Encode"), visited));
            std::vector<double> encode;
            if (encodeArr)
            {
                for (auto& item : encodeArr->items)
                {
                    visited.clear();
                    if (auto n = std::dynamic_pointer_cast<PdfNumber>(
                        doc->resolve(item, visited)))
                        encode.push_back(n->value);
                }
            }

            // Get Domain
            visited.clear();
            auto domainArr = std::dynamic_pointer_cast<PdfArray>(
                doc->resolve(funcDict->get("/Domain"), visited));
            double domainMin = 0.0, domainMax = 1.0;
            if (domainArr && domainArr->items.size() >= 2)
            {
                visited.clear();
                if (auto d0 = std::dynamic_pointer_cast<PdfNumber>(
                    doc->resolve(domainArr->items[0], visited)))
                    domainMin = d0->value;
                visited.clear();
                if (auto d1 = std::dynamic_pointer_cast<PdfNumber>(
                    doc->resolve(domainArr->items[1], visited)))
                    domainMax = d1->value;
            }

            LogDebug("DeviceN Type3: %zu sub-functions, bounds=%zu, encode=%zu",
                funcsArr->items.size(), bounds.size(), encode.size());

            // Parse sub-gradients (recursively call DeviceN version)
            std::vector<PdfGradient> subGradients;
            for (size_t i = 0; i < funcsArr->items.size(); ++i)
            {
                visited.clear();
                auto subFuncObj = doc->resolve(funcsArr->items[i], visited);
                if (!subFuncObj) continue;

                PdfGradient subGrad;
                if (parseFunctionToGradientDeviceN(subFuncObj, doc, subGrad, deviceNNames))
                {
                    subGradients.push_back(std::move(subGrad));
                }
            }

            if (subGradients.empty()) return false;

            // Build LUT by sampling the stitching function
            const int NUM_SAMPLES = 256;
            std::vector<double> samplesR(NUM_SAMPLES), samplesG(NUM_SAMPLES), samplesB(NUM_SAMPLES);

            for (int i = 0; i < NUM_SAMPLES; ++i)
            {
                double t = domainMin + (double)i / (NUM_SAMPLES - 1) * (domainMax - domainMin);

                // Find which sub-function to use
                size_t funcIdx = 0;
                double low = domainMin, high = domainMax;

                for (size_t j = 0; j < bounds.size(); ++j)
                {
                    if (t < bounds[j])
                    {
                        funcIdx = j;
                        high = bounds[j];
                        if (j > 0) low = bounds[j - 1];
                        break;
                    }
                    else
                    {
                        funcIdx = j + 1;
                        low = bounds[j];
                    }
                }

                if (funcIdx >= subGradients.size())
                    funcIdx = subGradients.size() - 1;

                // Apply Encode mapping
                double subT = 0.0;
                if (high > low)
                {
                    double encLo = 0.0, encHi = 1.0;
                    if (encode.size() >= (funcIdx + 1) * 2)
                    {
                        encLo = encode[funcIdx * 2];
                        encHi = encode[funcIdx * 2 + 1];
                    }
                    subT = encLo + (t - low) / (high - low) * (encHi - encLo);
                }
                subT = std::clamp(subT, 0.0, 1.0);

                // Get color from sub-gradient (already in RGB from recursive call)
                double rgb[3];
                subGradients[funcIdx].evaluateColor(subT, rgb);

                samplesR[i] = rgb[0];
                samplesG[i] = rgb[1];
                samplesB[i] = rgb[2];
            }

            // Build LUT
            outGradient.buildLUTFromSamples(samplesR, samplesG, samplesB);

            // Fill stops for fallback
            for (int i = 0; i < NUM_SAMPLES; i += 4)
            {
                GradientStop s;
                s.position = (double)i / (NUM_SAMPLES - 1);
                s.rgb[0] = samplesR[i];
                s.rgb[1] = samplesG[i];
                s.rgb[2] = samplesB[i];
                outGradient.stops.push_back(s);
            }

            LogDebug("DeviceN Type3: Built %d-sample LUT", NUM_SAMPLES);
            return true;
        }

        LogDebug("DeviceN: Unsupported function type %d", funcType);
        return false;
    }

} // namespace pdf