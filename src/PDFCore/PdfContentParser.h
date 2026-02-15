#pragma once
#include "PdfPath.h"
#include <vector>
#include <string>
#include <map>
#include <memory>
#include <stack>
#include <cstdint>

#include "PdfGraphicsState.h"
#include "PdfDocument.h"
#include "PdfObject.h"

namespace pdf
{
    struct PdfGradient;
    struct PdfPattern;

    class IPdfPainter;  // Use interface instead of concrete class

    class PdfContentParser
    {
    public:
        PdfContentParser(
            const std::vector<uint8_t>& streamData,
            IPdfPainter* painter,  // Changed: IPdfPainter* instead of PdfPainter*
            PdfDocument* doc,
            int pageIndex,
            std::map<std::string, PdfFontInfo>* fonts,
            const PdfGraphicsState& initialGs,
            const std::vector<std::shared_ptr<PdfDictionary>>& resourceStack
        );

        void parse();

        // ✅ Set inherited clipping state from parent (for Form XObjects)
        void setInheritedClipping(const PdfPath& clipPath, const PdfMatrix& clipCTM, bool evenOdd = false)
        {
            _inheritedClippingPath = clipPath;
            _inheritedClippingPathCTM = clipCTM;
            _hasInheritedClipping = true;
            _inheritedClippingEvenOdd = evenOdd;
        }

    private:
        bool eof() const;
        uint8_t peek() const;
        uint8_t get();
        void skipSpaces();
        void skipComment();

        double      readNumber();
        std::string readName();
        std::string readString();
        std::string readWord();

        double      popNumber(double def = 0.0);
        std::string popString();
        std::string popName();

        void parseToken();
        void handleOperator(const std::string& op);

        // path operators
        void op_m();
        void op_l();
        void op_re();
        void op_f();
        void op_S();
        void op_c();

        // graphics state
        void op_cm();
        void op_q();
        void op_Q();

        // text state
        void op_BT();
        void op_ET();
        void op_Tf();
        void op_TL();
        void op_Tm();
        void op_Td();
        void op_Tstar();
        void op_fill_stroke_evenodd();
        void op_fill_stroke();
        void op_f_evenodd();
        void op_h();
        void op_v();
        void op_y();
        void op_d();

        // text decode & draw
        std::wstring decodeText(const std::string& raw);
        void op_Tj();
        void op_TJ();

        // stroke state
        void op_w();
        void op_J();
        void op_j();
        void op_M();

        // color operators
        void op_CS(); void op_cs();
        void op_SC(); void op_sc();
        void op_SCN(); void op_scn();
        void op_G(); void op_g();
        void op_RG(); void op_rg();
        void op_K(); void op_k();
        void op_sh();

        // Color space resolution helpers
        int resolveColorSpaceType(const std::string& csName);
        void applyColorFromCS(const std::string& csName, double out[3]);

    private:
        PdfPath _clippingPath;
        PdfMatrix _clippingPathCTM;
        bool _hasClippingPath = false;
        bool _clippingEvenOdd = false;  // true if W* (even-odd), false if W (winding)
        std::stack<PdfPath> _clippingPathStack;
        std::stack<PdfMatrix> _clippingPathCTMStack;
        std::stack<bool> _hasClippingPathStack;
        std::stack<bool> _clippingEvenOddStack;

        // D2D clip layer count for current q/Q level (for nested clipping via W operator)
        int _clipLayerCount = 0;
        std::stack<int> _clipLayerCountStack;

        // SMask layer count for current q/Q level
        int _smaskLayerCount = 0;
        std::stack<int> _smaskLayerCountStack;

        // ✅ Inherited clipping from parent Form XObject
        // This should be applied IN ADDITION to any local clipping
        PdfPath _inheritedClippingPath;
        PdfMatrix _inheritedClippingPathCTM;
        bool _hasInheritedClipping = false;
        bool _inheritedClippingEvenOdd = false;

        bool _textBlockClipPushed = false;

        PdfPath _rectClippingPath;
        PdfMatrix _rectClippingPathCTM;
        bool _hasRectClipping = false;
        std::stack<PdfPath> _rectClippingPathStack;
        std::stack<PdfMatrix> _rectClippingPathCTMStack;
        std::stack<bool> _hasRectClippingStack;

        const std::vector<uint8_t>& _data;
        size_t _pos = 0;

        double _cpX = 0.0;
        double _cpY = 0.0;

        double _subpathStartX = 0.0;
        double _subpathStartY = 0.0;

        IPdfPainter* _painter = nullptr;  // Changed: IPdfPainter*
        PdfDocument* _doc = nullptr;
        int _pageIndex = 0;

        std::map<std::string, PdfFontInfo>* _fonts = nullptr;

        PdfGraphicsState _gs;
        std::stack<PdfGraphicsState> _gsStack;

        std::vector<std::shared_ptr<PdfObject>> _stack;

        PdfFontInfo* _currentFont = nullptr;

        std::vector<std::shared_ptr<PdfDictionary>> _resStack;
        std::shared_ptr<PdfDictionary> currentResources() const
        {
            if (_resStack.empty()) return nullptr;
            return _resStack.back();
        }

        std::shared_ptr<PdfDictionary> resolveDict(const std::shared_ptr<PdfObject>& o) const;
        std::shared_ptr<PdfObject> resolveObj(const std::shared_ptr<PdfObject>& o) const;

        void renderXObjectDo(const std::string& xName);
        PdfMatrix readMatrix6(const std::shared_ptr<PdfObject>& obj) const;

        // SMask: Render a Form XObject to luminosity bitmap
        bool renderFormToLuminosityMask(
            const std::shared_ptr<PdfStream>& formStream,
            std::vector<uint8_t>& outAlpha,
            int& outW, int& outH);

        bool resolvePatternToGradient(
            const std::string& patternName,
            PdfGradient& gradient,
            PdfMatrix& patternMatrix);

        bool resolvePattern(
            const std::string& patternName,
            PdfPattern& pattern);

        void renderPatternTile(
            const std::shared_ptr<PdfDictionary>& patternDict,
            PdfPattern& pattern);

        std::vector<PdfPathSegment> _currentPath;

        std::string _currentFillCS = "DeviceRGB";
        std::string _currentStrokeCS = "DeviceRGB";
    };

} // namespace pdf