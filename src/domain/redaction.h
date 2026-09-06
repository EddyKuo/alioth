#pragma once

// 塗黑（Redaction）領域模型（PRD-ANN-032 / 033 / 034、WBS 11）。
//
// 純 C++：不得引入 Qt 或 PDFium。塗黑的幾何與策略要能在無引擎的環境下被驗證，
// 因為「哪些內容會被刪掉」的判斷錯誤是會外洩的那一類錯誤，它必須是可以用
// 毫秒級單元測試逐條驗的純邏輯。
//
// 本檔的核心是把「標記」與「套用」分成兩個不同的型別，而不是同一個型別上的
// 一個布林旗標：
//
//   RedactionMark  可逆。它只是一則 /Redact 註解，刪掉註解就等於沒發生過。
//                  PRD-ANN-033（Find and Redact）只產出這個。
//   RedactionPlan  不可逆。它代表「真的把底下的內容從檔案裡刪掉」。
//                  PRD-ANN-032 才走到這裡。
//
// 兩者在型別上分開的理由：Redaction 事故的典型成因是「以為只是畫了個黑框」
// 或反過來「以為只是預覽卻已經毀掉原稿」。讓不可逆的那一邊必須經過一個
// 帶名字的明確動作（IrreversibleConsent::confirmed()）才建構得出來，
// 呼叫端就不可能在讀不到註解的情況下不小心觸發它。
//
// 座標一律為「未旋轉的頁面預設使用者空間」（點，原點左下、Y 軸向上），
// 與 domain::Annotation 一致。

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "domain/annotation.h"
#include "domain/geometry.h"

namespace alioth::domain {

// 部分重疊的顯示字串怎麼處理。
//
// PDF 的一個顯示運算子（Tj / TJ 的單一字串元素）是一段連續的字碼，中間沒有
// 可靠的切點：字距、字距調整、連字都會讓「切一半」留下位置錯亂的殘字，
// 而殘字對法務用途而言等同外洩。因此預設是整串移除。
enum class PartialOverlapPolicy : std::uint8_t {
    // 顯示字串的外框與塗黑區域有任何交集就整串移除。過度移除是安全方向。
    RemoveWholeString,
    // 只有顯示字串完全落在塗黑區域內才移除。留給「已知字串很長且只想動中間」
    // 的情境，但它會漏掉部分重疊的字，不得作為預設。
    RemoveOnlyFullyContained,
};

// 影像的處理策略。與文字採同一套理由：影像無法在不重新編碼的前提下切一半，
// 而重新編碼會改變原始影像資料以外的東西。
enum class ImageOverlapPolicy : std::uint8_t {
    // 與塗黑區域有任何交集就整張移除。
    RemoveWholeImage,
    // 只移除完全落在區域內的影像；部分重疊者僅被覆蓋矩形遮住（會殘留資料）。
    RemoveOnlyFullyContained,
};

// 一則塗黑標記。可逆，對應一則 /Redact 註解。
struct RedactionMark {
    int pageIndex{0};

    // 要移除的區域。一次選取跨行時會有多塊，對應 /QuadPoints；
    // /Rect 是它們的聯集。空的 areas 是無效標記，不得被當成「整頁」。
    std::vector<RectF> areas{};

    // 套用後覆蓋矩形的填色（/IC）。預設黑色——這是使用者對「塗黑」的預期，
    // 但它只是給人看的，不是安全機制：安全性來自底下的內容真的被刪掉。
    ColorRgb fillColor{0.0, 0.0, 0.0};

    // 標記階段外框的顏色（/C）。刻意與 fillColor 不同，讓「已標記但尚未套用」
    // 在畫面上一眼可辨——把兩個階段畫成一樣是使用者誤以為已經處理完的來源。
    ColorRgb markColor{1.0, 0.0, 0.0};

    // 覆蓋文字（/OverlayText）。目前僅支援 ASCII：CJK 需要字型子集內嵌，
    // 其授權策略在 CLAUDE.md 仍是待決策項，寫出非 ASCII 只會得到缺字。
    std::optional<std::string> overlayText{};
    double overlayFontSize{8.0};

    std::string author{};    // /T
    std::string subject{};   // /Subj
    std::string note{};      // /Contents，標記的說明（例如「個資」「營業秘密」）
    PdfDate modified{};      // /M

    [[nodiscard]] bool isValid() const noexcept {
        if (pageIndex < 0 || areas.empty()) return false;
        for (const RectF& area : areas) {
            if (area.normalized().isEmpty()) return false;
        }
        return true;
    }

    // /Rect：所有區域的聯集。
    [[nodiscard]] RectF boundingBox() const noexcept {
        RectF box{};
        for (const RectF& area : areas) box = box.united(area.normalized());
        return box;
    }
};

// 一份文件的所有標記。
class RedactionMarkSet {
public:
    RedactionMarkSet() = default;

    void add(RedactionMark mark) { marks_.push_back(std::move(mark)); }

