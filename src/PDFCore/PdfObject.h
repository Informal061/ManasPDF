#pragma once
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

namespace pdf
{
    // PDF objesi türleri
    enum class PdfObjectType
    {
        Null,
        Boolean,
        Number,
        String,
        Name,
        Array,
        Dictionary,
        Stream,
        IndirectRef
    };

    // Tüm PDF objelerinin temel sýnýfý
    class PdfObject
    {
    public:
        virtual ~PdfObject() = default;
        virtual PdfObjectType type() const = 0;
    };

    // ---- Null ----
    class PdfNull : public PdfObject
    {
    public:
        PdfObjectType type() const override { return PdfObjectType::Null; }
    };

    // ---- Boolean ----
    class PdfBoolean : public PdfObject
    {
    public:
        bool value;
        PdfBoolean(bool v) : value(v) {}
        PdfObjectType type() const override { return PdfObjectType::Boolean; }
    };

    // ---- Number ----
    class PdfNumber : public PdfObject
    {
    public:
        double value;
        PdfNumber(double v) : value(v) {}
        PdfObjectType type() const override { return PdfObjectType::Number; }
    };

    // ---- String ----
    class PdfString : public PdfObject
    {
    public:
        std::string value;
        PdfString(const std::string& v) : value(v) {}
        PdfObjectType type() const override { return PdfObjectType::String; }
    };

    // ---- Name ----  (ör: /Type, /Catalog)
    class PdfName : public PdfObject
    {
    public:
        std::string value;
        PdfName(const std::string& v) : value(v) {}
        PdfObjectType type() const override { return PdfObjectType::Name; }
    };

    // ---- Array ----
    class PdfArray : public PdfObject
    {
    public:
        std::vector<std::shared_ptr<PdfObject>> items;
        PdfObjectType type() const override { return PdfObjectType::Array; }
    };

    // ---- Dictionary ----
    class PdfDictionary : public PdfObject
    {
    public:
        std::unordered_map<std::string, std::shared_ptr<PdfObject>> entries;

        PdfObjectType type() const override { return PdfObjectType::Dictionary; }

        std::shared_ptr<PdfObject> get(const std::string& key) const
        {
            auto it = entries.find(key);
            if (it != entries.end())
                return it->second;
            return nullptr;
        }
    };

    // ---- Stream ----
    class PdfStream : public PdfObject
    {
    public:
        std::shared_ptr<PdfDictionary> dict;
        std::vector<uint8_t> data; // raw stream bytes

        PdfStream(std::shared_ptr<PdfDictionary> d, std::vector<uint8_t> bytes)
            : dict(std::move(d)), data(std::move(bytes)) {
        }

        PdfObjectType type() const override { return PdfObjectType::Stream; }
    };

    // ---- Indirect Reference ----
    class PdfIndirectRef : public PdfObject
    {
    public:
        int objNum;
        int genNum;

        PdfIndirectRef(int o, int g) : objNum(o), genNum(g) {}
        PdfObjectType type() const override { return PdfObjectType::IndirectRef; }
    };

    using PdfObjectPtr = std::shared_ptr<PdfObject>;
}
