#pragma once

// 文件與頁面的領域模型。與 PDFium 完全無關，可在無 GUI 環境下單元測試。

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "geometry.h"

namespace alioth::domain {

// 載入失敗原因。IL-4 失敗快失敗明：錯誤在最近節點捕捉，不靜默吞噬。
enum class DocumentError {
    None,
    FileNotFound,
    NotAPdf,
    PasswordRequired,
    WrongPassword,
    CorruptXref,
    UnsupportedFeature,
    OutOfMemory,
    Unknown,
};

[[nodiscard]] const char* describe(DocumentError error) noexcept;

// 文件權限旗標（PRD-SEC-001）。受限操作必須在 UI 灰化並提示，
// 不可只在儲存時才失敗。
struct Permissions {
    bool print{true};
    bool modify{true};
    bool copy{true};
    bool annotate{true};
    bool fillForms{true};
    bool extractForAccessibility{true};
    bool assemble{true};
    bool printHighQuality{true};

    [[nodiscard]] static Permissions allowAll() noexcept { return {}; }
};

struct PageInfo {
    std::int32_t index{0};
    SizeF sizePt{};              // MediaBox 尺寸，單位為點
    Rotation intrinsicRotation{Rotation::None};  // 文件自帶的 /Rotate
    std::optional<std::string> label;             // 頁面標籤（i, ii, 1, 2）
};

struct DocumentInfo {
    std::string title;
    std::string author;
    std::string subject;
    std::string keywords;
    std::string creator;
    std::string producer;
    std::string pdfVersion;      // 寫出時保留原版本號，不降級
    std::int32_t pageCount{0};
    bool encrypted{false};
    bool hasXfa{false};          // 僅顯示後備內容並提示（PRD-FORM-003）
    bool hasJavaScript{false};   // 不執行，狀態列提示（PRD §8.2）
    bool hasSignatures{false};
    Permissions permissions{};
};

// 書籤節點（PRD-NAV-003，巢狀 ≥ 8 層）。
struct OutlineNode {
    std::string title;
    std::optional<std::int32_t> pageIndex;
    std::optional<PointF> destination;
    std::optional<double> zoom;
    std::string uri;             // 外部連結；點擊前需確認對話框
    std::vector<OutlineNode> children;
};

// 連結註解的目標（PRD-NAV-006）。
//
// 內部跳轉與外部網址在安全性上是兩件事：前者只是換頁，後者會把使用者帶出應用程式，
// 因此 PRD §8.2 要求開啟前必須確認。把兩者放在同一個型別裡並以 optional 區分，
// 是為了讓「忘記處理外部連結」在呼叫端看起來很明顯。
struct LinkTarget {
    RectF rect{};                          // 連結區域（頁面座標）
    std::optional<std::int32_t> pageIndex;  // 內部跳轉的目標頁
    std::string uri;                       // 外部網址；非空即代表要離開應用程式

    [[nodiscard]] bool isExternal() const noexcept { return !uri.empty(); }
    [[nodiscard]] bool isValid() const noexcept { return pageIndex.has_value() || isExternal(); }
};

}  // namespace alioth::domain
