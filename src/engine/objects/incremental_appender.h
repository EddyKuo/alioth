#pragma once

// 增量附加器（ADR-002、WBS 5.1）。
//
// 輸出恆為「原檔位元組 + 新段」。原檔一個位元組都不能動，這是既有數位簽章
// 仍然顯示為「有效，簽章後有變更」的唯一前提（SDD §1.3）；因此 build() 除了
// 產生輸出，還會自己回頭比對前綴，而不是相信程式碼一定沒寫錯。
//
// 取號以原檔 trailer 的 /Size 為準並在寫入後驗證：物件編號一旦與既有物件相撞，
// 舊物件就被靜默取代，症狀是文件內容莫名消失，而我們自己的檢視器（PDFium）
// 容錯度高，很可能看不出來。
//
// 附加段的交叉參照形態必須沿用原檔：傳統 xref 表的檔案接 xref 表，
// xref 串流的檔案接 xref 串流。混用會讓部分解析器只看到其中一半的更新。

#include <cstdint>
#include <map>
#include <string>

#include "engine/objects/pdf_object.h"
#include "engine/objects/pdf_source_document.h"

namespace alioth::engine::objects {

struct BuildResult {
    bool ok{false};
    std::string diagnostic;
    std::string bytes;                 // 完整輸出檔（原檔前綴 + 附加段）
    std::uint64_t appendedBytes{0};    // PRD-IO-001 的「增量 ≤ 20 KB」量的就是這個
};

class IncrementalAppender {
public:
    // 開啟原檔。加密文件在這裡就會被擋下（ADR-002 驗收條件 5）。
    [[nodiscard]] SourceStatus open(std::string bytes, std::string* diagnostic = nullptr);

    [[nodiscard]] const PdfSourceDocument& source() const noexcept { return source_; }
    [[nodiscard]] bool isOpen() const noexcept { return opened_; }

    // 配一個保證不與既有物件相撞的新編號。
    [[nodiscard]] int allocateObject();

    // 寫入新物件的內容。編號必須來自 allocateObject()。
    void setObject(int number, PdfObject object);

    // 覆寫既有物件（例如把註解掛上頁面的 /Annots）。
    // 這仍然是純附加：舊版本的位元組留在原處，只是不再被最新的 xref 指到。
    [[nodiscard]] bool updateObject(int number, PdfObject object);

    // 物件的「目前狀態」：已排入附加段的版本優先，否則取原檔的版本。
    //
    // 同一頁寫入第二則註解時，若直接讀原檔就會拿到還沒加上第一則的 /Annots，
    // 寫回去等於把第一則刪掉。這個函式存在的唯一理由就是擋掉那個錯誤。
    [[nodiscard]] PdfObject currentObject(int number) const;

    [[nodiscard]] bool hasPendingObjects() const noexcept { return !pending_.empty(); }
    [[nodiscard]] int nextObjectNumber() const noexcept { return nextNumber_; }

    // 產生輸出位元組。刻意不叫 emit：Qt 把 emit 定義成空巨集，
    // 任何 Qt 的翻譯單元（包含所有測試）呼叫 appender.emit() 都會編譯失敗。
    [[nodiscard]] BuildResult build() const;

private:
    struct Pending {
        PdfObject object;
        int generation{0};
        bool isNew{true};
    };

    [[nodiscard]] std::string buildXrefTable(const std::map<int, std::size_t>& offsets,
                                             std::int64_t newSize, std::size_t xrefOffset) const;
    [[nodiscard]] PdfDictionary buildTrailerDictionary(std::int64_t newSize) const;

    PdfSourceDocument source_;
    std::map<int, Pending> pending_;
    int nextNumber_{1};
    bool opened_{false};
};

}  // namespace alioth::engine::objects
