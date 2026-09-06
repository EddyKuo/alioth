#pragma once

// 應用程式路徑。所有「設定檔放哪、快取放哪、最近檔案清單放哪」的答案都在這裡，
// 其他層一律透過本介面取得，不得自行組路徑。

#include <QString>

namespace alioth::platform {

// 設定檔目錄（Windows: %APPDATA%/Alioth）。
[[nodiscard]] QString configDirectory();

// 快取目錄。加密文件的圖磚不得寫入此處（PRD §8.2）。
[[nodiscard]] QString cacheDirectory();

// 自動儲存的暫存目錄（PRD-IO-003）。
[[nodiscard]] QString autosaveDirectory();

// 確保目錄存在，回傳是否可用。
bool ensureDirectory(const QString& path);

// 檔案路徑的識別鍵：同一份檔案用不同寫法（相對路徑、多餘的分隔符號、
// Windows 上大小寫不同）開啟時必須落在同一個鍵，否則最近檔案與歷史紀錄
// 會被同一份文件灌爆。
//
// 檔案不存在時仍要回傳可用的鍵——歷史紀錄必須容納已經被移走的檔案。
//
// 大小寫是否折疊屬於作業系統差異，因此收在這一層：Windows 的檔案系統
// 不分大小寫，Linux 分。上層自己判斷平台就是把 #ifdef 散出去。
[[nodiscard]] QString canonicalFileKey(const QString& path);

}  // namespace alioth::platform
