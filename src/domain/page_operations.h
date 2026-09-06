#pragma once

// 頁面管理的意圖模型與純邏輯（PRD-PAGE-001/002/005/006/012，WBS 5.6–5.8）。
//
// 為什麼這一層存在：頁面操作的錯誤絕大多數不是 PDFium 用錯，而是「使用者輸入
// 的頁面範圍被怎麼解讀」與「多個索引同時異動時順序怎麼變」這兩件事沒有講清楚。
// 那兩件事完全不需要 PDF 引擎就能定義與驗證，所以放在領域層，讓引擎層只剩下
// 「照著已經算好的結果呼叫 PDFium」這一件事。
//
// 邊界情況一律有明確定義而不是未定義行為：越界、重疊、倒序、開放結尾、空輸入
// 各自對應一個列舉值，呼叫端必須處理。沉默地「猜使用者的意思」是這個模組最不該做的事。
//
// 對外的頁碼慣例：使用者字串是 1 起算（PDF 慣例），本模組的所有 API 與回傳值
// 一律 0 起算（程式慣例）。轉換只發生在 parsePageRange 與 formatPageRange 兩處。

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace alioth::domain::pages {

// ---------------------------------------------------------------------------
// 頁面旋轉
// ---------------------------------------------------------------------------

// 文件層旋轉，對應 PDF 頁面字典的 /Rotate，也對應 PDFium 的 0–3 編碼。
// 與檢視層旋轉是兩件事：檢視層只影響畫面，不寫回文件；這裡的每一個值都會改檔案。
enum class PageRotation : int {
    None = 0,
    Clockwise90 = 1,
    Half = 2,
    CounterClockwise90 = 3,
};

[[nodiscard]] constexpr int quarterTurns(PageRotation rotation) noexcept {
    return static_cast<int>(rotation);
}

[[nodiscard]] constexpr int degreesOf(PageRotation rotation) noexcept {
    return static_cast<int>(rotation) * 90;
}

// 負數與超過一圈都要能吃：使用者按三次「逆時針」與按一次「順時針」是同一件事。
[[nodiscard]] constexpr PageRotation rotationFromQuarterTurns(int turns) noexcept {
    const int normalized = ((turns % 4) + 4) % 4;
    return static_cast<PageRotation>(normalized);
}

// 非 90 的倍數不做四捨五入而是回報失敗：PDF 的 /Rotate 只允許 90 的倍數，
// 悄悄取整會讓呼叫端以為自己設定成功了。
[[nodiscard]] constexpr bool rotationFromDegrees(int degrees, PageRotation& out) noexcept {
    if (degrees % 90 != 0) return false;
    out = rotationFromQuarterTurns(degrees / 90);
    return true;
}

[[nodiscard]] constexpr PageRotation combine(PageRotation base, PageRotation delta) noexcept {
    return rotationFromQuarterTurns(quarterTurns(base) + quarterTurns(delta));
}

// ---------------------------------------------------------------------------
// 頁面範圍解析
// ---------------------------------------------------------------------------

enum class RangeParseStatus {
    Ok,
    EmptyInput,        // 空字串、只有空白、或只有分隔符
    SyntaxError,       // 非數字字元、空的區段（"1,,3"）、多個減號
    ZeroOrNegative,    // 使用者寫了 0 或負數；PDF 頁碼從 1 起
    DescendingRange,   // 倒序區間且政策為 Reject
    OutOfRange,        // 超出文件頁數且政策為 Reject
    EmptyResult,       // 語法正確但夾取後一頁都不剩
    InvalidPageCount,  // pageCount <= 0，沒有任何頁碼可以是合法的
};

// 越界的處置。預設拒絕：讓「打錯字」與「刻意要全部剩下的頁」在 API 層就分得開。
enum class OutOfRangePolicy {
    Reject,
    // 區間的端點夾到 [1, pageCount]；完全落在文件外的區間與單頁**被丟棄**而不是
    // 夾到最後一頁——把 "7" 夾成第 5 頁會安靜地擷取出使用者沒要的內容。
    Clamp,
};

// 重疊的處置。預設合併：「1-3,2-5」是使用者常打的東西，多數情境要的是 1-5 各一次。
enum class DuplicatePolicy {
    Merge,  // 去重，保留第一次出現的位置
    Keep,   // 原樣保留，讓「複製頁面」這種需要重複的情境也能用同一個解析器
};

// 倒序區間（"5-3"）的處置。預設展開成遞減序列：那是「反轉這一段」最自然的寫法，
// 而且結果完全確定；要嚴格的呼叫端可以改成 Reject。
enum class DescendingPolicy {
    Expand,
    Reject,
};

struct PageRangeOptions {
    OutOfRangePolicy outOfRange{OutOfRangePolicy::Reject};
    DuplicatePolicy duplicates{DuplicatePolicy::Merge};
    DescendingPolicy descending{DescendingPolicy::Expand};
};

struct PageRangeParse {
    RangeParseStatus status{RangeParseStatus::Ok};
    std::vector<int> pages;          // 0 起算，順序即使用者書寫順序
    std::size_t errorOffset{0};      // SyntaxError 時指向出問題的字元位置

    [[nodiscard]] bool ok() const noexcept { return status == RangeParseStatus::Ok; }
};

namespace detail {

inline bool isSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

inline std::string_view trim(std::string_view text, std::size_t& offset) noexcept {
    std::size_t begin = 0;
    while (begin < text.size() && isSpace(text[begin])) ++begin;
    std::size_t end = text.size();
    while (end > begin && isSpace(text[end - 1])) --end;
    offset += begin;
    return text.substr(begin, end - begin);
}

// 回傳值：-1 代表不是合法的十進位整數。刻意不接受正負號——頁碼的符號在語法層
// 就沒有意義，把 "-3" 當成「到第 3 頁」是由區間規則決定的，不是由數字決定的。
inline long long parseNumber(std::string_view text) noexcept {
    if (text.empty()) return -1;
    long long value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return -1;
        value = value * 10 + (c - '0');
        if (value > 1'000'000'000LL) return 1'000'000'000LL;  // 飽和，避免溢位
    }
    return value;
}

}  // namespace detail

// 解析 "1-3,7,9-" 這類使用者輸入。
//
// 語法：以 ',' 或 ';' 分隔的區段，每段是 "N"、"A-B"、"A-"（到最後一頁）或
// "-B"（從第一頁起）。空白一律忽略。單獨一個 "-" 是語法錯誤而不是「全部」，
// 因為「全部」在 UI 上一定有別的入口，讓它有兩種寫法只會讓錯字被吃掉。
[[nodiscard]] inline PageRangeParse parsePageRange(std::string_view text, int pageCount,
                                                   const PageRangeOptions& options = {}) {
    PageRangeParse result;
    if (pageCount <= 0) {
        result.status = RangeParseStatus::InvalidPageCount;
        return result;
    }

    std::size_t cursor = 0;
    std::size_t trimOffset = 0;
    const std::string_view whole = detail::trim(text, trimOffset);
    if (whole.empty()) {
        result.status = RangeParseStatus::EmptyInput;
        return result;
    }

    std::vector<int> pages;
    bool sawAnyToken = false;
    bool droppedByClamp = false;

    while (cursor <= whole.size()) {
        const std::size_t separator = std::min(whole.find(',', cursor), whole.find(';', cursor));
        const std::size_t tokenEnd = separator == std::string_view::npos ? whole.size() : separator;

        std::size_t tokenOffset = trimOffset + cursor;
        const std::string_view token =
            detail::trim(whole.substr(cursor, tokenEnd - cursor), tokenOffset);

        // 空區段是錯字（"1,,3" 或結尾多一個逗號），不是「沒關係的空白」。
        if (token.empty()) {
            result.status = RangeParseStatus::SyntaxError;
            result.errorOffset = tokenOffset;
            return result;
        }
        sawAnyToken = true;

        const std::size_t dash = token.find('-');
        long long first = 0;
        long long last = 0;

        if (dash == std::string_view::npos) {
            const long long value = detail::parseNumber(token);
            if (value < 0) {
                result.status = RangeParseStatus::SyntaxError;
                result.errorOffset = tokenOffset;
                return result;
            }
            if (value == 0) {
                result.status = RangeParseStatus::ZeroOrNegative;
                result.errorOffset = tokenOffset;
                return result;
            }
            first = last = value;
        } else {
            std::size_t lhsOffset = tokenOffset;
            std::size_t rhsOffset = tokenOffset + dash + 1;
            const std::string_view lhs = detail::trim(token.substr(0, dash), lhsOffset);
            const std::string_view rhs = detail::trim(token.substr(dash + 1), rhsOffset);

            if (rhs.find('-') != std::string_view::npos) {
                result.status = RangeParseStatus::SyntaxError;
                result.errorOffset = rhsOffset + rhs.find('-');
                return result;
            }
            if (lhs.empty() && rhs.empty()) {
                result.status = RangeParseStatus::SyntaxError;
                result.errorOffset = tokenOffset;
                return result;
            }

            first = lhs.empty() ? 1 : detail::parseNumber(lhs);
            last = rhs.empty() ? pageCount : detail::parseNumber(rhs);
            if (first < 0 || last < 0) {
                result.status = RangeParseStatus::SyntaxError;
                result.errorOffset = first < 0 ? lhsOffset : rhsOffset;
                return result;
            }
            if (first == 0 || last == 0) {
                result.status = RangeParseStatus::ZeroOrNegative;
                result.errorOffset = tokenOffset;
                return result;
            }
        }

        const bool descending = first > last;
        if (descending && options.descending == DescendingPolicy::Reject) {
            result.status = RangeParseStatus::DescendingRange;
            result.errorOffset = tokenOffset;
            return result;
        }

        long long low = descending ? last : first;
        long long high = descending ? first : last;
        if (low > pageCount || high < 1) {
            if (options.outOfRange == OutOfRangePolicy::Reject) {
                result.status = RangeParseStatus::OutOfRange;
                result.errorOffset = tokenOffset;
                return result;
            }
            droppedByClamp = true;
            if (separator == std::string_view::npos) break;
            cursor = separator + 1;
            continue;
        }
        if (low < 1 || high > pageCount) {
            if (options.outOfRange == OutOfRangePolicy::Reject) {
                result.status = RangeParseStatus::OutOfRange;
                result.errorOffset = tokenOffset;
                return result;
            }
            droppedByClamp = true;
            low = std::max<long long>(low, 1);
            high = std::min<long long>(high, pageCount);
        }

        if (descending) {
            for (long long p = high; p >= low; --p) pages.push_back(static_cast<int>(p) - 1);
        } else {
            for (long long p = low; p <= high; ++p) pages.push_back(static_cast<int>(p) - 1);
        }

        if (separator == std::string_view::npos) break;
        cursor = separator + 1;
    }

    if (!sawAnyToken) {
        result.status = RangeParseStatus::EmptyInput;
        return result;
    }

    if (options.duplicates == DuplicatePolicy::Merge) {
        std::vector<int> unique;
        unique.reserve(pages.size());
        std::vector<bool> seen(static_cast<std::size_t>(pageCount), false);
        for (const int page : pages) {
            if (seen[static_cast<std::size_t>(page)]) continue;
            seen[static_cast<std::size_t>(page)] = true;
            unique.push_back(page);
        }
        pages.swap(unique);
    }

    if (pages.empty()) {
        // 只有在確實丟掉了東西時才叫 EmptyResult；語法正確又什麼都沒丟卻是空的，
        // 在目前的文法下不可能發生，真發生了就是解析器有 bug。
        result.status = droppedByClamp ? RangeParseStatus::EmptyResult : RangeParseStatus::SyntaxError;
        return result;
    }

    result.pages = std::move(pages);
    return result;
}

// 0 起算的頁碼清單 → 使用者看得懂的 1 起算字串。只有「連續遞增」才會被縮成區間；
// 遞減與跳號一律逐頁列出，因為把 "5,4,3" 印成 "5-3" 會讓往返轉換的語意依賴解析政策。
[[nodiscard]] inline std::string formatPageRange(const std::vector<int>& zeroBasedPages) {
    std::string out;
    std::size_t i = 0;
    while (i < zeroBasedPages.size()) {
        std::size_t j = i;
        while (j + 1 < zeroBasedPages.size() && zeroBasedPages[j + 1] == zeroBasedPages[j] + 1) ++j;
        if (!out.empty()) out += ',';
        out += std::to_string(zeroBasedPages[i] + 1);
        if (j > i) {
            out += '-';
            out += std::to_string(zeroBasedPages[j] + 1);
        }
        i = j + 1;
    }
    return out;
}

[[nodiscard]] inline std::vector<int> allPages(int pageCount) {
    std::vector<int> pages;
    for (int i = 0; i < pageCount; ++i) pages.push_back(i);
    return pages;
}

enum class PageParity { All, Odd, Even };

// 奇偶以**使用者看到的頁碼**（1 起算）判定，不是以索引判定。第 1 頁是奇數頁。
[[nodiscard]] inline std::vector<int> filterByParity(const std::vector<int>& zeroBasedPages,
                                                     PageParity parity) {
    if (parity == PageParity::All) return zeroBasedPages;
    std::vector<int> out;
    for (const int page : zeroBasedPages) {
        const bool odd = (page % 2) == 0;
        if ((parity == PageParity::Odd) == odd) out.push_back(page);
    }
    return out;
}

// ---------------------------------------------------------------------------
// 操作意圖
// ---------------------------------------------------------------------------

enum class OperationStatus {
    Ok,
    EmptySelection,      // 沒有選任何頁；空操作與「全選」是不同的意思，不猜
    IndexOutOfRange,
    DuplicateSelection,  // 同一頁在選取清單裡出現兩次（刪除／搬移不允許）
    InvalidDestination,
    InvalidGeometry,     // 頁面寬高非正
    InvalidCount,        // 插入張數 <= 0
    WouldEmptyDocument,  // PDF 至少要有一頁；刪光全部是非法而不是「產生空文件」
};

[[nodiscard]] constexpr const char* describe(OperationStatus status) noexcept {
    switch (status) {
        case OperationStatus::Ok:                 return "成功";
        case OperationStatus::EmptySelection:     return "未選取任何頁面";
        case OperationStatus::IndexOutOfRange:    return "頁碼超出文件範圍";
        case OperationStatus::DuplicateSelection: return "選取清單含重複頁碼";
        case OperationStatus::InvalidDestination: return "目標位置不合法";
        case OperationStatus::InvalidGeometry:    return "頁面尺寸不合法";
        case OperationStatus::InvalidCount:       return "數量不合法";
        case OperationStatus::WouldEmptyDocument: return "操作會刪光文件所有頁面";
    }
    return "未知狀態";
}

// A4 直式，插入空白頁的預設尺寸。
inline constexpr double kA4WidthPt = 595.0;
inline constexpr double kA4HeightPt = 842.0;

struct InsertBlankPages {
    int atIndex{0};      // 插入位置；等於 pageCount 代表附加在最後
    int count{1};
    double widthPt{kA4WidthPt};
    double heightPt{kA4HeightPt};
};

struct DeletePages {
    std::vector<int> pages;
};

struct RotatePages {
    std::vector<int> pages;
    PageRotation rotation{PageRotation::Clockwise90};
    // 相對旋轉是使用者按「向右轉」時的意思；絕對旋轉是屬性面板直接指定角度。
    bool relative{true};
};

struct MovePages {
    std::vector<int> pages;    // 依此順序落在目標位置
    int destinationIndex{0};   // 搬移後這批頁的起始索引
};

struct DuplicatePages {
    std::vector<int> pages;
    int destinationIndex{0};   // 複本插入位置（以操作前的索引計）
};

struct SwapPages {
    int first{0};
    int second{0};
};

// 反轉整份文件的頁序（PRD-PAGE-012）。
struct ReversePages {};

using PageOperation = std::variant<InsertBlankPages, DeletePages, RotatePages, MovePages,
                                   DuplicatePages, SwapPages, ReversePages>;

// ---------------------------------------------------------------------------
// 純模擬：不碰 PDF 也能算出「操作後的頁序長什麼樣」
// ---------------------------------------------------------------------------

// 一個頁面槽位。sourceIndex 是它在**原始文件**中的索引，新插入的空白頁為 kNewPage。
// 有了它，「刪第 2 頁再把第 5 頁移到最前面」的結果就能在沒有 PDFium 的情況下被驗證，
// 而測試也就能真的檢查順序，而不是只檢查頁數對不對。
inline constexpr int kNewPage = -1;

struct PageSlot {
    int sourceIndex{kNewPage};
    PageRotation rotation{PageRotation::None};

    friend constexpr bool operator==(const PageSlot&, const PageSlot&) = default;
};

using PageOrder = std::vector<PageSlot>;

[[nodiscard]] inline PageOrder identityOrder(int pageCount) {
    PageOrder order;
    for (int i = 0; i < pageCount; ++i) order.push_back(PageSlot{i, PageRotation::None});
    return order;
}

struct SimulationResult {
    OperationStatus status{OperationStatus::Ok};
    PageOrder order;

    [[nodiscard]] bool ok() const noexcept { return status == OperationStatus::Ok; }
};

namespace detail {

inline OperationStatus checkSelection(const std::vector<int>& pages, int pageCount,
                                      bool allowDuplicates) {
    if (pages.empty()) return OperationStatus::EmptySelection;
    std::vector<bool> seen(static_cast<std::size_t>(std::max(pageCount, 0)), false);
    for (const int page : pages) {
        if (page < 0 || page >= pageCount) return OperationStatus::IndexOutOfRange;
        if (!allowDuplicates) {
            if (seen[static_cast<std::size_t>(page)]) return OperationStatus::DuplicateSelection;
            seen[static_cast<std::size_t>(page)] = true;
        }
    }
    return OperationStatus::Ok;
}

}  // namespace detail

[[nodiscard]] inline SimulationResult applyOperation(const PageOrder& input,
                                                     const PageOperation& operation) {
    SimulationResult result;
    const int pageCount = static_cast<int>(input.size());

    if (const auto* op = std::get_if<InsertBlankPages>(&operation)) {
        if (op->count <= 0) {
            result.status = OperationStatus::InvalidCount;
            return result;
        }
        if (op->widthPt <= 0.0 || op->heightPt <= 0.0) {
            result.status = OperationStatus::InvalidGeometry;
            return result;
        }
        if (op->atIndex < 0 || op->atIndex > pageCount) {
            result.status = OperationStatus::InvalidDestination;
            return result;
        }
        result.order = input;
        result.order.insert(result.order.begin() + op->atIndex,
                            static_cast<std::size_t>(op->count), PageSlot{});
        return result;
    }

    if (const auto* op = std::get_if<DeletePages>(&operation)) {
        result.status = detail::checkSelection(op->pages, pageCount, false);
        if (result.status != OperationStatus::Ok) return result;
        if (static_cast<int>(op->pages.size()) >= pageCount) {
            result.status = OperationStatus::WouldEmptyDocument;
            return result;
        }
        std::vector<bool> remove(input.size(), false);
        for (const int page : op->pages) remove[static_cast<std::size_t>(page)] = true;
        for (std::size_t i = 0; i < input.size(); ++i) {
            if (!remove[i]) result.order.push_back(input[i]);
        }
        return result;
    }

    if (const auto* op = std::get_if<RotatePages>(&operation)) {
        result.status = detail::checkSelection(op->pages, pageCount, false);
        if (result.status != OperationStatus::Ok) return result;
        result.order = input;
        for (const int page : op->pages) {
            PageSlot& slot = result.order[static_cast<std::size_t>(page)];
            slot.rotation = op->relative ? combine(slot.rotation, op->rotation) : op->rotation;
        }
        return result;
    }

    if (const auto* op = std::get_if<MovePages>(&operation)) {
        result.status = detail::checkSelection(op->pages, pageCount, false);
        if (result.status != OperationStatus::Ok) return result;
        // 目標位置的上限是「剩下的頁數」而不是「總頁數」：被搬的頁自己不佔位置。
        // 這與 PDFium FPDF_MovePages 的約束一致，先在這裡擋掉可以避免它把文件
        // 留在不確定狀態（見其標頭註記）。
        if (op->destinationIndex < 0 ||
            op->destinationIndex + static_cast<int>(op->pages.size()) > pageCount) {
            result.status = OperationStatus::InvalidDestination;
            return result;
        }
        std::vector<bool> moved(input.size(), false);
        for (const int page : op->pages) moved[static_cast<std::size_t>(page)] = true;

        PageOrder rest;
        for (std::size_t i = 0; i < input.size(); ++i) {
            if (!moved[i]) rest.push_back(input[i]);
        }
        PageOrder block;
        for (const int page : op->pages) block.push_back(input[static_cast<std::size_t>(page)]);

        result.order = rest;
        result.order.insert(result.order.begin() + op->destinationIndex, block.begin(), block.end());
        return result;
    }

    if (const auto* op = std::get_if<DuplicatePages>(&operation)) {
        // 複製允許同一頁被選兩次（做兩份複本），這是它與刪除／搬移的差別。
        result.status = detail::checkSelection(op->pages, pageCount, true);
        if (result.status != OperationStatus::Ok) return result;
        if (op->destinationIndex < 0 || op->destinationIndex > pageCount) {
            result.status = OperationStatus::InvalidDestination;
            return result;
        }
        PageOrder copies;
        for (const int page : op->pages) copies.push_back(input[static_cast<std::size_t>(page)]);
        result.order = input;
        result.order.insert(result.order.begin() + op->destinationIndex, copies.begin(),
                            copies.end());
        return result;
    }

    if (const auto* op = std::get_if<SwapPages>(&operation)) {
        if (op->first < 0 || op->first >= pageCount || op->second < 0 || op->second >= pageCount) {
            result.status = OperationStatus::IndexOutOfRange;
            return result;
        }
        result.order = input;
        std::swap(result.order[static_cast<std::size_t>(op->first)],
                  result.order[static_cast<std::size_t>(op->second)]);
        return result;
    }

    if (std::get_if<ReversePages>(&operation) != nullptr) {
        if (pageCount == 0) {
            result.status = OperationStatus::EmptySelection;
            return result;
        }
        result.order.assign(input.rbegin(), input.rend());
        return result;
    }

    result.status = OperationStatus::InvalidCount;
    return result;
}

[[nodiscard]] inline OperationStatus validate(const PageOperation& operation, int pageCount) {
    return applyOperation(identityOrder(pageCount), operation).status;
}

// ---------------------------------------------------------------------------
// 分割規則
// ---------------------------------------------------------------------------

enum class SplitMode {
    EveryNPages,    // 每 N 頁一檔
    MaxBytes,       // 累積到接近上限就切（需要每頁的位元組估計值）
    AtPageNumbers,  // 在指定頁「之前」切開
};

struct SplitRule {
    SplitMode mode{SplitMode::EveryNPages};
    int pagesPerChunk{1};
    std::uint64_t maxBytes{0};
    std::vector<int> boundaries;  // 0 起算；在這些索引之前切開，0 沒有意義會被忽略
};

struct SplitChunk {
    int first{0};
    int last{0};  // 含端點

    [[nodiscard]] constexpr int count() const noexcept { return last - first + 1; }

    friend constexpr bool operator==(const SplitChunk&, const SplitChunk&) = default;
};

enum class SplitStatus {
    Ok,
    InvalidPageCount,
    InvalidChunkSize,
    InvalidBudget,
    MissingPageSizes,   // MaxBytes 模式但沒有給每頁大小；用猜的會切出離譜的檔案
    InvalidBoundary,
};

struct SplitPlan {
    SplitStatus status{SplitStatus::Ok};
    std::vector<SplitChunk> chunks;
    // 單一頁本身就超過大小上限時，它會獨自成為一檔並超標。呼叫端必須看得見這件事，
    // 因為那代表「每檔 ≤ N MB」的承諾在這份文件上做不到。
    bool hasOversizedChunk{false};

    [[nodiscard]] bool ok() const noexcept { return status == SplitStatus::Ok; }
};

// pageBytes 只有 MaxBytes 模式需要，長度必須等於 pageCount。
[[nodiscard]] inline SplitPlan planSplit(const SplitRule& rule, int pageCount,
                                         const std::vector<std::uint64_t>& pageBytes = {}) {
    SplitPlan plan;
    if (pageCount <= 0) {
        plan.status = SplitStatus::InvalidPageCount;
        return plan;
    }

    switch (rule.mode) {
        case SplitMode::EveryNPages: {
            if (rule.pagesPerChunk <= 0) {
                plan.status = SplitStatus::InvalidChunkSize;
                return plan;
            }
            for (int first = 0; first < pageCount; first += rule.pagesPerChunk) {
                plan.chunks.push_back(
                    SplitChunk{first, std::min(first + rule.pagesPerChunk, pageCount) - 1});
            }
            return plan;
        }
        case SplitMode::MaxBytes: {
            if (rule.maxBytes == 0) {
                plan.status = SplitStatus::InvalidBudget;
                return plan;
            }
            if (pageBytes.size() != static_cast<std::size_t>(pageCount)) {
                plan.status = SplitStatus::MissingPageSizes;
                return plan;
            }
            int first = 0;
            std::uint64_t used = 0;
            for (int i = 0; i < pageCount; ++i) {
                const std::uint64_t size = pageBytes[static_cast<std::size_t>(i)];
                if (used > 0 && used + size > rule.maxBytes) {
                    plan.chunks.push_back(SplitChunk{first, i - 1});
                    first = i;
                    used = 0;
                }
                used += size;
                if (size > rule.maxBytes) plan.hasOversizedChunk = true;
            }
            plan.chunks.push_back(SplitChunk{first, pageCount - 1});
            return plan;
        }
        case SplitMode::AtPageNumbers: {
            std::vector<int> cuts;
            for (const int boundary : rule.boundaries) {
                if (boundary < 0 || boundary >= pageCount) {
                    plan.status = SplitStatus::InvalidBoundary;
                    return plan;
                }
                if (boundary == 0) continue;  // 在第一頁之前切等於沒切
                cuts.push_back(boundary);
            }
            std::sort(cuts.begin(), cuts.end());
            cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

            int first = 0;
            for (const int cut : cuts) {
                plan.chunks.push_back(SplitChunk{first, cut - 1});
                first = cut;
            }
            plan.chunks.push_back(SplitChunk{first, pageCount - 1});
            return plan;
        }
    }

    plan.status = SplitStatus::InvalidPageCount;
    return plan;
}

}  // namespace alioth::domain::pages
