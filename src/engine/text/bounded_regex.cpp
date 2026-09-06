#include "engine/text/bounded_regex.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace alioth::engine::text {
namespace {

// 展開後的 NFA 指令數上限。存在理由不是效能，是資源上限：一個像 {1,100000}
// 這種量詞不會造成回溯爆炸（本引擎本來就不回溯），但展開成指令序列仍然會吃
// 記憶體與編譯時間，本身就是一種阻斷服務手段。編譯階段就擋下來，而不是讓它
// 编譯成功後才在執行期間發現「這個查詢吃光了記憶體」。
constexpr std::size_t kMaxInstructions = 4096;

enum class Op : std::uint8_t {
    Char, Class, Any, Jmp, Split, AssertStart, AssertEnd, MarkStart, Match,
};

struct ClassRange {
    char32_t lo{0};
    char32_t hi{0};
};

struct Inst {
    Op op{Op::Match};
    char32_t ch{0};
    std::vector<ClassRange> ranges;  // 僅 Class 使用
    bool negated{false};             // 僅 Class 使用
    int x{-1};
    int y{-1};
};

Inst instOp(Op op) {
    Inst i;
    i.op = op;
    return i;
}

char32_t foldAsciiCp(char32_t c) noexcept {
    return (c >= U'A' && c <= U'Z') ? (c - U'A' + U'a') : c;
}

// ---- 語法樹 --------------------------------------------------------------

enum class NType {
    Char, Any, Class, Concat, Alt, Star, Plus, Opt, Repeat, StartAnchor, EndAnchor,
};

struct AstNode {
    NType type{NType::Concat};
    char32_t ch{0};
    std::vector<ClassRange> ranges;
    bool negated{false};
    std::vector<std::unique_ptr<AstNode>> kids;
    int repMin{0};
    int repMax{-1};  // -1 代表無上限
};
using NodePtr = std::unique_ptr<AstNode>;

NodePtr makeNode(NType type) {
    auto n = std::make_unique<AstNode>();
    n->type = type;
    return n;
}

// ---- 語法解析 --------------------------------------------------------------
//
// 遞迴下降。輸入已先解碼成碼點陣列（見檔尾 decodeText），因此樣式本身允許
// 出現多位元組字元（例如使用者想搜尋含中文的正則式）。

class Parser {
public:
    explicit Parser(const std::vector<char32_t>& pattern) : p_(pattern) {}

    NodePtr parse(std::string* err) {
        NodePtr root = parseAlt();
        if (!ok_) {
            *err = err_;
            return nullptr;
        }
        if (pos_ != p_.size()) {
            *err = "樣式結尾多出未預期的字元（可能是懸空的 ) 或 ]）";
            return nullptr;
        }
        return root;
    }

private:
    const std::vector<char32_t>& p_;
    std::size_t pos_{0};
    bool ok_{true};
    std::string err_;

    [[nodiscard]] bool atEnd() const noexcept { return pos_ >= p_.size(); }
    [[nodiscard]] char32_t peek() const noexcept { return atEnd() ? 0 : p_[pos_]; }
    char32_t consume() noexcept { return p_[pos_++]; }
    void fail(std::string msg) {
        if (ok_) {
            ok_ = false;
            err_ = std::move(msg);
        }
    }

    NodePtr parseAlt() {
        NodePtr left = parseConcat();
        if (!ok_) return nullptr;
        if (atEnd() || peek() != U'|') return left;

        NodePtr alt = makeNode(NType::Alt);
        alt->kids.push_back(std::move(left));
        while (!atEnd() && peek() == U'|') {
            consume();
            NodePtr rhs = parseConcat();
            if (!ok_) return nullptr;
            alt->kids.push_back(std::move(rhs));
        }
        return alt;
    }

    NodePtr parseConcat() {
        NodePtr node = makeNode(NType::Concat);
        while (ok_ && !atEnd() && peek() != U'|' && peek() != U')') {
            NodePtr atom = parseRepeat();
            if (!ok_) return nullptr;
            node->kids.push_back(std::move(atom));
        }
        return node;
    }

