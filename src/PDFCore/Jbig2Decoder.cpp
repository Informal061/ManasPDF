// =====================================================
// Jbig2Decoder.cpp - JBIG2 Decoder (ITU T.88 / ISO 14492)
// Pure C++ implementation, no external libraries
// Context bit ordering verified against jbig2dec reference
// =====================================================

#include "Jbig2Decoder.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cassert>
#include <map>
namespace pdf
{
    // =====================================================
    // Jbig2Bitmap
    // =====================================================
    void Jbig2Bitmap::allocate(int w, int h)
    {
        width = w;
        height = h;
        stride = (w + 7) / 8;
        data.assign((size_t)stride * h, 0);
    }

    int Jbig2Bitmap::getPixel(int x, int y) const
    {
        if (x < 0 || x >= width || y < 0 || y >= height) return 0;
        int byteIdx = y * stride + x / 8;
        int bitIdx = 7 - (x % 8);
        return (data[byteIdx] >> bitIdx) & 1;
    }

    void Jbig2Bitmap::setPixel(int x, int y, int val)
    {
        if (x < 0 || x >= width || y < 0 || y >= height) return;
        int byteIdx = y * stride + x / 8;
        int bitIdx = 7 - (x % 8);
        if (val)
            data[byteIdx] |= (1 << bitIdx);
        else
            data[byteIdx] &= ~(1 << bitIdx);
    }

    void Jbig2Bitmap::fill(int val)
    {
        std::memset(data.data(), val ? 0xFF : 0x00, data.size());
    }

    // Combination operators: OR=0, AND=1, XOR=2, XNOR=3, REPLACE=4
    void Jbig2Bitmap::compositeFrom(const Jbig2Bitmap& src, int dx, int dy, int op)
    {
        for (int sy = 0; sy < src.height; sy++) {
            int ty = dy + sy;
            if (ty < 0 || ty >= height) continue;
            for (int sx = 0; sx < src.width; sx++) {
                int tx = dx + sx;
                if (tx < 0 || tx >= width) continue;
                int s = src.getPixel(sx, sy);
                int d = getPixel(tx, ty);
                int result;
                switch (op) {
                    case 0: result = d | s; break;
                    case 1: result = d & s; break;
                    case 2: result = d ^ s; break;
                    case 3: result = ~(d ^ s) & 1; break;
                    case 4: result = s; break;
                    default: result = d | s; break;
                }
                setPixel(tx, ty, result);
            }
        }
    }

    // =====================================================
    // Bit stream reader (big-endian)
    // =====================================================
    class Jbig2BitReader
    {
    public:
        Jbig2BitReader(const uint8_t* data, size_t length)
            : _data(data), _length(length), _pos(0), _bitPos(0) {}

        uint32_t readBits(int n)
        {
            uint32_t val = 0;
            for (int i = 0; i < n; i++)
                val = (val << 1) | readBit();
            return val;
        }

        int readBit()
        {
            if (_pos >= _length) return 0;
            int bit = (_data[_pos] >> (7 - _bitPos)) & 1;
            _bitPos++;
            if (_bitPos >= 8) { _bitPos = 0; _pos++; }
            return bit;
        }

        uint32_t readU32() { return readBits(32); }
        uint16_t readU16() { return (uint16_t)readBits(16); }
        uint8_t readU8() { return (uint8_t)readBits(8); }

        void alignToByte()
        {
            if (_bitPos > 0) { _bitPos = 0; _pos++; }
        }

        size_t bytePosition() const { return _pos; }
        int bitPosition() const { return _bitPos; }
        size_t remaining() const { return (_pos < _length) ? (_length - _pos) : 0; }
        const uint8_t* dataAt(size_t offset) const { return _data + offset; }
        void seek(size_t bytePos) { _pos = bytePos; _bitPos = 0; }

    private:
        const uint8_t* _data;
        size_t _length;
        size_t _pos;
        int _bitPos;
    };

    // =====================================================
    // Arithmetic Decoder (QM Coder - ITU T.88 Annex E)
    // =====================================================
    struct ArithCtx {
        uint8_t index = 0;
        uint8_t mps = 0;
    };

    struct QMEntry {
        uint16_t qe;
        uint8_t nmps;
        uint8_t nlps;
        uint8_t switchFlag;
    };

    static const QMEntry QM_TABLE[47] = {
        {0x5601, 1,  1,  1}, // 0
        {0x3401, 2,  6,  0}, // 1
        {0x1801, 3,  9,  0}, // 2
        {0x0AC1, 4,  12, 0}, // 3
        {0x0521, 5,  29, 0}, // 4
        {0x0221, 38, 33, 0}, // 5
        {0x5601, 7,  6,  1}, // 6
        {0x5401, 8,  14, 0}, // 7
        {0x4801, 9,  14, 0}, // 8
        {0x3801, 10, 14, 0}, // 9
        {0x3001, 11, 17, 0}, // 10
        {0x2401, 12, 18, 0}, // 11
        {0x1C01, 13, 20, 0}, // 12
        {0x1601, 29, 21, 0}, // 13
        {0x5601, 15, 14, 1}, // 14
        {0x5401, 16, 14, 0}, // 15
        {0x5101, 17, 15, 0}, // 16
        {0x4801, 18, 16, 0}, // 17
        {0x3801, 19, 17, 0}, // 18
        {0x3401, 20, 18, 0}, // 19
        {0x3001, 21, 19, 0}, // 20
        {0x2801, 22, 19, 0}, // 21
        {0x2401, 23, 20, 0}, // 22
        {0x2201, 24, 21, 0}, // 23
        {0x1C01, 25, 22, 0}, // 24
        {0x1801, 26, 23, 0}, // 25
        {0x1601, 27, 24, 0}, // 26
        {0x1401, 28, 25, 0}, // 27
        {0x1201, 29, 26, 0}, // 28
        {0x1101, 30, 27, 0}, // 29
        {0x0AC1, 31, 28, 0}, // 30
        {0x09C1, 32, 29, 0}, // 31
        {0x08A1, 33, 30, 0}, // 32
        {0x0521, 34, 31, 0}, // 33
        {0x0441, 35, 32, 0}, // 34
        {0x02A1, 36, 33, 0}, // 35
        {0x0221, 37, 34, 0}, // 36
        {0x0141, 38, 35, 0}, // 37
        {0x0111, 39, 36, 0}, // 38
        {0x0085, 40, 37, 0}, // 39
        {0x0049, 41, 38, 0}, // 40
        {0x0025, 42, 39, 0}, // 41
        {0x0015, 43, 40, 0}, // 42
        {0x0009, 44, 41, 0}, // 43
        {0x0005, 45, 42, 0}, // 44
        {0x0001, 45, 43, 0}, // 45
        {0x5601, 46, 46, 0}, // 46
    };

    class ArithDecoder
    {
    public:
        ArithDecoder(const uint8_t* data, size_t length)
            : _data(data), _length(length), _pos(0)
        {
            // ITU T.88 Annex F software convention (complementary C register)
            _buf = (_pos < _length) ? _data[_pos++] : 0xFF;
            _C = (uint32_t)(~_buf & 0xFF) << 16;
            _prevBuf = _buf;
            readByte();
            _C <<= 7;
            _CT -= 7;
            _A = 0x8000;
        }

        int decodeBit(ArithCtx& cx)
        {
            const QMEntry& e = QM_TABLE[cx.index];
            _A -= e.qe;
            int bit;

            if ((_C >> 16) < _A) {
                if (_A & 0x8000) {
                    return cx.mps;
                }
                if (_A < e.qe) {
                    bit = 1 - cx.mps;
                    if (e.switchFlag) cx.mps = 1 - cx.mps;
                    cx.index = e.nlps;
                } else {
                    bit = cx.mps;
                    cx.index = e.nmps;
                }
            } else {
                _C -= (uint32_t)_A << 16;
                if (_A < e.qe) {
                    bit = cx.mps;
                    cx.index = e.nmps;
                } else {
                    bit = 1 - cx.mps;
                    if (e.switchFlag) cx.mps = 1 - cx.mps;
                    cx.index = e.nlps;
                }
                _A = e.qe;
            }

            // Renormalize
            do {
                if (_CT == 0) readByte();
                _A <<= 1;
                _C <<= 1;
                _CT--;
            } while (!(_A & 0x8000));

            return bit;
        }

        // Decode integer (ITU T.88 Annex A, Table A.1)
        // Returns false for OOB (out-of-band)
        bool decodeInteger(ArithCtx* ctx, int32_t& value)
        {
            int prev = 1;
            int S = decodeIntBit(ctx, prev);

            int32_t v;
            int nTail;
            int32_t offset;

            if (decodeIntBit(ctx, prev) == 0) {
                nTail = 2; offset = 0;       // Prefix "0" → [0,3]
            }
            else if (decodeIntBit(ctx, prev) == 0) {
                nTail = 4; offset = 4;       // Prefix "10" → [4,19]
            }
            else if (decodeIntBit(ctx, prev) == 0) {
                nTail = 6; offset = 20;      // Prefix "110" → [20,83]
            }
            else if (decodeIntBit(ctx, prev) == 0) {
                nTail = 8; offset = 84;      // Prefix "1110" → [84,339]
            }
            else if (decodeIntBit(ctx, prev) == 0) {
                nTail = 12; offset = 340;    // Prefix "11110" → [340,4435]
            }
            else {
                nTail = 32; offset = 4436;   // Prefix "11111" → [4436,...]
            }

            v = 0;
            for (int i = 0; i < nTail; i++)
                v = (v << 1) | decodeIntBit(ctx, prev);
            v += offset;

            if (S == 0) {
                value = v;
            } else if (v == 0) {
                return false; // OOB
            } else {
                value = -(int32_t)v;
            }
            return true;
        }

        // Decode IAID (symbol ID) - fixed length, no sign
        uint32_t decodeIAID(ArithCtx* ctx, int codeLen)
        {
            int prev = 1;
            for (int i = 0; i < codeLen; i++) {
                int bit = decodeBit(ctx[prev]);
                prev = (prev << 1) | bit;
            }
            return prev - (1 << codeLen);
        }

        uint32_t getC() const { return _C; }
        uint16_t getA() const { return _A; }
        int getCT() const { return _CT; }
        size_t getPos() const { return _pos; }

    private:
        int decodeIntBit(ArithCtx* ctx, int& prev)
        {
            int bit = decodeBit(ctx[prev]);
            if (prev < 256)
                prev = (prev << 1) | bit;
            else
                prev = (((prev << 1) | bit) & 511) | 256;
            return bit;
        }

        void readByte()
        {
            if (_pos >= _length) {
                _CT = 8;
                return;
            }

            _buf = _data[_pos++];

            // ITU T.88 Annex F software convention BYTEIN
            if (_prevBuf == 0xFF) {
                if (_buf > 0x8F) {
                    // Marker found - don't consume, keep _prevBuf as 0xFF
                    _CT = 8;
                    _pos--;
                    return; // Don't update _prevBuf
                } else {
                    _C += 0xFE00 - ((uint32_t)_buf << 9);
                    _CT = 7;
                }
            } else {
                _C += 0xFF00 - ((uint32_t)_buf << 8);
                _CT = 8;
            }
            _prevBuf = _buf;
        }

        const uint8_t* _data;
        size_t _length;
        size_t _pos;
        uint32_t _C = 0;
        uint16_t _A = 0;
        int _CT = 0;
        uint8_t _buf = 0;
        uint8_t _prevBuf = 0;
    };

