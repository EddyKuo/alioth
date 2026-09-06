#pragma once

// 音訊／視訊註解（PRD-ANN-016）。
//
// **本產品不播放內嵌媒體，這是刻意的。** 三個理由，任何一個都足以成立：
//
//   1. 播放需要媒體框架（Qt Multimedia 會帶進 FFmpeg 或平台解碼器），
//      而技術堆疊固定為 Qt Widgets + PDFium + OpenSSL 三件（PRD §4.1）。
//   2. PDF 是不可信輸入。媒體解碼器是歷史上最多記憶體毀損漏洞的元件之一，
//      把不可信的位元組餵給它，等於在一個以「不執行內嵌內容」為賣點的
//      產品裡開一個執行任意程式碼的洞。
//   3. `/RichMedia` 註解可以夾帶 Flash 與 3D，而 PRD §2.1 已明確排除 3D PDF。
//
// 因此這一層做的是：**列出、標示、取出**。使用者要看那段影片，就把它存成檔案
// 用他自己信任的播放器打開——那個決定屬於他，不屬於我們。
//
// 面板上必須明講「本程式不會播放」，而不是放一個按了沒反應的播放鍵。

#include <cstdint>
#include <string>
#include <vector>

#include "domain/geometry.h"
#include "engine/objects/pdf_source_document.h"

namespace alioth::engine::attachments {

enum class MediaKind : std::uint8_t {
    Screen,     // /Subtype /Screen，PDF 1.5 起的標準做法
    Movie,      // /Subtype /Movie，PDF 1.2 的舊形態
    RichMedia,  // /Subtype /RichMedia，PDF 1.7 ExtensionLevel 3；可夾帶 Flash 與 3D
};

struct MediaAnnotation {
    MediaKind kind{MediaKind::Screen};
    std::int32_t pageIndex{-1};
    domain::RectF rectPt{};

    std::string title;        // /T
    std::string description;  // /Contents
    std::string mimeType;     // /CT 或內嵌檔案的 /Subtype
    std::string fileName;     // 內嵌媒體的檔名，可能是空的

    // 媒體位元組所在的物件；0 代表這則註解只有外部參照（/F 指向網址或
    // 本機路徑）而沒有內嵌資料。那種註解取不出東西，面板要標示出來，
    // 否則使用者會以為是我們壞了。
    int streamObject{0};

    // 引用外部資源的位址。**絕不自動開啟**——那等同 Launch Action，
    // 而本產品一律禁止（PRD §8.2）。顯示給使用者看，由他決定。
    std::string externalReference;

    [[nodiscard]] bool hasEmbeddedData() const noexcept { return streamObject > 0; }
};

// 列出全部媒體註解。沒有就回傳空清單（不是錯誤）。
[[nodiscard]] std::vector<MediaAnnotation> listMediaAnnotations(
    const objects::PdfSourceDocument& source);

struct MediaExtractResult {
    bool ok{false};
    std::string diagnostic;
    std::string bytes;
};

// 取出內嵌媒體的位元組。濾鏡不支援時明確失敗——把沒解開的壓縮位元組
// 存成 .mp4 交給使用者，他只會得到一個播不開的檔案而且不知道為什麼。
[[nodiscard]] MediaExtractResult extractMedia(const objects::PdfSourceDocument& source,
                                              const MediaAnnotation& media);

// 給 UI 的說明文字。內容依註解的實際狀態而不同：有內嵌資料的說「可另存」，
// 只有外部參照的說「本程式不會自動開啟」。
[[nodiscard]] std::string mediaNotice(const MediaAnnotation& media);

}  // namespace alioth::engine::attachments
