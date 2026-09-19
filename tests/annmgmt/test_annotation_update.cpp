// 改寫既有註解的屬性（PRD-ANN-009 的屬性面板寫回路徑）。
//
// 這條路徑與「刪掉再加一則」只差一件事，而那件事決定了它能不能用：
// 註解的物件編號必須沿用。回覆串的 /IRT 是指向**物件**的參照，換一個編號
// 之後既有的回覆全都指向一個不再掛在 /Annots 上的孤兒，而 Acrobat 會把
// 它們顯示成一堆獨立註解而不是一條串——沒有錯誤訊息，只是串沒了。
//
// 另外兩件同樣看不出來的事也一起釘住：
//   一、/Annots 裡不能多出一筆（同一則註解出現兩次，上面那份照樣點得到）
//   二、寫入必須仍然是純附加（原檔位元組一變，既有簽章就從「有效、簽章後
//       有變更」變成「無效」，而那是本產品的核心賣點）

#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include "app/annotation_service.h"
#include "engine/objects/incremental_appender.h"
#include "engine/objects/page_object_editor.h"
#include "object_fixture.h"

using alioth::app::AnnotationService;
using alioth::domain::ColorRgb;
using alioth::domain::RectF;
using alioth::engine::objects::IncrementalAppender;
using alioth::engine::objects::PdfDictionary;
using alioth::engine::objects::PdfObject;
using alioth::engine::objects::SourceStatus;

namespace {

QString writeFixture(const QTemporaryDir& dir, const QString& name) {
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return {};
    file.write(alioth::test::makeFixturePdf());
    file.close();
    return path;
}

QByteArray readAll(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

bool seedAnnotation(const QString& path, const ColorRgb& colour) {
    AnnotationService service;
    alioth::app::AnnotationRequest request;
    request.path = path;
    request.pageIndex = 0;
    alioth::domain::Annotation annotation;
    annotation.rect = RectF{40, 700, 160, 760};
    annotation.geometry =
        alioth::domain::ShapeGeometry{alioth::domain::ShapeKind::Square};
    annotation.color = colour;
    request.annotation = AnnotationService::stamped(std::move(annotation), QStringLiteral("甲"),
                                                    QStringLiteral("原本的內容"));
    return service.addAnnotation(request).ok;
}

// 第 0 頁 /Annots 裡的物件編號。
std::vector<int> annotsOnFirstPage(const QString& path) {
    const QByteArray bytes = readAll(path);
    IncrementalAppender appender;
    if (appender.open(std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()))) !=
        SourceStatus::Ok) {
        return {};
    }
    alioth::engine::objects::PdfRef pageRef{};
    if (!alioth::engine::objects::pageRefAt(appender, 0, pageRef)) return {};
    return alioth::engine::objects::pageAnnotationRefs(appender.source(), pageRef);
}

// 讀回一則註解的 /C（主色）與 /NM。
std::pair<std::vector<double>, std::string> colourAndIdOf(const QString& path, int number) {
    const QByteArray bytes = readAll(path);
    IncrementalAppender appender;
    if (appender.open(std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()))) !=
        SourceStatus::Ok) {
        return {};
    }
    const PdfObject object = appender.currentObject(number);
    const PdfDictionary* dict = object.asDictionary();
    if (dict == nullptr) return {};

    std::vector<double> colour;
    if (const PdfObject* value = dict->find("C"); value != nullptr) {
        if (const auto* array = value->asArray()) {
            for (const PdfObject& item : *array) colour.push_back(item.asNumber());
        }
    }
    std::string id;
    if (const PdfObject* value = dict->find("NM"); value != nullptr) {
        if (const auto* text =
                std::get_if<alioth::engine::objects::PdfString>(&value->value())) {
            id = text->bytes;
        }
    }
    return {colour, id};
}

}  // namespace

class TestAnnotationUpdate : public QObject {
    Q_OBJECT

private slots:
    // 改顏色：同一個物件編號、同一個 /NM、/Annots 不增加。
    void updateKeepsTheObjectNumberAndIdentity() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeFixture(dir, QStringLiteral("update.pdf"));
        QVERIFY(!path.isEmpty());
        QVERIFY(seedAnnotation(path, ColorRgb{1.0, 0.0, 0.0}));