    // =====================================================
    // MMR (Modified Modified READ / ITU T.6 Group 4) Decoder
    // Based on jbig2dec's jbig2_mmr.c (Artifex/Fitz/Ghostscript)
    // =====================================================

    // Huffman table node for MMR run-length decoding
    struct MmrTableNode {
        short val;
        short n_bits;
    };

    // White run-length decode table (ITU T.4)
    static const MmrTableNode mmrWhiteDecode[] = {
        {256, 12}, {272, 12}, {29, 8}, {30, 8}, {45, 8}, {46, 8}, {22, 7}, {22, 7},
        {23, 7}, {23, 7}, {47, 8}, {48, 8}, {13, 6}, {13, 6}, {13, 6}, {13, 6},
        {20, 7}, {20, 7}, {33, 8}, {34, 8}, {35, 8}, {36, 8}, {37, 8}, {38, 8},
        {19, 7}, {19, 7}, {31, 8}, {32, 8}, {1, 6}, {1, 6}, {1, 6}, {1, 6},
        {12, 6}, {12, 6}, {12, 6}, {12, 6}, {53, 8}, {54, 8}, {26, 7}, {26, 7},
        {39, 8}, {40, 8}, {41, 8}, {42, 8}, {43, 8}, {44, 8}, {21, 7}, {21, 7},
        {28, 7}, {28, 7}, {61, 8}, {62, 8}, {63, 8}, {0, 8}, {320, 8}, {384, 8},
        {10, 5}, {10, 5}, {10, 5}, {10, 5}, {10, 5}, {10, 5}, {10, 5}, {10, 5},
        {11, 5}, {11, 5}, {11, 5}, {11, 5}, {11, 5}, {11, 5}, {11, 5}, {11, 5},
        {27, 7}, {27, 7}, {59, 8}, {60, 8}, {288, 9}, {290, 9}, {18, 7}, {18, 7},
        {24, 7}, {24, 7}, {49, 8}, {50, 8}, {51, 8}, {52, 8}, {25, 7}, {25, 7},
        {55, 8}, {56, 8}, {57, 8}, {58, 8}, {192, 6}, {192, 6}, {192, 6}, {192, 6},
        {1664, 6}, {1664, 6}, {1664, 6}, {1664, 6}, {448, 8}, {512, 8}, {292, 9}, {640, 8},
        {576, 8}, {294, 9}, {296, 9}, {298, 9}, {300, 9}, {302, 9}, {256, 7}, {256, 7},
        {2, 4}, {2, 4}, {2, 4}, {2, 4}, {2, 4}, {2, 4}, {2, 4}, {2, 4},
        {2, 4}, {2, 4}, {2, 4}, {2, 4}, {2, 4}, {2, 4}, {2, 4}, {2, 4},
        {3, 4}, {3, 4}, {3, 4}, {3, 4}, {3, 4}, {3, 4}, {3, 4}, {3, 4},
        {3, 4}, {3, 4}, {3, 4}, {3, 4}, {3, 4}, {3, 4}, {3, 4}, {3, 4},
        {128, 5}, {128, 5}, {128, 5}, {128, 5}, {128, 5}, {128, 5}, {128, 5}, {128, 5},
        {8, 5}, {8, 5}, {8, 5}, {8, 5}, {8, 5}, {8, 5}, {8, 5}, {8, 5},
        {9, 5}, {9, 5}, {9, 5}, {9, 5}, {9, 5}, {9, 5}, {9, 5}, {9, 5},
        {16, 6}, {16, 6}, {16, 6}, {16, 6}, {17, 6}, {17, 6}, {17, 6}, {17, 6},
        {4, 4}, {4, 4}, {4, 4}, {4, 4}, {4, 4}, {4, 4}, {4, 4}, {4, 4},
        {4, 4}, {4, 4}, {4, 4}, {4, 4}, {4, 4}, {4, 4}, {4, 4}, {4, 4},
        {5, 4}, {5, 4}, {5, 4}, {5, 4}, {5, 4}, {5, 4}, {5, 4}, {5, 4},
        {5, 4}, {5, 4}, {5, 4}, {5, 4}, {5, 4}, {5, 4}, {5, 4}, {5, 4},
        {14, 6}, {14, 6}, {14, 6}, {14, 6}, {15, 6}, {15, 6}, {15, 6}, {15, 6},
        {64, 5}, {64, 5}, {64, 5}, {64, 5}, {64, 5}, {64, 5}, {64, 5}, {64, 5},
        {6, 4}, {6, 4}, {6, 4}, {6, 4}, {6, 4}, {6, 4}, {6, 4}, {6, 4},
        {6, 4}, {6, 4}, {6, 4}, {6, 4}, {6, 4}, {6, 4}, {6, 4}, {6, 4},
        {7, 4}, {7, 4}, {7, 4}, {7, 4}, {7, 4}, {7, 4}, {7, 4}, {7, 4},
        {7, 4}, {7, 4}, {7, 4}, {7, 4}, {7, 4}, {7, 4}, {7, 4}, {7, 4},
        {-2, 3}, {-2, 3}, {-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}, {-1, 0},
        {-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}, {-3, 4},
        {1792, 3}, {1792, 3}, {1984, 4}, {2048, 4}, {2112, 4}, {2176, 4}, {2240, 4}, {2304, 4},
        {1856, 3}, {1856, 3}, {1920, 3}, {1920, 3}, {2368, 4}, {2432, 4}, {2496, 4}, {2560, 4},
        {1472, 1}, {1536, 1}, {1600, 1}, {1728, 1}, {704, 1}, {768, 1}, {832, 1}, {896, 1},
        {960, 1}, {1024, 1}, {1088, 1}, {1152, 1}, {1216, 1}, {1280, 1}, {1344, 1}, {1408, 1}
    };

    // Black run-length decode table (ITU T.4)
    static const MmrTableNode mmrBlackDecode[] = {
        {128, 12}, {160, 13}, {224, 12}, {256, 12}, {10, 7}, {11, 7}, {288, 12}, {12, 7},
        {9, 6}, {9, 6}, {8, 6}, {8, 6}, {7, 5}, {7, 5}, {7, 5}, {7, 5},
        {6, 4}, {6, 4}, {6, 4}, {6, 4}, {6, 4}, {6, 4}, {6, 4}, {6, 4},
        {5, 4}, {5, 4}, {5, 4}, {5, 4}, {5, 4}, {5, 4}, {5, 4}, {5, 4},
        {1, 3}, {1, 3}, {1, 3}, {1, 3}, {1, 3}, {1, 3}, {1, 3}, {1, 3},
        {1, 3}, {1, 3}, {1, 3}, {1, 3}, {1, 3}, {1, 3}, {1, 3}, {1, 3},
        {4, 3}, {4, 3}, {4, 3}, {4, 3}, {4, 3}, {4, 3}, {4, 3}, {4, 3},
        {4, 3}, {4, 3}, {4, 3}, {4, 3}, {4, 3}, {4, 3}, {4, 3}, {4, 3},
        {3, 2}, {3, 2}, {3, 2}, {3, 2}, {3, 2}, {3, 2}, {3, 2}, {3, 2},
        {3, 2}, {3, 2}, {3, 2}, {3, 2}, {3, 2}, {3, 2}, {3, 2}, {3, 2},
        {3, 2}, {3, 2}, {3, 2}, {3, 2}, {3, 2}, {3, 2}, {3, 2}, {3, 2},
        {3, 2}, {3, 2}, {3, 2}, {3, 2}, {3, 2}, {3, 2}, {3, 2}, {3, 2},
        {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2},
        {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2},
        {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2},
        {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2},
        {-2, 4}, {-2, 4}, {-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}, {-1, 0},
        {-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}, {-3, 5},
        {1792, 4}, {1792, 4}, {1984, 5}, {2048, 5}, {2112, 5}, {2176, 5}, {2240, 5}, {2304, 5},
        {1856, 4}, {1856, 4}, {1920, 4}, {1920, 4}, {2368, 5}, {2432, 5}, {2496, 5}, {2560, 5},
        {18, 3}, {18, 3}, {18, 3}, {18, 3}, {18, 3}, {18, 3}, {18, 3}, {18, 3},
        {52, 5}, {52, 5}, {640, 6}, {704, 6}, {768, 6}, {832, 6}, {55, 5}, {55, 5},
        {56, 5}, {56, 5}, {1280, 6}, {1344, 6}, {1408, 6}, {1472, 6}, {59, 5}, {59, 5},
        {60, 5}, {60, 5}, {1536, 6}, {1600, 6}, {24, 4}, {24, 4}, {24, 4}, {24, 4},
        {25, 4}, {25, 4}, {25, 4}, {25, 4}, {1664, 6}, {1728, 6}, {320, 5}, {320, 5},
        {384, 5}, {384, 5}, {448, 5}, {448, 5}, {512, 6}, {576, 6}, {53, 5}, {53, 5},
        {54, 5}, {54, 5}, {896, 6}, {960, 6}, {1024, 6}, {1088, 6}, {1152, 6}, {1216, 6},
        {64, 3}, {64, 3}, {64, 3}, {64, 3}, {64, 3}, {64, 3}, {64, 3}, {64, 3},
        {13, 1}, {13, 1}, {13, 1}, {13, 1}, {13, 1}, {13, 1}, {13, 1}, {13, 1},
        {13, 1}, {13, 1}, {13, 1}, {13, 1}, {13, 1}, {13, 1}, {13, 1}, {13, 1},
        {23, 4}, {23, 4}, {50, 5}, {51, 5}, {44, 5}, {45, 5}, {46, 5}, {47, 5},
        {57, 5}, {58, 5}, {61, 5}, {256, 5}, {16, 3}, {16, 3}, {16, 3}, {16, 3},
        {17, 3}, {17, 3}, {17, 3}, {17, 3}, {48, 5}, {49, 5}, {62, 5}, {63, 5},
        {30, 5}, {31, 5}, {32, 5}, {33, 5}, {40, 5}, {41, 5}, {22, 4}, {22, 4},
        {14, 1}, {14, 1}, {14, 1}, {14, 1}, {14, 1}, {14, 1}, {14, 1}, {14, 1},
        {14, 1}, {14, 1}, {14, 1}, {14, 1}, {14, 1}, {14, 1}, {14, 1}, {14, 1},
        {15, 2}, {15, 2}, {15, 2}, {15, 2}, {15, 2}, {15, 2}, {15, 2}, {15, 2},
        {128, 5}, {192, 5}, {26, 5}, {27, 5}, {28, 5}, {29, 5}, {19, 4}, {19, 4},
        {20, 4}, {20, 4}, {34, 5}, {35, 5}, {36, 5}, {37, 5}, {38, 5}, {39, 5},
        {21, 4}, {21, 4}, {42, 5}, {43, 5}, {0, 3}, {0, 3}, {0, 3}, {0, 3}
    };

    // MMR special code values
    static const int MMR_ERROR = -1;
    static const int MMR_ZEROES = -2;
    static const int MMR_UNCOMPRESSED = -3;

    // MMR bit stream context - 32-bit word buffer, MSB-first
    struct MmrCtx {
        uint32_t width;
        uint32_t height;
        const uint8_t* data;
        size_t size;
        size_t consumed_bits;
        uint32_t data_index;
        uint32_t bit_index;
        uint32_t word;
    };

