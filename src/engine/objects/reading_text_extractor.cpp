#include "engine/objects/reading_text_extractor.h"

namespace alioth::engine::objects {
namespace {

[[nodiscard]] bool isHeadingType(const std::string& type) {
    if (type == "H") return true;
    return type.size() == 2 && type[0] == 'H' && type[1] >= '1' && type[1] <= '9';
}

[[nodiscard]] bool isTitleFallbackType(const std::string& type) {
    return isHeadingType(type) || type == "Table" || type == "Figure" || type == "Formula" ||
           type == "Caption";
}

void collectText(const StructNode& node, std::int32_t pageIndex, ReadingExtractionResult& result) {
    if (node.pageIndex == pageIndex) {
        std::string text;
        if (!node.actualText.empty()) {
            text = node.actualText;
        } else if (node.needsAlternateText()) {
            // Figure／Formula／Form／Link：沒有 /Alt 就是真的讀不出來，
            // 這正是 PRD-A11Y-004 要抓的缺陷，在朗讀情境下不能假裝沒事跳過。
            if (!node.altText.empty()) {
                text = node.altText;
            } else {
                ++result.skippedMissingAltCount;
            }
        } else if (!node.title.empty() && isTitleFallbackType(node.type)) {
            text = node.title;
        } else if (node.children.empty()) {
            // 葉節點、且三種來源都沒有：多半是一般段落，文字活在內容串流裡，
            // 本模組拿不到（見標頭說明）。只在葉節點計數——容器節點的「沒有自己的
            // 文字」是正常狀態，它的內容由子節點各自負責。
            ++result.skippedNoTextCount;
        }

        if (!text.empty()) {
            for (std::string& sentence : splitIntoSentences(text)) {
                ReadingUtterance utterance;
                utterance.text = std::move(sentence);
                utterance.sourceType = node.type;
                result.utterances.push_back(std::move(utterance));
            }
        }
    }

    for (const StructNode& child : node.children) collectText(child, pageIndex, result);
}

}  // namespace

std::vector<std::string> splitIntoSentences(const std::string& text) {
    // 全形句讀的 UTF-8 位元組序列：。！？。半形句點／驚嘆號／問號與換行則逐位元組比對即可。
    static const char* const kCjkTerminators[] = {
        "\xE3\x80\x82",  // 。 U+3002
        "\xEF\xBC\x81",  // ！ U+FF01
        "\xEF\xBC\x9F",  // ？ U+FF1F
    };

    std::vector<std::string> sentences;
    std::string current;

    const auto flush = [&]() {
        const std::size_t begin = current.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos) {
            current.clear();
            return;
        }
        const std::size_t end = current.find_last_not_of(" \t\r\n");
        sentences.push_back(current.substr(begin, end - begin + 1));
        current.clear();
    };

    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '.' || c == '!' || c == '?') {
            current.push_back(c);
            flush();
            ++i;
            continue;
        }
        if (c == '\n') {
            flush();
            ++i;
            continue;
        }

        bool matchedCjk = false;
        for (const char* terminator : kCjkTerminators) {
            const std::size_t length = std::char_traits<char>::length(terminator);
            if (text.compare(i, length, terminator) == 0) {
                current.append(terminator, length);
                flush();
                i += length;
                matchedCjk = true;
                break;
            }
        }
        if (matchedCjk) continue;

        current.push_back(c);
        ++i;
    }
    flush();

    return sentences;
}

ReadingExtractionResult extractReadingText(const StructTree& tree, std::int32_t pageIndex) {
    ReadingExtractionResult result;
    result.pageIndex = pageIndex;
    for (const StructNode& root : tree.roots) collectText(root, pageIndex, result);
    return result;
}

}  // namespace alioth::engine::objects