    // 整數上限只是防呆（避免使用者打一串幾百位數字），真正擋住資源耗盡的是
    // Compiler 的 kMaxInstructions。
    bool parseInt(int* out) {
        if (atEnd() || peek() < U'0' || peek() > U'9') return false;
        long value = 0;
        while (!atEnd() && peek() >= U'0' && peek() <= U'9') {
            value = value * 10 + (consume() - U'0');
            if (value > 100000) value = 100000;
        }
        *out = static_cast<int>(value);
        return true;
    }

    NodePtr parseRepeat() {
        NodePtr atom = parseAtom();
        if (!ok_ || atEnd()) return atom;

        const char32_t c = peek();
        if (c == U'*' || c == U'+' || c == U'?') {
            consume();
            if (!atEnd() && peek() == U'?') {
                fail("不支援非貪婪量詞（*?、+?、??）：本引擎不回溯，因此只提供單一種"
                     "「找得到就是這個範圍」語意，不區分貪婪／非貪婪的候選順序");
                return nullptr;
            }
            NodePtr node = makeNode(c == U'*' ? NType::Star : c == U'+' ? NType::Plus : NType::Opt);
            node->kids.push_back(std::move(atom));
            return node;
        }
        if (c == U'{') {
            const std::size_t save = pos_;
            consume();
            int mn = -1;
            int mx = -1;
            if (!parseInt(&mn)) {
                pos_ = save;
                fail("{} 量詞語法錯誤：缺少數字");
                return nullptr;
            }
            if (!atEnd() && peek() == U',') {
                consume();
                if (!atEnd() && peek() != U'}') {
                    if (!parseInt(&mx)) {
                        fail("{} 量詞語法錯誤：, 之後的上限不是數字");
                        return nullptr;
                    }
                }
            } else {
                mx = mn;
            }
            if (atEnd() || peek() != U'}') {
                fail("{} 量詞缺少結尾的 }");
                return nullptr;
            }
            consume();
            if (!atEnd() && peek() == U'?') {
                fail("不支援非貪婪量詞");
                return nullptr;
            }
            if (mn < 0 || (mx >= 0 && mx < mn)) {
                fail("{} 量詞範圍不合法（下限大於上限）");
                return nullptr;
            }
            NodePtr node = makeNode(NType::Repeat);
            node->repMin = mn;
            node->repMax = mx;
            node->kids.push_back(std::move(atom));
            return node;
        }
        return atom;
    }

    NodePtr parseAtom() {
        if (atEnd()) {
            fail("樣式不完整");
            return nullptr;
        }
        const char32_t c = consume();
        switch (c) {
            case U'.':
                return makeNode(NType::Any);
            case U'^':
                return makeNode(NType::StartAnchor);
            case U'$':
                return makeNode(NType::EndAnchor);
            case U'(': {
                NodePtr inner = parseAlt();
                if (!ok_) return nullptr;
                if (atEnd() || peek() != U')') {
                    fail("缺少對應的 )");
                    return nullptr;
                }
                consume();
                return inner;
            }
            case U'[':
                return parseClass();
            case U'\\':
                return parseEscape();
            case U'*':
            case U'+':
            case U'?':
            case U')':
            case U']':
            case U'{':
            case U'}':
            case U'|':
                fail("未預期的字元（沒有對應的量詞主體，或懸空的括號）");
                return nullptr;
            default: {
                NodePtr n = makeNode(NType::Char);
                n->ch = c;
                return n;
            }
        }
    }

    static NodePtr makeClass(std::vector<ClassRange> ranges, bool negated) {
        NodePtr n = makeNode(NType::Class);
        n->ranges = std::move(ranges);
        n->negated = negated;
        return n;
    }

