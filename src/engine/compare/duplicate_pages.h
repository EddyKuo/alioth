#pragma once

// 尋找重複頁面（PRD-CMP-002）。
//
// 判定規則（三條，缺一不可）：
//
//   1. **正規化後的文字必須完全相同。** 正規化只做「連續空白摺疊成一個空格、
//      去除頭尾空白」，不做大小寫摺疊也不做標點移除：這個功能的出口通常是
//      「刪掉重複的那幾頁」，寧可漏報也不能誤報。
//   2. **頁面尺寸必須相同。** 文字相同但尺寸不同不算重複。同一張圖的 A4 與 A3
//      兩個版本、或加了出血的印刷版，文字層可以一模一樣，但它們是不同的頁面；
//      把它們判成重複，使用者刪掉之後會發現少了一個版本。
//      尺寸以 0.1 點量化後比對——真正重複的頁面 MediaBox 是同一組數值，
//      量化只是為了吸收浮點誤差，不是容差。
//   3. **旋轉後的呈現尺寸相同。** /Rotate 90 或 270 的頁面，寬高交換後才比對：
//      直式與橫式在畫面上是兩頁不同的東西。
//
// 沒有文字的頁面預設不納入（includeTextlessPages）。掃描件與純圖形頁的文字層
// 全都是空的，「文字相同」在那裡不構成任何內容相同的證據，納入就是把整份掃描件
// 報成一大群重複頁。要涵蓋那種情形需要影像指紋（感知雜湊），屬於 R2 完整版。

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "domain/geometry.h"

namespace alioth::engine::compare {

struct PageFingerprintInput {
    std::int32_t pageIndex{0};
    std::string text;            // 頁面純文字（UTF-8），未正規化
    domain::SizeF sizePt{};      // 已套用 /Rotate 的呈現尺寸，單位為點
};

struct DuplicateOptions {
    bool includeTextlessPages{false};
    std::int32_t sizeQuantumTenths{1};  // 尺寸量化單位，以 0.1 點為 1
};

struct DuplicatePageGroup {
    std::vector<std::int32_t> pages;  // 依頁序遞增；第一頁視為保留者
    domain::SizeF sizePt{};
    std::string normalizedText;

    [[nodiscard]] std::size_t count() const noexcept { return pages.size(); }
};

// 連續空白（含 CJK 全形空白與換行）摺疊為單一空格，並去除頭尾空白。
[[nodiscard]] std::string normalizeForFingerprint(std::string_view utf8);

// 回傳所有大小 ≥ 2 的群組，依群組第一頁的頁碼排序。
[[nodiscard]] std::vector<DuplicatePageGroup> findDuplicatePages(
    std::span<const PageFingerprintInput> pages, const DuplicateOptions& options = {});

}  // namespace alioth::engine::compare
