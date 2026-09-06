#pragma once

// 原檔的唯讀檢視：交叉參照表、trailer 與物件取用（ADR-002）。
//
// 附加新物件不需要理解原檔的完整結構，只需要知道兩件事：最大的物件編號
// （取號不能與既有物件相撞，否則等於覆蓋別人的資料），以及我們要掛上東西的
// 那幾個物件（頁面字典）長什麼樣子。這個類別只提供這兩件事，
// 不做完整的物件圖走訪。
//
// 交叉參照有兩種形態：PDF 1.4 之前的傳統 xref 表，以及 1.5 起的 xref 串流。
// 兩者都必須支援，而且**判斷錯誤的後果是產生打不開的檔案**——附加段的格式
// 必須與原檔一致，混用會讓部分解析器只看到其中一半。

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/objects/pdf_object.h"

namespace alioth::engine::objects {

enum class SourceStatus {
    Ok,
    Empty,
    NotPdf,
    NoStartxref,
    BadXref,
    Encrypted,          // ADR-002 驗收條件 5：加密文件明確拒絕
    UnsupportedFilter,
};

[[nodiscard]] const char* describe(SourceStatus status) noexcept;

// 交叉參照的形態。附加段必須沿用原檔的形態。
enum class XrefStyle {
    Table,
    Stream,
};

class PdfSourceDocument {
public:
    struct ObjectLocation {
        bool inObjectStream{false};
        std::size_t offset{0};       // inObjectStream 為 false 時的檔案位移
        int containerObject{0};      // inObjectStream 為 true 時的 /ObjStm 物件編號
        int indexInContainer{0};
        int generation{0};
    };

    PdfSourceDocument() = default;

    // bytes 會被整份持有：附加式寫入的輸出是「原檔位元組 + 新段」，
    // 原檔必須在輸出時仍然在手上，逐位元組相同是簽章保全的前提。
    [[nodiscard]] SourceStatus open(std::string bytes, std::string* diagnostic = nullptr);

    [[nodiscard]] const std::string& bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::int64_t trailerSize() const noexcept { return trailerSize_; }
    [[nodiscard]] std::size_t lastXrefOffset() const noexcept { return lastXrefOffset_; }
    [[nodiscard]] XrefStyle style() const noexcept { return style_; }
    [[nodiscard]] bool encrypted() const noexcept { return encrypted_; }
    [[nodiscard]] const PdfDictionary& trailer() const noexcept { return trailer_; }

    [[nodiscard]] bool hasObject(int number) const;
    [[nodiscard]] int generationOf(int number) const;

    // 取出物件。找不到或解析失敗時回傳 null 物件，不丟例外——PDF 是不可信任輸入。
    [[nodiscard]] PdfObject object(int number) const;
    [[nodiscard]] PdfObject resolve(const PdfObject& object) const;

    // 頁面物件的參照，依頁序。頁面樹以迭代走訪並設上限，
    // 惡意檔案可以用互相指涉的 /Kids 讓遞迴實作直接爆掉。
    [[nodiscard]] const std::vector<PdfRef>& pages() const noexcept { return pages_; }

    // 沿 /Parent 鏈找可繼承的屬性（/Resources、/MediaBox…）。
    [[nodiscard]] PdfObject inheritedPageAttribute(const PdfRef& page, const std::string& key) const;

private:
    [[nodiscard]] bool parseXrefChain(std::string* diagnostic);
    [[nodiscard]] bool parseXrefTable(std::size_t offset, std::size_t& previous,
                                      std::size_t& hybrid, bool& hasPrevious, bool& hasHybrid,
                                      std::string* diagnostic);
    [[nodiscard]] bool parseXrefStream(std::size_t offset, std::size_t& previous, bool& hasPrevious,
                                       std::string* diagnostic);
    void mergeTrailer(const PdfDictionary& dict);
    void recordEntry(int number, const ObjectLocation& location);
    void collectPages();

    std::string bytes_;
    std::map<int, ObjectLocation> xref_;
    PdfDictionary trailer_;
    std::vector<PdfRef> pages_;
    mutable std::unordered_map<int, PdfObject> cache_;
    mutable std::vector<int> loading_;  // 迴圈保護：/Length 指回自己的檔案存在
    std::int64_t trailerSize_{0};
    std::size_t lastXrefOffset_{0};
    XrefStyle style_{XrefStyle::Table};
    bool encrypted_{false};
};

}  // namespace alioth::engine::objects