        // 兩筆：註解本身與它的 /Popup。/Popup 也是 /Annots 的成員，
        // 少了它 Acrobat 找不到彈出視窗（見 annotation_object_writer.cpp）。
        const std::vector<int> before = annotsOnFirstPage(path);
        QCOMPARE(static_cast<int>(before.size()), 2);
        const int number = before.front();
        const auto [colourBefore, idBefore] = colourAndIdOf(path, number);
        QCOMPARE(static_cast<int>(colourBefore.size()), 3);
        QVERIFY(colourBefore[0] > 0.9);
        QVERIFY2(!idBefore.empty(), "種下去的註解沒有 /NM，這條測不到身分保留");

        // 讀回完整的註解，只改顏色——與屬性面板走同一條路。
        AnnotationService service;
        QString error;
        const std::vector<alioth::app::XfdfEntry> entries =
            service.readAnnotationsForExport(path, &error);
        QVERIFY(error.isEmpty());
        QCOMPARE(static_cast<int>(entries.size()), 1);

        alioth::domain::Annotation edited = entries.front().annotation;
        edited.color = ColorRgb{0.0, 0.0, 1.0};
        const auto result = service.updateAnnotation(path, 0, 0, edited);
        QVERIFY2(result.ok, qPrintable(result.message));

        // 數量不變、編號不變：多一筆就代表同一則註解在頁面上出現了兩次，
        // 而浮在上面的那一份照樣點得到、改得動。
        const std::vector<int> after = annotsOnFirstPage(path);
        QCOMPARE(after, before);

        const auto [colourAfter, idAfter] = colourAndIdOf(path, number);
        QCOMPARE(static_cast<int>(colourAfter.size()), 3);
        QVERIFY2(colourAfter[2] > 0.9, "顏色沒有換成藍色");
        QCOMPARE(idAfter, idBefore);
    }

    // 寫入必須是純附加：原檔那一段位元組一個都不能變。
    void updateIsPurelyIncremental() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeFixture(dir, QStringLiteral("incremental.pdf"));
        QVERIFY(!path.isEmpty());
        QVERIFY(seedAnnotation(path, ColorRgb{1.0, 0.0, 0.0}));

        const QByteArray before = readAll(path);
        QVERIFY(!before.isEmpty());

        AnnotationService service;
        QString error;
        const auto entries = service.readAnnotationsForExport(path, &error);
        QCOMPARE(static_cast<int>(entries.size()), 1);
        alioth::domain::Annotation edited = entries.front().annotation;
        edited.opacity = 0.25;
        const auto result = service.updateAnnotation(path, 0, 0, edited);
        QVERIFY2(result.ok, qPrintable(result.message));

        const QByteArray after = readAll(path);
        QVERIFY2(after.size() > before.size(), "檔案沒有變長，那就不是附加");
        QCOMPARE(after.left(before.size()), before);
        // 復原點與邊界守衛要對得起來，否則按下復原會被拒絕。
        QCOMPARE(result.previousSize, static_cast<quint64>(before.size()));
        QVERIFY(service.revertAppend(path, result.previousSize, result.boundaryGuard, nullptr));
        QCOMPARE(readAll(path), before);
    }

    // 回覆註解要被擋下來並說明理由，不是默默把它的 /IRT 拆掉。
    void repliesAreRefusedRatherThanSilentlyUnthreaded() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeFixture(dir, QStringLiteral("reply.pdf"));
        QVERIFY(!path.isEmpty());
        QVERIFY(seedAnnotation(path, ColorRgb{1.0, 0.0, 0.0}));

        AnnotationService service;
        const auto replied = service.replyToAnnotation(path, 0, 0, QStringLiteral("收到"),
                                                       QStringLiteral("乙"), QString());
        QVERIFY2(replied.ok, qPrintable(replied.message));

        const std::vector<int> annots = annotsOnFirstPage(path);
        QVERIFY2(annots.size() >= 3, "回覆沒有掛上 /Annots，這條測不到");

        // 回覆是最後加上去的那一則（回覆自己不帶 /Popup）。
        const int replyIndex = static_cast<int>(annots.size()) - 1;
        QString error;
        const auto entries = service.readAnnotationsForExport(path, &error);
        QVERIFY(!entries.empty());

        alioth::domain::Annotation edited = entries.back().annotation;
        edited.color = ColorRgb{0.0, 1.0, 0.0};
        const auto result = service.updateAnnotation(path, 0, replyIndex, edited);
        QVERIFY2(!result.ok, "回覆註解被改寫了，它的 /IRT 與狀態會就此消失");
        QVERIFY2(result.message.contains(QStringLiteral("回覆")),
                 qPrintable(QStringLiteral("錯誤訊息沒有說明理由：%1").arg(result.message)));
    }
};

QTEST_MAIN(TestAnnotationUpdate)
#include "test_annotation_update.moc"
