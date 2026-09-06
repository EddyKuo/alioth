#pragma once

// 文字選取控制器（WBS 3.11 的應用層部分）。
//
// 為什麼獨立於 DocumentController：文字擷取用的是另一份文件把手與另一條執行緒
// （PDFium 非執行緒安全，見 text_extractor.h 的說明）。把它混進 DocumentController
// 會讓「哪個成員屬於哪條執行緒」這件事變得難以推理，而那正是這類程式最常見的
// 崩潰來源。這裡維持一個規則：本類別的公開成員只在 GUI 執行緒上動。

#include <QObject>
#include <QString>

#include <cstdint>
#include <memory>
#include <vector>

#include "domain/geometry.h"
#include "domain/quad_point.h"
#include "domain/text_layer.h"
#include "engine/text/text_extractor.h"
#include "engine/text/text_index.h"
#include "engine/text/text_search.h"

namespace alioth::app {

// 目前的選取。跨頁選取（PRD-TXT-002）尚未支援，因此只記一頁。
// 一頁之內的選取。跨頁選取由多個這種結構組成——PDF 的文字層本來就是逐頁的，
// 沒有跨頁的字元索引空間，硬造一個只會讓每次換算都要先問「這是哪一頁的索引」。
struct PageSelection {
    std::int32_t pageIndex{-1};
    domain::TextRange range{};
    std::vector<domain::QuadPoint> quads{};
    QString text{};
};

struct Selection {
    // 涵蓋的每一頁，依頁碼排序。單頁選取就是只有一項。
    std::vector<PageSelection> pages{};

    // 以下三個是「第一頁」的鏡射，讓既有的單頁呼叫端不必全部改寫。
    // 跨頁時它們只描述第一頁——需要完整範圍的呼叫端要走 pages。
    std::int32_t pageIndex{-1};
    domain::TextRange range{};
    std::vector<domain::QuadPoint> quads{};
    // 跨頁時是所有頁串起來的文字，頁與頁之間插入換行。
    QString text{};

    [[nodiscard]] bool isEmpty() const noexcept { return pages.empty(); }
    [[nodiscard]] bool isMultiPage() const noexcept { return pages.size() > 1; }
};

class SelectionController : public QObject {
    Q_OBJECT

public:
    explicit SelectionController(QObject* parent = nullptr);
    ~SelectionController() override;

    void openDocument(const QString& path, const QString& password = {});
    void closeDocument();

    // 座標一律是 PDF 頁面空間（點，原點左下）。呼叫端負責用 PageTransform 換算，
    // 不要在這裡再翻一次 Y 軸。
    void beginSelection(std::int32_t pageIndex, const domain::PointF& pagePoint);
    void extendSelection(std::int32_t pageIndex, const domain::PointF& pagePoint);
    void selectWordAt(std::int32_t pageIndex, const domain::PointF& pagePoint);
    void selectLineAt(std::int32_t pageIndex, const domain::PointF& pagePoint);
    // 選取整頁的文字（PRD-TXT-002 的 Ctrl+A）。
    //
    // 刻意只選一頁而不是整份文件：500 頁的全選會把整份文字讀進記憶體再
    // 串成一個 QString，而使用者按 Ctrl+A 的意圖幾乎一定是「這一頁」。
    // 真要整份文字有「匯出純文字」那條路。
    void selectAllOnPage(std::int32_t pageIndex);
    void clearSelection();

    // 全文搜尋（PRD-SRCH-001）。逐頁增量進行，結果一頁一頁送出，
    // 不等整份掃完才顯示——500 頁的文件那樣等於沒有回應。
    void search(const QString& query, std::int32_t startPage, bool matchCase, bool matchWholeWord);
    void cancelSearch();

    struct SearchHit {
        std::int32_t pageIndex{0};
        domain::TextRange range{};
        QString context;
        std::int32_t matchOffset{0};
        std::int32_t matchLength{0};
    };
    [[nodiscard]] const std::vector<SearchHit>& searchHits() const noexcept { return hits_; }

    // 跳到某一筆命中並選取它。
    void selectSearchHit(std::size_t index);

    [[nodiscard]] const Selection& selection() const noexcept { return selection_; }
    [[nodiscard]] bool isOpen() const noexcept { return open_; }

signals:
    void selectionChanged();
    void documentReady(bool ok, int error);
    void searchHitsChanged();
    void searchFinished(int totalMatches, bool cancelled);
    // 建索引的進度。UI 據此顯示「建立搜尋索引中」，而不是讓使用者以為已經可以搜。
    void searchIndexProgress(int indexedPages, int totalPages);

private:
    // 命中容差的單位是點而不是像素：手指與滑鼠的精準度不隨縮放改變，
    // 用像素會讓放大後極難選中、縮小後選到隔壁行。
    static constexpr double kHitTolerancePt = 4.0;

    void updateSelection(std::int32_t pageIndex, domain::TextRange range);
    // 跨頁選取（PRD-TXT-002）。收齊所有涉及的頁面之後才更新一次——逐頁更新
    // 會讓拖曳過程中每經過一頁就閃一次不完整的選取。
    void extendSelectionAcrossPages(std::int32_t endPage, std::int32_t endChar);
    // 把 pages 摘要成既有的單頁欄位，並串出完整文字。
    void mirrorSelectionSummary();
    void appendHits(std::int32_t pageIndex, const std::vector<domain::SearchResult>& results);
    // 開檔成功後在背景逐頁建索引。
    void startIndexing();
    // 索引是否已涵蓋整份文件。只有涵蓋完整時才可以拿它來搜。
    [[nodiscard]] bool indexIsComplete() const;

    std::unique_ptr<engine::text::TextExtractor> extractor_;
    std::unique_ptr<engine::text::SearchSession> search_;

    // 預建索引（ADR-005）。開檔後在背景逐頁建，建完之後搜尋就不再碰 PDFium——
    // 500 頁工程圖從 6688 毫秒降到 4.2 毫秒。
    //
    // 索引未建完時**不會**拿它來搜：不完整的索引會把「還沒建完」顯示成
    // 「找不到」，那是搜尋最不能犯的錯。這種情況退回逐頁搜尋，慢但正確。
    engine::text::TextIndex index_;
    std::unique_ptr<engine::text::TextIndexBuilder> indexBuilder_;
    QString path_;
    QString password_;
    std::vector<SearchHit> hits_;

    // 開檔完成前送進來的搜尋要記著，等文件就緒再跑。
    // SearchSession 在 start() 當下就讀頁數，那時還是 0，會直接回報「找不到」——
    // 使用者看到的是「搜尋沒東西」而不是「還在載入」，是最糟的一種錯誤回報。
    struct PendingSearch {
        QString query;
        std::int32_t startPage{0};
        bool matchCase{false};
        bool matchWholeWord{false};
        bool valid{false};
    };
    PendingSearch pending_{};
    Selection selection_{};
    std::int32_t anchorPage_{-1};
    std::int32_t anchorChar_{-1};
    bool open_{false};
};

}  // namespace alioth::app
