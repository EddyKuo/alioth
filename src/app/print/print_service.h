#pragma once

// 列印服務（PRD-IO-008、WBS 5.9）。
//
// 分層立場：本檔屬於應用層，可以用 Qt，但不得直接呼叫 PDFium。頁面點陣一律
// 向 PdfiumEngine 索取。
//
// 為什麼自己持有一個 PdfiumEngine 而不是共用檢視器的那一個：PDFium 非執行緒
// 安全，同一份文件把手同時只能有一個執行緒觸碰。列印是長時間、大量、與使用者
// 捲動無關的渲染，塞進檢視器的佇列會讓畫面在整個列印期間卡住。CLAUDE.md 對
// 這種情況指定的正確做法就是另開一個引擎實體，各自持有同一檔案的獨立把手。
//
// 解析度：列印**不能**沿用螢幕圖磚的倍率。螢幕圖磚是為 96 dpi 算的，
// 印表機是 300–600 dpi，直接把螢幕點陣放大貼上去，細線會斷、小字會糊。
// 因此每一張紙都以印表機解析度重新渲染，並且仍然走 512×512 圖磚
// （PRD-VIEW-001 禁止整頁光柵化，A0 圖在 600 dpi 下整頁點陣是數 GB）。

#include <QPrinter>
#include <QSizeF>
#include <QString>

#include <memory>
#include <vector>

#include "app/print/print_plan.h"

namespace alioth::engine {
class PdfiumEngine;
}

namespace alioth::app::print {

struct PrintResult {
    bool ok{false};
    QString message;
    int sheetsPrinted{0};
    // 實際印出的 Bates 序列，依紙張順序。回傳而不是只記在日誌裡，
    // 是為了讓上層能把它寫進送達證明——法務流程需要「這批印了哪些號碼」。
    std::vector<QString> batesNumbers;
};

class PrintService {
public:
    PrintService();
    ~PrintService();

    PrintService(const PrintService&) = delete;
    PrintService& operator=(const PrintService&) = delete;

    // 同步開檔。列印通常由強制回應對話框驅動，非同步在此只會讓流程更難推理。
    [[nodiscard]] bool openDocument(const QString& path, const QString& password = {},
                                    QString* error = nullptr);
    void closeDocument();

    [[nodiscard]] bool isOpen() const noexcept { return pageCount_ > 0; }
    [[nodiscard]] int pageCount() const noexcept { return pageCount_; }

    // 頁面尺寸（點）。未開檔或索引越界回傳空尺寸。
    [[nodiscard]] QSizeF pageSizePt(int pageIndex) const;

    // 只算計畫不送紙。列印對話框的「共 N 張」預覽與自動化測試都用它。
    [[nodiscard]] PrintPlan planFor(const QPrinter& printer, const PrintOptions& options);

    // 執行列印。printer 已由呼叫端設定好紙張、方向與輸出目標
    // （輸出成 PDF 時設 QPrinter::PdfFormat 與 outputFileName）。
    PrintResult print(QPrinter& printer, const PrintOptions& options);

    // 印表機可列印區，單位為點，原點固定為 (0, 0)。
    //
    // 原點歸零是刻意的：QPainter 畫在 QPrinter 上時，裝置座標原點就是可列印區的
    // 左上角，而 QPageLayout 回報的矩形原點是相對於紙張邊緣的邊距。兩者混用會讓
    // 所有內容多偏移一個邊距，而且在無邊列印時偏移量為零，測不出來。
    [[nodiscard]] static QRectF printableRectPt(const QPrinter& printer);

private:
    // 把一頁的指定區域以印表機解析度畫到 painter 上。回傳 false 表示渲染失敗。
    bool renderSheet(QPainter& painter, const SheetPlan& sheet, const PrintOptions& options,
                     double deviceScale);

    std::unique_ptr<engine::PdfiumEngine> engine_;
    QString path_;
    int pageCount_{0};
    // 需要時才向引擎詢問，索引與文件頁次一致。整份文件一次問完在萬頁文件上
    // 是上萬次佇列往返，而列印通常只碰其中幾頁。
    mutable std::vector<QSizeF> pageSizes_;
};

}  // namespace alioth::app::print
