#pragma once

// FreeText 家族（Text Box / Typewriter / Callout，WP24）共用的文字量測與換行。
//
// 與 engine/objects/content_stream_appender.cpp 同一個立場：CJK 字型內嵌授權策略
// 未定案（CLAUDE.md 待決策清單），因此對無法以 WinAnsi/Helvetica 表示的內容
// 一律明確失敗，不輸出殘缺或亂碼的位元組。這裡刻意不重用
// engine/formbuild/field_appearance.h 的 foldToWinAnsi/estimateHelveticaWidth——
// 那一份是「靜默丟棄非 ASCII 字元」的舊寬鬆策略，兩者语义不同，混用只會讓
// 呼叫端搞不清楚自己踩到哪一條規則。engine/formbuild 屬於另一個工作包，
// 不在本檔異動範圍內；此處是 ANN 家族（WP24）自己的權威版本。
//
// 字寬表與 engine/formbuild/field_appearance.cpp::estimateHelveticaWidth 相同
// （Helvetica 的 AFM 概略分佈），因為那組數字本身沒有錯，錯的是拿到非法輸入
// 時的處置方式。

#include <string>
#include <vector>

#include "domain/geometry.h"

namespace alioth::engine::annotations {

// FreeText 家族（Text Box／Typewriter／Callout）內文與框緣的內縮量。
// 這裡是唯一定義：appearance_stream.cpp 畫外觀時、以及 app 層算 Fit Box
// 高度時都要用同一個數字，兩邊算出來的結果才會對得上——算的時候留 3pt、
// 畫的時候留 2pt 會讓貼合後的框仍然裁掉最後一行的一小截。
inline constexpr double kFreeTextPaddingPt = 2.0;

struct TextFitOptions {
    double fontSize{12.0};
    // <= 0 代表不換行，只用於量測單行的自然寬度（例如自動決定文字框初始寬度）。
    double maxWidth{0.0};
    double lineSpacingRatio{1.2};  // 行距＝fontSize × 這個係數
};

struct TextLayoutResult {
    bool ok{false};
    std::string diagnostic;

    std::vector<std::string> lines{};  // 已換行、保證可用 WinAnsi/Helvetica 畫出
    double lineHeight{0.0};
    double contentWidth{0.0};   // 最寬一行的量測寬度
    double contentHeight{0.0};  // lines.size() * lineHeight
};

// 單行文字（僅可列印 ASCII）以 Helvetica 估算的寬度，單位為點。
[[nodiscard]] double estimateTextWidth(const std::string& asciiLine, double fontSize);

// 量測並換行。輸入必須是可列印 ASCII 加上 \n / \r\n 的換行；遇到其餘字元
// （包含 CJK）明確回傳失敗，理由見檔案頂端註解。
[[nodiscard]] TextLayoutResult layoutText(const std::string& utf8Text, const TextFitOptions& options);

// PRD-ANN-030 Fit Box by Text Content：讓文字框的高度貼合內容。
//
// 錨點是 box 的左上角（left、top 不變），只有 bottom（因而 height）依換行結果
// 重算；width 若已經 > 0 就保持不變（使用者已手動拖出寬度），否則退化成
// 單行文字的自然寬度——這與 Acrobat「未拖曳寬度時貼合單行」的行為一致。
struct FitBoxOptions {
    double fontSize{12.0};
    double paddingPt{kFreeTextPaddingPt};
    double minHeight{0.0};  // 只在使用者已手動決定高度時可能用得到；預設不限制

    // 固定框模式（PRD-ANN-030 的另一半）：> 0 時框的高度**不變**，改成縮小
    // 字級直到內容塞得下。使用者已經把框拖成想要的大小時，把框撐高等於
    // 蓋掉他底下的內容——那時該讓字變小，不是讓框變大。
    double maxHeight{0.0};
    // 縮到這個字級仍塞不下就停手，並在結果裡標明溢出。繼續縮下去只會得到
    // 一塊讀不了的灰影，而使用者不會知道那是「字太小」還是「畫壞了」。
    double minFontSize{4.0};
    // 每次縮小的級距。太細會讓重排跑很多輪，太粗會浪費可用空間。
    double fontSizeStepPt{0.5};
};

struct FitBoxResult {
    bool ok{false};
    std::string diagnostic;
    domain::RectF rect{};
    TextLayoutResult layout{};
    // 實際採用的字級。固定框模式下可能小於 FitBoxOptions::fontSize。
    double fontSize{0.0};
    // 已經縮到 minFontSize 仍塞不下。呼叫端要讓使用者看見，不能靜默裁掉。
    bool overflows{false};
};

[[nodiscard]] FitBoxResult fitBoxByTextContent(const domain::RectF& box, const std::string& utf8Text,
                                               const FitBoxOptions& options = {});

}  // namespace alioth::engine::annotations
