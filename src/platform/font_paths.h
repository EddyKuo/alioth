#pragma once

// CJK 內嵌字型的來源（ADR-007）。
//
// 放在平台層是因為「字型檔在哪裡」是作業系統差異：Windows 在
// %WINDIR%\Fonts、macOS 在 /System/Library/Fonts、Linux 在 fontconfig 管的
// 目錄樹底下。CLAUDE.md 規定這類差異只能收斂在平台層。
//
// ## 為什麼優先找隨附的那一份
//
// 產品散布時會把思源黑體（Noto Sans TC，SIL OFL 1.1）放進安裝目錄。
// 優先使用它的理由不是效能，而是**可預測性**：使用者機器上的同名字型可能是
// 不同版本、不同字重，甚至是同名的別家字型。內嵌到 PDF 裡的東西必須是我們
// 驗證過的那一份，否則「產出的檔案在別人機器上長得一樣」這個承諾就沒了。
//
// 找不到隨附字型時退回系統字型，並讓呼叫端知道用的是哪一份——
// 那是降級而不是等價，使用者有權知道。

#include <QString>

#include <vector>

namespace alioth::platform {

struct CjkFontSource {
    QString path;
    // 隨附於安裝目錄的那一份為 true。false 代表退回了系統字型。
    bool bundled{false};
    // PostScript 名稱，寫進 PDF 的 /BaseFont。
    QString baseName;

    [[nodiscard]] bool isValid() const noexcept { return !path.isEmpty(); }
};

// 依序尋找可用的 CJK 字型，回傳第一個存在的。全都找不到時回傳無效值——
// 呼叫端必須據此明確拒絕繪製中文，而不是靜默改用拉丁字型（那會畫出一排空框）。
[[nodiscard]] CjkFontSource findCjkFont();

// 候選清單，依優先序。公開出來是為了讓測試能檢查順序，以及讓診斷訊息
// 可以列出「我找過哪些地方」——找不到字型時那是使用者唯一能自救的資訊。
[[nodiscard]] std::vector<QString> cjkFontCandidates();

}  // namespace alioth::platform
