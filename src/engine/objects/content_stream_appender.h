#pragma once

// 內容串流附加（ADR-002、PRD-PAGE-004 Bates 編號的「永久套用」側）。
//
// 列印時的戳記只存在於紙上；法務用途的 Bates 編號通常必須留在檔案裡。
// 那需要新增一個內容串流物件並把它掛到頁面的 /Contents 陣列上——
// ADR-002 把這件事歸類為「新增物件」，因為原本的內容串流一個位元組都不會動。
//
// 繪圖指令由呼叫端提供（app/print/stamp_content_stream.h 已經產生好），
// 這一層只負責把它變成 PDF 物件、登記字型資源、掛上 /Contents。
//
// 字型只支援標準 14 種。CJK 需要字型子集內嵌，其授權策略在 CLAUDE.md 仍是
// 待決策項（影響 WBS 4.8），因此遇到非 ASCII 內容一律明確失敗，
// 不輸出會變成亂碼或缺字的位元組。

#include <string>
#include <vector>

#include "engine/objects/incremental_appender.h"

namespace alioth::engine::objects {

struct ContentFontRequest {
    std::string resourceName{"F0"};    // 內容串流裡用的名稱，例如 /F0 10 Tf
    std::string baseFont{"Helvetica"}; // 必須是標準 14 之一
};

struct ContentAppendOptions {
    std::vector<ContentFontRequest> fonts{};

    // 以 q/Q 包住新增的指令。/Contents 陣列在解析時等同單一串流，
    // 不平衡的圖形狀態會外溢到後續內容；預設包起來是唯一安全的作法。
    bool wrapInGraphicsState{true};

    // 在新增的串流字典裡放一個私有標記鍵（值為 true），讓之後找得回來。
    //
    // 沒有它就沒有辦法實作「移除所有浮水印」：內容串流附加之後與原本的內容
    // 在解析上是同一份，要分辨哪一段是我們加的，只能回頭剖析運算子並猜——
    // 而猜錯的代價是刪掉使用者原本的內容。私有鍵是 PDF 規格允許的擴充方式
    // （ISO 32000-2 §7.3.7：未知鍵必須被忽略），任何檢視器都不會受影響。
    //
    // 留空代表不加標記（例如單純的內容附加，不屬於任何一種戳記）。
    std::string markerKey{};
};

struct ContentAppendResult {
    bool ok{false};
    std::string diagnostic;
    int contentObject{0};
    std::vector<int> fontObjects{};
};

// 標準 14 字型的合法 /BaseFont 名稱。
[[nodiscard]] bool isStandard14Font(const std::string& baseFont);

[[nodiscard]] ContentAppendResult appendPageContent(IncrementalAppender& appender, int pageIndex,
                                                    const std::string& content,
                                                    const ContentAppendOptions& options = {});

// 移除所有帶指定標記鍵的內容串流參照（「移除所有浮水印／頁首頁尾／Bates」）。
//
// 只從 /Contents 陣列把參照摘掉，**不刪除被指向的物件**：附加式寫入本來就
// 不能刪東西，而留著它讓復原仍然只是把檔案截回原長度。因此這一步與寫入
// 一樣是純附加，既有簽章維持「有效，簽署後有變更」而不是「無效」。
//
// 只會動我們自己加的串流——標記鍵是我們寫的，別的工具不會有。
// 別人加的浮水印移不掉，這是正確的行為：那需要剖析並猜測原始內容。
struct RemoveMarkedResult {
    bool ok{false};
    std::string diagnostic;
    int removedReferences{0};
    int affectedPages{0};
};

[[nodiscard]] RemoveMarkedResult removeMarkedPageContent(IncrementalAppender& appender,
                                                          const std::string& markerKey);

}  // namespace alioth::engine::objects
