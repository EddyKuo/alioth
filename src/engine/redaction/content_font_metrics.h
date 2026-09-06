#pragma once

// 內容串流的字寬查表（WBS 11）。
//
// 判斷一段顯示字串有沒有落進塗黑區域，需要知道它畫出來有多寬。PDF 的字寬
// 來自字型字典的 /Widths（簡單字型）或 /W（CID 字型），單位是 1/1000 em。
//
// 拿不到寬度時的預設值刻意偏大（1 em）而不是常見的 0.5 em：偏大會讓外框
// 過長，最壞情況是多刪一些字；偏小則會讓落在區域邊緣的字被判定為區域外而
// 留在檔案裡。這兩種錯誤的代價不對稱，Redaction 必須往「多刪」的方向倒。

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "engine/objects/pdf_object.h"

namespace alioth::engine::redaction {

// 解析字型字典時需要的間接參照解析器。
using Resolver = std::function<objects::PdfObject(const objects::PdfObject&)>;

class FontMetrics {
public:
    FontMetrics() = default;

    // 由 /Font 資源中的一個字型字典建立。無法辨識的字型會得到一份「全部用
    // 預設寬度」的度量，而不是失敗——字型認不得不該讓整次塗黑無法進行。
    [[nodiscard]] static FontMetrics fromFontDictionary(const objects::PdfObject& fontDict,
                                                        const Resolver& resolve);

    // Identity-H 這類 CID 編碼一個字碼佔兩個位元組。切錯位元組數會讓寬度
    // 累加完全錯亂，而不是小幅偏差。
    [[nodiscard]] bool isTwoByte() const noexcept { return twoByte_; }

    // 把字串位元組切成字碼。
    [[nodiscard]] std::vector<std::uint32_t> decode(const std::string& bytes) const;

    // 字碼寬度，單位 em。
    [[nodiscard]] double width(std::uint32_t code) const;

    // 字型的上下界（em），用來估算顯示字串的垂直範圍。
    [[nodiscard]] double ascent() const noexcept { return ascent_; }
    [[nodiscard]] double descent() const noexcept { return descent_; }

private:
    std::map<std::uint32_t, double> widths_{};
    double defaultWidth_{1.0};
    double ascent_{1.0};
    double descent_{-0.35};
    bool twoByte_{false};
};

}  // namespace alioth::engine::redaction
