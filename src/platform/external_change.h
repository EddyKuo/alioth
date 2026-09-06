#pragma once

// 外部變更偵測（PRD-IO-004）。
//
// 「唯讀與雲端同步資料夾相容」（PRD-IO-005）已經處理了「打開時就唯讀」的情況；
// 這裡處理的是另一半：文件開著看的期間，別的行程（使用者自己在另一個視窗、
// 雲端同步用戶端下載到新版本、版本控制系統簽出）動了同一個檔案。
// 存檔前若不比對，行為就是「安靜地覆蓋掉別人的版本」——這正是 IL-4 要擋的
// 那類靜默失敗。
//
// 偵測只看檔案系統中繼資料（大小、修改時間）加尾端取樣雜湊，理由與
// app/annotation_service.h 的邊界守衛相同：不整份讀檔也能高機率抓到「內容真的
// 變了」，100 MB 的文件也能瞬間完成。這不是密碼學等級的比對——兩個不同版本
// 尾端剛好雜湊相同在理論上可能發生，但那個機率遠低於「使用者真的在意」的門檻，
// 而整份雜湊在大檔案上會讓每次存檔前都多一次全檔案讀取，得不償失。
//
// 刻意不做成背景輪詢或檔案系統監看（QFileSystemWatcher）：那類 API 在雲端同步
// 資料夾上的行為因平台而異且不可靠（PRD-IO-005 的相容情境），本檔只提供
// 「存檔前問一次」的同步比對，呼叫時機由應用層決定。

#include <QByteArray>
#include <QDateTime>
#include <QString>

#include <cstdint>

namespace alioth::platform {

// 開檔當下（或上次成功存檔後）記錄的快照。
struct FileSnapshot {
    bool exists{false};
    qint64 size{0};
    QDateTime lastModified;
    // 尾端 4 KB 的雜湊，取樣長度與 app::AnnotationService 的邊界守衛一致，
    // 讓兩處的「這段時間內容有沒有變」的判準是同一個實作（IL-3）。
    QByteArray tailHash;

    [[nodiscard]] bool valid() const noexcept { return exists; }
};

enum class ExternalChangeStatus {
    Unchanged,           // 大小、修改時間、尾端雜湊都與快照一致
    ModifiedExternally,  // 檔案還在，但內容變了
    DeletedExternally,   // 檔案已經不存在或無法讀取
    Unknown,             // 快照本身無效（例如從未成功開檔），無法比對
};

[[nodiscard]] const char* describe(ExternalChangeStatus status) noexcept;

// 對指定路徑取一次快照。檔案不存在或無法開啟時回傳 exists=false，
// 呼叫端應視為「無法確認」而不是「沒有變更」。
[[nodiscard]] FileSnapshot captureSnapshot(const QString& path);

// 用目前的檔案狀態與先前的快照比對。
//
// 修改時間刻意不是唯一判準：部分雲端同步用戶端在內容不變的情況下也會更新
// mtime（例如重新下載同一版本、或時區/夏令時造成的時間戳偏移），單看 mtime
// 會產生大量假警報，很快就會被使用者無視。因此只有「大小不同」或「尾端雜湊
// 不同」才判定為真的變更；mtime 只用來快速跳過大多數真的沒變的情況。
[[nodiscard]] ExternalChangeStatus checkForExternalChange(const QString& path,
                                                          const FileSnapshot& previous);

}  // namespace alioth::platform
