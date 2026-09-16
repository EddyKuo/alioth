#include "app/document_controller.h"

#include <QMetaObject>

#include <atomic>
#include <chrono>
#include <iterator>
#include <memory>
#include <thread>
#include <QTimer>

#include <algorithm>
#include <cmath>

#include "engine/layers/ocg_reader.h"

namespace alioth::app {
namespace {

// 可視區外要預取的圖磚圈數。1 圈足以蓋掉一般捲動速度，再多只是浪費渲染預算。
constexpr std::int32_t kPrefetchRings = 1;

}  // namespace

DocumentController::DocumentController(QObject* parent)
    : QObject(parent), engine_(std::make_unique<engine::PdfiumEngine>()), cache_() {
    qRegisterMetaType<domain::TileKey>("alioth::domain::TileKey");

    // 每 15 秒問一次「到期了嗎」，而不是每 2 分鐘直接存檔：
    // 到期判定屬於 AutosaveManager，計時器只負責提供時間刻度。
    autosaveTimer_ = new QTimer(this);
    autosaveTimer_->setInterval(15000);
    connect(autosaveTimer_, &QTimer::timeout, this, &DocumentController::runAutosaveIfDue);
    autosaveTimer_->start();
}

DocumentController::~DocumentController() {
    // 引擎執行緒的回呼會寫 cache_。成員的銷毀順序是宣告順序的反序，engine_ 宣告在最前面
    // 所以會最後才銷毀——那表示 cache_ 已經沒了，引擎執行緒卻還在往裡面塞圖磚。
    // 先明確關掉引擎（解構會 join 那條執行緒），其餘成員才安全。
    engine_.reset();
    // 同理：圖層解析執行緒完成時會用 this 呼叫 QMetaObject::invokeMethod，
    // 必須在 this 還完整存在時等它結束，不能留著讓它變成用後即焚的懸空指標。
    if (layerThread_.joinable()) layerThread_.join();
}

void DocumentController::openDocument(const QString& path, const QString& password) {
    path_ = path;
    viewportGeneration_.cancelAll();
    viewportGeneration_.reset();
    ++renderGeneration_;
    cache_.clear();
    pageSizes_.clear();
    open_ = false;

    engine_->openDocument(
        path.toStdString(), password.toStdString(), [this, path](engine::OpenResult result) {
            // 這裡是引擎執行緒。任何 UI 相關動作都必須排回 GUI 執行緒。
            QMetaObject::invokeMethod(
                this,
                [this, path, result] {
                    if (!result.ok()) {
                        emit documentOpenFailed(static_cast<int>(result.error),
                                                QString::fromUtf8(domain::describe(result.error)));
                        return;
                    }
                    info_ = result.info;
                    open_ = true;
                    pageSizes_.assign(static_cast<std::size_t>(info_.pageCount), domain::SizeF{});
                    loadPageGeometry();
                    emit documentOpened(path);
                    requestOutline();
                },
                Qt::QueuedConnection);
        });
}

void DocumentController::closeDocument() {
    viewportGeneration_.cancelAll();
    ++renderGeneration_;
    cache_.clear();
    pageSizes_.clear();
    outline_.clear();
    annotations_.clear();
    links_.clear();
    open_ = false;
    info_ = {};
    // 正常關檔後清掉自動儲存殘留，下次啟動才不會誤報「上次異常結束」。
    autosave_.clearSession(path_.toStdString());
    autosave_.markClean();
    engine_->closeDocument([this] {
        QMetaObject::invokeMethod(this, [this] { emit documentClosed(); }, Qt::QueuedConnection);
    });
}

void DocumentController::loadPageGeometry() {
    // 只先問前幾頁：版面計算需要真實尺寸，但一次問一萬頁會塞爆佇列。
    // 其餘頁面在捲動接近時才補（TODO WBS 2.3 頁面生命週期）。
    const std::int32_t probe = std::min<std::int32_t>(info_.pageCount, 32);
    for (std::int32_t i = 0; i < probe; ++i) {
        engine_->pageInfo(i, [this, i](std::optional<domain::PageInfo> info) {
            if (!info) return;
            const domain::SizeF size = info->sizePt;
            QMetaObject::invokeMethod(
                this,
                [this, i, size] {
                    if (i < static_cast<std::int32_t>(pageSizes_.size())) {
                        pageSizes_[static_cast<std::size_t>(i)] = size;
                        emit pageGeometryChanged();
                    }
                },
                Qt::QueuedConnection);
        });
    }
}

domain::SizeF DocumentController::pageSizePt(std::int32_t index) const {
    if (index < 0 || index >= static_cast<std::int32_t>(pageSizes_.size())) {
        return {};
    }
    const domain::SizeF size = pageSizes_[static_cast<std::size_t>(index)];
    // 尚未問到真實尺寸前先用 A4，避免版面在載入瞬間塌成 0 高度。
    return size.isEmpty() ? domain::SizeF{595.0, 842.0} : size;
}

void DocumentController::scheduleTiles(const std::vector<PageTileRequest>& pages,
                                       const TileScheduleOptions& options) {
    if (!open_) return;

    const bool zoomChanged = std::abs(options.scale - lastOptions_.scale) > 1e-9;
    const bool rotationChanged = options.rotation != lastOptions_.rotation;
    lastOptions_ = options;

    // 可視區一變，先取消所有還沒開始的預取與縮圖任務。
    // 可見圖磚不在取消之列——它們就算來自上一幀，畫出來也是對的。
    viewportGeneration_.cancelAll();
    viewportGeneration_.reset();
    engine_->discardPending(domain::TaskPriority::Prefetch);

    // 縮放過程中用粗略倍率共用圖磚，停止後改用精確倍率重算——
    // PRD §4.3 禁止把拉伸結果當成最終畫面。
    const std::int32_t scaleKey = options.zooming ? domain::coarseScaleKey(options.scale)
                                                  : domain::exactScaleKey(options.scale);

    if ((zoomChanged || rotationChanged) && !options.zooming) {
        // 只在縮放結束時清掉其他倍率：過程中清會把還在用的過場圖磚也丟掉。
        cache_.evictOtherScales(scaleKey);
    }

    for (const PageTileRequest& page : pages) {
        const domain::RectI& visible = page.visibleInPage;
        if (visible.isEmpty() || page.pageSize.isEmpty()) continue;

        const std::int32_t firstCol = std::max(0, visible.x) / domain::kTileSize;
        const std::int32_t firstRow = std::max(0, visible.y) / domain::kTileSize;
        const std::int32_t lastCol = (visible.right() - 1) / domain::kTileSize;
        const std::int32_t lastRow = (visible.bottom() - 1) / domain::kTileSize;

        const std::int32_t maxCol = (page.pageSize.width - 1) / domain::kTileSize;
        const std::int32_t maxRow = (page.pageSize.height - 1) / domain::kTileSize;

        for (std::int32_t row = firstRow; row <= lastRow; ++row) {
            for (std::int32_t col = firstCol; col <= lastCol; ++col) {
                if (row < 0 || col < 0 || row > maxRow || col > maxCol) continue;
                requestTile(domain::TileKey{page.pageIndex, scaleKey, col, row, options.rotation,
                                            renderOptions_.nightMode},
                            domain::TaskPriority::Visible);
            }
        }

        // 外圍一圈預取，讓捲動時邊捲邊補而不是捲到才開始算。
        if (options.zooming) continue;
        for (std::int32_t row = firstRow - kPrefetchRings; row <= lastRow + kPrefetchRings; ++row) {
            for (std::int32_t col = firstCol - kPrefetchRings; col <= lastCol + kPrefetchRings;
                 ++col) {
                if (row < 0 || col < 0 || row > maxRow || col > maxCol) continue;
                if (row >= firstRow && row <= lastRow && col >= firstCol && col <= lastCol) continue;
                requestTile(domain::TileKey{page.pageIndex, scaleKey, col, row, options.rotation,
                                            renderOptions_.nightMode},
                            domain::TaskPriority::Prefetch);
            }
        }
    }
}

void DocumentController::requestTile(const domain::TileKey& key, domain::TaskPriority priority) {
    if (cache_.find(key)) return;

    // 預取可以被下一次可視區變更取消；可見圖磚不給取消權杖，一律算完。
    const engine::CancellationToken token = priority == domain::TaskPriority::Visible
                                                ? engine::CancellationToken{}
                                                : viewportGeneration_.token();

    const auto generation = renderGeneration_;
    engine_->renderTile(key, renderOptions_, priority, token, [this, generation](engine::RenderResult result) {
        if (!result.ok()) return;
        QMetaObject::invokeMethod(this, [this, generation, result = std::move(result)] {
            // Old queued renders must not repopulate a cache cleared for a new
            // document or display option. Check and insert on the same thread.
            if (generation != renderGeneration_) return;
            cache_.insert(result.key, result.buffer);
            emit tileReady(result.key);
        }, Qt::QueuedConnection);
    });
}

QImage DocumentController::tileIfReady(const domain::TileKey& key) {
    engine::PixelBufferPtr buffer = cache_.find(key);
    if (!buffer) {
        requestTile(key, domain::TaskPriority::Visible);
        return {};
    }

    // 零複製包裝：QImage 直接指向引擎緩衝區。清理函式持有 shared_ptr 的一份拷貝，
    // 所以就算圖磚同時被 LRU 逐出，這個 QImage 指向的記憶體仍然有效。
    auto* keepAlive = new engine::PixelBufferPtr(buffer);
    return QImage(buffer->data(), buffer->width(), buffer->height(),
                  static_cast<qsizetype>(buffer->stride()), QImage::Format_ARGB32_Premultiplied,
                  [](void* p) { delete static_cast<engine::PixelBufferPtr*>(p); }, keepAlive);
}

void DocumentController::requestOutline() {
    if (!open_) return;
    engine_->outline([this](std::vector<domain::OutlineNode> roots) {
        QMetaObject::invokeMethod(
            this,
            [this, roots = std::move(roots)]() mutable {
                outline_ = std::move(roots);
                emit outlineReady();
            },
            Qt::QueuedConnection);
    });
}

void DocumentController::requestAnnotations(std::int32_t fromPage, std::int32_t toPage) {
    if (!open_) return;
    annotations_.clear();

    const std::int32_t first = std::max(0, fromPage);
    const std::int32_t last = std::min(info_.pageCount - 1, toPage);
    if (last < first) {
        emit annotationsReady();
        return;
    }

    // 每頁各自回報，最後一頁回來時才發訊號。若每頁都發，1,000 筆註解會讓
    // 列表重繪 N 次，而 PRD-ANN-008 的預算是整份載入 500 毫秒。
    auto remaining = std::make_shared<std::atomic<std::int32_t>>(last - first + 1);
    for (std::int32_t page = first; page <= last; ++page) {
        engine_->pageAnnotations(
            page, [this, remaining](std::vector<domain::AnnotationSummary> summaries) {
                QMetaObject::invokeMethod(
                    this,
                    [this, remaining, summaries = std::move(summaries)]() mutable {
                        annotations_.insert(annotations_.end(),
                                            std::make_move_iterator(summaries.begin()),
                                            std::make_move_iterator(summaries.end()));
                        if (remaining->fetch_sub(1) == 1) {
                            std::sort(annotations_.begin(), annotations_.end(),
                                      [](const domain::AnnotationSummary& a,
                                         const domain::AnnotationSummary& b) {
                                          if (a.pageIndex != b.pageIndex)
                                              return a.pageIndex < b.pageIndex;
                                          return a.indexOnPage < b.indexOnPage;
                                      });
                            emit annotationsReady();
                        }
                    },
                    Qt::QueuedConnection);
            });
    }
}

void DocumentController::requestLinks(std::int32_t pageIndex) {
    if (!open_ || links_.count(pageIndex) != 0) return;

    // 先放一個空項佔位，避免同一頁在回呼回來之前被重複請求。
    links_[pageIndex] = {};
    engine_->pageLinks(pageIndex, [this, pageIndex](std::vector<domain::LinkTarget> links) {
        QMetaObject::invokeMethod(
            this,
            [this, pageIndex, links = std::move(links)]() mutable {
                links_[pageIndex] = std::move(links);
                emit linksReady(pageIndex);
            },
            Qt::QueuedConnection);
    });
}

const std::vector<domain::LinkTarget>& DocumentController::linksForPage(
    std::int32_t pageIndex) const {
    static const std::vector<domain::LinkTarget> kEmpty;
    const auto it = links_.find(pageIndex);
    return it == links_.end() ? kEmpty : it->second;
}

void DocumentController::requestThumbnail(std::int32_t pageIndex, std::int32_t maxEdgePixels) {
    if (!open_) return;
    engine_->renderThumbnail(
        pageIndex, maxEdgePixels, viewportGeneration_.token(),
        [this, pageIndex](engine::RenderResult result) {
            if (!result.ok()) return;
            // 縮圖不進圖磚快取（鍵的語意不同），這裡直接複製一份成 QImage 交給 UI。
            // 縮圖數量多但單張很小，複製成本遠低於維護第二套快取的複雜度。
            const QImage image(result.buffer->data(), result.buffer->width(),
                               result.buffer->height(),
                               static_cast<qsizetype>(result.buffer->stride()),
                               QImage::Format_ARGB32_Premultiplied);
            const QImage detached = image.copy();
            QMetaObject::invokeMethod(
                this, [this, pageIndex, detached] { emit thumbnailReady(pageIndex, detached); },
                Qt::QueuedConnection);
        });
}

void DocumentController::requestLayers() {
    if (!open_) {
        layers_ = {};
        emit layersReady();
        return;
    }

    // 刻意用獨立執行緒而不是 engine_ 的 PDFium 工作佇列：這裡讀的是我們自己的
    // 物件剖析器，完全不碰文件把手，混進 PDFium 佇列只會白白排隊等渲染完成。
    //
    // 前一次請求若還沒結束就先 join：同一時間只需要最新一次的結果，
    // 而 std::thread 若在還 joinable 時被重新指派會直接 terminate。
    if (layerThread_.joinable()) layerThread_.join();
    const std::string path = path_.toStdString();
    layerThread_ = std::thread([this, path] {
        domain::OcgTree tree = engine::layers::loadOcgTree(path);
        QMetaObject::invokeMethod(
            this,
            [this, tree = std::move(tree)]() mutable {
                layers_ = std::move(tree);
                emit layersReady();
            },
            Qt::QueuedConnection);
    });
}

std::vector<std::int32_t> DocumentController::setLayerVisible(std::int32_t objectNumber,
                                                               bool visible) {
    const std::int32_t index = [&] {
        for (std::size_t i = 0; i < layers_.layers.size(); ++i) {
            if (layers_.layers[i].objectNumber == objectNumber) return static_cast<std::int32_t>(i);
        }
        return static_cast<std::int32_t>(-1);
    }();
    std::vector<std::int32_t> changed = domain::setLayerVisible(layers_, index, visible);
    if (!changed.empty()) emit layerVisibilityChanged();
    return changed;
}

void DocumentController::setDocumentDirty() {
    if (open_) autosave_.markDirty();
}

void DocumentController::runAutosaveIfDue() {
    if (!open_ || !autosave_.isDirty() || !autosave_.isDue()) return;

    const std::string path = path_.toStdString();
    engine_->withDocument([this, path](void* document) {
        if (!document) return;
        // 自動儲存在引擎執行緒上進行：文件把手只能在那條執行緒上使用。
        const engine::save::AutosaveResult result = autosave_.autosave(document, path);
        QMetaObject::invokeMethod(
            this,
            [this, result] {
                if (result.ok()) {
                    emit autosaved(QString::fromStdString(result.entry.autosavePath));
                } else {
                    emit autosaveFailed(QString::fromStdString(result.message));
                }
            },
            Qt::QueuedConnection);
    });
}

void DocumentController::setCacheBytes(std::size_t bytes) { cache_.setCapacityBytes(bytes); }

void DocumentController::setAutosaveSeconds(int seconds) {
    if (seconds <= 0) {
        autosaveTimer_->stop();
        return;
    }
    autosave_.setInterval(std::chrono::seconds(seconds));
    if (!autosaveTimer_->isActive()) autosaveTimer_->start();
}

void DocumentController::setNightMode(bool enabled) {
    if (renderOptions_.nightMode == enabled) return;
    renderOptions_.nightMode = enabled;
    ++renderGeneration_;
    cache_.clear();  // 夜間模式是圖磚鍵的一部分，舊圖磚全部作廢
    // 重新排程由呈現層負責：只有它知道現在看得到哪些頁。
    emit pageGeometryChanged();
}

void DocumentController::setCustomColors(bool enabled, const QColor& background,
                                         const QColor& text) {
    renderOptions_.customColors = enabled;
    renderOptions_.backgroundR = static_cast<std::uint8_t>(background.red());
    renderOptions_.backgroundG = static_cast<std::uint8_t>(background.green());
    renderOptions_.backgroundB = static_cast<std::uint8_t>(background.blue());
    renderOptions_.textR = static_cast<std::uint8_t>(text.red());
    renderOptions_.textG = static_cast<std::uint8_t>(text.green());
    renderOptions_.textB = static_cast<std::uint8_t>(text.blue());
    ++renderGeneration_;
    // 顏色不是圖磚鍵的一部分（那會讓每換一次配色就多一整份快取），
    // 因此必須手動清掉：不清的話換了顏色仍然顯示舊配色的圖磚。
    cache_.clear();
    emit pageGeometryChanged();
}

void DocumentController::setTransparencyGrid(bool enabled) {
    if (renderOptions_.transparencyGrid == enabled) return;
    renderOptions_.transparencyGrid = enabled;
    ++renderGeneration_;
    // 與夜間模式不同，這個選項不是圖磚鍵的一部分（見標頭的說明），
    // 所以一定要手動清快取，否則切換前用不透明白底烘出來的舊圖磚會繼續顯示。
    cache_.clear();
    emit pageGeometryChanged();
}

void DocumentController::setRenderQuality(bool grayscale, bool smoothPaths,
                                          bool smoothText, bool smoothImages) {
    if (renderOptions_.grayscale == grayscale && renderOptions_.strokeAdjust == !smoothPaths &&
        renderOptions_.smoothText == smoothText && renderOptions_.smoothImages == smoothImages) return;
    renderOptions_.grayscale = grayscale;
    renderOptions_.strokeAdjust = !smoothPaths;
    renderOptions_.smoothText = smoothText;
    renderOptions_.smoothImages = smoothImages;
    ++renderGeneration_;
    cache_.clear();
    emit pageGeometryChanged();
}

}  // namespace alioth::app
