#include "pch.h"
#include "PdfParser.h"
#include "PdfDebug.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <chrono>

namespace pdf
{
    PdfParser::PdfParser(const std::vector<uint8_t>& data)
        : _data(data), _lexer(data)
    {
    }

    bool PdfParser::parse()
    {
        LogDebug("PdfParser::parse() started");

        size_t safetyCounter = 0;
        const size_t MAX_ITERATIONS = 500000;

        auto startTime = std::chrono::steady_clock::now();
        const auto MAX_DURATION = std::chrono::seconds(30);

        while (true)
        {
            auto elapsed = std::chrono::steady_clock::now() - startTime;
            if (elapsed > MAX_DURATION)
            {
                LogDebug("ERROR: Parse timeout after 30 seconds");
                return false;
            }

            if (++safetyCounter > MAX_ITERATIONS)
            {
                LogDebug("ERROR: Max iterations reached: %zu", safetyCounter);
                break;
            }

            // ✅ Her 1000 iterasyonda log
            if (safetyCounter % 1000 == 0)
            {
                LogDebug("Parse iteration: %zu, objects found: %zu",
                    safetyCounter, _objects.size());
            }

            Token t = _lexer.peekToken();
            if (t.type == TokenType::EndOfFile)
                break;

            Token t1 = _lexer.nextToken();
            if (t1.type == TokenType::EndOfFile)
                break;
            if (t1.type != TokenType::Number)
                continue;

            Token t2 = _lexer.nextToken();
            if (t2.type != TokenType::Number)
                continue;

            Token t3 = _lexer.nextToken();
            if (t3.type != TokenType::Keyword || t3.text != "obj")
                continue;

            int objNum = std::atoi(t1.text.c_str());

            LogDebug("Parsing object %d at position %zu", objNum, _lexer.getPosition());

            PdfObjectPtr obj = parseObject();
            if (obj)
            {
                _objects[objNum] = obj;
            }

            Token endTok = _lexer.nextToken();
            int innerSafe = 0;
            while (!(endTok.type == TokenType::Keyword && endTok.text == "endobj"))
            {
                if (endTok.type == TokenType::EndOfFile)
                    break;
                if (++innerSafe > 100000)
                {
                    LogDebug("ERROR: Stuck in endobj search for object %d", objNum);
                    break;
                }
                endTok = _lexer.nextToken();
            }
        }

        LogDebug("PdfParser::parse() finished - found %zu objects", _objects.size());
        return true;
    }

    PdfObjectPtr PdfParser::parseObject()
    {
        Token tok = _lexer.nextToken();

        if (tok.type == TokenType::Delimiter && tok.text == "<<")
        {
            auto dict = parseDictionary();
            Token next = _lexer.peekToken();
            if (next.type == TokenType::Keyword && next.text == "stream")
            {
                _lexer.nextToken();
                return parseStream(dict);
            }
            return dict;
        }
        else if (tok.type == TokenType::Delimiter && tok.text == "[")
        {
            return parseArray();
        }
        return parseAtomicObject(tok);
    }

    PdfObjectPtr PdfParser::parseAtomicObject(const Token& tok)
    {
        switch (tok.type)
        {
        case TokenType::Number:
            return std::make_shared<PdfNumber>(std::atof(tok.text.c_str()));
        case TokenType::String:
            return std::make_shared<PdfString>(tok.text);
        case TokenType::HexString:
            return std::make_shared<PdfString>(tok.text);
        case TokenType::Name:
            return std::make_shared<PdfName>(tok.text);
        case TokenType::Keyword:
            if (tok.text == "null")  return std::make_shared<PdfNull>();
            if (tok.text == "true")  return std::make_shared<PdfBoolean>(true);
            if (tok.text == "false") return std::make_shared<PdfBoolean>(false);
            break;
        default: break;
        }
        return nullptr;
    }

    // ----------------------------------------------------------------
    // DÜZELTİLEN FONKSİYON: parseArray
    // ----------------------------------------------------------------
    std::shared_ptr<PdfArray> PdfParser::parseArray()
    {
        auto arr = std::make_shared<PdfArray>();
        int safety = 0;
        const int MAX_ITEMS = 50000; // ✅ Limit artırıldı

        while (safety++ < MAX_ITEMS)
        {
            Token t = _lexer.peekToken();
            if (t.type == TokenType::Delimiter && t.text == "]")
            {
                _lexer.nextToken();
                break;
            }
            if (t.type == TokenType::EndOfFile)
                break;

            Token tok = _lexer.nextToken();

            if (tok.type == TokenType::Delimiter && tok.text == "[")
            {
                arr->items.push_back(parseArray());
                continue;
            }

            if (tok.type == TokenType::Delimiter && tok.text == "<<")
            {
                arr->items.push_back(parseDictionary());
                continue;
            }

            if (tok.type == TokenType::Number)
            {
                size_t savePos = _lexer.getPosition();

                Token t2 = _lexer.nextToken();
                if (t2.type == TokenType::Number)
                {
                    Token t3 = _lexer.nextToken();
                    if (t3.type == TokenType::Keyword && t3.text == "R")
                    {
                        arr->items.push_back(
                            std::make_shared<PdfIndirectRef>(
                                std::atoi(tok.text.c_str()),
                                std::atoi(t2.text.c_str())
                            )
                        );
                        continue;
                    }
                }

                _lexer.setPosition(savePos);
                arr->items.push_back(parseAtomicObject(tok));
                continue;
            }

            arr->items.push_back(parseAtomicObject(tok));
        }

        return arr;
    }


