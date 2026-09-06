// 手寫／輸入式簽名（PRD-SIG-008）。
//
// 這裡最重要的一條，不是任何幾何或序列化的細節，而是「手寫簽名不可以看起來
// 像數位簽章」。因此本測試第一件驗的就是身分標記，以及它與 engine/signature
// 完全沒有交集。

#include <QtTest>

#include "app/handwritten_signature.h"

using namespace alioth;
using app::SavedSignature;
using app::SignatureSourceKind;

namespace {

SavedSignature inkSignature() {
    SavedSignature signature;
    signature.name = QStringLiteral("正式");
    signature.kind = SignatureSourceKind::Ink;
    signature.strokes = {
        {domain::PointF{0.0, 0.0}, domain::PointF{0.5, 1.0}, domain::PointF{1.0, 0.0}},
        {domain::PointF{0.2, 0.5}, domain::PointF{0.8, 0.5}},
    };
    return signature;
}

SavedSignature imageSignature() {
    SavedSignature signature;
    signature.name = QStringLiteral("掃描");
    signature.kind = SignatureSourceKind::Image;
    signature.width = 4;
    signature.height = 2;
    signature.channels = 3;
    signature.pixels.assign(static_cast<std::size_t>(4 * 2 * 3), 128);
    return signature;
}

}  // namespace

class TestHandwrittenSignature : public QObject {
    Q_OBJECT

private slots:
    void signatureAnnotationIsMarkedAsNotCryptographic() {
        // 把手寫簽名顯示得像數位簽章是這個產品最不該犯的錯。
        const auto annotation = app::buildSignatureAnnotation(
            inkSignature(), domain::RectF{100.0, 100.0, 300.0, 160.0}, QStringLiteral("Eddy"));
        QVERIFY(annotation.has_value());
        QVERIFY(app::isHandwrittenSignature(*annotation));
        QVERIFY(QString::fromStdString(annotation->subject).contains(QStringLiteral("非數位簽章")));
    }

    void ordinaryAnnotationIsNotMistakenForOne() {
        domain::Annotation ink;
        ink.geometry = domain::InkGeometry{};
        ink.subject = "隨手畫的";
        QVERIFY(!app::isHandwrittenSignature(ink));
    }

    void inkStrokesAreMappedIntoTheTargetRect() {
        const domain::RectF rect{100.0, 200.0, 300.0, 260.0};
        const auto annotation =
            app::buildSignatureAnnotation(inkSignature(), rect, QStringLiteral("Eddy"));
        QVERIFY(annotation.has_value());

        const auto* geometry = std::get_if<domain::InkGeometry>(&annotation->geometry);
        QVERIFY(geometry != nullptr);
        QCOMPARE(geometry->strokes.size(), std::size_t{2});

        // 單位方框的四個極值要落在矩形的四個邊上。
        QCOMPARE(geometry->strokes[0][0].x, 100.0);
        QCOMPARE(geometry->strokes[0][0].y, 200.0);
        QCOMPARE(geometry->strokes[0][2].x, 300.0);
        // Y 不翻轉：翻了的話簽名會上下顛倒，而單元測試裡只表現成幾個數字不同。
        QCOMPARE(geometry->strokes[0][1].y, 260.0);
    }

    void sameSignatureFitsDifferentSizedFields() {
        // 存正規化座標的理由：同一個簽名要能貼進不同大小的欄位。
        const auto small = app::buildSignatureAnnotation(
            inkSignature(), domain::RectF{0.0, 0.0, 50.0, 20.0}, QStringLiteral("Eddy"));
        const auto large = app::buildSignatureAnnotation(
            inkSignature(), domain::RectF{0.0, 0.0, 400.0, 160.0}, QStringLiteral("Eddy"));
        QVERIFY(small.has_value() && large.has_value());

        const auto& smallInk = std::get<domain::InkGeometry>(small->geometry);
        const auto& largeInk = std::get<domain::InkGeometry>(large->geometry);
        QCOMPARE(smallInk.strokes.size(), largeInk.strokes.size());
        QVERIFY(largeInk.strokes[0][2].x > smallInk.strokes[0][2].x);
        // 線寬也要跟著縮，固定寬度在小欄位裡會糊成一團黑。
        QVERIFY(large->border.width > small->border.width);
    }

