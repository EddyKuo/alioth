#pragma once

// 清除隱藏中繼資料（PRD-ANN-034，WBS 11）。
//
// 這是**獨立於 Redaction 的操作**，刻意不與塗黑合併：
//
//   - 兩者的觸發時機不同。使用者常常只想在對外發布前清乾淨中繼資料，
//     而文件裡一個字都不用塗黑
//   - 兩者的風險不同。塗黑會刪掉使用者看得到的內容，清除中繼資料不會。
//     綁在一起會逼使用者為了其中一件事承擔另一件的後果
//
// 清除的對象是「使用者在檢視器裡看不到、卻會跟著檔案散布出去」的東西：
// /Info 的作者與製作程式、XMP 中繼資料、頁面的 /PieceInfo（製作工具的私有
// 資料，常含原始檔路徑）、嵌入檔案、以及 JavaScript。
//
// JavaScript 一併清除與 PRD §8.2 的安全立場一致：本產品不執行任何 PDF 內嵌
// JavaScript，留著它只會讓別的檢視器去執行。

#include <string>
#include <vector>

namespace alioth::engine::redaction {

struct SanitizeOptions {
    bool clearDocumentInfo{true};   // /Info 的作者、標題、製作程式…
    bool clearXmpMetadata{true};    // /Metadata（文件層與頁面層）
    bool clearPieceInfo{true};      // /PieceInfo
    bool removeEmbeddedFiles{true}; // /Names /EmbeddedFiles 與 /FileAttachment 註解
    bool removeJavaScript{true};    // /Names /JavaScript、/OpenAction、/AA
};

struct SanitizeStats {
    int clearedInfoKeys{0};
    int removedMetadataStreams{0};
    int removedPieceInfo{0};
    int removedEmbeddedFiles{0};
    int removedJavaScript{0};
};

struct SanitizeResult {
    bool ok{false};
    std::string diagnostic{};
    std::string bytes{};
    SanitizeStats stats{};
};

// 清除中繼資料並產生新的檔案位元組。
//
// 與塗黑一樣走全檔重寫：增量附加只會讓新的 /Info 疊在舊的上面，
// 舊值仍然留在檔案裡，`strings` 一撈就出來——那等於什麼都沒清。
[[nodiscard]] SanitizeResult sanitizeDocument(std::string sourceBytes,
                                              const SanitizeOptions& options = {});

}  // namespace alioth::engine::redaction
