// PRD-VIEW-014：圖層攤平為基礎內容（ADR-003 的還款計畫）。
//
// 驗收刻意不只驗位元組：ADR-003 存在的理由就是「PDFium 的公開 API 不會照
// 使用者的勾選狀態重新渲染」，因此唯一有意義的驗收是真的把攤平後的位元組
// 交給 PdfiumEngine 渲染回讀，確認隱藏圖層的內容真的從畫面上消失，
// 可見圖層則原封不動——只驗我們自己剖析器讀出來的內容串流，驗不出
// 「輸出的檔案結構是否仍然是一份 PDFium 也認得的合法 PDF」。

#include <QtTest>

#include <memory>
#include <string>
#include <vector>

#include "domain/ocg.h"
#include "domain/redaction.h"
#include "engine/layers/ocg_flatten.h"
#include "engine/layers/ocg_reader.h"
#include "engine/pdfium_engine.h"

using namespace alioth;
using namespace alioth::domain;
using namespace alioth::engine;

namespace {

// 等待非同步結果的小工具，與 tests/test_pdfium_engine.cpp 同一套模式。
template <typename T>
class Latch {
public:
    void set(T value) {
        {
            std::lock_guard lock(mutex_);
            value_ = std::move(value);
            ready_ = true;
        }
        cv_.notify_all();
    }
    [[nodiscard]] bool wait(int milliseconds = 10000) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, std::chrono::milliseconds(milliseconds),
                            [this] { return ready_; });
    }
    [[nodiscard]] const T& value() const { return value_; }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool ready_{false};
    T value_{};
};

bool isWhite(const std::uint8_t* px) {
    return px[0] == 0xFF && px[1] == 0xFF && px[2] == 0xFF;
}

// BGRA：px[0]=Blue px[1]=Green px[2]=Red。
bool isSolidRed(const std::uint8_t* px) { return px[0] == 0x00 && px[1] == 0x00 && px[2] == 0xFF; }
bool isSolidBlack(const std::uint8_t* px) { return px[0] == 0x00 && px[1] == 0x00 && px[2] == 0x00; }

// 一頁 200x400 的頁面，左半（x 0–100）用圖層 "Base" 畫黑色矩形，
// 右半（x 100–200）用圖層 "Hidden" 畫紅色矩形，兩個矩形都貫穿整個頁高，
// 避免取樣點落在邊緣反鋸齒上。/OCProperties 的 /D 預設兩者都 ON——
// 這樣「攤平前渲染兩半都有顏色、攤平後右半變白」才能證明是攤平真的把
// 內容拿掉了，而不是文件本來就預設關閉右邊那個圖層。
std::string buildFixturePdf() {
    const std::string content =
        "0 0 0 rg\n"
        "/OC /MC0 BDC\n"
        "0 0 100 400 re\n"
        "f\n"
        "EMC\n"
        "1 0 0 rg\n"
        "/OC /MC1 BDC\n"
        "100 0 100 400 re\n"
        "f\n"
        "EMC\n";

    std::vector<std::string> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R /OCProperties 8 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    objects.push_back(
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 400] /Contents 4 0 R "
        "/Resources 5 0 R >>");
    objects.push_back("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content +
                      "endstream");
    objects.push_back("<< /Properties << /MC0 6 0 R /MC1 7 0 R >> >>");
    objects.push_back("<< /Type /OCG /Name (Base) >>");
    objects.push_back("<< /Type /OCG /Name (Hidden) >>");
    objects.push_back("<< /OCGs [6 0 R 7 0 R] /D << /ON [6 0 R 7 0 R] >> >>");

    std::string pdf = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    std::vector<std::size_t> offsets;
    offsets.reserve(objects.size());
    for (std::size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const std::size_t xrefOffset = pdf.size();
    const std::size_t count = objects.size() + 1;
    pdf += "xref\n0 " + std::to_string(count) + "\n0000000000 65535 f \n";
    for (const std::size_t offset : offsets) {
        std::string entry = std::to_string(offset);
        entry = std::string(10 - entry.size(), '0') + entry;
        pdf += entry + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size " + std::to_string(count) + " /Root 1 0 R >>\nstartxref\n" +
          std::to_string(xrefOffset) + "\n%%EOF\n";
    return pdf;
}

std::unique_ptr<QTemporaryFile> writeTemp(const std::string& bytes) {
    auto file = std::make_unique<QTemporaryFile>(QStringLiteral("alioth-ocg-XXXXXX.pdf"));
    if (!file->open()) return nullptr;
    file->write(bytes.data(), static_cast<qint64>(bytes.size()));
    file->flush();
    return file;
}

// 開檔並渲染第 0 頁的第一張圖磚，回傳結果。逾時或失敗一律讓呼叫端
// QVERIFY，不吞錯——渲染回讀本身就是這支測試最重要的一步。
RenderResult renderFirstTile(const std::string& path) {
    PdfiumEngine engine;
    Latch<OpenResult> open;
    engine.openDocument(path, "", [&open](OpenResult r) { open.set(std::move(r)); });
    if (!open.wait() || !open.value().ok()) return RenderResult{};

    Latch<RenderResult> latch;
    engine.renderTile(TileKey{0, exactScaleKey(1.0), 0, 0, Rotation::None, false}, RenderOptions{},
                      TaskPriority::Visible, CancellationToken{},
                      [&latch](RenderResult r) { latch.set(std::move(r)); });
    if (!latch.wait()) return RenderResult{};
    return latch.value();
}

}  // namespace

