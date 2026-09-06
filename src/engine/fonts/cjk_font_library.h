#pragma once

// 內嵌 CJK 字型的載入與量測（ADR-007）。
//
// 存在理由是「量測」與「內嵌」必須用**同一份字型**。文字換行是用字寬算出來的，
// 而字寬來自字型；量測時用 A 字型、內嵌時放 B 字型，結果是換行位置與實際畫出來
// 的不一致——文字會超出框、或框底下多出一大塊空白。這種錯不會有訊息，
// 只會讓每一個中文文字方塊都排得有點怪。
//
// 字型檔只讀一次並常駐（思源黑體約 12 MB）。每次畫一則註解都重讀一次 12 MB
// 會讓連續加註明顯卡頓，而註解正是會連續加很多次的操作。

#include <cstdint>
#include <map>
#include <set>
#include <string>

#include "engine/fonts/truetype_subset.h"

namespace alioth::engine::fonts {

class CjkFontLibrary {
public:
    // 行程共用一份。字型是唯讀資料，沒有理由每個子系統各持一份 12 MB。
    static CjkFontLibrary& instance();

    // 是否有可用的 CJK 字型。false 時呼叫端必須明確拒絕繪製非 ASCII，
    // 不可退回拉丁字型——那會畫出一排空框，使用者不知道發生什麼事。
    [[nodiscard]] bool available();

    // 找不到字型時的說明，含找過哪些路徑。這是使用者唯一能自救的資訊。
    [[nodiscard]] std::string diagnostic();

    [[nodiscard]] std::string baseName();

    // 單一字元的前進寬度，單位是 PDF 的千分之一 em（與 Helvetica 的字寬表同單位，
    // 兩者才能混在同一行裡量測）。字型裡沒有這個字時回傳 0——
    // 呼叫端據此判斷缺字，不要拿 0 當成「寬度為零的合法字元」。
    [[nodiscard]] std::uint16_t advanceFor(char32_t codepoint);

    // 字元對應的 glyph index。0 代表字型裡沒有這個字。
    //
    // 可以在「還沒決定子集內容」的時候就問到 GID，是因為子集化**保留原始
    // 編號**（見 truetype_subset.h）。內容串流因此可以先寫好，子集稍後再嵌，
    // 兩者的 GID 保證一致。編號會變動的子集器做不到這件事——那時內容串流
    // 就得等子集完成才能產生，順序整個反過來。
    [[nodiscard]] std::uint16_t glyphFor(char32_t codepoint);

    // 為指定字集做子集。回傳的 SubsetResult 可直接交給 embedSubsetFont。
    [[nodiscard]] SubsetResult subsetFor(const std::set<char32_t>& codepoints);

    // 測試用：換掉字型來源。正式路徑不呼叫這個。
    void overrideFontBytesForTesting(std::string bytes, std::string baseName);

private:
    CjkFontLibrary() = default;
    void ensureLoaded();

    bool loaded_{false};
    std::string bytes_;
    std::string baseName_;
    std::string diagnostic_;
    // 字寬快取。每次量一個字都重新解析 cmap 與 hmtx 會讓長文字的換行變成
    // O(字數 × 字型大小)。
    std::map<char32_t, std::uint16_t> advanceCache_;
    std::map<char32_t, std::uint16_t> glyphCache_;
    SubsetResult metrics_;
};

}  // namespace alioth::engine::fonts
