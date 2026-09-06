#include "app/history_store.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>

#include "platform/paths.h"

#include <algorithm>

namespace alioth::app {
namespace {

constexpr const char* kKey = "history/entries";

QJsonObject markToJson(const ReadingMark& mark) {
    QJsonObject object;
    object[QStringLiteral("page")] = mark.pageIndex;
    if (!mark.label.isEmpty()) object[QStringLiteral("label")] = mark.label;
    // 時間以 ISO 8601 存。存 epoch 秒比較短，但歷史檔偶爾要人工看，
    // 而且時區資訊在 epoch 裡會遺失。
    if (mark.createdAt.isValid()) {
        object[QStringLiteral("createdAt")] = mark.createdAt.toString(Qt::ISODate);
    }
    return object;
}

std::optional<ReadingMark> markFromJson(const QJsonValue& value) {
    if (!value.isObject()) return std::nullopt;
    const QJsonObject object = value.toObject();
    if (!object.value(QStringLiteral("page")).isDouble()) return std::nullopt;
    ReadingMark mark;
    mark.pageIndex = object.value(QStringLiteral("page")).toInt(-1);
    if (mark.pageIndex < 0) return std::nullopt;
    mark.label = object.value(QStringLiteral("label")).toString();
    mark.createdAt =
        QDateTime::fromString(object.value(QStringLiteral("createdAt")).toString(), Qt::ISODate);
    return mark;
}

}  // namespace

bool HistoryEntry::fileExists() const {
    return !path.isEmpty() && QFileInfo::exists(path);
}

QString normalizeDocumentPath(const QString& path) {
    // 大小寫折疊與符號連結解析屬於作業系統差異，收在平台層。
    // 在這裡寫 #ifdef _WIN32 就是把平台差異散進應用層。
    return platform::canonicalFileKey(path);
}

void HistoryStore::recordOpen(const QString& path, const QString& title, std::int32_t pageCount) {
    const QString key = normalizeDocumentPath(path);
    if (key.isEmpty()) return;

    HistoryEntry entry;
    if (HistoryEntry* existing = findMutable(key)) {
        entry = *existing;
        entries_.erase(entries_.begin() +
                       (existing - entries_.data()));
    }
    entry.path = key;
    entry.title = title.isEmpty() ? QFileInfo(path).fileName() : title;
    entry.pageCount = pageCount;
    entry.lastOpened = QDateTime::currentDateTime();

    entries_.insert(entries_.begin(), std::move(entry));
    if (static_cast<int>(entries_.size()) > kMaxEntries) {
        entries_.resize(static_cast<std::size_t>(kMaxEntries));
    }
}

bool HistoryStore::updatePosition(const QString& path, std::int32_t pageIndex, double scale) {
    HistoryEntry* entry = findMutable(normalizeDocumentPath(path));
    if (entry == nullptr) return false;
    if (pageIndex < 0) return false;
    // 頁數已知時越界的位置一律拒絕。存進去的話下次開檔會捲到不存在的頁，
    // 而那個症狀離這裡很遠，很難查。
    if (entry->pageCount > 0 && pageIndex >= entry->pageCount) return false;
    if (!(scale > 0.0)) return false;
    entry->lastPageIndex = pageIndex;
    entry->lastScale = scale;
    return true;
}

bool HistoryStore::addMark(const QString& path, const ReadingMark& mark) {
    if (!mark.isValid()) return false;
    HistoryEntry* entry = findMutable(normalizeDocumentPath(path));
    if (entry == nullptr) return false;
    if (entry->pageCount > 0 && mark.pageIndex >= entry->pageCount) return false;
    if (static_cast<int>(entry->marks.size()) >= kMaxMarksPerDocument) return false;

    ReadingMark stored = mark;
    if (!stored.createdAt.isValid()) stored.createdAt = QDateTime::currentDateTime();

    // 同一頁重複加書籤視為更新標籤，不是新增第二筆。
    // 使用者按兩次 Ctrl+B 的意圖是「這頁有書籤」，不是「這頁有兩個書籤」。
    const auto it = std::find_if(entry->marks.begin(), entry->marks.end(),
                                 [&](const ReadingMark& existing) {
                                     return existing.pageIndex == stored.pageIndex;
                                 });
    if (it != entry->marks.end()) {
        *it = stored;
        return true;
    }

    const auto position =
        std::lower_bound(entry->marks.begin(), entry->marks.end(), stored,
                         [](const ReadingMark& a, const ReadingMark& b) {
                             return a.pageIndex < b.pageIndex;
                         });
    entry->marks.insert(position, stored);
    return true;
}

bool HistoryStore::removeMark(const QString& path, std::int32_t pageIndex) {
    HistoryEntry* entry = findMutable(normalizeDocumentPath(path));
    if (entry == nullptr) return false;
    const auto it = std::find_if(
        entry->marks.begin(), entry->marks.end(),
        [pageIndex](const ReadingMark& mark) { return mark.pageIndex == pageIndex; });
    if (it == entry->marks.end()) return false;
    entry->marks.erase(it);
    return true;
}

std::vector<ReadingMark> HistoryStore::marks(const QString& path) const {
    const HistoryEntry* entry = find(path);
    return entry != nullptr ? entry->marks : std::vector<ReadingMark>{};
}

bool HistoryStore::remove(const QString& path) {
    const QString key = normalizeDocumentPath(path);
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [&](const HistoryEntry& entry) { return entry.path == key; });
    if (it == entries_.end()) return false;
    entries_.erase(it);
    return true;
}

