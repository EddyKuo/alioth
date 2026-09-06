#pragma once

// 雲端儲存整合（PRD-CLD-001 ~ 005）。
//
// **這一層做的是同步資料夾，不是 OAuth 原生 API。** 理由值得寫清楚：
//
// OneDrive、Google Drive、Dropbox、Box 在桌面上的實際運作方式就是同步資料夾——
// 檔案是本機路徑，開檔存檔走一般檔案 I/O，同步由各家的用戶端負責。走它們的
// REST API 只會多一層：使用者要另外登入、我們要保管權杖、離線時反而不能用，
// 而檔案明明就在硬碟上。
//
// 真正需要 API 的是 **SharePoint 文件庫**（不一定有同步）與「不落地」的
// 檔案串流模式。那兩者需要各家的應用程式註冊、OAuth 流程與廠商審核，
// 那是商務決策不是工程決策，見 exceptions/ 的對應報告。
//
// 因此這裡提供的是：偵測本機上有哪些雲端同步資料夾、判斷一個路徑屬於哪一家、
// 以及據此在 UI 上顯示正確的來源標示。最後一項比聽起來重要——使用者需要知道
// 「我存下去的這個檔案會同步到公司雲端」，那會改變他要不要在裡面寫敏感註解。

#include <QString>
#include <QStringList>

#include <cstdint>
#include <vector>

namespace alioth::platform {

enum class CloudProvider : std::uint8_t {
    None,
    OneDrive,
    OneDriveBusiness,
    GoogleDrive,
    Dropbox,
    Box,
    SharePoint,  // 對映到本機的文件庫同步資料夾
};

[[nodiscard]] QString providerDisplayName(CloudProvider provider);

struct CloudFolder {
    CloudProvider provider{CloudProvider::None};
    QString rootPath;     // 同步根目錄
    QString accountHint;  // 例如公司帳號名稱，取自資料夾名；可能是空的
};

// 偵測本機上的雲端同步資料夾。Windows 走各家用戶端註冊的環境變數與
// 已知的登錄位置——刻意不掃描整個磁碟，那既慢又會掃到別人的東西。
//
// 回傳空清單代表這台機器上沒有偵測到，**不代表使用者沒有用雲端**：
// 自訂位置、還沒登入、或用網頁版都會落在這個情況。UI 措辭要照這個語意寫。
[[nodiscard]] std::vector<CloudFolder> detectCloudFolders();

// 這個路徑落在哪個同步資料夾裡。沒有就回傳 None。
//
// 判定用路徑前綴比對而不是檔案系統查詢：後者在雲端檔案「僅線上」時
// 會觸發下載，而使用者只是開了一個檔案總管視窗。
[[nodiscard]] CloudProvider providerForPath(const QString& path,
                                            const std::vector<CloudFolder>& folders);

// 給 UI 的來源說明，例如「這份文件位於 OneDrive（公司帳號），存檔後會自動同步」。
// 空字串代表這是一般的本機檔案，不需要額外說明。
[[nodiscard]] QString cloudNoticeForPath(const QString& path,
                                         const std::vector<CloudFolder>& folders);

}  // namespace alioth::platform
