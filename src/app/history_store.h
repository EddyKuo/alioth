#pragma once

// 文件歷史與使用者書籤（PRD-NAV-005、PRD-NAV-009）。
//
// 這一塊看起來只是「最近開過的檔案」，但它承擔兩件不同的事：
//   1. History 面板要回答「我上週看過的那份規範叫什麼」——那是搜尋與辨識的問題
//   2. 閱讀位置記憶要回答「我上次看到哪」——那是續讀的問題
// 兩者共用同一筆記錄，但失效方式不同：檔案被移走時第 1 項仍然有價值
// （使用者要靠標題想起那是什麼），第 2 項則無從套用。
// 因此找不到檔案時**保留記錄並標示**，不是靜默刪除。
// Settings::recentFiles() 只存路徑清單，那是給「開啟舊檔」選單用的，
// 與這裡的用途不同，兩者刻意不合併。
//
// 序列化與儲存分離：toJson/fromJson 是純函式，測試直接打它們，
// 不必碰使用者機器上的 QSettings。

#include <QDateTime>
#include <QJsonObject>
#include <QString>

#include <cstdint>
#include <optional>
#include <vector>

namespace alioth::app {

// 使用者手動放的書籤。與 PDF 內建的 /Outlines 是兩回事：
// 那是文件作者給的結構，這是讀者自己的標記，存在使用者端而不寫進文件。
// 不寫進文件是刻意的——審閱者對唯讀或已簽章文件也要能做標記。
struct ReadingMark {
    std::int32_t pageIndex{0};
    QString label;          // 空字串代表沿用「第 N 頁」的預設顯示
    QDateTime createdAt;

    [[nodiscard]] bool isValid() const noexcept { return pageIndex >= 0; }
};

struct HistoryEntry {
    QString path;                  // 正規化後的絕對路徑，作為識別鍵
    QString title;                 // 文件標題，取不到時退回檔名
    QDateTime lastOpened;
    std::int32_t pageCount{0};
    std::int32_t lastPageIndex{0}; // 續讀位置
    double lastScale{1.0};
    std::vector<ReadingMark> marks;

    [[nodiscard]] bool isValid() const noexcept { return !path.isEmpty(); }

    // 檔案目前是否還在。回傳值不影響記錄是否保留，只影響 UI 要不要灰掉。
    [[nodiscard]] bool fileExists() const;
};

// 路徑正規化：同一份檔案用不同寫法開啟（相對路徑、大小寫不同、多餘的分隔符號）
// 必須落在同一筆記錄，否則歷史會被同一份文件灌爆。
// Windows 的檔案系統不分大小寫，這裡照平台語意處理。
[[nodiscard]] QString normalizeDocumentPath(const QString& path);

class HistoryStore {
public:
    // 上限。超過就丟最舊的——歷史面板的價值隨時間衰減得很快，
    // 而無上限的清單會讓序列化與載入變成開檔路徑上的固定成本。
    static constexpr int kMaxEntries = 100;
    static constexpr int kMaxMarksPerDocument = 500;

    HistoryStore() = default;

    // 記錄一次開啟。已存在的路徑會被更新並移到最前，既有的書籤保留。
    void recordOpen(const QString& path, const QString& title, std::int32_t pageCount);

    // 更新續讀位置。找不到記錄時不建立新的——位置沒有文件當載體沒有意義。
    bool updatePosition(const QString& path, std::int32_t pageIndex, double scale);

    bool addMark(const QString& path, const ReadingMark& mark);
    bool removeMark(const QString& path, std::int32_t pageIndex);
    [[nodiscard]] std::vector<ReadingMark> marks(const QString& path) const;

    bool remove(const QString& path);
    void clear() noexcept { entries_.clear(); }

    // 依最後開啟時間新到舊。內部就維持這個順序，讀取端不必再排序。
    [[nodiscard]] const std::vector<HistoryEntry>& entries() const noexcept { return entries_; }
    [[nodiscard]] const HistoryEntry* find(const QString& path) const;

    // 標題或檔名的子字串比對，不分大小寫。History 面板的搜尋框用。
    [[nodiscard]] std::vector<HistoryEntry> search(const QString& needle) const;

    [[nodiscard]] QJsonObject toJson() const;
    // 壞掉的欄位個別跳過而不是整份放棄：歷史是輔助資料，
    // 為了一筆爛記錄丟掉其餘 99 筆是不划算的。
    static HistoryStore fromJson(const QJsonObject& object);

    void load();
    void save() const;

private:
    HistoryEntry* findMutable(const QString& path);

    std::vector<HistoryEntry> entries_;
};

}  // namespace alioth::app