const HistoryEntry* HistoryStore::find(const QString& path) const {
    const QString key = normalizeDocumentPath(path);
    for (const HistoryEntry& entry : entries_) {
        if (entry.path == key) return &entry;
    }
    return nullptr;
}

HistoryEntry* HistoryStore::findMutable(const QString& path) {
    for (HistoryEntry& entry : entries_) {
        if (entry.path == path) return &entry;
    }
    return nullptr;
}

std::vector<HistoryEntry> HistoryStore::search(const QString& needle) const {
    if (needle.isEmpty()) return entries_;
    std::vector<HistoryEntry> result;
    for (const HistoryEntry& entry : entries_) {
        const bool hit = entry.title.contains(needle, Qt::CaseInsensitive) ||
                         entry.path.contains(needle, Qt::CaseInsensitive);
        if (hit) result.push_back(entry);
    }
    return result;
}

QJsonObject HistoryStore::toJson() const {
    QJsonArray array;
    for (const HistoryEntry& entry : entries_) {
        QJsonObject object;
        object[QStringLiteral("path")] = entry.path;
        object[QStringLiteral("title")] = entry.title;
        object[QStringLiteral("pageCount")] = entry.pageCount;
        object[QStringLiteral("lastPage")] = entry.lastPageIndex;
        object[QStringLiteral("lastScale")] = entry.lastScale;
        if (entry.lastOpened.isValid()) {
            object[QStringLiteral("lastOpened")] = entry.lastOpened.toString(Qt::ISODate);
        }
        if (!entry.marks.empty()) {
            QJsonArray marks;
            for (const ReadingMark& mark : entry.marks) marks.append(markToJson(mark));
            object[QStringLiteral("marks")] = marks;
        }
        array.append(object);
    }
    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("entries")] = array;
    return root;
}

HistoryStore HistoryStore::fromJson(const QJsonObject& object) {
    HistoryStore store;
    const QJsonArray array = object.value(QStringLiteral("entries")).toArray();
    for (const QJsonValue& value : array) {
        if (!value.isObject()) continue;
        const QJsonObject item = value.toObject();
        HistoryEntry entry;
        entry.path = item.value(QStringLiteral("path")).toString();
        if (entry.path.isEmpty()) continue;
        entry.title = item.value(QStringLiteral("title")).toString();
        entry.pageCount = item.value(QStringLiteral("pageCount")).toInt(0);
        entry.lastPageIndex = item.value(QStringLiteral("lastPage")).toInt(0);
        entry.lastScale = item.value(QStringLiteral("lastScale")).toDouble(1.0);
        if (!(entry.lastScale > 0.0)) entry.lastScale = 1.0;
        entry.lastOpened =
            QDateTime::fromString(item.value(QStringLiteral("lastOpened")).toString(), Qt::ISODate);
        for (const QJsonValue& markValue : item.value(QStringLiteral("marks")).toArray()) {
            if (const std::optional<ReadingMark> mark = markFromJson(markValue)) {
                entry.marks.push_back(*mark);
            }
        }
        store.entries_.push_back(std::move(entry));
        if (static_cast<int>(store.entries_.size()) >= kMaxEntries) break;
    }
    // 存檔時就是排序好的，但外部編輯過的檔案不能假設。
    std::stable_sort(store.entries_.begin(), store.entries_.end(),
                     [](const HistoryEntry& a, const HistoryEntry& b) {
                         return a.lastOpened > b.lastOpened;
                     });
    return store;
}

void HistoryStore::load() {
    QSettings settings;
    const QByteArray raw = settings.value(QLatin1String(kKey)).toByteArray();
    if (raw.isEmpty()) {
        entries_.clear();
        return;
    }
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(raw, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        entries_.clear();
        return;
    }
    *this = fromJson(document.object());
}

void HistoryStore::save() const {
    QSettings settings;
    settings.setValue(QLatin1String(kKey),
                      QJsonDocument(toJson()).toJson(QJsonDocument::Compact));
}

}  // namespace alioth::app
