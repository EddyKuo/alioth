#pragma once

// 註解寫入服務（應用層）。
//
// 把「目前的選取」變成一則寫進檔案的螢光筆註解，是本產品的核心迴圈：
// 選取 → 產生 QuadPoints → 產生外觀串流 → 增量儲存。三個子系統在這裡第一次接起來。
//
// 寫入走 ADR-002 的物件層通道：PDFium 的 FPDFAnnot_SetAP 建不出 /AP 的 /Resources，
// 螢光筆的 Multiply 混合會遺失，/Popup 與 /IRT 也寫不進去。
//
// 目前這條路徑是同步的，而且會把整份檔案讀進記憶體。對 M0 的驗證足夠，
// 但 100 MB 文件會卡住 GUI 執行緒——正式路徑要走命令匯流排並在引擎執行緒上執行
// （WBS 4.12 / 5.1 的接線）。

#include <QByteArray>
#include <QObject>
#include <QString>

#include <cstdint>
#include <vector>

#include "domain/annotation.h"
#include "domain/quad_point.h"
#include "app/xfdf_io.h"

namespace alioth::app {

// 一則要寫進文件的註解。註解本身用領域模型描述，服務只負責把它落地——
// 每加一種註解型別就多一個 Request 結構的話，這個檔案會變成型別的目錄。
struct AnnotationRequest {
    QString path;
    std::int32_t pageIndex{0};
    domain::Annotation annotation;
};

// 螢光筆是最高頻的操作，保留一個專用的入口省去呼叫端組模型。
struct HighlightRequest {
    QString path;
    std::int32_t pageIndex{0};
    std::vector<domain::QuadPoint> quads;
    QString author;
    QString contents;
    domain::ColorRgb color{1.0, 0.85, 0.0};
    double opacity{0.4};
};

struct HighlightResult {
    bool ok{false};
    QString message;

    // 復原用的邊界資訊。增量儲存是純附加，所以「把檔案截回原長度」就是精確的反操作——
    // 不需要保存整份快照，100 MB 的文件也一樣。
    quint64 previousSize{0};
    // 邊界守衛：原檔尾端一小段的雜湊。復原前比對，確保這段期間沒有別人改過檔案。
    // 沒有這道檢查，復原會在使用者不知情的情況下砍掉別人寫入的內容。
    QByteArray boundaryGuard;
};

class AnnotationService : public QObject {
    Q_OBJECT

public:
    explicit AnnotationService(QObject* parent = nullptr);

    [[nodiscard]] HighlightResult addAnnotation(const AnnotationRequest& request);

    // 刪除一則註解（PRD-ANN-010 的「所有註解操作」之一）。
    //
    // 做法是把參照從頁面的 /Annots 拿掉，**不刪除註解物件本身**——附加式寫入
    // 本來就不能刪東西，而留著它讓復原仍然只是把檔案截回原長度。
    // 代價是檔案不會變小；那是增量儲存的本質，不是這個操作的缺陷。
    //
    // pageIndex 與 indexOnPage 來自 domain::AnnotationSummary，也就是使用者
    // 在註解列表上看到的那一則。
    [[nodiscard]] HighlightResult deleteAnnotation(const QString& path, std::int32_t pageIndex,
                                                   std::int32_t indexOnPage);
    [[nodiscard]] HighlightResult addHighlight(const HighlightRequest& request);

    // 改寫一則既有註解的註釋文字（PRD-ANN-004 的彈出視窗編輯）。
    //
    // 與 deleteAnnotation 一樣用 (pageIndex, indexOnPage) 定位，理由也一樣：
    // 那正是使用者在註解列表上看到的那一則。空字串代表清空註釋。
    //
    // 外觀由文字決定的子型（/FreeText、/Redact）會被拒絕——只改字典不重畫 /AP
    // 會讓 Acrobat 與其他檢視器顯示不同的文字。
    [[nodiscard]] HighlightResult updateAnnotationContents(const QString& path,
                                                          std::int32_t pageIndex,
                                                          std::int32_t indexOnPage,
                                                          const QString& contents);

    // 回覆一則註解，或替它標上審閱狀態（PRD-ANN-007）。
    //
    // 兩者共用同一條路徑，因為在 PDF 裡它們是同一件事：狀態由一則**獨立的
    // 回覆註解**承載（/IRT 指向被回覆者，加上 /StateModel /State），而不是
    // 改寫原註解。那是刻意的——這樣「誰在什麼時候把它標成已完成」才留得下來
    // （ISO 32000-1 §12.5.6.19）。改寫原註解會把那段歷史抹掉。
    //
    // state 留空代表純回覆；有值時必須是 Review 模型的四個值之一
    // （Accepted / Rejected / Cancelled / Completed）。
    [[nodiscard]] HighlightResult replyToAnnotation(const QString& path, std::int32_t pageIndex,
                                                    std::int32_t indexOnPage,
                                                    const QString& contents,
                                                    const QString& author,
                                                    const QString& state = {});

    // 讀出文件裡全部可匯出的註解（PRD-ANN-013）。
    //
    // 回傳 XfdfEntry 而不是 AnnotationSummary：匯出需要 /QuadPoints、/InkList
    // 這些真正決定形狀的鍵，摘要沒有。不支援的子型（Widget、Popup、Stamp…）
    // 直接略過，回傳的每一項都是可以原樣寫回另一份文件的。
    [[nodiscard]] std::vector<XfdfEntry> readAnnotationsForExport(const QString& path,
                                                                  QString* error = nullptr) const;

    // 一次寫入一批註解。整批放在同一個增量段裡，因此復原也是一步——
    // 逐則各寫一次會讓「匯入 200 則註解」或「一筆壓力筆畫拆成的五層」在
    // 復原堆疊上變成 200 步、5 步，而使用者心裡那都是一個動作。
    //
    // 頁碼超出文件範圍或子型不支援的項目會被跳過並計入回報訊息，不中止整批。
    [[nodiscard]] HighlightResult addAnnotations(const QString& path,
                                                 const std::vector<XfdfEntry>& entries);

    // 從整份匯出結果裡挑出使用者選定的那幾則（PRD-ANN-013「匯出選定註解」）。
    //
    // 比對用的是「頁碼 + 子型 + 外框 + 作者 + 內容」而不是索引。索引看起來
    // 更直接，但兩份清單的來源不同——畫面上的清單來自 PDFium 的列舉，匯出
    // 的內容來自物件層的 /Annots 走訪，兩者對 Popup 之類的附屬註解是否計入
    // 並不保證一致。索引一旦錯開，使用者選第 3 則卻匯出第 5 則，而輸出檔
    // 看起來完全正常。
    //
    // 對不上的選取項目不會憑空補一則出來，呼叫端應該把「選了幾則、實際匯出
    // 幾則」都告訴使用者。
    [[nodiscard]] static std::vector<XfdfEntry> selectEntries(
        const std::vector<XfdfEntry>& entries,
        const std::vector<domain::AnnotationSummary>& wanted);

    // 補上作者、時間戳與唯一 ID。呼叫端只需要描述幾何與顏色。
    [[nodiscard]] static domain::Annotation stamped(domain::Annotation annotation,
                                                    const QString& author,
                                                    const QString& contents = {});

    // 復原一次附加式寫入：把檔案截回 previousSize。
    // 邊界守衛不符時拒絕執行並回報，不會硬幹。
    [[nodiscard]] bool revertAppend(const QString& path, quint64 previousSize,
                                    const QByteArray& boundaryGuard, QString* message = nullptr);
};

}  // namespace alioth::app
