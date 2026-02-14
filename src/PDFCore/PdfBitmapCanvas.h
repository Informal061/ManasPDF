#pragma once
#include "PdfPainter.h"
#include <vector>
#include <cstdint>
#include <string>

namespace pdf
{
    class PdfBitmapCanvas : public IPdfCanvas
    {
    public:
        PdfBitmapCanvas(int width, int height);

        // Çizim fonksiyonlarý
        void fillRect(double x, double y, double w, double h, uint32_t argb) override;
        void drawText(double x, double y, const std::string& text, uint32_t argb) override;
        void strokeLine(double x1, double y1, double x2, double y2, uint32_t argb, double width) override;

        const std::vector<uint8_t>& getBuffer() const { return _buffer; }

        int width()  const { return _width; }
        int height() const { return _height; }

    private:
        int _width;
        int _height;
        std::vector<uint8_t> _buffer; // RGBA

        void putPixel(int x, int y, uint32_t argb);
        void drawAsciiChar(int x, int y, char c, uint32_t argb);
    };
}
