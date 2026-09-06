// PRD-VIEW-008：解析 /OCProperties（ISO 32000-1 §8.11）。
//
// 測試語料自產（一份最小、結構完整的傳統 xref 表 PDF），不依賴外部檔案，
// 與 tests/objects/object_fixture.h 的做法一致，只是不需要 Qt 依賴，
// 直接用 std::string 組字節與位移。

#include <QtTest>

#include <string>
#include <vector>

#include "engine/layers/ocg_reader.h"

using namespace alioth;

namespace {

// objects[0] 固定是 Catalog（物件編號 1），呼叫端負責讓它的 /Pages 與
// /OCProperties 指到後面正確的物件編號。
std::string buildPdf(const std::vector<std::string>& objects) {
    std::string pdf = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    std::vector<std::size_t> offsets;
    offsets.reserve(objects.size());
    for (std::size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const std::size_t xrefOffset = pdf.size();
    const std::size_t count = objects.size() + 1;
    pdf += "xref\n0 " + std::to_string(count) + "\n";
    pdf += "0000000000 65535 f \n";
    for (const std::size_t offset : offsets) {
        std::string entry = std::to_string(offset);
        entry = std::string(10 - entry.size(), '0') + entry;
        pdf += entry + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size " + std::to_string(count) + " /Root 1 0 R >>\nstartxref\n" +
          std::to_string(xrefOffset) + "\n%%EOF\n";
    return pdf;
}

// 固定的頁面骨架：1=Catalog（由呼叫端提供）、2=Pages、3=Page。
std::string pagesObject() { return "<< /Type /Pages /Kids [3 0 R] /Count 1 >>"; }
std::string pageObject() {
    return "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] >>";
}

}  // namespace

class TestOcgReader : public QObject {
    Q_OBJECT

private slots:
    void documentWithoutOcPropertiesYieldsEmptyTree() {
        const std::string pdf = buildPdf({
            "<< /Type /Catalog /Pages 2 0 R >>",
            pagesObject(),
            pageObject(),
        });
        const auto tree = engine::layers::readOcgTree(pdf);
        QVERIFY(!tree.present);
        QVERIFY(tree.layers.empty());
    }

    void basicOnOffAndNamesAreParsed() {
        // 4 = OCG "Base Map"（預設可見，隱含在 ON）
        // 5 = OCG "Annotations Layer"（明確 OFF）
        // 6 = OCProperties
        const std::string pdf = buildPdf({
            "<< /Type /Catalog /Pages 2 0 R /OCProperties 6 0 R >>",
            pagesObject(),
            pageObject(),
            "<< /Type /OCG /Name (Base Map) >>",
            "<< /Type /OCG /Name (Annotations Layer) >>",
            "<< /OCGs [4 0 R 5 0 R] /D << /ON [4 0 R] /OFF [5 0 R] >> >>",
        });

        const auto tree = engine::layers::readOcgTree(pdf);
        QVERIFY(tree.present);
        QCOMPARE(tree.layers.size(), std::size_t(2));

        const auto* base = tree.find(4);
        QVERIFY(base != nullptr);
        QCOMPARE(QString::fromStdString(base->name), QStringLiteral("Base Map"));
        QVERIFY(base->visible);

        const auto* annotations = tree.find(5);
        QVERIFY(annotations != nullptr);
        QVERIFY(!annotations->visible);

        // 沒有 /Order 時兩個圖層都附加成根節點，依 /OCGs 陣列順序。
        QCOMPARE(tree.roots.size(), std::size_t(2));
    }

    void baseStateOffFlipsDefaultVisibility() {
        const std::string pdf = buildPdf({
            "<< /Type /Catalog /Pages 2 0 R /OCProperties 6 0 R >>",
            pagesObject(),
            pageObject(),
            "<< /Type /OCG /Name (A) >>",
            "<< /Type /OCG /Name (B) >>",
            "<< /OCGs [4 0 R 5 0 R] /D << /BaseState /OFF /ON [4 0 R] >> >>",
        });
        const auto tree = engine::layers::readOcgTree(pdf);
        QVERIFY(tree.find(4)->visible);   // 在 ON 清單裡，明確覆寫
        QVERIFY(!tree.find(5)->visible);  // BaseState OFF，且不在 ON 清單裡
    }

    void orderBuildsHeadingAndNestedChildren() {
        // /Order 語意（ISO 32000-1 §8.11.4.3）：
        //   4 0 R                        -> 根節點
        //   [(Detail Layers) 6 0 R 7 0 R] -> 分組標題（第一個元素是文字字串），子項 6、7
        //   5 0 R                        -> 根節點
        const std::string pdf = buildPdf({
            "<< /Type /Catalog /Pages 2 0 R /OCProperties 8 0 R >>",
            pagesObject(),
            pageObject(),
            "<< /Type /OCG /Name (Base Map) >>",
            "<< /Type /OCG /Name (Annotations Layer) >>",
            "<< /Type /OCG /Name (Detail A) >>",
            "<< /Type /OCG /Name (Detail B) >>",
            "<< /OCGs [4 0 R 5 0 R 6 0 R 7 0 R] "
            "/D << /Order [4 0 R [(Detail Layers) 6 0 R 7 0 R] 5 0 R] "
            "/RBGroups [[6 0 R 7 0 R]] >> >>",
        });

        const auto tree = engine::layers::readOcgTree(pdf);
        QVERIFY(tree.present);
        QCOMPARE(tree.roots.size(), std::size_t(3));  // 4、分組標題、5

        const std::int32_t headingIndex = tree.roots[1];
        const auto& heading = tree.layers[static_cast<std::size_t>(headingIndex)];
        QVERIFY(heading.isGroupHeading);
        QCOMPARE(QString::fromStdString(heading.name), QStringLiteral("Detail Layers"));
        QCOMPARE(heading.children.size(), std::size_t(2));

        // RBGroups：6 與 7 屬於同一互斥群組。
        const auto* detailA = tree.find(6);
        const auto* detailB = tree.find(7);
        QVERIFY(detailA != nullptr && detailB != nullptr);
        QCOMPARE(detailA->radioGroup, detailB->radioGroup);
        QVERIFY(detailA->radioGroup >= 0);
    }

    void malformedOcPropertiesDoesNotCrash() {
        // /OCProperties 指到一個不是字典的物件（純數字）：PDF 是不可信任輸入，
        // 這種畸形檔案要明確回報「沒有圖層」而不是崩潰或猜一個結果出來。
        const std::string pdf = buildPdf({
            "<< /Type /Catalog /Pages 2 0 R /OCProperties 4 0 R >>",
            pagesObject(),
            pageObject(),
            "42",
        });
        const auto tree = engine::layers::readOcgTree(pdf);
        QVERIFY(!tree.present);
    }
};

QTEST_MAIN(TestOcgReader)
#include "test_ocg_reader.moc"
