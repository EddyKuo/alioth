// Session 儲存與復原（PRD-UI-012）。
//
// 這一層的失敗方式很特別：寫壞了不會崩潰，只會讓使用者每次重開都回到第 1 頁，
// 然後認定「這個功能沒做」。所以這裡的斷言集中在「還原回來的值與存進去的一致」，
// 以及「壞掉的設定不會讓程式開不起來」。

#include <QtTest>

#include <QCoreApplication>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>

#include "app/session.h"

using namespace alioth;

class TestSession : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName(QStringLiteral("AliothTest"));
        QCoreApplication::setApplicationName(QStringLiteral("SessionTest"));
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());

        existing_ = dir_->filePath(QStringLiteral("doc.pdf"));
        QFile file(existing_);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("%PDF-1.7\n");
        file.close();
    }

    void init() { QSettings().clear(); }

    void roundTripPreservesReadingPosition() {
        app::Session session;
        app::DocumentSession document;
        document.path = existing_;
        document.pageIndex = 217;
        document.scale = 1.75;
        document.layoutMode = domain::LayoutMode::TwoPageContinuous;
        document.coverPageSeparate = false;
        document.rightToLeft = true;
        session.documents.push_back(document);
        session.windowGeometry = QByteArray("geometry-blob");
        session.windowState = QByteArray("state-blob");

        app::saveSession(session);

        const app::Session restored = app::loadSession();
        QCOMPARE(restored.documents.size(), std::size_t{1});

        const app::DocumentSession& back = restored.documents.front();
        QCOMPARE(back.path, existing_);
        QCOMPARE(back.pageIndex, 217);
        QCOMPARE(back.scale, 1.75);
        QCOMPARE(back.layoutMode, domain::LayoutMode::TwoPageContinuous);
        QCOMPARE(back.coverPageSeparate, false);
        QCOMPARE(back.rightToLeft, true);
        QCOMPARE(restored.windowGeometry, QByteArray("geometry-blob"));
        QCOMPARE(restored.windowState, QByteArray("state-blob"));
    }

    void documentsThatNoLongerExistAreDropped() {
        // 還原到一個開不起來的檔案，使用者看到的是啟動時跳一個錯誤對話框。
        // 那比什麼都不還原更糟。
        app::Session session;
        app::DocumentSession gone;
        gone.path = dir_->filePath(QStringLiteral("deleted.pdf"));
        session.documents.push_back(gone);
        app::saveSession(session);

        QVERIFY(app::loadSession().isEmpty());
    }

    void absurdValuesFallBackToDefaults() {
        // 設定檔可能被手改壞。0 倍的縮放會讓畫面完全空白，
        // 而使用者無從判斷發生了什麼事。
        QSettings settings;
        settings.beginGroup(QStringLiteral("session"));
        settings.beginWriteArray(QStringLiteral("documents"), 1);
        settings.setArrayIndex(0);
        settings.setValue(QStringLiteral("path"), existing_);
        settings.setValue(QStringLiteral("scale"), 0.0);
        settings.setValue(QStringLiteral("pageIndex"), -5);
        settings.endArray();
        settings.endGroup();
        settings.sync();

        const app::Session restored = app::loadSession();
        QCOMPARE(restored.documents.size(), std::size_t{1});
        QCOMPARE(restored.documents.front().scale, 1.0);
        QCOMPARE(restored.documents.front().pageIndex, 0);
    }

    void unknownLayoutModeFallsBackInsteadOfSilentlyChanging() {
        // 版面模式以字串儲存正是為了這件事：讀不到就退回預設，
        // 而不是把一個越界的整數當成另一種版面。
        QSettings settings;
        settings.beginGroup(QStringLiteral("session"));
        settings.beginWriteArray(QStringLiteral("documents"), 1);
        settings.setArrayIndex(0);
        settings.setValue(QStringLiteral("path"), existing_);
        settings.setValue(QStringLiteral("layoutMode"), QStringLiteral("nonsense"));
        settings.endArray();
        settings.endGroup();
        settings.sync();

        QCOMPARE(app::loadSession().documents.front().layoutMode, domain::LayoutMode::Continuous);
    }

    void savingFewerDocumentsRemovesTheOldEntries() {
        // QSettings 的 beginWriteArray 不會移除多出來的舊項目。
        // 少了那次 remove，關掉一份文件之後它還會在下次啟動時被還原。
        app::Session two;
        app::DocumentSession a;
        a.path = existing_;
        two.documents.push_back(a);
        two.documents.push_back(a);
        app::saveSession(two);
        QCOMPARE(app::loadSession().documents.size(), std::size_t{2});

        app::Session one;
        one.documents.push_back(a);
        app::saveSession(one);
        QCOMPARE(app::loadSession().documents.size(), std::size_t{1});
    }

    void activeIndexIsClampedToAvailableDocuments() {
        app::Session session;
        app::DocumentSession document;
        document.path = existing_;
        session.documents.push_back(document);
        session.activeIndex = 7;
        app::saveSession(session);

        QCOMPARE(app::loadSession().activeIndex, 0);
    }

    void emptySessionIsNotAnError() {
        app::saveSession(app::Session{});
        const app::Session restored = app::loadSession();
        QVERIFY(restored.isEmpty());
        QCOMPARE(restored.activeIndex, 0);
    }

    void clearRemovesEverything() {
        app::Session session;
        app::DocumentSession document;
        document.path = existing_;
        session.documents.push_back(document);
        app::saveSession(session);

        app::clearSession();
        QVERIFY(app::loadSession().isEmpty());
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
    QString existing_;
};

QTEST_MAIN(TestSession)
#include "test_session.moc"
