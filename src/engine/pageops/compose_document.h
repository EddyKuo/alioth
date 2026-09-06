#pragma once

// 頁面進階操作的共用基礎（WBS 12，PRD-PAGE-007 ~ 011）。
//
// 為什麼這五項一律走全檔重寫而不是專案預設的增量附加：
// 合併頁面、覆蓋、取代、改 MediaBox、正規化，每一項都會改寫頁面樹或頁面的
// 座標系。既有簽章覆蓋的位元組雖然還在，簽章的語意（「我簽的是這幾頁、
// 這個座標系」）已經不成立，留著一份「有效，簽章後有變更」的假象比重寫更糟。
// 因此本子系統的輸出恆為新檔位元組，呼叫端要自己決定另存還是覆蓋。
//
// 開檔時會先把頁面樹**攤平**成單層：所有頁面直接掛在根 /Pages 底下，
// 可繼承屬性（/Resources /MediaBox /CropBox /Rotate）先下推到每一頁。
// 這不是為了好看——插入、刪除、取代頁面若要在多層樹上就地做，
// 每一次都得處理 /Count 回溯更新與繼承屬性被中間節點遮蔽的問題，
// 而那兩者出錯的症狀分別是「檢視器看到的頁數不對」與「某幾頁突然沒有字型」。

#include <cstdint>
#include <string>
#include <vector>

#include "domain/geometry.h"
#include "domain/page_compose.h"
#include "engine/objects/pdf_object.h"
#include "engine/redaction/pdf_document_rewriter.h"

namespace alioth::engine::pageops {

using objects::PdfArray;
using objects::PdfDictionary;
using objects::PdfObject;
using objects::PdfRef;
using redaction::PdfDocumentRewriter;

enum class PageOpsStatus : std::uint8_t {
    Ok,
    SourceInvalid,     // 主文件打不開（含加密：字串與串流需加密後才寫得回去）
    SecondaryInvalid,  // 覆蓋／取代用的第二份文件打不開
    PageOutOfRange,
    InvalidRequest,     // 請求本身矛盾（重複頁、範圍為空）
    ContentUnreadable,  // 內容串流解不開（例如 LZW / DCT 濾鏡）
    LayoutRejected,     // 領域層版面驗證未過，細節在 compose 欄位
};

[[nodiscard]] const char* describe(PageOpsStatus status) noexcept;

// 所有操作的共同輸出。bytes 只在 ok() 時有意義。
struct PageOpsResult {
    PageOpsStatus status{PageOpsStatus::Ok};
    domain::compose::ComposeStatus compose{domain::compose::ComposeStatus::Ok};
    std::string diagnostic;
    std::string bytes;
    int pageCount{0};

    [[nodiscard]] bool ok() const noexcept { return status == PageOpsStatus::Ok; }
};

// 攤平頁面樹之後的文件檢視。頁序以本類別持有的清單為準，
// 不要回頭問 PdfDocumentRewriter::pageRef——那是原檔的頁序，改過樹之後就過期了。
class ComposeDocument {
public:
    [[nodiscard]] PageOpsStatus open(std::string bytes, std::string* diagnostic = nullptr);
    [[nodiscard]] bool isOpen() const noexcept { return opened_; }

    [[nodiscard]] PdfDocumentRewriter& rewriter() noexcept { return document_; }
    [[nodiscard]] const PdfDocumentRewriter& rewriter() const noexcept { return document_; }

    [[nodiscard]] int pageCount() const noexcept { return static_cast<int>(pages_.size()); }
    [[nodiscard]] const std::vector<PdfRef>& pages() const noexcept { return pages_; }
    [[nodiscard]] bool pageAt(int index, PdfRef& out) const;

    // 換掉整份頁序。同時修好 /Kids、/Count 與每一頁的 /Parent；
    // 少修 /Parent 的症狀是繼承鏈斷掉，而不是立刻的錯誤。
    void setPages(std::vector<PdfRef> pages);

    [[nodiscard]] PdfRef pagesRoot() const noexcept { return root_; }

    // 頁面的框。/MediaBox 缺失時退回 US Letter：規格說它必填，
    // 真實檔案不見得照做，而沒有框就無法計算任何版面。
    [[nodiscard]] domain::RectF mediaBox(const PdfRef& page) const;
    // /CropBox 不存在時等於 /MediaBox；存在但超出時夾進去（規格要求落在其內）。
    [[nodiscard]] domain::RectF cropBox(const PdfRef& page) const;
    [[nodiscard]] int rotation(const PdfRef& page) const;

    [[nodiscard]] std::string build() const { return document_.build(); }

private:
    void flatten();

    PdfDocumentRewriter document_;
    std::vector<PdfRef> pages_;
    PdfRef root_{};
    bool opened_{false};
};

// 取得頁面字典（攤平後每頁都是獨立字典，可直接改）。找不到回傳 nullptr。
[[nodiscard]] PdfDictionary* pageDictionary(PdfDocumentRewriter& document, const PdfRef& page);
[[nodiscard]] const PdfDictionary* pageDictionary(const PdfDocumentRewriter& document,
                                                  const PdfRef& page);

// 把 /Contents（單一串流或串流陣列）解開並串接成一份內容位元組。
//
// 串接時一定要補空白：規格說多個串流在語意上等同單一串流，而前一段的結尾
// 若是運算元的最後一個字元，直接相接會與下一段的第一個運算子黏成一個 token。
struct ContentBytes {
    bool ok{false};
    std::string diagnostic;
    std::string data;
};

[[nodiscard]] ContentBytes readPageContent(const PdfDocumentRewriter& document,
                                           const PdfRef& page);

// 建立一個未壓縮的內容串流物件，回傳物件編號。
[[nodiscard]] int addContentStream(PdfDocumentRewriter& document, std::string data);

// 把頁面的 /Contents 換成指定的物件序列。
void setPageContents(PdfDocumentRewriter& document, const PdfRef& page,
                     const std::vector<int>& contentObjects);

// 以 q / Q 把頁面原有的內容包起來。
//
// 疊加內容之前一定要做這件事：原頁的內容串流不保證圖形狀態平衡，
// 少一個 Q 的話它設的裁切區、顏色、CTM 會外溢到後面疊上去的內容，
// 症狀是浮水印顏色莫名其妙或整個不見，而原檔本身看起來完全正常。
// 回傳包好之後的內容物件序列，讓呼叫端可以決定把新內容插在最前或最後。
[[nodiscard]] std::vector<int> wrapPageContentInGraphicsState(PdfDocumentRewriter& document,
                                                              const PdfRef& page);

// 矩陣序列化成 cm 運算元的運算元字串（不含 " cm"）。
[[nodiscard]] std::string formatMatrix(const domain::compose::Matrix& matrix);

// 矩形序列化成 PDF 陣列物件。
[[nodiscard]] PdfObject makeRectArray(const domain::RectF& rect);

// 讀取 PDF 的矩形陣列。長度不足或非數值時回傳 false。
[[nodiscard]] bool readRectArray(const PdfDocumentRewriter& document, const PdfObject& value,
                                 domain::RectF& out);

}  // namespace alioth::engine::pageops