    NodePtr parseEscape() {
        if (atEnd()) {
            fail("樣式結尾出現懸空的跳脫字元 \\");
            return nullptr;
        }
        const char32_t c = consume();
        switch (c) {
            case U'd': return makeClass({{U'0', U'9'}}, false);
            case U'D': return makeClass({{U'0', U'9'}}, true);
            case U'w': return makeClass(wordRanges(), false);
            case U'W': return makeClass(wordRanges(), true);
            case U's': return makeClass(spaceRanges(), false);
            case U'S': return makeClass(spaceRanges(), true);
            case U'n': { NodePtr n = makeNode(NType::Char); n->ch = U'\n'; return n; }
            case U't': { NodePtr n = makeNode(NType::Char); n->ch = U'\t'; return n; }
            case U'r': { NodePtr n = makeNode(NType::Char); n->ch = U'\r'; return n; }
            case U'.': case U'^': case U'$': case U'*': case U'+': case U'?': case U'(':
            case U')': case U'[': case U']': case U'{': case U'}': case U'|': case U'\\':
            case U'/': {
                NodePtr n = makeNode(NType::Char);
                n->ch = c;
                return n;
            }
            default:
                if (c >= U'1' && c <= U'9') {
                    fail("不支援回頭參照（\\1 等）：不存在不回溯又支援回頭參照的通用演算法");
                    return nullptr;
                }
                fail("不支援的跳脫序列");
                return nullptr;
        }
    }

    static std::vector<ClassRange> wordRanges() {
        return {{U'a', U'z'}, {U'A', U'Z'}, {U'0', U'9'}, {U'_', U'_'}};
    }
    static std::vector<ClassRange> spaceRanges() {
        return {{U' ', U' '}, {U'\t', U'\t'}, {U'\n', U'\n'}, {U'\r', U'\r'},
                {U'\f', U'\f'}, {U'\v', U'\v'}};
    }

    NodePtr parseClass() {
        bool negated = false;
        if (!atEnd() && peek() == U'^') {
            negated = true;
            consume();
        }
        std::vector<ClassRange> ranges;
        bool first = true;
        for (;;) {
            if (atEnd()) {
                fail("字元類缺少結尾的 ]");
                return nullptr;
            }
            if (peek() == U']' && !first) {
                consume();
                break;
            }
            first = false;

            char32_t lo = 0;
            bool isRangeCandidate = true;
            if (peek() == U'\\') {
                consume();
                if (atEnd()) {
                    fail("字元類內出現懸空的跳脫字元");
                    return nullptr;
                }
                const char32_t e = consume();
                switch (e) {
                    case U'd': appendAll(ranges, {{U'0', U'9'}}); isRangeCandidate = false; break;
                    case U'w': appendAll(ranges, wordRanges()); isRangeCandidate = false; break;
                    case U's': appendAll(ranges, spaceRanges()); isRangeCandidate = false; break;
                    case U'D': case U'W': case U'S':
                        fail("字元類內不支援 \\D \\W \\S（否定跳脫類疊在字元類的否定語意上不明確，"
                             "改用外層 [^...] 或把否定類拆到類外）");
                        return nullptr;
                    case U'n': lo = U'\n'; break;
                    case U't': lo = U'\t'; break;
                    case U'r': lo = U'\r'; break;
                    default: lo = e; break;  // \] \- \\ 等逐字處理
                }
            } else {
                lo = consume();
            }
            if (!isRangeCandidate) continue;

            char32_t hi = lo;
            if (!atEnd() && peek() == U'-' && pos_ + 1 < p_.size() && p_[pos_ + 1] != U']') {
                consume();  // '-'
                if (!atEnd() && peek() == U'\\') {
                    consume();
                    if (atEnd()) {
                        fail("字元類內出現懸空的跳脫字元");
                        return nullptr;
                    }
                    hi = consume();
                } else if (!atEnd()) {
                    hi = consume();
                }
            }
            if (hi < lo) {
                fail("字元類的範圍頭尾顛倒（例如 [z-a]）");
                return nullptr;
            }
            ranges.push_back(ClassRange{lo, hi});
        }
        return makeClass(std::move(ranges), negated);
    }

