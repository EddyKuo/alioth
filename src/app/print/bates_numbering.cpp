#include "app/print/bates_numbering.h"

#include <QSet>

#include <algorithm>

namespace alioth::app::print {

long long batesValueAt(const BatesOptions& options, long long ordinal) noexcept {
    if (ordinal < 0) ordinal = 0;
    // 用 __int128 級的保護沒有必要，但 long long 溢位在這裡會直接產生錯號碼，
    // 所以先把乘積夾在安全範圍內再相加。
    const long long increment = options.increment;
    if (increment != 0 && ordinal > (9'000'000'000'000'000LL / std::max<long long>(1, std::llabs(increment)))) {
        return options.startNumber;
    }
    return options.startNumber + increment * ordinal;
}

QString formatBatesNumber(const BatesOptions& options, long long ordinal) {
    const long long value = batesValueAt(options, ordinal);
    const bool negative = value < 0;
    // 取絕對值前先轉成無號，否則 LLONG_MIN 的相反數會溢位。
    const unsigned long long magnitude =
        negative ? (~static_cast<unsigned long long>(value) + 1ULL)
                 : static_cast<unsigned long long>(value);

    QString digits = QString::number(magnitude);
    const int width = std::clamp(options.digits, 0, kMaxBatesDigits);
    if (digits.size() < width) {
        digits = digits.rightJustified(width, QLatin1Char('0'));
    }
    if (negative) digits.prepend(QLatin1Char('-'));

    return options.prefix + digits + options.suffix;
}

std::vector<QString> batesSequence(const BatesOptions& options, const std::vector<int>& pages) {
    std::vector<QString> numbers;
    numbers.reserve(pages.size());
    for (std::size_t i = 0; i < pages.size(); ++i) {
        const long long ordinal = options.basis == BatesBasis::PrintSequence
                                      ? static_cast<long long>(i)
                                      : static_cast<long long>(pages[i]);
        numbers.push_back(formatBatesNumber(options, ordinal));
    }
    return numbers;
}

bool isBatesSequenceUnique(const std::vector<QString>& numbers) {
    // 用 QSet 而不是 std::unordered_set：QString 的 std::hash 特化並非在所有
    // Qt 組態下都存在，而雜湊容器選型不值得為此加一層自訂 hasher。
    QSet<QString> seen;
    seen.reserve(static_cast<qsizetype>(numbers.size()));
    for (const QString& number : numbers) {
        if (seen.contains(number)) return false;
        seen.insert(number);
    }
    return true;
}

}  // namespace alioth::app::print
