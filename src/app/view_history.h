#pragma once

// 瀏覽歷史的前進／後退（PRD-NAV-001 的最後一項）。
//
// 與 app/history_store.h 是兩件不同的事，不要混：那個記的是「開過哪些檔案」，
// 這個記的是「在這份文件裡去過哪些位置」。前者跨 session 存到磁碟，後者只活在
// 目前這份文件的生命週期裡。
//
// **設計上最容易做錯的一點是「什麼才算一次導覽」。** 如果每次捲動都記一筆，
// 使用者按一次後退只會退回幾十像素，而且要按上百次才回得到原本的位置——
// 那個功能等於沒有。因此只有**離散跳轉**才記錄：頁碼跳轉、書籤、命名目標、
// 搜尋命中、連結、縮圖點選。連續捲動與縮放不記。
//
// 另一個容易做錯的是回到同一個位置時的重複記錄（例如按書籤兩次）。
// 相同位置不重複入堆，否則後退鍵會有一次「按了沒反應」。
//
// 語意與瀏覽器一致：從歷史中間位置產生新的導覽時，前方的紀錄整段丟棄。

#include <cstdint>
#include <vector>

namespace alioth::app {

// 一個瀏覽位置。刻意只記頁碼與倍率，不記捲動位移——
// 使用者對「回到剛才那頁」的期待是頁，不是像素。記到像素會讓後退在
// 版面模式或視窗大小變過之後跳到看起來不對的地方。
struct ViewPosition {
    std::int32_t pageIndex{0};
    double scale{0.0};  // 0 表示不還原倍率，只跳頁

    friend bool operator==(const ViewPosition& a, const ViewPosition& b) noexcept {
        return a.pageIndex == b.pageIndex;
    }
};

class ViewHistory {
public:
    // 上限。歷史是輔助功能，不該無限成長；100 筆遠超過任何人會按後退的次數。
    static constexpr std::size_t kMaxEntries = 100;

    // 記錄一次離散跳轉。與目前位置相同時不記錄（回傳 false）。
    bool record(const ViewPosition& position);

    [[nodiscard]] bool canGoBack() const noexcept { return cursor_ > 0; }
    [[nodiscard]] bool canGoForward() const noexcept {
        return !entries_.empty() && cursor_ + 1 < entries_.size();
    }

    // 後退／前進一步並回傳新位置。不能動時回傳目前位置且不改變游標。
    ViewPosition goBack();
    ViewPosition goForward();

    [[nodiscard]] ViewPosition current() const;
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool isEmpty() const noexcept { return entries_.empty(); }

    void clear() noexcept;

private:
    std::vector<ViewPosition> entries_;
    // 指向 entries_ 裡「目前所在」的那一筆。空歷史時無意義。
    std::size_t cursor_{0};
};

}  // namespace alioth::app
