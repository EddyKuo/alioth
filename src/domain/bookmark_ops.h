#pragma once

// 書籤的意圖模型與批次操作的純邏輯（PRD-BM-002 ~ 019，WBS 9）。
//
// 為什麼這一層存在：18 條批次功能裡真正難的部分幾乎都不是 PDF 語法，而是
// 「使用者說的那件事到底是什麼意思」——每 N 頁的 N 從第幾頁起算、標題的
// 大小寫轉換遇到縮寫怎麼辦、目錄頁的點狀引導線在哪裡斷、重複書籤要以什麼
// 為相同的判準。那些全部可以在不開啟任何 PDF 的情況下定義與驗證，因此放在
// 領域層，讓引擎層只剩下「把已經算好的樹寫成 PDF 物件」這一件事。
//
// header-only 是刻意的：這一層沒有任何跨翻譯單元的狀態，而且不進
// src/domain/CMakeLists.txt 就不會拖慢其他工作包的建置。
//
// 頁碼慣例與 domain::pages 一致：本檔所有 API 的 pageIndex 一律 0 起算，
// 只有「使用者看到的字串」（目錄頁文字、匯出檔、標題樣板）才是 1 起算，
// 轉換只發生在明確標示的那幾個函式裡。
//
// 大小寫與字寬：本檔刻意只處理 ASCII。正確的 Unicode 大小寫折疊需要 ICU，
// 而專案的引擎級相依上限是三個元件（PRD §4.1），不能為了書籤標題再加一個。
// 非 ASCII 位元組一律原樣保留，這比用 std::toupper 逐位元組處理 UTF-8 好——
// 後者會把中文字的續接位元組改掉，產出亂碼而不是「沒有轉換」。

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "document.h"
#include "geometry.h"

namespace alioth::domain::bookmarks {

// ---------------------------------------------------------------------------
// 目標與縮放類型（PRD-BM-001 的「縮放類型設定」）
// ---------------------------------------------------------------------------

// ISO 32000-2 §12.3.2.2 的八種顯式目標語法。
//
// 這裡不提供「預設值」的縮放類型：/XYZ 與 /Fit 在 Acrobat 的行為差很多
// （前者保留使用者當下的倍率，後者強制縮到整頁），選錯的症狀是使用者每點
// 一個書籤畫面倍率就被重設，而那看起來像檢視器的 bug 不像書籤的設定錯誤。
enum class ZoomType {
    XYZ,    // [page /XYZ left top zoom]，任一項可為 null（沿用目前值）
    Fit,    // [page /Fit]
    FitH,   // [page /FitH top]
    FitV,   // [page /FitV left]
    FitR,   // [page /FitR left bottom right top]
    FitB,   // [page /FitB]
    FitBH,  // [page /FitBH top]
    FitBV,  // [page /FitBV left]
};

[[nodiscard]] inline const char* zoomTypeName(ZoomType type) noexcept {
    switch (type) {
        case ZoomType::XYZ: return "XYZ";
        case ZoomType::Fit: return "Fit";
        case ZoomType::FitH: return "FitH";
        case ZoomType::FitV: return "FitV";
        case ZoomType::FitR: return "FitR";
        case ZoomType::FitB: return "FitB";
        case ZoomType::FitBH: return "FitBH";
        case ZoomType::FitBV: return "FitBV";
    }
    return "Fit";
}

[[nodiscard]] inline bool zoomTypeFromName(std::string_view name, ZoomType& out) noexcept {
    if (name == "XYZ") { out = ZoomType::XYZ; return true; }
    if (name == "Fit") { out = ZoomType::Fit; return true; }
    if (name == "FitH") { out = ZoomType::FitH; return true; }
    if (name == "FitV") { out = ZoomType::FitV; return true; }
    if (name == "FitR") { out = ZoomType::FitR; return true; }
    if (name == "FitB") { out = ZoomType::FitB; return true; }
    if (name == "FitBH") { out = ZoomType::FitBH; return true; }
    if (name == "FitBV") { out = ZoomType::FitBV; return true; }
    return false;
}

// 目標的座標參數。全部是 optional，因為 PDF 允許寫 null 表示「沿用目前值」，
// 而 0 與 null 在 /XYZ 裡是兩件不同的事：前者把畫面捲到頁面左下角。
struct Destination {
    std::int32_t pageIndex{0};
    ZoomType zoom{ZoomType::Fit};
    std::optional<double> left;
    std::optional<double> top;
    std::optional<double> right;
    std::optional<double> bottom;
    std::optional<double> zoomFactor;  // 只有 ZoomType::XYZ 用得到

    [[nodiscard]] static Destination fitPage(std::int32_t page) noexcept {
        Destination d;
        d.pageIndex = page;
        d.zoom = ZoomType::Fit;
        return d;
    }

    // 捲到頁面上緣、倍率不變。這是「跳到某一頁的某個位置」最常見的形式，
    // 也是由文字或高亮產生書籤時唯一合理的預設。
    [[nodiscard]] static Destination atTop(std::int32_t page, double topPt) noexcept {
        Destination d;
        d.pageIndex = page;
        d.zoom = ZoomType::XYZ;
        d.left = 0.0;
        d.top = topPt;
        return d;
    }
};

// 目標的兩種形態。PDF 允許書籤用 /Dest（直接寫目標陣列或命名目標）或
// /A（GoTo action，字典裡再放 /D），兩者語意相同但相容性不同：
// 部分舊工具只看 /Dest，另一些只看 /A。寫入端必須讓呼叫端能明確選擇，
// 讀取端則兩種都要認得。
enum class TargetEncoding {
    Dest,
    GoToAction,
};

enum class TargetKind {
    None,    // 只有標題、不跳轉（PRD-BM-017 移除動作後的狀態）
    Direct,  // 直接寫出目標陣列
    Named,   // 指向 /Dests 名稱樹的一個鍵（PRD-BM-006/007）
};

struct BookmarkTarget {
    TargetKind kind{TargetKind::None};
    TargetEncoding encoding{TargetEncoding::Dest};
    Destination destination{};
    std::string name;  // kind == Named 時有效

    [[nodiscard]] bool empty() const noexcept { return kind == TargetKind::None; }

    [[nodiscard]] static BookmarkTarget direct(Destination destination,
                                               TargetEncoding encoding = TargetEncoding::Dest) {
        BookmarkTarget target;
        target.kind = TargetKind::Direct;
        target.encoding = encoding;
        target.destination = destination;
        return target;
    }

    [[nodiscard]] static BookmarkTarget named(std::string name,
                                              TargetEncoding encoding = TargetEncoding::Dest) {
        BookmarkTarget target;
        target.kind = TargetKind::Named;
        target.encoding = encoding;
        target.name = std::move(name);
        return target;
    }
};

// 書籤節點。與 domain::OutlineNode 的差別是這個型別是**可寫入**的完整模型：
// OutlineNode 只保留檢視器畫面需要的欄位，缺了 /C /F /Count 與命名目標，
// 拿它去寫檔會靜默丟掉使用者設定的顏色與粗體。兩者刻意分開，避免讀取路徑
// 為了寫入需求被迫背上一堆它用不到的欄位。
struct Bookmark {
    std::string title;
    BookmarkTarget target{};

    bool bold{false};
    bool italic{false};
    bool hasColor{false};
    double colorR{0.0};
    double colorG{0.0};
    double colorB{0.0};

    // 對應 /Count 的正負號：展開時 /Count 為正、收合時為負。
    bool open{false};

    std::vector<Bookmark> children;
};

using BookmarkTree = std::vector<Bookmark>;

// 節點位置。用索引路徑而不是指標，因為批次操作會整批重建子樹，
// 指標在第一次改動之後就全部失效，而失效的指標不會當場出錯。
using BookmarkPath = std::vector<std::size_t>;

// ---------------------------------------------------------------------------
// 走訪與基本查詢
// ---------------------------------------------------------------------------

// 深度上限。書籤樹來自不可信任的輸入（SDD §7），遞迴走訪必須設限；
// PRD-NAV-003 只要求支援 8 層以上，這裡取 64 是為了讓合法的深巢狀不會被誤擋。
inline constexpr int kMaxDepth = 64;

namespace detail {

template <typename Node, typename Fn>
void visitImpl(std::vector<Node>& nodes, BookmarkPath& path, int depth, const Fn& fn) {
    if (depth >= kMaxDepth) return;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        path.push_back(i);
        fn(nodes[i], static_cast<const BookmarkPath&>(path), depth);
        visitImpl(nodes[i].children, path, depth + 1, fn);
        path.pop_back();
    }
}

}  // namespace detail

