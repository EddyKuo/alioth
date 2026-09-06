// 戳記對話框（PRD-PAGE-004）：頁首頁尾／浮水印／Bates 三種預設。
//
// 一個對話框三種預設，而不是三個對話框。這支測試守的就是那個決定的代價：
// 預設只決定「開啟時長什麼樣」，不能變成不可逾越的模式——否則「浮水印但
// 放在頁尾」這種需求就得去找別的功能。

#include <QtTest>

#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QSpinBox>

#include "ui/stamp_dialog.h"

using alioth::app::print::BatesBasis;
using alioth::app::print::StampAnchor;
using alioth::ui::StampDialog;

class TestStampDialog : public QObject {
    Q_OBJECT

private slots:
    void presetsDifferInPlacementAndText();
    void batesPresetTurnsBatesOnAndUsesTheToken();
    void batesFieldsAreDisabledUntilBatesIsEnabled();
    void presetsAreDefaultsNotModes();
    void writtenBatesFollowsDocumentPageNotPrintSequence();
    void emptyPageRangeMeansEveryPage();
    void invalidPageRangeIsReportedInsteadOfMeaningEveryPage();
};

void TestStampDialog::presetsDifferInPlacementAndText() {
    StampDialog header(StampDialog::Preset::HeaderFooter);
    StampDialog watermark(StampDialog::Preset::Watermark);

    const auto headerRequest = header.request(10);
    const auto watermarkRequest = watermark.request(10);

    QCOMPARE(headerRequest.anchor, StampAnchor::BottomCenter);
    QCOMPARE(watermarkRequest.anchor, StampAnchor::Center);
    // 浮水印預設是大字，頁首頁尾是小字。兩者一樣大的話浮水印在頁面上
    // 看起來只是一行不知道為什麼跑到中間的文字。
    QVERIFY(watermarkRequest.fontSize > headerRequest.fontSize * 2.0);
    QVERIFY(headerRequest.textTemplate.contains(QStringLiteral("<<Page>>")));
}

void TestStampDialog::batesPresetTurnsBatesOnAndUsesTheToken() {
    StampDialog dialog(StampDialog::Preset::Bates);
    const auto request = dialog.request(10);
    QVERIFY2(request.useBates, "Bates 預設沒有啟用編號——按下確定會蓋出一個空字串");
    QVERIFY(request.bates.enabled);
    QVERIFY2(request.textTemplate.contains(QStringLiteral("<<Bates>>")),
             "Bates 預設的文字裡沒有 <<Bates>> 符號，編號不會出現在頁面上");
}

void TestStampDialog::batesFieldsAreDisabledUntilBatesIsEnabled() {
    StampDialog dialog(StampDialog::Preset::HeaderFooter);
    auto* useBates = dialog.findChild<QCheckBox*>(QStringLiteral("stampUseBates"));
    auto* prefix = dialog.findChild<QLineEdit*>(QStringLiteral("stampBatesPrefix"));
    auto* digits = dialog.findChild<QSpinBox*>(QStringLiteral("stampBatesDigits"));
    QVERIFY(useBates != nullptr && prefix != nullptr && digits != nullptr);

    QVERIFY(!useBates->isChecked());
    QVERIFY2(!prefix->isEnabled(), "Bates 沒啟用卻能填前綴——填了不會有任何效果");

    useBates->setChecked(true);
    QVERIFY(prefix->isEnabled());
    QVERIFY(digits->isEnabled());

    // 位數上限來自 print::kMaxBatesDigits：再多只會產出讀不懂的字串，
    // 而且多半代表使用者把起始號填進了位數欄位。
    QCOMPARE(digits->maximum(), alioth::app::print::kMaxBatesDigits);
}

void TestStampDialog::presetsAreDefaultsNotModes() {
    // 「浮水印但放在頁尾」必須做得到。預設若變成模式，使用者得為了一個
    // 位置的差異去找別的功能。
    StampDialog dialog(StampDialog::Preset::Watermark);
    auto* anchor = dialog.findChild<QComboBox*>(QStringLiteral("stampAnchor"));
    auto* text = dialog.findChild<QLineEdit*>(QStringLiteral("stampText"));
    QVERIFY(anchor != nullptr && text != nullptr);
    QVERIFY(anchor->isEnabled());

    anchor->setCurrentIndex(anchor->findData(static_cast<int>(StampAnchor::BottomLeft)));
    text->setText(QStringLiteral("draft <<Page>>"));

    const auto request = dialog.request(10);
    QCOMPARE(request.anchor, StampAnchor::BottomLeft);
    QCOMPARE(request.textTemplate, QStringLiteral("draft <<Page>>"));
}

void TestStampDialog::writtenBatesFollowsDocumentPageNotPrintSequence() {
    // 寫進文件的編號要跟著文件頁碼走。列印張數會因為雙面或 N-up 與頁碼脫鉤，
    // 而法務要的是「這一頁是第幾號」。
    StampDialog dialog(StampDialog::Preset::Bates);
    QCOMPARE(dialog.request(10).bates.basis, BatesBasis::DocumentPage);
}

void TestStampDialog::emptyPageRangeMeansEveryPage() {
    // 留空＝全部頁面。這與「刪除頁面」相反（那裡留空＝取消），因為兩個
    // 操作的風險不對稱：多蓋一頁戳記可以復原，多刪一頁不行。
    StampDialog dialog(StampDialog::Preset::HeaderFooter);
    QVERIFY(dialog.pageRangeIsValid(10));
    QVERIFY2(dialog.request(10).pages.empty(), "空的 pages 才代表全部頁面");
}

void TestStampDialog::invalidPageRangeIsReportedInsteadOfMeaningEveryPage() {
    StampDialog dialog(StampDialog::Preset::HeaderFooter);
    auto* range = dialog.findChild<QLineEdit*>(QStringLiteral("stampPageRange"));
    QVERIFY(range != nullptr);

    range->setText(QStringLiteral("1,x,5"));
    // 不合法的範圍必須被呼叫端擋下來。若這裡回 true，request() 會產出一個
    // 空的 pages，而那代表「全部頁面」——整份文件會被蓋滿戳記。
    QVERIFY2(!dialog.pageRangeIsValid(10), "不合法的範圍被當成合法");

    range->setText(QStringLiteral("1-999"));
    QVERIFY2(!dialog.pageRangeIsValid(10), "超出頁數的範圍被夾成全部");

    range->setText(QStringLiteral("2,4-5"));
    QVERIFY(dialog.pageRangeIsValid(10));
    QCOMPARE(dialog.request(10).pages, (std::vector<std::int32_t>{1, 3, 4}));
}

QTEST_MAIN(TestStampDialog)
#include "test_stamp_dialog.moc"
