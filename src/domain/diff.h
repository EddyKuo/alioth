#pragma once

// 文件比對的領域模型（PRD-CMP-001，WBS 5.12）。純 C++，不依賴 Qt 也不依賴 PDFium。
//
// 這一層只描述「差異長什麼樣」，不描述「怎麼算出來的」。演算法在
// engine/compare/，並排標示在呈現層；兩者都只認識這裡的型別。
//
// 兩個刻意的設計：
//
// 一、頁碼一律成對出現（oldPage / newPage），而不是單一個「頁碼」欄位。
// 兩份文件的頁數可以不同，插入一頁之後同一個內容在兩邊的頁碼就永遠不一樣了；
// 只存一個頁碼的模型會逼呈現層自己去猜另一邊是哪頁，而那正是並排標示最容易錯的地方。
// 不存在的一邊以 kNoPage 表示。
//
// 二、文字範圍沿用 domain::TextRange（半開區間）與頁內字元索引，
// 與 PageTextLayer 的編號完全相同。差異區段因此可以直接餵給 PageTextLayer::quads()
// 取得標示用的 QuadPoints，中間不需要任何換算——多一層自訂編號就多一個標錯位置的機會。

#include <cstdint>
#include <string>
#include <vector>

#include "text_layer.h"

namespace alioth::domain {

// 兩邊都不存在的頁碼。
inline constexpr std::int32_t kNoPage = -1;

enum class DiffKind : std::uint8_t {
    Equal,    // 兩邊相同
    Insert,   // 只存在於新文件
    Delete,   // 只存在於舊文件
    Replace,  // 兩邊都有內容但不相同
};

[[nodiscard]] constexpr const char* describe(DiffKind kind) noexcept {
    switch (kind) {
        case DiffKind::Equal: return "equal";
        case DiffKind::Insert: return "insert";
        case DiffKind::Delete: return "delete";
        case DiffKind::Replace: return "replace";
    }
    return "unknown";
}

// 一段編輯。索引的意義由產生者決定（token 序列或字元序列），
// 兩側都是半開區間，Equal 段兩側長度必然相同。
//
// 差異結果一定是「完整覆蓋」的：把所有 EditSpan 依序接起來，
// oldSpan 會恰好覆蓋舊序列一次，newSpan 覆蓋新序列一次。呈現層可以直接照著走，
// 不必自己補中間沒被提到的部分——那種補法在空輸入與尾端變更上很容易差一格。
struct EditSpan {
    DiffKind kind{DiffKind::Equal};
    TextRange oldSpan{};
    TextRange newSpan{};

    friend constexpr bool operator==(const EditSpan&, const EditSpan&) = default;
};

enum class PageMatchKind : std::uint8_t {
    Matched,   // 兩份文件都有這一頁（內容可能仍有差異）
    Inserted,  // 只存在於新文件
    Deleted,   // 只存在於舊文件
};

[[nodiscard]] constexpr const char* describe(PageMatchKind kind) noexcept {
    switch (kind) {
        case PageMatchKind::Matched: return "matched";
        case PageMatchKind::Inserted: return "inserted";
        case PageMatchKind::Deleted: return "deleted";
    }
    return "unknown";
}

// 頁面對應關係的一項。整份對應是單調的：oldPage 與 newPage 各自遞增，
// 因此並排檢視可以直接照這個順序捲動兩邊。
struct PageAlignment {
    PageMatchKind kind{PageMatchKind::Matched};
    std::int32_t oldPage{kNoPage};
    std::int32_t newPage{kNoPage};

    // Matched 時的頁面文字相似度，1.0 表示文字完全相同。
    // Inserted / Deleted 恆為 0，因為沒有對象可比。
    double similarity{0.0};

    friend constexpr bool operator==(const PageAlignment&, const PageAlignment&) = default;
};

// 一段文字差異。索引是頁內字元索引，與 PageTextLayer 同一套編號。
//
// kind 不會是 Equal：相同的部分不需要標示，全部收進來只會讓結果大一個數量級。
struct TextDiffRegion {
    DiffKind kind{DiffKind::Replace};
    std::int32_t oldPage{kNoPage};
    std::int32_t newPage{kNoPage};
    TextRange oldRange{};
    TextRange newRange{};
    std::string oldText;
    std::string newText;
};

// 每頁的變更數量，供並排檢視在縮圖與捲軸上標記用（PRD-CMP-001 的「並排標示」）。
struct PageDiffSummary {
    PageMatchKind kind{PageMatchKind::Matched};
    std::int32_t oldPage{kNoPage};
    std::int32_t newPage{kNoPage};
    double similarity{0.0};
    std::int32_t insertions{0};
    std::int32_t deletions{0};
    std::int32_t replacements{0};

