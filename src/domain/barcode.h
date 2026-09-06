#pragma once

// 一維條碼編碼：Code 128（PRD-ENH-007「新增條碼」、PRD-FORM-025「條碼欄位」共用）。
//
// 刻意只做 Code 128，不做 QR：QR 需要 Reed-Solomon 糾錯碼與資料遮罩選擇演算法，
// 複雜度與本工作包其餘五項需求的總和相當，而 PRD 把它列為 C（有餘力才做）。
// 明確不做比做一半更誠實——半成品的 QR（例如缺糾錯碼）會產生「看起來像 QR
// 但掃不出來」的輸出，那比不提供功能更糟。若後續要做，見檔尾的 BLOCKED 說明。
//
// header-only：與 domain/document_source.h 同一個理由，這裡沒有跨翻譯單元的
// 狀態，不必讓其他工作包多背一次重編譯。
//
// 只接受可列印 ASCII（0x20–0x7E，Code 128 Subset B 的字元範圍）。這與專案既有的
// 「CJK 字型內嵌授權策略未定案」立場一致：條碼的資料內容一樣要能被人眼核對
// （欄位下方通常會印出人讀文字），非 ASCII 一樣畫不出來。
//
// **已知風險（必須在報告中揭露，不得靜默）**：Code 128 的 107 組符號寬度表
// 是本檔最容易被抄錯的部分，抄錯的後果是「印出來像條碼、但掃描器讀不出來」——
// 這種錯誤在畫面或列印預覽上完全看不出來，只有實際掃描才會發現。本次實作是
// 在沒有網路存取、無法比對 ISO/IEC 15417 官方表格的沙盒環境中，依訓練記憶
// 重建這份表格。程式內建的測試只能驗證「校驗碼公式」（可獨立以純算術驗證，
// 見 computeChecksumValue 的文件）與「表格內部一致性」（每組寬度總和是否
// 正確、編碼後解碼能否還原），無法驗證這份表格是否與官方標準逐位元組相符。
// 交付前務必用一台實體或軟體條碼掃描器驗證印出的樣張，這是本模組上線前
// 的强制步驟，不是可選項。

#include <array>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "domain/geometry.h"

namespace alioth::domain::barcode {

// Code 128 的三個子集。目前只實作 B（可列印 ASCII 32–126 直接對映），
// A（控制字元）與 C（雙數字壓縮）不實作：表單欄位與一般文件的條碼內容
// 幾乎都是可列印文字或數字，Subset B 已能表示全部，只是數字內容的密度
// 不如 Subset C——對「新增條碼」這種一次性標記而言，密度不是關鍵指標。
enum class Code128Subset : std::uint8_t { B };

// 一段連續的模組（module）寬度，交替代表墨（bar）與空白（space）。
// 序列恆以 bar 起始：這是 Code 128 起始碼的定義，不是實作選擇。
struct BarcodeRun {
    int widthModules{0};
    bool bar{true};
};

struct Code128Result {
    bool ok{false};
    std::string diagnostic;

    std::string text;               // 原始輸入（人讀文字用）
    std::vector<int> symbolValues;  // 不含起始碼／校驗碼／終止碼，供測試比對
    int checksumValue{0};

    std::vector<BarcodeRun> runs;   // 含起始碼、資料、校驗碼、終止碼的完整模組序列
    int totalModules{0};            // runs 的寬度總和，含終止碼後的收尾 bar

