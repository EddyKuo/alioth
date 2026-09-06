#pragma once

// Fields 面板的資料模型（PRD-FORM-021）。
//
// PDF 的欄位名以點分層（"address.city"），Acrobat 的 Fields 面板顯示的就是
// 這棵樹。樹只在名字裡，不在檔案結構裡：兩個毫無關係的物件只要名字有共同前綴
// 就屬於同一個分支，因此建樹是純字串運算，不需要重新走訪 PDF。
//
// 中介節點（"address"）可能沒有對應的實體欄位，這種節點只有分組意義。
// 面板必須能分辨兩者，否則使用者會對著一個不存在的欄位按「屬性」。
//
// 這裡刻意接受一個中性的 FieldSummary 而不是 FieldDefinition 或
// forms::FormFieldInfo：面板要同時顯示「剛建立的」與「原本就有的」欄位，
// 綁死其中一種型別就等於要求呼叫端做轉換，而那個轉換遲早會有兩份。

#include <cstdint>
#include <string>
#include <vector>

#include "domain/geometry.h"
#include "engine/formbuild/field_definition.h"

namespace alioth::engine::formbuild {

struct FieldSummary {
    std::string name;  // 完整欄位名，可含點
    BuildFieldType type{BuildFieldType::Text};
    std::int32_t pageIndex{0};
    domain::RectF rectPt{};
    std::int32_t widgetCount{1};  // 單選群組會大於 1
    bool readOnly{false};
    bool required{false};
};

struct FieldTreeNode {
    std::string segment;   // 這一層的名字片段
    std::string fullName;  // 由根到此節點的完整名稱
    bool isField{false};   // 為 false 代表只是分組用的中介節點

    BuildFieldType type{BuildFieldType::Text};
    std::int32_t pageIndex{-1};
    domain::RectF rectPt{};
    std::int32_t widgetCount{0};
    bool readOnly{false};
    bool required{false};

    std::vector<FieldTreeNode> children;

    // 面板顯示用：本節點與其子孫中實體欄位的總數。
    [[nodiscard]] std::int32_t fieldCount() const;
};

// 由欄位摘要建樹。輸入順序決定同層節點的順序（穩定），
// 因為面板需要「剛建立的欄位出現在最後」這種可預期的行為。
[[nodiscard]] std::vector<FieldTreeNode> buildFieldTree(const std::vector<FieldSummary>& fields);

// 由建立規格產生摘要，供剛寫完檔案時直接建樹。
[[nodiscard]] std::vector<FieldSummary> summarize(const std::vector<FieldDefinition>& definitions);

// 依頁碼過濾，供「只顯示本頁欄位」的檢視模式。
[[nodiscard]] std::vector<FieldSummary> fieldsOnPage(const std::vector<FieldSummary>& fields,
                                                     std::int32_t pageIndex);

}  // namespace alioth::engine::formbuild