    static void appendAll(std::vector<ClassRange>& dst, std::vector<ClassRange> src) {
        for (const ClassRange& r : src) dst.push_back(r);
    }
};

// ---- 編譯：語法樹 → NFA 指令序列 -------------------------------------------
//
// 每個節點的 emit() 呼叫完成後，控制流「自然落到」目前程式的結尾（呼叫端不需
// 要知道任何跳躍目標），除了 Alt / Star / Plus / Opt / Repeat 需要在自己的子樹
// 範圍內組出跳躍——而這些跳躍的目標永遠是「目前已知的位置」（子樹發射前後的
// 位置），不需要對還沒發射的未來程式碼做前向參照，因此不需要傳統的 fragment
// patch-list，直接記錄索引再回填即可。
class Compiler {
public:
    bool compile(const AstNode& root, std::vector<Inst>* out, std::string* err) {
        insts_ = out;

        const bool anchored =
            (root.type == NType::StartAnchor) ||
            (root.type == NType::Concat && !root.kids.empty() &&
             root.kids.front()->type == NType::StartAnchor);

        std::size_t splitIdx = 0;
        if (!anchored) splitIdx = push(instOp(Op::Split));
        push(instOp(Op::MarkStart));
        emit(root);
        if (ok_) push(instOp(Op::Match));

        if (ok_ && !anchored) {
            const std::size_t markIdx = splitIdx + 1;
            const std::size_t anyIdx = push(instOp(Op::Any));
            Inst jmp = instOp(Op::Jmp);
            jmp.x = static_cast<int>(splitIdx);
            push(jmp);
            if (ok_ && splitIdx < insts_->size()) {
                // x 優先於 y：先試著現在就進入使用者樣式（markIdx），
                // 找不到才退而求其次消耗一個字元再回頭重試——這個優先序
                // 正是「找最左邊那個命中」的來源（見標頭檔的說明）。
                (*insts_)[splitIdx].x = static_cast<int>(markIdx);
                (*insts_)[splitIdx].y = static_cast<int>(anyIdx);
            }
        }

        if (!ok_) {
            *err = err_;
            return false;
        }
        return true;
    }

private:
    std::vector<Inst>* insts_{nullptr};
    bool ok_{true};
    std::string err_;

    void fail(std::string msg) {
        if (ok_) {
            ok_ = false;
            err_ = std::move(msg);
        }
    }

    std::size_t push(Inst inst) {
        if (insts_->size() >= kMaxInstructions) {
            fail("樣式展開後過於複雜（常見原因是 {m,n} 量詞的重複次數過大），已拒絕編譯");
            return insts_->size();
        }
        insts_->push_back(std::move(inst));
        return insts_->size() - 1;
    }

    void emitStar(const AstNode& body) {
        const std::size_t l1 = insts_->size();
        const std::size_t sp = push(instOp(Op::Split));
        const std::size_t l2 = insts_->size();
        emit(body);
        if (!ok_) return;
        Inst jmp = instOp(Op::Jmp);
        jmp.x = static_cast<int>(l1);
        push(jmp);
        const std::size_t l3 = insts_->size();
        if (sp < insts_->size()) {
            (*insts_)[sp].x = static_cast<int>(l2);
            (*insts_)[sp].y = static_cast<int>(l3);
        }
    }

    void emitPlus(const AstNode& body) {
        const std::size_t l1 = insts_->size();
        emit(body);
        if (!ok_) return;
        const std::size_t sp = push(instOp(Op::Split));
        const std::size_t l3 = insts_->size();
        if (sp < insts_->size()) {
            (*insts_)[sp].x = static_cast<int>(l1);
            (*insts_)[sp].y = static_cast<int>(l3);
        }
    }

    void emitOpt(const AstNode& body) {
        const std::size_t sp = push(instOp(Op::Split));
        const std::size_t l2 = insts_->size();
        emit(body);
        if (!ok_) return;
        const std::size_t l3 = insts_->size();
        if (sp < insts_->size()) {
            (*insts_)[sp].x = static_cast<int>(l2);
            (*insts_)[sp].y = static_cast<int>(l3);
        }
    }

