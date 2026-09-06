#pragma once

// 內容串流的語法檢查器，僅供測試使用。
//
// 外觀串流的錯誤幾乎都是「安靜」的：運算元少一個、q 沒有配對的 Q、寫出了
// 不存在的運算子——Acrobat 多半會盡力顯示，於是缺陷要到跨檢視器比對時才爆。
// 這裡用一個極小的分詞器把這些錯誤攔在單元測試層。
//
// 它刻意不是完整的 PDF 剖析器：外觀串流由我們自己產生，語法子集是已知且封閉的，
// 支援未出現過的運算子只會讓檢查變鬆。

#include <QByteArray>
#include <QList>
#include <QString>

#include <optional>

namespace alioth::test {

struct ContentStreamReport {
    bool valid{false};
    QString error{};
    int maxSaveDepth{0};
    QList<QString> operators{};
    QList<double> numbers{};

    [[nodiscard]] int countOperator(const QString& op) const {
        int n = 0;
        for (const QString& o : operators) {
            if (o == op) ++n;
        }
        return n;
    }
};

// 各運算子需要的運算元數量。未列出者一律視為錯誤。
//
// BT/ET/W/Tj/Td/Tf 是 WP24（FreeText 家族：Text Box／Typewriter／Callout）
// 新增的文字運算子集合，之前的註解外觀從不畫文字，這裡是它們第一次出現。
inline std::optional<int> operatorArity(const QString& op) {
    if (op == QLatin1String("q") || op == QLatin1String("Q") || op == QLatin1String("h") ||
        op == QLatin1String("f") || op == QLatin1String("S") || op == QLatin1String("B") ||
        op == QLatin1String("n") || op == QLatin1String("f*") || op == QLatin1String("B*") ||
        op == QLatin1String("BT") || op == QLatin1String("ET") || op == QLatin1String("W")) {
        return 0;
    }
    if (op == QLatin1String("w") || op == QLatin1String("J") || op == QLatin1String("j") ||
        op == QLatin1String("gs") || op == QLatin1String("Tj")) {
        return 1;
    }
    if (op == QLatin1String("m") || op == QLatin1String("l") || op == QLatin1String("Td") ||
        op == QLatin1String("Tf")) {
        return 2;
    }
    if (op == QLatin1String("RG") || op == QLatin1String("rg")) return 3;
    if (op == QLatin1String("re")) return 4;
    if (op == QLatin1String("c")) return 6;
    return std::nullopt;
}

inline ContentStreamReport checkContentStream(const QByteArray& content) {
    ContentStreamReport report{};
    const QList<QByteArray> tokens = content.simplified().split(' ');

    int operands = 0;
    int depth = 0;
    for (const QByteArray& raw : tokens) {
        if (raw.isEmpty()) continue;
        const QString token = QString::fromLatin1(raw);

        if (token.startsWith(QLatin1Char('/'))) {
            ++operands;
            continue;
        }
        // 常值字串運算元（Tj 的參數）。這個分詞器只用空格切詞，因此只認得
        // 「整個字串裡沒有空白」的 token——WP24 的測試文字刻意只用不含空白的
        // 單字，讓這個已知限制不影響驗證，不在這裡實作完整的括號配對剖析。
        if (token.startsWith(QLatin1Char('(')) && token.endsWith(QLatin1Char(')'))) {
            ++operands;
            continue;
        }
        bool isNumber = false;
        const double value = token.toDouble(&isNumber);
        if (isNumber) {
            ++operands;
            report.numbers.append(value);
            continue;
        }

        const std::optional<int> arity = operatorArity(token);
        if (!arity.has_value()) {
            report.error = QStringLiteral("未知的運算子：%1").arg(token);
            return report;
        }
        if (operands != *arity) {
            report.error = QStringLiteral("運算子 %1 需要 %2 個運算元，實得 %3")
                               .arg(token)
                               .arg(*arity)
                               .arg(operands);
            return report;
        }
        operands = 0;
        report.operators.append(token);

        if (token == QLatin1String("q")) {
            ++depth;
            report.maxSaveDepth = std::max(report.maxSaveDepth, depth);
        } else if (token == QLatin1String("Q")) {
            --depth;
            if (depth < 0) {
                report.error = QStringLiteral("Q 多於 q，圖形狀態堆疊被彈空");
                return report;
            }
        }
    }

    if (operands != 0) {
        report.error = QStringLiteral("串流結尾殘留 %1 個運算元").arg(operands);
        return report;
    }
    if (depth != 0) {
        report.error = QStringLiteral("q 與 Q 未配對，結尾深度為 %1").arg(depth);
        return report;
    }

    report.valid = true;
    return report;
}

}  // namespace alioth::test