    // ----------------------------------------------------------------
    // DÜZELTİLEN FONKSİYON: parseDictionary
    // ----------------------------------------------------------------
    std::shared_ptr<PdfDictionary> PdfParser::parseDictionary()
    {
        auto dict = std::make_shared<PdfDictionary>();
        int safety = 0;

        while (safety++ < 10000)
        {
            Token key = _lexer.nextToken();
            if (key.type == TokenType::Delimiter && key.text == ">>")
                break;

            if (key.type != TokenType::Name)
                break;

            Token val = _lexer.nextToken();

            if (val.type == TokenType::Delimiter && val.text == "<<")
            {
                dict->entries[key.text] = parseDictionary();
                continue;
            }

            if (val.type == TokenType::Delimiter && val.text == "[")
            {
                dict->entries[key.text] = parseArray();
                continue;
            }

            if (val.type == TokenType::Number)
            {
                size_t savePos = _lexer.getPosition();

                Token t2 = _lexer.nextToken();
                if (t2.type == TokenType::Number)
                {
                    Token t3 = _lexer.nextToken();
                    if (t3.type == TokenType::Keyword && t3.text == "R")
                    {
                        dict->entries[key.text] =
                            std::make_shared<PdfIndirectRef>(
                                std::atoi(val.text.c_str()),
                                std::atoi(t2.text.c_str())
                            );
                        continue;
                    }
                }

                // ❗ Referans değil → geri sar
                _lexer.setPosition(savePos);
                dict->entries[key.text] = parseAtomicObject(val);
                continue;
            }

            dict->entries[key.text] = parseAtomicObject(val);
        }

        return dict;
    }



    std::shared_ptr<PdfStream> PdfParser::parseStream(std::shared_ptr<PdfDictionary> dict)
    {
        // "stream" keyword'ünden hemen sonra gelen \r\n boşluklarını atla
        while (_lexer.getPosition() < _data.size())
        {
            unsigned char c = _data[_lexer.getPosition()];
            if (c == '\n' || c == '\r')
                _lexer.setPosition(_lexer.getPosition() + 1);
            else
                break;
        }

        size_t pos = _lexer.getPosition();  // stream verisinin BAŞI
        size_t length = 0;

        // /Length veya Length anahtarından uzunluğu almaya çalış
        auto lenObj = dict->get("/Length");
        if (!lenObj)
            lenObj = dict->get("Length");

        if (lenObj)
        {
            auto num = std::dynamic_pointer_cast<PdfNumber>(lenObj);
            if (num && num->value > 0)
                length = static_cast<size_t>(num->value);
        }

        size_t newPos = 0;

        if (length > 0 && pos + length <= _data.size())
        {
            // Length güvenilir → direkt uzunluk kadar oku
            newPos = pos + length;
        }
        else
        {
            // Length yok ya da saçma → "endstream" kelimesini ara
            const std::string marker = "endstream";
            auto it = std::search(_data.begin() + pos, _data.end(),
                marker.begin(), marker.end());
            if (it != _data.end())
                newPos = static_cast<size_t>(it - _data.begin());
            else
                newPos = _data.size(); // dosya bozuksa sonuna kadar
        }

        // *** ÖNEMLİ: Artık stream ham baytlarını gerçekten okuyalım ***
        std::vector<uint8_t> bytes;
        if (newPos > pos && newPos <= _data.size())
        {
            bytes.assign(_data.begin() + pos, _data.begin() + newPos);
        }

        // Lexer konumunu endstream'in başına taşı
        _lexer.setPosition(newPos);

        // Eski koddaki "dummy" vektör yerine gerçek veriyi veriyoruz
        return std::make_shared<PdfStream>(dict, bytes);
    }


    const std::map<int, PdfObjectPtr>& PdfParser::objects() const
    {
        return _objects;
    }

    PdfObjectPtr PdfParser::parseObjectAt(size_t offset)
    {
        _lexer.setPosition(offset);
        Token t1 = _lexer.peekToken();
        if (t1.type == TokenType::Number)
        {
            _lexer.nextToken();
            Token t2 = _lexer.peekToken();
            if (t2.type == TokenType::Number)
            {
                _lexer.nextToken();
                Token t3 = _lexer.peekToken();
                if (t3.type == TokenType::Keyword && t3.text == "obj")
                {
                    _lexer.nextToken();
                }
            }
        }
        return parseObject();
    }
}