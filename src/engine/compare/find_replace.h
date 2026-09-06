#pragma once

// 尋找並取代文字（PRD-SRCH-003）。
//
// **範圍的解讀**（PRD 註記為「限文字層可編輯情境」）：
//
// 本產品明確排除內文文字編輯（PRD §2.1 第 1 項：PDFium 無文字重排能力）。
// 因此「文字層可編輯的情境」在 Alioth 裡只有兩處：
//
//   1. 註解的內文（/Contents）——標註、便利貼、文字方塊的文字
//   2. 表單文字欄位的值（/FT /Tx 的 /V）
//
// 頁面內文不在範圍內。把取代套用到頁面內容串流，等於在沒有排版引擎的情況下
// 改寫 Tj 運算元：字串長度一變，字距與換行就全錯，而且無法回頭。那正是
// §2.1 排除內文編輯的原因，不會因為換一個功能名稱就變得可行。
//
// 這個解讀與 PRD-SRCH-004（拼字檢查「作用於註解與表單文字」）一致——
// 同一份 PRD 在相鄰條目上已經把文字編輯的邊界劃在同一個位置。
//
// 寫入走物件層的增量附加（純附加），因此既有數位簽章仍顯示為
// 「有效，簽章後有變更」而不是「無效」（SDD §1.3）。

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "engine/objects/incremental_appender.h"
#include "engine/objects/pdf_source_document.h"

namespace alioth::engine::compare {

enum class EditableTextKind : std::uint8_t {
    AnnotationContents,
    FormFieldValue,
};

struct EditableText {
    EditableTextKind kind{EditableTextKind::AnnotationContents};
    std::int32_t pageIndex{-1};  // 未掛在任何頁面上的表單欄位為 -1
    int objectNumber{0};         // 實際持有該字串的物件；取代時就地改寫它
    std::string label;           // 註解的 /T（作者）或欄位的完整名稱
    std::string text;            // UTF-8
};

struct FindOptions {
    bool matchCase{false};
    bool matchWholeWord{false};
};

struct TextMatch {
    std::size_t targetIndex{0};  // 對應 collectEditableText 的索引
    std::size_t byteOffset{0};   // 命中在該段文字中的 UTF-8 位元組位移
    std::size_t byteLength{0};
};

struct ReplaceResult {
    bool ok{false};
    std::string diagnostic;
    std::int32_t replacements{0};
    std::int32_t changedObjects{0};
    std::string bytes;  // ok 時為完整輸出檔：原檔位元組 + 附加段

    // 表單欄位值被改過但外觀串流沒有重畫時為真。
    // 我們改設 AcroForm 的 /NeedAppearances，讓檢視器自行重畫；
    // 這是 PDF 規格認可的做法，但不是所有檢視器都會照做，所以要讓 UI 看得見。
    bool needAppearances{false};
};

// 列舉可編輯的文字。註解走頁面的 /Annots（跳過 Widget，那些屬於表單），
// 表單走 AcroForm 的 /Fields 樹，兩者不會重複。
[[nodiscard]] std::vector<EditableText> collectEditableText(
    const objects::PdfSourceDocument& source);

// 單段文字內的所有命中，位置以位元組計。
[[nodiscard]] std::vector<TextMatch> findMatchesIn(const std::string& text, std::size_t targetIndex,
                                                   const std::string& query,
                                                   const FindOptions& options);

[[nodiscard]] std::vector<TextMatch> findMatches(std::span<const EditableText> targets,
                                                 const std::string& query,
                                                 const FindOptions& options);

[[nodiscard]] std::string replaceIn(const std::string& text, const std::string& query,
                                    const std::string& replacement, const FindOptions& options,
                                    std::int32_t* replacements = nullptr);

// 對整份文件取代並產生新的位元組。query 為空時不做任何事並回報失敗——
// 空字串會在每個位置命中，靜默地把檔案改成一堆重複的取代字串。
[[nodiscard]] ReplaceResult replaceAll(std::string pdfBytes, const std::string& query,
                                       const std::string& replacement,
                                       const FindOptions& options = {});

}  // namespace alioth::engine::compare
