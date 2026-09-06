#include "app/print/print_service.h"

#include <QDateTime>
#include <QFileInfo>
#include <QImage>
#include <QPageLayout>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <future>

#include "app/print/stamp_painter.h"
#include "domain/tile.h"
#include "engine/pdfium_engine.h"

namespace alioth::app::print {
namespace {

constexpr double kEpsilon = 1e-9;
constexpr double kPointsPerInch = 72.0;

QPrinter::DuplexMode toQtDuplex(DuplexMode mode) {
    switch (mode) {
        case DuplexMode::None:
            return QPrinter::DuplexNone;
        case DuplexMode::LongSide:
            return QPrinter::DuplexLongSide;
        case DuplexMode::ShortSide:
            return QPrinter::DuplexShortSide;
    }
    return QPrinter::DuplexNone;
}

QRectF toDevice(const QRectF& rectPt, double deviceScale) {
    return QRectF{rectPt.left() * deviceScale, rectPt.top() * deviceScale,
                  rectPt.width() * deviceScale, rectPt.height() * deviceScale};
}

}  // namespace

PrintService::PrintService() = default;
PrintService::~PrintService() = default;

bool PrintService::openDocument(const QString& path, const QString& password, QString* error) {
    closeDocument();
    engine_ = std::make_unique<engine::PdfiumEngine>();

    std::promise<engine::OpenResult> promise;
    auto future = promise.get_future();
    engine_->openDocument(path.toStdString(), password.toStdString(),
                          [&promise](engine::OpenResult result) { promise.set_value(result); });
    const engine::OpenResult result = future.get();

    if (!result.ok()) {
        if (error) *error = QString::fromUtf8(domain::describe(result.error));
        engine_.reset();
        return false;
    }

    path_ = path;
    pageCount_ = result.info.pageCount;
    pageSizes_.assign(static_cast<std::size_t>(std::max(0, pageCount_)), QSizeF{});
    return pageCount_ > 0;
}

void PrintService::closeDocument() {
    // 直接銷毀引擎而不是先關文件：解構會 join 引擎執行緒，順序反過來的話
    // 回呼有機會在成員已被清空之後才執行。
    engine_.reset();
    path_.clear();
    pageCount_ = 0;
    pageSizes_.clear();
}

QSizeF PrintService::pageSizePt(int pageIndex) const {
    if (!engine_ || pageIndex < 0 || pageIndex >= pageCount_) return QSizeF{};
    QSizeF& cached = pageSizes_[static_cast<std::size_t>(pageIndex)];
    if (!cached.isEmpty()) return cached;

    std::promise<std::optional<domain::PageInfo>> promise;
    auto future = promise.get_future();
    engine_->pageInfo(pageIndex, [&promise](std::optional<domain::PageInfo> info) {
        promise.set_value(info);
    });
    const std::optional<domain::PageInfo> info = future.get();
    if (info) cached = QSizeF{info->sizePt.width, info->sizePt.height};
    return cached;
}

QRectF PrintService::printableRectPt(const QPrinter& printer) {
    const QRectF paint = printer.pageLayout().paintRectPoints();
    return QRectF{QPointF{0.0, 0.0}, paint.size()};
}

PrintPlan PrintService::planFor(const QPrinter& printer, const PrintOptions& options) {
    PrintPlan invalid;
    if (!isOpen()) {
        invalid.valid = false;
        invalid.diagnostic = QStringLiteral("尚未開啟文件");
        return invalid;
    }

    // 只查會被印到的頁面的尺寸。先解析一次範圍，再把尺寸填進去；
    // buildPrintPlan 會用同一份選項再解析一次，結果必然相同（純函式）。
    const PageRangeResult range = parsePageRange(options.pageRangeSpec, pageCount_, options.subset);
    if (!range.valid) {
        invalid.valid = false;
        invalid.diagnostic = range.diagnostic;
        return invalid;
    }

    std::vector<QSizeF> sizes(static_cast<std::size_t>(pageCount_), QSizeF{});
    for (const int page : range.pages) {
        sizes[static_cast<std::size_t>(page)] = pageSizePt(page);
    }

    return buildPrintPlan(sizes, printableRectPt(printer), options);
}

bool PrintService::renderSheet(QPainter& painter, const SheetPlan& sheet,
                               const PrintOptions& options, double deviceScale) {
    const QSizeF pageSize = pageSizePt(sheet.pageIndex);
    if (pageSize.isEmpty()) return false;

    const double sourceWidth = sheet.rotate90 ? pageSize.height() : pageSize.width();
    const double sourceHeight = sheet.rotate90 ? pageSize.width() : pageSize.height();
    if (sourceWidth <= kEpsilon || sourceHeight <= kEpsilon) return false;

    const QRectF destDevice = toDevice(sheet.pageRectPt, deviceScale);
    const QRectF clipDevice = toDevice(sheet.clipRectPt, deviceScale).intersected(destDevice);
    if (destDevice.isEmpty() || clipDevice.isEmpty()) return false;

    // 這裡是「不能沿用螢幕倍率」的落點：倍率由印表機解析度反推，
    // 讓一個渲染像素恰好對應一個印表機點，不放大也不縮小。
    double renderScale = destDevice.width() / sourceWidth;

    // 上限保護。600 dpi 的 A0 整頁是十億級像素，即使切成圖磚也會讓
    // 列印時間爆炸。超出上限時整體降階並由 QPainter 平滑放大——結果比
    // 螢幕倍率直上好得多，但仍是降級，所以門檻是顯式選項而不是暗規則。
    const double megapixels = destDevice.width() * destDevice.height() / 1.0e6;
    if (options.maxRenderMegapixels > 0.0 && megapixels > options.maxRenderMegapixels) {
        renderScale *= std::sqrt(options.maxRenderMegapixels / megapixels);
    }

    std::int32_t scaleKey = domain::exactScaleKey(renderScale);
    if (scaleKey <= 0) scaleKey = 1;  // 倍率鍵為 0 會讓引擎直接放棄渲染
    const double actualScale = domain::scaleOfKey(scaleKey);

    const auto pixelWidth = static_cast<std::int32_t>(
        std::max(1.0, std::floor(sourceWidth * actualScale + 0.5)));
    const auto pixelHeight = static_cast<std::int32_t>(
        std::max(1.0, std::floor(sourceHeight * actualScale + 0.5)));

    const std::int32_t columns = (pixelWidth + domain::kTileSize - 1) / domain::kTileSize;
    const std::int32_t rows = (pixelHeight + domain::kTileSize - 1) / domain::kTileSize;

    const double sx = destDevice.width() / static_cast<double>(pixelWidth);
    const double sy = destDevice.height() / static_cast<double>(pixelHeight);

    engine::RenderOptions renderOptions;
    renderOptions.drawAnnotations = options.includeAnnotations;
    // 子像素抗鋸齒是為 RGB 條紋排列的螢幕設計的，印到紙上只會變成彩色鑲邊。
    renderOptions.lcdText = false;
    renderOptions.nightMode = false;

    const domain::Rotation rotation =
        sheet.rotate90 ? domain::Rotation::Cw90 : domain::Rotation::None;

    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setClipRect(clipDevice);
    painter.translate(destDevice.topLeft());
    painter.scale(sx, sy);

    bool ok = true;
    for (std::int32_t row = 0; row < rows && ok; ++row) {
        for (std::int32_t column = 0; column < columns && ok; ++column) {
            const QRectF tileDevice{
                destDevice.left() + column * domain::kTileSize * sx,
                destDevice.top() + row * domain::kTileSize * sy,
                domain::kTileSize * sx, domain::kTileSize * sy};
            // 海報分割時絕大多數圖磚落在別張紙上，先篩掉才不會為了一張紙
            // 渲染整頁——那正是海報功能最容易變成效能災難的地方。
            if (!tileDevice.intersects(clipDevice)) continue;

            const domain::TileKey key{sheet.pageIndex, scaleKey, column, row, rotation, false};

            std::promise<engine::RenderResult> promise;
            auto future = promise.get_future();
            engine_->renderTile(key, renderOptions, domain::TaskPriority::Background,
                                engine::CancellationToken{},
                                [&promise](engine::RenderResult result) {
                                    promise.set_value(std::move(result));
                                });
            const engine::RenderResult result = future.get();
            if (!result.ok()) {
                ok = false;
                break;
            }

            const engine::PixelBuffer& buffer = *result.buffer;
            // 零複製：QImage 直接包住引擎緩衝區，stride 顯式帶入。
            // 假設為「寬 × 4」在對齊不同時會畫出斜切的頁面，而不是崩潰。
            const QImage image(buffer.data(), buffer.width(), buffer.height(),
                               static_cast<qsizetype>(buffer.stride()),
                               QImage::Format_ARGB32_Premultiplied);
            painter.drawImage(QPointF{static_cast<double>(column) * domain::kTileSize,
                                      static_cast<double>(row) * domain::kTileSize},
                              image);
        }
    }

    painter.restore();
    return ok;
}

PrintResult PrintService::print(QPrinter& printer, const PrintOptions& options) {
    PrintResult out;
    if (!isOpen()) {
        out.message = QStringLiteral("尚未開啟文件");
        return out;
    }

    const PrintPlan plan = planFor(printer, options);
    if (!plan.valid || plan.sheets.empty()) {
        out.message = plan.diagnostic.isEmpty() ? QStringLiteral("沒有可列印的內容")
                                                : plan.diagnostic;
        return out;
    }

    if (options.stamps.bates.enabled) {
        // 送紙前先驗序列。印完才發現重號等於整批作廢重印，
        // 而在法務流程裡重印還要重新走一次揭示紀錄。
        const std::vector<QString> numbers = batesSequence(options.stamps.bates, plan.pages);
        if (!isBatesSequenceUnique(numbers)) {
            out.message = QStringLiteral("Bates 序列出現重號（遞增量為 0 或範圍重疊）");
            return out;
        }
    }

    printer.setCopyCount(std::max(1, options.copies));
    printer.setDuplex(toQtDuplex(options.duplex));
    // 範圍已在計畫階段套用。若同時交給印表機自己的範圍設定會被過濾兩次，
    // 症狀是「選了第 3-5 頁卻只印到第 3 頁」。
    printer.setPrintRange(QPrinter::AllPages);
    if (options.renderDpi > 0) printer.setResolution(options.renderDpi);

    // 一律以印表機回報的解析度換算，而不是以我們要求的值：驅動程式可以拒絕
    // 設定解析度，此時照著要求值算會讓整頁內容縮放錯誤。
    const double deviceScale = printer.resolution() / kPointsPerInch;
    if (deviceScale <= kEpsilon) {
        out.message = QStringLiteral("印表機解析度無效");
        return out;
    }

    const QRectF printable = printableRectPt(printer);
    // 整批共用同一個時間戳。逐頁取當下時間會讓跨午夜的長列印在同一批紙上
    // 出現兩個日期，稽核時看起來像是兩次列印。
    const QDateTime timestamp = QDateTime::currentDateTime();
    const QString fileName = QFileInfo(path_).fileName();

    QPainter painter;
    if (!painter.begin(&printer)) {
        out.message = QStringLiteral("無法開始列印工作");
        return out;
    }

    bool ok = true;
    for (std::size_t i = 0; i < plan.sheets.size(); ++i) {
        if (i > 0 && !printer.newPage()) {
            ok = false;
            out.message = QStringLiteral("印表機拒絕換頁");
            break;
        }

        const SheetPlan& sheet = plan.sheets[i];
        if (!renderSheet(painter, sheet, options, deviceScale)) {
            ok = false;
            out.message = QStringLiteral("第 %1 頁渲染失敗").arg(sheet.pageIndex + 1);
            break;
        }

        StampContext context;
        context.pageNumber = sheet.pageIndex + 1;
        context.pageCount = pageCount_;
        context.printSequence = sheet.printSequence + 1;
        context.sheetNumber = static_cast<int>(i) + 1;
        context.batesText = sheet.batesText;
        context.fileName = fileName;
        context.timestamp = timestamp;

        paintStamps(painter, printable, options.stamps, context, deviceScale);
        if (!sheet.batesText.isEmpty()) out.batesNumbers.push_back(sheet.batesText);
        ++out.sheetsPrinted;
    }

    painter.end();
    out.ok = ok;
    if (ok) out.message = QStringLiteral("已送出 %1 張").arg(out.sheetsPrinted);
    return out;
}

}  // namespace alioth::app::print
