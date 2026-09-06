#pragma once

// 把子集字型寫成 PDF 的內嵌字型物件（ADR-007）。
//
// 結構是 PDF 內嵌 CJK 的標準三層（ISO 32000-1 §9.7）：
//
//   Type0 字型          /Encoding /Identity-H，/DescendantFonts [CIDFont]
//     └ CIDFontType2    /CIDToGIDMap /Identity，/W 寬度陣列
//         └ FontDescriptor  /FontFile2 = 子集後的 TrueType 位元組
//
// ## 為什麼是 Identity-H
//
// Identity-H 讓 CID 直接等於 glyph index，字串就是「每個字兩位元組的 GID」。
// 這樣就不需要在字型裡放 cmap，也不需要一張編碼表——而任何一張需要維護的
// 對照表，遲早會與另一張不一致。
//
// 代價是**字串本身不再是可讀文字**。沒有 /ToUnicode 的話，從 PDF 複製文字會
// 得到一串亂碼，搜尋也找不到。因此 /ToUnicode 不是選配，是這條路徑的必要部分——
// 少了它，使用者會發現「中文看得到但複製不出來」，而那看起來像檔案壞了。

#include <functional>
#include <string>

#include "engine/fonts/truetype_subset.h"
#include "engine/objects/incremental_appender.h"

namespace alioth::engine::fonts {

struct EmbeddedFontResult {
    bool ok{false};
    std::string diagnostic;

    // Type0 字型物件的編號。放進頁面或外觀串流的 /Resources /Font 就是它。
    int fontObject{0};
};

// 配置一個新物件並寫入內容，回傳物件編號。
//
// 以回呼表達而不是綁定某個特定的寫入器：增量附加（IncrementalAppender）與
// 整份重寫（PdfDocumentRewriter，塗黑用）是兩套不同的寫入路徑，但內嵌字型
// 要做的事完全一樣。綁死其中一套會逼另一套複製一份，而兩份字型物件產生
// 程式碼遲早會不一致——不一致的那一份會產出「某條路徑的中文是壞的」。
using ObjectSink = std::function<int(objects::PdfObject)>;

// 把子集寫成 PDF 字型物件。baseName 是 PostScript 名稱（例如 "NotoSansTC"），
// 會自動加上 PDF 規格要求的六字母子集前綴。
//
// subset 必須是 subsetTrueType 成功的結果；ok 為 false 時直接回報而不寫入。
[[nodiscard]] EmbeddedFontResult embedSubsetFont(const ObjectSink& sink,
                                                 const SubsetResult& subset,
                                                 const std::string& baseName);

// 增量附加的便利版本。
[[nodiscard]] EmbeddedFontResult embedSubsetFont(objects::IncrementalAppender& appender,
                                                 const SubsetResult& subset,
                                                 const std::string& baseName);

// 把文字轉成 Identity-H 的位元組（每字兩位元組的 big-endian GID），
// 並以 PDF 字串字面值的形式回傳（含跳脫，不含外層括號）。
//
// 任何一個碼點在子集裡找不到字形時回傳 false——**不輸出 .notdef**。
// 靜默輸出 .notdef 會在畫面上變成空白方塊，使用者不知道是缺字還是程式壞了；
// 而這正是 CJK 路徑最容易出現的失敗，必須讓呼叫端有機會明確報錯。
[[nodiscard]] bool encodeIdentityH(const SubsetResult& subset, const std::u32string& text,
                                   std::string& out);

// 六字母子集前綴（PDF 規格 §9.6.4：子集字型的 /BaseFont 必須是 "ABCDEF+Name"）。
// 由子集內容雜湊而來，因此同樣的子集永遠得到同樣的前綴——
// 用隨機值會讓同一份文件每次存檔都產生不同的字型名稱，增量段因此白白變大。
[[nodiscard]] std::string subsetTag(const std::string& fontBytes);

}  // namespace alioth::engine::fonts
