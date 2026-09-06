#include "app/print/page_range.h"

#include <QStringList>

#include <algorithm>
#include <unordered_set>

namespace alioth::app::print {
namespace {

// 回傳 -1 表示不是合法的十進位頁碼。不用 QString::toInt 的 ok 旗標是因為
// 它接受前後空白與正負號，「+3」「 -2 」都會被判為合法，而那些在範圍語法裡
// 有別的意義（連字號是區間分隔符）。
int parsePageToken(const QString& token) {
    if (token.isEmpty()) return -1;
    for (const QChar ch : token) {
        if (ch < QLatin1Char('0') || ch > QLatin1Char('9')) return -1;
    }
    bool ok = false;
    const long long value = token.toLongLong(&ok);
    if (!ok || value <= 0 || value > 1'000'000'000LL) return -1;
    return static_cast<int>(value);
}

bool keepUnderSubset(int pageNumber1Based, PageSubset subset) {
    switch (subset) {
        case PageSubset::All:
            return true;
        case PageSubset::Odd:
            return pageNumber1Based % 2 == 1;
        case PageSubset::Even:
            return pageNumber1Based % 2 == 0;
    }
    return true;
}

}  // namespace

PageRangeResult parsePageRange(const QString& spec, int pageCount, PageSubset subset) {
    PageRangeResult result;
    if (pageCount <= 0) {
        result.valid = false;
        result.diagnostic = QStringLiteral("文件沒有可列印的頁面");
        return result;
    }

    std::unordered_set<int> seen;
    const auto append = [&](int first1, int last1) {
        const int step = first1 <= last1 ? 1 : -1;
        for (int n = first1;; n += step) {
            if (n >= 1 && n <= pageCount && keepUnderSubset(n, subset)) {
                if (seen.insert(n).second) result.pages.push_back(n - 1);
            }
            if (n == last1) break;
        }
    };

    const QString trimmed = spec.trimmed();
    if (trimmed.isEmpty()) {
        append(1, pageCount);
        return result;
    }

    const QStringList parts = trimmed.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        result.valid = false;
        result.diagnostic = QStringLiteral("範圍字串沒有任何項目");
        return result;
    }

    for (const QString& rawPart : parts) {
        const QString part = rawPart.trimmed();
        if (part.isEmpty()) continue;

        const int dash = part.indexOf(QLatin1Char('-'));
        if (dash < 0) {
            const int page = parsePageToken(part);
            if (page < 0) {
                result.valid = false;
                result.diagnostic = QStringLiteral("無法解析的頁碼：%1").arg(part);
                return result;
            }
            append(page, page);
            continue;
        }

        const QString lhs = part.left(dash).trimmed();
        const QString rhs = part.mid(dash + 1).trimmed();
        if (lhs.isEmpty() && rhs.isEmpty()) {
            result.valid = false;
            result.diagnostic = QStringLiteral("區間缺少端點：%1").arg(part);
            return result;
        }

        const int first = lhs.isEmpty() ? 1 : parsePageToken(lhs);
        const int last = rhs.isEmpty() ? pageCount : parsePageToken(rhs);
        if (first < 0 || last < 0) {
            result.valid = false;
            result.diagnostic = QStringLiteral("無法解析的區間：%1").arg(part);
            return result;
        }
        append(first, last);
    }

    if (result.pages.empty()) {
        // 語法合法但選不到任何頁（例如只選奇數頁卻指定了偶數區間）。
        // 這不是解析錯誤，但讓列印靜默印出零張紙同樣是壞行為，所以標記為無效。
        result.valid = false;
        result.diagnostic = QStringLiteral("指定的範圍在此文件中沒有對應頁面");
    }
    return result;
}

std::vector<int> reversed(std::vector<int> pages) {
    std::reverse(pages.begin(), pages.end());
    return pages;
}

}  // namespace alioth::app::print
