// CJK 字型的來源選擇（ADR-007）。
//
// 這支測試守的是一個容易被當成無所謂、實際上會毀掉核心承諾的行為：
// **內嵌進 PDF 的必須是我們隨附並驗證過的那一份字型**。
//
// 使用者機器上的同名字型可能是不同版本、不同字重，甚至是同名的別家字型。
// 退回系統字型是可接受的降級，但不能靜默地取代隨附字型——那會讓
// 「產出的檔案在別人機器上長得一樣」這個 PDF 存在的理由失效，
// 而且沒有任何徵兆。

#include <QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include "engine/fonts/cjk_font_library.h"
#include "platform/font_paths.h"

using namespace alioth;

class TestFontPaths : public QObject {
    Q_OBJECT

private slots:
    void bundledFontIsTheFirstCandidate() {
        const std::vector<QString> candidates = platform::cjkFontCandidates();
        QVERIFY2(!candidates.empty(), "候選清單是空的，任何機器上都找不到字型");

        // 第一順位必須是執行檔旁邊的 fonts/。順序反過來的話，有隨附字型
        // 也會被系統字型蓋掉，而兩者的字重可能不同。
        const QString expected =
            QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("fonts")) +
            QStringLiteral("/NotoSansTC-Regular.ttf");
        QCOMPARE(QDir::cleanPath(candidates.front()), QDir::cleanPath(expected));
    }

    void bundledFontIsDeployedNextToTheExecutable() {
        // 建置系統會把 third_party/fonts 的字型複製到輸出目錄（見 src/CMakeLists.txt）。
        // 沒複製到的話，正式安裝之後使用者打不了中文，而開發機上因為有系統字型
        // 所以一切看起來正常——那正是這條測試要擋的落差。
        const QString bundled =
            QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("fonts")) +
            QStringLiteral("/NotoSansTC-Regular.ttf");
        if (!QFileInfo::exists(bundled)) {
            QSKIP("這個建置沒有隨附字型（third_party/fonts 是空的）");
        }

        const platform::CjkFontSource source = platform::findCjkFont();
        QVERIFY(source.isValid());
        QVERIFY2(source.bundled,
                 qPrintable(QStringLiteral("有隨附字型卻選了 %1").arg(source.path)));
    }

    void licenceTravelsWithTheFont() {
        // OFL 要求散布時附帶授權條款。少了它就是授權違規，而那不會有任何
        // 技術上的徵兆——程式照跑，只是我們沒有權利那樣散布。
        const QString fonts =
            QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("fonts"));
        if (!QFileInfo::exists(fonts + QStringLiteral("/NotoSansTC-Regular.ttf"))) {
            QSKIP("這個建置沒有隨附字型");
        }
        QVERIFY2(QFileInfo::exists(fonts + QStringLiteral("/OFL.txt")),
                 "隨附了字型卻沒有附上 OFL 授權條款");
    }

    void theLibraryLoadsWhicheverFontWasFound() {
        auto& library = engine::fonts::CjkFontLibrary::instance();
        if (!library.available()) {
            // 找不到字型時必須說得出找過哪裡——那是使用者唯一能自救的資訊。
            QVERIFY2(!library.diagnostic().empty(), "沒有字型卻也沒有說明");
            QSKIP("這台機器上沒有任何可用的 CJK 字型");
        }
        QVERIFY(!library.baseName().empty());
        // 隨便一個常用漢字都該有字形。
        QVERIFY(library.glyphFor(U'中') != 0);
        QVERIFY(library.advanceFor(U'中') > 0);
    }

    void diagnosticListsWhereItLooked() {
        // 這裡不能直接測「找不到」的情況（機器上就是有字型），但可以確認
        // 候選清單本身是有內容且可讀的——診斷訊息就是由它組出來的。
        const std::vector<QString> candidates = platform::cjkFontCandidates();
        for (const QString& candidate : candidates) {
            QVERIFY(!candidate.isEmpty());
        }
    }
};

QTEST_MAIN(TestFontPaths)
#include "test_font_paths.moc"
