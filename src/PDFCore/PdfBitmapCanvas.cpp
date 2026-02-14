#include "pch.h"
#include "PdfBitmapCanvas.h"
#include <algorithm>
#include <cmath>

namespace pdf
{
    PdfBitmapCanvas::PdfBitmapCanvas(int width, int height)
        : _width(width), _height(height)
    {
        _buffer.resize(width * height * 4, 255); // ARGB → A=255 default
    }

    void PdfBitmapCanvas::putPixel(int x, int y, uint32_t argb)
    {
        if (x < 0 || y < 0 || x >= _width || y >= _height)
            return;

        int i = (y * _width + x) * 4;
        _buffer[i + 0] = (argb >> 16) & 0xFF; // R
        _buffer[i + 1] = (argb >> 8) & 0xFF; // G
        _buffer[i + 2] = (argb >> 0) & 0xFF; // B
        _buffer[i + 3] = (argb >> 24) & 0xFF; // A
    }

    void PdfBitmapCanvas::fillRect(double x, double y, double w, double h, uint32_t argb)
    {
        int ix = (int)x;
        int iy = (int)y;
        int iw = (int)w;
        int ih = (int)h;

        for (int yy = iy; yy < iy + ih; yy++)
            for (int xx = ix; xx < ix + iw; xx++)
                putPixel(xx, yy, argb);
    }

    void PdfBitmapCanvas::strokeLine(double x1, double y1, double x2, double y2, uint32_t argb, double width)
    {
        int ix1 = (int)x1;
        int iy1 = (int)y1;
        int ix2 = (int)x2;
        int iy2 = (int)y2;

        int dx = std::abs(ix2 - ix1);
        int sx = ix1 < ix2 ? 1 : -1;
        int dy = -std::abs(iy2 - iy1);
        int sy = iy1 < iy2 ? 1 : -1;

        int err = dx + dy;

        while (true)
        {
            putPixel(ix1, iy1, argb);

            if (ix1 == ix2 && iy1 == iy2)
                break;

            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; ix1 += sx; }
            if (e2 <= dx) { err += dx; iy1 += sy; }
        }
    }

    void PdfBitmapCanvas::drawAsciiChar(int x, int y, char c, uint32_t argb)
    {
        static const int W = 8;
        static const int H = 12;

        // Çok basit bir ascii font (placeholder)
        for (int yy = 0; yy < H; yy++)
            for (int xx = 0; xx < W; xx++)
                if ((xx + yy + c) % 7 == 0) // tamamen random fake font :)
                    putPixel(x + xx, y + yy, argb);
    }

    void PdfBitmapCanvas::drawText(double x, double y, const std::string& text, uint32_t argb)
    {
        int ix = (int)x;
        int iy = (int)y;

        int cursor = 0;

        for (char c : text)
        {
            drawAsciiChar(ix + cursor, iy, c, argb);
            cursor += 8; // monospace 8px
        }
    }
}
