#pragma once

// 目標（/Dest、/A GoTo）與命名目標（/Dests）的編解碼（WBS 9，PRD-BM-006/007）。
//
// 為什麼要自己做而不是用 PDFium：PDFium 的 FPDFBookmark_GetDest 只給得出頁面
// 索引，縮放類型與座標參數一律拿不到，而 PRD-BM-001 明確要求「縮放類型設定」。
// 寫入端更沒有選擇——PDFium 完全沒有建立書籤的公開 API。
//
// 兩種目標形態都要支援，而且不能二選一：/Dest 是 PDF 1.1 起的原始形態，
// /A << /S /GoTo /D ... >> 是 1.2 起的動作形態。實務上兩者都有工具只認其中一種，
// 讀取端全認、寫入端由呼叫端指定，是唯一不會在別人的檢視器上壞掉的作法。
//
// 命名目標同樣有兩代語法：catalog 的 /Dests 字典（1.1）與 /Names /Dests 名稱樹
// （1.2）。讀取端兩者都掃；寫入端一律輸出名稱樹，因為字典形態沒有 /Limits，
// 大量目標時每個檢視器都得線性掃描。

#include <string>
#include <vector>

#include "domain/bookmark_ops.h"
#include "engine/objects/incremental_appender.h"

namespace alioth::engine::bookmarks {

// 頁面參照 → 頁索引的對照表。每次解析目標都重掃一次 pages() 會讓
// 書籤數 × 頁數 變成平方級，一萬頁的文件光是讀書籤就要好幾秒。
class PageIndexMap {
public:
    explicit PageIndexMap(const objects::PdfSourceDocument& source);

    // 找不到回傳 -1。遠端目標（/GoToR）的頁面是數字而不是參照，由呼叫端處理。
    [[nodiscard]] std::int32_t indexOf(const objects::PdfRef& ref) const;
    [[nodiscard]] bool refAt(std::int32_t index, objects::PdfRef& out) const;
    [[nodiscard]] std::size_t pageCount() const noexcept { return pages_.size(); }

private:
    std::vector<objects::PdfRef> pages_;
};

// 目標陣列 → 領域模型。value 可以是陣列、也可以是含 /D 的字典。
// 命名目標（名稱或字串）不由這裡處理，回傳 false 並把名稱寫進 outName。
[[nodiscard]] bool decodeDestination(const objects::PdfSourceDocument& source,
                                     const PageIndexMap& pages, const objects::PdfObject& value,
                                     domain::bookmarks::Destination& out, std::string& outName);

// 領域模型 → 目標陣列。頁索引越界時回傳 false，不做夾住：
// 靜默把第 999 頁改成最後一頁，使用者只會看到書籤跳錯地方。
[[nodiscard]] bool encodeDestination(const PageIndexMap& pages,
                                     const domain::bookmarks::Destination& destination,
                                     objects::PdfObject& out);

// << /S /GoTo /D <目標> >>。
[[nodiscard]] objects::PdfObject makeGoToAction(objects::PdfObject destination);

struct NamedDestinationEntry {
    domain::bookmarks::NamedDestination entry;
    // 目標頁解析不出來（多半是頁面被刪掉了但沒人清 /Dests）。
    // 這種項目在真實文件裡很常見，命名目標面板要「列出但不可點」，
    // 因此不能在解碼階段就丟掉——但書籤那邊需要的是可用的目標，
    // 所以 readNamedDestinations 仍然只回傳 resolved 的項目。
    bool resolved{true};
};

// 讀出全部命名目標，含解析不到目標頁的項目。
[[nodiscard]] std::vector<NamedDestinationEntry> readNamedDestinationEntries(
    const objects::PdfSourceDocument& source, const PageIndexMap& pages);

// 讀出全部命名目標。同名時以名稱樹的為準（較新的語法）。
// 解析不到目標頁的項目會被濾掉——指向不存在頁面的目標對書籤沒有用處。
[[nodiscard]] std::vector<domain::bookmarks::NamedDestination> readNamedDestinations(
    const objects::PdfSourceDocument& source, const PageIndexMap& pages);

struct NamedDestinationWriteResult {
    bool ok{false};
    std::string diagnostic;
    int namesTreeObject{0};
    std::size_t entryCount{0};
};

// 寫回名稱樹。merge 為 true 時保留原檔既有的目標，false 則整棵換掉。
//
// 注意：原檔若同時有舊式的 catalog /Dests 字典，本函式不會刪掉它——刪除等於
// 改動既有物件的語意，而附加式寫入的立場是不動原檔。既有項目會被合併進名稱樹，
// 因此兩邊都讀得到；只認舊式字典的解析器看不到新加的目標，這是已知且刻意的取捨。
[[nodiscard]] NamedDestinationWriteResult writeNamedDestinations(
    objects::IncrementalAppender& appender, const PageIndexMap& pages,
    const std::vector<domain::bookmarks::NamedDestination>& destinations, bool merge = true);

// catalog 的物件編號（trailer /Root）。找不到回傳 0。
[[nodiscard]] int catalogObjectNumber(const objects::PdfSourceDocument& source);

// PDF 文字字串 → UTF-8（UTF-16BE with BOM 或 PDFDocEncoding）。
[[nodiscard]] std::string decodeTextString(const objects::PdfString& string);

}  // namespace alioth::engine::bookmarks
