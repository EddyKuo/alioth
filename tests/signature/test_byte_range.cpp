// /ByteRange 完整性檢查與三態分類的測試（WBS 6.5 / 6.7，PRD-SIG-001）。
//
// 這一組完全是純函數，不開 PDF 也不碰 OpenSSL。惡意 /ByteRange 的每一種形狀
// 都要能單獨餵進來——那些輸入在真實檔案裡很難造，但在這裡只是一個陣列。

#include <QtTest>

#include "engine/signature/byte_range.h"
#include "engine/signature/signature_types.h"

using namespace alioth::engine::signature;

namespace {

// 一份「涵蓋完整」的正常簽章：檔案 1000 位元組，簽章值佔 [200, 300)。
std::vector<int> normalRange() { return {0, 200, 300, 700}; }

SignatureReport validReport() {
    SignatureReport report;
    report.coverage = checkByteRange(normalRange(), 1000);
    report.contentsPresent = true;
    report.parsed = true;
    report.digestMatches = true;
    report.cryptographicallyValid = true;
    report.chainTrusted = true;
    report.revocation = RevocationStatus::Good;
    return report;
}

}  // namespace

class TestByteRange : public QObject {
    Q_OBJECT

private slots:
    void completeCoverageIsValid() {
        const ByteRangeCheck check = checkByteRange(normalRange(), 1000);
        QCOMPARE(check.verdict, ByteRangeVerdict::Valid);
        QVERIFY(check.ok());
        QCOMPARE(check.coveredBytes, std::int64_t{900});
        QCOMPARE(check.uncoveredBytes(), std::int64_t{100});
        // 唯一的空洞就是簽章值本身。
        QCOMPARE(check.gaps.size(), std::size_t{1});
        QCOMPARE(check.gaps[0].offset, std::int64_t{200});
        QCOMPARE(check.gaps[0].length, std::int64_t{100});
    }

    void oddElementCountIsMalformed() {
        QCOMPARE(checkByteRange({0, 200, 300}, 1000).verdict, ByteRangeVerdict::Malformed);
        QCOMPARE(checkByteRange({}, 1000).verdict, ByteRangeVerdict::Malformed);
    }

    void negativeValuesAreMalformed() {
        QCOMPARE(checkByteRange({-1, 200, 300, 700}, 1000).verdict, ByteRangeVerdict::Malformed);
        QCOMPARE(checkByteRange({0, -200, 300, 700}, 1000).verdict, ByteRangeVerdict::Malformed);
    }

    void outOfBoundsIsRejected() {
        const ByteRangeCheck check = checkByteRange({0, 200, 300, 5000}, 1000);
        QCOMPARE(check.verdict, ByteRangeVerdict::OutOfBounds);
        QVERIFY(!check.detail.empty());
    }

    void overlappingSegmentsAreRejected() {
        // 第二段從 100 起，落在第一段 [0,200) 內。重疊會讓同一段位元組
        // 被算兩次雜湊，攻擊者可以藉此構造出兩份不同檔案的相同摘要輸入。
        QCOMPARE(checkByteRange({0, 200, 100, 700}, 1000).verdict, ByteRangeVerdict::Overlapping);
        // 未依位移遞增同樣視為重疊：串出來的位元組順序與閱讀器不一致。
        QCOMPARE(checkByteRange({300, 700, 0, 200}, 1000).verdict, ByteRangeVerdict::Overlapping);
    }

    void notStartingAtZeroIsIncomplete() {
        const ByteRangeCheck check = checkByteRange({10, 190, 300, 700}, 1000);
        QCOMPARE(check.verdict, ByteRangeVerdict::IncompleteCoverage);
        QVERIFY(check.partiallyCovered());
    }

    void trailingUncoveredBytesAreIncomplete() {
        // 這是 Incremental Saving Attack 的形狀：簽章涵蓋前半段，
        // 攻擊者把內容附加在後面。密碼學驗證會完全通過。
        const ByteRangeCheck check = checkByteRange({0, 200, 300, 600}, 1000);
        QCOMPARE(check.verdict, ByteRangeVerdict::IncompleteCoverage);
        QVERIFY(check.partiallyCovered());
        QCOMPARE(check.uncoveredBytes(), std::int64_t{200});
        QVERIFY(check.detail.find("部分") != std::string::npos);
    }

    void multipleGapsAreIncomplete() {
        const ByteRangeCheck check = checkByteRange({0, 100, 200, 100, 400, 600}, 1000);
        QCOMPARE(check.verdict, ByteRangeVerdict::IncompleteCoverage);
        QVERIFY(check.gaps.size() >= 2);
    }

    void assemblyRefusesUnsafeRanges() {
        const std::vector<std::uint8_t> data(1000, 0x41);

        const ByteRangeCheck good = checkByteRange(normalRange(), 1000);
        QCOMPARE(assembleSignedBytes(data.data(), data.size(), good).size(), std::size_t{900});

        // 重疊 / 越界的情況下串出位元組會產生一個看似能驗、實際毫無意義的摘要。
        for (const std::vector<int>& raw :
             {std::vector<int>{0, 200, 100, 700}, std::vector<int>{0, 200, 300, 5000}}) {
            const ByteRangeCheck bad = checkByteRange(raw, 1000);
            QVERIFY(assembleSignedBytes(data.data(), data.size(), bad).empty());
        }
    }

