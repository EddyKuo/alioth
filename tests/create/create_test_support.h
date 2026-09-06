#pragma once

// 四支建立 PDF 的測試共用的驗證輔助。
//
// 抽出來的理由不是省字，是讓「產出算不算通過」只有一份定義：能被 PDFium
// 開啟、頁數正確、qpdf --check 零警告。任何一支測試自行放寬其中一項，
// 都會讓整組測試對「合格」的判準悄悄產生分歧。

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QtTest>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "engine/pdfium_engine.h"
#include "qa/qpdf_check.h"

namespace alioth::test::create {

// 等待非同步結果。逾時視為失敗，不無限期掛住 CI。
template <typename T>
class Latch {
public:
    void set(T value) {
        {
            std::lock_guard lock(mutex_);
            value_ = std::move(value);
            ready_ = true;
        }
        cv_.notify_all();
    }

    [[nodiscard]] bool wait(int milliseconds = 15000) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, std::chrono::milliseconds(milliseconds),
                            [this] { return ready_; });
    }

    [[nodiscard]] const T& value() const { return value_; }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool ready_{false};
    T value_{};
};

inline QString writeBytes(const QString& directory, const QString& name,
                          const std::string& bytes) {
    QDir().mkpath(directory);
    const QString path = QDir(directory).filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    file.write(bytes.data(), static_cast<qint64>(bytes.size()));
    file.close();
    return path;
}

struct OpenedDocument {
    bool ok{false};
    int pageCount{0};
    QString detail;
    std::vector<alioth::domain::SizeF> pageSizes;
};

// 用 PdfiumEngine 開一次並取回每頁尺寸。這是「檔案真的能用」的證明——
// 我們自己產生它，再用我們自己的位元組斷言驗證它，等於自問自答。
inline OpenedDocument openWithPdfium(const QString& path) {
    OpenedDocument opened;
    alioth::engine::PdfiumEngine engine;

    Latch<alioth::engine::OpenResult> openLatch;
    engine.openDocument(path.toStdString(), "",
                        [&openLatch](alioth::engine::OpenResult r) { openLatch.set(std::move(r)); });
    if (!openLatch.wait()) {
        opened.detail = QStringLiteral("開檔逾時");
        return opened;
    }
    if (!openLatch.value().ok()) {
        opened.detail = QStringLiteral("PDFium 無法開啟，錯誤碼 %1")
                            .arg(static_cast<int>(openLatch.value().error));
        return opened;
    }

    opened.pageCount = openLatch.value().info.pageCount;
    for (int i = 0; i < opened.pageCount; ++i) {
        Latch<std::optional<alioth::domain::PageInfo>> infoLatch;
        engine.pageInfo(i, [&infoLatch](std::optional<alioth::domain::PageInfo> info) {
            infoLatch.set(std::move(info));
        });
        if (!infoLatch.wait() || !infoLatch.value().has_value()) {
            opened.detail = QStringLiteral("取第 %1 頁的尺寸失敗").arg(i);
            return opened;
        }
        opened.pageSizes.push_back(infoLatch.value()->sizePt);
    }

    Latch<bool> closeLatch;
    engine.closeDocument([&closeLatch] { closeLatch.set(true); });
    (void)closeLatch.wait();

    opened.ok = true;
    return opened;
}

}  // namespace alioth::test::create

// qpdf 不存在時 QSKIP 而不是通過：CI 上缺工具卻靜默綠燈，比紅燈更糟。
#define ALIOTH_REQUIRE_QPDF_CLEAN(path, label)                                          \
    do {                                                                                \
        const auto _result = alioth::test::runQpdfCheck(path);                          \
        if (_result.status == alioth::test::QpdfStatus::NotAvailable) {                 \
            QSKIP(alioth::test::qpdfSkipReason().constData());                          \
        }                                                                               \
        QVERIFY2(_result.clean(), alioth::test::describeQpdfFailure(label, _result).constData()); \
    } while (false)
