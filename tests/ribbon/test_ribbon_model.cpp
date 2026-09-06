// Ribbon 資料模型的測試（WBS 3.22 / PRD-UI-010 / PRD-UI-011）。
//
// 重點不在 JSON 好不好看，而在兩件事：使用者自訂的配置存出去再讀回來必須一模一樣，
// 以及設定檔被手改壞時不能崩潰。後者是實際情境——設定檔是純文字，使用者會去編它。

#include <QtTest>

#include <QJsonDocument>
#include <QSet>
#include <QJsonObject>
#include <QTemporaryDir>

#include "ui/ribbon/ribbon_default_layout.h"
#include "ui/ribbon/ribbon_model.h"

using namespace alioth::ui::ribbon;

namespace {

[[nodiscard]] Layout sampleLayout() {
    Item copy;
    copy.actionId = QStringLiteral("edit.copy");
    copy.label = QStringLiteral("複製");
    copy.iconName = QStringLiteral("edit-copy");
    copy.size = ItemSize::Large;
    copy.keyTip = QStringLiteral("C");

    Item paste;
    paste.actionId = QStringLiteral("edit.paste");
    paste.size = ItemSize::Small;

    Item separator;
    separator.type = ItemType::Separator;

    Group clipboard;
    clipboard.id = QStringLiteral("home.clipboard");
    clipboard.title = QStringLiteral("剪貼簿");
    clipboard.keyTip = QStringLiteral("B");
    clipboard.items = {copy, separator, paste};

    Page home;
    home.id = QStringLiteral("home");
    home.title = QStringLiteral("常用");
    home.keyTip = QStringLiteral("H");
    home.groups = {clipboard};

    Layout layout;
    layout.version = 1;
    layout.pages = {home};
    layout.quickAccessActionIds = {QStringLiteral("file.open"), QStringLiteral("file.save")};
    layout.collapsed = true;
    return layout;
}

[[nodiscard]] QJsonObject jsonFrom(const char* text) {
    return QJsonDocument::fromJson(QByteArray(text)).object();
}

}  // namespace

class TestRibbonModel : public QObject {
    Q_OBJECT

private slots:
    void jsonRoundTripPreservesStructure() {
        const Layout original = sampleLayout();
        ParseResult result;
        const Layout restored = fromJson(toJson(original), &result);

        QVERIFY(result.ok());
        QVERIFY(result.warnings.isEmpty());
        QCOMPARE(restored, original);
    }

