#pragma once

// /ByteRange 完整性檢查（WBS 6.5，PRD-SIG-001）。
//
// PDFium 只把 /ByteRange 的整數陣列交出來，它不驗證那些數字是否合理，
// 也不驗證它們是否真的涵蓋整份檔案。這個檔案就是補上那一段。
//
// 為什麼要當成攻擊面而不是格式解析：
// 「簽章只涵蓋檔案的一部分」是現實世界的簽章偽造手法（Incremental Saving Attack
// 與 Shadow Attack 的共同基礎）。攻擊者把惡意內容放在 /ByteRange 沒有涵蓋的區段，
// 密碼學驗證仍然完全通過，閱讀器若只回報「簽章有效」就等於替偽造背書。
// 因此本模組把下列情形全部視為必須明確回報、不得歸類為有效：
//   1. 區段數量不是成對的偶數，或為空
//   2. 位移或長度為負、加總溢位、超出檔案尾端
//   3. 區段之間彼此重疊，或未依位移遞增
//   4. 未從位移 0 開始
//   5. 未涵蓋到檔案結尾
//   6. 中間留下超過一個空洞（正常簽章只會有一個：/Contents 本身）
//
// 本檔完全是純函數，不碰 PDFium 也不碰 OpenSSL，因此可以逐個惡意輸入單獨測試。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace alioth::engine::signature {

struct ByteRangeSegment {
    std::int64_t offset{0};
    std::int64_t length{0};

    [[nodiscard]] std::int64_t end() const noexcept { return offset + length; }
};

enum class ByteRangeVerdict {
    Valid,               // 兩段以上、無重疊、涵蓋全檔且僅有一個空洞
    Malformed,           // 數量不成對、為空、或含負值
    OutOfBounds,         // 超出檔案尾端
    Overlapping,         // 區段重疊或未遞增
    IncompleteCoverage,  // 語法合法但未涵蓋整份檔案
};

[[nodiscard]] const char* describe(ByteRangeVerdict verdict) noexcept;

struct ByteRangeCheck {
    ByteRangeVerdict verdict{ByteRangeVerdict::Malformed};
    std::vector<ByteRangeSegment> segments;
    std::vector<ByteRangeSegment> gaps;  // 未被涵蓋的區間，含尾端未涵蓋部分
    std::int64_t fileSize{0};
    std::int64_t coveredBytes{0};
    std::string detail;  // 繁體中文，可直接顯示於 Signatures 面板

    [[nodiscard]] bool ok() const noexcept { return verdict == ByteRangeVerdict::Valid; }

    // 「簽章只涵蓋部分檔案」與「簽章無效」是兩件不同的事，但兩者都不是有效。
    // 分開回報是為了讓 UI 能說出正確的原因。
    [[nodiscard]] bool partiallyCovered() const noexcept {
        return verdict == ByteRangeVerdict::IncompleteCoverage;
    }
    [[nodiscard]] std::int64_t uncoveredBytes() const noexcept {
        return fileSize - coveredBytes;
    }
};

// raw 是 PDFium 交出的整數陣列，成對解讀為 (offset, length)。
[[nodiscard]] ByteRangeCheck checkByteRange(const std::vector<int>& raw, std::int64_t fileSize);

// 依區段串出實際被簽章覆蓋的位元組。verdict 為 Malformed / OutOfBounds / Overlapping
// 時回傳空向量——那些情況下「被簽的內容」本身沒有定義，硬串出來只會產生
// 一個看似可驗但毫無意義的摘要。
[[nodiscard]] std::vector<std::uint8_t> assembleSignedBytes(const std::uint8_t* data,
                                                            std::size_t size,
                                                            const ByteRangeCheck& check);

// 「整段輸入都被涵蓋」的檢查結果。
//
// 存在理由是讓 PKCS#7 驗證能脫離 PDF 單獨測試：純函數層的呼叫端手上只有
// 一段位元組與一個簽章 blob，沒有 /ByteRange 可言。刻意做成明示呼叫的函式
// 而不是預設值，這樣「沒有做涵蓋範圍檢查」永遠是程式碼裡看得見的一行。
[[nodiscard]] ByteRangeCheck coverageOfWholeInput(std::int64_t size);

}  // namespace alioth::engine::signature
