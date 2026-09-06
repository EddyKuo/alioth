#pragma once

// 附件寫入（PRD-ANN-015 的寫入側）。
//
// 兩種目的地，對應讀取側的兩種來源：
//   1. 文件層——寫進 catalog 的 /Names /EmbeddedFiles 名稱樹
//   2. 檔案附件註解——掛在某一頁的座標上，同時寫 /Annots
//
// 走物件層的增量附加：既有簽章不會失效（新內容會被正確回報為未受簽章涵蓋）。
//
// **不做壓縮**：內嵌檔案可以帶 /Filter /FlateDecode，但這一層刻意輸出未壓縮的
// 位元組。理由是附件多半已經是壓縮格式（zip、docx、jpg），再壓一次只是浪費，
// 而對真的可壓縮的純文字，省下的空間不值得引入一條會出錯的路徑。
// /Params /Size 一律寫原始大小，讀取側會拿它和實際內容比對。

#include <cstdint>
#include <string>

#include "domain/geometry.h"
#include "engine/objects/incremental_appender.h"

namespace alioth::engine::attachments {

struct AttachmentSpec {
    std::string fileName;     // 顯示與另存時的預設名稱
    std::string description;  // /Desc
    std::string mimeType;     // 例如 "text/plain"；空字串代表不寫 /Subtype
    std::string bytes;        // 檔案內容

    // 檔案附件註解才用得到。pageIndex 為負代表寫成文件層附件。
    std::int32_t pageIndex{-1};
    domain::RectF rectPt{};   // 註解圖示的位置
    std::string author;       // /T
};

struct AttachmentWriteResult {
    bool ok{false};
    std::string diagnostic;
    int fileSpecObject{0};
    int streamObject{0};
    int annotationObject{0};  // 只有檔案附件註解會有
};

// 單一附件大小上限。沒有上限的話，一個 2 GB 的附件會讓整份文件在記憶體裡
// 被複製好幾次（附加式寫入本來就持有原檔的全部位元組）。
inline constexpr std::size_t kMaxAttachmentBytes = 256u * 1024u * 1024u;

// 加入附件。pageIndex 為負時寫成文件層附件，否則寫成該頁的檔案附件註解。
[[nodiscard]] AttachmentWriteResult addAttachment(objects::IncrementalAppender& appender,
                                                  const AttachmentSpec& spec);

}  // namespace alioth::engine::attachments
