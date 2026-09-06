#pragma once

// 頁面字典的附加式編輯（ADR-002）。
//
// 把註解掛上 /Annots、把內容串流掛上 /Contents，形式上都是「往頁面的某個陣列
// 加一項」。這件事有四種原檔形態要處理，寫錯任何一種的後果都不是崩潰，
// 而是新東西看不見或原有內容消失：
//
//   1. 該鍵不存在        → 新建陣列
//   2. 直接陣列          → 複製後附加，頁面物件寫出新版本
//   3. 指向陣列的間接參照 → 改寫那個陣列物件，頁面物件不動（增量段更小）
//   4. 指向單一串流的參照 → 轉成陣列 [原本的, 新的]（/Contents 的常見形態）
//
// /Resources 另有一個陷阱：它可以從 /Pages 繼承。頁面上沒有 /Resources 時
// 直接建一個新的會**遮蔽**繼承來的資源，頁面原本用到的字型與影像會全部消失。
// 因此必須先把繼承來的字典複製下來再加上新項目。

#include <string>
#include <vector>

#include "engine/objects/incremental_appender.h"

namespace alioth::engine::objects {

struct PageEditStatus {
    bool ok{false};
    std::string diagnostic;
};

// 取得第 index 頁的物件參照。超出範圍時回傳 false。
[[nodiscard]] bool pageRefAt(const IncrementalAppender& appender, int index, PdfRef& out);

// 往頁面字典的某個陣列鍵附加一項（/Annots、/Contents）。
[[nodiscard]] PageEditStatus appendToPageArray(IncrementalAppender& appender, const PdfRef& page,
                                               const std::string& key, PdfObject value);

// 從頁面的某個陣列移除一個間接參照（刪除註解用）。
//
// 只動陣列，**不刪除被指向的物件**：附加式寫入本來就不能刪東西，而留著它
// 讓「復原」仍然只是把檔案截回原長度。找不到該項目時視為成功——
// 復原之後再重做必須是安全的。
[[nodiscard]] PageEditStatus removeFromPageArray(IncrementalAppender& appender,
                                                 const PdfRef& page, const std::string& key,
                                                 int objectNumber);

// 一頁的 /Annots 裡所有間接參照的物件編號，依陣列順序。
// 順序就是 domain::AnnotationSummary 的 indexOnPage，兩者必須一致，
// 否則使用者在列表上選第 2 則、實際刪掉的是別的那一則。
[[nodiscard]] std::vector<int> pageAnnotationRefs(const PdfSourceDocument& source,
                                                  const PdfRef& page);

// 在頁面的 /Resources /<category> 底下登記一個資源（例如 /Font /F0）。
[[nodiscard]] PageEditStatus setPageResource(IncrementalAppender& appender, const PdfRef& page,
                                             const std::string& category,
                                             const std::string& resourceName, PdfObject value);

// 設定頁面的 /Tabs（欄位跳位順序，ISO 32000-2 表 30）。只在頁面**尚未有**
// /Tabs 鍵時才寫入；已有值代表原作者或前一個工具已經決定過跳位順序
// （常見於 R/C/S 三種），我們不應覆蓋既有的選擇。
//
// 之所以選 /W（Widget order：忽略非 Widget 的註解，只依 /Annots 陣列裡
// Widget 出現的順序決定跳位順序）而不是 /A（Annotations array order，
// 涵蓋所有註解型別）：本產品建立欄位時是依序把 widget 附加進 /Annots，
// 這與程式呼叫端建立欄位的順序一致，也是使用者拖曳欄位順序時最直覺的
// 期待；但同一頁若已有非 Widget 的註解（螢光筆、便簽…），/A 會讓那些
// 註解也被排進跳位序列裡，於邏輯上沒有意義（它們沒有可以跳進去的焦點）。
[[nodiscard]] PageEditStatus ensurePageTabOrder(IncrementalAppender& appender, const PdfRef& page,
                                                const std::string& tabsValue = "W");

}  // namespace alioth::engine::objects
