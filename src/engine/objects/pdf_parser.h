#pragma once

// 最小 PDF 物件剖析器（ADR-002）。
//
// ADR-002 把寫入範圍限制在「附加新物件」，但把註解掛上 /Annots、把 Bates 的
// 內容串流掛上 /Contents，都必須先讀出既有的頁面字典再原樣寫回一份新版本。
// 因此需要讀取能力——但只需要「讀得懂我們要動的那幾個物件」，
// 不需要走訪整張物件圖，那正是採用 PDFium 的理由。
//
// 這一層只認得語法，不認得檔案結構（xref、trailer 屬於 pdf_source_document）。
// PDF 是不可信任輸入：所有巢狀走訪都是迭代加深度上限，任何語法錯誤一律
// 回傳 false 而不是丟例外，也不會回傳「成功但內容是空的」。

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

#include "engine/objects/pdf_object.h"

namespace alioth::engine::objects {

// 解析 /Length 這種以間接參照給出的值時用的回呼。無法解析時回傳 null 物件。
using ObjectResolver = std::function<PdfObject(const PdfRef&)>;

class PdfParser {
public:
    explicit PdfParser(std::string_view bytes, std::size_t position = 0)
        : bytes_(bytes), pos_(position) {}

    void setResolver(ObjectResolver resolver) { resolver_ = std::move(resolver); }

    [[nodiscard]] std::size_t position() const noexcept { return pos_; }
    void seek(std::size_t position) noexcept { pos_ = position; }

    // 解析目前位置的一個物件（含串流）。
    [[nodiscard]] bool parseObject(PdfObject& out);

    // 解析「N G obj … endobj」。位置必須落在物件編號上。
    [[nodiscard]] bool parseIndirectObject(int& number, int& generation, PdfObject& out);

    // 跳過空白與註解，回傳是否還有內容。
    bool skipWhitespace();

    // 讀一個關鍵字（obj / endobj / stream / trailer / R / true …）。
    [[nodiscard]] bool readKeyword(std::string& out);

    // 若目前位置正好是指定關鍵字就吃掉它並回傳 true，否則位置不動。
    [[nodiscard]] bool consumeKeyword(std::string_view keyword);

private:
    [[nodiscard]] bool parseValue(PdfObject& out, int depth);
    [[nodiscard]] bool parseName(PdfObject& out);
    [[nodiscard]] bool parseLiteralString(PdfObject& out);
    [[nodiscard]] bool parseHexString(PdfObject& out);
    [[nodiscard]] bool parseNumberOrRef(PdfObject& out);
    [[nodiscard]] bool parseArray(PdfObject& out, int depth);
    [[nodiscard]] bool parseDictionaryOrStream(PdfObject& out, int depth);
    [[nodiscard]] bool readStreamData(const PdfDictionary& dict, std::string& out);

    std::string_view bytes_;
    std::size_t pos_{0};
    ObjectResolver resolver_{};
};

struct DecodeResult {
    bool ok{false};
    std::string data;
    std::string diagnostic;  // 失敗原因，不做靜默失敗
};

// 解開串流的 /Filter。
//
// 只支援 FlateDecode（含 /DecodeParms 的 PNG 預測器）與無壓縮：xref 串流與
// 物件串流在實務上就這兩種。遇到 LZW / DCT 等其他濾鏡一律明確失敗，
// 因為猜錯的結果是我們據以計算物件編號的資料是垃圾，那會覆蓋既有物件。
[[nodiscard]] DecodeResult decodeStream(const PdfStream& stream, const ObjectResolver& resolver = {});

// 純 zlib 解壓（RFC 1950），供測試與濾鏡層共用。
[[nodiscard]] DecodeResult flateDecode(const std::string& input);

}  // namespace alioth::engine::objects
