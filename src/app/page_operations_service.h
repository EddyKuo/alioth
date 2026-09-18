#pragma once

// 頁面操作服務（PRD-PAGE-007~011）。
//
// 這一層把引擎的頁面組合能力接到應用層。與註解服務的差別在於**這些操作會全檔重寫**：
// 合併、疊加、取代、正規化都會改動頁面樹或頁面座標系，沒有辦法以純附加表達。
//
// 那代表既有的數位簽章一定會失效。這不是實作缺陷而是操作本身的性質——
// 把頁面重排之後還宣稱「簽章有效、簽署後有變更」是謊話。因此這一層的每個入口
// 都要求呼叫端明確承認這件事，而不是安靜地把簽章毀掉。

#include <QObject>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <vector>

#include "domain/page_operations.h"

#include "domain/page_compose.h"
#include "engine/pageops/page_boxes.h"

namespace alioth::app {

// 全檔重寫的同意權杖。與 domain::IrreversibleConsent 同樣的用意：
// 讓「這一步會讓簽章失效」在原始碼裡有一個搜尋得到的字面痕跡。
class RewriteConsent {
public:
    [[nodiscard]] static RewriteConsent confirmed() noexcept { return RewriteConsent{}; }

private:
    RewriteConsent() = default;
};

struct PageOperationResult {
    bool ok{false};
    QString message;
    int pageCount{0};

    // 操作前的檔案大小與雜湊守衛，供命令堆疊復原用。
    // 全檔重寫不能靠「截回原長度」還原，必須保留原始位元組。
    QByteArray previousBytes;
};

class PageOperationsService : public QObject {
    Q_OBJECT

public:
    explicit PageOperationsService(QObject* parent = nullptr);

    // N 頁疊成一頁。
    [[nodiscard]] PageOperationResult mergePages(const QString& path,
                                                 const std::vector<int>& pages,
                                                 const domain::compose::MergeLayout& layout,
                                                 RewriteConsent);

    // 以另一份文件疊在本文件之上或之下（浮水印、印章）。
    [[nodiscard]] PageOperationResult overlay(const QString& path, const QString& overlayPath,
                                              const domain::compose::OverlayOptions& options,
                                              RewriteConsent);

    // 以另一份文件的頁面取代指定範圍。
    [[nodiscard]] PageOperationResult replacePages(const QString& path,
                                                   const QString& replacementPath, int firstPage,
                                                   int lastPage, RewriteConsent);

    // MediaBox 原點移到 (0,0)，內容與註解一併平移。
    // 把 pages 裡的頁面移到 destinationIndex（PRD-NAV-004 的縮圖拖曳重排）。
    // destinationIndex 以「移動前」的索引表示插入點，與 PageEditor 一致。
    [[nodiscard]] PageOperationResult movePages(const QString& path, const std::vector<int>& pages,
                                                int destinationIndex, RewriteConsent);

    // 從另一份 PDF 插入頁面（PRD-PAGE-001）。atIndex 是插入位置（0 起算）；
    // sourcePages 留空代表整份。
    [[nodiscard]] PageOperationResult insertPagesFrom(const QString& path,
                                                      const QString& sourcePath, int atIndex,
                                                      const std::vector<int>& sourcePages,
                                                      RewriteConsent);

    // 從純文字插入頁面（PRD-PAGE-001 的「從文字插入」）。
    //
    // 先把文字排版成一份獨立的 PDF，再走與「從檔案插入」完全相同的插入路徑——
    // 不另寫一條插入邏輯。兩條插入路徑會在頁面樹的處理上慢慢分歧，而分歧的
    // 那一條遲早會弄丟註解或搞錯順序。
    [[nodiscard]] PageOperationResult insertPagesFromText(const QString& path,
                                                          const QString& text, int atIndex,
                                                          RewriteConsent);

    // 刪除頁面（PRD-PAGE-002）。全檔重寫；復原靠保留原始位元組。
    //
    // 刪光所有頁面會產生一份沒有頁面的 PDF——多數檢視器打不開，而使用者
    // 得到的是一個「壞掉的檔案」而不是一個空文件。因此至少保留一頁。
    [[nodiscard]] PageOperationResult deletePages(const QString& path,
                                                  const std::vector<int>& pages, RewriteConsent);

    // 擷取指定頁另存新檔（PRD-PAGE-002）。**不動原檔**，因此不需要
    // RewriteConsent——它保護的是「原檔會被重寫」這件事，而這裡不會。
    [[nodiscard]] PageOperationResult extractPages(const QString& path,
                                                   const std::vector<int>& pages,
                                                   const QString& targetPath);

    // 合併多份文件另存新檔（PRD-PAGE-002）。**不動任何來源檔**，因此不需要
    // RewriteConsent——它保護的是「原檔會被重寫」，而這裡不會。
    [[nodiscard]] PageOperationResult mergeDocuments(const QStringList& sourcePaths,
                                                     const QString& targetPath);

