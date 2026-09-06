#pragma once

// 原子檔案寫入。
//
// PRD §8.4 要求「寫入暫存 → 落盤同步 → 原子更名」：斷電或行程被強制中止時，
// 使用者的原檔要嘛是舊版本、要嘛是新版本，不允許出現寫到一半的第三種狀態。
// 少了中間那步落盤同步，更名雖然是原子的，內容卻可能還在作業系統的寫入快取裡，
// 斷電後會得到「更名成功但檔案是空的」——這是最惡劣的一種失敗。
//
// ReplaceFileW / MoveFileExW / FlushFileBuffers 這類作業系統呼叫只准出現在本檔，
// 引擎層與其他層一律透過本介面取得原子性。

#include <QString>

#include <cstddef>
#include <memory>

namespace alioth::platform {

// commit() 各階段耗時。存檔預算（PRD-IO-001：100 MB 文件 ≤ 300 毫秒）超標時，
// 要能分辨是 PDFium 慢、複製慢、還是落盤同步慢，否則只能亂猜。
struct CommitTiming {
    double syncMs{0.0};
    double renameMs{0.0};
};

// 一次性的原子寫入工作階段。未 commit 就解構時，暫存檔會被刪除，目標檔保持原樣。
class AtomicFileWriter {
public:
    explicit AtomicFileWriter(QString targetPath);
    ~AtomicFileWriter();

    AtomicFileWriter(const AtomicFileWriter&) = delete;
    AtomicFileWriter& operator=(const AtomicFileWriter&) = delete;

    // 在目標檔的同一個目錄下建立暫存檔。同目錄是必要條件而非美觀選擇：
    // 跨磁碟區的更名會退化成「複製 + 刪除」，那不是原子操作。
    bool begin();

    // 把既有檔案的內容整份搬進暫存檔。增量儲存需要它——PDFium 只交出要追加的
    // 那一段位元組，原檔內容必須由呼叫端負責帶過去。
    bool copyFrom(const QString& sourcePath);

    bool write(const void* data, std::size_t size);

    // flush → 落盤同步 → 原子更名。任一步失敗都會刪掉暫存檔並保留原檔。
    bool commit(CommitTiming* timing = nullptr);

    // 主動放棄；解構時會自動呼叫。
    void abort();

    [[nodiscard]] qint64 bytesWritten() const noexcept;
    [[nodiscard]] QString temporaryPath() const;
    [[nodiscard]] QString targetPath() const;
    [[nodiscard]] QString lastError() const;
    [[nodiscard]] bool isOpen() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 目錄項目層級的原子更名。暫存檔與目標檔必須位於同一磁碟區。
bool atomicReplace(const QString& temporaryPath, const QString& targetPath,
                   QString* error = nullptr);

// 把檔案描述子對應的資料確實推到儲存媒體。傳回 false 代表不保證持久化，
// 呼叫端必須視同寫入失敗，不可當成警告忽略（IL-4）。
bool syncFileDescriptor(int fileDescriptor, QString* error = nullptr);

}  // namespace alioth::platform
