#include "engine/compare/spellcheck.h"

#include <algorithm>
#include <string_view>
#include <utility>

#include "engine/compare/utf8_scan.h"

namespace alioth::engine::compare {
namespace {

[[nodiscard]] bool isAsciiLetter(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// 編輯距離 1 的所有候選（含許多根本不是詞的雜訊字串，交由字典過濾）。
// 只在 26 個 ASCII 字母上生成：字典本身也只收 ASCII 詞，生成非 ASCII 候選
// 沒有意義，還會白白拉長候選清單。
std::vector<std::string> editDistance1(const std::string& lowerWord) {
    static constexpr std::string_view kAlphabet = "abcdefghijklmnopqrstuvwxyz";
    std::vector<std::string> out;
    const std::size_t n = lowerWord.size();
    out.reserve(n * 2 + n * kAlphabet.size() + (n + 1) * kAlphabet.size());

    // 刪除一個字元。
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(lowerWord.substr(0, i) + lowerWord.substr(i + 1));
    }
    // 相鄰兩字元換位——這是打字最常見的錯誤形態之一，單獨列一類比純替換更精準。
    for (std::size_t i = 0; i + 1 < n; ++i) {
        std::string s = lowerWord;
        std::swap(s[i], s[i + 1]);
        out.push_back(std::move(s));
    }
    // 替換一個字元。
    for (std::size_t i = 0; i < n; ++i) {
        for (const char c : kAlphabet) {
            if (c == lowerWord[i]) continue;
            std::string s = lowerWord;
            s[i] = c;
            out.push_back(std::move(s));
        }
    }
    // 插入一個字元（含頭尾）。
    for (std::size_t i = 0; i <= n; ++i) {
        for (const char c : kAlphabet) {
            out.push_back(lowerWord.substr(0, i) + c + lowerWord.substr(i));
        }
    }
    return out;
}

// 見檔頭「字典來源」：這份清單只是常見英文功能詞加上本產品自己的領域詞彙，
// 存在的理由是讓分詞／建議／自訂詞／忽略清單這條管線可以被測試，不是拿來上線用的。
const char* const kSampleWords[] = {
    "the", "a", "an", "and", "or", "but", "if", "then", "else", "for", "to", "of", "in", "on",
    "at", "by", "with", "from", "is", "are", "was", "were", "be", "been", "being", "this", "that",
    "these", "those", "it", "its", "as", "not", "no", "yes", "please", "review", "reviewer",
    "document", "documents", "page", "pages", "text", "file", "files", "comment", "comments",
    "annotation", "annotations", "highlight", "highlighted", "note", "notes", "sticky", "stamp",
    "signature", "signed", "sign", "approve", "approved", "approval", "reject", "rejected",
    "revise", "revision", "revised", "update", "updated", "final", "draft", "version", "attach",
    "attachment", "attachments", "bookmark", "bookmarks", "outline", "form", "field", "fields",
    "checkbox", "table", "cell", "cells", "column", "columns", "row", "rows", "search", "find",
    "replace", "match", "matches", "case", "sensitive", "whole", "word", "words", "regular",
    "expression", "pattern", "hello", "world", "second", "third", "line", "lines", "beta",
    "unique", "marker", "alioth", "engineering", "drawing", "project", "team", "member",
    "meeting", "schedule", "deadline", "budget", "report", "summary", "detail", "details",
    "please", "thanks", "thank", "you", "correct", "incorrect", "error", "errors", "warning",
    "warnings", "success", "failure", "test", "tests", "testing", "quality", "assurance",
    "workstation", "engine", "render", "rendering", "layer", "layers", "zoom", "scroll",
    "navigation", "print", "printing", "export", "import", "convert", "conversion", "spelling",
    "dictionary", "custom", "ignore", "ignored", "suggestion", "suggestions", "language",
};

}  // namespace

InMemoryDictionary::InMemoryDictionary(std::unordered_set<std::string> words)
    : words_(std::move(words)) {}

bool InMemoryDictionary::contains(const std::string& lowerWord) const {
    return words_.find(lowerWord) != words_.end();
}

void InMemoryDictionary::addWord(const std::string& word) { words_.insert(foldAscii(word)); }

InMemoryDictionary makeSampleDictionary() {
    std::unordered_set<std::string> words;
    for (const char* w : kSampleWords) words.insert(foldAscii(std::string(w)));
    return InMemoryDictionary{std::move(words)};
}

std::vector<SpellToken> tokenizeForSpelling(const std::string& text) {
    std::vector<SpellToken> tokens;
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        if (!isAsciiLetter(text[i])) {
            ++i;
            continue;
        }
        const std::size_t start = i;
        while (i < n) {
            if (isAsciiLetter(text[i])) {
                ++i;
                continue;
            }
            // 詞中的撇號／連字號（don't、co-worker）只在後面緊接字母時才吃進來，
            // 否則它是句子的標點（結尾的引號、連接子句的破折號），不屬於詞本身。
            if ((text[i] == '\'' || text[i] == '-') && i + 1 < n && isAsciiLetter(text[i + 1])) {
                ++i;
                continue;
            }
            break;
        }
        std::size_t end = i;
        // 防禦性收尾：上面的迴圈條件已經保證不會停在裸的 ' 或 - 上，這裡是
        // 為了讓「迴圈條件將來被誰改壞」的失敗模式是少切一點文字，而不是把
        // 標點也當成詞的一部分送給字典去查。
        while (end > start && (text[end - 1] == '\'' || text[end - 1] == '-')) --end;
        if (end > start) {
            tokens.push_back(SpellToken{text.substr(start, end - start), start, end - start});
        }
    }
    return tokens;
}

