#pragma once
#include <cmath>
#include <string>

namespace pdf
{
    struct PdfMatrix
    {
        // a b 0
        // c d 0
        // e f 1
        double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    };


    // Basit 2D matrix carpimi: R = A * B
    inline PdfMatrix PdfMul(const PdfMatrix& A, const PdfMatrix& B)
    {
        PdfMatrix R;
        R.a = A.a * B.a + A.b * B.c;
        R.b = A.a * B.b + A.b * B.d;
        R.c = A.c * B.a + A.d * B.c;
        R.d = A.c * B.b + A.d * B.d;
        R.e = A.e * B.a + A.f * B.c + B.e;
        R.f = A.e * B.b + A.f * B.d + B.f;
        return R;
    }

    struct PdfGraphicsState
    {
        PdfMatrix ctm;

        PdfMatrix textMatrix;
        PdfMatrix textLineMatrix;

        double textPosX = 0.0;
        double textPosY = 0.0;

        // Text
        double fontSize = 12.0;
        double charSpacing = 0.0;
        double wordSpacing = 0.0;
        double horizontalScale = 100.0;
        double leading = 0.0;
        double textRise = 0.0;

        // Colors
        double fillColor[3] = { 0, 0, 0 };
        double strokeColor[3] = { 0, 0, 0 };

        // Pattern fill/stroke deste�i
        std::string fillColorSpace;
        std::string strokeColorSpace;
        std::string fillPatternName;
        std::string strokePatternName;

        // ===== STROKE STATE =====
        double lineWidth = 1.0;

        int lineCap = 0;     // 0=butt 1=round 2=square   (PDF J)
        int lineJoin = 0;    // 0=miter 1=round 2=bevel  (PDF j)
        double miterLimit = 10.0; // (PDF M)

        // ===== TRANSPARENCY & BLEND MODE =====
        double fillAlpha = 1.0;      // ca - fill alpha
        double strokeAlpha = 1.0;    // CA - stroke alpha
        std::string blendMode = "/Normal";  // BM - blend mode

        PdfGraphicsState()
        {
            ctm = PdfMatrix();
            textMatrix = PdfMatrix();
            textLineMatrix = PdfMatrix();
        }
    };

}