class TestOcgFlatten : public QObject {
    Q_OBJECT

private slots:
    // 攤平前：兩個圖層在 /D 都是 ON，PDFium 依文件內建預設渲染，
    // 左右兩半都應該有顏色。這是後面那個測試案例的基準，證明「右半原本
    // 真的畫得出來」，而不是文件本來就沒有內容。
    void beforeFlattenBothLayersRender() {
        const std::string pdf = buildFixturePdf();
        auto file = writeTemp(pdf);
        QVERIFY2(file != nullptr, "無法建立測試用 PDF");

        const RenderResult result = renderFirstTile(file->fileName().toStdString());
        QVERIFY2(result.ok(), "渲染逾時或失敗");

        const std::uint8_t* left = result.buffer->data() + result.buffer->stride() * 200 + 50 * 4;
        const std::uint8_t* right = result.buffer->data() + result.buffer->stride() * 200 + 150 * 4;
        QVERIFY2(isSolidBlack(left), "左半（Base 圖層）應為黑色");
        QVERIFY2(isSolidRed(right), "右半（Hidden 圖層）攤平前應為紅色，用來確認基準畫面");
    }

    // 核心案例：把 "Hidden" 圖層在面板上關掉、攤平，渲染回讀確認右半真的
    // 變白（內容已從檔案消失），左半（未受影響的圖層）保持原樣。
    void hiddenLayerContentIsGoneAfterFlatten() {
        const std::string pdf = buildFixturePdf();
        const OcgTree parsed = engine::layers::readOcgTree(pdf);
        QVERIFY(parsed.present);
        QCOMPARE(parsed.layers.size(), std::size_t(2));

        OcgTree tree = parsed;
        const int hiddenIndex = [&] {
            for (std::size_t i = 0; i < tree.layers.size(); ++i) {
                if (tree.layers[i].name == "Hidden") return static_cast<int>(i);
            }
            return -1;
        }();
        QVERIFY(hiddenIndex >= 0);
        (void)setLayerVisible(tree, hiddenIndex, false);
        QVERIFY(!tree.layers[static_cast<std::size_t>(hiddenIndex)].visible);

        const auto flattenResult =
            engine::layers::flattenLayers(pdf, tree, IrreversibleConsent::confirmed());
        QVERIFY2(flattenResult.ok, flattenResult.diagnostic.c_str());
        QCOMPARE(flattenResult.stats.pagesTouched, 1);
        QCOMPARE(flattenResult.stats.removedMarkedContentSpans, 1);
        QVERIFY(!flattenResult.sawUnsupportedVisibilityExpression);

        auto file = writeTemp(flattenResult.bytes);
        QVERIFY2(file != nullptr, "無法寫出攤平後的檔案");

        const RenderResult result = renderFirstTile(file->fileName().toStdString());
        QVERIFY2(result.ok(), "攤平後的檔案渲染失敗——輸出可能不是合法 PDF");

        const std::uint8_t* left = result.buffer->data() + result.buffer->stride() * 200 + 50 * 4;
        const std::uint8_t* right = result.buffer->data() + result.buffer->stride() * 200 + 150 * 4;
        QVERIFY2(isSolidBlack(left), "可見圖層（Base）攤平後應該原封不動");
        QVERIFY2(isWhite(right), "隱藏圖層（Hidden）攤平後內容應該真的消失，不是只換個方式藏起來");
    }