// 前序走訪。回呼收到 (節點, 路徑, 深度)，深度由 0 起算。
template <typename Fn>
void visit(BookmarkTree& tree, const Fn& fn) {
    BookmarkPath path;
    detail::visitImpl(tree, path, 0, fn);
}

template <typename Fn>
void visit(const BookmarkTree& tree, const Fn& fn) {
    BookmarkPath path;
    detail::visitImpl(const_cast<BookmarkTree&>(tree), path, 0,
                      [&fn](Bookmark& node, const BookmarkPath& p, int depth) {
                          fn(static_cast<const Bookmark&>(node), p, depth);
                      });
}

[[nodiscard]] inline std::size_t nodeCount(const BookmarkTree& tree) {
    std::size_t count = 0;
    visit(tree, [&count](const Bookmark&, const BookmarkPath&, int) { ++count; });
    return count;
}

// 最大深度，以「層數」計：空樹是 0、只有根節點是 1。
[[nodiscard]] inline int treeDepth(const BookmarkTree& tree) {
    int deepest = 0;
    visit(tree, [&deepest](const Bookmark&, const BookmarkPath&, int depth) {
        deepest = std::max(deepest, depth + 1);
    });
    return deepest;
}

[[nodiscard]] inline Bookmark* nodeAt(BookmarkTree& tree, const BookmarkPath& path) {
    if (path.empty()) return nullptr;
    BookmarkTree* level = &tree;
    Bookmark* node = nullptr;
    for (const std::size_t index : path) {
        if (index >= level->size()) return nullptr;
        node = &(*level)[index];
        level = &node->children;
    }
    return node;
}

[[nodiscard]] inline const Bookmark* nodeAt(const BookmarkTree& tree, const BookmarkPath& path) {
    return nodeAt(const_cast<BookmarkTree&>(tree), path);
}

// 父層容器。根層的父容器就是 tree 本身，因此空路徑合法。
[[nodiscard]] inline BookmarkTree* containerAt(BookmarkTree& tree, const BookmarkPath& parentPath) {
    BookmarkTree* level = &tree;
    for (const std::size_t index : parentPath) {
        if (index >= level->size()) return nullptr;
        level = &(*level)[index].children;
    }
    return level;
}

// a 是否為 b 的祖先或就是 b。移動節點時用來擋掉「把父節點搬進自己的子孫」，
// 那個操作會讓被搬走的子樹整段從樹上消失，而且不會有任何錯誤訊息。
[[nodiscard]] inline bool isPrefixOf(const BookmarkPath& a, const BookmarkPath& b) noexcept {
    if (a.size() > b.size()) return false;
    return std::equal(a.begin(), a.end(), b.begin());
}

// ---------------------------------------------------------------------------
// 樹的編輯（PRD-BM-001：建立、刪除、重新命名、移動）
// ---------------------------------------------------------------------------

// index 等於容器大小代表附加到尾端；超過則失敗而不是夾住，
// 因為「使用者以為插在第 5 個、實際插在第 3 個」是無聲的錯誤。
inline bool insertNode(BookmarkTree& tree, const BookmarkPath& parentPath, std::size_t index,
                       Bookmark node) {
    if (parentPath.size() >= static_cast<std::size_t>(kMaxDepth)) return false;
    BookmarkTree* container = containerAt(tree, parentPath);
    if (container == nullptr || index > container->size()) return false;
    container->insert(container->begin() + static_cast<std::ptrdiff_t>(index), std::move(node));
    return true;
}

inline bool appendNode(BookmarkTree& tree, const BookmarkPath& parentPath, Bookmark node) {
    BookmarkTree* container = containerAt(tree, parentPath);
    if (container == nullptr) return false;
    return insertNode(tree, parentPath, container->size(), std::move(node));
}

