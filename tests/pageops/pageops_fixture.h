#pragma once

// 頁面進階操作的測試語料（WBS 12）。
//
// 刻意自己造而不是放二進位語料檔：合併與覆蓋的驗收條件是「來源頁的內容有沒有
// 出現在正確的位置」，那需要語料在版面上是可辨識的——每一頁一段獨一無二的文字、
// 已知座標的螢光筆、已知顏色的色塊。外部語料給不了這些，而且會讓失敗時
// 無法分辨是我們寫錯還是語料本來就怪。
//
// 產生器自己算 xref 偏移量，順便驗證我們對檔案結構的理解；
// 這一點與 tests/pdf_fixture.h 的立場一致。

#include <QByteArray>
#include <QFile>
#include <QString>

#include <optional>
#include <string>
#include <vector>

#include "domain/geometry.h"

namespace alioth::test::pageops {

struct FixtureAnnotation {
    alioth::domain::RectF rect{};
    std::string subtype{"Highlight"};
    // 螢光筆的實際形狀在 /QuadPoints，不在 /Rect。兩者都要跟著版面搬，
    // 只搬 /Rect 的缺陷正是這組測試要抓的東西。
    bool quadPoints{true};
};

struct FixturePage {
    alioth::domain::RectF media{0.0, 0.0, 200.0, 400.0};
    std::optional<alioth::domain::RectF> cropBox{};

    std::string text;  // 只用 ASCII：標準 14 字型無法表示中文
    alioth::domain::PointF textAt{20.0, 300.0};
    double fontSize{14.0};

    // 插在文字之前的原始運算子。圖形狀態污染的測試靠它製造「少一個 Q」的頁面，
    // 那在真實檔案裡非常常見——單獨一頁時沒有人看得出差別。
    std::string rawContentPrefix;

    std::vector<FixtureAnnotation> annotations;
    int rotate{0};
};

namespace detail {

inline std::string number(double value) {
    QByteArray text = QByteArray::number(value, 'f', 4);
    while (text.endsWith('0')) text.chop(1);
    if (text.endsWith('.')) text.chop(1);
    if (text.isEmpty() || text == "-0") text = "0";
    return text.toStdString();
}

inline std::string rect(const alioth::domain::RectF& r) {
    return "[" + number(r.left) + " " + number(r.bottom) + " " + number(r.right) + " " +
           number(r.top) + "]";
}

}  // namespace detail

// 依序組出一份最小但完整的 PDF。物件編號的配置方式：
// 1 = Catalog、2 = Pages、3 = Helvetica，其後每頁依序是頁面、內容、註解。
inline std::string makeFixturePdf(const std::vector<FixturePage>& pages) {
    using detail::number;
    using detail::rect;

    std::vector<std::string> objects;  // 索引 i 對應物件編號 i+1
    const auto reserve = [&objects]() {
        objects.emplace_back();
        return static_cast<int>(objects.size());
    };
    const auto assign = [&objects](int object, std::string body) {
        objects[static_cast<std::size_t>(object - 1)] = std::move(body);
    };

    const int catalog = reserve();
    const int pageTree = reserve();
    const int font = reserve();
    assign(font, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");

    std::string kids;
    for (const FixturePage& page : pages) {
        const int pageObject = reserve();
        const int contentObject = reserve();

        std::string content = page.rawContentPrefix;
        if (!page.text.empty()) {
            content += "BT\n/F0 " + number(page.fontSize) + " Tf\n" + number(page.textAt.x) + " " +
                       number(page.textAt.y) + " Td\n(" + page.text + ") Tj\nET\n";
        }
        assign(contentObject, "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" +
                                  content + "endstream");

        std::string annots;
        for (const FixtureAnnotation& annotation : page.annotations) {
            const int annotObject = reserve();
            std::string body = "<< /Type /Annot /Subtype /" + annotation.subtype + " /Rect " +
                               rect(annotation.rect) + " /F 4 /C [1 1 0] /CA 1 /P " +
                               std::to_string(pageObject) + " 0 R";
            if (annotation.quadPoints) {
                const alioth::domain::RectF& r = annotation.rect;
                body += " /QuadPoints [" + number(r.left) + " " + number(r.top) + " " +
                        number(r.right) + " " + number(r.top) + " " + number(r.left) + " " +
                        number(r.bottom) + " " + number(r.right) + " " + number(r.bottom) + "]";
            }
            body += " >>";
            assign(annotObject, std::move(body));
            if (!annots.empty()) annots += " ";
            annots += std::to_string(annotObject) + " 0 R";
        }

        std::string body = "<< /Type /Page /Parent " + std::to_string(pageTree) +
                           " 0 R /MediaBox " + rect(page.media) +
                           " /Resources << /Font << /F0 " + std::to_string(font) +
                           " 0 R >> >> /Contents " + std::to_string(contentObject) + " 0 R";
        if (page.cropBox) body += " /CropBox " + rect(*page.cropBox);
        if (page.rotate != 0) body += " /Rotate " + std::to_string(page.rotate);
        if (!annots.empty()) body += " /Annots [" + annots + "]";
        body += " >>";
        assign(pageObject, std::move(body));

        if (!kids.empty()) kids += " ";
        kids += std::to_string(pageObject) + " 0 R";
    }

    assign(catalog, "<< /Type /Catalog /Pages " + std::to_string(pageTree) + " 0 R >>");
    assign(pageTree, "<< /Type /Pages /Kids [" + kids + "] /Count " +
                         std::to_string(pages.size()) + " >>");

    std::string pdf = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    std::vector<std::size_t> offsets;
    offsets.reserve(objects.size());
    for (std::size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }

    const std::size_t xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (const std::size_t offset : offsets) {
        std::string digits = std::to_string(offset);
        pdf += std::string(10 - digits.size(), '0') + digits + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1) + " /Root " +
           std::to_string(catalog) + " 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return pdf;
}

inline bool writeBytes(const QString& path, const std::string& bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const qint64 written = file.write(bytes.data(), static_cast<qint64>(bytes.size()));
    file.close();
    return written == static_cast<qint64>(bytes.size());
}

}  // namespace alioth::test::pageops