    void fileRoundTripPreservesStructure() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("ribbon.json"));

        const Layout original = defaultLayout();
        ParseResult saveResult;
        QVERIFY(saveToFile(original, path, &saveResult));
        QVERIFY(saveResult.ok());

        ParseResult loadResult;
        const Layout restored = loadFromFile(path, &loadResult);
        QVERIFY2(loadResult.ok(), qPrintable(loadResult.error));
        QVERIFY(loadResult.warnings.isEmpty());
        QCOMPARE(restored, original);
    }

    void defaultLayoutMatchesPrdPages() {
        const Layout layout = defaultLayout();
        // PRD-UI-002 指定的八個分頁，順序也是規格的一部分。
        const QStringList expected{"file", "home", "view", "comment", "protect",
                                   "form", "organize", "help"};
        QStringList actual;
        for (const Page& page : layout.pages) actual << page.id;
        QCOMPARE(actual, expected);

        for (const Page& page : layout.pages) {
            QVERIFY2(!page.groups.isEmpty(), qPrintable(page.id + " 沒有任何群組"));
            QVERIFY2(!page.keyTip.isEmpty(), qPrintable(page.id + " 沒有 KeyTip"));
            for (const Group& group : page.groups) {
                QVERIFY(!group.id.isEmpty());
                QVERIFY(!group.items.isEmpty());
                for (const Item& item : group.items) {
                    if (item.type == ItemType::Action) QVERIFY(!item.actionId.isEmpty());
                }
            }
        }
        QVERIFY(!layout.quickAccessActionIds.isEmpty());
    }

    void emptyConfigurationIsValid() {
        // 使用者把所有分頁都刪掉是合法狀態，不是錯誤。
        ParseResult result;
        const Layout layout = fromJson(jsonFrom("{}"), &result);
        QVERIFY(result.ok());
        QVERIFY(layout.isEmpty());
        QCOMPARE(layout.version, 1);
        QVERIFY(layout.quickAccessActionIds.isEmpty());

        const Layout roundTrip = fromJson(toJson(layout));
        QCOMPARE(roundTrip, layout);
    }

    void malformedNodesAreSkippedNotFatal() {
        ParseResult result;
        const Layout layout = fromJson(jsonFrom(R"({
            "version": "不是數字",
            "pages": [
                42,
                {"title": "沒有 id 的分頁"},
                {"id": "home", "groups": "不是陣列"},
                {"id": "home", "title": "重複 id"},
                {"id": "view", "groups": [
                    17,
                    {"id": "view.zoom", "items": [
                        {"actionId": "view.zoomIn", "size": "gigantic"},
                        {"label": "沒有 actionId"},
                        "不是物件",
                        {"type": "separator"}
                    ]}
                ]}
            ],
            "quickAccess": ["file.open", 5, ""]
        })"),
                                       &result);

        QVERIFY(result.ok());
        QVERIFY(!result.warnings.isEmpty());

        // 救得回來的部分要完整：home（空群組）與 view（一個群組、兩個項目）。
        QCOMPARE(layout.pages.size(), 2);
        QCOMPARE(layout.pages.at(0).id, QStringLiteral("home"));
        QVERIFY(layout.pages.at(0).groups.isEmpty());

        const Page& view = layout.pages.at(1);
        QCOMPARE(view.groups.size(), 1);
        QCOMPARE(view.groups.at(0).items.size(), 2);
        QCOMPARE(view.groups.at(0).items.at(0).actionId, QStringLiteral("view.zoomIn"));
        // 未知尺寸退回 large 而不是整個項目消失。
        QCOMPARE(view.groups.at(0).items.at(0).size, ItemSize::Large);
        QCOMPARE(view.groups.at(0).items.at(1).type, ItemType::Separator);

        QCOMPARE(layout.quickAccessActionIds, QStringList{QStringLiteral("file.open")});
    }

    void brokenFileReportsErrorInsteadOfCrashing() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString garbage = dir.filePath(QStringLiteral("garbage.json"));
        QFile file(garbage);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{ this is not json at all ");
        file.close();

        ParseResult result;
        const Layout layout = loadFromFile(garbage, &result);
        QVERIFY(!result.ok());
        QVERIFY(layout.isEmpty());

        // 根節點不是物件（陣列）同樣要被擋下來。
        const QString array = dir.filePath(QStringLiteral("array.json"));
        QFile arrayFile(array);
        QVERIFY(arrayFile.open(QIODevice::WriteOnly));
        arrayFile.write("[]");
        arrayFile.close();
        ParseResult arrayResult;
        QVERIFY(loadFromFile(array, &arrayResult).isEmpty());
        QVERIFY(!arrayResult.ok());

        // 檔案根本不存在時也只是錯誤，不是崩潰。
        ParseResult missingResult;
        QVERIFY(loadFromFile(dir.filePath(QStringLiteral("nope.json")), &missingResult).isEmpty());
        QVERIFY(!missingResult.ok());

        // 不傳 result 的呼叫路徑也要安全。
        QVERIFY(loadFromFile(garbage).isEmpty());
    }

    void automaticKeyTipsAreUniquePerLevel() {
        Layout layout;
        Page page;
        page.id = QStringLiteral("home");
        page.title = QStringLiteral("Home");

        Group group;
        group.id = QStringLiteral("g");
        for (const char* id : {"copy", "cut", "clone", "貼上"}) {
            Item item;
            item.actionId = QString::fromUtf8(id);
            item.label = QString::fromUtf8(id);
            group.items.append(item);
        }
        page.groups = {group};
        layout.pages = {page};

        assignAutomaticKeyTips(layout);
        QCOMPARE(layout.pages.at(0).keyTip, QStringLiteral("H"));

        QStringList tips;
        for (const Item& item : layout.pages.at(0).groups.at(0).items) {
            QVERIFY2(!item.keyTip.isEmpty(), qPrintable(item.actionId));
            tips << item.keyTip;
        }
        QCOMPARE(tips.at(0), QStringLiteral("C"));
        // 同一層不得重複，否則按下去會有二義性。
        QSet<QString> unique(tips.begin(), tips.end());
        QCOMPARE(unique.size(), tips.size());
        // 中文標籤按不出 ASCII 鍵，必須退回數字而不是留空。
        QCOMPARE(tips.at(3), QStringLiteral("1"));
    }

    void collectActionIdsCoversQuickAccessAndPages() {
        const QStringList ids = collectActionIds(sampleLayout());
        QVERIFY(ids.contains(QStringLiteral("file.open")));
        QVERIFY(ids.contains(QStringLiteral("edit.copy")));
        QVERIFY(ids.contains(QStringLiteral("edit.paste")));
        // 分隔線沒有 id，不該混進來。
        QCOMPARE(ids.size(), 4);
    }
};

QTEST_GUILESS_MAIN(TestRibbonModel)
#include "test_ribbon_model.moc"