    // 依規則分割成多個檔案（PRD-PAGE-002）。同樣不動原檔。
    // targetPattern 內的 "{n}" 會被換成 1 起算的序號；沒有 "{n}" 時序號
    // 插在副檔名之前。
    [[nodiscard]] PageOperationResult splitDocument(const QString& path, int pagesPerChunk,
                                                    const QString& targetPattern);

    // 裁切至白邊（PRD-PAGE-003）。整頁皆白的頁會被跳過而不是失敗——
    // 把空矩形拿去裁切會產出一份沒有內容的頁面。
    [[nodiscard]] PageOperationResult cropToContent(const QString& path,
                                                    const std::vector<int>& pages, RewriteConsent);

    // 複製頁面（PDF-XChange 的 Organize / Pages → Duplicate Page）。
    //
    // 複本插在 destinationIndex（以**複製前**的索引表示插入點，與 movePages
    // 一致）。-1 代表插在每一個來源頁的正後方——那是「複製這一頁」最常見的
    // 意圖，而讓呼叫端自己算插入點會在多選時算錯。
    [[nodiscard]] PageOperationResult duplicatePages(const QString& path,
                                                     const std::vector<int>& pages,
                                                     int destinationIndex, RewriteConsent);

    // 頁面尺寸調整（PRD-PAGE-003）。
    //
    // 縮放政策不給預設值，因為「改紙張大小」的兩種意思差別很大而且無法從
    // 操作本身推斷：ScaleContent 讓內容跟著等比縮放（A4 報告印成 A3），
    // KeepContent 只換紙並置中、內容維持原尺寸（工程圖換紙時的唯一正解——
    // 圖上標的 1:100 是紙上的事實，縮放過的圖再量就是錯的）。
    //
    // pages 留空代表全部頁面。這一項會全檔重寫，且註解跟著同一個矩陣搬。
    [[nodiscard]] PageOperationResult resizePages(const QString& path,
                                                  const std::vector<int>& pages,
                                                  domain::SizeF pageSizePt,
                                                  engine::pageops::ResizePolicy policy,
                                                  RewriteConsent);

    // 旋轉頁面（PRD-PAGE-*）。角度是**相對**的：在頁面現有的 /Rotate 上累加，
    // 因為使用者按的是「再轉 90 度」而不是「轉成 90 度」。
    [[nodiscard]] PageOperationResult rotatePages(const QString& path,
                                                  const std::vector<int>& pages,
                                                  domain::pages::PageRotation rotation, RewriteConsent);

    // 「文件加摘要」（PRD-ANN-028）：在指定的每一頁之後插入該頁的註解摘要頁。
    //
    // summaries 的 first 是主文件的頁碼（0 起算，**插入前**的編號），
    // second 是那一頁的摘要文字。位置由 interleavePagesFrom 一次算完，
    // 呼叫端不必自己由後往前排。整份文件只重寫一次。
    [[nodiscard]] PageOperationResult insertSummaryPages(
        const QString& path, const std::vector<std::pair<int, QString>>& summaries,
        RewriteConsent);

    // 「並排」（PRD-ANN-028 的第三種版面）：每一頁與它的摘要併成一張，
    // 左邊原頁面、右邊摘要。目標頁是原頁面的兩倍寬、等高，所以原內容維持
    // 原尺寸不縮小——把正文縮成一半來騰出摘要空間，等於為了看註解而讓
    // 正文變得難讀，而使用者是為了對照才選並排的。
    //
    // 沒有註解的頁面**原樣保留**，不會併出一張右半空白的頁。
    // 整份文件重寫兩次（插入摘要頁、合成並排頁），與逐頁合成的 N 次相比
    // 是固定成本。
    [[nodiscard]] PageOperationResult insertSideBySideSummary(
        const QString& path, const std::vector<std::pair<int, QString>>& summaries,
        RewriteConsent);

    [[nodiscard]] PageOperationResult normalize(const QString& path, RewriteConsent);

    // 復原：把先前保留的原始位元組寫回去。
    [[nodiscard]] bool restore(const QString& path, const QByteArray& previousBytes,
                               QString* message = nullptr);

private:
    // 排版好的摘要中繼文件。三種版面（僅摘要／文件加摘要／並排）共用同一份：
    // 差別只在之後怎麼放，不在摘要本身長什麼樣。
    struct SummaryDocument {
        std::string bytes;
        int totalPages{0};
        std::vector<int> firstPage;   // 每一段摘要在中繼文件裡的起始頁
        std::vector<int> pageCounts;  // 每一段佔幾頁（一段可能長到跨頁）
        std::vector<int> targets;     // 對應主文件的哪一頁（0 起算，插入前的編號）
    };

    [[nodiscard]] bool composeSummaryDocument(
        const std::vector<std::pair<int, QString>>& summaries, SummaryDocument* out,
        QString* message);

    [[nodiscard]] PageOperationResult writeBack(const QString& path, const QByteArray& previous,
                                                const std::string& bytes, int pageCount,
                                                const QString& successMessage);
};

}  // namespace alioth::app