    [[nodiscard]] std::size_t symbolCount() const noexcept {
        // 起始碼 + 資料 + 校驗碼 + 終止碼。
        return symbolValues.size() + 3;
    }
};

namespace detail {

// Code 128 符號的模組寬度表。索引 0–102 為資料符號，103/104/105 為
// START A/B/C，106 為 STOP。每個字串是六個（STOP 為七個）介於 1–4 的數字，
// 依序代表 bar、space、bar、space、bar、space（STOP 多一個收尾 bar）。
//
// 資料符號的寬度總和恆為 11 個模組，STOP 為 13 個——這是 buildStructuralCheck
// 用來抓「打錯一個數字」這類錯誤的依據，見檔案開頭的風險說明。
inline constexpr std::array<std::string_view, 107> kPatterns = {
    "212222", "222122", "222221", "121223", "121322", "131222", "122213", "122312",
    "132212", "221213", "221312", "231212", "112232", "122132", "122231", "113222",
    "123122", "123221", "223211", "221132", "221231", "213212", "223112", "312131",
    "311222", "321122", "321221", "312212", "322112", "322211", "212123", "212321",
    "232121", "111323", "131123", "131321", "112313", "132113", "132311", "211313",
    "231113", "231311", "112133", "112331", "132131", "113123", "113321", "133121",
    "313121", "211331", "231131", "213113", "213311", "213131", "311123", "311321",
    "331121", "312113", "312311", "332111", "314111", "221411", "431111", "111224",
    "111422", "121124", "121421", "141122", "141221", "112214", "112412", "122114",
    "122411", "142112", "142211", "241211", "221114", "413111", "241112", "134111",
    "111242", "121142", "121241", "114212", "124112", "124211", "411212", "421112",
    "421211", "212141", "214121", "412121", "111143", "111341", "131141", "114113",
    "114311", "411113", "411311", "113141", "114131", "311141", "411131",
    "211412",  // 103: START A
    "211214",  // 104: START B
    "211232",  // 105: START C
    "2331112", // 106: STOP（7 位數，多一個收尾 bar）
};

inline constexpr int kStartA = 103;
inline constexpr int kStartB = 104;
inline constexpr int kStartC = 105;
inline constexpr int kStop = 106;

[[nodiscard]] inline int patternWidth(std::string_view pattern) noexcept {
    int sum = 0;
    for (const char c : pattern) sum += (c - '0');
    return sum;
}

// 把一個符號值展開成一串模組寬度，接續在既有序列之後（延續 bar/space 極性）。
inline void appendSymbol(std::vector<BarcodeRun>& runs, int value) {
    const std::string_view pattern = kPatterns[static_cast<std::size_t>(value)];
    bool bar = true;
    for (const char c : pattern) {
        runs.push_back(BarcodeRun{c - '0', bar});
        bar = !bar;
    }
}

}  // namespace detail

// 校驗碼公式（ISO/IEC 15417 §4.3.5，mod 103）：checksum = (start + Σ value[i] *
// (i+1)) mod 103，i 為 0 起算的資料符號索引。這條公式是純算術，可以獨立於
// 寬度表以手算驗證，不依賴上面那張有風險的表格——這是本模組唯一能不靠外部
// 校驗工具就確定正確的部分。
[[nodiscard]] inline int computeChecksumValue(int startValue, const std::vector<int>& values) noexcept {
    long long sum = startValue;
    for (std::size_t i = 0; i < values.size(); ++i) {
        sum += static_cast<long long>(values[i]) * static_cast<long long>(i + 1);
    }
    return static_cast<int>(sum % 103);
}

// 掃描輸入是否全為 Code 128 Subset B 可表示的可列印 ASCII（0x20–0x7E）。
[[nodiscard]] inline bool isSubsetBText(std::string_view text) noexcept {
    if (text.empty()) return false;
    for (const unsigned char c : text) {
        if (c < 0x20 || c > 0x7E) return false;
    }
    return true;
}

// 編碼。輸入必須是非空、全為可列印 ASCII 的文字，否則明確失敗
// （不猜測、不做非 ASCII 的替代表示——理由與 domain/create 的 ASCII-only
// 規則相同：靜默輸出的後果是使用者看不出條碼為什麼印不出來）。
[[nodiscard]] inline Code128Result encodeCode128(std::string_view text,
                                                 Code128Subset subset = Code128Subset::B) {
    Code128Result result;
    result.text = std::string(text);

    if (!isSubsetBText(text)) {
        result.diagnostic =
            text.empty() ? "條碼內容不可為空"
                         : "條碼內容含 Code 128 Subset B 無法表示的字元（僅支援可列印 ASCII）";
        return result;
    }
    if (subset != Code128Subset::B) {
        result.diagnostic = "目前只實作 Code 128 Subset B";
        return result;
    }

    result.symbolValues.reserve(text.size());
    for (const unsigned char c : text) {
        result.symbolValues.push_back(static_cast<int>(c) - 0x20);
    }

    const int startValue = detail::kStartB;
    result.checksumValue = computeChecksumValue(startValue, result.symbolValues);

    result.runs.reserve((result.symbolValues.size() + 3) * 6);
    detail::appendSymbol(result.runs, startValue);
    for (const int value : result.symbolValues) detail::appendSymbol(result.runs, value);
    detail::appendSymbol(result.runs, result.checksumValue);
    detail::appendSymbol(result.runs, detail::kStop);

    int total = 0;
    for (const BarcodeRun& run : result.runs) total += run.widthModules;
    result.totalModules = total;

    result.ok = true;
    return result;
}

// 結構性自我檢查：每個資料符號（0–105）的寬度總和必須是 11，STOP（106）必須是
// 13，且每個數字必須落在 1–4。這只抓得出表格內部矛盾（例如手誤把某個數字多打
// 或少打一位），抓不出「這組寬度是否真的對應標準規定的那個符號值」——後者需要
// 外部掃描器驗證，見檔案開頭的風險說明。
[[nodiscard]] inline bool patternTableStructurallyValid() noexcept {
    for (std::size_t i = 0; i < detail::kPatterns.size(); ++i) {
        const std::string_view pattern = detail::kPatterns[i];
        const bool isStop = (i == detail::kStop);
        if (pattern.size() != (isStop ? std::size_t{7} : std::size_t{6})) return false;
        for (const char c : pattern) {
            if (c < '1' || c > '4') return false;
        }
        if (detail::patternWidth(pattern) != (isStop ? 13 : 11)) return false;
    }
    return true;
}

// 解碼（僅供測試用的往返驗證）：把 runs 依 6（或末段 7）一組切回符號值，
// 用同一張表反查。這驗證的是「編碼器有沒有把自己的表用對」，不是表格本身
// 對不對標準——兩者的差異已在檔案開頭寫明。
struct Code128Decoded {
    bool ok{false};
    std::string diagnostic;
    std::vector<int> symbolValues;  // 含起始碼，不含終止碼
};

[[nodiscard]] inline Code128Decoded decodeCode128(const std::vector<BarcodeRun>& runs) {
    Code128Decoded decoded;
    std::size_t index = 0;
    while (index < runs.size()) {
        const bool isLast = (runs.size() - index) == 7;
        const std::size_t groupSize = isLast ? 7 : 6;
        if (index + groupSize > runs.size()) {
            decoded.diagnostic = "模組序列長度不是 6 的整數倍（加末端 7）";
            return decoded;
        }
        std::string pattern;
        pattern.reserve(groupSize);
        for (std::size_t i = 0; i < groupSize; ++i) {
            pattern.push_back(static_cast<char>('0' + runs[index + i].widthModules));
        }
        bool found = false;
        for (std::size_t value = 0; value < detail::kPatterns.size(); ++value) {
            if (detail::kPatterns[value] == pattern) {
                // 終止碼（7 位數的最後一組）不計入 symbolValues：函式的合約是
                // 「含起始碼、不含終止碼」，與 encodeCode128 的 symbolValues
                // （不含起始碼也不含終止碼）故意錯開一格，呼叫端比對時才不必
                // 另外再減掉終止碼。
                if (!isLast) decoded.symbolValues.push_back(static_cast<int>(value));
                found = true;
                break;
            }
        }
        if (!found) {
            decoded.diagnostic = "模組序列找不到對應的符號：" + pattern;
            return decoded;
        }
        index += groupSize;
    }
    decoded.ok = true;
    return decoded;
}

// ---------------------------------------------------------------------------
// 內容串流繪製
// ---------------------------------------------------------------------------
//
// 只畫 bar（墨），space（空白）留白給頁面背景——這假設條碼畫在白底之上，
// 與「新增條碼」功能的實際使用情境（貼在文件版面的空白處）一致。若貼在
// 非白背景上，呼叫端必須自行先畫一塊白底矩形，本函式不做這個假設。
//
// rect 是條碼「有墨」區域，不含安規要求的靜區（quiet zone）——Code 128 規格
// 建議靜區至少 10 個模組寬，這是版面配置的責任（呼叫端在 rect 左右各留出
// 相當於 10 個模組的空白），不屬於繪製本身。

namespace detail {
[[nodiscard]] inline std::string formatFixed(double value) {
    std::ostringstream stream;
    stream.precision(3);
    stream << std::fixed << value;
    std::string text = stream.str();
    // 去掉多餘的尾端零與小數點，內容串流的位元組數這裡不是重點但保持乾淨。
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
    return text.empty() ? "0" : text;
}
}  // namespace detail

[[nodiscard]] inline std::string barcodeBarsContentStream(const Code128Result& code,
                                                           const RectF& rect) {
    if (!code.ok || code.totalModules <= 0 || rect.isEmpty()) return {};

    const double moduleWidth = rect.width() / static_cast<double>(code.totalModules);
    std::string out = "q\n0 0 0 rg\n";
    double x = rect.left;
    for (const BarcodeRun& run : code.runs) {
        const double width = moduleWidth * static_cast<double>(run.widthModules);
        if (run.bar) {
            out += detail::formatFixed(x) + " " + detail::formatFixed(rect.bottom) + " " +
                   detail::formatFixed(width) + " " + detail::formatFixed(rect.height()) + " re\n";
        }
        x += width;
    }
    out += "f\nQ\n";
    return out;
}

}  // namespace alioth::domain::barcode