    void emitAlt(const std::vector<NodePtr>& kids, std::size_t index) {
        if (!ok_) return;
        if (index + 1 == kids.size()) {
            emit(*kids[index]);
            return;
        }
        const std::size_t sp = push(instOp(Op::Split));
        const std::size_t l2 = insts_->size();
        emit(*kids[index]);
        if (!ok_) return;
        const std::size_t jmpIdx = push(instOp(Op::Jmp));
        const std::size_t l3 = insts_->size();
        emitAlt(kids, index + 1);
        if (!ok_) return;
        const std::size_t l4 = insts_->size();
        if (sp < insts_->size()) {
            (*insts_)[sp].x = static_cast<int>(l2);
            (*insts_)[sp].y = static_cast<int>(l3);
        }
        if (jmpIdx < insts_->size()) (*insts_)[jmpIdx].x = static_cast<int>(l4);
    }

    void emit(const AstNode& n) {
        if (!ok_) return;
        switch (n.type) {
            case NType::Char: {
                Inst i = instOp(Op::Char);
                i.ch = n.ch;
                push(i);
                break;
            }
            case NType::Any:
                push(instOp(Op::Any));
                break;
            case NType::Class: {
                Inst i = instOp(Op::Class);
                i.ranges = n.ranges;
                i.negated = n.negated;
                push(i);
                break;
            }
            case NType::StartAnchor:
                push(instOp(Op::AssertStart));
                break;
            case NType::EndAnchor:
                push(instOp(Op::AssertEnd));
                break;
            case NType::Concat:
                for (const NodePtr& k : n.kids) {
                    emit(*k);
                    if (!ok_) return;
                }
                break;
            case NType::Alt:
                emitAlt(n.kids, 0);
                break;
            case NType::Star:
                emitStar(*n.kids[0]);
                break;
            case NType::Plus:
                emitPlus(*n.kids[0]);
                break;
            case NType::Opt:
                emitOpt(*n.kids[0]);
                break;
            case NType::Repeat: {
                for (int i = 0; i < n.repMin; ++i) {
                    emit(*n.kids[0]);
                    if (!ok_) return;
                }
                if (n.repMax < 0) {
                    emitStar(*n.kids[0]);
                } else {
                    for (int i = n.repMin; i < n.repMax; ++i) {
                        emitOpt(*n.kids[0]);
                        if (!ok_) return;
                    }
                }
                break;
            }
        }
    }
};

// ---- 執行：Pike VM 式的同步模擬 --------------------------------------------
//
// 每一步只做「目前所有活著的執行緒各自嘗試消耗同一個字元」，不存在任何一條
// 執行緒需要「回頭重來」，因此時間複雜度是 O(文字長度 × 程式指令數)，與輸入
// 內容（例如文字裡有多少個幾乎符合又不完全符合的片段）無關——這正是它不會
// 發生災難性回溯的原因。

struct Thread {
    int pc{0};
    std::size_t start{0};
};

bool inRanges(const std::vector<ClassRange>& ranges, char32_t c) noexcept {
    for (const ClassRange& r : ranges) {
        if (c >= r.lo && c <= r.hi) return true;
    }
    return false;
}

bool matchClass(const Inst& inst, char32_t c, bool caseInsensitive) noexcept {
    bool hit = inRanges(inst.ranges, c);
    if (!hit && caseInsensitive) {
        const char32_t alt = (c >= U'a' && c <= U'z')   ? c - U'a' + U'A'
                              : (c >= U'A' && c <= U'Z') ? c - U'A' + U'a'
                                                          : c;
        hit = inRanges(inst.ranges, alt);
    }
    return inst.negated ? !hit : hit;
}

class Vm {
public:
    explicit Vm(const std::vector<Inst>& prog) : prog_(prog) {}

