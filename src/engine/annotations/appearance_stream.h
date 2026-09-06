#pragma once

// 外觀串流產生器（PRD §4.4、WBS 4.2 / 4.3）。
//
// PDFium 對多數註解型別不會自動產生 /AP，缺了它 Acrobat 會自行補畫，但
// macOS 預覽與 Chrome 可能顯示不一致甚至完全不顯示。因此每一則註解都必須
// 由我們自己輸出符合 ISO 32000-2 §12.5.5 的 /AP /N Form XObject。
//
// 這一層刻意不連結 PDFium：輸入是純領域模型，輸出是位元組與字典字串，
// 沒有任何檔案 I/O 與全域狀態。全案最高風險的繪圖邏輯因此可以在毫秒級的
// 單元測試裡逐一驗證，而不必每次都繞一趟真實 PDF。
//
// 座標約定：內容串流直接以頁面預設使用者空間作圖，/BBox 等於註解 /Rect、
// /Matrix 除便利貼外皆為單位矩陣。§12.5.5 的外觀對映演算法在這個組合下
// 退化為恆等，任何座標偏移都會是我們自己算錯，不會是對映規則的鍋。

#include <array>
#include <set>
#include <string>
#include <vector>

#include "domain/annotation.h"
#include "domain/geometry.h"

namespace alioth::engine::annotations {

// 目前只需要這兩種。螢光筆必須是 Multiply，否則會蓋掉底下的文字；
// 其餘註解一律 Normal。不開放整份 PDF 混合模式表是刻意的範圍控制。
enum class BlendMode : std::uint8_t {
    Normal,
    Multiply,
};

// 一筆 /ExtGState。/CA 是描邊透明度、/ca 是填色透明度，兩者名稱只差大小寫，
// 是 PDF 規格裡最容易寫反的一對鍵。
struct ExtGState {
    std::string name{"GS0"};
    double strokeAlpha{1.0};
    double fillAlpha{1.0};
    BlendMode blend{BlendMode::Normal};
};

struct AppearanceOptions {
    // 頁面 /Rotate。註解座標不受它影響，只有便利貼圖示需要反向旋轉才不會躺著。
    domain::Rotation pageRotation{domain::Rotation::None};

    // 目標容器能否附帶 /Resources。PDFium 的 FPDFAnnot_SetAP 只建立串流本身，
    // 不會建 /Resources，此時引用 /GS0 會是懸空名稱。設為 false 時改為不輸出
    // gs 運算子，透明度退回註解字典的 /CA 由檢視器套用。
    bool resourcesSupported{true};
};

struct Appearance {
    bool valid{false};
    std::string diagnostic{};  // valid 為 false 時說明原因，不做靜默失敗

    std::string content{};                    // /AP /N 的內容串流位元組，恆為 7-bit ASCII
    std::vector<ExtGState> extGStates{};      // 需寫進 /Resources /ExtGState 的項目

    // 因 resourcesSupported 為 false 而被迫捨棄的透明度／混合模式。
    // 寫入層必須把它往上報，不能讓「螢光筆變成不透明色塊」這種降級無聲發生。
    bool resourcesElided{false};

    // 內容串流是否引用了 /Helv（FreeText 家族：Text Box／Typewriter／Callout）。
    // 寫入層據此決定要不要在 /Resources 補上 /Font /Helv，理由與 extGStates
    // 相同：PDFium 的 FPDFAnnot_SetAP 建不出 /Resources，缺了字型項目 /Helv
    // 就是懸空名稱，文字完全不會被畫出來。
    bool needsFont{false};

    // 內容串流是否引用了內嵌的 CJK 字型 /CJK（ADR-007）。
    //
    // cjkCodepoints 是實際用到的字，寫入層據此做子集並嵌進 /Resources /Font /CJK。
    // 兩者必須成對：needsCjkFont 為 true 卻沒有註冊字型，中文就完全不會被畫出來
    // ——不是亂碼，是整段消失，而且沒有任何錯誤訊息。
    bool needsCjkFont{false};
    std::set<char32_t> cjkCodepoints{};

    // 內容串流是否引用了 /Im0（自訂圖片圖章，PRD-ANN-006）。寫入層據此把影像
    // 內嵌成 XObject 並註冊到 /AP 的 /Resources /XObject。理由與 needsFont 相同：
    // 缺了資源項目就是懸空名稱，圖章會是一片空白，而且不會有任何錯誤訊息。
    bool needsStampImage{false};

    domain::RectF bbox{};                     // /BBox，同時也是建議的註解 /Rect
    std::array<double, 6> matrix{1, 0, 0, 1, 0, 0};  // /Matrix

    [[nodiscard]] bool requiresResources() const noexcept { return !extGStates.empty(); }

    // 序列化 /Resources 字典。無資源時回傳 "<< >>"，讓呼叫端不必分支。
    [[nodiscard]] std::string resourcesDictionary() const;
};

// 由註解模型產生外觀串流。純函數，同輸入必得同輸出。
[[nodiscard]] Appearance generateAppearance(const domain::Annotation& annotation,
                                            const AppearanceOptions& options = {});

// PDF 實數格式化：最多四位小數、去除尾隨零、不受地區設定影響。
//
// 不用 printf 系列是因為 LC_NUMERIC 被改成使用逗號小數點的地區時，
// 產出的內容串流會整份損毀，而那種錯誤只在特定使用者機器上重現。
[[nodiscard]] std::string formatNumber(double value);

}  // namespace alioth::engine::annotations
