#pragma once

// 版面計算（PRD-VIEW-004）。純 C++，不碰 Qt 也不碰 PDFium，因此可以完整單元測試。
//
// 職責是唯一的一件事：給定各頁尺寸、縮放、旋轉與版面模式，算出每一頁在
// 「文件裝置空間」中的矩形。所謂文件裝置空間，是把整份文件依版面攤平後的座標系，
// 原點在左上、Y 向下，捲軸的值直接就是這個空間的位移。
//
// 把它從呈現層抽出來的理由很實際：連續捲動、雙頁、封面獨立、右至左這四個維度
// 交叉起來有十幾種組合，塞在 paintEvent 裡沒有人能驗證它是對的。

#include <cstdint>
#include <vector>

#include "geometry.h"

namespace alioth::domain {

enum class LayoutMode : std::uint8_t {
    SinglePage,        // 單頁：一次只顯示一頁
    Continuous,        // 連續：所有頁面垂直排列
    TwoPage,           // 雙頁：一次顯示兩頁
    TwoPageContinuous, // 雙頁連續
    // Ribbon Layout（PRD-VIEW-011）：所有頁面排成一列，水平捲動。
    // 比對兩頁相鄰內容時比垂直連續好用得多——垂直連續模式下，
    // 相鄰兩頁在畫面上永遠隔著一整頁的高度。
    Horizontal,
};

struct LayoutOptions {
    LayoutMode mode{LayoutMode::Continuous};
    // 封面獨立：第一頁單獨佔一列，之後才兩頁一組。技術文件與書籍的慣例不同，
    // 所以必須可切換而不是寫死。
    bool coverPageSeparate{true};
    bool rightToLeft{false};  // PRD-VIEW-012，雙頁模式下生效
    double pageGapPx{12.0};   // 頁間間隙
};

// 一頁在文件裝置空間中的位置。
struct PagePlacement {
    std::int32_t pageIndex{0};
    RectI rect{};
};

class PageLayout {
public:
    // 設定頁面尺寸（點）。尺寸未知的頁面可以先給預設值，之後再更新重算。
    void setPageSizes(std::vector<SizeF> sizesPt);

    // 依目前的縮放、旋轉與選項重算。viewportWidth 用於水平置中。
    void update(double scale, Rotation rotation, const LayoutOptions& options,
                std::int32_t viewportWidth);

    [[nodiscard]] std::int32_t pageCount() const noexcept {
        return static_cast<std::int32_t>(placements_.size());
    }
    [[nodiscard]] SizeF contentSize() const noexcept { return contentSize_; }
    [[nodiscard]] const std::vector<PagePlacement>& placements() const noexcept {
        return placements_;
    }

    [[nodiscard]] RectI pageRect(std::int32_t pageIndex) const;

    // 與可視區相交的頁面。單頁模式下只會有目前這一頁。
    [[nodiscard]] std::vector<PagePlacement> visiblePages(const RectI& viewport) const;

    // 捲動位置對應的「當前頁」：可視區中點落在哪一頁。狀態列的頁碼用它。
    [[nodiscard]] std::int32_t pageAtViewportCenter(const RectI& viewport) const;

    // 把文件裝置空間的點換算成某一頁的頁內裝置座標。
    [[nodiscard]] PointF toPageLocal(std::int32_t pageIndex, const PointF& documentPoint) const;

    // 單頁 / 雙頁模式下，版面只包含目前這一組頁面，因此需要知道目前停在哪裡。
    void setCurrentPage(std::int32_t pageIndex) noexcept { currentPage_ = pageIndex; }
    [[nodiscard]] std::int32_t currentPage() const noexcept { return currentPage_; }

private:
    [[nodiscard]] SizeF deviceSizeOf(std::int32_t pageIndex) const;
    [[nodiscard]] std::vector<std::vector<std::int32_t>> buildRows() const;

    std::vector<SizeF> sizesPt_;
    std::vector<PagePlacement> placements_;
    SizeF contentSize_{};
    double scale_{1.0};
    Rotation rotation_{Rotation::None};
    LayoutOptions options_{};
    std::int32_t currentPage_{0};
};

}  // namespace alioth::domain