    // 從 cps[fromPos] 開始找第一個命中（若樣式未錨定，實際會嘗試從 fromPos
    // 或之後的任何位置起始，取最左邊那個——見編譯階段組出的「非貪婪 .*? 前綴」）。
    bool run(const std::vector<char32_t>& cps, std::size_t fromPos, bool caseInsensitive,
             std::size_t* matchStart, std::size_t* matchEnd) const {
        const std::size_t n = cps.size();
        textLen_ = n;
        std::vector<int> genClist(prog_.size(), 0);
        std::vector<int> genNlist(prog_.size(), 0);
        int clistGen = 1;
        int nlistGen = 1;

        std::vector<Thread> clist;
        std::vector<Thread> nlist;
        addThread(clist, genClist, clistGen, 0, fromPos, fromPos);

        bool matched = false;
        std::size_t bestStart = 0;
        std::size_t bestEnd = 0;
        std::size_t pos = fromPos;

        for (;;) {
            if (clist.empty()) break;
            const bool haveChar = pos < n;
            const char32_t c = haveChar ? cps[pos] : 0;

            nlist.clear();
            ++nlistGen;
            for (const Thread& th : clist) {
                const Inst& inst = prog_[static_cast<std::size_t>(th.pc)];
                if (inst.op == Op::Match) {
                    matched = true;
                    bestStart = th.start;
                    bestEnd = pos;
                    // 較低優先序（本輪清單裡排在它之後）的執行緒一律捨棄：
                    // 它們代表「更晚起始」或「更不被偏好」的候選，命中一旦
                    // 確立就不該被它們覆蓋（見標頭檔「最左命中優先」的說明）。
                    break;
                }
                if (!haveChar) continue;
                bool ok = false;
                switch (inst.op) {
                    case Op::Char:
                        ok = caseInsensitive ? foldAsciiCp(inst.ch) == foldAsciiCp(c) : inst.ch == c;
                        break;
                    case Op::Any:
                        ok = true;
                        break;
                    case Op::Class:
                        ok = matchClass(inst, c, caseInsensitive);
                        break;
                    default:
                        break;
                }
                if (ok) {
                    addThread(nlist, genNlist, nlistGen, th.pc + 1, th.start, pos + 1);
                }
            }

            if (!haveChar) break;
            std::swap(clist, nlist);
            std::swap(genClist, genNlist);
            ++pos;
        }

        if (matched) {
            *matchStart = bestStart;
            *matchEnd = bestEnd;
        }
        return matched;
    }

private:
    const std::vector<Inst>& prog_;
    mutable std::size_t textLen_{0};  // run() 開頭設定一次，AssertEnd 判斷 $ 用

    // 對 pc 做 epsilon 閉包展開，把「真正會消耗字元」的指令（Char/Class/Any）
    // 與 Match 加進 list。用顯式堆疊而非遞迴：使用者的樣式巢狀深度雖然被
    // kMaxInstructions 間接限制，但仍以迭代實作比較保險（不可信任輸入的同一
    // 個原則：不讓使用者的輸入決定我們呼叫堆疊的深度）。
    void addThread(std::vector<Thread>& list, std::vector<int>& gen, int currentGen, int pc,
                   std::size_t start, std::size_t pos) const {
        std::vector<std::pair<int, std::size_t>> stack;
        stack.emplace_back(pc, start);
        while (!stack.empty()) {
            const auto [p, st] = stack.back();
            stack.pop_back();
            if (p < 0 || static_cast<std::size_t>(p) >= prog_.size()) continue;
            const auto idx = static_cast<std::size_t>(p);
            if (gen[idx] == currentGen) continue;
            gen[idx] = currentGen;

            const Inst& inst = prog_[idx];
            switch (inst.op) {
                case Op::Jmp:
                    stack.emplace_back(inst.x, st);
                    break;
                case Op::Split:
                    // 後進先出：先押 y 再押 x，讓 x 的整棵子樹先被走完，
                    // 維持「x 優先於 y」的語意（貪婪量詞、交替的第一分支優先）。
                    stack.emplace_back(inst.y, st);
                    stack.emplace_back(inst.x, st);
                    break;
                case Op::MarkStart:
                    stack.emplace_back(p + 1, pos);
                    break;
                case Op::AssertStart:
                    if (pos == 0) stack.emplace_back(p + 1, st);
                    break;
                case Op::AssertEnd:
                    if (pos == textLen_) stack.emplace_back(p + 1, st);
                    break;
                default:
                    list.push_back(Thread{p, st});
                    break;
            }
        }
    }
};

struct DecodedText {
    std::vector<char32_t> codepoints;
    std::vector<std::size_t> byteOffsets;  // 大小為 codepoints.size()+1，末項是 text.size()
};

DecodedText decodeText(const std::string& text) {
    DecodedText out;
    out.codepoints.reserve(text.size());
    out.byteOffsets.reserve(text.size() + 1);

    std::size_t i = 0;
    while (i < text.size()) {
        out.byteOffsets.push_back(i);
        const auto byte = static_cast<unsigned char>(text[i]);
        char32_t cp = 0;
        std::size_t len = 1;
        if (byte < 0x80) {
            cp = byte;
        } else if ((byte & 0xE0) == 0xC0) {
            cp = byte & 0x1Fu;
            len = 2;
        } else if ((byte & 0xF0) == 0xE0) {
            cp = byte & 0x0Fu;
            len = 3;
        } else if ((byte & 0xF8) == 0xF0) {
            cp = byte & 0x07u;
            len = 4;
        } else {
            cp = 0xFFFD;
            len = 1;
        }
        if (len > 1) {
            if (i + len > text.size()) {
                cp = 0xFFFD;
                len = 1;
            } else {
                bool okSeq = true;
                char32_t value = cp;
                for (std::size_t k = 1; k < len; ++k) {
                    const auto cont = static_cast<unsigned char>(text[i + k]);
                    if ((cont & 0xC0) != 0x80) {
                        okSeq = false;
                        break;
                    }
                    value = (value << 6) | (cont & 0x3Fu);
                }
                if (okSeq) {
                    cp = value;
                } else {
                    cp = 0xFFFD;
                    len = 1;
                }
            }
        }
        out.codepoints.push_back(cp);
        i += len;
    }
    out.byteOffsets.push_back(text.size());
    return out;
}

}  // namespace

