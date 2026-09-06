// 註解回覆與審閱狀態（PRD-ANN-007）。
//
// 在 PDF 裡這兩件事是同一件事：狀態由一則**獨立的回覆註解**承載
// （/IRT 指向被回覆者，加上 /StateModel /State），而不是改寫原註解。
// 那是刻意的——這樣「誰在什麼時候把它標成已完成」才留得下來
// （ISO 32000-1 §12.5.6.19）。改寫原註解會把那段歷史抹掉。
//
// 這支測試釘住的就是那個結構：原註解一個位元組都不變，新增的那一則帶 /IRT。

#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include "app/annotation_service.h"
#include "engine/objects/incremental_appender.h"
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

// 造一則可以被回覆的註解。
bool seedAnnotation(const QString& path) {
    AnnotationService service;
    alioth::app::AnnotationRequest request;
    request.path = path;
    request.pageIndex = 0;
    alioth::domain::Annotation annotation;
    annotation.rect = RectF{40, 700, 60, 720};
    annotation.geometry = alioth::domain::TextNoteGeometry{};
    annotation.color = ColorRgb{1.0, 0.85, 0.0};
    request.annotation =
        AnnotationService::stamped(std::move(annotation), QStringLiteral("甲"),
                                   QStringLiteral("請確認"));
    return service.addAnnotation(request).ok;
}

// 掃出全文件所有帶 /IRT 的註解物件，回傳它們的 (IRT 目標, /State)。
std::vector<std::pair<int, std::string>> repliesIn(const QString& path) {
    std::vector<std::pair<int, std::string>> out;
    const QByteArray bytes = readAll(path);
    IncrementalAppender appender;
    if (appender.open(std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()))) !=
        SourceStatus::Ok) {
        return out;
    }
    for (int number = 1; number < 200; ++number) {
        const PdfObject object = appender.currentObject(number);
        const PdfDictionary* dict = object.asDictionary();
        if (dict == nullptr) continue;
        const PdfObject* irt = dict->find("IRT");
        if (irt == nullptr || !irt->isRef()) continue;
        std::string state;
        if (const PdfObject* value = dict->find("State"); value != nullptr) {
            if (const auto* text =
                    std::get_if<alioth::engine::objects::PdfString>(&value->value())) {
                state = text->bytes;
            }
        }
        out.emplace_back(irt->asRef().number, state);
    }
    return out;
}

}  // namespace

class TestAnnotationReply : public QObject {
    Q_OBJECT

private slots:
    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
    }

    void replyIsANewAnnotationPointingAtTheOriginal();
    void statusIsCarriedByAReplyNotByRewritingTheOriginal();
    void emptyReplyIsRejected();
    void replyingToAMissingAnnotationFails();

private:
    std::unique_ptr<QTemporaryDir> dir_;
};

void TestAnnotationReply::replyIsANewAnnotationPointingAtTheOriginal() {
    const QString path = writeFixture(*dir_, QStringLiteral("reply.pdf"));
    QVERIFY(!path.isEmpty());
    QVERIFY(seedAnnotation(path));
    const QByteArray beforeReply = readAll(path);

    AnnotationService service;
    const auto result = service.replyToAnnotation(path, 0, 0, QStringLiteral("已確認"),
                                                  QStringLiteral("乙"));
    QVERIFY2(result.ok, qPrintable(result.message));

    // 純附加：原本的內容（含被回覆的那一則）一個位元組都不變。
    const QByteArray after = readAll(path);
    QCOMPARE(after.left(beforeReply.size()), beforeReply);

    const auto replies = repliesIn(path);
    QCOMPARE(replies.size(), std::size_t(1));
    QVERIFY2(replies[0].first > 0, "回覆沒有帶 /IRT——它會顯示成一則獨立的註解");
    QVERIFY(replies[0].second.empty());  // 純回覆不帶狀態
}

void TestAnnotationReply::statusIsCarriedByAReplyNotByRewritingTheOriginal() {
    const QString path = writeFixture(*dir_, QStringLiteral("state.pdf"));
    QVERIFY(seedAnnotation(path));
    const QByteArray beforeStatus = readAll(path);

    AnnotationService service;
    const auto result = service.replyToAnnotation(path, 0, 0, QString(), QStringLiteral("乙"),
                                                  QStringLiteral("Completed"));
    QVERIFY2(result.ok, qPrintable(result.message));

    // 原註解沒有被改寫——「誰在什麼時候標成已完成」因此留得下來。
    const QByteArray after = readAll(path);
    QCOMPARE(after.left(beforeStatus.size()), beforeStatus);

    const auto replies = repliesIn(path);
    QCOMPARE(replies.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(replies[0].second), QStringLiteral("Completed"));
}

void TestAnnotationReply::emptyReplyIsRejected() {
    const QString path = writeFixture(*dir_, QStringLiteral("empty.pdf"));
    QVERIFY(seedAnnotation(path));
    const QByteArray before = readAll(path);

    AnnotationService service;
    // 既沒有內容也沒有狀態：清單上會多一列，點開什麼都沒有。
    const auto result =
        service.replyToAnnotation(path, 0, 0, QStringLiteral("   "), QStringLiteral("乙"));
    QVERIFY(!result.ok);
    QCOMPARE(readAll(path), before);
}

void TestAnnotationReply::replyingToAMissingAnnotationFails() {
    const QString path = writeFixture(*dir_, QStringLiteral("missing.pdf"));
    const QByteArray before = readAll(path);

    AnnotationService service;
    const auto result =
        service.replyToAnnotation(path, 0, 7, QStringLiteral("x"), QStringLiteral("乙"));
    QVERIFY(!result.ok);
    QCOMPARE(readAll(path), before);
}

QTEST_MAIN(TestAnnotationReply)
#include "test_annotation_reply.moc"