    static void mmrInit(MmrCtx* mmr, int width, int height, const uint8_t* data, size_t size)
    {
        mmr->width = width;
        mmr->height = height;
        mmr->data = data;
        mmr->size = size;
        mmr->data_index = 0;
        mmr->bit_index = 32;
        mmr->word = 0;
        mmr->consumed_bits = 0;

        while (mmr->bit_index >= 8 && mmr->data_index < mmr->size) {
            mmr->bit_index -= 8;
            mmr->word |= ((uint32_t)mmr->data[mmr->data_index] << mmr->bit_index);
            mmr->data_index++;
        }
    }

    static void mmrConsume(MmrCtx* mmr, int n_bits)
    {
        mmr->consumed_bits += n_bits;
        if (mmr->consumed_bits > mmr->size * 8)
            mmr->consumed_bits = mmr->size * 8;

        mmr->word <<= n_bits;
        mmr->bit_index += n_bits;
        while (mmr->bit_index >= 8 && mmr->data_index < mmr->size) {
            mmr->bit_index -= 8;
            mmr->word |= ((uint32_t)mmr->data[mmr->data_index] << mmr->bit_index);
            mmr->data_index++;
        }
    }

    static int mmrGetCode(MmrCtx* mmr, const MmrTableNode* table, int initial_bits)
    {
        uint32_t word = mmr->word;
        int table_ix = word >> (32 - initial_bits);
        int val = table[table_ix].val;
        int n_bits = table[table_ix].n_bits;

        if (n_bits > initial_bits) {
            int mask = (1 << (32 - initial_bits)) - 1;
            table_ix = val + ((word & mask) >> (32 - n_bits));
            val = table[table_ix].val;
            n_bits = initial_bits + table[table_ix].n_bits;
        }

        mmrConsume(mmr, n_bits);
        return val;
    }

    static int mmrGetRun(MmrCtx* mmr, const MmrTableNode* table, int initial_bits)
    {
        int result = 0;
        int val;

        do {
            val = mmrGetCode(mmr, table, initial_bits);
            if (val == MMR_ERROR || val == MMR_UNCOMPRESSED || val == MMR_ZEROES)
                return -1; // error
            result += val;
        } while (val >= 64);

        return result;
    }

    // Get bit from a line buffer (MSB-first within bytes)
    static inline int mmrGetBit(const uint8_t* buf, uint32_t x)
    {
        return (buf[x >> 3] >> (7 - (x & 7))) & 1;
    }

    // Find next changing element in line starting after position x
    static uint32_t mmrFindChangingElement(const uint8_t* line, uint32_t x, uint32_t w)
    {
        if (line == nullptr)
            return w;

        int a;
        static const uint32_t MINUS1 = UINT32_MAX;

        if (x == MINUS1) {
            a = 0;
            x = 0;
        } else if (x < w) {
            a = mmrGetBit(line, x);
            x++;
        } else {
            return x;
        }

        // Scan bit-by-bit (simple but correct implementation)
        while (x < w) {
            if (mmrGetBit(line, x) != a)
                return x;
            x++;
        }
        return w;
    }

    // Find next changing element of specific color
    static uint32_t mmrFindChangingElementOfColor(const uint8_t* line, uint32_t x, uint32_t w, int color)
    {
        if (line == nullptr)
            return w;
        x = mmrFindChangingElement(line, x, w);
        if (x < w && mmrGetBit(line, x) != color)
            x = mmrFindChangingElement(line, x, w);
        return x;
    }

    // Set bits [x0, x1) to 1 in a line buffer
    static void mmrSetBits(uint8_t* line, uint32_t x0, uint32_t x1)
    {
        static const uint8_t lm[8] = { 0xFF, 0x7F, 0x3F, 0x1F, 0x0F, 0x07, 0x03, 0x01 };
        static const uint8_t rm[8] = { 0x00, 0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE };

        if (x0 >= x1) return;

        uint32_t a0 = x0 >> 3;
        uint32_t a1 = x1 >> 3;
        uint32_t b0 = x0 & 7;
        uint32_t b1 = x1 & 7;

        if (a0 == a1) {
            line[a0] |= lm[b0] & rm[b1];
        } else {
            line[a0] |= lm[b0];
            for (uint32_t a = a0 + 1; a < a1; a++)
                line[a] = 0xFF;
            if (b1)
                line[a1] |= rm[b1];
        }
    }

    // Decode one MMR line using 2D coding (ITU T.6)
    static int mmrDecodeLine(MmrCtx* mmr, const uint8_t* ref, uint8_t* dst, int* eofb)
    {
        static const uint32_t MINUS1 = UINT32_MAX;
        uint32_t a0 = MINUS1;
        uint32_t a1, a2, b1, b2;
        int c = 0; // 0=white, 1=black

        while (1) {
            uint32_t word = mmr->word;

            if (a0 != MINUS1 && a0 >= mmr->width)
                break;

            // Horizontal mode: prefix 001
            if ((word >> (32 - 3)) == 1) {
                int white_run, black_run;
                mmrConsume(mmr, 3);

                if (a0 == MINUS1)
                    a0 = 0;

                if (c == 0) {
                    white_run = mmrGetRun(mmr, mmrWhiteDecode, 8);
                    if (white_run < 0) return -1;
                    black_run = mmrGetRun(mmr, mmrBlackDecode, 7);
                    if (black_run < 0) return -1;
                    a1 = a0 + white_run;
                    a2 = a1 + black_run;
                    if (a1 > mmr->width) a1 = mmr->width;
                    if (a2 > mmr->width) a2 = mmr->width;
                    if (a2 < a1) a2 = a1;
                    if (a1 < mmr->width)
                        mmrSetBits(dst, a1, a2);
                    a0 = a2;
                } else {
                    black_run = mmrGetRun(mmr, mmrBlackDecode, 7);
                    if (black_run < 0) return -1;
                    white_run = mmrGetRun(mmr, mmrWhiteDecode, 8);
                    if (white_run < 0) return -1;
                    a1 = a0 + black_run;
                    a2 = a1 + white_run;
                    if (a1 > mmr->width) a1 = mmr->width;
                    if (a2 > mmr->width) a2 = mmr->width;
                    if (a1 < a0) a1 = a0;
                    if (a0 < mmr->width)
                        mmrSetBits(dst, a0, a1);
                    a0 = a2;
                }
            }
            // Pass mode: prefix 0001
            else if ((word >> (32 - 4)) == 1) {
                mmrConsume(mmr, 4);
                b1 = mmrFindChangingElementOfColor(ref, a0, mmr->width, !c);
                b2 = mmrFindChangingElement(ref, b1, mmr->width);
                if (c) {
                    if (b2 < a0) b2 = a0;
                    if (a0 < mmr->width)
                        mmrSetBits(dst, a0, b2);
                }
                a0 = b2;
            }
            // V(0): prefix 1
            else if ((word >> (32 - 1)) == 1) {
                mmrConsume(mmr, 1);
                b1 = mmrFindChangingElementOfColor(ref, a0, mmr->width, !c);
                if (c) {
                    if (b1 < a0) b1 = a0;
                    if (a0 < mmr->width)
                        mmrSetBits(dst, a0, b1);
                }
                a0 = b1;
                c = !c;
            }
            // VR(1): prefix 011
            else if ((word >> (32 - 3)) == 3) {
                mmrConsume(mmr, 3);
                b1 = mmrFindChangingElementOfColor(ref, a0, mmr->width, !c);
                if (b1 + 1 <= mmr->width) b1 += 1;
                if (c) {
                    if (b1 < a0) b1 = a0;
                    if (a0 < mmr->width)
                        mmrSetBits(dst, a0, b1);
                }
                a0 = b1;
                c = !c;
            }
            // VR(2): prefix 000011
            else if ((word >> (32 - 6)) == 3) {
                mmrConsume(mmr, 6);
                b1 = mmrFindChangingElementOfColor(ref, a0, mmr->width, !c);
                if (b1 + 2 <= mmr->width) b1 += 2;
                if (c) {
                    if (b1 < a0) b1 = a0;
                    if (a0 < mmr->width)
                        mmrSetBits(dst, a0, b1);
                }
                a0 = b1;
                c = !c;
            }
            // VR(3): prefix 0000011
            else if ((word >> (32 - 7)) == 3) {
                mmrConsume(mmr, 7);
                b1 = mmrFindChangingElementOfColor(ref, a0, mmr->width, !c);
                if (b1 + 3 <= mmr->width) b1 += 3;
                if (c) {
                    if (b1 < a0) b1 = a0;
                    if (a0 < mmr->width)
                        mmrSetBits(dst, a0, b1);
                }
                a0 = b1;
                c = !c;
            }
            // VL(1): prefix 010
            else if ((word >> (32 - 3)) == 2) {
                mmrConsume(mmr, 3);
                b1 = mmrFindChangingElementOfColor(ref, a0, mmr->width, !c);
                if (b1 >= 1) b1 -= 1;
                if (c) {
                    if (b1 < a0) b1 = a0;
                    if (a0 < mmr->width)
                        mmrSetBits(dst, a0, b1);
                }
                a0 = b1;
                c = !c;
            }
            // VL(2): prefix 000010
            else if ((word >> (32 - 6)) == 2) {
                mmrConsume(mmr, 6);
                b1 = mmrFindChangingElementOfColor(ref, a0, mmr->width, !c);
                if (b1 >= 2) b1 -= 2;
                if (c) {
                    if (b1 < a0) b1 = a0;
                    if (a0 < mmr->width)
                        mmrSetBits(dst, a0, b1);
                }
                a0 = b1;
                c = !c;
            }
            // VL(3): prefix 0000010
            else if ((word >> (32 - 7)) == 2) {
                mmrConsume(mmr, 7);
                b1 = mmrFindChangingElementOfColor(ref, a0, mmr->width, !c);
                if (b1 >= 3) b1 -= 3;
                if (c) {
                    if (b1 < a0) b1 = a0;
                    if (a0 < mmr->width)
                        mmrSetBits(dst, a0, b1);
                }
                a0 = b1;
                c = !c;
            }
            // EOFB: 000000000001 000000000001 (24 bits: 0x001001)
            else if ((word >> (32 - 24)) == 0x1001) {
                mmrConsume(mmr, 24);
                *eofb = 1;
                break;
            }
            else {
                // Unknown code - break to avoid infinite loop
                break;
            }
        }

        return 0;
    }

    // Decode a full MMR-coded generic region
    static Jbig2Bitmap decodeMMRRegion(const uint8_t* data, size_t length, int width, int height)
    {
        Jbig2Bitmap bm;
        bm.allocate(width, height);

        MmrCtx mmr;
        mmrInit(&mmr, width, height, data, length);

        int rowstride = bm.stride;
        int eofb = 0;

        for (int y = 0; !eofb && y < height; y++) {
            uint8_t* dst = bm.data.data() + y * rowstride;
            const uint8_t* ref = (y > 0) ? (bm.data.data() + (y - 1) * rowstride) : nullptr;
            std::memset(dst, 0, rowstride);
            int code = mmrDecodeLine(&mmr, ref, dst, &eofb);
            if (code < 0) {
                break;
            }
        }

        // If EOFB came early, remaining rows are already zero-filled from allocate()
        return bm;
    }