SpellCheckSession::SpellCheckSession(const SpellDictionary& dictionary) : dictionary_(dictionary) {}

void SpellCheckSession::addCustomWord(const std::string& word) {
    customWords_.insert(foldAscii(word));
}

void SpellCheckSession::ignoreWord(const std::string& word) { ignoredWords_.insert(foldAscii(word)); }

bool SpellCheckSession::isKnown(const std::string& word) const {
    const std::string lower = foldAscii(word);
    if (ignoredWords_.find(lower) != ignoredWords_.end()) return true;
    if (customWords_.find(lower) != customWords_.end()) return true;
    return dictionary_.contains(lower);
}

std::vector<std::string> SpellCheckSession::suggest(const std::string& word,
                                                     std::size_t maxSuggestions) const {
    const std::string lower = foldAscii(word);
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (std::string& candidate : editDistance1(lower)) {
        if (!seen.insert(candidate).second) continue;
        if (dictionary_.contains(candidate)) {
            out.push_back(std::move(candidate));
            if (out.size() >= maxSuggestions) break;
        }
    }
    return out;
}

std::vector<SpellIssue> SpellCheckSession::checkText(const std::string& text) const {
    std::vector<SpellIssue> issues;
    for (const SpellToken& token : tokenizeForSpelling(text)) {
        if (isKnown(token.text)) continue;
        SpellIssue issue;
        issue.byteOffset = token.byteOffset;
        issue.byteLength = token.byteLength;
        issue.word = token.text;
        issue.suggestions = suggest(token.text);
        issues.push_back(std::move(issue));
    }
    return issues;
}

std::vector<SpellIssue> SpellCheckSession::checkDocument(
    const objects::PdfSourceDocument& source) const {
    std::vector<SpellIssue> issues;
    const std::vector<EditableText> targets = collectEditableText(source);
    for (std::size_t i = 0; i < targets.size(); ++i) {
        for (SpellIssue issue : checkText(targets[i].text)) {
            issue.targetIndex = i;
            issues.push_back(std::move(issue));
        }
    }
    return issues;
}

}  // namespace alioth::engine::compare
