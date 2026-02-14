#pragma once

#include <map>
#include <memory>
#include <vector>
#include "PdfObject.h"
#include "PdfLexer.h"

namespace pdf
{
    class PdfParser
    {
    public:
        PdfParser(const std::vector<uint8_t>& data);

        bool parse();  // PDF içindeki tüm indirect object'leri okumaya çalýþýr
        const std::map<int, PdfObjectPtr>& objects() const;
        PdfObjectPtr parseObjectAt(size_t offset);
        

    private:
        const std::vector<uint8_t>& _data;
        PdfLexer _lexer;
        std::map<int, PdfObjectPtr> _objects;

        PdfObjectPtr parseObject();
        PdfObjectPtr parseAtomicObject(const Token& tok);

        std::shared_ptr<PdfArray> parseArray();
        std::shared_ptr<PdfDictionary> parseDictionary();
        std::shared_ptr<PdfStream> parseStream(std::shared_ptr<PdfDictionary> dict);
    };
}