    // Decode MMR for halftone/pattern dict - returns consumed bytes count
    static Jbig2Bitmap decodeMMRRegionWithConsumed(const uint8_t* data, size_t length,
                                                     int width, int height, size_t* consumed_bytes)
    {
        Jbig2Bitmap bm;
        bm.allocate(width, height);

        MmrCtx mmr;
        mmrInit(&mmr, width, height, data, length);

        int rowstride = bm.stride;
        int eofb = 0;

        for (int y = 0; !eofb && y < height; y++) {
            uint8_t* dst = bm.data.data() + y * rowstride;
            const uint8_t* ref = (y > 0) ? (bm.data.data() + (y - 1) * rowstride) : nullptr;
            std::memset(dst, 0, rowstride);
            mmrDecodeLine(&mmr, ref, dst, &eofb);
        }

        // Check for trailing EOFB (section 6.2.6)
        if ((mmr.word >> 8) == 0x001001) {
            mmrConsume(&mmr, 24);
        }

        if (consumed_bytes)
            *consumed_bytes = (mmr.consumed_bits + 7) / 8;

        return bm;
    }

    // =====================================================
    // Generic Region Decoder (Section 6.2)
    // Bit ordering per jbig2dec reference / ITU T.88 Fig 3
    // =====================================================
    static Jbig2Bitmap decodeGenericRegion(
        ArithDecoder& arith,
        int width, int height,
        int templateID,
        bool tpgdOn,
        int atx[4], int aty[4],
        ArithCtx* externalGbStats = nullptr)
    {
        Jbig2Bitmap bm;
        bm.allocate(width, height);

        int contextSize;
        switch (templateID) {
            case 0: contextSize = 65536; break; // 16 bits
            case 1: contextSize = 8192; break;  // 13 bits
            case 2: contextSize = 1024; break;  // 10 bits
            case 3: contextSize = 1024; break;  // 10 bits
            default: contextSize = 65536; break;
        }

        // Use external contexts if provided (for shared GB_stats in symbol dict)
        std::vector<ArithCtx> localCtx;
        ArithCtx* gbCtxPtr;
        if (externalGbStats) {
            gbCtxPtr = externalGbStats;
        } else {
            localCtx.resize(contextSize);
            gbCtxPtr = localCtx.data();
        }
        ArithCtx tpgdCtx;
        bool ltp = false;

        for (int y = 0; y < height; y++) {
            if (tpgdOn) {
                ltp = ltp != (arith.decodeBit(tpgdCtx) != 0);
                if (ltp) {
                    if (y > 0) {
                        for (int x = 0; x < width; x++)
                            bm.setPixel(x, y, bm.getPixel(x, y - 1));
                    }
                    continue;
                }
            }

            for (int x = 0; x < width; x++) {
                uint32_t context = 0;

                if (templateID == 0) {
                    // Template 0: 16-bit context (ITU T.88 Figure 3)
                    // Bit ordering per spec: a0=(x-1,y) ... a15=(x-2,y-2)[AT3]
                    context =
                        ((uint32_t)bm.getPixel(x - 1, y)) |
                        ((uint32_t)bm.getPixel(x - 2, y) << 1) |
                        ((uint32_t)bm.getPixel(x - 3, y) << 2) |
                        ((uint32_t)bm.getPixel(x - 4, y) << 3) |
                        ((uint32_t)bm.getPixel(x + atx[0], y + aty[0]) << 4) |
                        ((uint32_t)bm.getPixel(x + 2, y - 1) << 5) |
                        ((uint32_t)bm.getPixel(x + 1, y - 1) << 6) |
                        ((uint32_t)bm.getPixel(x    , y - 1) << 7) |
                        ((uint32_t)bm.getPixel(x - 1, y - 1) << 8) |
                        ((uint32_t)bm.getPixel(x - 2, y - 1) << 9) |
                        ((uint32_t)bm.getPixel(x + atx[1], y + aty[1]) << 10) |
                        ((uint32_t)bm.getPixel(x + atx[2], y + aty[2]) << 11) |
                        ((uint32_t)bm.getPixel(x + 1, y - 2) << 12) |
                        ((uint32_t)bm.getPixel(x    , y - 2) << 13) |
                        ((uint32_t)bm.getPixel(x - 1, y - 2) << 14) |
                        ((uint32_t)bm.getPixel(x + atx[3], y + aty[3]) << 15);
                }
                else if (templateID == 1) {
                    // Template 1: 13-bit context (ITU T.88 Figure 4, jbig2dec verified)
                    context =
                        ((uint32_t)bm.getPixel(x - 1, y)) |
                        ((uint32_t)bm.getPixel(x - 2, y) << 1) |
                        ((uint32_t)bm.getPixel(x - 3, y) << 2) |
                        ((uint32_t)bm.getPixel(x + atx[0], y + aty[0]) << 3) |
                        ((uint32_t)bm.getPixel(x + 2, y - 1) << 4) |
                        ((uint32_t)bm.getPixel(x + 1, y - 1) << 5) |
                        ((uint32_t)bm.getPixel(x    , y - 1) << 6) |
                        ((uint32_t)bm.getPixel(x - 1, y - 1) << 7) |
                        ((uint32_t)bm.getPixel(x - 2, y - 1) << 8) |
                        ((uint32_t)bm.getPixel(x + 2, y - 2) << 9) |
                        ((uint32_t)bm.getPixel(x + 1, y - 2) << 10) |
                        ((uint32_t)bm.getPixel(x    , y - 2) << 11) |
                        ((uint32_t)bm.getPixel(x - 1, y - 2) << 12);
                }
                else if (templateID == 2) {
                    // Template 2: 10-bit context (ITU T.88 Figure 5, jbig2dec verified)
                    context =
                        ((uint32_t)bm.getPixel(x - 1, y)) |
                        ((uint32_t)bm.getPixel(x - 2, y) << 1) |
                        ((uint32_t)bm.getPixel(x + atx[0], y + aty[0]) << 2) |
                        ((uint32_t)bm.getPixel(x + 1, y - 1) << 3) |
                        ((uint32_t)bm.getPixel(x    , y - 1) << 4) |
                        ((uint32_t)bm.getPixel(x - 1, y - 1) << 5) |
                        ((uint32_t)bm.getPixel(x - 2, y - 1) << 6) |
                        ((uint32_t)bm.getPixel(x + 1, y - 2) << 7) |
                        ((uint32_t)bm.getPixel(x    , y - 2) << 8) |
                        ((uint32_t)bm.getPixel(x - 1, y - 2) << 9);
                }
                else { // template 3
                    // Template 3: 10-bit context (ITU T.88 Figure 6, jbig2dec verified)
                    // Only uses rows y and y-1
                    context =
                        ((uint32_t)bm.getPixel(x - 1, y)) |
                        ((uint32_t)bm.getPixel(x - 2, y) << 1) |
                        ((uint32_t)bm.getPixel(x - 3, y) << 2) |
                        ((uint32_t)bm.getPixel(x - 4, y) << 3) |
                        ((uint32_t)bm.getPixel(x + atx[0], y + aty[0]) << 4) |
                        ((uint32_t)bm.getPixel(x + 2, y - 1) << 5) |
                        ((uint32_t)bm.getPixel(x + 1, y - 1) << 6) |
                        ((uint32_t)bm.getPixel(x    , y - 1) << 7) |
                        ((uint32_t)bm.getPixel(x - 1, y - 1) << 8) |
                        ((uint32_t)bm.getPixel(x - 2, y - 1) << 9);
                }

                int pixel = arith.decodeBit(gbCtxPtr[context]);
                bm.setPixel(x, y, pixel);
            }
        }

        return bm;
    }

