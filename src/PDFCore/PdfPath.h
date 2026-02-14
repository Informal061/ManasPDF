#pragma once
#include <vector>

namespace pdf
{
    struct PdfPathSegment
    {
        enum Type
        {
            MoveTo,
            LineTo,
            CurveTo,
            Close
        } type;

        // MoveTo / LineTo için x,y
        double x = 0;
        double y = 0;

        // CurveTo için kontrol noktaları
        double x1 = 0, y1 = 0;
        double x2 = 0, y2 = 0;
        double x3 = 0, y3 = 0;

        // MoveTo / LineTo
        PdfPathSegment(Type t, double xx, double yy)
            : type(t), x(xx), y(yy) {
        }

        // CurveTo
        PdfPathSegment(double cx1, double cy1,
            double cx2, double cy2,
            double cx3, double cy3)
            : type(CurveTo),
            x1(cx1), y1(cy1),
            x2(cx2), y2(cy2),
            x3(cx3), y3(cy3) {
        }

        // Close
        PdfPathSegment()
            : type(Close) {
        }
    };

    using PdfPath = std::vector<PdfPathSegment>;
}