    [[nodiscard]] constexpr std::int32_t totalChanges() const noexcept {
        return insertions + deletions + replacements;
    }

    // 整頁被插入或刪除時 totalChanges 也會大於零（整頁算成一段），
    // 但這裡仍然把 kind 納入判斷：空白頁被插入時沒有任何文字差異，
    // 只看 totalChanges 會把它報成「沒變」。
    [[nodiscard]] constexpr bool changed() const noexcept {
        return kind != PageMatchKind::Matched || totalChanges() > 0;
    }
};

class DocumentDiff {
public:
    DocumentDiff() = default;

    [[nodiscard]] const std::vector<PageAlignment>& alignments() const noexcept {
        return alignments_;
    }
    [[nodiscard]] const std::vector<TextDiffRegion>& regions() const noexcept { return regions_; }
    [[nodiscard]] const std::vector<PageDiffSummary>& summaries() const noexcept {
        return summaries_;
    }

    // 因資源上限而降級為粗略結果（見 engine/compare/token_diff.h 的 DiffLimits）。
    // 結果仍然正確——只是某些區段被整段報成替換，而不是最小編輯。
    // 這個旗標必須傳到 UI：使用者要知道自己看到的不是最精細的比對（SDD §7）。
    [[nodiscard]] bool degraded() const noexcept { return degraded_; }
    void setDegraded(bool value) noexcept { degraded_ = value; }

    [[nodiscard]] bool isIdentical() const noexcept {
        if (!regions_.empty()) return false;
        for (const PageAlignment& a : alignments_) {
            if (a.kind != PageMatchKind::Matched) return false;
        }
        return true;
    }

    [[nodiscard]] std::int32_t changedPageCount() const noexcept {
        std::int32_t count = 0;
        for (const PageDiffSummary& s : summaries_) {
            if (s.changed()) ++count;
        }
        return count;
    }

    void addAlignment(const PageAlignment& alignment) { alignments_.push_back(alignment); }
    void addRegion(TextDiffRegion region) { regions_.push_back(std::move(region)); }
    void setSummaries(std::vector<PageDiffSummary> summaries) {
        summaries_ = std::move(summaries);
    }

    // 並排檢視每畫一頁就會問一次，所以兩邊都要能查。線性掃描是刻意的：
    // 頁數是數千的量級，而這裡一次只回一頁的結果，建索引的複雜度換不到東西。
    [[nodiscard]] std::vector<const TextDiffRegion*> regionsForNewPage(std::int32_t page) const {
        std::vector<const TextDiffRegion*> out;
        if (page < 0) return out;
        for (const TextDiffRegion& r : regions_) {
            if (r.newPage == page) out.push_back(&r);
        }
        return out;
    }

    [[nodiscard]] std::vector<const TextDiffRegion*> regionsForOldPage(std::int32_t page) const {
        std::vector<const TextDiffRegion*> out;
        if (page < 0) return out;
        for (const TextDiffRegion& r : regions_) {
            if (r.oldPage == page) out.push_back(&r);
        }
        return out;
    }

    [[nodiscard]] const PageDiffSummary* summaryForNewPage(std::int32_t page) const noexcept {
        if (page < 0) return nullptr;
        for (const PageDiffSummary& s : summaries_) {
            if (s.newPage == page) return &s;
        }
        return nullptr;
    }

    [[nodiscard]] const PageDiffSummary* summaryForOldPage(std::int32_t page) const noexcept {
        if (page < 0) return nullptr;
        for (const PageDiffSummary& s : summaries_) {
            if (s.oldPage == page) return &s;
        }
        return nullptr;
    }

private:
    std::vector<PageAlignment> alignments_;
    std::vector<TextDiffRegion> regions_;
    std::vector<PageDiffSummary> summaries_;
    bool degraded_{false};
};

}  // namespace alioth::domain
