#pragma once

// 戳記的 PDF 內容串流產生器（PRD-PAGE-004 的「寫進文件」側）。
//
// 列印時的戳記只存在於紙上；法務用途的 Bates 編號通常必須留在檔案裡，
// 讓收件方開啟 PDF 就看得到同一組號碼。那條路徑需要把繪圖指令寫成
// 內容串流，手法與 engine/annotations/appearance_stream.h 相同。
//
// 這一檔只產生位元組，不碰 PDFium、不碰檔案：真正把物件寫入文件並增量
// 儲存屬於引擎層（WBS 5.6 頁面管理）的職責，本工作包不跨過那條界線。
// 先把繪圖指令這一段做出來並測到位，接手的一方就不必重新推導座標。
//
// 座標系是 PDF 使用者空間：原點左下、Y 軸向上、單位為點。

#include <set>
#include <string>

#include "app/print/stamp_layout.h"
#include "domain/geometry.h"

namespace alioth::app::print {

struct StampStreamOptions {
    std::string fontResourceName{"F0"};  // 需存在於目標 /Resources /Font
    double fontSize{10.0};
    double red{0.0};
    double green{0.0};
    double blue{0.0};
    std::string extGStateName{};  // 空字串表示不套用透明度
    // CJK 字型的資源名稱（ADR-007）。呼叫端負責在同一個 /Resources /Font
    // 底下登記這個名字，並內嵌對應的子集。
    std::string cjkFontResourceName{"CJK"};
};

struct StampStream {
    bool valid{false};
    std::string diagnostic;  // valid 為 false 時說明原因，不做靜默失敗
    std::string content;     // 內容串流位元組，恆為 7-bit ASCII
    // 這則戳記用到的 CJK 碼點。非空時呼叫端必須子集化並內嵌字型，
    // 否則畫出來是空白——而空白的戳記看起來就像功能沒作用。
    std::set<char32_t> cjkCodepoints;
};

// PDF 實數格式化：最多四位小數、去尾隨零、不受地區設定影響。
//
// 不用 printf 系列，因為 LC_NUMERIC 被設成以逗號為小數點的地區時，
// 產出的串流會整份損毀，而那種錯誤只在特定使用者的機器上重現。
[[nodiscard]] std::string formatStreamNumber(double value);

// 依 PDF 字串規則跳脫。反斜線與括號沒跳脫會讓整份文件的括號配對失衡，
// 症狀是後續物件全部解析錯位，而不是單一戳記顯示怪異。
[[nodiscard]] std::string escapePdfLiteralString(const std::string& text);

// 產生「在指定位置畫一行文字」的內容串流。
//
// 拉丁與 CJK 分段畫（ADR-007）：兩者是不同的字型物件，編碼也不同
// （WinAnsi 單位元組 vs Identity-H 雙位元組）。整行用同一個字型的話，
// 不是中文變亂碼就是拉丁字被當成雙位元組讀掉。
//
// 內嵌字型裡找不到字形時明確失敗，不輸出「只剩拉丁字」的殘缺戳記——
// 缺字的浮水印看起來像排版問題，不像功能失敗。
[[nodiscard]] StampStream makeTextStampStream(const std::string& text, double xPt, double yPt,
                                              const StampStreamOptions& options);

// 依九宮格與邊距算出文字基線起點（PDF 座標）。
// textWidthPt / textHeightPt 由呼叫端量測，本函式不猜字型度量。
[[nodiscard]] domain::PointF stampBaselineOrigin(double pageWidthPt, double pageHeightPt,
                                                 double textWidthPt, double textHeightPt,
                                                 StampAnchor anchor, const StampMargins& margins);

}  // namespace alioth::app::print