    void imageSignatureBecomesACustomStamp() {
        const auto annotation = app::buildSignatureAnnotation(
            imageSignature(), domain::RectF{10.0, 10.0, 110.0, 60.0}, QStringLiteral("Eddy"));
        QVERIFY(annotation.has_value());
        const auto* stamp = std::get_if<domain::StampGeometry>(&annotation->geometry);
        QVERIFY(stamp != nullptr);
        QVERIFY(stamp->isCustom());
        QVERIFY(stamp->hasImage());
    }

    void timestampAndAuthorAreAlwaysRecorded() {
        // 時間與作者是手寫簽名唯一能提供的可追溯性。
        const auto annotation = app::buildSignatureAnnotation(
            inkSignature(), domain::RectF{0.0, 0.0, 100.0, 40.0}, QStringLiteral("Eddy"));
        QVERIFY(annotation.has_value());
        QCOMPARE(annotation->author, std::string{"Eddy"});
        QVERIFY(annotation->creationDate.isValid());
        QCOMPARE(annotation->creationDate, annotation->modifiedDate);
    }

    void invalidSignatureYieldsNothingRatherThanAnEmptyAnnotation() {
        // 回傳空註解會在文件裡留下看不見的東西，使用者以為簽了但其實沒有。
        SavedSignature broken = inkSignature();
        broken.strokes.clear();
        QVERIFY(!app::buildSignatureAnnotation(broken, domain::RectF{0, 0, 10, 10},
                                               QStringLiteral("Eddy"))
                     .has_value());

        SavedSignature unnamed = inkSignature();
        unnamed.name.clear();
        QVERIFY(!unnamed.isValid());

        SavedSignature outOfRange = inkSignature();
        outOfRange.strokes[0][1].x = 2.0;  // 超出單位方框
        QVERIFY(!outOfRange.isValid());

        SavedSignature singlePoint = inkSignature();
        singlePoint.strokes = {{domain::PointF{0.5, 0.5}}};
        QVERIFY(!singlePoint.isValid());
    }

    void emptyRectIsRejected() {
        QVERIFY(!app::buildSignatureAnnotation(inkSignature(), domain::RectF{5, 5, 5, 5},
                                               QStringLiteral("Eddy"))
                     .has_value());
    }

    void mismatchedImageDataIsRejected() {
        SavedSignature broken = imageSignature();
        broken.pixels.pop_back();
        QVERIFY(!broken.isValid());

        SavedSignature badChannels = imageSignature();
        badChannels.channels = 2;
        QVERIFY(!badChannels.isValid());
    }

    void libraryReplacesSameNameAndEnforcesLimit() {
        app::SignatureLibrary library;
        QVERIFY(library.add(inkSignature()));
        SavedSignature updated = inkSignature();
        updated.strokes.pop_back();
        QVERIFY(library.add(updated));
        QCOMPARE(library.signatures().size(), std::size_t{1});
        QCOMPARE(library.find(QStringLiteral("正式"))->strokes.size(), std::size_t{1});

        for (int i = 0; i < app::SignatureLibrary::kMaxSignatures + 5; ++i) {
            SavedSignature extra = inkSignature();
            extra.name = QStringLiteral("sig-%1").arg(i);
            library.add(extra);
        }
        QCOMPARE(static_cast<int>(library.signatures().size()),
                 app::SignatureLibrary::kMaxSignatures);
    }

    void libraryRejectsInvalidEntries() {
        app::SignatureLibrary library;
        SavedSignature broken = inkSignature();
        broken.strokes.clear();
        QVERIFY(!library.add(broken));
        QVERIFY(library.signatures().empty());
    }

    void libraryRemoveReportsWhetherItExisted() {
        app::SignatureLibrary library;
        QVERIFY(library.add(inkSignature()));
        QVERIFY(library.remove(QStringLiteral("正式")));
        QVERIFY(!library.remove(QStringLiteral("正式")));
    }
};

QTEST_MAIN(TestHandwrittenSignature)
#include "test_handwritten_signature.moc"
