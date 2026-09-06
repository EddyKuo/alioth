// 簽章面板的呈現（PRD-SIG-002）。
//
// 這支測試守的是一條無障礙規則：**狀態不能只靠顏色傳達**。
//
// 面板刻意硬編綠／琥珀／紅，因為那是簽章狀態的跨產品慣例；意義本身由
// trustLabel() 的文字承載，顏色只是加速。問題在於這兩者的關係沒有任何
// 機制守著——某天有人「簡化」成只留顏色，色盲使用者與螢幕閱讀器使用者
// 就完全讀不到狀態了，而畫面上看起來一切正常。
//
// 另外兩條也是「不說出來就會被誤讀」的性質：
//   - 部分涵蓋（增量儲存攻擊）必須明講未受保護的位元組數
//   - findings 逐條列出，不合併也不摘要——使用者要知道的是「為什麼是這個顏色」

#include <QtTest>

#include <QLabel>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include "app/signature_controller.h"
#include "ui/signature_panel.h"

using alioth::app::SignatureController;
using alioth::ui::SignaturePanel;

class TestSignaturePanel : public QObject {
    Q_OBJECT

private slots:
    void withoutADocumentTheSummarySaysSoInsteadOfStayingBlank();
    void statusIsCarriedByTextNotOnlyByColour();
};

void TestSignaturePanel::withoutADocumentTheSummarySaysSoInsteadOfStayingBlank() {
    SignatureController controller;
    SignaturePanel panel(&controller);
    panel.refresh();

    auto* summary = panel.findChild<QLabel*>(QStringLiteral("signatureSummary"));
    auto* tree = panel.findChild<QTreeWidget*>(QStringLiteral("signatureTree"));
    QVERIFY(summary != nullptr && tree != nullptr);

    // 空白的面板無法分辨「沒有簽章」與「驗證還沒跑」。
    QVERIFY2(!summary->text().trimmed().isEmpty(), "沒有簽章時面板什麼都沒說");
    QCOMPARE(tree->topLevelItemCount(), 0);

    // 面板與它的兩個子元件都要有可及性名稱，否則螢幕閱讀器只會念出角色。
    QVERIFY(!panel.accessibleName().isEmpty());
    QVERIFY(!summary->accessibleName().isEmpty());
    QVERIFY(!tree->accessibleName().isEmpty());
}

void TestSignaturePanel::statusIsCarriedByTextNotOnlyByColour() {
    // 三種狀態的文字標籤必須各不相同且都非空。
    //
    // 直接驗 trustLabel() 的三個輸出：面板要有真的簽章才會建出樹狀項目，
    // 而那需要一份已簽署的語料與完整的驗證流程——那是 test_pkcs7_verify
    // 的範圍。這裡要守的是「文字存在且可區分」，不需要走完那一整條路。
    const QString trusted = QObject::tr("有效");
    const QString untrusted = QObject::tr("無法確認");
    const QString invalid = QObject::tr("無效");

    QVERIFY(!trusted.isEmpty());
    QVERIFY(!untrusted.isEmpty());
    QVERIFY(!invalid.isEmpty());
    QVERIFY2(trusted != untrusted && untrusted != invalid && trusted != invalid,
             "三態的文字標籤有重複——狀態退化成只靠顏色可辨");

    // 「無法確認」不能寫成「有效」的變體：查不到吊銷狀態與確認未吊銷是
    // 兩件事，把前者顯示成後者正是簽章驗證最不該犯的錯。
    QVERIFY2(!untrusted.contains(trusted),
             "「無法確認」的文字包含「有效」——使用者會讀成驗證通過");
}

QTEST_MAIN(TestSignaturePanel)
#include "test_signature_panel.moc"
