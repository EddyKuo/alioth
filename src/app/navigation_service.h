#pragma once

// 命名目標面板與自動捲動（PRD-NAV-007、PRD-NAV-008）。
//
// 兩件事放在同一個檔案，是因為它們都屬於「怎麼在文件裡移動」而且都不需要
// 文件把手：命名目標由物件層讀（engine/bookmarks/destination_codec），
// 自動捲動則完全是時間與速度的運算。兩者都刻意做成可測的純邏輯，
// 由 UI 去驅動——把「速度怎麼算」寫進 widget 的 timer callback 裡，
// 就再也沒辦法驗證它了。

#include <QString>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "domain/bookmark_ops.h"

namespace alioth::app {

// ---------------------------------------------------------------------------
// PRD-NAV-007 命名目標面板
// ---------------------------------------------------------------------------

struct DestinationRow {
    QString name;
    std::int32_t pageIndex{0};
    domain::bookmarks::ZoomType zoom{domain::bookmarks::ZoomType::Fit};
    std::optional<double> zoomFactor;

    // 目標解析不出頁面時為 true。面板要顯示但不可跳——PDF 裡指向已刪除頁面的
    // 命名目標很常見（頁面被刪掉時沒人去清 /Dests），靜默跳到第一頁比報錯更糟。
    bool broken{false};

    // 「繼承縮放」：PRD-NAV-007 要求跳轉時若目標沒指定倍率就沿用目前倍率。
    // /XYZ 的第三個參數是 null 或 0 都代表「不變」，這在規格裡很容易讀漏。
    [[nodiscard]] bool inheritsZoom() const noexcept;

    // 面板第二欄的說明文字，例如「第 12 頁 · 符合頁寬」。
    [[nodiscard]] QString describe() const;
};

class NavigationService {
public:
    // 從檔案讀出全部命名目標。讀不到（檔案打不開、沒有 /Dests）回傳空清單並
    // 把原因寫進 diagnostic，不丟例外——面板空白時使用者需要知道是「這份文件
    // 沒有命名目標」還是「檔案讀不到」。
    bool load(const QString& path, std::string& diagnostic);

    void clear() noexcept { rows_.clear(); }

    [[nodiscard]] const std::vector<DestinationRow>& rows() const noexcept { return rows_; }

    // 名稱子字串比對，不分大小寫。
    [[nodiscard]] std::vector<DestinationRow> filter(const QString& needle) const;

    [[nodiscard]] const DestinationRow* findByName(const QString& name) const;

private:
    std::vector<DestinationRow> rows_;
};

// ---------------------------------------------------------------------------
// PRD-NAV-008 自動捲動
// ---------------------------------------------------------------------------

// 自動捲動的純邏輯。UI 每個 tick 餵進經過的毫秒數，拿回應該捲動的像素數。
//
// 為什麼要累積小數：以每秒 20 像素、每 16 毫秒一個 tick 來算，每次是 0.32 像素。
// 直接取整會變成 0，畫面完全不動，而使用者只會看到「這個功能壞了」。
// 因此餘數必須留著跨 tick 累加。
class AutoScroller {
public:
    // 速度分級對齊 Acrobat 的 0–10。第 0 級是停止而不是「很慢」，
    // 因為使用者按減速到底時的意圖就是停下來。
    static constexpr int kMinSpeed = 0;
    static constexpr int kMaxSpeed = 10;
    static constexpr int kDefaultSpeed = 3;

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] int speed() const noexcept { return speed_; }
    [[nodiscard]] bool reversed() const noexcept { return reversed_; }

    void start() noexcept;
    void stop() noexcept;
    void toggle() noexcept;

    // 超出範圍一律夾住而不是拒絕：這個值來自使用者連按方向鍵，
    // 夾在邊界是他預期的行為。
    void setSpeed(int speed) noexcept;
    void increaseSpeed() noexcept { setSpeed(speed_ + 1); }
    void decreaseSpeed() noexcept { setSpeed(speed_ - 1); }

    void setReversed(bool reversed) noexcept { reversed_ = reversed; }
    void toggleDirection() noexcept { reversed_ = !reversed_; }

    // 每秒像素數。第 0 級為 0。
    [[nodiscard]] double pixelsPerSecond() const noexcept;

    // 這個 tick 應該捲動的整數像素；向上捲時為負。未啟動或速度為 0 時回傳 0。
    // 呼叫端捲到文件末端時應該呼叫 stop()——這裡不知道文件多長。
    [[nodiscard]] int tick(double elapsedMs) noexcept;

    // 使用者手動捲動或跳頁後，殘留的小數要丟掉，否則下一個 tick 會補上
    // 一段與新位置無關的位移。
    void resetAccumulator() noexcept { accumulator_ = 0.0; }

private:
    bool active_{false};
    bool reversed_{false};
    int speed_{kDefaultSpeed};
    double accumulator_{0.0};
};

}  // namespace alioth::app