    // =====================================================
    // Generic Refinement Region Decoder (Section 6.3)
    // Bit ordering per jbig2dec reference / ITU T.88 Fig 12-13
    // =====================================================
    static Jbig2Bitmap decodeRefinementRegion(
        ArithDecoder& arith,
        int width, int height,
        const Jbig2Bitmap& ref,
        int refDX, int refDY,
        int templateID,
        int ratx[2], int raty[2],
        ArithCtx* externalGrStats = nullptr)
    {
        Jbig2Bitmap bm;
        bm.allocate(width, height);

        int contextSize = (templateID == 0) ? 8192 : 1024; // 13 or 10 bits

        // Use external contexts if provided (for shared GR_stats in symbol dict)
        std::vector<ArithCtx> localCtx;
        ArithCtx* grCtxPtr;
        if (externalGrStats) {
            grCtxPtr = externalGrStats;
        } else {
            localCtx.resize(contextSize);
            grCtxPtr = localCtx.data();
        }
        ArithCtx* grCtx = grCtxPtr;

        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                uint32_t context = 0;

                int rx = x - refDX;
                int ry = y - refDY;

                if (templateID == 0) {
                    // Template 0: 13-bit context (ITU T.88 Figure 12)
                    // jbig2dec-compatible ordering: spec [12]→bit0, [0]→bit12
                    context =
                        ((uint32_t)bm.getPixel(x - 1, y)) |                          // [12]
                        ((uint32_t)bm.getPixel(x + 1, y - 1) << 1) |                 // [11]
                        ((uint32_t)bm.getPixel(x    , y - 1) << 2) |                 // [10]
                        ((uint32_t)bm.getPixel(x + ratx[0], y + raty[0]) << 3) |     // [9] AT
                        ((uint32_t)ref.getPixel(rx + ratx[1], ry + raty[1]) << 4) |   // [8] AT
                        ((uint32_t)ref.getPixel(rx    , ry - 1) << 5) |               // [7]
                        ((uint32_t)ref.getPixel(rx + 1, ry - 1) << 6) |              // [6]
                        ((uint32_t)ref.getPixel(rx - 1, ry) << 7) |                  // [5]
                        ((uint32_t)ref.getPixel(rx    , ry) << 8) |                   // [4]
                        ((uint32_t)ref.getPixel(rx + 1, ry) << 9) |                  // [3]
                        ((uint32_t)ref.getPixel(rx - 1, ry + 1) << 10) |             // [2]
                        ((uint32_t)ref.getPixel(rx    , ry + 1) << 11) |              // [1]
                        ((uint32_t)ref.getPixel(rx + 1, ry + 1) << 12);              // [0]
                }
                else {
                    // Template 1: 10-bit context (ITU T.88 Figure 13)
                    // jbig2dec-compatible ordering: spec [9]→bit0, [0]→bit9
                    context =
                        ((uint32_t)bm.getPixel(x - 1, y)) |                          // [9]
                        ((uint32_t)bm.getPixel(x + 1, y - 1) << 1) |                 // [8]
                        ((uint32_t)bm.getPixel(x    , y - 1) << 2) |                 // [7]
                        ((uint32_t)bm.getPixel(x - 1, y - 1) << 3) |                 // [6]
                        ((uint32_t)ref.getPixel(rx    , ry - 1) << 4) |               // [5]
                        ((uint32_t)ref.getPixel(rx - 1, ry) << 5) |                  // [4]
                        ((uint32_t)ref.getPixel(rx    , ry) << 6) |                   // [3]
                        ((uint32_t)ref.getPixel(rx + 1, ry) << 7) |                  // [2]
                        ((uint32_t)ref.getPixel(rx    , ry + 1) << 8) |               // [1]
                        ((uint32_t)ref.getPixel(rx + 1, ry + 1) << 9);               // [0]
                }

                int pixel = arith.decodeBit(grCtx[context]);
                bm.setPixel(x, y, pixel);
            }
        }

        return bm;
    }

    // =====================================================
    // JBIG2 Segment Types
    // =====================================================
    enum Jbig2SegmentType {
        SEG_SYMBOL_DICT = 0,
        SEG_TEXT_REGION_IMM = 6,
        SEG_TEXT_REGION_IMM_LOSSLESS = 7,
        SEG_PATTERN_DICT = 16,
        SEG_HALFTONE_REGION_INTERMEDIATE = 22,
        SEG_HALFTONE_REGION_IMM = 23,
        SEG_HALFTONE_REGION_IMM_LOSSLESS = 24,
        SEG_GENERIC_REGION_INTERMEDIATE = 36,
        SEG_GENERIC_REGION_IMM = 38,
        SEG_GENERIC_REGION_IMM_LOSSLESS = 39,
        SEG_GENERIC_REFINE_INTERMEDIATE = 40,
        SEG_GENERIC_REFINE_IMM = 42,
        SEG_GENERIC_REFINE_IMM_LOSSLESS = 43,
        SEG_PAGE_INFO = 48,
        SEG_END_OF_PAGE = 49,
        SEG_END_OF_STRIPE = 50,
        SEG_END_OF_FILE = 51,
    };

    struct Jbig2Segment {
        uint32_t number;
        int type;
        bool pageAssoc4byte;
        std::vector<uint32_t> referredTo;
        uint32_t pageAssociation;
        uint32_t dataLength;
        const uint8_t* data;
    };

    // =====================================================
    // Parse segment header
    // =====================================================
    static bool parseSegmentHeader(Jbig2BitReader& reader, Jbig2Segment& seg)
    {
        if (reader.remaining() < 11) return false;

        seg.number = reader.readU32();
        uint8_t flags = reader.readU8();
        seg.type = flags & 0x3F;
        seg.pageAssoc4byte = (flags >> 6) & 1;

        uint8_t refByte = reader.readU8();
        int refCount = (refByte >> 5) & 7;

        if (refCount == 7) {
            // Long form
            uint32_t longCount = reader.readU32();
            refCount = longCount & 0x1FFFFFFF;
            // Skip retain flag bytes
            int retainBytes = (refCount + 9) / 8;
            for (int i = 0; i < retainBytes; i++)
                reader.readU8();
        }

        // Segment number size depends on current segment number
        int segNumSize = 1;
        if (seg.number > 65535) segNumSize = 4;
        else if (seg.number > 255) segNumSize = 2;

        seg.referredTo.clear();
        for (int i = 0; i < refCount; i++) {
            uint32_t refSeg;
            if (segNumSize == 4) refSeg = reader.readU32();
            else if (segNumSize == 2) refSeg = reader.readU16();
            else refSeg = reader.readU8();
            seg.referredTo.push_back(refSeg);
        }

        if (seg.pageAssoc4byte)
            seg.pageAssociation = reader.readU32();
        else
            seg.pageAssociation = reader.readU8();

        seg.dataLength = reader.readU32();

        reader.alignToByte();
        size_t dataOffset = reader.bytePosition();
        seg.data = reader.dataAt(dataOffset);
        reader.seek(dataOffset + seg.dataLength);

        return true;
    }

    // =====================================================
    // Calculate IAID code length: ceil(log2(n))
    // =====================================================
    static int calcCodeLen(uint32_t n)
    {
        if (n <= 1) return 1;
        int bits = 0;
        uint32_t t = n - 1;
        while (t > 0) { bits++; t >>= 1; }
        return (bits < 1) ? 1 : bits;
    }

    // =====================================================
    // Decode Symbol Dictionary (Section 6.5)
    // =====================================================
    static bool decodeSymbolDictionary(
        const Jbig2Segment& seg,
        const std::map<uint32_t, std::vector<Jbig2Bitmap>>& segmentSymbols,
        std::vector<Jbig2Bitmap>& exportedSymbols)
    {
        if (seg.dataLength < 10) return false;

        const uint8_t* d = seg.data;

        uint16_t sdFlags = (d[0] << 8) | d[1];
        bool sdHuff = sdFlags & 1;
        bool sdRefAgg = (sdFlags >> 1) & 1;
        int sdTemplate = (sdFlags >> 10) & 3;
        int sdrTemplate = (sdFlags >> 12) & 1;

        int offset = 2;

        // AT pixel offsets
        int numAT = (sdTemplate == 0) ? 4 : 1;
        int atx[4] = { 3, -3, 2, -2 };
        int aty[4] = { -1, -1, -2, -2 };

        if (!sdHuff) {
            for (int i = 0; i < numAT; i++) {
                atx[i] = (int8_t)d[offset++];
                aty[i] = (int8_t)d[offset++];
            }
        }

        // Refinement AT pixels
        int ratx[2] = { 0, 0 };
        int raty[2] = { 0, 0 };
        if (sdRefAgg && !sdHuff) {
            int numRAT = (sdrTemplate == 0) ? 2 : 0;
            for (int i = 0; i < numRAT; i++) {
                ratx[i] = (int8_t)d[offset++];
                raty[i] = (int8_t)d[offset++];
            }
        }

        uint32_t sdNumExSyms = (d[offset] << 24) | (d[offset + 1] << 16) |
                               (d[offset + 2] << 8) | d[offset + 3];
        offset += 4;

        uint32_t sdNumNewSyms = (d[offset] << 24) | (d[offset + 1] << 16) |
                                (d[offset + 2] << 8) | d[offset + 3];
        offset += 4;

        // Collect input symbols from referred-to segments
        std::vector<Jbig2Bitmap> inputSymbols;
        for (uint32_t refSeg : seg.referredTo) {
            auto it = segmentSymbols.find(refSeg);
            if (it != segmentSymbols.end()) {
                for (const auto& sym : it->second)
                    inputSymbols.push_back(sym);
            }
        }

        uint32_t numInputSyms = (uint32_t)inputSymbols.size();

        ArithDecoder arith(d + offset, seg.dataLength - offset);

        // Persistent integer contexts (must survive across all symbols!)
        ArithCtx iadhCtx[512] = {};
        ArithCtx iadwCtx[512] = {};
        ArithCtx iaexCtx[512] = {};
        ArithCtx iaaiCtx[512] = {};
        ArithCtx iardxCtx[512] = {};
        ArithCtx iardyCtx[512] = {};

        // Persistent IAID context - grows as symbols are added
        // Max possible code len we'll need
        int maxCodeLen = calcCodeLen(numInputSyms + sdNumNewSyms);
        std::vector<ArithCtx> iaidCtx((size_t)1 << (maxCodeLen + 1));

        // Shared contexts for embedded text regions (SDREFAGG, aggCount>1)
        // Created once at SD level, reused across ALL aggregate symbols (like jbig2dec)
        ArithCtx trIadtCtx[512] = {};
        ArithCtx trIafsCtx[512] = {};
        ArithCtx trIadsCtx[512] = {};
        ArithCtx trIaitCtx[512] = {};
        ArithCtx trIariCtx[512] = {};
        ArithCtx trIardwCtx[512] = {};
        ArithCtx trIardhCtx[512] = {};
        ArithCtx trIardxCtx[512] = {};
        ArithCtx trIardyCtx[512] = {};
        std::vector<ArithCtx> trIaidCtx((size_t)1 << (maxCodeLen + 1));

        // Shared GB_stats contexts across all individual symbol decodes (like jbig2dec)
        int gbCxSize = (sdTemplate == 0) ? 65536 : ((sdTemplate == 1) ? 8192 : 1024);
        std::vector<ArithCtx> gbStats(gbCxSize);

        // Shared GR_stats contexts across all refinement region decodes (like jbig2dec)
        int grCxSize = (sdrTemplate == 0) ? 8192 : 1024;
        std::vector<ArithCtx> grStats(grCxSize);

        // Decode new symbols
        std::vector<Jbig2Bitmap> newSymbols;
        int32_t heightClassHeight = 0;

        while (newSymbols.size() < sdNumNewSyms) {
            int32_t deltaHeight;
            if (!arith.decodeInteger(iadhCtx, deltaHeight)) {
                break;
            }
            heightClassHeight += deltaHeight;

            int32_t symbolWidth = 0;
            int32_t totalWidth = 0;
            int heightClassStart = (int)newSymbols.size();

            int safetyCount = 0;
            while (true) {
                if (++safetyCount > 1000) {
                    break;
                }
                int32_t deltaWidth;
                if (!arith.decodeInteger(iadwCtx, deltaWidth)) {
                    break;
                }
                symbolWidth += deltaWidth;

                if (symbolWidth <= 0 || heightClassHeight <= 0) {
                    Jbig2Bitmap empty;
                    empty.allocate(1, 1);
                    newSymbols.push_back(empty);
                    continue;
                }

                totalWidth += symbolWidth;

                if (sdRefAgg) {
                    int32_t aggCount;
                    arith.decodeInteger(iaaiCtx, aggCount);

                    if (aggCount == 1) {
                        // Single symbol refinement
                        // Uses shared trIaidCtx (same as jbig2dec's tparams.IAID)
                        int codeLen = maxCodeLen;

                        uint32_t symID = arith.decodeIAID(trIaidCtx.data(), codeLen);

                        int32_t rdx = 0, rdy = 0;
                        arith.decodeInteger(trIardxCtx, rdx);
                        arith.decodeInteger(trIardyCtx, rdy);


                        Jbig2Bitmap refBm;
                        if (symID < numInputSyms) {
                            refBm = inputSymbols[symID];
                        } else if (symID - numInputSyms < newSymbols.size()) {
                            refBm = newSymbols[symID - numInputSyms];
                        } else {
                            refBm.allocate(symbolWidth, heightClassHeight);
                        }

                        Jbig2Bitmap sym = decodeRefinementRegion(
                            arith, symbolWidth, heightClassHeight,
                            refBm, rdx, rdy, sdrTemplate, ratx, raty, grStats.data());
                        newSymbols.push_back(std::move(sym));
                    }
                    else {
                        // Multiple symbol aggregation (aggCount > 1)
                        // Decode an embedded text region per spec 6.5.8.2
                        // SBHUFF=0, SBREFINE=1, SBSTRIPS=1, TRANSPOSED=0, REFCORNER=TOPLEFT
                        // SBDSOFFSET=0, SBDEFPIXEL=0, SBCOMBOP=OR
                        // Uses SHARED contexts from SD level (like jbig2dec: created once, reused across all agg symbols)
                        int codeLen2 = maxCodeLen;

                        Jbig2Bitmap aggBitmap;
                        aggBitmap.allocate(symbolWidth, heightClassHeight);

                        // Build symbol list: input + new so far
                        std::vector<const Jbig2Bitmap*> aggSymbols;
                        for (size_t si = 0; si < inputSymbols.size(); si++)
                            aggSymbols.push_back(&inputSymbols[si]);
                        for (size_t si = 0; si < newSymbols.size(); si++)
                            aggSymbols.push_back(&newSymbols[si]);

                        // Decode initial DT (using shared trIadtCtx)
                        int32_t idt;
                        arith.decodeInteger(trIadtCtx, idt);
                        int32_t stripT2 = -idt; // SBSTRIPS=1

                        int32_t firstS2 = 0, curS2 = 0;
                        uint32_t instDecoded = 0;

                        // Embedded text region decode loop - matches jbig2dec structure exactly
                        // jbig2dec: outer while checks NINSTANCES < SBNUMINSTANCES
                        // inner for(;;) decodes DFS/IDS at top, then IAID/IARI/place at bottom
                        while (instDecoded < (uint32_t)aggCount) {
                            // Decode DT for strip
                            int32_t dtStrip;
                            arith.decodeInteger(trIadtCtx, dtStrip);
                            stripT2 += dtStrip;

                            bool firstSymInStrip = true;

                            for (;;) {
                                // (3c.i/ii) Decode DFS or IDS
                                if (firstSymInStrip) {
                                    int32_t dfs;
                                    if (!arith.decodeInteger(trIafsCtx, dfs)) break;
                                    firstS2 += dfs;
                                    curS2 = firstS2;
                                    firstSymInStrip = false;
                                } else {
                                    // jbig2dec: decode IDS even after last instance (OOB ends strip)
                                    int32_t ids;
                                    if (!arith.decodeInteger(trIadsCtx, ids)) {
                                        break;
                                    }
                                    curS2 += ids; // dsOffset=0
                                }

                                // (3c.iii) CURT = 0 since SBSTRIPS=1
                                int curT2 = stripT2;

                                // (3c.iv) Decode symbol ID
                                uint32_t sid = arith.decodeIAID(trIaidCtx.data(), codeLen2);

                                const Jbig2Bitmap* useSym2 = nullptr;
                                Jbig2Bitmap refinedSym2;
                                if (sid < aggSymbols.size()) {
                                    useSym2 = aggSymbols[sid];
                                } else {
                                    break;
                                }

                                // (3c.v) SBREFINE=1: decode RI (refinement indicator)
                                int32_t ri2 = 0;
                                arith.decodeInteger(trIariCtx, ri2);

                                if (ri2) {
                                    // Refinement: decode RDW, RDH, RDX, RDY
                                    int32_t rdw2 = 0, rdh2 = 0, rdx2 = 0, rdy2 = 0;
                                    arith.decodeInteger(trIardwCtx, rdw2);
                                    arith.decodeInteger(trIardhCtx, rdh2);
                                    arith.decodeInteger(trIardxCtx, rdx2);
                                    arith.decodeInteger(trIardyCtx, rdy2);
                                    int refW2 = useSym2->width + rdw2;
                                    int refH2 = useSym2->height + rdh2;
                                    if (refW2 > 0 && refH2 > 0) {
                                        int refDx2 = (rdw2 >> 1) + rdx2;
                                        int refDy2 = (rdh2 >> 1) + rdy2;
                                        refinedSym2 = decodeRefinementRegion(
                                            arith, refW2, refH2, *useSym2, refDx2, refDy2,
                                            sdrTemplate, ratx, raty, grStats.data());
                                        useSym2 = &refinedSym2;
                                    }
                                }

                                // (3c.viii/ix) TOPLEFT, not transposed: place at (curS, curT)
                                aggBitmap.compositeFrom(*useSym2, curS2, curT2, 0); // OR

                                // (3c.x) Post-placement: TOPLEFT, not transposed → curS += width - 1
                                curS2 += useSym2->width - 1;

                                // (3c.xi) Count instance
                                instDecoded++;
                            }
                        }

                        newSymbols.push_back(std::move(aggBitmap));
                    }
                }
                else if (sdHuff) {
                    // SDHUFF=1 and SDREFAGG=0: placeholder, will be replaced by collective bitmap below
                    Jbig2Bitmap placeholder;
                    placeholder.allocate(symbolWidth, heightClassHeight);
                    newSymbols.push_back(placeholder);
                }
                else {
                    // SDHUFF=0 and SDREFAGG=0: decode INDIVIDUAL symbol bitmap
                    // Uses shared gbStats contexts across all symbol decodes (like jbig2dec)
                    Jbig2Bitmap sym = decodeGenericRegion(
                        arith, symbolWidth, heightClassHeight,
                        sdTemplate, false, atx, aty, gbStats.data());
                    newSymbols.push_back(std::move(sym));
                }

                // Do NOT break early here! jbig2dec always consumes the IADW OOB
                // at the end of each height class. Breaking early would skip the OOB,
                // shifting the arithmetic decoder state and corrupting IAEX decode.
            }

            // Collective bitmap: only for SDHUFF=1 and SDREFAGG=0
            if (sdHuff && !sdRefAgg && (int)newSymbols.size() > heightClassStart) {
                int numInClass = (int)newSymbols.size() - heightClassStart;
                if (totalWidth > 0 && heightClassHeight > 0) {
                    Jbig2Bitmap collective = decodeGenericRegion(
                        arith, totalWidth, heightClassHeight,
                        sdTemplate, false, atx, aty);


                    int xOff = 0;
                    for (int i = heightClassStart; i < (int)newSymbols.size(); i++) {
                        int sw = newSymbols[i].width;
                        Jbig2Bitmap sym;
                        sym.allocate(sw, heightClassHeight);
                        for (int sy = 0; sy < heightClassHeight; sy++)
                            for (int sx = 0; sx < sw; sx++)
                                sym.setPixel(sx, sy, collective.getPixel(xOff + sx, sy));
                        newSymbols[i] = std::move(sym);
                        xOff += sw;
                    }
                }
            }
        }

        // Export symbols (Section 6.5.5)

        std::vector<Jbig2Bitmap> allSymbols;
        allSymbols.reserve(inputSymbols.size() + newSymbols.size());
        for (auto& s : inputSymbols) allSymbols.push_back(s);
        for (auto& s : newSymbols) allSymbols.push_back(s);


        exportedSymbols.clear();
        bool exportFlag = false;
        uint32_t idx = 0;

        while (idx < allSymbols.size()) {
            int32_t runLen;
            if (!arith.decodeInteger(iaexCtx, runLen)) {
                break;
            }
            // Clamp negative/oversized run lengths
            if (runLen < 0) runLen = 0;
            if ((uint32_t)runLen > allSymbols.size() - idx)
                runLen = (int32_t)(allSymbols.size() - idx);
            if (exportFlag) {
                for (int32_t i = 0; i < runLen && idx < allSymbols.size(); i++, idx++)
                    exportedSymbols.push_back(allSymbols[idx]);
            } else {
                idx += runLen;
            }
            exportFlag = !exportFlag;
        }

        if (exportedSymbols.empty()) {
            exportedSymbols = allSymbols;
        }

        return true;
    }

    // =====================================================
    // Decode Text Region (Section 6.4)
    // =====================================================
    static bool decodeTextRegion(
        const Jbig2Segment& seg,
        const std::map<uint32_t, std::vector<Jbig2Bitmap>>& segmentSymbols,
        Jbig2Bitmap& page)
    {
        if (seg.dataLength < 17) return false;

        const uint8_t* d = seg.data;

        // Region segment info (17 bytes)
        uint32_t regionW = (d[0] << 24) | (d[1] << 16) | (d[2] << 8) | d[3];
        uint32_t regionH = (d[4] << 24) | (d[5] << 16) | (d[6] << 8) | d[7];
        uint32_t regionX = (d[8] << 24) | (d[9] << 16) | (d[10] << 8) | d[11];
        uint32_t regionY = (d[12] << 24) | (d[13] << 16) | (d[14] << 8) | d[15];
        uint8_t regionFlags = d[16];
        int combinationOp = regionFlags & 7;

        int offset = 17;

        // Text region flags (2 bytes)
        uint16_t textFlags = (d[offset] << 8) | d[offset + 1];
        offset += 2;

        bool sbHuff = textFlags & 1;
        bool sbRefine = (textFlags >> 1) & 1;
        int logStrips = (textFlags >> 2) & 3;
        int sbStrips = 1 << logStrips;
        int refCorner = (textFlags >> 4) & 3;
        bool transposed = (textFlags >> 6) & 1;
        int sbCombOp = (textFlags >> 7) & 3;
        int sbDefPixel = (textFlags >> 9) & 1;
        int dsOffset = (textFlags >> 10) & 0x1F;
        if (dsOffset >= 16) dsOffset -= 32;
        int sbrTemplate = (textFlags >> 15) & 1;

        // Refinement AT pixels
        int ratx[2] = { 0, 0 };
        int raty[2] = { 0, 0 };
        if (sbRefine && !sbHuff) {
            int numRAT = (sbrTemplate == 0) ? 2 : 0;
            for (int i = 0; i < numRAT; i++) {
                ratx[i] = (int8_t)d[offset++];
                raty[i] = (int8_t)d[offset++];
            }
        }

        // Number of symbol instances
        uint32_t numInstances = (d[offset] << 24) | (d[offset + 1] << 16) |
                                (d[offset + 2] << 8) | d[offset + 3];
        offset += 4;

        // Collect symbols
        std::vector<Jbig2Bitmap> symbols;
        for (uint32_t refSeg : seg.referredTo) {
            auto it = segmentSymbols.find(refSeg);
            if (it != segmentSymbols.end()) {
                for (const auto& sym : it->second)
                    symbols.push_back(sym);
            }
        }

        uint32_t numSyms = (uint32_t)symbols.size();
        if (numSyms == 0) return false;

        int symbolCodeLen = calcCodeLen(numSyms);

        // Create text region bitmap
        Jbig2Bitmap region;
        region.allocate(regionW, regionH);
        region.fill(sbDefPixel);

        ArithDecoder arith(d + offset, seg.dataLength - offset);

        ArithCtx iadtCtx[512] = {};
        ArithCtx iafsCtx[512] = {};
        ArithCtx iadsCtx[512] = {};
        ArithCtx iaitCtx[512] = {};
        std::vector<ArithCtx> iaidCtx((size_t)1 << (symbolCodeLen + 1));
        ArithCtx iardwCtx[512] = {};
        ArithCtx iardhCtx[512] = {};
        ArithCtx iardxCtx[512] = {};
        ArithCtx iardyCtx[512] = {};
        ArithCtx iariCtx[512] = {};

        // Shared GR_stats for all inline refinements within this text region (like jbig2dec)
        int grCxSize = (sbrTemplate == 0) ? 8192 : 1024;
        std::vector<ArithCtx> trGrStats(grCxSize);

        // Decode initial STRIPT (step 1 - separate from strip loop)
        int32_t dt;
        arith.decodeInteger(iadtCtx, dt);
        int32_t stripT = -dt * sbStrips;

        int32_t firstS = 0;
        int32_t curS = 0;
        uint32_t instancesDecoded = 0;

        // jbig2dec loop structure: outer while + inner for(;;) with DFS/IDS at top
        while (instancesDecoded < numInstances) {
            // (step 2) Decode DT for this strip
            int32_t dtStrip;
            arith.decodeInteger(iadtCtx, dtStrip);
            stripT += dtStrip * sbStrips;

            bool firstSymInStrip = true;

            for (;;) {
                // (3c.i/ii) Decode DFS or IDS
                if (firstSymInStrip) {
                    int32_t dfs;
                    if (!arith.decodeInteger(iafsCtx, dfs)) break;
                    firstS += dfs;
                    curS = firstS;
                    firstSymInStrip = false;
                } else {
                    int32_t ids;
                    if (!arith.decodeInteger(iadsCtx, ids)) {
                        break;
                    }
                    curS += ids + dsOffset;
                }

                // (3c.iii) T offset within strip
                int32_t tt = 0;
                if (sbStrips > 1) {
                    arith.decodeInteger(iaitCtx, tt);
                }
                int curT = stripT + tt;

                // (3c.iv) Decode symbol ID
                uint32_t symID = arith.decodeIAID(iaidCtx.data(), symbolCodeLen);
                if (symID >= numSyms) symID = 0;

                const Jbig2Bitmap& sym = symbols[symID];

                // (3c.v) Inline refinement
                Jbig2Bitmap refinedSym;
                const Jbig2Bitmap* useSym = &sym;
                if (sbRefine) {
                    int32_t ri;
                    arith.decodeInteger(iariCtx, ri);
                    if (ri) {
                        // Decode all 4 refinement deltas: RDW, RDH, RDX, RDY (per spec 6.4.11)
                        int32_t rdw = 0, rdh = 0, rdx = 0, rdy = 0;
                        arith.decodeInteger(iardwCtx, rdw);
                        arith.decodeInteger(iardhCtx, rdh);
                        arith.decodeInteger(iardxCtx, rdx);
                        arith.decodeInteger(iardyCtx, rdy);
                        int refW = sym.width + rdw;
                        int refH = sym.height + rdh;
                        int refDx = (rdw >> 1) + rdx;
                        int refDy = (rdh >> 1) + rdy;
                        if (refW > 0 && refH > 0) {
                            refinedSym = decodeRefinementRegion(
                                arith, refW, refH,
                                sym, refDx, refDy, sbrTemplate, ratx, raty, trGrStats.data());
                            useSym = &refinedSym;
                        }
                    }
                }

                // REFCORNER mapping per ITU T.88 Table 2 (matches jbig2dec):
                // 0 = BOTTOMLEFT, 1 = TOPLEFT, 2 = BOTTOMRIGHT, 3 = TOPRIGHT

                // (3c.vi) Pre-placement: BOTTOMRIGHT(2) and TOPRIGHT(3) only
                if (!transposed) {
                    if (refCorner > 1) // BOTTOMRIGHT or TOPRIGHT
                        curS += useSym->width - 1;
                } else {
                    if (!(refCorner & 1)) // BOTTOMLEFT or BOTTOMRIGHT
                        curS += useSym->height - 1;
                }

                // (3c.vii/viii) Calculate position
                int px, py;
                if (!transposed) {
                    switch (refCorner) {
                        case 0: // BOTTOMLEFT
                            px = curS; py = curT - useSym->height + 1; break;
                        case 1: // TOPLEFT
                            px = curS; py = curT; break;
                        case 2: // BOTTOMRIGHT
                            px = curS - useSym->width + 1; py = curT - useSym->height + 1; break;
                        case 3: // TOPRIGHT
                            px = curS - useSym->width + 1; py = curT; break;
                        default:
                            px = curS; py = curT; break;
                    }
                } else {
                    switch (refCorner) {
                        case 0: // BOTTOMLEFT
                            px = curT; py = curS - useSym->height + 1; break;
                        case 1: // TOPLEFT
                            px = curT; py = curS; break;
                        case 2: // BOTTOMRIGHT
                            px = curT - useSym->width + 1; py = curS - useSym->height + 1; break;
                        case 3: // TOPRIGHT
                            px = curT - useSym->width + 1; py = curS; break;
                        default:
                            px = curT; py = curS; break;
                    }
                }

                // (3c.ix) Composite
                region.compositeFrom(*useSym, px, py, sbCombOp);

                // (3c.x) Post-placement: BOTTOMLEFT(0) and TOPLEFT(1) only
                if (!transposed) {
                    if (refCorner < 2) // BOTTOMLEFT or TOPLEFT
                        curS += useSym->width - 1;
                } else {
                    if (refCorner & 1) // TOPLEFT or TOPRIGHT
                        curS += useSym->height - 1;
                }

                // (3c.xi) Count instance
                instancesDecoded++;
                if (instancesDecoded >= numInstances) break;
            }
        }

        // Composite region onto page
        page.compositeFrom(region, regionX, regionY, combinationOp);

        return true;
    }

    // =====================================================
    // Main JBIG2 Decoder
    // =====================================================
    bool Jbig2Decoder::decode(
        const std::vector<uint8_t>& data,
        const std::vector<uint8_t>& globals,
        std::vector<uint8_t>& output,
        int& outW, int& outH)
    {

        std::map<uint32_t, std::vector<Jbig2Bitmap>> segmentSymbols;
        std::map<uint32_t, Jbig2Bitmap> segmentResults; // Store decoded region results for refinement references
        Jbig2Bitmap page;
        bool pageAllocated = false;
        int defaultPixel = 0;
        int pageCombOp = 0;

        // Helper to grow page for striped pages with initially unknown height
        auto growPage = [&](int needW, int needH) {
            if (!pageAllocated) return;
            if (needH <= page.height && needW <= page.width) return;
            int newW = std::max(page.width, needW);
            int newH = std::max(page.height, needH);
            Jbig2Bitmap newPage;
            newPage.allocate(newW, newH);
            newPage.fill(defaultPixel);
            // Copy old page data
            for (int y = 0; y < page.height; y++)
                for (int x = 0; x < page.width; x++)
                    newPage.setPixel(x, y, page.getPixel(x, y));
            page = std::move(newPage);
            outH = newH;
        };

        auto processSegments = [&](const uint8_t* streamData, size_t streamLen) -> bool {
            Jbig2BitReader reader(streamData, streamLen);

            while (reader.remaining() > 10) {
                Jbig2Segment seg;
                size_t posBefore = reader.bytePosition();
                if (!parseSegmentHeader(reader, seg)) {
                    break;
                }


                switch (seg.type) {
                    case SEG_PAGE_INFO: {
                        if (seg.dataLength >= 19) {
                            const uint8_t* pd = seg.data;
                            outW = (pd[0] << 24) | (pd[1] << 16) | (pd[2] << 8) | pd[3];
                            outH = (pd[4] << 24) | (pd[5] << 16) | (pd[6] << 8) | pd[7];
                            pageCombOp = pd[16] & 7;           // bits 0-2: default combination operator
                            // Match jbig2dec: use (flags & 4) for default pixel value
                            // jbig2dec reads bit 2 of flags (part of combOp) as defPixel
                            defaultPixel = (pd[16] & 4) ? 1 : 0;

                            if (outW > 0 && outW < 65536) {
                                if ((uint32_t)outH == 0xFFFFFFFF) {
                                    // Striped page with unknown height - start with 0 and grow
                                    outH = 0;
                                    page.allocate(outW, 1); // minimal allocation
                                    page.fill(defaultPixel);
                                    pageAllocated = true;
                                } else if (outH > 0 && outH < 65536) {
                                    page.allocate(outW, outH);
                                    page.fill(defaultPixel);
                                    pageAllocated = true;
                                }
                            }
                        }
                        break;
                    }

                    case SEG_SYMBOL_DICT: {
                        std::vector<Jbig2Bitmap> exported;
                        if (decodeSymbolDictionary(seg, segmentSymbols, exported)) {
                            segmentSymbols[seg.number] = std::move(exported);
                        }
                        break;
                    }

                    case SEG_TEXT_REGION_IMM:
                    case SEG_TEXT_REGION_IMM_LOSSLESS: {
                        if (pageAllocated) {
                            if (seg.dataLength >= 17) {
                                uint32_t trW = ((uint32_t)seg.data[0]<<24)|((uint32_t)seg.data[1]<<16)|((uint32_t)seg.data[2]<<8)|seg.data[3];
                                uint32_t trH = ((uint32_t)seg.data[4]<<24)|((uint32_t)seg.data[5]<<16)|((uint32_t)seg.data[6]<<8)|seg.data[7];
                                uint32_t trX = ((uint32_t)seg.data[8]<<24)|((uint32_t)seg.data[9]<<16)|((uint32_t)seg.data[10]<<8)|seg.data[11];
                                uint32_t trY = ((uint32_t)seg.data[12]<<24)|((uint32_t)seg.data[13]<<16)|((uint32_t)seg.data[14]<<8)|seg.data[15];
                                growPage(trX + trW, trY + trH);
                            }
                            decodeTextRegion(seg, segmentSymbols, page);
                        }
                        break;
                    }

                    case SEG_PATTERN_DICT: {
                        // Pattern Dictionary (Section 6.7 / 7.4.4)
                        if (seg.dataLength < 7) break;
                        const uint8_t* pd = seg.data;
                        uint8_t hdFlags = pd[0];
                        bool hdMMR = hdFlags & 1;
                        int hdTemplate = (hdFlags >> 1) & 3;
                        int hdPW = pd[1];
                        int hdPH = pd[2];
                        uint32_t grayMax = ((uint32_t)pd[3] << 24) | ((uint32_t)pd[4] << 16) |
                                           ((uint32_t)pd[5] << 8) | pd[6];

                        int off = 7;
                        // Pattern dict does NOT have AT pixels in header (per jbig2dec).
                        // Arithmetic data starts immediately at offset 7.
                        // jbig2dec uses AT[0] = (-HDPW, 0) for pattern context, rest are defaults.
                        // For template 0: gbat[0]=-HDPW, gbat[1]=0
                        // For template 1: gbat[0]=(HDTEMPLATE<=1?3:2), gbat[1]=-1
                        // jbig2dec ALWAYS uses AT[0] = (-HDPW, 0) for pattern dict,
                        // regardless of template. Other AT pixels use defaults.
                        int patAtx[4] = { -(int)hdPW, -3, 2, -2 };
                        int patAty[4] = { 0, -1, -2, -2 };


                        int collectiveW = (int)(grayMax + 1) * hdPW;
                        int collectiveH = hdPH;

                        Jbig2Bitmap collective;
                        if (hdMMR) {
                            collective = decodeMMRRegion(pd + off, seg.dataLength - off, collectiveW, collectiveH);
                        } else {
                            ArithDecoder patArith(pd + off, seg.dataLength - off);
                            collective = decodeGenericRegion(
                                patArith, collectiveW, collectiveH, hdTemplate, false, patAtx, patAty);
                        }

                        // Split collective bitmap into individual patterns
                        std::vector<Jbig2Bitmap> patterns;
                        for (uint32_t i = 0; i <= grayMax; i++) {
                            Jbig2Bitmap pat;
                            pat.allocate(hdPW, hdPH);
                            for (int y = 0; y < hdPH; y++)
                                for (int x = 0; x < hdPW; x++)
                                    pat.setPixel(x, y, collective.getPixel((int)i * hdPW + x, y));
                            patterns.push_back(std::move(pat));
                        }

                        segmentSymbols[seg.number] = std::move(patterns);
                        break;
                    }

                    case SEG_HALFTONE_REGION_INTERMEDIATE:
                    case SEG_HALFTONE_REGION_IMM:
                    case SEG_HALFTONE_REGION_IMM_LOSSLESS: {
                        // Halftone Region (Section 6.6 / 7.4.5)
                        if (seg.dataLength < 38) break;
                        const uint8_t* hd = seg.data;

                        // Region segment info (17 bytes)
                        uint32_t regionW = ((uint32_t)hd[0] << 24) | ((uint32_t)hd[1] << 16) | ((uint32_t)hd[2] << 8) | hd[3];
                        uint32_t regionH = ((uint32_t)hd[4] << 24) | ((uint32_t)hd[5] << 16) | ((uint32_t)hd[6] << 8) | hd[7];
                        uint32_t regionX = ((uint32_t)hd[8] << 24) | ((uint32_t)hd[9] << 16) | ((uint32_t)hd[10] << 8) | hd[11];
                        uint32_t regionY = ((uint32_t)hd[12] << 24) | ((uint32_t)hd[13] << 16) | ((uint32_t)hd[14] << 8) | hd[15];
                        uint8_t regionFlags = hd[16];
                        int htRegionCombOp = regionFlags & 7;

                        // Halftone flags
                        uint8_t htFlags = hd[17];
                        bool hMMR = htFlags & 1;
                        int htTemplate = (htFlags >> 1) & 3;
                        bool hEnableSkip = (htFlags >> 4) & 1;
                        int hCombOp = (htFlags >> 5) & 3;
                        int hDefPixel = (htFlags >> 7) & 1;

                        // Grid parameters
                        uint32_t hgW = ((uint32_t)hd[18] << 24) | ((uint32_t)hd[19] << 16) | ((uint32_t)hd[20] << 8) | hd[21];
                        uint32_t hgH = ((uint32_t)hd[22] << 24) | ((uint32_t)hd[23] << 16) | ((uint32_t)hd[24] << 8) | hd[25];
                        int32_t hgX = (int32_t)(((uint32_t)hd[26] << 24) | ((uint32_t)hd[27] << 16) | ((uint32_t)hd[28] << 8) | hd[29]);
                        int32_t hgY = (int32_t)(((uint32_t)hd[30] << 24) | ((uint32_t)hd[31] << 16) | ((uint32_t)hd[32] << 8) | hd[33]);
                        uint16_t hrX = ((uint16_t)hd[34] << 8) | hd[35];
                        uint16_t hrY = ((uint16_t)hd[36] << 8) | hd[37];

                        int off = 38;
                        // NOTE: Despite the spec mentioning AT pixels in halftone headers,
                        // test files do NOT include them. Arith data starts right at offset 38.
                        // Use default AT pixels for gray-scale bit plane decoding.
                        int htAtx[4] = { (htTemplate <= 1 ? 3 : 2), -3, 2, -2 };
                        int htAty[4] = { -1, -1, -2, -2 };


                        // Get patterns from referred segment
                        std::vector<Jbig2Bitmap> patterns;
                        for (uint32_t refSeg : seg.referredTo) {
                            auto it = segmentSymbols.find(refSeg);
                            if (it != segmentSymbols.end()) {
                                patterns = it->second;
                                break;
                            }
                        }

                        uint32_t numPats = (uint32_t)patterns.size();
                        if (numPats == 0) {
                            break;
                        }

                        // HBPP = ceil(log2(HNUMPATS))
                        int hBPP = 0;
                        while ((1u << hBPP) < numPats) hBPP++;

                        // Create region bitmap
                        Jbig2Bitmap htRegion;
                        htRegion.allocate(regionW, regionH);
                        htRegion.fill(hDefPixel);

                        // Decode bit planes (MSB to LSB) with Gray-code to binary conversion
                        std::vector<Jbig2Bitmap> planes(hBPP);

                        if (hMMR) {
                            // MMR decoding of bit planes
                            size_t mmrOff = 0;
                            const uint8_t* mmrData = hd + off;
                            size_t mmrLen = seg.dataLength - off;
                            for (int j = hBPP - 1; j >= 0; j--) {
                                size_t consumed = 0;
                                planes[j] = decodeMMRRegionWithConsumed(
                                    mmrData + mmrOff, mmrLen - mmrOff, hgW, hgH, &consumed);
                                mmrOff += consumed;
                                // Gray-code to binary: XOR with previous (higher) plane
                                if (j < hBPP - 1) {
                                    for (uint32_t py = 0; py < hgH; py++)
                                        for (uint32_t px = 0; px < hgW; px++)
                                            planes[j].setPixel(px, py,
                                                planes[j].getPixel(px, py) ^ planes[j + 1].getPixel(px, py));
                                }
                            }
                        } else {
                            // Arithmetic decoding of bit planes
                            ArithDecoder htArith(hd + off, seg.dataLength - off);

                            // Shared GB_stats across all bit planes (like jbig2dec)
                            int gbCxSize = (htTemplate == 0) ? 65536 : ((htTemplate == 1) ? 8192 : 1024);
                            std::vector<ArithCtx> htGbStats(gbCxSize);

                            for (int j = hBPP - 1; j >= 0; j--) {
                                planes[j] = decodeGenericRegion(
                                    htArith, hgW, hgH, htTemplate, false, htAtx, htAty, htGbStats.data());
                                // Gray-code to binary: XOR with previous (higher) plane
                                if (j < hBPP - 1) {
                                    for (uint32_t py = 0; py < hgH; py++)
                                        for (uint32_t px = 0; px < hgW; px++)
                                            planes[j].setPixel(px, py,
                                                planes[j].getPixel(px, py) ^ planes[j + 1].getPixel(px, py));
                                }
                            }
                        }

                        // Render: for each grid cell, look up pattern and composite
                        for (uint32_t m = 0; m < hgH; m++) {
                            for (uint32_t n = 0; n < hgW; n++) {
                                // Combine bit planes to get gray value
                                uint32_t grayVal = 0;
                                for (int j = 0; j < hBPP; j++)
                                    grayVal |= (uint32_t)planes[j].getPixel(n, m) << j;

                                if (grayVal >= numPats) grayVal = numPats - 1;

                                // Grid position per jbig2dec (mg=row, ng=col):
                                // x = (HGX + mg*HRY + ng*HRX) >> 8
                                // y = (HGY + mg*HRX - ng*HRY) >> 8
                                int x = (hgX + (int32_t)m * (int32_t)hrY + (int32_t)n * (int32_t)hrX) >> 8;
                                int y = (hgY + (int32_t)m * (int32_t)hrX - (int32_t)n * (int32_t)hrY) >> 8;

                                htRegion.compositeFrom(patterns[grayVal], x, y, hCombOp);
                            }
                        }


                        // Composite onto page (or store for intermediate)
                        // jbig2dec always composites halftone to page regardless of intermediate flag
                        if (pageAllocated) {
                            growPage(regionX + regionW, regionY + regionH);
                            page.compositeFrom(htRegion, regionX, regionY, htRegionCombOp);
                        }
                        segmentResults[seg.number] = std::move(htRegion);
                        break;
                    }

                    case SEG_GENERIC_REGION_INTERMEDIATE:
                    case SEG_GENERIC_REGION_IMM:
                    case SEG_GENERIC_REGION_IMM_LOSSLESS: {
                        if (seg.dataLength > 17) {
                            const uint8_t* gd = seg.data;
                            uint32_t rw = (gd[0] << 24) | (gd[1] << 16) | (gd[2] << 8) | gd[3];
                            uint32_t rh = (gd[4] << 24) | (gd[5] << 16) | (gd[6] << 8) | gd[7];
                            uint32_t rx = (gd[8] << 24) | (gd[9] << 16) | (gd[10] << 8) | gd[11];
                            uint32_t ry = (gd[12] << 24) | (gd[13] << 16) | (gd[14] << 8) | gd[15];
                            uint8_t rFlags = gd[16];
                            int combOp = rFlags & 7;

                            uint8_t genFlags = gd[17];
                            bool mmr = genFlags & 1;
                            int tplID = (genFlags >> 1) & 3;
                            bool tpgdOn = (genFlags >> 3) & 1;

                            Jbig2Bitmap genRegion;
                            if (mmr) {
                                // MMR mode: no AT pixels in the data stream
                                int off = 18;
                                genRegion = decodeMMRRegion(gd + off, seg.dataLength - off, rw, rh);
                            } else {
                                // Arithmetic mode: read AT pixels then decode
                                int off = 18;
                                int nAT = (tplID == 0) ? 4 : 1;
                                int gAtx[4] = {3, -3, 2, -2};
                                int gAty[4] = {-1, -1, -2, -2};
                                for (int i = 0; i < nAT; i++) {
                                    gAtx[i] = (int8_t)gd[off++];
                                    gAty[i] = (int8_t)gd[off++];
                                }

                                ArithDecoder genArith(gd + off, seg.dataLength - off);
                                genRegion = decodeGenericRegion(
                                    genArith, rw, rh, tplID, tpgdOn, gAtx, gAty);
                            }

                            bool isIntermediate = (seg.type == SEG_GENERIC_REGION_INTERMEDIATE);
                            if (!isIntermediate && pageAllocated) {
                                growPage(rx + rw, ry + rh);
                                page.compositeFrom(genRegion, rx, ry, combOp);
                            }
                            segmentResults[seg.number] = std::move(genRegion);
                        }
                        break;
                    }

                    case SEG_GENERIC_REFINE_INTERMEDIATE:
                    case SEG_GENERIC_REFINE_IMM:
                    case SEG_GENERIC_REFINE_IMM_LOSSLESS: {
                        if (seg.dataLength > 18 && !seg.referredTo.empty()) {
                            const uint8_t* gd = seg.data;
                            uint32_t rw = (gd[0] << 24) | (gd[1] << 16) | (gd[2] << 8) | gd[3];
                            uint32_t rh = (gd[4] << 24) | (gd[5] << 16) | (gd[6] << 8) | gd[7];
                            uint32_t rx = (gd[8] << 24) | (gd[9] << 16) | (gd[10] << 8) | gd[11];
                            uint32_t ry = (gd[12] << 24) | (gd[13] << 16) | (gd[14] << 8) | gd[15];
                            uint8_t rFlags = gd[16];
                            int combOp = rFlags & 7;

                            uint8_t grFlags = gd[17];
                            int grTemplate = grFlags & 1;
                            // bool tpgrOn = (grFlags >> 1) & 1; // typical prediction for refinement

                            int off = 18;
                            int ratx[2] = {-1, -1};
                            int raty[2] = {-1, -1};
                            if (grTemplate == 0) {
                                ratx[0] = (int8_t)gd[off++];
                                raty[0] = (int8_t)gd[off++];
                                ratx[1] = (int8_t)gd[off++];
                                raty[1] = (int8_t)gd[off++];
                            }

                            // Find reference bitmap from referred-to segment
                            uint32_t refSegNum = seg.referredTo[0];
                            const Jbig2Bitmap* refBitmap = nullptr;

                            // Check segmentResults first (generic region outputs)
                            auto resIt = segmentResults.find(refSegNum);
                            if (resIt != segmentResults.end()) {
                                refBitmap = &resIt->second;
                            }
                            // Also check page as fallback if ref segment was composited directly
                            if (!refBitmap) {
                                // Use page bitmap as reference
                                refBitmap = &page;
                            }


                            ArithDecoder refArith(gd + off, seg.dataLength - off);
                            Jbig2Bitmap refined = decodeRefinementRegion(
                                refArith, rw, rh, *refBitmap, 0, 0, grTemplate, ratx, raty);

                            bool isIntermediate = (seg.type == SEG_GENERIC_REFINE_INTERMEDIATE);
                            if (!isIntermediate && pageAllocated) {
                                growPage(rx + rw, ry + rh);
                                page.compositeFrom(refined, rx, ry, combOp);
                            }
                            segmentResults[seg.number] = std::move(refined);
                        }
                        break;
                    }

                    case SEG_END_OF_PAGE:
                    case SEG_END_OF_FILE:
                        break;

                    default:
                        break;
                }
            }
            return true;
        };

        if (!globals.empty()) {
            processSegments(globals.data(), globals.size());
        }

        processSegments(data.data(), data.size());

        if (!pageAllocated) {
            return false;
        }

        output = page.data;
        return true;
    }

} // namespace pdf
