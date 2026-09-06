// SIG-003 端到端測試：已簽章文件加註解後，簽章驗證管線要怎麼回報（WP27）。
//
// 這支測試把兩個各自獨立測過的子系統接在一起：
//   app::AnnotationService（核心迴圈：選取 → 外觀串流 → 增量儲存）
//   engine::signature::SignatureScanner（/ByteRange 完整性 + PKCS#7 驗證）
//
// PRD-SIG-003 的字面驗收標準是「Acrobat 顯示簽章有效、簽章後有變更」。
// 那是 Acrobat 自己的判讀，我們控制不了、也不該在這裡假裝驗證得到。
// 這裡能驗、也必須驗的是我們自己這邊唯一能保證的三件事：
//
//   一、原始簽章涵蓋的位元組完全沒被改動（前綴逐位元組相同）。
//   二、密碼學驗證本身完全通過（digest 與簽章都對得上原始 /ByteRange）。
//   三、新增的內容被正確辨識為「不在簽章涵蓋範圍內」，不會被誤判成
//      涵蓋範圍內的一部分（這是 Incremental Saving Attack 防護的同一條邏輯，
//      見 tests/signature/test_signature_scanner.cpp 的
//      appendedContentIsReportedAsPartialCoverage）。
//
// 這裡刻意 *不* 斷言最終 trust 是 Trusted 或 Untrusted，而是把目前 classify()
// 的實際行為（partiallyCovered → 一律 Invalid，不分辨「我們自己的合法增量」
// 與「惡意的 Incremental Saving Attack」）原樣斷言下來，並在下面留言解釋這與
// PRD-SIG-003 字面驗收標準之間的落差。這是刻意的行為記錄，不是偷懶——
// 詳見 exceptions/EXC_20260906_RD_SA_sig003_trust_gap.md。

#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include <condition_variable>
#include <mutex>

#include "app/annotation_service.h"
#include "engine/signature/signature_scanner.h"
#include "signature/signature_fixture.h"

using namespace alioth;
using namespace alioth::engine::signature;
using alioth::test::Identity;

namespace {

constexpr long kYear = 365L * 24 * 3600;

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
    bool wait(int milliseconds = 20000) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, std::chrono::milliseconds(milliseconds),
                            [this] { return ready_; });
    }
    const T& value() const { return value_; }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool ready_{false};
    T value_{};
};

}  // namespace

class TestSignedIncrementalSave : public QObject {
    Q_OBJECT

private:
    Identity ca_;
    Identity leaf_;

    static bool openScanner(SignatureScanner& scanner, const QString& path) {
        Latch<domain::DocumentError> latch;
        scanner.open(path.toStdString(), {},
                    [&latch](domain::DocumentError error) { latch.set(error); });
        if (!latch.wait()) return false;
        return latch.value() == domain::DocumentError::None;
    }

    static std::vector<SignatureReport> scanWith(SignatureScanner& scanner,
                                                 const TrustStore& store) {
        Latch<std::vector<SignatureReport>> latch;
        VerifyOptions options;
        options.revocationPolicy = RevocationPolicy::Skip;
        scanner.scan(&store, options, nullptr,
                    [&latch](std::vector<SignatureReport> reports) {
                        latch.set(std::move(reports));
                    });
        if (!latch.wait()) return {};
        return latch.value();
    }

private slots:
    void initTestCase() {
        ca_ = alioth::test::makeIdentity("Alioth Test Root CA", nullptr, -kYear, kYear, 1, true);
        QVERIFY(ca_.valid());
        leaf_ = alioth::test::makeIdentity("Mia Legal", &ca_, -3600, kYear, 1001, false);
        QVERIFY(leaf_.valid());
    }

    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
    }

    void cleanup() { dir_.reset(); }

    void annotatingSignedDocumentPreservesOriginalSignatureBytes() {
        const auto signed_ = alioth::test::makeSignedPdf(leaf_, &ca_);
        QVERIFY2(signed_.ok, "簽章語料產生失敗，後面的斷言都沒有意義");

        const QString path = dir_->filePath(QStringLiteral("signed.pdf"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(signed_.bytes) == signed_.bytes.size());
        file.close();

        // 簽章語料的頁面是 200×300，內容畫了一個 20,20 到 180,120 的黑色矩形。
        // 這裡疊一則螢光筆註解上去，走的是核心迴圈的正式路徑（AnnotationService），
        // 不是為了測試另外搭一條寫入捷徑。
        app::HighlightRequest request;
        request.path = path;
        request.pageIndex = 0;
        request.quads = {domain::QuadPoint::fromRect(domain::RectF{30, 30, 150, 100})};
        request.author = QStringLiteral("Alioth QA");
        request.contents = QStringLiteral("SIG-003 e2e");

        app::AnnotationService service;
        const app::HighlightResult result = service.addHighlight(request);
        QVERIFY2(result.ok, qPrintable(result.message));

        QFile after(path);
        QVERIFY(after.open(QIODevice::ReadOnly));
        const QByteArray afterBytes = after.readAll();
        after.close();

        // 一、原檔的每一個位元組原封不動：這是簽章保全唯一的技術基礎。
        QVERIFY(afterBytes.size() > signed_.bytes.size());
        QCOMPARE(afterBytes.left(signed_.bytes.size()), signed_.bytes);

        SignatureScanner scanner;
        QVERIFY(openScanner(scanner, path));
        QCOMPARE(scanner.signatureCount(), 1);

        TrustStore store;
        QVERIFY(store.addCertificatePem(alioth::test::certificatePem(ca_)));
        const auto reports = scanWith(scanner, store);
        QCOMPARE(reports.size(), std::size_t{1});
        const SignatureReport& report = reports.front();

        // 二、密碼學驗證完全通過：原始 /ByteRange 涵蓋的位元組沒有被動過，
        // 附加的註解落在 /ByteRange 之外，不會影響摘要比對。
        QVERIFY(report.parsed);
        QVERIFY(report.digestMatches);
        QVERIFY(report.cryptographicallyValid);
        QVERIFY(report.chainTrusted);

        // 三、新增的內容被正確辨識為「不在簽章涵蓋範圍內」——這正是
        // Incremental Saving Attack 防護要偵測的訊號，我們自己的合法增量
        // 在位元組層面上與惡意附加無法區分，因此也會觸發同一條偵測。
        QVERIFY2(report.coverage.partiallyCovered(),
                 "新增的註解沒有被偵測為未涵蓋範圍，簽章保全的量測本身就是錯的");
        QVERIFY(report.coverage.uncoveredBytes() > 0);

        // 黃燈（見 EXC_20260906_RD_SA_sig003_trust_gap.md 的裁決）。
        //
        // 這一條的情境正是本產品的核心迴圈：使用者在已簽章的合約上加一個註解
        // 然後存檔。判成紅燈「無效」是對自家主要工作流程的誤報，而誤報會教會
        // 使用者忽略這個指示燈——那時真正的攻擊也不會被看見。
        //
        // 密碼學驗證通過、但有一段內容不在保護範圍內，正確的說法是「無法確認」。
        // 要真的分辨合法增量與增量儲存攻擊需要修改分析，那是還沒做的子系統。
        QCOMPARE(report.trust, SignatureTrust::Untrusted);
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
};

QTEST_APPLESS_MAIN(TestSignedIncrementalSave)
#include "test_signed_incremental_save.moc"
