#pragma once

// 附件（PRD-ANN-015 的讀取側，Attachments 面板）。
//
// PDF 有兩種附件，面板必須同時顯示而且要分得出來：
//   1. 文件層附件——catalog 的 /Names /EmbeddedFiles 名稱樹，沒有頁面位置
//   2. 檔案附件註解（/Subtype /FileAttachment）——掛在某一頁的某個座標上
// 只列其中一種，使用者會在另一種存在時以為文件沒有附件。
//
// 走物件層而不是 PDFium：PDFium 有 FPDFAttachment_* 可以讀文件層附件，
// 但讀不到檔案附件註解攜帶的那一份，而且拿不到 /Desc 與 /Params 的日期。
//
// **安全立場**：附件是不可信輸入的不可信輸入。這一層只做兩件事——列出、
// 取出位元組。絕不開啟、絕不執行、絕不依副檔名決定行為。取出後要放到哪、
// 要不要開，是使用者在 UI 上的明確決定。

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "engine/objects/pdf_source_document.h"

namespace alioth::engine::attachments {

enum class AttachmentOrigin : std::uint8_t {
    DocumentLevel,       // /Names /EmbeddedFiles
    FileAttachmentAnnot, // 頁面上的 /FileAttachment 註解
};

struct Attachment {
    AttachmentOrigin origin{AttachmentOrigin::DocumentLevel};
    std::string name;         // 名稱樹的鍵，或註解的 /T
    std::string fileName;     // /F 或 /UF
    std::string description;  // /Desc 或註解的 /Contents
    std::string mimeType;     // /Subtype，PDF 裡以 #2F 跳脫的形式儲存
    std::int64_t size{-1};    // /Params /Size；-1 代表檔案沒寫
    std::string creationDate;   // 原樣保留 PDF 日期字串，不在這一層解析
    std::string modificationDate;
    std::int32_t pageIndex{-1};  // 僅檔案附件註解有
    int streamObject{0};         // 內容所在的物件編號，取出時用

    // /Params /Size 與實際串流長度不一致。畸形或被截斷的檔案會這樣，
    // 面板要標示出來——照著 /Size 顯示「2 MB」但只取得出 3 KB，
    // 使用者會以為是我們的問題。
    bool sizeMismatch{false};
};

// 列出全部附件。沒有附件時回傳空清單（不是錯誤）。
[[nodiscard]] std::vector<Attachment> listAttachments(const objects::PdfSourceDocument& source);

struct ExtractResult {
    bool ok{false};
    std::string diagnostic;
    std::string bytes;
};

// 取出單一附件的位元組。串流用的濾鏡若不支援（例如 LZW），明確失敗而不是
// 回傳沒解開的資料——把壓縮位元組當成檔案內容寫出去，使用者拿到的是垃圾。
[[nodiscard]] ExtractResult extractAttachment(const objects::PdfSourceDocument& source,
                                              const Attachment& attachment);

// 檔名清理：附件的檔名來自不可信輸入，可能含路徑分隔符號、`..`、
// Windows 的保留裝置名稱、或控制字元。直接拿去開檔就是路徑穿越。
// 回傳空字串代表清不出可用的檔名，呼叫端應要求使用者自己命名。
[[nodiscard]] std::string sanitizeAttachmentFileName(const std::string& raw);

}  // namespace alioth::engine::attachments
