#pragma once

// 塗黑服務（PRD-ANN-032 / PRD-ANN-033）。
//
// 兩階段的分界在這裡也保持著，而且是**不同的回傳型別**，不是同一個型別上的
// 旗標：
//
//   標記（mark）  — 走增量附加，可逆。回傳與註解服務相同的 HighlightResult，
//                   因此復原就是「把檔案截回原長度」，與其他註解操作同一條路。
//   套用（apply） — 整份重寫，**不可逆**。回傳 PageOperationResult，復原靠
//                   保留原始位元組。
//
// 兩者用不同的通道與不同的復原機制，混在一個函式裡靠參數區分，遲早會有人
// 傳錯一個 bool，而那一次的代價是使用者的原稿被永久改寫。
//
// 「塗黑」的安全性來自底下的內容真的被刪掉，不是來自畫上去的黑框。套用之後
// 以文字擷取與位元組掃描雙重驗證，見 tests/redaction/。

#include <QObject>
#include <QString>

#include "app/annotation_service.h"
#include "app/page_operations_service.h"
#include "domain/redaction.h"

namespace alioth::app {

class RedactionService : public QObject {
    Q_OBJECT

public:
    explicit RedactionService(QObject* parent = nullptr);

    // 標記一塊區域待塗黑。可逆：只是寫一則 /Redact 註解。
    [[nodiscard]] HighlightResult markArea(const QString& path, std::int32_t pageIndex,
                                           const domain::RectF& area);

    // 移除標記（pageIndex < 0 代表全部）。同樣是純附加的寫入。
    [[nodiscard]] HighlightResult clearMarks(const QString& path, std::int32_t pageIndex = -1);

    // 這份文件目前有幾則待套用的標記。UI 用它決定「套用」要不要啟用，
    // 以及在確認對話框裡說出會影響多少塊——「確定要塗黑嗎」而不說幾塊，
    // 使用者沒有辦法判斷自己標對了沒有。
    [[nodiscard]] int pendingMarkCount(const QString& path) const;

    // 套用所有標記。**不可逆**：內容真的被刪掉，既有簽章也會失效。
    // 需要 RewriteConsent，與其他全檔重寫的操作一致。
    [[nodiscard]] PageOperationResult applyMarks(const QString& path, RewriteConsent);

    // 清除隱藏中繼資料（PRD-ANN-034）：/Info、XMP、/PieceInfo、嵌入檔案、
    // JavaScript。與塗黑是**兩個獨立的操作**，刻意不合併——使用者常常只想在
    // 對外發布前清乾淨中繼資料，而文件裡一個字都不用塗黑；綁在一起會逼他
    // 為了其中一件事承擔另一件的後果。
    //
    // 同樣是全檔重寫：增量附加只會讓新的 /Info 疊在舊的上面，舊值仍然留在
    // 檔案裡，`strings` 一撈就出來——那等於什麼都沒清。
    [[nodiscard]] PageOperationResult sanitize(const QString& path, RewriteConsent);
};

}  // namespace alioth::app
