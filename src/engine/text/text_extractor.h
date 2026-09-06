#pragma once

// 文字層擷取（WBS 2.8，PRD-TXT-001 ~ 004）。
//
// 執行緒規則：PDFium 非執行緒安全，且文字層把手屬於它所屬的文件把手。
// 因此 TextExtractor 持有自己的專用執行緒與**獨立的文件把手**，
// 不共用 PdfiumEngine 的把手——共用就等於兩條執行緒碰同一份文件。
// 這正是 CLAUDE.md 對「搜尋若需並行必須另開獨立文件把手」的要求。
//
// 本檔的所有自由函式都以「已在文字執行緒上」為前提，只能在 withTextPage()
// 的回呼裡呼叫；在其他執行緒上呼叫它們是未定義行為，不是效能問題。
//
// 座標一律頁面空間（點，原點左下、Y 向上）。PDFium 的 CharBox 本來就是這個座標系，
// 這裡不做任何翻轉；翻轉統一由 domain::PageTransform 負責。

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "domain/document.h"
#include "domain/geometry.h"
#include "domain/text_layer.h"

namespace alioth::engine::text {

// FPDFTEXT_PAGE 的 RAII 包裝，保證 LoadPage / ClosePage 成對。
//
// 把手型別以 void* 藏起來，讓 PDFium 標頭不外洩到引擎轉接層以外——
// 這是分層規則的實際執行手段，不只是風格偏好。
class TextPage {
public:
    TextPage() = default;
    ~TextPage();

    TextPage(const TextPage&) = delete;
    TextPage& operator=(const TextPage&) = delete;
    TextPage(TextPage&& other) noexcept;
    TextPage& operator=(TextPage&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return textPage_ != nullptr; }
    [[nodiscard]] std::int32_t pageIndex() const noexcept { return pageIndex_; }

    // 字元層只在第一次被問到時建立，之後重複使用。
    // 選取拖曳每幀都要幾何，逐次重建會把 O(n) 變成每幀 O(n)。
    [[nodiscard]] const domain::PageTextLayer& layer() const;

    // 以下兩者僅供本子系統的實作檔使用，把手型別分別是 FPDF_PAGE 與 FPDFTEXT_PAGE。
    // 外部拿到 void* 也用不了——PDFium 標頭只有引擎轉接層看得到。
    TextPage(void* fpdfPage, void* fpdfTextPage, std::int32_t pageIndex) noexcept;
    [[nodiscard]] void* handle() const noexcept { return textPage_; }

private:
    void reset() noexcept;

    void* page_{nullptr};      // FPDF_PAGE，必須活得比 textPage_ 久
    void* textPage_{nullptr};  // FPDFTEXT_PAGE
    std::int32_t pageIndex_{0};
    mutable std::unique_ptr<domain::PageTextLayer> layer_;
};

// 以下皆須在文字執行緒上呼叫（見檔頭）。

// FPDFText_CountChars。含 PDFium 生成的空白與換行字元。
[[nodiscard]] std::int32_t charCount(const TextPage& page);

[[nodiscard]] domain::TextRange pageRange(const TextPage& page);

// FPDFText_GetCharIndexAtPos。未命中回傳 -1。
// tolerance 單位是點，給的是「手指粗細」而非像素，因此不隨縮放變化。
[[nodiscard]] std::int32_t charIndexAt(const TextPage& page, const domain::PointF& pagePoint,
                                       double tolerance);

// 字元外框（FPDFText_GetCharBox）。索引越界回傳空矩形。
[[nodiscard]] domain::RectF charBox(const TextPage& page, std::int32_t index);

// 選取範圍 → QuadPoints。跨行會切成多個 quad，這是螢光筆註解的直接輸入。
[[nodiscard]] std::vector<domain::QuadPoint> quadsForRange(const TextPage& page,
                                                           domain::TextRange range);

// 選取範圍 → 純文字（UTF-8）。
[[nodiscard]] std::string textForRange(const TextPage& page, domain::TextRange range);

// 矩形區域內的文字（FPDFText_GetBoundedText），區域選取用（PRD-TXT-003）。
[[nodiscard]] std::string boundedText(const TextPage& page, const domain::RectF& area);

// 雙擊選詞、三擊選行（PRD-TXT-004）。
[[nodiscard]] domain::TextRange wordRangeAt(const TextPage& page, std::int32_t index);
[[nodiscard]] domain::TextRange lineRangeAt(const TextPage& page, std::int32_t index);

// 文字子系統的門面：一條專用執行緒 + 一份獨立文件把手。
class TextExtractor {
public:
    TextExtractor();
    ~TextExtractor();

    TextExtractor(const TextExtractor&) = delete;
    TextExtractor& operator=(const TextExtractor&) = delete;

    // 非同步。回呼在文字執行緒上被呼叫，呼叫端須自行排回自己的執行緒。
    void open(std::string path, std::string password,
              std::function<void(domain::DocumentError)> callback);
    void close(std::function<void()> callback = {});

    [[nodiscard]] std::int32_t pageCount() const noexcept;

    // 在文字執行緒上以指定頁的文字層執行工作；頁面不存在時傳入 nullptr。
    // TextPage 的生命週期由本類別管理，回呼結束後不得保留指標。
    void withTextPage(std::int32_t pageIndex, std::function<void(const TextPage*)> work);

    // 同步等待佇列清空，僅供測試使用。
    void waitForIdle();

    [[nodiscard]] std::size_t pendingTaskCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace alioth::engine::text
