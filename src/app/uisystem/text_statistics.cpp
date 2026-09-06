#include "app/uisystem/text_statistics.h"

namespace alioth::app {

namespace {

bool isCjkScript(const QChar& ch) {
    switch (ch.script()) {
        case QChar::Script_Han:
        case QChar::Script_Hiragana:
        case QChar::Script_Katakana:
        case QChar::Script_Hangul:
        case QChar::Script_Bopomofo:
            return true;
        default:
            return false;
    }
}

}  // namespace

TextStatistics computeTextStatistics(const QString& text) {
    TextStatistics stats;
    bool inLatinWord = false;

    for (const QChar ch : text) {
        const bool isSpace = ch.isSpace();
        if (!isSpace) {
            ++stats.characterCountWithSpaces;
            ++stats.characterCountNoSpaces;
        } else if (ch != QChar(u'\n') && ch != QChar(u'\r')) {
            // 換行不算「可見字元」；一般空白／全形空白算，比照多數字數統計工具。
            ++stats.characterCountWithSpaces;
        }

        if (isCjkScript(ch)) {
            // 每個 CJK 表意文字獨立成詞——沒有空白可斷，逐字算是唯一合理的辦法。
            ++stats.wordCount;
            ++stats.cjkCharacterCount;
            inLatinWord = false;
            continue;
        }

        const bool isWordChar = ch.isLetterOrNumber();
        if (isWordChar) {
            if (!inLatinWord) {
                ++stats.wordCount;
                ++stats.latinWordCount;
                inLatinWord = true;
            }
        } else {
            inLatinWord = false;
        }
    }
    return stats;
}

TextStatistics operator+(const TextStatistics& a, const TextStatistics& b) {
    TextStatistics sum;
    sum.characterCountWithSpaces = a.characterCountWithSpaces + b.characterCountWithSpaces;
    sum.characterCountNoSpaces = a.characterCountNoSpaces + b.characterCountNoSpaces;
    sum.wordCount = a.wordCount + b.wordCount;
    sum.cjkCharacterCount = a.cjkCharacterCount + b.cjkCharacterCount;
    sum.latinWordCount = a.latinWordCount + b.latinWordCount;
    return sum;
}

}  // namespace alioth::app