struct BoundedRegex::Program {
    std::vector<Inst> insts;
    bool caseInsensitive{false};
};

BoundedRegex BoundedRegex::compile(const std::string& pattern, RegexOptions options) {
    BoundedRegex re;
    if (pattern.empty()) {
        re.diagnostic_ = "查詢樣式為空";
        return re;
    }

    const DecodedText decoded = decodeText(pattern);
    Parser parser(decoded.codepoints);
    std::string err;
    NodePtr root = parser.parse(&err);
    if (!root) {
        re.diagnostic_ = err;
        return re;
    }

    auto program = std::make_shared<Program>();
    program->caseInsensitive = options.caseInsensitive;
    Compiler compiler;
    if (!compiler.compile(*root, &program->insts, &err)) {
        re.diagnostic_ = err;
        return re;
    }

    re.valid_ = true;
    re.program_ = program;
    return re;
}

std::vector<RegexMatch> BoundedRegex::findAll(const std::string& text) const {
    std::vector<RegexMatch> out;
    if (!valid_ || text.empty()) return out;

    const DecodedText decoded = decodeText(text);
    const Vm vm(program_->insts);

    std::size_t pos = 0;
    while (pos <= decoded.codepoints.size()) {
        std::size_t s = 0;
        std::size_t e = 0;
        if (!vm.run(decoded.codepoints, pos, program_->caseInsensitive, &s, &e)) break;

        RegexMatch m;
        m.byteOffset = decoded.byteOffsets[s];
        m.byteLength = decoded.byteOffsets[e] - decoded.byteOffsets[s];
        out.push_back(m);

        // 零寬命中（例如純錨點樣式 ^ 或 $）要往前挪一個碼點，否則會原地卡死。
        pos = (e > s) ? e : e + 1;
    }
    return out;
}

bool BoundedRegex::findFirst(const std::string& text, std::size_t fromByteOffset,
                             RegexMatch* match) const {
    if (!valid_ || match == nullptr || text.empty()) return false;

    const DecodedText decoded = decodeText(text);
    std::size_t startCp = 0;
    while (startCp < decoded.codepoints.size() && decoded.byteOffsets[startCp] < fromByteOffset) {
        ++startCp;
    }

    const Vm vm(program_->insts);
    std::size_t s = 0;
    std::size_t e = 0;
    if (!vm.run(decoded.codepoints, startCp, program_->caseInsensitive, &s, &e)) return false;

    match->byteOffset = decoded.byteOffsets[s];
    match->byteLength = decoded.byteOffsets[e] - decoded.byteOffsets[s];
    return true;
}

}  // namespace alioth::engine::text
