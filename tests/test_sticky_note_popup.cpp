// 註釋視窗（PRD-ANN-004）。
//
// 驗的是這個元件對外的契約：什麼時候發出 contentsCommitted、什麼時候不發。
// 「不發」比「發」更重要——每一次發出都會變成一次增量寫入，沒改也發就等於
// 每開一次註釋視窗檔案就長大一點。

#include <QtTest>

#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>

#include "ui/sticky_note_popup.h"

using alioth::ui::StickyNotePopup;

class TestStickyNotePopup : public QObject {
    Q_OBJECT

private slots:
    void savingChangedTextEmitsCommitWithTheAnnotationIdentity();
    void savingUnchangedTextEmitsNothing();
    void closingCommitsPendingEditsAndAnnouncesDismissal();
    void readOnlyDocumentCannotCommit();
    void reopeningAnotherAnnotationResetsTheBaseline();
};

void TestStickyNotePopup::savingChangedTextEmitsCommitWithTheAnnotationIdentity() {
    StickyNotePopup popup;
    popup.showAnnotation(2, 5, QStringLiteral("Text"), QStringLiteral("審閱者"),
                         QStringLiteral("D:20260905"), QStringLiteral("原本"));
    QSignalSpy spy(&popup, &StickyNotePopup::contentsCommitted);

    auto* editor = popup.findChild<QPlainTextEdit*>(QStringLiteral("stickyNotePopupEditor"));
    QVERIFY(editor != nullptr);
    editor->setPlainText(QStringLiteral("改過"));

    auto* save = popup.findChild<QPushButton*>(QStringLiteral("stickyNotePopupSave"));
    QVERIFY(save->isEnabled());
    save->click();

    QCOMPARE(spy.count(), 1);
    // 頁碼與頁內序號要原封不動傳回去：寫入端就是靠這兩個數字定位那一則註解，
    // 錯一個就會改到隔壁那則。
    QCOMPARE(spy.at(0).at(0).toInt(), 2);
    QCOMPARE(spy.at(0).at(1).toInt(), 5);
    QCOMPARE(spy.at(0).at(2).toString(), QStringLiteral("改過"));

    // 存過之後再按一次不該重複寫入。
    save->click();
    QCOMPARE(spy.count(), 1);
}

void TestStickyNotePopup::savingUnchangedTextEmitsNothing() {
    StickyNotePopup popup;
    popup.showAnnotation(0, 0, QStringLiteral("Text"), QString(), QString(),
                         QStringLiteral("沒動過"));
    QSignalSpy spy(&popup, &StickyNotePopup::contentsCommitted);

    auto* save = popup.findChild<QPushButton*>(QStringLiteral("stickyNotePopupSave"));
    QVERIFY(!save->isEnabled());
    save->click();
    QCOMPARE(spy.count(), 0);
}

void TestStickyNotePopup::closingCommitsPendingEditsAndAnnouncesDismissal() {
    StickyNotePopup popup;
    popup.showAnnotation(1, 0, QStringLiteral("Text"), QString(), QString(), QStringLiteral("舊"));
    QSignalSpy committed(&popup, &StickyNotePopup::contentsCommitted);
    QSignalSpy dismissed(&popup, &StickyNotePopup::dismissed);

    popup.findChild<QPlainTextEdit*>(QStringLiteral("stickyNotePopupEditor"))
        ->setPlainText(QStringLiteral("新"));
    popup.close();

    // 便利貼的預期是「打字就會留著」，關窗不該把剛輸入的字丟掉。
    QCOMPARE(committed.count(), 1);
    QCOMPARE(committed.at(0).at(2).toString(), QStringLiteral("新"));
    QCOMPARE(dismissed.count(), 1);
}

void TestStickyNotePopup::readOnlyDocumentCannotCommit() {
    StickyNotePopup popup;
    popup.setReadOnly(true);
    popup.showAnnotation(0, 0, QStringLiteral("Text"), QString(), QString(), QStringLiteral("原"));
    QSignalSpy spy(&popup, &StickyNotePopup::contentsCommitted);

    auto* editor = popup.findChild<QPlainTextEdit*>(QStringLiteral("stickyNotePopupEditor"));
    QVERIFY(editor->isReadOnly());
    QVERIFY(!popup.findChild<QPushButton*>(QStringLiteral("stickyNotePopupSave"))->isEnabled());

    // 即使有人繞過 UI 直接改文字（例如程式化設定），關窗也不該寫進去。
    editor->setPlainText(QStringLiteral("硬改"));
    popup.close();
    QCOMPARE(spy.count(), 0);
}

void TestStickyNotePopup::reopeningAnotherAnnotationResetsTheBaseline() {
    StickyNotePopup popup;
    popup.showAnnotation(0, 0, QStringLiteral("Text"), QString(), QString(), QStringLiteral("甲"));
    popup.showAnnotation(3, 1, QStringLiteral("Text"), QString(), QString(), QStringLiteral("乙"));
    QSignalSpy spy(&popup, &StickyNotePopup::contentsCommitted);

    // 切到另一則之後基準必須跟著換，否則「儲存」一開啟就是啟用的，
    // 而按下去會把乙的內容當成一次修改再寫一遍。
    QVERIFY(!popup.findChild<QPushButton*>(QStringLiteral("stickyNotePopupSave"))->isEnabled());
    QCOMPARE(popup.contents(), QStringLiteral("乙"));
    QCOMPARE(popup.pageIndex(), 3);
    QCOMPARE(popup.indexOnPage(), 1);

    popup.close();
    QCOMPARE(spy.count(), 0);
}

QTEST_MAIN(TestStickyNotePopup)
#include "test_sticky_note_popup.moc"
