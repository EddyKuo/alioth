#pragma once

// 列印工作計畫（PRD-IO-008）。
//
// 把「要印哪幾張紙、每張紙上放頁面的哪一塊、放在什麼位置、戳什麼號碼」
// 完整算成一個資料結構，再交給 PrintService 執行。
//
// 這樣拆的理由是可驗證性：印表機在 CI 上不存在，但計畫是純資料，
// 張數、切片座標、縮放矩形、Bates 序列都可以逐項斷言。列印最貴的錯誤
// （少印一頁、海報接縫錯位、頁面被壓扁）全部發生在這一層。
//
// 座標系與 stamp_layout.h 一致：紙張座標、單位為點、原點左上、Y 軸向下。

#include <QRectF>
#include <QSizeF>
#include <QString>

#include <cstdint>
#include <vector>

#include "app/print/bates_numbering.h"
#include "app/print/page_range.h"
#include "app/print/stamp_layout.h"

namespace alioth::app::print {

enum class ScaleMode : std::uint8_t {
    FitToPaper,   // 等比縮放至可列印區，置中，絕不變形
    ActualSize,   // 100%，超出紙張的部分裁掉
    Custom,       // 自訂百分比
};

enum class DuplexMode : std::uint8_t {
    None,
    LongSide,
    ShortSide,
};

struct PosterOptions {
    bool enabled{false};
    // 海報放大倍率。與 ScaleMode 是獨立的一層：先由 ScaleMode 決定頁面在
    // 一張紙上的基準大小，再乘上這個倍率把它攤到多張紙。
    double zoomPercent{200.0};
    // 相鄰紙張的重疊寬度。裁切拼貼時沒有重疊就沒有容錯餘裕，
    // 印表機的可列印區誤差會直接變成接縫白線。
    double overlapPt{18.0};
};

struct PrintOptions {
    QString pageRangeSpec;  // 空字串代表全部
    PageSubset subset{PageSubset::All};
    bool reverseOrder{false};

    ScaleMode scaleMode{ScaleMode::FitToPaper};
    double customScalePercent{100.0};
    // 符合紙張時是否也把小頁面放大。關掉時小頁面維持原尺寸，
    // 適合「原尺寸優先、只縮不放」的工程圖情境。
    bool enlargeSmallPages{true};
    bool autoRotate{true};  // 頁面與紙張長邊方向不一致時旋轉 90 度

    bool includeAnnotations{true};  // 對應 RenderOptions::drawAnnotations
    DuplexMode duplex{DuplexMode::None};
    int copies{1};

    PosterOptions poster{};
    StampOptions stamps{};

    // 列印解析度。0 表示沿用印表機自己回報的解析度，非 0 則覆寫。
    //
    // 不論取哪一個，頁面都以該解析度**重新渲染**，絕不沿用螢幕圖磚：
    // 螢幕圖磚的倍率是為約 96 dpi 算的，放大到 300–600 dpi 的紙上，
    // 細線會斷、小字會糊，而那正是使用者最容易注意到的列印品質缺陷。
    int renderDpi{0};
    double maxRenderMegapixels{120.0};
};

// 一張紙。海報分割時同一個 pageIndex 會產生多筆。
struct SheetPlan {
    int pageIndex{0};      // 0-based 文件頁索引
    int printSequence{0};  // 0-based，該頁在列印集合中的次序
    int posterColumn{0};
    int posterRow{0};
    int posterColumns{1};
    int posterRows{1};

    // 縮放後的整張頁面在紙張座標中的位置與大小。海報分割時原點會落在
    // 紙張之外（負值），那是刻意的：繪製端只要照這個原點畫整頁再裁切，
    // 就不需要為海報寫第二套繪製路徑。
    QRectF pageRectPt;
    // 本張紙實際要畫出來的區域，等於裁切框。
    QRectF clipRectPt;

    // 頁面與紙張長邊方向不一致時，頁面轉 90 度再放。這裡只記旗標，
    // 實際轉向交給引擎在渲染時做——把已渲染的點陣再轉一次會多一次重採樣，
    // 300 dpi 以上的細線與小字經不起那一次。
    bool rotate90{false};

    QString batesText;
};

struct PrintPlan {
    bool valid{true};
    QString diagnostic;
    std::vector<int> pages;        // 0-based，已套用範圍、奇偶、反序
    std::vector<SheetPlan> sheets;

    [[nodiscard]] int sheetCount() const noexcept { return static_cast<int>(sheets.size()); }
};

// 依縮放模式算出頁面在紙張上的目標矩形。等比縮放，永不變形。
[[nodiscard]] QRectF computePageTargetRect(const QSizeF& pageSizePt, const QRectF& printableRectPt,
                                           const PrintOptions& options,
                                           bool* rotated90Out = nullptr);

// 海報切片。sheetSize 是可列印區大小；回傳的矩形位於「放大後頁面」的座標系，
// 原點為頁面左上角。
[[nodiscard]] std::vector<QRectF> computePosterSlices(const QSizeF& scaledPageSizePt,
                                                      const QSizeF& sheetSizePt, double overlapPt,
                                                      int* columnsOut = nullptr,
                                                      int* rowsOut = nullptr);

// 組出完整計畫。pageSizesPt 是文件每一頁的尺寸（點）。
[[nodiscard]] PrintPlan buildPrintPlan(const std::vector<QSizeF>& pageSizesPt,
                                       const QRectF& printableRectPt, const PrintOptions& options);

}  // namespace alioth::app::print
