#pragma once

// 觸控最佳化的純邏輯核心（PRD-UI-013）。
//
// 這裡刻意不碰任何 widget：命中區域大小、長按判定、雙指縮放的錨點計算
// 全部是可以離線單元測試的數學，widget（PageView）只負責把 QTouchEvent
// 的座標與時間戳記餵進來，再把結果套用到畫面上。
//
// 長按判定最容易做錯、也最容易讓使用者惱怒的一點：位移超過門檻就不該再算
// 長按，而要讓上層把它當拖曳處理。LongPressGesture 把「時間」與「位移」
// 兩個條件都做成明確的狀態轉移，不是只用一個 QTimer 卡時間了事。

#include <QPointF>
#include <QSizeF>

#include <cstdint>

namespace alioth::app {

// 觸控命中目標的度量。
//
// PRD-UI-013 要求「最小觸控目標 44×44 CSS px 等效」。CSS px 的定義基準是
// 96 DPI（== Qt 的 96 logicalDpi 基準），所以換算成裝置像素只需要按目前螢幕
// 的邏輯 DPI 等比例放大：44 * (logicalDpi / 96)。
struct TouchTargetMetrics {
    static constexpr double kReferenceCssPx = 44.0;
    static constexpr double kReferenceDpi = 96.0;

    // 回傳目前 DPI 下，最小觸控目標的邊長（裝置獨立像素）。
    //
    // 兩個函式都是 inline：Ribbon（alioth_ribbon）要用這個門檻，而那個 target
    // 刻意不連結 Alioth::app——見 src/ui/ribbon/CMakeLists.txt 的說明。把三行
    // 算術放進標頭，門檻仍然只有一份定義，也不必為此拉一條相依。
    [[nodiscard]] static double minimumTargetSizePx(double logicalDpi) {
        if (logicalDpi <= 0.0) logicalDpi = kReferenceDpi;
        return kReferenceCssPx * (logicalDpi / kReferenceDpi);
    }

    // 判斷一個候選命中區域是否達到最小觸控目標；小於門檻的按鈕、圖示在觸控
    // 模式下應該被放大而不是照舊使用滑鼠時期的尺寸。
    [[nodiscard]] static bool meetsMinimumTarget(const QSizeF& candidateSize, double logicalDpi) {
        const double minimum = minimumTargetSizePx(logicalDpi);
        return candidateSize.width() >= minimum && candidateSize.height() >= minimum;
    }
};

// 長按手勢判定。
//
// 使用方式：手指按下呼叫 press()；每次移動呼叫 move()；由 widget 的計時器
// 週期性呼叫 checkTimeout() 問「現在算不算長按觸發了」；手指放開呼叫 release()。
//
// 位移門檻與時間門檻互相獨立：只要在計時器判定觸發之前位移超過門檻，
// 這次手勢就永久失去成為長按的資格（改由呼叫端當拖曳處理），即使後來
// 手指移回原點也不會恢復——使用者的意圖在超過門檻的那一刻就已經是拖曳了。
class LongPressGesture {
public:
    struct Config {
        // 標準長按時長。Windows 觸控與多數行動平台的預設值落在 400–500 毫秒。
        std::int64_t pressDurationMs{500};
        // 位移容許誤差，以裝置獨立像素表示，數值取自一般觸控滑動門檻
        // （約 10 CSS px），呼叫端依 DPI 換算後傳入。
        double moveToleranceDevicePx{10.0};
    };

    explicit LongPressGesture(Config config = {});

    // 手指按下。任何進行中的追蹤狀態會被這次呼叫重置。
    void press(const QPointF& position, std::int64_t timestampMs);

    // 手指移動。若累積位移超過門檻，這次手勢即失去長按資格；之後
    // checkTimeout() 一律回傳 false，直到下一次 press()。
    void move(const QPointF& position, std::int64_t timestampMs);

    // 手指放開：長按資格立即結束（放開之後不該再觸發長按選單）。
    void release();

    // 詢問「現在算不算長按觸發」。同一次按壓只會在時長跨過門檻的那一次
    // 呼叫回傳 true，之後即使繼續呼叫也回傳 false——避免上層重複開兩次選單。
    [[nodiscard]] bool checkTimeout(std::int64_t nowMs);

    [[nodiscard]] bool isTracking() const noexcept { return tracking_; }
    [[nodiscard]] QPointF startPosition() const noexcept { return startPosition_; }

private:
    Config config_;
    bool tracking_{false};
    bool eligible_{false};  // 位移是否仍在容許範圍內
    bool fired_{false};
    QPointF startPosition_{};
    std::int64_t startTimestampMs_{0};
};

// 雙指縮放（Pinch）手勢的錨點與倍率計算。
//
// 錨點採用「目前兩指中點」而不是「手勢開始時的中點」：手指在縮放過程中通常
// 會漂移，畫面應該持續對齊使用者手指目前所在的位置，這與滑鼠滾輪縮放
// 對齊游標位置（PageView::applyZoomAnchored）是同一個使用者期待。
class PinchZoomTracker {
public:
    // 手勢開始：記錄兩指的初始距離與當時的顯示倍率，作為後續倍率換算的基準。
    void begin(const QPointF& point1, const QPointF& point2, double startScale);

    struct Update {
        double scale{1.0};
        QPointF anchor{};
    };

    // 依目前兩指位置算出建議的新倍率（未夾限），與應該對齊的錨點（兩指中點）。
    // 倍率是否要夾進 [kMinScale, kMaxScale] 之類的合法範圍由呼叫端
    // （PageView）負責，這裡只做手勢本身的數學，不認識檢視器的縮放上下限。
    [[nodiscard]] Update update(const QPointF& point1, const QPointF& point2) const;

private:
    double startDistance_{1.0};
    double startScale_{1.0};
};

}  // namespace alioth::app
