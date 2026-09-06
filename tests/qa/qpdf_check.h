#pragma once

// qpdf 結構檢查（PRD-IO-001 的明列驗收條件、PRD §9 的單元測試欄）。
//
// 為什麼需要它：我們自己的測試是用 PDFium 把檔案讀回來驗。PDFium 對格式的容忍度很高，
// 壞掉的 xref、錯的 /Length、懸空的物件參照它多半照樣讀得出來，於是「讀得回來」
// 這件事完全不能證明產出的檔案結構是對的。qpdf --check 是獨立於 PDFium 的第二意見，
// 這正是 PRD 指定它的理由。Acrobat 是最終裁決者，qpdf 是能自動化的那一關。
//
// 判定標準是零警告，不只是零錯誤。qpdf 的退出碼 3 代表「只有警告」，
// 而警告的典型內容是「物件 X 的 /Length 不對，已自行修正」——
// 那正是我們最需要被擋下來的一類缺陷：檢視器會幫忙修，簽章驗證不會。
//
// 工具不存在時一律 skip 而非 pass。CI 上缺工具卻靜默通過比紅燈更糟：
// 紅燈會有人修，綠燈沒有人會去看它到底驗了什麼。因此 skip 訊息必須寫清楚
// 找過哪些位置、以及怎麼把工具裝回來。

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

namespace alioth::test {

enum class QpdfStatus {
    NotAvailable,  // 找不到 qpdf，測試應 skip
    RunFailed,     // 找到了但跑不起來（權限、缺 DLL）——這是失敗，不是 skip
    Clean,         // 零錯誤零警告
    Warnings,      // 退出碼 3 或輸出含 WARNING
    Errors,        // 退出碼 2
};

struct QpdfCheckResult {
    QpdfStatus status{QpdfStatus::NotAvailable};
    int exitCode{-1};
    QString output;
    QString executable;

    [[nodiscard]] bool clean() const noexcept { return status == QpdfStatus::Clean; }
};

// 依序嘗試：環境變數 ALIOTH_QPDF（讓 CI 指定自己的版本）→ 建置時設定的路徑
// （third_party/qpdf，見 tests/qa/CMakeLists.txt）→ PATH。找不到回傳空字串。
inline QString findQpdf() {
    const QString fromEnv = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("ALIOTH_QPDF"));
    if (!fromEnv.isEmpty() && QFileInfo::exists(fromEnv)) return fromEnv;

#ifdef ALIOTH_QPDF_EXECUTABLE
    const QString configured = QStringLiteral(ALIOTH_QPDF_EXECUTABLE);
    if (!configured.isEmpty() && QFileInfo::exists(configured)) return configured;
#endif

    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("qpdf"));
    if (!onPath.isEmpty()) return onPath;

    return {};
}

// 找不到 qpdf 時給測試用的 skip 說明。刻意列出所有找過的位置與取得方式，
// 讓看到 skip 的人不需要翻原始碼就知道下一步該做什麼。
inline QByteArray qpdfSkipReason() {
    QString text = QStringLiteral(
        "找不到 qpdf，跳過結構檢查。這不代表產出的 PDF 通過檢查，只代表沒有驗。\n"
        "  找過的位置：環境變數 ALIOTH_QPDF");
#ifdef ALIOTH_QPDF_EXECUTABLE
    text += QStringLiteral("、建置時設定的 ") + QStringLiteral(ALIOTH_QPDF_EXECUTABLE);
#else
    text += QStringLiteral("、（建置時未設定 third_party/qpdf 路徑）");
#endif
    text += QStringLiteral(
        "、系統 PATH\n"
        "  取得方式：執行 tools/qpdf/fetch_qpdf.bat，或自行把 qpdf.exe 放進 "
        "third_party/qpdf/bin/ 後重新 configure");
    return text.toUtf8();
}

// 對單一檔案執行 qpdf --check。
inline QpdfCheckResult runQpdfCheck(const QString& pdfPath) {
    QpdfCheckResult result;
    result.executable = findQpdf();
    if (result.executable.isEmpty()) return result;

    QProcess process;
    // qpdf 把錯誤寫 stderr、檢查摘要寫 stdout，兩者合併才不會漏掉半邊。
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(result.executable, QStringList{QStringLiteral("--check"), pdfPath});
    if (!process.waitForStarted(15000)) {
        result.status = QpdfStatus::RunFailed;
        result.output = QStringLiteral("qpdf 無法啟動：") + process.errorString();
        return result;
    }
    if (!process.waitForFinished(120000)) {
        process.kill();
        process.waitForFinished(5000);
        result.status = QpdfStatus::RunFailed;
        result.output = QStringLiteral("qpdf 逾時未結束");
        return result;
    }

    result.output = QString::fromLocal8Bit(process.readAll());
    result.exitCode = process.exitCode();

    if (process.exitStatus() != QProcess::NormalExit) {
        result.status = QpdfStatus::RunFailed;
        return result;
    }

    // 退出碼：0 = 乾淨、2 = 有錯誤、3 = 只有警告。
    // 輸出裡的 WARNING 一併檢查，因為 qpdf 的部分警告不影響退出碼。
    if (result.exitCode == 2) {
        result.status = QpdfStatus::Errors;
    } else if (result.exitCode == 3 || result.output.contains(QStringLiteral("WARNING"))) {
        result.status = QpdfStatus::Warnings;
    } else if (result.exitCode == 0) {
        result.status = QpdfStatus::Clean;
    } else {
        result.status = QpdfStatus::RunFailed;
    }
    return result;
}

// 把位元組寫成暫存檔再檢查。AnnotationDocument::saveIncremental 直接回傳記憶體中的
// 位元組，落檔只是為了餵給 qpdf。
inline QpdfCheckResult runQpdfCheckOnBytes(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QpdfCheckResult failed;
        failed.status = QpdfStatus::RunFailed;
        failed.output = QStringLiteral("無法寫入暫存檔 ") + path;
        return failed;
    }
    file.write(bytes);
    file.close();
    return runQpdfCheck(path);
}

// 失敗時給 QVERIFY2 用的訊息。包含 qpdf 的完整輸出——只說「檢查失敗」的訊息
// 等於要人重跑一次才知道發生什麼事。
inline QByteArray describeQpdfFailure(const QString& label, const QpdfCheckResult& result) {
    QString text = label + QStringLiteral(" 未通過 qpdf --check（退出碼 %1）\n")
                               .arg(result.exitCode);
    text += QStringLiteral("  qpdf: ") + result.executable + QLatin1Char('\n');
    text += result.output;
    return text.toUtf8();
}

}  // namespace alioth::test