    void wholeInputCoverageHelper() {
        const ByteRangeCheck check = coverageOfWholeInput(42);
        QVERIFY(check.ok());
        QCOMPARE(check.coveredBytes, std::int64_t{42});
        QCOMPARE(coverageOfWholeInput(0).verdict, ByteRangeVerdict::Malformed);
    }

    void classificationIsGreenOnlyWhenEverythingPasses() {
        SignatureReport report = validReport();
        QCOMPARE(classify(report, RevocationPolicy::SoftFail), SignatureTrust::Trusted);
    }

    void untrustedRootIsYellowNotGreen() {
        SignatureReport report = validReport();
        report.chainTrusted = false;
        // 自簽憑證在密碼學上完全有效。把它算成綠燈等於讓任何人
        // 都能偽造出一份看起來被認證過的合約。
        QCOMPARE(classify(report, RevocationPolicy::SoftFail), SignatureTrust::Untrusted);
    }

    void expiredCertificateIsYellowNotGreen() {
        SignatureReport report = validReport();
        report.certificateExpired = true;
        QCOMPARE(classify(report, RevocationPolicy::SoftFail), SignatureTrust::Untrusted);
    }

    void unknownRevocationDependsOnPolicy() {
        SignatureReport report = validReport();
        report.revocation = RevocationStatus::NotChecked;
        QCOMPARE(classify(report, RevocationPolicy::SoftFail), SignatureTrust::Untrusted);
        // 使用者明確選擇離線驗證時不因未查而降級，但那是使用者的決定，不是預設。
        QCOMPARE(classify(report, RevocationPolicy::Skip), SignatureTrust::Trusted);
        QCOMPARE(classify(report, RevocationPolicy::HardFail), SignatureTrust::Invalid);
    }

    void revokedCertificateIsRed() {
        SignatureReport report = validReport();
        report.revocation = RevocationStatus::Revoked;
        QCOMPARE(classify(report, RevocationPolicy::Skip), SignatureTrust::Invalid);
    }

    void partialCoverageIsAmberNotGreenAndNotRed() {
        SignatureReport report = validReport();
        report.coverage = checkByteRange({0, 200, 300, 600}, 1000);
        QVERIFY(report.digestMatches);
        QVERIFY(report.cryptographicallyValid);
        QVERIFY(report.chainTrusted);
        // 密碼學全對，但涵蓋範圍不完整。
        //
        // 不得是綠燈：檔案裡有一段不受簽章保護，那可能是增量儲存攻擊。
        // 也不該是紅燈：紅燈代表這份簽章無效，而它並不無效。正確的說法是
        // 「無法確認」——我們分不出那段附加內容是使用者自己加的註解還是偽造，
        // 要分辨需要修改分析（見 EXC_20260906_RD_SA_sig003_trust_gap 的裁決）。
        QCOMPARE(classify(report, RevocationPolicy::Skip), SignatureTrust::Untrusted);
    }

    // /ByteRange 本身壞掉與「合法但只涵蓋部分檔案」是兩件事。前者代表這份
    // 簽章無法解讀，仍然是紅燈——上面那條裁決沒有放寬這一種。
    void malformedByteRangeStaysRed() {
        SignatureReport report = validReport();
        report.coverage = checkByteRange({0, 200, 100, 600}, 1000);  // 區段重疊
        QVERIFY(!report.coverage.ok());
        QVERIFY(!report.coverage.partiallyCovered());
        QCOMPARE(classify(report, RevocationPolicy::Skip), SignatureTrust::Invalid);
    }

    void tamperedDigestIsRed() {
        SignatureReport report = validReport();
        report.digestMatches = false;
        QCOMPARE(classify(report, RevocationPolicy::Skip), SignatureTrust::Invalid);
    }

    void finalizeExplainsWhy() {
        SignatureReport report = validReport();
        report.chainTrusted = false;
        finalize(report, RevocationPolicy::Skip);
        QCOMPARE(report.trust, SignatureTrust::Untrusted);
        // 黃燈必須說得出原因，否則使用者只看得到一個沒有解釋的警告。
        QVERIFY(!report.findings.empty());
        bool mentionsTrust = false;
        for (const auto& finding : report.findings) {
            if (finding.find("受信任") != std::string::npos) mentionsTrust = true;
        }
        QVERIFY(mentionsTrust);
    }

    void finalizeReportsPartialCoverageExplicitly() {
        SignatureReport report = validReport();
        report.coverage = checkByteRange({0, 200, 300, 600}, 1000);
        finalize(report, RevocationPolicy::Skip);
        QCOMPARE(report.trust, SignatureTrust::Untrusted);
        bool mentionsPartial = false;
        for (const auto& finding : report.findings) {
            if (finding.find("只涵蓋部分檔案") != std::string::npos) mentionsPartial = true;
        }
        QVERIFY2(mentionsPartial, "涵蓋不完整必須明說，不能只丟一個燈號了事");
    }
};

QTEST_APPLESS_MAIN(TestByteRange)
#include "test_byte_range.moc"