    // 巢狀 BDC/EMC 配對正確性：外層可見、內層（不同 tag）隱藏，只有內層被拿掉。
    void nestedMarkedContentOnlyRemovesInnerHiddenSpan() {
        const std::string content =
            "/OC /MC0 BDC\n"
            "0 0 0 rg\n"
            "0 0 50 400 re\n"
            "f\n"
            "/Span BMC\n"
            "1 0 0 rg\n"
            "/OC /MC1 BDC\n"
            "50 0 50 400 re\n"
            "f\n"
            "EMC\n"
            "EMC\n"
            "0 0 0 rg\n"
            "100 0 50 400 re\n"
            "f\n"
            "EMC\n";
        std::vector<std::string> objects;
        objects.push_back("<< /Type /Catalog /Pages 2 0 R /OCProperties 8 0 R >>");
        objects.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
        objects.push_back(
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 400] /Contents 4 0 R "
            "/Resources 5 0 R >>");
        objects.push_back("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" +
                          content + "endstream");
        objects.push_back("<< /Properties << /MC0 6 0 R /MC1 7 0 R >> >>");
        objects.push_back("<< /Type /OCG /Name (Outer) >>");
        objects.push_back("<< /Type /OCG /Name (Inner) >>");
        objects.push_back("<< /OCGs [6 0 R 7 0 R] /D << /ON [6 0 R 7 0 R] >> >>");

        std::string pdf = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
        std::vector<std::size_t> offsets;
        for (std::size_t i = 0; i < objects.size(); ++i) {
            offsets.push_back(pdf.size());
            pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
        }
        const std::size_t xrefOffset = pdf.size();
        const std::size_t count = objects.size() + 1;
        pdf += "xref\n0 " + std::to_string(count) + "\n0000000000 65535 f \n";
        for (const std::size_t offset : offsets) {
            std::string entry = std::to_string(offset);
            entry = std::string(10 - entry.size(), '0') + entry;
            pdf += entry + " 00000 n \n";
        }
        pdf += "trailer\n<< /Size " + std::to_string(count) + " /Root 1 0 R >>\nstartxref\n" +
              std::to_string(xrefOffset) + "\n%%EOF\n";

        OcgTree tree = engine::layers::readOcgTree(pdf);
        QVERIFY(tree.present);
        const int innerIndex = [&] {
            for (std::size_t i = 0; i < tree.layers.size(); ++i) {
                if (tree.layers[i].name == "Inner") return static_cast<int>(i);
            }
            return -1;
        }();
        QVERIFY(innerIndex >= 0);
        (void)setLayerVisible(tree, innerIndex, false);

        const auto result =
            engine::layers::flattenLayers(pdf, tree, IrreversibleConsent::confirmed());
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QCOMPARE(result.stats.removedMarkedContentSpans, 1);
    }

    // 巢狀不平衡（多一個 EMC）：明確失敗，不產生結構壞掉的頁面。
    void unbalancedNestingFailsExplicitly() {
        const std::string content = "/OC /MC0 BDC\nEMC\nEMC\n";
        std::vector<std::string> objects;
        objects.push_back("<< /Type /Catalog /Pages 2 0 R /OCProperties 7 0 R >>");
        objects.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
        objects.push_back(
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 400] /Contents 4 0 R "
            "/Resources 5 0 R >>");
        objects.push_back("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" +
                          content + "endstream");
        objects.push_back("<< /Properties << /MC0 6 0 R >> >>");
        objects.push_back("<< /Type /OCG /Name (A) >>");
        objects.push_back("<< /OCGs [6 0 R] /D << /ON [6 0 R] >> >>");

        std::string pdf = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
        std::vector<std::size_t> offsets;
        for (std::size_t i = 0; i < objects.size(); ++i) {
            offsets.push_back(pdf.size());
            pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
        }
        const std::size_t xrefOffset = pdf.size();
        const std::size_t count = objects.size() + 1;
        pdf += "xref\n0 " + std::to_string(count) + "\n0000000000 65535 f \n";
        for (const std::size_t offset : offsets) {
            std::string entry = std::to_string(offset);
            entry = std::string(10 - entry.size(), '0') + entry;
            pdf += entry + " 00000 n \n";
        }
        pdf += "trailer\n<< /Size " + std::to_string(count) + " /Root 1 0 R >>\nstartxref\n" +
              std::to_string(xrefOffset) + "\n%%EOF\n";

        const OcgTree tree = engine::layers::readOcgTree(pdf);
        QVERIFY(tree.present);
        const auto result =
            engine::layers::flattenLayers(pdf, tree, IrreversibleConsent::confirmed());
        QVERIFY(!result.ok);
        QVERIFY(!result.diagnostic.empty());
    }

    // 沒有 /OCProperties 的文件：明確拒絕，不是靜默回傳原檔。
    void documentWithoutOcPropertiesRejected() {
        OcgTree tree;
        const auto result =
            engine::layers::flattenLayers("%PDF-1.7\n%%EOF\n", tree, IrreversibleConsent::confirmed());
        QVERIFY(!result.ok);
    }
};

QTEST_MAIN(TestOcgFlatten)
#include "test_ocg_flatten.moc"
