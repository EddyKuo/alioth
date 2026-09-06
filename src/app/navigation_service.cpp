#include "app/navigation_service.h"

#include <QCoreApplication>
#include <QFile>

#include <algorithm>
#include <cmath>

#include "engine/bookmarks/destination_codec.h"
#include "engine/objects/pdf_source_document.h"

namespace alioth::app {
namespace {

using domain::bookmarks::ZoomType;

QString zoomDescription(const DestinationRow& row) {
    switch (row.zoom) {
        case ZoomType::Fit:   return QCoreApplication::translate("NavigationService", "整頁");
        case ZoomType::FitH:  return QCoreApplication::translate("NavigationService", "符合頁寬");
        case ZoomType::FitV:  return QCoreApplication::translate("NavigationService", "符合頁高");
        case ZoomType::FitR:  return QCoreApplication::translate("NavigationService", "指定矩形");
        case ZoomType::FitB:  return QCoreApplication::translate("NavigationService", "符合內容");
        case ZoomType::FitBH: return QCoreApplication::translate("NavigationService", "符合內容寬");
        case ZoomType::FitBV: return QCoreApplication::translate("NavigationService", "符合內容高");
        case ZoomType::XYZ:   break;
    }
    if (row.inheritsZoom()) {
        return QCoreApplication::translate("NavigationService", "沿用目前倍率");
    }
    return QCoreApplication::translate("NavigationService", "%1%")
        .arg(QString::number(*row.zoomFactor * 100.0, 'f', 0));
}

}  // namespace

bool DestinationRow::inheritsZoom() const noexcept {
    if (zoom != ZoomType::XYZ) return false;
    // /XYZ 的倍率參數是 null 或 0 都代表「不變」。只判 has_value 會讓
    // 「0」被當成「縮到零倍」，畫面直接空白。
    if (!zoomFactor.has_value()) return true;
    return *zoomFactor <= 0.0;
}

QString DestinationRow::describe() const {
    if (broken) {
        return QCoreApplication::translate("NavigationService", "目標頁不存在");
    }
    return QCoreApplication::translate("NavigationService", "第 %1 頁 · %2")
        .arg(pageIndex + 1)
        .arg(zoomDescription(*this));
}

bool NavigationService::load(const QString& path, std::string& diagnostic) {
    rows_.clear();
    diagnostic.clear();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        diagnostic = "無法開啟檔案";
        return false;
    }
    const QByteArray raw = file.readAll();
    file.close();

    engine::objects::PdfSourceDocument source;
    std::string openDiagnostic;
    if (source.open(std::string(raw.constData(), static_cast<std::size_t>(raw.size())),
                    &openDiagnostic) != engine::objects::SourceStatus::Ok) {
        diagnostic = openDiagnostic.empty() ? "無法解析 PDF 結構" : openDiagnostic;
        return false;
    }
    if (source.encrypted()) {
        // 物件層通道對加密文件一律明確拒絕，不做部分解析後給出可能錯誤的清單。
        diagnostic = "加密文件不支援命名目標讀取";
        return false;
    }

    const engine::bookmarks::PageIndexMap pages(source);
    // 用 readNamedDestinationEntries 而不是 readNamedDestinations：後者會濾掉
    // 解析不到目標頁的項目，而面板要的正是「列出但不可點」。
    const std::vector<engine::bookmarks::NamedDestinationEntry> destinations =
        engine::bookmarks::readNamedDestinationEntries(source, pages);

    const auto pageCount = static_cast<std::int32_t>(pages.pageCount());
    rows_.reserve(destinations.size());
    for (const engine::bookmarks::NamedDestinationEntry& item : destinations) {
        DestinationRow row;
        row.name = QString::fromStdString(item.entry.name);
        row.pageIndex = item.entry.destination.pageIndex;
        row.zoom = item.entry.destination.zoom;
        row.zoomFactor = item.entry.destination.zoomFactor;
        row.broken = !item.resolved || row.pageIndex < 0 ||
                     (pageCount > 0 && row.pageIndex >= pageCount);
        rows_.push_back(std::move(row));
    }

    // 依頁序排。名稱字典序在工程文件上幾乎沒有意義（名稱多半是 BM_1、BM_10、
    // BM_2 這種機器產生的），頁序才對應使用者腦中的文件順序。
    std::stable_sort(rows_.begin(), rows_.end(),
                     [](const DestinationRow& a, const DestinationRow& b) {
                         if (a.broken != b.broken) return !a.broken;
                         return a.pageIndex < b.pageIndex;
                     });
    return true;
}

std::vector<DestinationRow> NavigationService::filter(const QString& needle) const {
    if (needle.isEmpty()) return rows_;
    std::vector<DestinationRow> result;
    for (const DestinationRow& row : rows_) {
        if (row.name.contains(needle, Qt::CaseInsensitive)) result.push_back(row);
    }
    return result;
}

const DestinationRow* NavigationService::findByName(const QString& name) const {
    for (const DestinationRow& row : rows_) {
        if (row.name == name) return &row;
    }
    return nullptr;
}

void AutoScroller::start() noexcept {
    active_ = true;
    accumulator_ = 0.0;
}

void AutoScroller::stop() noexcept {
    active_ = false;
    accumulator_ = 0.0;
}

void AutoScroller::toggle() noexcept {
    if (active_) {
        stop();
    } else {
        start();
    }
}

void AutoScroller::setSpeed(int speed) noexcept {
    speed_ = std::clamp(speed, kMinSpeed, kMaxSpeed);
}

double AutoScroller::pixelsPerSecond() const noexcept {
    if (speed_ <= 0) return 0.0;
    // 幾何級數而非線性：低速段要能細調（校對文字時每秒十幾像素才跟得上眼睛），
    // 高速段則是拿來快速掃過整份文件，兩者差一個數量級以上。
    // 第 1 級 12 px/s，第 10 級約 750 px/s。
    constexpr double kBase = 12.0;
    constexpr double kRatio = 1.5;
    return kBase * std::pow(kRatio, static_cast<double>(speed_ - 1));
}

int AutoScroller::tick(double elapsedMs) noexcept {
    if (!active_ || speed_ <= 0 || !(elapsedMs > 0.0)) return 0;
    // 視窗失焦後再回來，elapsed 可能是好幾秒。照算會讓畫面瞬間跳一大段，
    // 使用者完全失去閱讀位置，因此上限夾在 250 毫秒。
    const double bounded = std::min(elapsedMs, 250.0);
    accumulator_ += pixelsPerSecond() * bounded / 1000.0;

    const double whole = std::trunc(accumulator_);
    accumulator_ -= whole;
    const int pixels = static_cast<int>(whole);
    return reversed_ ? -pixels : pixels;
}

}  // namespace alioth::app
