#include "app/print/print_plan.h"

#include <algorithm>
#include <cmath>

namespace alioth::app::print {
namespace {

constexpr double kEpsilon = 1e-9;

[[nodiscard]] bool isUsable(const QSizeF& size) noexcept {
    return size.width() > kEpsilon && size.height() > kEpsilon &&
           std::isfinite(size.width()) && std::isfinite(size.height());
}

[[nodiscard]] double scaleFactorFor(const QSizeF& sourceSizePt, const QRectF& printableRectPt,
                                    const PrintOptions& options) {
    switch (options.scaleMode) {
        case ScaleMode::ActualSize:
            return 1.0;
        case ScaleMode::Custom: {
            const double percent = options.customScalePercent;
            if (!std::isfinite(percent) || percent <= kEpsilon) return 1.0;
            return percent / 100.0;
        }
        case ScaleMode::FitToPaper: {
            // 兩軸取同一個較小的倍率，這是「不得變形」的唯一保證；
            // 分軸縮放在紙張與頁面長寬比不同時會把圖壓扁，而工程圖與簽名
            // 一旦變形就失去可信度。
            const double fit = std::min(printableRectPt.width() / sourceSizePt.width(),
                                        printableRectPt.height() / sourceSizePt.height());
            if (!options.enlargeSmallPages) return std::min(fit, 1.0);
            return fit;
        }
    }
    return 1.0;
}

}  // namespace

QRectF computePageTargetRect(const QSizeF& pageSizePt, const QRectF& printableRectPt,
                             const PrintOptions& options, bool* rotated90Out) {
    if (rotated90Out) *rotated90Out = false;
    if (!isUsable(pageSizePt) || printableRectPt.width() <= kEpsilon ||
        printableRectPt.height() <= kEpsilon) {
        return QRectF{};
    }

    QSizeF source = pageSizePt;
    if (options.autoRotate) {
        const bool pageLandscape = source.width() > source.height();
        const bool paperLandscape = printableRectPt.width() > printableRectPt.height();
        if (pageLandscape != paperLandscape) {
            source = QSizeF{source.height(), source.width()};
            if (rotated90Out) *rotated90Out = true;
        }
    }

    double factor = scaleFactorFor(source, printableRectPt, options);
    if (options.poster.enabled) {
        const double zoom = options.poster.zoomPercent;
        if (std::isfinite(zoom) && zoom > kEpsilon) factor *= zoom / 100.0;
    }
    if (!std::isfinite(factor) || factor <= kEpsilon) return QRectF{};

    const QSizeF target{source.width() * factor, source.height() * factor};
    const QPointF topLeft{printableRectPt.left() + (printableRectPt.width() - target.width()) / 2.0,
                          printableRectPt.top() + (printableRectPt.height() - target.height()) / 2.0};
    return QRectF{topLeft, target};
}

std::vector<QRectF> computePosterSlices(const QSizeF& scaledPageSizePt, const QSizeF& sheetSizePt,
                                        double overlapPt, int* columnsOut, int* rowsOut) {
    std::vector<QRectF> slices;
    if (columnsOut) *columnsOut = 0;
    if (rowsOut) *rowsOut = 0;
    if (!isUsable(scaledPageSizePt) || !isUsable(sheetSizePt)) return slices;

    // 重疊必須小於紙張本身，否則步進為零，切片數會發散成無限迴圈。
    // 夾到半張紙是保守但可預期的行為，總比拒絕列印好。
    const double maxOverlap = std::min(sheetSizePt.width(), sheetSizePt.height()) / 2.0;
    const double overlap = std::clamp(std::isfinite(overlapPt) ? overlapPt : 0.0, 0.0, maxOverlap);

    const auto axisCount = [overlap](double total, double sheet) {
        if (total <= sheet + kEpsilon) return 1;
        const double step = sheet - overlap;
        if (step <= kEpsilon) return 1;
        return std::max(1, static_cast<int>(std::ceil((total - overlap) / step - kEpsilon)));
    };

    const int columns = axisCount(scaledPageSizePt.width(), sheetSizePt.width());
    const int rows = axisCount(scaledPageSizePt.height(), sheetSizePt.height());
    if (columnsOut) *columnsOut = columns;
    if (rowsOut) *rowsOut = rows;

    const double stepX = columns > 1 ? sheetSizePt.width() - overlap : sheetSizePt.width();
    const double stepY = rows > 1 ? sheetSizePt.height() - overlap : sheetSizePt.height();

    slices.reserve(static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows));
    for (int row = 0; row < rows; ++row) {
        const double y = row * stepY;
        const double h = std::min(sheetSizePt.height(), scaledPageSizePt.height() - y);
        for (int column = 0; column < columns; ++column) {
            const double x = column * stepX;
            const double w = std::min(sheetSizePt.width(), scaledPageSizePt.width() - x);
            slices.push_back(QRectF{x, y, std::max(0.0, w), std::max(0.0, h)});
        }
    }
    return slices;
}

