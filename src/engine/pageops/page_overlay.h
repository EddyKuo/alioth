#pragma once

// 覆蓋頁面（PRD-PAGE-008）與取代頁面（PRD-PAGE-009），WBS 12。
//
// 兩者都要把「另一份文件」的東西搬進主文件，因此共用 object_copier 的跨文件
// 複製規則。差別在於疊或換：
//
//   Overlay 把來源頁包成 Form XObject 疊在底頁的內容之上或之下。
//   放在下方是浮水印（不能蓋掉正文），放在上方是印章與騎縫章（就是要蓋住）。
//   疊之前一定要先把底頁原有的內容用 q/Q 包起來——底頁的內容串流不保證
//   圖形狀態平衡，少一個 Q 會讓它設的顏色或裁切區吃掉疊上去的東西。
//
//   Replace 直接換掉頁面物件，不做任何內容合成。頁面的可繼承屬性在開檔時
//   已經下推，所以複製過來的頁面不會因為離開原本的頁面樹而失去字型或尺寸。

#include <string>
#include <vector>

#include "domain/page_compose.h"
#include "engine/pageops/compose_document.h"

namespace alioth::engine::pageops {

struct OverlayRequest {
    domain::compose::OverlayOptions options{};

    // 要被疊的底頁，0 起算；留空代表全部頁面。
    std::vector<int> basePages;

    // 要拿來疊的來源頁。-1 代表「依序循環對應」：底頁 i 用來源頁 i % 來源頁數，
    // 這是「兩份同頁數文件互相疊合」這個用法唯一合理的預設。
    int overlayPageIndex{0};

    // 來源頁的註解要不要一起搬過來。浮水印通常沒有註解，
    // 但「把審閱意見疊回原稿」這個用法需要它，所以預設打開。
    bool copyAnnotations{true};
};

struct OverlayResult : PageOpsResult {
    int overlaidPages{0};
    int copiedAnnotations{0};
};

[[nodiscard]] OverlayResult overlayDocument(std::string baseBytes, std::string overlayBytes,
                                            const OverlayRequest& request);

struct ReplaceRequest {
    int firstPage{0};   // 0 起算
    int pageCount{1};   // 要被取代的頁數

    // 取代用的頁面（來自第二份文件），0 起算；留空代表整份。
    std::vector<int> replacementPages;
};

struct ReplaceResult : PageOpsResult {
    int removedPages{0};
    int insertedPages{0};
};

[[nodiscard]] ReplaceResult replacePages(std::string baseBytes, std::string replacementBytes,
                                         const ReplaceRequest& request);

// 從另一份文件插入頁面（PRD-PAGE-001），不刪除任何既有頁面。
//
// 這是 replacePages 的 pageCount == 0 特例，但獨立一個入口而不是要呼叫端
// 自己知道那個技巧：「取代零頁」讀起來像沒有作用，而呼叫端寫錯成 1 就會
// 靜默地刪掉插入點那一頁——插入變成取代，而且沒有任何提示。
//
// atIndex 是插入位置（0 起算，等於 base.pageCount() 代表附加在最後）。
// sourcePages 留空代表插入整份。
[[nodiscard]] ReplaceResult insertPagesFrom(std::string baseBytes, std::string sourceBytes,
                                            int atIndex, std::vector<int> sourcePages = {});

// 一次把來源文件的多頁插進主文件的多個位置（PRD-ANN-028「文件加摘要」）。
//
// 逐次呼叫 insertPagesFrom 也做得到，但每插一頁後面的索引就位移一格，
// 呼叫端必須自己由後往前排——那個要求沒有任何地方擋得住，一旦有人由前往後
// 呼叫，插入點會愈來愈偏，而症狀是「第 30 頁之後對不上」，在兩頁的測試文件上
// 完全看不出來。這個函式收下的是**原始編號**，順序由它自己算。
//
// 而且它只重寫一次檔案：100 頁註解的摘要用逐次插入就是 100 次全檔重寫。
struct PagePlacement {
    int sourcePage{0};      // 來源文件的頁碼（0 起算）
    int afterBasePage{-1};  // 插在主文件這一頁之後；-1 代表插在最前面
};

[[nodiscard]] ReplaceResult interleavePagesFrom(std::string baseBytes, std::string sourceBytes,
                                                const std::vector<PagePlacement>& placements);

}  // namespace alioth::engine::pageops