    // 標記是可逆的，因此刪除是它的正常操作而不是例外路徑。
    void removeAt(std::size_t index) {
        if (index < marks_.size()) marks_.erase(marks_.begin() + static_cast<std::ptrdiff_t>(index));
    }
    void clear() noexcept { marks_.clear(); }

    [[nodiscard]] const std::vector<RedactionMark>& marks() const noexcept { return marks_; }
    [[nodiscard]] std::size_t size() const noexcept { return marks_.size(); }
    [[nodiscard]] bool empty() const noexcept { return marks_.empty(); }

    [[nodiscard]] std::vector<RedactionMark> marksForPage(int pageIndex) const {
        std::vector<RedactionMark> result;
        for (const RedactionMark& mark : marks_) {
            if (mark.pageIndex == pageIndex) result.push_back(mark);
        }
        return result;
    }

private:
    std::vector<RedactionMark> marks_{};
};

// 不可逆操作的同意權杖。
//
// 唯一的建構方式是呼叫具名的 confirmed()，因此 RedactionPlan 不可能由
// 一組標記隱式轉換而來。這個型別存在的唯一目的就是讓「毀掉原始內容」
// 在原始碼裡有一個搜尋得到、code review 看得見的字面痕跡。
class IrreversibleConsent {
public:
    [[nodiscard]] static IrreversibleConsent confirmed() noexcept { return IrreversibleConsent{}; }

private:
    IrreversibleConsent() = default;
};

// 套用計畫。持有這個型別代表呼叫端已經明確表示要刪掉內容。
class RedactionPlan {
public:
    RedactionPlan(RedactionMarkSet marks, IrreversibleConsent)
        : marks_(std::move(marks)) {}

    [[nodiscard]] const RedactionMarkSet& marks() const noexcept { return marks_; }

    // 逾越預設策略必須是顯式的，因為兩個非預設值都會讓殘留資料留在檔案裡。
    [[nodiscard]] PartialOverlapPolicy textPolicy() const noexcept { return textPolicy_; }
    [[nodiscard]] ImageOverlapPolicy imagePolicy() const noexcept { return imagePolicy_; }
    void setTextPolicy(PartialOverlapPolicy policy) noexcept { textPolicy_ = policy; }
    void setImagePolicy(ImageOverlapPolicy policy) noexcept { imagePolicy_ = policy; }

    // 落在區域內的註解要一併刪除，否則便利貼與彈出視窗裡的內容原封不動留著。
    // 這是 Redaction 最常被忘記的一塊，預設為真且不建議關閉。
    [[nodiscard]] bool removesOverlappingAnnotations() const noexcept { return removeAnnotations_; }
    void setRemovesOverlappingAnnotations(bool value) noexcept { removeAnnotations_ = value; }

    // 覆蓋矩形只是給人看的。關掉它不影響安全性，但會讓輸出看起來沒被處理過。
    [[nodiscard]] bool drawsOverlay() const noexcept { return drawOverlay_; }
    void setDrawsOverlay(bool value) noexcept { drawOverlay_ = value; }

private:
    RedactionMarkSet marks_{};
    PartialOverlapPolicy textPolicy_{PartialOverlapPolicy::RemoveWholeString};
    ImageOverlapPolicy imagePolicy_{ImageOverlapPolicy::RemoveWholeImage};
    bool removeAnnotations_{true};
    bool drawOverlay_{true};
};

// 幾何判定的共用規則。
//
// 交集用的是嚴格不等（RectF::intersects），退化成一條線的接觸不算重疊：
// 相鄰兩行文字的外框常常剛好共邊，把共邊算成重疊會讓每次塗黑都多吃一行。
[[nodiscard]] inline bool overlapsAny(const std::vector<RectF>& areas, const RectF& box) noexcept {
    for (const RectF& area : areas) {
        if (area.normalized().intersects(box)) return true;
    }
    return false;
}

[[nodiscard]] inline bool containedInAny(const std::vector<RectF>& areas,
                                         const RectF& box) noexcept {
    const RectF b = box.normalized();
    for (const RectF& area : areas) {
        const RectF a = area.normalized();
        if (b.left >= a.left && b.right <= a.right && b.bottom >= a.bottom && b.top <= a.top) {
            return true;
        }
    }
    return false;
}

// 依策略決定某個外框是否該被移除。文字與影像共用同一個判斷形狀，
// 讓兩者的行為差異只存在於策略值，而不是兩份各自演化的程式碼。
[[nodiscard]] inline bool shouldRemove(const std::vector<RectF>& areas, const RectF& box,
                                       PartialOverlapPolicy policy) noexcept {
    return policy == PartialOverlapPolicy::RemoveWholeString ? overlapsAny(areas, box)
                                                             : containedInAny(areas, box);
}

[[nodiscard]] inline bool shouldRemove(const std::vector<RectF>& areas, const RectF& box,
                                       ImageOverlapPolicy policy) noexcept {
    return policy == ImageOverlapPolicy::RemoveWholeImage ? overlapsAny(areas, box)
                                                          : containedInAny(areas, box);
}

}  // namespace alioth::domain