PrintPlan buildPrintPlan(const std::vector<QSizeF>& pageSizesPt, const QRectF& printableRectPt,
                         const PrintOptions& options) {
    PrintPlan plan;
    const int pageCount = static_cast<int>(pageSizesPt.size());

    const PageRangeResult range = parsePageRange(options.pageRangeSpec, pageCount, options.subset);
    if (!range.valid) {
        plan.valid = false;
        plan.diagnostic = range.diagnostic;
        return plan;
    }
    if (printableRectPt.width() <= kEpsilon || printableRectPt.height() <= kEpsilon) {
        plan.valid = false;
        plan.diagnostic = QStringLiteral("紙張可列印區為空");
        return plan;
    }

    plan.pages = options.reverseOrder ? reversed(range.pages) : range.pages;

    // Bates 序列一次算完再逐頁取用。逐頁即算會讓「跳過某頁」的分支
    // 有機會悄悄改變後續號碼，而跳號正是這套編號最不能出的錯。
    std::vector<QString> bates;
    if (options.stamps.bates.enabled) {
        bates = batesSequence(options.stamps.bates, plan.pages);
    }

    const QSizeF sheetSize = printableRectPt.size();
    for (std::size_t i = 0; i < plan.pages.size(); ++i) {
        const int pageIndex = plan.pages[i];
        const QSizeF pageSize = pageSizesPt[static_cast<std::size_t>(pageIndex)];

        bool rotate90 = false;
        const QRectF target = computePageTargetRect(pageSize, printableRectPt, options, &rotate90);
        if (target.isEmpty()) {
            plan.valid = false;
            plan.diagnostic = QStringLiteral("第 %1 頁的尺寸無效，無法計算列印版面")
                                  .arg(pageIndex + 1);
            return plan;
        }

        SheetPlan base;
        base.pageIndex = pageIndex;
        base.printSequence = static_cast<int>(i);
        base.rotate90 = rotate90;
        if (!bates.empty()) base.batesText = bates[i];

        if (!options.poster.enabled) {
            base.pageRectPt = target;
            const QRectF clip = target.intersected(printableRectPt);
            base.clipRectPt = clip.isEmpty() ? printableRectPt : clip;
            plan.sheets.push_back(base);
            continue;
        }

        int columns = 0;
        int rows = 0;
        const std::vector<QRectF> slices =
            computePosterSlices(target.size(), sheetSize, options.poster.overlapPt, &columns, &rows);
        if (slices.empty()) {
            plan.valid = false;
            plan.diagnostic = QStringLiteral("第 %1 頁的海報分割無法計算").arg(pageIndex + 1);
            return plan;
        }

        for (std::size_t s = 0; s < slices.size(); ++s) {
            const QRectF& slice = slices[s];
            SheetPlan sheet = base;
            sheet.posterColumns = columns;
            sheet.posterRows = rows;
            sheet.posterColumn = static_cast<int>(s) % columns;
            sheet.posterRow = static_cast<int>(s) / columns;
            // 整頁的原點往左上移出紙張，讓「畫整頁再裁切」這一條路徑
            // 同時服務一般列印與海報分割。
            sheet.pageRectPt = QRectF{printableRectPt.topLeft() - slice.topLeft(), target.size()};
            sheet.clipRectPt = QRectF{printableRectPt.topLeft(), slice.size()};
            plan.sheets.push_back(sheet);
        }
    }

    if (plan.sheets.empty()) {
        plan.valid = false;
        plan.diagnostic = QStringLiteral("沒有任何可列印的紙張");
    }
    return plan;
}

}  // namespace alioth::app::print