// 刪除整個子樹。removed 非空時把被刪的節點交出去，讓復原不需要另外快照。
inline bool removeNode(BookmarkTree& tree, const BookmarkPath& path, Bookmark* removed = nullptr) {
    if (path.empty()) return false;
    BookmarkPath parentPath(path.begin(), path.end() - 1);
    BookmarkTree* container = containerAt(tree, parentPath);
    if (container == nullptr) return false;
    const std::size_t index = path.back();
    if (index >= container->size()) return false;
    if (removed != nullptr) *removed = std::move((*container)[index]);
    container->erase(container->begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

inline bool renameNode(BookmarkTree& tree, const BookmarkPath& path, std::string title) {
    Bookmark* node = nodeAt(tree, path);
    if (node == nullptr) return false;
    node->title = std::move(title);
    return true;
}

inline bool setTarget(BookmarkTree& tree, const BookmarkPath& path, BookmarkTarget target) {
    Bookmark* node = nodeAt(tree, path);
    if (node == nullptr) return false;
    node->target = std::move(target);
    return true;
}

// 移動節點（改變層級與順序）。
//
// 路徑一律以「操作前的樹」為準：呼叫端看到的是移動前的畫面，要求它先自行
// 換算移除後的索引，等於把最容易錯的一步推給每一個呼叫端。移除造成的索引
// 位移在這裡統一修正——同層往後搬時目標索引要減一，這正是拖放排序最常見的
// 差一錯誤。
inline bool moveNode(BookmarkTree& tree, const BookmarkPath& from, const BookmarkPath& toParent,
                     std::size_t toIndex) {
    if (from.empty()) return false;
    if (isPrefixOf(from, toParent)) return false;  // 搬進自己的子孫
    if (nodeAt(tree, from) == nullptr) return false;
    if (containerAt(tree, toParent) == nullptr) return false;

    const BookmarkPath fromParent(from.begin(), from.end() - 1);
    const std::size_t fromIndex = from.back();

    // 目標父層在來源之後的同層兄弟底下時，移除會讓它的路徑往前挪一格。
    BookmarkPath adjustedParent = toParent;
    if (adjustedParent.size() > fromParent.size() &&
        std::equal(fromParent.begin(), fromParent.end(), adjustedParent.begin()) &&
        adjustedParent[fromParent.size()] > fromIndex) {
        --adjustedParent[fromParent.size()];
    }

    std::size_t adjustedIndex = toIndex;
    if (adjustedParent == fromParent && toIndex > fromIndex) --adjustedIndex;

    Bookmark moved;
    if (!removeNode(tree, from, &moved)) return false;

    BookmarkTree* container = containerAt(tree, adjustedParent);
    if (container == nullptr || adjustedIndex > container->size()) {
        // 還原：失敗的移動不能吃掉節點。
        BookmarkTree* origin = containerAt(tree, fromParent);
        if (origin != nullptr) {
            const std::size_t back = std::min(fromIndex, origin->size());
            origin->insert(origin->begin() + static_cast<std::ptrdiff_t>(back), std::move(moved));
        }
        return false;
    }
    container->insert(container->begin() + static_cast<std::ptrdiff_t>(adjustedIndex),
                      std::move(moved));
    return true;
}

// 同層上下移動，拖放與「上移／下移」按鈕共用。
inline bool moveSibling(BookmarkTree& tree, const BookmarkPath& path, int delta) {
    if (path.empty() || delta == 0) return false;
    const BookmarkPath parentPath(path.begin(), path.end() - 1);
    BookmarkTree* container = containerAt(tree, parentPath);
    if (container == nullptr) return false;
    const std::ptrdiff_t index = static_cast<std::ptrdiff_t>(path.back());
    const std::ptrdiff_t target = index + delta;
    if (index < 0 || index >= static_cast<std::ptrdiff_t>(container->size())) return false;
    if (target < 0 || target >= static_cast<std::ptrdiff_t>(container->size())) return false;
    std::swap((*container)[static_cast<std::size_t>(index)],
              (*container)[static_cast<std::size_t>(target)]);
    return true;
}

// 降級：變成前一個兄弟的最後一個子節點。升級：變成父節點的下一個兄弟。
inline bool demoteNode(BookmarkTree& tree, const BookmarkPath& path) {
    if (path.empty() || path.back() == 0) return false;
    BookmarkPath newParent = path;
    newParent.back() -= 1;
    const Bookmark* target = nodeAt(tree, newParent);
    if (target == nullptr) return false;
    return moveNode(tree, path, newParent, target->children.size());
}

inline bool promoteNode(BookmarkTree& tree, const BookmarkPath& path) {
    if (path.size() < 2) return false;
    BookmarkPath grandParent(path.begin(), path.end() - 2);
    const std::size_t parentIndex = path[path.size() - 2];
    return moveNode(tree, path, grandParent, parentIndex + 1);
}

// ---------------------------------------------------------------------------
// 字串工具（ASCII 限定，理由見檔頭）
// ---------------------------------------------------------------------------

namespace detail {

[[nodiscard]] inline bool isAsciiAlpha(unsigned char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

[[nodiscard]] inline char asciiUpper(char c) noexcept {
    const unsigned char u = static_cast<unsigned char>(c);
    return (u >= 'a' && u <= 'z') ? static_cast<char>(u - 32) : c;
}

[[nodiscard]] inline char asciiLower(char c) noexcept {
    const unsigned char u = static_cast<unsigned char>(c);
    return (u >= 'A' && u <= 'Z') ? static_cast<char>(u + 32) : c;
}

[[nodiscard]] inline std::string trimmed(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    const auto isSpace = [](char c) {
        const unsigned char u = static_cast<unsigned char>(c);
        return u == ' ' || u == '\t' || u == '\r' || u == '\n' || u == '\f' || u == '\v';
    };
    while (begin < end && isSpace(text[begin])) ++begin;
    while (end > begin && isSpace(text[end - 1])) --end;
    return std::string(text.substr(begin, end - begin));
}

[[nodiscard]] inline std::vector<std::string> splitLines(std::string_view text) {
    std::vector<std::string> lines;
    std::string current;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else if (c == '\r') {
            // CRLF 與單獨的 CR 都算換行；不處理的話 Windows 產生的文字檔
            // 每個標題結尾都會多一個看不見的字元，寫進 PDF 之後才會發現。
            lines.push_back(current);
            current.clear();
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else {
            current.push_back(c);
        }
    }
    lines.push_back(current);
    return lines;
}

[[nodiscard]] inline bool equalsIgnoreAsciiCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (asciiLower(a[i]) != asciiLower(b[i])) return false;
    }
    return true;
}

[[nodiscard]] inline std::string replaceAll(std::string_view subject, std::string_view token,
                                            std::string_view value) {
    if (token.empty()) return std::string(subject);
    std::string out;
    std::size_t position = 0;
    while (true) {
        const std::size_t hit = subject.find(token, position);
        if (hit == std::string_view::npos) break;
        out.append(subject.substr(position, hit - position));
        out.append(value);
        position = hit + token.size();
    }
    out.append(subject.substr(position));
    return out;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// PRD-BM-002：標題加文字（前綴／後綴）
// ---------------------------------------------------------------------------

struct AffixOptions {
    std::string prefix;
    std::string suffix;
    std::optional<int> level;     // 只改指定深度（0 起算）；未設定代表全部
    bool skipEmptyTitles{true};   // 空標題加了前綴會變成看起來有內容的空節點
    std::string containing;       // 非空時只改標題含這段文字的節點
    bool caseSensitive{true};
};

// 回傳實際改動的節點數。回傳 0 代表條件沒有命中任何節點，
// 呼叫端應該把它當成「什麼都沒發生」提示使用者，而不是當成成功。
inline std::size_t applyAffix(BookmarkTree& tree, const AffixOptions& options) {
    if (options.prefix.empty() && options.suffix.empty()) return 0;
    std::size_t changed = 0;
    visit(tree, [&](Bookmark& node, const BookmarkPath&, int depth) {
        if (options.level.has_value() && *options.level != depth) return;
        if (options.skipEmptyTitles && detail::trimmed(node.title).empty()) return;
        if (!options.containing.empty()) {
            const bool hit = options.caseSensitive
                                 ? node.title.find(options.containing) != std::string::npos
                                 : [&] {
                                       std::string haystack;
                                       haystack.reserve(node.title.size());
                                       for (const char c : node.title) {
                                           haystack.push_back(detail::asciiLower(c));
                                       }
                                       std::string needle;
                                       needle.reserve(options.containing.size());
                                       for (const char c : options.containing) {
                                           needle.push_back(detail::asciiLower(c));
                                       }
                                       return haystack.find(needle) != std::string::npos;
                                   }();
            if (!hit) return;
        }
        node.title = options.prefix + node.title + options.suffix;
        ++changed;
    });
    return changed;
}

// ---------------------------------------------------------------------------
// PRD-BM-003：每 N 頁自動加書籤
// ---------------------------------------------------------------------------

struct EveryNPagesOptions {
    std::int32_t pageCount{0};
    std::int32_t interval{1};     // N
    std::int32_t firstPage{0};    // 從哪一頁開始（0 起算）
    std::int32_t pageLabelOffset{1};  // 標題裡顯示的頁碼 = pageIndex + 這個值
    std::string titlePattern{"Page {page}"};  // {page} 顯示頁碼、{index} 第幾個書籤
    ZoomType zoom{ZoomType::Fit};
    TargetEncoding encoding{TargetEncoding::Dest};
};

// 產生一層平坦的書籤。刻意不接受 interval <= 0：那會產生無限多個書籤，
// 而使用者輸入 0 的意思幾乎一定是「還沒填」而不是「每 0 頁一個」。
[[nodiscard]] inline BookmarkTree generateEveryNPages(const EveryNPagesOptions& options) {
    BookmarkTree tree;
    if (options.pageCount <= 0 || options.interval <= 0) return tree;
    std::int32_t ordinal = 1;
    for (std::int32_t page = std::max<std::int32_t>(options.firstPage, 0); page < options.pageCount;
         page += options.interval) {
        Bookmark node;
        std::string title = detail::replaceAll(options.titlePattern, "{page}",
                                               std::to_string(page + options.pageLabelOffset));
        title = detail::replaceAll(title, "{index}", std::to_string(ordinal));
        node.title = std::move(title);
        Destination destination;
        destination.pageIndex = page;
        destination.zoom = options.zoom;
        if (options.zoom == ZoomType::XYZ) {
            destination.left = 0.0;
            // top 留空代表「沿用目前的垂直位置」，那不是使用者要的；
            // 但這裡不知道頁高，因此交給寫入端在有頁面尺寸時補上。
        }
        node.target = BookmarkTarget::direct(destination, options.encoding);
        tree.push_back(std::move(node));
        ++ordinal;
    }
    return tree;
}

// ---------------------------------------------------------------------------
// PRD-BM-004：大小寫轉換
// ---------------------------------------------------------------------------

enum class CaseMode {
    Upper,
    Lower,
    TitleCase,     // 每個字的首字母大寫
    SentenceCase,  // 只有第一個字母大寫
};

[[nodiscard]] inline std::string convertCase(std::string_view text, CaseMode mode) {
    std::string out;
    out.reserve(text.size());
    bool atWordStart = true;
    bool seenLetter = false;
    for (const char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        switch (mode) {
            case CaseMode::Upper:
                out.push_back(detail::asciiUpper(c));
                break;
            case CaseMode::Lower:
                out.push_back(detail::asciiLower(c));
                break;
            case CaseMode::TitleCase:
                out.push_back(atWordStart ? detail::asciiUpper(c) : detail::asciiLower(c));
                break;
            case CaseMode::SentenceCase:
                out.push_back((!seenLetter && detail::isAsciiAlpha(u)) ? detail::asciiUpper(c)
                                                                       : detail::asciiLower(c));
                break;
        }
        if (detail::isAsciiAlpha(u)) seenLetter = true;
        // 撇號後面不算新字：don't 不該變成 Don'T。
        atWordStart = !(detail::isAsciiAlpha(u) || u == '\'' || (u >= '0' && u <= '9') || u >= 0x80);
    }
    return out;
}

inline std::size_t applyCase(BookmarkTree& tree, CaseMode mode) {
    std::size_t changed = 0;
    visit(tree, [&](Bookmark& node, const BookmarkPath&, int) {
        std::string converted = convertCase(node.title, mode);
        if (converted != node.title) ++changed;
        node.title = std::move(converted);
    });
    return changed;
}

// ---------------------------------------------------------------------------
// PRD-BM-011：標題的尋找取代
// ---------------------------------------------------------------------------

struct FindReplaceOptions {
    std::string find;
    std::string replace;
    bool caseSensitive{true};
    bool wholeWord{false};
    bool firstOccurrenceOnly{false};  // 每個標題只換第一處
};

// 回傳被改動的**節點數**而不是取代次數：使用者要知道的是「哪些書籤變了」，
// 而取代次數在同一標題出現多次時會給出誤導性的大數字。
inline std::size_t findReplaceTitles(BookmarkTree& tree, const FindReplaceOptions& options) {
    if (options.find.empty()) return 0;
    const auto isWordChar = [](char c) {
        const unsigned char u = static_cast<unsigned char>(c);
        return detail::isAsciiAlpha(u) || (u >= '0' && u <= '9') || u == '_';
    };

    std::size_t changed = 0;
    visit(tree, [&](Bookmark& node, const BookmarkPath&, int) {
        const std::string& source = node.title;
        std::string out;
        std::size_t position = 0;
        bool hit = false;
        while (position <= source.size()) {
            std::size_t found = std::string::npos;
            for (std::size_t i = position; i + options.find.size() <= source.size(); ++i) {
                const std::string_view window(source.data() + i, options.find.size());
                const bool same = options.caseSensitive
                                      ? window == std::string_view(options.find)
                                      : detail::equalsIgnoreAsciiCase(window, options.find);
                if (!same) continue;
                if (options.wholeWord) {
                    const bool leftOk = i == 0 || !isWordChar(source[i - 1]);
                    const std::size_t after = i + options.find.size();
                    const bool rightOk = after >= source.size() || !isWordChar(source[after]);
                    if (!leftOk || !rightOk) continue;
                }
                found = i;
                break;
            }
            if (found == std::string::npos) break;
            out.append(source, position, found - position);
            out.append(options.replace);
            position = found + options.find.size();
            hit = true;
            if (options.firstOccurrenceOnly) break;
        }
        if (!hit) return;
        out.append(source, position, std::string::npos);
        node.title = std::move(out);
        ++changed;
    });
    return changed;
}

// ---------------------------------------------------------------------------
// PRD-BM-016：合併重複書籤
// ---------------------------------------------------------------------------

struct MergeDuplicatesOptions {
    bool caseSensitive{false};
    bool requireSameTarget{true};  // 標題相同但指到不同頁時不合併
    bool mergeChildren{true};      // 合併時把子節點接到留下來的那一個底下
};

namespace detail {

[[nodiscard]] inline bool sameTarget(const BookmarkTarget& a, const BookmarkTarget& b) noexcept {
    if (a.kind != b.kind) return false;
    if (a.kind == TargetKind::None) return true;
    if (a.kind == TargetKind::Named) return a.name == b.name;
    return a.destination.pageIndex == b.destination.pageIndex;
}

inline std::size_t mergeLevel(BookmarkTree& level, const MergeDuplicatesOptions& options,
                              int depth) {
    if (depth >= kMaxDepth) return 0;
    std::size_t removed = 0;
    for (std::size_t i = 0; i < level.size(); ++i) {
        for (std::size_t j = i + 1; j < level.size();) {
            const bool titleMatch =
                options.caseSensitive ? level[i].title == level[j].title
                                      : equalsIgnoreAsciiCase(level[i].title, level[j].title);
            const bool targetMatch =
                !options.requireSameTarget || sameTarget(level[i].target, level[j].target);
            if (!titleMatch || !targetMatch) {
                ++j;
                continue;
            }
            if (options.mergeChildren) {
                level[i].children.insert(level[i].children.end(),
                                         std::make_move_iterator(level[j].children.begin()),
                                         std::make_move_iterator(level[j].children.end()));
            }
            // 留下來的那一個若沒有目標，就接收被合併者的目標；
            // 否則「合併」會把唯一能跳轉的那一個刪掉。
            if (level[i].target.empty() && !level[j].target.empty()) {
                level[i].target = level[j].target;
            }
            level.erase(level.begin() + static_cast<std::ptrdiff_t>(j));
            ++removed;
        }
        removed += mergeLevel(level[i].children, options, depth + 1);
    }
    return removed;
}

}  // namespace detail

// 回傳被移除的節點數。
inline std::size_t mergeDuplicates(BookmarkTree& tree, const MergeDuplicatesOptions& options = {}) {
    return detail::mergeLevel(tree, options, 0);
}

// ---------------------------------------------------------------------------
// PRD-BM-017：移除動作
// ---------------------------------------------------------------------------

// 只清掉跳轉目標，標題與樹狀結構保留。使用場景是「把別人做壞的書籤目標
// 全部清掉再重建」，因此刻意不順手刪掉節點本身。
inline std::size_t removeActions(BookmarkTree& tree) {
    std::size_t changed = 0;
    visit(tree, [&changed](Bookmark& node, const BookmarkPath&, int) {
        if (node.target.empty()) return;
        node.target = BookmarkTarget{};
        ++changed;
    });
    return changed;
}

// ---------------------------------------------------------------------------
// PRD-BM-006 / 007：命名目標與書籤的雙向轉換
// ---------------------------------------------------------------------------

struct NamedDestination {
    std::string name;
    Destination destination;
};

// 名稱樹的鍵是字串不是名稱物件，理論上任何位元組都合法；但實務上含空白或
// 括號的鍵會讓不少工具（含部分 PDF 生成器）產出無法解析的檔案，因此一律
// 收斂成 ASCII 的識別字形態。
[[nodiscard]] inline std::string sanitizeDestinationName(std::string_view title,
                                                         std::string_view fallback = "dest") {
    std::string out;
    bool lastWasSeparator = false;
    for (const char c : title) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (detail::isAsciiAlpha(u) || (u >= '0' && u <= '9')) {
            out.push_back(c);
            lastWasSeparator = false;
        } else if (!lastWasSeparator && !out.empty()) {
            out.push_back('_');
            lastWasSeparator = true;
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.empty()) out = std::string(fallback);
    return out;
}

struct ToNamedOptions {
    std::string prefix{"BM_"};
    bool retargetBookmarks{true};  // 轉換後把書籤改成指向名稱
};

// 書籤 → 命名目標。同名時加上序號，因為名稱樹的鍵必須唯一，
// 重複的鍵會讓後面那個靜默覆蓋前面那個。
[[nodiscard]] inline std::vector<NamedDestination> bookmarksToNamedDestinations(
    BookmarkTree& tree, const ToNamedOptions& options = {}) {
    std::vector<NamedDestination> result;
    std::map<std::string, int> used;
    visit(tree, [&](Bookmark& node, const BookmarkPath&, int) {
        if (node.target.kind != TargetKind::Direct) return;
        std::string base = options.prefix + sanitizeDestinationName(node.title);
        std::string name = base;
        auto it = used.find(base);
        if (it != used.end()) {
            name = base + "_" + std::to_string(++it->second);
        } else {
            used.emplace(base, 1);
        }
        result.push_back(NamedDestination{name, node.target.destination});
        if (options.retargetBookmarks) {
            const TargetEncoding encoding = node.target.encoding;
            node.target = BookmarkTarget::named(name, encoding);
        }
    });
    return result;
}

// 命名目標 → 書籤（平坦一層）。名稱通常沒有可讀性，因此標題直接用名稱，
// 由使用者接著用尋找取代或大小寫轉換整理——那正是這一組功能要互相搭配的理由。
[[nodiscard]] inline BookmarkTree bookmarksFromNamedDestinations(
    const std::vector<NamedDestination>& destinations, bool sortByPage = true) {
    std::vector<NamedDestination> sorted = destinations;
    if (sortByPage) {
        std::stable_sort(sorted.begin(), sorted.end(),
                         [](const NamedDestination& a, const NamedDestination& b) {
                             return a.destination.pageIndex < b.destination.pageIndex;
                         });
    }
    BookmarkTree tree;
    tree.reserve(sorted.size());
    for (const NamedDestination& entry : sorted) {
        Bookmark node;
        node.title = entry.name;
        node.target = BookmarkTarget::named(entry.name);
        tree.push_back(std::move(node));
    }
    return tree;
}

// 命名目標 → 直接目標。回傳解析成功的節點數；找不到名稱的節點保持原樣，
// 讓 validate() 之後還能把它們列出來（靜默清掉會讓問題消失但檔案仍然壞的）。
inline std::size_t resolveNamedTargets(BookmarkTree& tree,
                                       const std::vector<NamedDestination>& destinations) {
    std::map<std::string, Destination> lookup;
    for (const NamedDestination& entry : destinations) lookup.emplace(entry.name, entry.destination);

    std::size_t resolved = 0;
    visit(tree, [&](Bookmark& node, const BookmarkPath&, int) {
        if (node.target.kind != TargetKind::Named) return;
        const auto it = lookup.find(node.target.name);
        if (it == lookup.end()) return;
        const TargetEncoding encoding = node.target.encoding;
        node.target = BookmarkTarget::direct(it->second, encoding);
        ++resolved;
    });
    return resolved;
}

// ---------------------------------------------------------------------------
// PRD-BM-005：由書籤建目錄頁的版面
// ---------------------------------------------------------------------------

// 字寬估計。目錄頁的引導點與頁碼要右對齊，得先知道標題有多寬。
//
// 這裡刻意用固定的平均字寬而不是真正的字型度量：目錄頁用的是標準 14 字型
// （Helvetica），把整份 AFM 度量表搬進領域層只為了排一頁目錄，成本遠高於
// 收益，而估算誤差的後果只是引導點多一個或少一個。
[[nodiscard]] inline double estimateTextWidthPt(std::string_view text, double fontSizePt) {
    double units = 0.0;
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char u = static_cast<unsigned char>(text[i]);
        if (u < 0x80) {
            // 窄字元（i l 標點）與寬字元（W M）差三倍，全部當成 0.5 em 的話
            // 全大寫標題會嚴重低估。
            if (u == 'i' || u == 'l' || u == 'j' || u == 't' || u == 'f' || u == 'I' || u == '.' ||
                u == ',' || u == ':' || u == ';' || u == '\'' || u == '|' || u == '(' || u == ')') {
                units += 0.28;
            } else if (u == 'W' || u == 'M' || u == 'm' || u == '@') {
                units += 0.88;
            } else if (u >= 'A' && u <= 'Z') {
                units += 0.68;
            } else {
                units += 0.52;
            }
            ++i;
        } else {
            // UTF-8 多位元組：CJK 一律當全形。跳過續接位元組，
            // 不跳的話一個中文字會被算成三個字寬。
            std::size_t length = 1;
            if ((u & 0xE0) == 0xC0) length = 2;
            else if ((u & 0xF0) == 0xE0) length = 3;
            else if ((u & 0xF8) == 0xF0) length = 4;
            units += (length >= 3) ? 1.0 : 0.52;
            i += length;
        }
    }
    return units * fontSizePt;
}

struct TocLayoutOptions {
    double pageWidthPt{595.276};   // A4
    double pageHeightPt{841.89};
    double marginPt{56.0};
    std::string heading{"Table of Contents"};
    double headingFontSizePt{18.0};
    double headingGapPt{28.0};
    double fontSizePt{11.0};
    double lineLeadingPt{16.0};
    double indentPerLevelPt{14.0};
    int maxDepth{8};
    bool showPageNumbers{true};
    std::int32_t pageLabelOffset{1};  // 顯示的頁碼 = pageIndex + 這個值
    char leader{'.'};                 // 引導點；設成 '\0' 代表不畫
};

// 目錄頁上的一行。rect 是可點擊區域，直接餵給 PRD-BM-008 產生連結。
struct TocLine {
    std::string text;
    std::string pageLabel;
    std::int32_t targetPageIndex{-1};  // -1 代表沒有目標（例如標題列）
    bool isHeading{false};
    int depth{0};
    double xPt{0.0};
    double baselineYPt{0.0};
    double fontSizePt{11.0};
    double leaderStartXPt{0.0};
    double leaderEndXPt{0.0};
    double pageLabelXPt{0.0};
    RectF rect{};
    std::optional<Destination> destination;
};

struct TocPage {
    std::vector<TocLine> lines;
};

struct TocLayout {
    std::vector<TocPage> pages;
    double pageWidthPt{0.0};
    double pageHeightPt{0.0};

    [[nodiscard]] std::size_t lineCount() const {
        std::size_t total = 0;
        for (const TocPage& page : pages) total += page.lines.size();
        return total;
    }
};

// 排出目錄頁。超過一頁會自動續頁——書籤數量沒有上限，硬塞在一頁上的結果是
// 文字直接畫到頁面外，而那在螢幕上看起來只是「後面的書籤不見了」。
[[nodiscard]] inline TocLayout layoutTableOfContents(const BookmarkTree& tree,
                                                     const TocLayoutOptions& options = {}) {
    TocLayout layout;
    layout.pageWidthPt = options.pageWidthPt;
    layout.pageHeightPt = options.pageHeightPt;

    const double leftPt = options.marginPt;
    const double rightPt = options.pageWidthPt - options.marginPt;
    const double topPt = options.pageHeightPt - options.marginPt;
    const double bottomPt = options.marginPt;
    if (rightPt <= leftPt || topPt <= bottomPt || options.lineLeadingPt <= 0.0) return layout;

    layout.pages.push_back(TocPage{});
    double cursorY = topPt;

    if (!options.heading.empty()) {
        TocLine line;
        line.text = options.heading;
        line.isHeading = true;
        line.fontSizePt = options.headingFontSizePt;
        line.xPt = leftPt;
        cursorY -= options.headingFontSizePt;
        line.baselineYPt = cursorY;
        line.rect = RectF{leftPt, cursorY - options.headingFontSizePt * 0.25, rightPt,
                          cursorY + options.headingFontSizePt};
        layout.pages.back().lines.push_back(std::move(line));
        cursorY -= options.headingGapPt;
    }

    visit(tree, [&](const Bookmark& node, const BookmarkPath&, int depth) {
        if (depth >= options.maxDepth) return;

        cursorY -= options.lineLeadingPt;
        if (cursorY < bottomPt) {
            layout.pages.push_back(TocPage{});
            cursorY = topPt - options.lineLeadingPt;
        }

        TocLine line;
        line.text = node.title;
        line.depth = depth;
        line.fontSizePt = options.fontSizePt;
        line.xPt = leftPt + options.indentPerLevelPt * depth;
        line.baselineYPt = cursorY;

        if (node.target.kind == TargetKind::Direct) {
            line.targetPageIndex = node.target.destination.pageIndex;
            line.destination = node.target.destination;
        }

        if (options.showPageNumbers && line.targetPageIndex >= 0) {
            line.pageLabel = std::to_string(line.targetPageIndex + options.pageLabelOffset);
            const double labelWidth = estimateTextWidthPt(line.pageLabel, options.fontSizePt);
            line.pageLabelXPt = rightPt - labelWidth;
            line.leaderStartXPt =
                line.xPt + estimateTextWidthPt(line.text, options.fontSizePt) + 4.0;
            line.leaderEndXPt = line.pageLabelXPt - 4.0;
            if (line.leaderEndXPt < line.leaderStartXPt) line.leaderEndXPt = line.leaderStartXPt;
        }

        // 可點擊區域涵蓋整行寬度（含引導點與頁碼），這是使用者對目錄的期待：
        // 點到點點也要能跳。
        line.rect = RectF{line.xPt, cursorY - options.fontSizePt * 0.25, rightPt,
                          cursorY + options.fontSizePt * 0.85};
        layout.pages.back().lines.push_back(std::move(line));
    });

    return layout;
}

// ---------------------------------------------------------------------------
// PRD-BM-008：由書籤建連結
// ---------------------------------------------------------------------------

struct BookmarkLink {
    std::int32_t pageIndex{0};  // 連結**所在**的頁（目錄頁），不是目標頁
    RectF rect{};
    Destination destination{};
};

// 由已排好的目錄版面產生連結。分成兩步而不是一次做完，是因為目錄頁可能不只
// 一頁，連結所在的頁碼要等到目錄頁實際插進文件、拿到起始頁索引之後才知道。
[[nodiscard]] inline std::vector<BookmarkLink> linksFromTocLayout(const TocLayout& layout,
                                                                  std::int32_t firstTocPageIndex) {
    std::vector<BookmarkLink> links;
    for (std::size_t page = 0; page < layout.pages.size(); ++page) {
        for (const TocLine& line : layout.pages[page].lines) {
            if (!line.destination.has_value()) continue;
            BookmarkLink link;
            link.pageIndex = firstTocPageIndex + static_cast<std::int32_t>(page);
            link.rect = line.rect;
            link.destination = *line.destination;
            links.push_back(link);
        }
    }
    return links;
}

// ---------------------------------------------------------------------------
// PRD-BM-009 / 010：匯出純文字與 HTML
// ---------------------------------------------------------------------------

struct ExportOptions {
    bool includePageNumbers{true};
    std::string indent{"    "};
    std::int32_t pageLabelOffset{1};
    std::string documentTitle{"Bookmarks"};
    int maxDepth{kMaxDepth};
};

[[nodiscard]] inline std::string exportAsText(const BookmarkTree& tree,
                                              const ExportOptions& options = {}) {
    std::string out;
    visit(tree, [&](const Bookmark& node, const BookmarkPath&, int depth) {
        if (depth >= options.maxDepth) return;
        for (int i = 0; i < depth; ++i) out += options.indent;
        out += node.title;
        if (options.includePageNumbers && node.target.kind == TargetKind::Direct) {
            out += '\t';
            out += std::to_string(node.target.destination.pageIndex + options.pageLabelOffset);
        }
        out += '\n';
    });
    return out;
}

[[nodiscard]] inline std::string escapeHtml(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

// 巢狀 <ul>。用巢狀清單而不是縮排的 <div>，是因為前者在瀏覽器與螢幕閱讀器上
// 都保有層級語意（PRD-A11Y 的一致立場），而縮排只有視覺效果。
[[nodiscard]] inline std::string exportAsHtml(const BookmarkTree& tree,
                                              const ExportOptions& options = {}) {
    std::string out;
    out += "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n<title>";
    out += escapeHtml(options.documentTitle);
    out += "</title>\n</head>\n<body>\n<h1>";
    out += escapeHtml(options.documentTitle);
    out += "</h1>\n";

    int openLevels = 0;
    const auto indentFor = [](int level) { return std::string(static_cast<std::size_t>(level) * 2, ' '); };

    visit(tree, [&](const Bookmark& node, const BookmarkPath&, int depth) {
        if (depth >= options.maxDepth) return;
        while (openLevels > depth) {
            --openLevels;
            out += indentFor(openLevels) + "</ul>\n";
        }
        while (openLevels <= depth) {
            out += indentFor(openLevels) + "<ul>\n";
            ++openLevels;
        }
        out += indentFor(openLevels) + "<li>" + escapeHtml(node.title);
        if (options.includePageNumbers && node.target.kind == TargetKind::Direct) {
            out += " <span class=\"page\">";
            out += std::to_string(node.target.destination.pageIndex + options.pageLabelOffset);
            out += "</span>";
        }
        out += "</li>\n";
    });

    while (openLevels > 0) {
        --openLevels;
        out += indentFor(openLevels) + "</ul>\n";
    }
    out += "</body>\n</html>\n";
    return out;
}

// ---------------------------------------------------------------------------
// PRD-BM-012 / 013：由目錄頁文字與文字檔產生書籤
// ---------------------------------------------------------------------------

namespace detail {

// 由行首空白算層級。一行的縮排不見得是 unit 的整數倍（目錄頁的文字擷取
// 會帶進不規則的空白），因此用「已見過的縮排量」排序後取名次，
// 而不是硬除以固定寬度。
struct IndentedLine {
    std::string title;
    std::int32_t pageNumber{-1};  // 使用者看到的頁碼（1 起算）；-1 代表沒有
    int indent{0};
};

[[nodiscard]] inline int leadingIndent(std::string_view line, int tabWidth) {
    int indent = 0;
    for (const char c : line) {
        if (c == ' ') ++indent;
        else if (c == '\t') indent += tabWidth;
        else break;
    }
    return indent;
}

// 從行尾抓頁碼。允許 "標題 .... 12"、"標題\t12"、"標題 12"。
// 只認阿拉伯數字：羅馬數字的前置頁碼（i、ii）在多數文件裡與正文頁碼是兩套
// 編號，猜錯會把整份目錄的頁碼往前挪。
[[nodiscard]] inline bool splitTrailingPageNumber(std::string_view line, std::string& title,
                                                  std::int32_t& page) {
    std::size_t end = line.size();
    while (end > 0 && (line[end - 1] == ' ' || line[end - 1] == '\t')) --end;
    std::size_t digitsBegin = end;
    while (digitsBegin > 0 && line[digitsBegin - 1] >= '0' && line[digitsBegin - 1] <= '9') {
        --digitsBegin;
    }
    if (digitsBegin == end) return false;
    if (digitsBegin == 0) return false;  // 整行都是數字，那是頁碼不是標題

    std::string head(line.substr(0, digitsBegin));
    // 去掉引導點與空白。
    std::size_t tail = head.size();
    while (tail > 0 && (head[tail - 1] == ' ' || head[tail - 1] == '\t' || head[tail - 1] == '.' ||
                        head[tail - 1] == '\xB7')) {
        --tail;
    }
    head.resize(tail);
    head = trimmed(head);
    if (head.empty()) return false;

    long value = 0;
    for (std::size_t i = digitsBegin; i < end; ++i) value = value * 10 + (line[i] - '0');
    if (value <= 0 || value > 1000000) return false;

    title = std::move(head);
    page = static_cast<std::int32_t>(value);
    return true;
}

// 把帶縮排的平坦行列表組成樹。
[[nodiscard]] inline BookmarkTree buildFromIndentedLines(const std::vector<IndentedLine>& lines,
                                                         std::int32_t pageIndexOffset,
                                                         int maxDepth) {
    BookmarkTree tree;
    // 每一層記住縮排量與「插到哪個容器」。用容器指標會在 vector 成長時失效，
    // 所以記路徑再每次查回去——樹不深（上限 kMaxDepth），成本可忽略。
    struct Level {
        int indent;
        BookmarkPath path;  // 該層最後一個節點的路徑
    };
    std::vector<Level> stack;

    for (const IndentedLine& line : lines) {
        while (!stack.empty() && line.indent <= stack.back().indent) stack.pop_back();
        if (static_cast<int>(stack.size()) >= maxDepth) {
            // 超過上限就併到最後一個合法層級，而不是丟掉這一行。
            stack.resize(static_cast<std::size_t>(maxDepth) - 1);
        }

        Bookmark node;
        node.title = line.title;
        if (line.pageNumber > 0) {
            Destination destination;
            destination.pageIndex = line.pageNumber - 1 + pageIndexOffset;
            if (destination.pageIndex < 0) destination.pageIndex = 0;
            destination.zoom = ZoomType::Fit;
            node.target = BookmarkTarget::direct(destination);
        }

        BookmarkPath parentPath;
        if (!stack.empty()) parentPath = stack.back().path;

        BookmarkTree* container = containerAt(tree, parentPath);
        if (container == nullptr) container = &tree;
        const std::size_t index = container->size();
        container->push_back(std::move(node));

        BookmarkPath myPath = parentPath;
        myPath.push_back(index);
        stack.push_back(Level{line.indent, myPath});
    }
    return tree;
}

}  // namespace detail

struct TextOutlineParseOptions {
    int tabWidth{4};
    // pageIndex = 印出的頁碼 - 1 + pageIndexOffset。掃描件的正文常常從第 3 頁
    // 才開始編號 1，這個欄位就是那個差。
    std::int32_t pageIndexOffset{0};
    bool requirePageNumber{false};  // 目錄頁解析時開啟，濾掉頁首頁尾雜訊
    int maxDepth{8};
};

// PRD-BM-013：由文字檔產生書籤。格式是「縮排 + 標題 [+ TAB/空白 + 頁碼]」，
// 也就是 exportAsText 的輸出——匯出與匯入互為反向是刻意的，讓使用者可以
// 匯出、用文字編輯器整批改、再匯入。
[[nodiscard]] inline BookmarkTree bookmarksFromText(std::string_view text,
                                                    const TextOutlineParseOptions& options = {}) {
    std::vector<detail::IndentedLine> parsed;
    for (const std::string& raw : detail::splitLines(text)) {
        const std::string body = detail::trimmed(raw);
        if (body.empty()) continue;

        detail::IndentedLine line;
        line.indent = detail::leadingIndent(raw, options.tabWidth);

        std::string title;
        std::int32_t page = -1;
        if (detail::splitTrailingPageNumber(body, title, page)) {
            line.title = std::move(title);
            line.pageNumber = page;
        } else {
            if (options.requirePageNumber) continue;
            line.title = body;
        }
        parsed.push_back(std::move(line));
    }
    return detail::buildFromIndentedLines(parsed, options.pageIndexOffset,
                                          std::max(options.maxDepth, 1));
}

// PRD-BM-012：由目錄頁產生書籤。輸入是目錄頁擷取出來的文字。
// 與上一個函式的差別只在預設值：目錄頁一定有頁碼，沒有頁碼的行是頁首頁尾。
[[nodiscard]] inline BookmarkTree bookmarksFromTocText(std::string_view text,
                                                       TextOutlineParseOptions options = {}) {
    options.requirePageNumber = true;
    return bookmarksFromText(text, options);
}

// ---------------------------------------------------------------------------
// PRD-BM-014：由高亮產生書籤
// ---------------------------------------------------------------------------

struct HighlightSource {
    std::int32_t pageIndex{0};
    std::string text;
    double topPt{0.0};   // 高亮矩形的上緣（頁面座標，Y 向上）
    double leftPt{0.0};
};

struct FromHighlightsOptions {
    bool groupByPage{false};      // 加一層「第 N 頁」的父節點
    std::size_t maxTitleChars{80};
    std::string pageGroupPattern{"Page {page}"};
    std::int32_t pageLabelOffset{1};
};

namespace detail {

// 依 UTF-8 字元邊界截斷。逐位元組截會把中文字切一半，
// 產出的 PDF 字串在部分檢視器上是亂碼，在另一些上是問號。
[[nodiscard]] inline std::string truncateUtf8(std::string_view text, std::size_t maxChars) {
    if (maxChars == 0) return {};
    std::size_t chars = 0;
    std::size_t i = 0;
    while (i < text.size() && chars < maxChars) {
        const unsigned char u = static_cast<unsigned char>(text[i]);
        std::size_t length = 1;
        if ((u & 0xE0) == 0xC0) length = 2;
        else if ((u & 0xF0) == 0xE0) length = 3;
        else if ((u & 0xF8) == 0xF0) length = 4;
        if (i + length > text.size()) break;
        i += length;
        ++chars;
    }
    if (i >= text.size()) return std::string(text);
    return std::string(text.substr(0, i)) + "...";
}

}  // namespace detail

[[nodiscard]] inline BookmarkTree bookmarksFromHighlights(
    std::vector<HighlightSource> highlights, const FromHighlightsOptions& options = {}) {
    // 閱讀順序：先頁碼、同頁由上而下（頁面座標 Y 向上，所以 top 大的在前）。
    std::stable_sort(highlights.begin(), highlights.end(),
                     [](const HighlightSource& a, const HighlightSource& b) {
                         if (a.pageIndex != b.pageIndex) return a.pageIndex < b.pageIndex;
                         if (a.topPt != b.topPt) return a.topPt > b.topPt;
                         return a.leftPt < b.leftPt;
                     });

    BookmarkTree tree;
    std::int32_t currentPage = -1;
    for (const HighlightSource& highlight : highlights) {
        Bookmark node;
        node.title = detail::truncateUtf8(detail::trimmed(highlight.text), options.maxTitleChars);
        if (node.title.empty()) node.title = "(untitled)";
        node.target =
            BookmarkTarget::direct(Destination::atTop(highlight.pageIndex, highlight.topPt));

        if (!options.groupByPage) {
            tree.push_back(std::move(node));
            continue;
        }
        if (highlight.pageIndex != currentPage || tree.empty()) {
            currentPage = highlight.pageIndex;
            Bookmark group;
            group.title = detail::replaceAll(
                options.pageGroupPattern, "{page}",
                std::to_string(highlight.pageIndex + options.pageLabelOffset));
            group.open = true;
            group.target = BookmarkTarget::direct(Destination::fitPage(highlight.pageIndex));
            tree.push_back(std::move(group));
        }
        tree.back().children.push_back(std::move(node));
    }
    return tree;
}

// ---------------------------------------------------------------------------
// PRD-BM-015：由頁面文字產生書籤
// ---------------------------------------------------------------------------

struct TextLine {
    std::int32_t pageIndex{0};
    std::string text;
    double fontSizePt{0.0};
    double topPt{0.0};
    bool bold{false};
};

struct HeadingDetectionOptions {
    // 0 代表自動判定：取出現次數最多的字級當內文字級。用「最多」而不是
    // 「最小」，因為頁碼與註腳的字級往往比內文更小，取最小會把內文整段
    // 誤判成標題。
    double bodyFontSizePt{0.0};
    double headingRatio{1.12};  // 大於內文這個倍數才算標題
    int maxLevels{4};
    std::size_t maxTitleChars{120};
    std::size_t maxTitleWords{20};  // 太長的行是段落不是標題
    bool acceptBoldAtBodySize{false};
};

namespace detail {

[[nodiscard]] inline double quantizeFontSize(double size) {
    // 同一個標題的字級在文字擷取後常常差 0.01 點，不量化會讓每一個標題
    // 自成一級，層級數瞬間爆掉。
    return std::floor(size * 2.0 + 0.5) / 2.0;
}

[[nodiscard]] inline std::size_t countWords(std::string_view text) {
    std::size_t words = 0;
    bool inWord = false;
    for (const char c : text) {
        const bool space = c == ' ' || c == '\t';
        if (!space && !inWord) ++words;
        inWord = !space;
    }
    return words;
}

}  // namespace detail

[[nodiscard]] inline BookmarkTree bookmarksFromPageText(const std::vector<TextLine>& lines,
                                                        HeadingDetectionOptions options = {}) {
    if (lines.empty()) return {};

    if (options.bodyFontSizePt <= 0.0) {
        std::map<double, std::size_t> histogram;
        for (const TextLine& line : lines) {
            if (detail::trimmed(line.text).empty()) continue;
            histogram[detail::quantizeFontSize(line.fontSizePt)] += line.text.size();
        }
        std::size_t best = 0;
        for (const auto& [size, weight] : histogram) {
            if (weight > best) {
                best = weight;
                options.bodyFontSizePt = size;
            }
        }
    }
    if (options.bodyFontSizePt <= 0.0) return {};

    // 先收集所有夠大的字級，由大到小排出層級。
    std::vector<double> headingSizes;
    const double threshold = options.bodyFontSizePt * options.headingRatio;
    for (const TextLine& line : lines) {
        const double size = detail::quantizeFontSize(line.fontSizePt);
        if (size < threshold) continue;
        if (std::find(headingSizes.begin(), headingSizes.end(), size) == headingSizes.end()) {
            headingSizes.push_back(size);
        }
    }
    std::sort(headingSizes.begin(), headingSizes.end(), std::greater<double>());
    if (static_cast<int>(headingSizes.size()) > options.maxLevels) {
        headingSizes.resize(static_cast<std::size_t>(options.maxLevels));
    }

    std::vector<detail::IndentedLine> flattened;
    std::vector<std::pair<int, Destination>> targets;  // 與 flattened 同索引

    for (const TextLine& line : lines) {
        const std::string title = detail::trimmed(line.text);
        if (title.empty()) continue;
        if (detail::countWords(title) > options.maxTitleWords) continue;

        const double size = detail::quantizeFontSize(line.fontSizePt);
        const auto it = std::find(headingSizes.begin(), headingSizes.end(), size);
        int level = -1;
        if (it != headingSizes.end()) {
            level = static_cast<int>(std::distance(headingSizes.begin(), it));
        } else if (options.acceptBoldAtBodySize && line.bold && size >= options.bodyFontSizePt) {
            level = static_cast<int>(headingSizes.size());
        }
        if (level < 0) continue;
        if (level >= options.maxLevels) continue;

        detail::IndentedLine entry;
        entry.title = detail::truncateUtf8(title, options.maxTitleChars);
        entry.indent = level;
        flattened.push_back(std::move(entry));
        targets.emplace_back(level, Destination::atTop(line.pageIndex, line.topPt));
    }

    // buildFromIndentedLines 只會給 /Fit 目標，這裡要的是保留垂直位置的
    // /XYZ，所以自行組樹而不重用它。
    BookmarkTree tree;
    struct Level {
        int indent;
        BookmarkPath path;
    };
    std::vector<Level> stack;
    for (std::size_t i = 0; i < flattened.size(); ++i) {
        const int level = flattened[i].indent;
        while (!stack.empty() && level <= stack.back().indent) stack.pop_back();

        Bookmark node;
        node.title = flattened[i].title;
        node.target = BookmarkTarget::direct(targets[i].second);

        BookmarkPath parentPath;
        if (!stack.empty()) parentPath = stack.back().path;
        BookmarkTree* container = containerAt(tree, parentPath);
        if (container == nullptr) container = &tree;
        const std::size_t index = container->size();
        container->push_back(std::move(node));

        BookmarkPath myPath = parentPath;
        myPath.push_back(index);
        stack.push_back(Level{level, myPath});
    }
    return tree;
}

// ---------------------------------------------------------------------------
// PRD-BM-018：依書籤排序頁面
// ---------------------------------------------------------------------------

struct PageOrderResult {
    std::vector<std::int32_t> order;         // 新頁序：order[i] 是原本的頁索引
    std::vector<std::int32_t> unreferenced;  // 沒有任何書籤指到的頁（附在最後）
    bool identity{true};                     // 與原順序相同時為 true，可省下整次寫入
};

// 依書籤的前序走訪決定頁序。沒有書籤指到的頁不會被丟掉，而是按原順序附在
// 尾端——丟頁是不可逆的資料損失，而使用者要的只是「重排」。
[[nodiscard]] inline PageOrderResult pageOrderFromBookmarks(const BookmarkTree& tree,
                                                            std::int32_t pageCount) {
    PageOrderResult result;
    if (pageCount <= 0) return result;

    std::vector<bool> seen(static_cast<std::size_t>(pageCount), false);
    visit(tree, [&](const Bookmark& node, const BookmarkPath&, int) {
        if (node.target.kind != TargetKind::Direct) return;
        const std::int32_t page = node.target.destination.pageIndex;
        if (page < 0 || page >= pageCount) return;
        if (seen[static_cast<std::size_t>(page)]) return;
        seen[static_cast<std::size_t>(page)] = true;
        result.order.push_back(page);
    });

    for (std::int32_t page = 0; page < pageCount; ++page) {
        if (seen[static_cast<std::size_t>(page)]) continue;
        result.unreferenced.push_back(page);
        result.order.push_back(page);
    }

    result.identity = true;
    for (std::size_t i = 0; i < result.order.size(); ++i) {
        if (result.order[i] != static_cast<std::int32_t>(i)) {
            result.identity = false;
            break;
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// PRD-BM-019：驗證書籤
// ---------------------------------------------------------------------------

enum class IssueKind {
    EmptyTitle,
    MissingTarget,
    PageOutOfRange,
    UnresolvedName,
    DuplicateSibling,
    ExcessiveDepth,
};

[[nodiscard]] inline const char* describe(IssueKind kind) noexcept {
    switch (kind) {
        case IssueKind::EmptyTitle: return "標題是空的";
        case IssueKind::MissingTarget: return "沒有跳轉目標";
        case IssueKind::PageOutOfRange: return "指向不存在的頁面";
        case IssueKind::UnresolvedName: return "命名目標不存在";
        case IssueKind::DuplicateSibling: return "同層有完全相同的書籤";
        case IssueKind::ExcessiveDepth: return "巢狀深度超過上限";
    }
    return "未知問題";
}

struct ValidationIssue {
    BookmarkPath path;
    IssueKind kind{IssueKind::EmptyTitle};
    std::string title;
    std::string detail;
};

struct ValidationOptions {
    std::int32_t pageCount{0};
    int maxDepth{kMaxDepth};
    std::vector<std::string> knownDestinationNames;
    bool reportMissingTarget{true};
    bool reportDuplicateSiblings{true};
};

// 全部問題一次列出而不是遇到第一個就停：使用者要的是一份可以逐項修的清單，
// 「還有沒有別的問題」不該需要重跑一次驗證才知道。
[[nodiscard]] inline std::vector<ValidationIssue> validate(const BookmarkTree& tree,
                                                           const ValidationOptions& options) {
    std::vector<ValidationIssue> issues;

    visit(tree, [&](const Bookmark& node, const BookmarkPath& path, int depth) {
        const auto report = [&](IssueKind kind, std::string detail) {
            issues.push_back(ValidationIssue{path, kind, node.title, std::move(detail)});
        };

        if (detail::trimmed(node.title).empty()) {
            report(IssueKind::EmptyTitle, "書籤面板上會顯示成一列空白");
        }
        if (depth + 1 > options.maxDepth) {
            report(IssueKind::ExcessiveDepth,
                   "深度 " + std::to_string(depth + 1) + " 超過上限 " +
                       std::to_string(options.maxDepth));
        }

        switch (node.target.kind) {
            case TargetKind::None:
                if (options.reportMissingTarget) {
                    report(IssueKind::MissingTarget, "點擊不會有任何反應");
                }
                break;
            case TargetKind::Direct: {
                const std::int32_t page = node.target.destination.pageIndex;
                if (page < 0 || page >= options.pageCount) {
                    report(IssueKind::PageOutOfRange,
                           "目標頁 " + std::to_string(page + 1) + "，文件只有 " +
                               std::to_string(options.pageCount) + " 頁");
                }
                break;
            }
            case TargetKind::Named: {
                const auto& names = options.knownDestinationNames;
                if (std::find(names.begin(), names.end(), node.target.name) == names.end()) {
                    report(IssueKind::UnresolvedName, "找不到命名目標 " + node.target.name);
                }
                break;
            }
        }
    });

    if (options.reportDuplicateSiblings) {
        // 同層重複只在同一個容器內比較，跨層同名是合法的（章與節可以同名）。
        const auto scanLevel = [&issues](const BookmarkTree& level, const BookmarkPath& base,
                                         const auto& self) -> void {
            for (std::size_t i = 0; i < level.size(); ++i) {
                for (std::size_t j = 0; j < i; ++j) {
                    if (level[i].title != level[j].title) continue;
                    if (!detail::sameTarget(level[i].target, level[j].target)) continue;
                    BookmarkPath path = base;
                    path.push_back(i);
                    issues.push_back(ValidationIssue{path, IssueKind::DuplicateSibling,
                                                     level[i].title,
                                                     "與同層第 " + std::to_string(j + 1) +
                                                         " 項完全相同"});
                    break;
                }
                BookmarkPath child = base;
                child.push_back(i);
                self(level[i].children, child, self);
            }
        };
        scanLevel(tree, BookmarkPath{}, scanLevel);
    }

    return issues;
}

// 把 domain::OutlineNode（讀取路徑的精簡模型）轉成可寫入的 Bookmark。
// 存在理由是「讀出來、改一改、寫回去」這個最常見的流程；轉換會遺失
// OutlineNode 沒有的資訊（顏色、樣式、命名目標），所以只適合當起點。
[[nodiscard]] inline Bookmark fromOutlineNode(const OutlineNode& source) {
    Bookmark node;
    node.title = source.title;
    if (source.pageIndex.has_value()) {
        Destination destination;
        destination.pageIndex = *source.pageIndex;
        if (source.destination.has_value()) {
            destination.zoom = ZoomType::XYZ;
            destination.left = source.destination->x;
            destination.top = source.destination->y;
            destination.zoomFactor = source.zoom;
        } else {
            destination.zoom = ZoomType::Fit;
        }
        node.target = BookmarkTarget::direct(destination);
    }
    node.children.reserve(source.children.size());
    for (const OutlineNode& child : source.children) node.children.push_back(fromOutlineNode(child));
    return node;
}

[[nodiscard]] inline BookmarkTree fromOutline(const std::vector<OutlineNode>& source) {
    BookmarkTree tree;
    tree.reserve(source.size());
    for (const OutlineNode& node : source) tree.push_back(fromOutlineNode(node));
    return tree;
}

}  // namespace alioth::domain::bookmarks
