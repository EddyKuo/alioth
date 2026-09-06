// 雲端同步資料夾（PRD-CLD-001 ~ 005 的桌面實作路徑）。
//
// 偵測本身依賴這台機器上裝了什麼，沒辦法在測試裡固定；但**路徑歸屬的判定**
// 是純邏輯，而它正是會出錯又難以察覺的部分——把一般本機資料夾誤標成
// 「會同步到雲端」，比完全不標示更容易誤導使用者。

#include <QtTest>

#include <QTemporaryDir>

#include "platform/cloud_folders.h"

using namespace alioth::platform;

namespace {

std::vector<CloudFolder> folders(const QString& root) {
    return {
        CloudFolder{CloudProvider::OneDrive, root + QStringLiteral("/OneDrive"), {}},
        CloudFolder{CloudProvider::Dropbox, root + QStringLiteral("/Dropbox"), {}},
        // SharePoint 的文件庫常常同步在 OneDrive 根目錄底下，用來驗最長前綴優先。
        CloudFolder{CloudProvider::SharePoint,
                    root + QStringLiteral("/OneDrive/Contoso 工程部 - Documents"), {}},
    };
}

}  // namespace

class TestCloudFolders : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        root_ = dir_->path();
    }

    void pathInsideASyncFolderIsRecognised() {
        const auto list = folders(root_);
        QCOMPARE(providerForPath(root_ + QStringLiteral("/OneDrive/plan.pdf"), list),
                 CloudProvider::OneDrive);
        QCOMPARE(providerForPath(root_ + QStringLiteral("/Dropbox/a/b/plan.pdf"), list),
                 CloudProvider::Dropbox);
    }

    void ordinaryLocalPathIsNotClaimed() {
        // 誤標比不標示嚴重：使用者會以為檔案已經備份出去了。
        const auto list = folders(root_);
        QCOMPARE(providerForPath(root_ + QStringLiteral("/Documents/plan.pdf"), list),
                 CloudProvider::None);
        QCOMPARE(providerForPath(QString(), list), CloudProvider::None);
        QVERIFY(cloudNoticeForPath(root_ + QStringLiteral("/Documents/plan.pdf"), list).isEmpty());
    }

    void similarlyNamedSiblingIsNotMatched() {
        // "OneDriveBackup" 不在 "OneDrive" 底下。前綴比對忘了要求接分隔符號時，
        // 這一條就會錯。
        const auto list = folders(root_);
        QCOMPARE(providerForPath(root_ + QStringLiteral("/OneDriveBackup/plan.pdf"), list),
                 CloudProvider::None);
    }

    void longestPrefixWins() {
        // SharePoint 文件庫同步在 OneDrive 底下時，答案不可以取決於偵測順序。
        const auto list = folders(root_);
        QCOMPARE(providerForPath(
                     root_ + QStringLiteral("/OneDrive/Contoso 工程部 - Documents/spec.pdf"), list),
                 CloudProvider::SharePoint);
    }

    void syncRootItselfCounts() {
        const auto list = folders(root_);
        QCOMPARE(providerForPath(root_ + QStringLiteral("/OneDrive"), list),
                 CloudProvider::OneDrive);
    }

    void redundantSeparatorsDoNotBreakMatching() {
        const auto list = folders(root_);
        QCOMPARE(providerForPath(root_ + QStringLiteral("/OneDrive//sub/./plan.pdf"), list),
                 CloudProvider::OneDrive);
    }

#ifdef _WIN32
    void windowsMatchingIsCaseInsensitive() {
        const auto list = folders(root_);
        QCOMPARE(providerForPath((root_ + QStringLiteral("/onedrive/PLAN.pdf")).toUpper(), list),
                 CloudProvider::OneDrive);
    }
#endif

    void noticeNamesTheProvider() {
        const auto list = folders(root_);
        const QString notice = cloudNoticeForPath(root_ + QStringLiteral("/Dropbox/plan.pdf"), list);
        QVERIFY(notice.contains(QStringLiteral("Dropbox")));
        QVERIFY(notice.contains(QStringLiteral("同步")));
    }

    void emptyFolderListClaimsNothing() {
        // 偵測不到不代表使用者沒用雲端（自訂位置、還沒登入、用網頁版），
        // 但我們也不該憑空宣稱任何事。
        QCOMPARE(providerForPath(root_ + QStringLiteral("/OneDrive/plan.pdf"), {}),
                 CloudProvider::None);
    }

    void detectionDoesNotCrashOnThisMachine() {
        // 偵測結果依機器而異，無法固定；能驗的是它不會爆、也不會回傳
        // 不存在的目錄。
        for (const CloudFolder& folder : detectCloudFolders()) {
            QVERIFY(!folder.rootPath.isEmpty());
            QVERIFY(QFileInfo(folder.rootPath).isDir());
            QVERIFY(folder.provider != CloudProvider::None);
            QVERIFY(!providerDisplayName(folder.provider).isEmpty());
        }
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
    QString root_;
};

QTEST_MAIN(TestCloudFolders)
#include "test_cloud_folders.moc"
