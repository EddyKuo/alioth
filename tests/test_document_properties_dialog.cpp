// 文件屬性對話框（PRD-ENH-005）。
//
// 這支測試守的是「降級必須看得見」（SDD §7）：XFA 與 JavaScript 兩種文件
// 本程式都是刻意不完整支援的——XFA 只顯示後備內容，JavaScript 一律不執行。
// 如果屬性對話框不說，使用者會以為自己看到的是完整的文件，然後把一份
// 表單填錯或以為某個按鈕壞了。
//
// 這種「不說出來」的缺陷不會有任何徵兆：對話框照樣開得起來、每一欄都有值。

#include <QtTest>

#include <QLabel>

#include "domain/document.h"
#include "ui/document_properties_dialog.h"

using alioth::domain::DocumentInfo;
using alioth::ui::DocumentPropertiesDialog;

namespace {

// 對話框沒有給欄位取 objectName，所以用「掃過所有 QLabel 的文字」來驗。
// 那正是使用者看得到的東西，比綁 objectName 更貼近要守的性質。
QString allText(const QWidget& widget) {
    QString joined;
    for (const QLabel* label : widget.findChildren<QLabel*>()) {
        joined += label->text();
        joined += QLatin1Char('\n');
    }
    return joined;
}

DocumentInfo plainDocument() {
    DocumentInfo info;
    info.pageCount = 12;
    info.pdfVersion = "1.7";
    return info;
}

}  // namespace

class TestDocumentPropertiesDialog : public QObject {
    Q_OBJECT

private slots:
    void xfaDocumentSaysOnlyFallbackContentIsShown();
    void javaScriptDocumentSaysItIsNotExecuted();
    void plainDocumentHasNoScaryNotes();
    void restrictionsAreListedByName();
};

void TestDocumentPropertiesDialog::xfaDocumentSaysOnlyFallbackContentIsShown() {
    DocumentInfo info = plainDocument();
    info.hasXfa = true;

    DocumentPropertiesDialog dialog(QStringLiteral("C:/tmp/x.pdf"), info);
    const QString text = allText(dialog);
    QVERIFY2(text.contains(QStringLiteral("XFA")), qPrintable(text));
    // 光說「含 XFA」不夠——使用者不知道那對他有什麼影響。
    QVERIFY2(text.contains(QStringLiteral("後備內容")),
             "沒有說明 XFA 只顯示後備內容，使用者會以為看到的是完整表單");
}

void TestDocumentPropertiesDialog::javaScriptDocumentSaysItIsNotExecuted() {
    DocumentInfo info = plainDocument();
    info.hasJavaScript = true;

    DocumentPropertiesDialog dialog(QStringLiteral("C:/tmp/j.pdf"), info);
    const QString text = allText(dialog);
    QVERIFY(text.contains(QStringLiteral("JavaScript")));
    QVERIFY2(text.contains(QStringLiteral("不執行")),
             "沒有說明 JavaScript 不會執行，使用者會以為表單的自動計算壞了");
}

void TestDocumentPropertiesDialog::plainDocumentHasNoScaryNotes() {
    // 普通文件不該出現任何「注意」——把警告常態化，使用者就不再讀它了。
    DocumentPropertiesDialog dialog(QStringLiteral("C:/tmp/p.pdf"), plainDocument());
    const QString text = allText(dialog);
    QVERIFY(!text.contains(QStringLiteral("XFA")));
    QVERIFY(!text.contains(QStringLiteral("JavaScript")));
    QVERIFY(text.contains(QStringLiteral("12")));   // 頁數
    QVERIFY(text.contains(QStringLiteral("1.7")));  // PDF 版本
}

void TestDocumentPropertiesDialog::restrictionsAreListedByName() {
    DocumentInfo info = plainDocument();
    info.encrypted = true;
    info.permissions.print = false;
    info.permissions.annotate = false;

    DocumentPropertiesDialog dialog(QStringLiteral("C:/tmp/r.pdf"), info);
    const QString text = allText(dialog);
    // 逐項列出被禁止的操作。只說「有限制」的話，使用者無從知道為什麼
    // 某顆按鈕是灰的。
    QVERIFY2(text.contains(QStringLiteral("列印")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("加註")), qPrintable(text));
}

QTEST_MAIN(TestDocumentPropertiesDialog)
#include "test_document_properties_dialog.moc"
